#!/bin/bash
# Measure the GFNI RAID-Z math (branch raidz-gfni) and the VAES AVX-512
# AES-GCM (branch aes-gcm-vaes-avx512) on the CPU it runs on: correctness
# first, then raidz_test -B and crypto_test -p AES-GCM.  Userspace only:
# nothing is loaded into the kernel.  Run as root on Debian 13 or Ubuntu 24.04.
# Usage: bench-simd.sh <output directory>
set -u
O=${1:-/root/zfs-bench}
R=$O/results
REPO=https://github.com/tigerblue77/zfs
GFNI=raidz-gfni
AES=aes-gcm-vaes-avx512
mkdir -p "$R"
exec 3>&1 >>"$R/full.log" 2>&1
say() { echo "$*" >&3; echo "== $*"; }
fail() { say "FAILED: $*"; exit 1; }

say "CPU: $(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2- | sed 's/^ *//')"
f=""
for x in avx2 avx512f avx512bw gfni vaes vpclmulqdq; do
	grep -qw "$x" /proc/cpuinfo && f="$f $x"
done
say "Flags:$f"

export DEBIAN_FRONTEND=noninteractive
apt-get -qq update && apt-get -qq install -y --no-install-recommends \
    build-essential autoconf automake libtool gawk git ca-certificates \
    pkg-config uuid-dev libblkid-dev libudev-dev libssl-dev zlib1g-dev \
    libaio-dev libattr1-dev libelf-dev libtirpc-dev libffi-dev \
    libcurl4-openssl-dev python3 || fail "apt-get (see $R/full.log)"

build() {
	rm -rf "${O:?}/${1:?}"
	git clone -q --depth 1 -b "$1" "$REPO" "$O/$1" || return 1
	(cd "$O/$1" && ./autogen.sh && ./configure --with-config=user \
	    --disable-pyzfs && make -j"$(nproc)" "${@:2}")
}
build $GFNI raidz_test || fail "build of $GFNI (see $R/full.log)"
build $AES tests/zfs-tests/cmd/crypto_test || fail "build of $AES (see $R/full.log)"
say "Commits: $GFNI $(git -C "$O/$GFNI" rev-parse --short=9 HEAD)," \
    "$AES $(git -C "$O/$AES" rev-parse --short=9 HEAD)"

cd "$O/$GFNI" || exit 1
if ./raidz_test > "$R/raidz-correctness.log" 2>&1; then
	say "raidz_test (every implementation): PASS"
else
	say "raidz_test (every implementation): FAIL"
fi
cd "$O/$AES" || exit 1
./tests/zfs-tests/cmd/crypto_test -c \
    tests/zfs-tests/tests/functional/crypto/aes_gcm_test.txt \
    > "$R/gcm-correctness.log" 2>&1
say "crypto_test AES-GCM vectors: $(grep -c 'failed=0' "$R/gcm-correctness.log")" \
    "implementations pass, $(grep -c 'failed=[1-9]' "$R/gcm-correctness.log") fail"

for r in 1 2; do
	(cd "$O/$GFNI" && ./raidz_test -B) > "$R/raidz-bench-$r.csv" 2>&1
	(cd "$O/$AES" && ./tests/zfs-tests/cmd/crypto_test -p AES-GCM) \
	    > "$R/gcm-bench-$r.txt" 2>&1
done

say ""
say "raidz_test -B, 1 MiB blocks, total GB/s (run 1/run 2):"
awk -F', *' '
$2 ~ /^(gen|rec)_/ && $4 + 0 == 1048576 {
	i = $1; gsub(/ /, "", i)
	k = i SUBSEP $2
	v = sprintf("%.1f", $6 / 1024)
	val[k] = (k in val) ? val[k] "/" v : v
	if (!(i in seen)) { seen[i] = 1; ord[++n] = i }
}
END {
	m = split("gen_p gen_pq gen_pqr rec_p rec_pq rec_pqr", op, " ")
	printf "%-12s", "impl"
	for (j = 1; j <= m; j++) printf " %11s", op[j]
	print ""
	for (i = 1; i <= n; i++) {
		printf "%-12s", ord[i]
		for (j = 1; j <= m; j++) printf " %11s", val[ord[i], op[j]]
		print ""
	}
}' "$R/raidz-bench-1.csv" "$R/raidz-bench-2.csv" >&3
for r in 1 2; do
	say ""
	say "crypto_test -p AES-GCM, run $r (average time per message):"
	grep -E '^avg encrypt|\[aesni\+(avx|avx2-vaes|avx512-vaes)\]' \
	    "$R/gcm-bench-$r.txt" >&3
done
say ""
say "Full results: $R"
