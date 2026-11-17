#!/bin/sh
# Self-contained AES-256-GCM benchmark: OpenZFS ICP "avx" GCM (OpenSSL
# stitched code) vs Linux aes-gcm-aesni-x86_64.S (Eric Biggers), userspace,
# pinned sources, no OpenZFS configure needed.  Needs: cc, git, curl,
# OpenSSL headers (libssl-dev), taskset (util-linux).  Root is not needed.
# usage: sh run.sh            (from the directory holding bench.c, mkport.sh)
# Prints the result table only; build output goes to $work/build.log.
set -eu
here=$(cd "$(dirname "$0")" && pwd -P)
zfs_rev=924b8c89c04e324649855b05986f686b58ca4cb9
linux_rev=104a9526e1448dc45b96534e2d78d41b3ecac3f9
work=$(mktemp -d "${TMPDIR:-/tmp}/gcmbench.XXXXXX")
log=$work/build.log
cd "$work"
{
	git init -q zfs
	git -C zfs remote add origin https://github.com/openzfs/zfs.git
	git -C zfs sparse-checkout set --no-cone /module/icp/ /include/ \
	    /lib/libspl/include/
	git -C zfs fetch -q --depth 1 --filter=blob:none origin "$zfs_rev"
	git -C zfs checkout -q FETCH_HEAD
	curl -sSfL -o aes-gcm-aesni-x86_64.S "https://raw.githubusercontent.com/torvalds/linux/$linux_rev/arch/x86/crypto/aes-gcm-aesni-x86_64.S"
	sh "$here/mkport.sh" aes-gcm-aesni-x86_64.S lx-port.S
	Z=$work/zfs
	inc="-include $Z/include/sys/simd_config.h -DHAVE_TOOLCHAIN_AVX=1
	    -DHAVE_TOOLCHAIN_AES=1 -DHAVE_TOOLCHAIN_PCLMULQDQ=1
	    -DHAVE_TOOLCHAIN_MOVBE=1 -I$Z/include -I$Z/lib/libspl/include
	    -I$Z/lib/libspl/include/os/linux -I$Z/module/icp/include"
	A=$Z/module/icp/asm-x86_64
	# shellcheck disable=SC2086
	for f in lx-port.S $A/modes/aesni-gcm-x86_64.S \
	    $A/modes/ghash-x86_64.S $A/aes/aes_aesni.S; do
		cc -c $inc -o "$(basename "$f" .S).o" "$f"
	done
	cc -O2 -o gcmbench "$here/bench.c" lx-port.o aesni-gcm-x86_64.o \
	    ghash-x86_64.o aes_aesni.o -lcrypto
} > "$log" 2>&1 || { echo "build failed, see $log" >&2; exit 1; }
# TSC runs at the nominal clock in the model name ("@ 2.20GHz")
ghz=$(sed -n 's/^model name.*@ *\([0-9.]*\)GHz.*/\1/p' /proc/cpuinfo | head -1)
cpu=$(( $(nproc) - 1 ))
grep -m1 '^model name' /proc/cpuinfo
for v in avx sse; do
	taskset -c "$cpu" ./gcmbench 400 "${ghz:-2.1}" "$v" 2>/dev/null
done
echo "work dir: $work"
