#!/bin/sh
# SPDX-License-Identifier: CDDL-1.0
#
# Stage B of the AnyRAID adoption spike, on file vdevs, in userspace.
# Usage: run_pool_test.sh <build dir> <work dir> <width> <parity> <ashift>
#
# The work dir is deleted and recreated.
#
B=${1:?build dir}
W=${2:?work dir}
WIDTH=${3:?width}
PAR=${4:?parity}
ASHIFT=${5:?ashift}
T="$B/tests/anyraid_adopt/anyraid_adopt_pool"
ZDB="$B/zdb"
POOL="adopt${WIDTH}p${PAR}a${ASHIFT}"
V="$W/vdevs"
R="$W/ref"
L="$W/log"

rm -rf "$W"
mkdir -p "$V" "$R" "$L" || exit 1

step() {
	name=$1
	shift
	"$@" > "$L/$name.txt" 2>&1
	rc=$?
	printf '%-22s rc=%d\n' "$name" "$rc"
}

summary() {
	grep -E "$2" "$L/$1.txt" | sed 's/^/    /'
}

step 01-create "$T" create -d "$V" -p "$POOL" -r "$R" -w "$WIDTH" \
    -P "$PAR" -a "$ASHIFT" -s $((256 << 20))
summary 01-create 'created'
step 02-zdb-C-raidz "$ZDB" -e -p "$V" -C "$POOL"
summary 02-zdb-C-raidz "type: 'raidz'|metaslab_shift|nparity|ashift: "
step 03-zdb-m-raidz "$ZDB" -e -p "$V" -m "$POOL"
step 04-zdb-bcc-raidz "$ZDB" -e -p "$V" -bcc "$POOL"
summary 04-zdb-bcc-raidz 'leaks|bp count|errors|block sum'
step 05-zdb-bcc-adopt-ro "$ZDB" -e -p "$V" -o zfs_anyraid_adopt_raidz=1 \
    -bcc "$POOL"
summary 05-zdb-bcc-adopt-ro 'leaks|bp count|errors|block sum'
step 06-zdb-control "$ZDB" -e -p "$V" -o zfs_anyraid_adopt_raidz=1 \
    -o zfs_anyraid_adopt_test_rotate=1 -bcc "$POOL"
summary 06-zdb-control 'leaks|bp count|errors|block sum|zdb:|open|ASSERT|VERIFY'
step 07a-verify-adopt-ro "$T" verify -d "$V" -p "$POOL" -r "$R" -A -R
summary 07a-verify-adopt-ro 'top-level|bad|VERIFY'
step 07-verify-adopt "$T" verify -d "$V" -p "$POOL" -r "$R" -A -S
summary 07-verify-adopt 'top-level|bad|scrub|VERIFY|activated|child|top:'
step 08-zdb-C-anyraid "$ZDB" -e -p "$V" -C "$POOL"
summary 08-zdb-C-anyraid "type: 'anyraidz'|anyraid_|metaslab_shift"
step 09-verify-write "$T" verify -d "$V" -p "$POOL" -r "$R" -W -S
summary 09-verify-write 'top-level|bad|scrub|VERIFY|wrote|child|top:'
step 10-verify-again "$T" verify -d "$V" -p "$POOL" -r "$R"
summary 10-verify-again 'bad|VERIFY'
step 11-zdb-bcc-anyraid "$ZDB" -e -p "$V" -bcc "$POOL"
summary 11-zdb-bcc-anyraid 'leaks|bp count|errors|block sum'
step 12-zdb-m-anyraid "$ZDB" -e -p "$V" -m "$POOL"
step 13-zdb-anyraid-map "$ZDB" -e -p "$V" --anyraid-map "$POOL"
du -sh "$V" "$R" | sed 's/^/    /'

# Control: the same pool (creation is deterministic), never adopted, scrubbed
# as RAID-Z, to tell what the first scrub repairs on its own.
rm -rf "$V" "$R"
mkdir -p "$V" "$R" || exit 1
step 14-create-control "$T" create -d "$V" -p "$POOL" -r "$R" -w "$WIDTH" \
    -P "$PAR" -a "$ASHIFT" -s $((256 << 20))
summary 14-create-control 'created'
step 15-scrub-raidz-control "$T" verify -d "$V" -p "$POOL" -r "$R" -S
summary 15-scrub-raidz-control 'top-level|bad|scrub|VERIFY|child|top:'
rm -rf "$V" "$R"
