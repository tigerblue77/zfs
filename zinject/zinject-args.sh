#!/bin/bash
# SPDX-License-Identifier: CDDL-1.0
#
# zinject-args.sh: ZTS zinject_args, 20 runs on master (3028351741) and 20
# on tigerblue77/zfs zts-zinject-args-mirror-read (the fix), alternating,
# each run on the modules and tools of its own tree.  Disposable VM only:
# it creates a user "zts" with passwordless sudo, unloads and loads the
# trees' modules, and ZTS creates and destroys pools named testpool on loop
# devices over files under $ZA_WORK/files.  See VM-zinject-args.md.
#
#   zinject-args.sh setup | run [runs-per-tree] | summary | clean
#
# Each run logs "RUN <n> <master|fix> <PASS|FAIL|...> rc=... s=..." into
# $ZA_WORK/za.log; every line of a failed run's test log that names the
# failure is logged as "FAILLINE <n> <tree> <line>".  "run" resumes after
# the last RUN line, so it can be started again after a reboot.
set -u -o pipefail

W=${ZA_WORK:-/var/tmp/za}
U=zts
SRC=$W/src
FD=$W/files
LOG=$W/za.log
REPO=https://github.com/tigerblue77/zfs.git
BR=zts-zinject-args-mirror-read
MASTER=3028351741106e28af57961a97d81c760a73fd44
FIX=9ae72311c375062354bff005fb591d4a72a5f696
TP=tests/functional/cli_root/zinject/zinject_args.ksh
# A line only the fixed test has.
FIXED="for disk in \$DISKS; do"
DM='VERIFY|ASSERT|PANIC|BUG:|Oops|WARNING:|hung_task|blocked for more|soft lockup|Call Trace'
FAILS=0
ME=$(readlink -f "$0")
mkdir -p "$W/run" && touch "$LOG" && cd "$W" || exit 3

log() { printf '%s\n' "$*" | tee -a "$LOG"; }
die() { log "STOP $*"; exit 3; }
chk() {
	local n=$1
	shift
	if "$@" > /dev/null 2>&1; then
		log "CHECK $n PASS"
	else
		log "CHECK $n FAIL: $*"
		FAILS=$((FAILS + 1))
	fi
}
as() { runuser -u "$U" -- "$@"; }
eq() { [ "$1" = "$2" ]; }
dmesg_n() { dmesg | grep -ciE "$DM"; }
loaded() { lsmod | awk '$1 == "zfs" || $1 == "spl" {f = 1} END {exit !f}'; }
gitrev() {
	sed -n 's/^#define[[:blank:]]ZFS_META_GITREV "\(.*\)"$/\1/p' \
	    "$1/include/zfs_gitrev.h"
}
mine() {
	losetup -l -n -O NAME,BACK-FILE 2> /dev/null |
	    awk -v d="$FD/" 'index($2, d) == 1 {print $1}'
}

# What a killed run leaves: injections, testpool*, our loop devices, files.
leftovers() {
	local z=$SRC/master p l
	if [ -e /dev/zfs ]; then
		timeout 60 "$z/zinject" -c all > /dev/null 2>&1
		for p in $(timeout 60 "$z/zpool" list -H -o name 2> /dev/null); do
			case $p in
			testpool*)
				timeout 300 "$z/zpool" destroy -f "$p" >> "$LOG" 2>&1 ||
				    return 1
				;;
			*) ;;
			esac
		done
	fi
	for l in $(mine); do
		losetup -d "$l" || return 1
	done
	rm -f "$FD"/file-vdev* "$FD"/zfs-tests.*.run
	[ -z "$(mine)" ]
}

# The modules of tree $1, checked against what that tree built.
load() {
	local t=$1
	if loaded; then
		timeout 300 "$t/scripts/zfs.sh" -u >> "$LOG" 2>&1 || return 1
	fi
	timeout 300 "$t/scripts/zfs.sh" >> "$LOG" 2>&1 || return 1
	eq "$(cat /sys/module/zfs/srcversion)" \
	    "$(modinfo -F srcversion "$t/module/zfs.ko")" || return 1
	eq "$(cat /proc/sys/kernel/spl/gitrev)" "$(gitrev "$t")" || return 1
	dmesg | grep 'ZFS: Loaded module' | tail -n 1 | grep -q 'DEBUG mode'
}

