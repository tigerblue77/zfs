#!/usr/bin/env bash

######################################################################
# Fork-only experiment, not for upstream: select icp_gcm_impl=avx512-vaes
# for the ZTS run, after checking that data encrypted with it decrypts
# with the generic implementation and the other way round.
#
# called on qemu-vm: qemu-6-vaes-test-vm.sh [--after]
# Exits non-zero when ZTS should not run: no AVX-512/VAES in this VM,
# avx512-vaes cannot be selected, or the cross-check failed.
# --after: once ZTS is done, show the selection and the kernel's warnings.
######################################################################

set -u

PARAM=/sys/module/zfs/parameters/icp_gcm_impl
test -e "$PARAM" || PARAM=/sys/module/icp/parameters/icp_gcm_impl
POOL=vaestest
DIR=/var/tmp/vaestest
PASSPHRASE="vaes-test-passphrase"
KERNEL_ISSUES='BUG:|Oops|general protection|kernel_fpu|WARNING:'

if [ "${1:-}" = "--after" ]; then
  echo "VAES-TEST: after ZTS, icp_gcm_impl: $(cat "$PARAM")"
  # ZTS clears dmesg after a failed test; the journal keeps everything.
  { sudo journalctl -k -b --no-pager 2>&1 || sudo dmesg; } \
    > /var/tmp/kernel-vaes.txt
  grep -E "$KERNEL_ISSUES" /var/tmp/kernel-vaes.txt \
    | sed 's/^/VAES-TEST: kernel: /'
  echo "VAES-TEST: kernel log checked, $(wc -l < /var/tmp/kernel-vaes.txt) lines"
  exit 0
fi

lscpu
grep -o -E 'avx512f|avx512bw|avx512vl|vaes|vpclmulqdq|bmi2' /proc/cpuinfo \
  | sort -u | tr '\n' ' '
echo
echo "VAES-TEST: $PARAM: $(cat "$PARAM")"

for flag in avx512f avx512bw avx512vl vaes vpclmulqdq bmi2; do
  if ! grep -qw "$flag" /proc/cpuinfo; then
    echo "VAES-TEST: NO AVX-512/VAES ON THIS RUNNER (no $flag)"
    exit 1
  fi
done

set_impl() {
  echo "$1" | sudo tee "$PARAM" >/dev/null
  echo "VAES-TEST: icp_gcm_impl: $(cat "$PARAM")"
  grep -qF "[$1]" "$PARAM"
}

cleanup() {
  sudo zpool destroy "$POOL" 2>/dev/null
  rm -f "$DIR/vdev" "$DIR/src"
  rmdir "$DIR" 2>/dev/null
}

fail() {
  echo "VAES-TEST: CROSS-CHECK FAIL: $*"
  sudo zpool status -v "$POOL" 2>/dev/null
  sudo dmesg | tail -n 50
  cleanup
  exit 1
}

# Write the data with implementation $1, read it back with $2.
cross_check() {
  echo "VAES-TEST: write with $1, read with $2"
  set_impl "$1" || fail "cannot select $1"
  truncate -s 2G "$DIR/vdev"
  sudo zpool create -f -O compression=off "$POOL" "$DIR/vdev" ||
    fail "zpool create"
  echo "$PASSPHRASE" | sudo zfs create -o encryption=aes-256-gcm \
    -o keyformat=passphrase "$POOL/enc" || fail "zfs create"
  sudo dd if="$DIR/src" of="/$POOL/enc/data" bs=1M status=none ||
    fail "write with $1"
  sudo zpool export "$POOL" || fail "zpool export"

  set_impl "$2" || fail "cannot select $2"
  sudo zpool import -d "$DIR" "$POOL" || fail "zpool import"
  echo "$PASSPHRASE" | sudo zfs load-key "$POOL/enc" ||
    fail "load-key with $2"
  sudo zfs mount "$POOL/enc" || fail "zfs mount"
  sum=$(sudo sha256sum "/$POOL/enc/data" | cut -d' ' -f1)
  [ "$sum" = "$SRCSUM" ] ||
    fail "read with $2: sha256 $sum, written $SRCSUM"
  sudo zpool scrub -w "$POOL" || fail "zpool scrub"
  status=$(sudo zpool status -x "$POOL")
  echo "$status" | grep -q "is healthy" || fail "zpool status: $status"
  sudo zpool destroy "$POOL" || fail "zpool destroy"
  rm -f "$DIR/vdev"
}

mkdir -p "$DIR"
dd if=/dev/urandom of="$DIR/src" bs=1M count=512 status=none ||
  fail "cannot create the random data"
SRCSUM=$(sha256sum < "$DIR/src" | cut -d' ' -f1)
echo "VAES-TEST: 512 MiB of random data, sha256 $SRCSUM"

cross_check avx512-vaes generic
cross_check generic avx512-vaes
cleanup
echo "VAES-TEST: CROSS-CHECK PASS"

# ZTS does not reload the module, but should anything do so, keep the
# selection.
echo "options zfs icp_gcm_impl=avx512-vaes" \
  | sudo tee /etc/modprobe.d/zfs-vaes-test.conf >/dev/null
if ! set_impl avx512-vaes; then
  echo "VAES-TEST: cannot select avx512-vaes"
  exit 1
fi
sudo dmesg | grep -E "$KERNEL_ISSUES" | sed 's/^/VAES-TEST: kernel: /'
exit 0