setup() {
	local t
	eq "$(id -u)" 0 || die "setup must run as root"
	if loaded; then
		die "a zfs or spl module is already loaded: not a clean VM"
	fi
	for t in ksh sudo runuser losetup git modinfo; do
		command -v "$t" > /dev/null || die "$t is missing (step 1)"
	done
	id "$U" > /dev/null 2>&1 || useradd -m -s /bin/bash "$U" ||
	    die "useradd $U"
	if ! { printf '%s ALL=(ALL) NOPASSWD: ALL\n' "$U" > \
	    /etc/sudoers.d/za-zts && chmod 0440 /etc/sudoers.d/za-zts &&
	    visudo -cf /etc/sudoers.d/za-zts > /dev/null; }; then
		die "sudoers"
	fi
	chk sudo eq "$(as sudo -n id -un)" root
	if ! { install -d -o "$U" "$SRC" && install -d -m 1777 "$FD"; }; then
		die "mkdir"
	fi
	for t in master fix; do
		if [ ! -d "$SRC/$t/.git" ]; then
			timeout 1800 as git clone -q --depth 2 -b "$BR" "$REPO" \
			    "$SRC/$t" || die "clone $t"
		fi
	done
	timeout 300 as git -C "$SRC/master" checkout -q --detach "$MASTER" ||
	    die "checkout master"
	chk head_master eq "$(as git -C "$SRC/master" rev-parse HEAD)" "$MASTER"
	chk head_fix eq "$(as git -C "$SRC/fix" rev-parse HEAD)" "$FIX"
	chk test_fix grep -qF "$FIXED" "$SRC/fix/tests/zfs-tests/$TP"
	chk test_master_unfixed eq \
	    "$(grep -cF "$FIXED" "$SRC/master/tests/zfs-tests/$TP")" 0
	[ "$FAILS" = 0 ] || die "setup: $FAILS checks failed"
	for t in master fix; do
		if [ ! -f "$SRC/$t/module/zfs.ko" ]; then
			(cd "$SRC/$t" && timeout 7200 runuser -u "$U" -- sh -c \
			    "./autogen.sh && ./configure --enable-debug &&
			    make -j$(nproc)") > "$W/build-$t.log" 2>&1
			log "INFO build $t rc=$?"
		fi
		chk "ko_$t" test -f "$SRC/$t/module/zfs.ko" -a \
		    -f "$SRC/$t/module/spl.ko"
		chk "tools_$t" test -x "$SRC/$t/zinject" -a -x "$SRC/$t/zpool" -a \
		    -f "$SRC/$t/scripts/common.sh"
		chk "debug_$t" grep -q '^#define ZFS_DEBUG 1' "$SRC/$t/zfs_config.h"
		log "INFO tree $t $(gitrev "$SRC/$t")" \
		    "srcversion=$(modinfo -F srcversion "$SRC/$t/module/zfs.ko")"
	done
	for t in master fix; do
		chk "load_$t" load "$SRC/$t"
	done
	chk unload timeout 300 "$SRC/fix/scripts/zfs.sh" -u
	log "DONE setup $(date -Is): $FAILS checks failed"
	[ "$FAILS" = 0 ]
}

run_one() {
	local i=$1 tree=$2 t=$SRC/$2 o=$W/run/$1-$2 rc res ld d0 d1 s0 s1 l
	leftovers || die "run $i $tree: leftovers not cleaned; reboot, rerun"
	load "$t" || die "run $i $tree: modules of $t not loaded; reboot, rerun"
	d0=$(dmesg_n)
	s0=$(date +%s)
	(cd "$t" && timeout -k 60 1500 runuser -u "$U" -- \
	    ./scripts/zfs-tests.sh -v -d "$FD" -t "$TP") > "$o.out" 2>&1
	rc=$?
	s1=$(date +%s)
	d1=$(dmesg_n)
	res=$(awk '/Test: .*\/zinject_args(\.ksh)? / {r = $NF}
	    END {sub(/^\[/, "", r); sub(/\]$/, "", r); print r}' "$o.out")
	[ -n "$res" ] || res=NORESULT
	if [ "$rc" = 124 ] || [ "$rc" = 137 ]; then
		res=TIMEOUT
	fi
	ld=$(awk '/^Log directory/ {print $3}' "$o.out")
	if [ -n "$ld" ] && [ -d "$ld" ]; then
		cp -a "$ld" "$o.results"
	fi
	if [ "$i" = 1 ]; then
		log "INFO $(grep -m 1 'Missing util' "$o.out")"
	fi
	log "RUN $i $tree $res rc=$rc s=$((s1 - s0)) dmesg_new=$((d1 - d0))" \
	    "log=${ld:--}"
	if [ "$res" != PASS ]; then
		{
			grep 'Test: .*zinject_args' "$o.out"
			if [ -f "$o.results/log" ]; then
				grep -E 'ERROR|FAIL|KILLED|did not execute|inject=' \
				    "$o.results/log"
			fi
		} | while IFS= read -r l; do
			log "FAILLINE $i $tree $l"
		done
	fi
	if [ "$d1" -gt "$d0" ]; then
		dmesg | grep -iE "$DM" | tail -n $((d1 - d0)) |
		    while IFS= read -r l; do
			log "DMESG $i $tree $l"
		done
		dmesg > "$o.dmesg"
	fi
}

run() {
	local per=${1:-20} n i
	eq "$(id -u)" 0 || die "run must run as root"
	n=$(grep -c '^RUN ' "$LOG")
	i=$((n + 1))
	while [ "$i" -le $((2 * per)) ]; do
		if [ $((i % 2)) = 1 ]; then
			run_one "$i" master
		else
			run_one "$i" fix
		fi
		i=$((i + 1))
	done
	summary
}

summary() {
	local t
	for t in master fix; do
		echo "$t: $(grep -c "^RUN [0-9]* $t PASS " "$LOG") PASS of" \
		    "$(grep -c "^RUN [0-9]* $t " "$LOG") runs, seconds" \
		    "$(awk -v t="$t" '$1 == "RUN" && $3 == t {
		    s = substr($6, 3) + 0; if (n++ == 0 || s < lo) lo = s
		    if (s > hi) hi = s} END {print (n ? lo "-" hi : "-")}' "$LOG")"
	done
	grep -E '^(RUN [0-9]+ [a-z]+ [^P]|RUN .* (rc|dmesg_new)=[1-9])' "$LOG"
	grep -E '^(FAILLINE|DMESG|STOP|CHECK .* FAIL|INFO)' "$LOG"
}

clean() {
	leftovers || die "clean: leftovers not cleaned"
	if loaded; then
		timeout 300 "$SRC/master/scripts/zfs.sh" -u || die "clean: unload"
	fi
	log "DONE clean $(date -Is)"
}

case "${1:-}" in
setup) setup ;;
run) run "${2:-20}" ;;
summary) summary ;;
clean) clean ;;
*) sed -n '2,16p' "$ME"; exit 2 ;;
esac
