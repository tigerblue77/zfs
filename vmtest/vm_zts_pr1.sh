#!/usr/bin/env bash
#
# vm_zts_pr1.sh - build and test the "zpool remove of an empty vdev beside
# raidz" change (branch zpool-remove-empty-vdev) on a DISPOSABLE Debian 13
# (trixie) x86_64 VM, with the OpenZFS test suite (ZTS).
#
# Install as root:  /opt/zts/vm_zts_pr1.sh  (root:root, 0755), run as root.
#
# NOTE: the branch is NOT on the remote until the maintainer pushes it
# (https://github.com/tigerblue77/zfs, branch zpool-remove-empty-vdev).  Until
# then "build" fails at the clone step with a clear message.  Commit under
# test: 4e0c3a2d7238b3085c0d9b65935a49c7080f8668, parent 4542a317e2fa.
#
# Subcommands (re-runnable; completed steps are recorded in
# /var/lib/zts-pr1/state and skipped; "reset" clears them):
#   prepare   apt dependencies and kernel headers, XFS on the ZTSTESTS disk
#             mounted on /var/tmp, sysctl/journald/rcu settings, test user
#             (exit 3 = reboot needed, then run prepare again)
#   build     clone patched + base trees, autogen/configure --enable-debug/make
#   test      BASE tree: the two new tests (expected to FAIL before the fix);
#             PATCHED tree: the two new tests plus neighbouring tests
#   demo      OPTIONAL manual demonstration on the six ZTSD1..6 disks
#   all       prepare, build, test
#   status    show recorded state and results
#   unload    unload the ZFS modules loaded from the in-tree builds
#   reset     forget recorded steps and results (does not delete trees)
#
# Facts that shape this script:
#  - zfs-tests.sh refuses to run as root and needs passwordless sudo, so a
#    local user "zts" (with a sudoers drop-in) owns the trees and runs the
#    suite.  /root is mode 0700, hence the trees live in /var/tmp/zts-pr1
#    (the XFS disk) and not in /root/zts-pr1.
#  - Tests run through "zfs-tests.sh -vKO -s 3GB -t <path>" from /var/tmp,
#    as the upstream CI does; modules come from the in-tree build via
#    scripts/zfs.sh.  scripts/zfs-helpers.sh is NOT run (no changes under
#    /etc or /lib/udev).
#  - Safety: refuses to run if a pool is imported, the zfs module is loaded,
#    Debian ZFS packages are installed, or the disk with serial ZTSTESTS is
#    absent.  Never uses "zfs-tests.sh -x".  Only the ZTSTESTS disk (once, if
#    blank) and, in "demo", the ZTSD1..6 disks are written by this script.
#
# Logs: /var/log/zts-pr1/ (run.log, build-*.log, <tree>-<test>.zts.log,
# .dmesg.before/.after, .results/, results.tsv).

set -eEuo pipefail
umask 022
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
export LC_ALL=C DEBIAN_FRONTEND=noninteractive

REPO_URL=https://github.com/tigerblue77/zfs
BRANCH=zpool-remove-empty-vdev-v2
PATCHED_COMMIT=bab1a6eb8677f11cadd39c27ec495c0e33b8cc09
BASE_COMMIT=c27dff0c3
TESTS_SERIAL=ZTSTESTS
DEMO_SERIALS="ZTSD1 ZTSD2 ZTSD3 ZTSD4 ZTSD5 ZTSD6"
TEST_USER=zts
WORK=/var/tmp/zts-pr1
LOG=/var/log/zts-pr1
STATE_DIR=/var/lib/zts-pr1
STATE=$STATE_DIR/state
RESULTS=$LOG/results.tsv
FUNC=tests/functional
FUNCDIR=tests/zfs-tests/tests/functional
START=$(date +%s)
STEP=init
REBOOT_NEEDED=0
SCRIPT=$0

NEW_TESTS="removal/remove_raidz_empty removal/remove_raidz_busy"
NEIGHBOURS="removal/remove_raidz removal/removal_with_add removal/remove_mirror
    pool_checkpoint/checkpoint_removal pool_checkpoint/checkpoint_vdev_add"
KERN_BAD='BUG:|Oops|Call Trace|general protection|Kernel panic|VERIFY|PANIC|blocked for more than|rcu_sched.*stall|WARNING: CPU:'

trap 'rc=$?; printf "FAILED: step=%s line=%s exit=%s (logs: %s)\n" "$STEP" "$LINENO" "$rc" "$LOG" >&2' ERR

say() {
	printf '[%s] %s\n' "$(date +%H:%M:%S)" "$*"
	printf '[%s] %s\n' "$(date '+%F %T')" "$*" >>"$LOG/run.log"
}
die() { printf 'ERROR: %s\n' "$*" >&2; exit 1; }

is_done() { grep -qx "$1" "$STATE" 2>/dev/null; }
mark() { if ! is_done "$1"; then echo "$1" >>"$STATE"; fi; }
unmark() { sed -i "/^$1\$/d" "$STATE"; }
as_zts() {
	runuser -u "$TEST_USER" -- env HOME="/home/$TEST_USER" \
	    USER="$TEST_USER" LOGNAME="$TEST_USER" "$@"
}

find_disk() {
	local out n
	out=$(lsblk -dnpo NAME,SERIAL 2>/dev/null | awk -v s="$1" '$2==s {print $1}')
	n=$(printf '%s\n' "$out" | grep -c . || true)
	if [ "$n" != 1 ]; then
		echo "disk with serial $1: found $n (expected 1)" >&2
		return 1
	fi
	printf '%s\n' "$out"
}

by_id() {
	local l
	for l in /dev/disk/by-id/*"$2"*; do
		[ -e "$l" ] || continue
		case $l in *-part*) continue ;; esac
		if [ "$(readlink -f "$l")" = "$1" ]; then
			printf '%s\n' "$l"
			return 0
		fi
	done
	return 1
}

# Stable name for a disk already identified by its serial: its by-id link if
# udev made one (this VM's did not), else the device node itself.
disk_ref() {
	by_id "$1" "$2" 2>/dev/null || printf '%s\n' "$1"
}

imported_pools() {
	local z out=""
	[ -e /dev/zfs ] || return 0
	for z in "$(command -v zpool || true)" "$WORK/patched/zpool" "$WORK/base/zpool"; do
		if [ -z "$z" ] || [ ! -x "$z" ]; then continue; fi
		out="$out $("$z" list -H -o name 2>/dev/null || true)"
	done
	printf '%s\n' "$out" | tr -s ' ' | sed 's/^ //'
}

guard() {
	local pools pk dev
	[ "$(id -u)" = 0 ] || die "must run as root"
	# shellcheck disable=SC1091
	. /etc/os-release
	[ "${ID:-}" = debian ] || die "not Debian (disposable Debian 13 VM only)"
	install -d -m 0755 "$LOG" "$STATE_DIR"
	touch "$LOG/run.log" "$STATE"
	chmod 0644 "$LOG/run.log"
	pools=$(imported_pools)
	[ -z "${pools// /}" ] || die "ZFS pool(s) imported: $pools; refusing (disposable VM only)"
	if [ "${1:-}" != allow-loaded ]; then
		if [ -d /sys/module/zfs ] || grep -q '^zfs ' /proc/modules; then
			die "zfs module is loaded; run '$SCRIPT unload' first"
		fi
	fi
	pk=$(dpkg-query -W -f '${db:Status-Abbrev} ${Package}\n' 2>/dev/null |
	    awk '$1=="ii" && $2 ~ /^(zfs|libzfs|libzpool|libnvpair|libuutil|spl)/ {print $2}' | tr '\n' ' ')
	[ -z "$pk" ] || die "Debian ZFS packages installed: $pk"
	dev=$(find_disk "$TESTS_SERIAL") || die "tests disk (serial $TESTS_SERIAL) not found"
}

require_tmp_mount() {
	local dev src
	dev=$(find_disk "$TESTS_SERIAL")
	src=$(findmnt -rno SOURCE -M /var/tmp 2>/dev/null || true)
	[ "$src" = "$dev" ] || die "/var/tmp is not the $TESTS_SERIAL disk; run prepare"
}

# ---------------------------------------------------------------- prepare

prep_system() {
	STEP=prepare:system
	printf 'kernel.panic = 10\nkernel.panic_on_oops = 1\n' >/etc/sysctl.d/90-zts-pr1.conf
	sysctl -q -p /etc/sysctl.d/90-zts-pr1.conf >/dev/null
	install -d /etc/systemd/journald.conf.d
	printf '[Journal]\nStorage=persistent\n' >/etc/systemd/journald.conf.d/90-zts-pr1.conf
	install -d -m 2755 /var/log/journal
	systemctl restart systemd-journald
	journalctl --flush >/dev/null 2>&1 || true
	printf 'w /sys/module/rcupdate/parameters/rcu_cpu_stall_timeout - - - - 120\n' \
	    >/etc/tmpfiles.d/90-zts-pr1.conf
	if [ -w /sys/module/rcupdate/parameters/rcu_cpu_stall_timeout ]; then
		echo 120 >/sys/module/rcupdate/parameters/rcu_cpu_stall_timeout
	fi
	echo loop >/etc/modules-load.d/90-zts-pr1.conf
	modprobe loop
	mark prep_system
}

prep_apt() {
	STEP=prepare:apt
	local p alog=$LOG/prepare-apt.log
	local required="build-essential autoconf automake libtool libtool-bin gawk bc
	    cpio git curl wget ca-certificates kmod udev util-linux sudo ksh fio jq acl
	    attr cryptsetup parted lsscsi psmisc xfsprogs xfslibs-dev e2fsprogs rsync
	    strace sysstat python3 python3-dev python3-cffi python3-setuptools
	    python3-packaging python3-distlib libacl1-dev libaio-dev libattr1-dev
	    libblkid-dev libcurl4-openssl-dev libdevmapper-dev libelf-dev libffi-dev
	    libmount-dev libpam0g-dev libselinux1-dev libssl-dev libudev-dev
	    libunwind-dev libtirpc-dev uuid-dev zlib1g-dev xxhash binutils file
	    libcap2-bin iputils-ping openssh-client openssl pax procps"
	local optional="pamtester linux-perf gdb lsof samba-common-bin zip unzip"
	say "apt: update and install dependencies (log: $alog)"
	apt-get update -qq >>"$alog" 2>&1
	# shellcheck disable=SC2086
	apt-get install -y -qq --no-install-recommends $required >>"$alog" 2>&1
	for p in $optional; do
		apt-get install -y -qq --no-install-recommends "$p" >>"$alog" 2>&1 ||
		    say "optional package $p could not be installed (continuing)"
	done
	for p in ksh fio sudo runuser mkfs.xfs git make gcc autoreconf parted lsscsi visudo; do
		command -v "$p" >/dev/null 2>&1 || die "missing command after install: $p"
	done
	mark prep_apt
}

prep_disk() {
	STEP=prepare:disk
	local dev id src fs pt m uuid
	dev=$(find_disk "$TESTS_SERIAL")
	id=$(disk_ref "$dev" "$TESTS_SERIAL")
	src=$(findmnt -rno SOURCE -M /var/tmp 2>/dev/null || true)
	if [ -z "$src" ]; then
		while read -r m; do
			[ -z "$m" ] || die "$dev is mounted at $m; refusing"
		done < <(lsblk -nr -o MOUNTPOINTS "$dev")
		fs=$(blkid -p -o value -s TYPE "$dev" 2>/dev/null || true)
		pt=$(blkid -p -o value -s PTTYPE "$dev" 2>/dev/null || true)
		if [ -z "$fs" ] && [ -z "$pt" ]; then
			say "formatting $id as XFS"
			mkfs.xfs -q -L ZTSTESTS "$dev"
		elif [ "$fs" = xfs ] && [ -z "$pt" ]; then
			say "$id already holds XFS; reusing"
		else
			die "$dev has existing content (fs='$fs' pt='$pt'); refusing to format"
		fi
		if ! grep -qE '[[:space:]]/var/tmp[[:space:]]' /etc/fstab; then
			uuid=$(blkid -p -o value -s UUID "$dev")
			[ -n "$uuid" ] || die "no filesystem UUID on $dev"
			printf 'UUID=%s /var/tmp xfs defaults,noatime,nofail 0 2\n' "$uuid" >>/etc/fstab
		fi
		systemctl daemon-reload
		mount /var/tmp
	elif [ "$src" != "$dev" ]; then
		die "/var/tmp is already a different mount ($src)"
	fi
	chmod 1777 /var/tmp
	require_tmp_mount
	mark prep_disk
}

prep_user() {
	STEP=prepare:user
	local tmp
	id "$TEST_USER" >/dev/null 2>&1 || useradd -m -s /bin/bash "$TEST_USER"
	tmp=$(mktemp)
	printf '%s ALL=(ALL) NOPASSWD: ALL\n' "$TEST_USER" >"$tmp"
	visudo -cf "$tmp" >/dev/null
	install -m 0440 -o root -g root "$tmp" /etc/sudoers.d/90-zts-pr1
	rm -f "$tmp"
	install -d -o "$TEST_USER" -g "$TEST_USER" "$WORK"
	mark prep_user
}

prep_headers() {
	STEP=prepare:headers
	local k pkg cand
	k=$(uname -r)
	if [ ! -e "/lib/modules/$k/build/Makefile" ]; then
		pkg=linux-headers-$k
		cand=$(apt-cache policy "$pkg" 2>/dev/null | awk '/Candidate:/ {print $2}')
		if [ -n "$cand" ] && [ "$cand" != "(none)" ]; then
			say "installing $pkg"
			apt-get install -y -qq "$pkg" >>"$LOG/prepare-apt.log" 2>&1
		else
			say "no candidate for $pkg; installing linux-image-cloud-amd64 and linux-headers-cloud-amd64"
			apt-get install -y -qq linux-image-cloud-amd64 linux-headers-cloud-amd64 \
			    >>"$LOG/prepare-apt.log" 2>&1
			REBOOT_NEEDED=1
			return 0
		fi
		[ -e "/lib/modules/$k/build/Makefile" ] || die "headers for $k still missing"
	fi
	mark prep_headers
}

step_prepare() {
	is_done prep_system || prep_system
	is_done prep_apt || prep_apt
	is_done prep_disk || prep_disk
	is_done prep_user || prep_user
	is_done prep_headers || prep_headers
	if [ "$REBOOT_NEEDED" = 1 ]; then
		say "REBOOT NEEDED: a newer cloud kernel and its headers were installed; reboot, then run '$SCRIPT prepare' again"
	else
		mark prepare
		say "prepare: done"
	fi
}

# ------------------------------------------------------------------ build

build_tree() {
	local name=$1 dir=$WORK/$1 blog=$LOG/build-$1.log ko vm
	if is_done "build_$name"; then
		say "build $name: already done"
		return 0
	fi
	say "build $name: autogen, configure --enable-debug, make (log: $blog)"
	# shellcheck disable=SC2016
	as_zts bash -c 'set -e; cd "$1"; ./autogen.sh; ./configure --enable-debug; make -j"$(nproc)"' \
	    _ "$dir" >"$blog" 2>&1
	ko=$(find "$dir/module" -maxdepth 2 -name zfs.ko | head -n 1)
	[ -n "$ko" ] || die "build $name: zfs.ko not found (see $blog)"
	vm=$(modinfo -F vermagic "$ko" | awk '{print $1}')
	[ "$vm" = "$(uname -r)" ] || die "build $name: module vermagic $vm != running kernel $(uname -r)"
	[ -x "$dir/zpool" ] || die "build $name: $dir/zpool missing"
	[ -x "$dir/scripts/zfs-tests.sh" ] || die "build $name: zfs-tests.sh missing"
	mark "build_$name"
}

step_build() {
	STEP=build
	local head prev
	if ! is_done prepare; then
		die "run '$SCRIPT prepare' first (and reboot if it asked for it)"
	fi
	require_tmp_mount
	if [ ! -d "$WORK/patched/.git" ]; then
		say "cloning $REPO_URL branch $BRANCH"
		as_zts git clone -q --branch "$BRANCH" "$REPO_URL" "$WORK/patched" \
		    >"$LOG/clone.log" 2>&1 ||
		    die "clone failed (branch not pushed yet?); see $LOG/clone.log"
	fi
	head=$(as_zts git -C "$WORK/patched" rev-parse HEAD)
	if [ "$head" != "$PATCHED_COMMIT" ]; then
		say "patched tree is at $head; moving it to $PATCHED_COMMIT and keeping the earlier results"
		as_zts git -C "$WORK/patched" fetch -q origin "$BRANCH" >>"$LOG/clone.log" 2>&1 ||
		    die "fetch of $BRANCH failed; see $LOG/clone.log"
		as_zts git -C "$WORK/patched" checkout -q --detach "$PATCHED_COMMIT" >>"$LOG/clone.log" 2>&1 ||
		    die "cannot check out $PATCHED_COMMIT; see $LOG/clone.log"
		prev=$LOG/before-${head:0:12}
		install -d "$prev"
		mv -f "$RESULTS" "$LOG"/*.zts.log "$LOG"/*.dmesg.* "$LOG"/*.results "$prev"/ 2>/dev/null || true
		rm -f "$STATE_DIR/inprogress"
		unmark build_patched
		unmark build
		head=$(as_zts git -C "$WORK/patched" rev-parse HEAD)
	fi
	[ "$head" = "$PATCHED_COMMIT" ] ||
	    die "branch tip is $head, expected $PATCHED_COMMIT; update PATCHED_COMMIT if intended"
	if [ ! -d "$WORK/base/.git" ]; then
		say "creating base tree at $BASE_COMMIT"
		as_zts git clone -q --local "$WORK/patched" "$WORK/base" >>"$LOG/clone.log" 2>&1
		as_zts git -C "$WORK/base" checkout -q "$BASE_COMMIT" >>"$LOG/clone.log" 2>&1
	fi
	head=$(as_zts git -C "$WORK/base" rev-parse HEAD)
	case $head in "$BASE_COMMIT"*) ;; *) die "base tree is at $head, expected $BASE_COMMIT" ;; esac
	[ "$(as_zts git -C "$WORK/patched" rev-parse 'HEAD^')" = "$head" ] ||
	    die "base commit is not the parent of the patched commit"
	build_tree base
	build_tree patched
	mark build
	say "build: done"
}

# ------------------------------------------------------------------- test

record() { printf '%s\t%s\t%s\t%s\t%s\n' "$1" "$2" "$3" "$4" "$5" >>"$RESULTS"; }
have_result() { grep -q "^$1	$2	" "$RESULTS" 2>/dev/null; }

run_one() {
	local tree=$1 rel=$2 name tag rc=0 res bad rdir inprog
	name=$(basename "$rel")
	tag=$tree-$name
	inprog=$STATE_DIR/inprogress
	if [ -f "$inprog" ] && [ "$(cat "$inprog")" = "$tag" ]; then
		record "$tree" "$rel" CRASH - -
		rm -f "$inprog"
		die "an earlier run of $tag did not finish (host reset or hang); recorded as CRASH; see $LOG and 'journalctl -k -b -1'"
	fi
	have_result "$tree" "$rel" && return 0
	say "run $tree: $rel"
	dmesg >"$LOG/$tag.dmesg.before"
	dmesg -C
	echo "$tag" >"$inprog"
	rm -rf /var/tmp/test_results
	(cd /var/tmp && timeout 2400 runuser -u "$TEST_USER" -- env HOME="/home/$TEST_USER" \
	    USER="$TEST_USER" LOGNAME="$TEST_USER" PATH="$PATH" \
	    "$WORK/$tree/scripts/zfs-tests.sh" -vKO -s 3GB -t "$FUNC/$rel.ksh") \
	    >"$LOG/$tag.zts.log" 2>&1 || rc=$?
	rm -f "$inprog"
	dmesg >"$LOG/$tag.dmesg.after"
	res=$(sed -n "s/^.*Test: .*\\/$name\\.ksh .*\\[\\(PASS\\|FAIL\\|SKIP\\|KILLED\\)\\].*/\\1/p" \
	    "$LOG/$tag.zts.log" | tail -n 1)
	[ -n "$res" ] || res=NORESULT
	[ "$rc" != 124 ] || res=TIMEOUT
	bad=$(grep -cE "$KERN_BAD" "$LOG/$tag.dmesg.after" || true)
	rdir=$(awk '/^Log directory/ {print $3}' "$LOG/$tag.zts.log" | tail -n 1)
	if [ -n "$rdir" ] && [ -d "$rdir" ]; then
		rm -rf "$LOG/$tag.results"
		cp -r "$rdir" "$LOG/$tag.results"
		chmod -R u=rwX,go=rX "$LOG/$tag.results"
	fi
	record "$tree" "$rel" "$res" "$bad" "$rc"
	say "  $tree $name: $res (exit $rc, kernel-warnings $bad)"
	if [ "$res" = TIMEOUT ]; then
		pkill -u "$TEST_USER" || true
		die "$tag timed out; stopping (modules left loaded; run '$SCRIPT unload' after checking)"
	fi
}

test_tree() {
	local tree=$1 list=$2 t pending=0
	for t in $list; do
		have_result "$tree" "$t" || pending=1
	done
	[ "$pending" = 1 ] || { say "test $tree: nothing pending"; return 0; }
	if [ -d /sys/module/zfs ]; then
		die "zfs module already loaded; run '$SCRIPT unload'"
	fi
	for t in $list; do
		run_one "$tree" "$t"
	done
	say "unloading modules ($tree)"
	"$WORK/$tree/scripts/zfs.sh" -u >"$LOG/unload-$tree.log" 2>&1 ||
	    die "module unload failed after $tree tests; see $LOG/unload-$tree.log"
}

step_test() {
	STEP="test"
	local t d f base_list patched_list=""
	is_done build || die "run '$SCRIPT build' first"
	require_tmp_mount
	touch "$RESULTS"
	chmod 0644 "$RESULTS"
	for t in $NEW_TESTS; do
		[ -f "$WORK/patched/$FUNCDIR/$t.ksh" ] || die "missing in patched tree: $t.ksh"
		install -d -o "$TEST_USER" -g "$TEST_USER" "$(dirname "$WORK/base/$FUNCDIR/$t.ksh")"
		as_zts cp -p "$WORK/patched/$FUNCDIR/$t.ksh" "$WORK/base/$FUNCDIR/$t.ksh"
		[ -x "$WORK/base/$FUNCDIR/$t.ksh" ] || die "copied test is not executable: $t.ksh"
	done
	base_list="$NEW_TESTS cli_root/zpool_add/zpool_add_004_pos"
	for d in cli_root/zpool_remove cli_root/zpool_add; do
		for f in "$WORK/patched/$FUNCDIR/$d"/*.ksh; do
			case ${f##*/} in setup.ksh | cleanup.ksh) continue ;; esac
			patched_list="$patched_list $d/$(basename "$f" .ksh)"
		done
	done
	patched_list="$NEW_TESTS $NEIGHBOURS $patched_list"
	for t in $patched_list; do
		[ -f "$WORK/patched/$FUNCDIR/$t.ksh" ] || die "missing in patched tree: $t.ksh"
	done
	test_tree base "$base_list"
	test_tree patched "$patched_list"
	mark test
	summary
}

summary() {
	local tree rel res bad rc exp flag fails="" n_pass=0 n_fail=0 n_skip=0 n_other=0 now
	now=$(date +%s)
	echo "==================== SUMMARY ===================="
	echo "kernel:   $(uname -r)"
	echo "patched:  $PATCHED_COMMIT"
	echo "base:     $BASE_COMMIT"
	[ -s "$RESULTS" ] || { echo "no results recorded"; return 0; }
	while IFS=$'\t' read -r tree rel res bad rc; do
		exp=PASS
		if [ "$tree" = base ]; then exp=FAIL; fi
		flag=""
		[ "$res" = "$exp" ] || flag=" <-- differs from expected $exp"
		[ "$bad" = 0 ] || [ "$bad" = - ] || flag="$flag [kernel warnings: $bad]"
		printf '%-8s %-8s %s%s\n' "$tree" "$res" "$rel" "$flag"
		case $res in
		PASS) n_pass=$((n_pass + 1)) ;;
		FAIL) n_fail=$((n_fail + 1)) ;;
		SKIP) n_skip=$((n_skip + 1)) ;;
		*) n_other=$((n_other + 1)) ;;
		esac
		if [ "$tree" = patched ] && [ "$res" != PASS ] && [ "$res" != SKIP ]; then
			fails="$fails $rel"
		fi
		if [ "$tree" = base ] && [ "$res" != FAIL ]; then
			fails="$fails base:$rel"
		fi
	done <"$RESULTS"
	echo "counts:   PASS=$n_pass FAIL=$n_fail SKIP=$n_skip OTHER=$n_other"
	echo "problems: ${fails:-none}"
	echo "logs:     $LOG (results.tsv, <tree>-<test>.zts.log, .dmesg.*, .results/)"
	echo "wall time of this invocation: $((now - START)) s"
}

# ------------------------------------------------------------------- demo

demo_cleanup() {
	local z=$WORK/patched/zpool d
	if [ -e /dev/zfs ] && [ -x "$z" ]; then
		"$z" destroy -f zts_demo >/dev/null 2>&1 || true
	fi
	for d in ${DEMO_DEVS:-}; do
		wipefs -a "$d" >/dev/null 2>&1 || true
	done
}

step_demo() {
	STEP=demo
	local s dev id m f z ids="" p
	local -a dv
	is_done build || die "run '$SCRIPT build' first"
	echo "MANUAL DEMONSTRATION (not a test): raidz1 pool zts_demo on the ZTSD1..6 disks."
	echo "The disks are wiped at the end. Timing-dependent: see the limits note at the end."
	DEMO_DEVS=""
	for s in $DEMO_SERIALS; do
		dev=$(find_disk "$s") || die "demo disk $s not found"
		id=$(disk_ref "$dev" "$s")
		while read -r m; do
			[ -z "$m" ] || die "$dev is mounted at $m"
		done < <(lsblk -nr -o MOUNTPOINTS "$dev")
		while read -r f; do
			case $f in "" | zfs_member) ;; *) die "$dev holds a $f filesystem; refusing" ;; esac
		done < <(lsblk -nro FSTYPE "$dev")
		ids="$ids $id"
		DEMO_DEVS="$DEMO_DEVS $dev"
	done
	read -ra dv <<<"$ids"
	z=$WORK/patched/zpool
	trap demo_cleanup EXIT
	say "demo: loading patched modules"
	"$WORK/patched/scripts/zfs.sh" >"$LOG/demo-load.log" 2>&1
	{
		"$z" create -f -o ashift=12 zts_demo raidz1 "${dv[0]}" "${dv[1]}" "${dv[2]}"
		"$z" add -f zts_demo raidz1 "${dv[3]}" "${dv[4]}" "${dv[5]}"
		"$z" set allocating=off zts_demo raidz1-1
		echo "----- BEFORE REMOVE -----"
		"$z" status zts_demo
		"$z" list -v zts_demo
	} >"$LOG/demo.log" 2>&1 || { cat "$LOG/demo.log" >&2; die "demo setup failed"; }
	sed -n '/BEFORE REMOVE/,$p' "$LOG/demo.log" | sed 's#/dev/disk/by-id/##'
	p=0
	{
		echo "----- REMOVE raidz1-1 -----"
		"$z" remove zts_demo raidz1-1 || p=$?
		echo "(zpool remove exit status: $p)"
		sleep 3
		echo "----- AFTER REMOVE -----"
		"$z" status zts_demo
		"$z" list -v zts_demo
	} >>"$LOG/demo.log" 2>&1 || true
	sed -n '/REMOVE raidz1-1/,$p' "$LOG/demo.log" | sed 's#/dev/disk/by-id/##'
	if [ "$p" = 0 ]; then
		echo "DEMO RESULT: removal of the empty raidz1-1 succeeded"
	else
		echo "DEMO RESULT: removal failed (exit $p); if the vdev had received data this is the documented limit"
	fi
	echo "Limit: a normal-class vdev stays empty only until the first txg that writes to it; allocating=off must be set before that."
	demo_cleanup
	trap - EXIT
	"$WORK/patched/scripts/zfs.sh" -u >>"$LOG/demo-load.log" 2>&1 ||
	    die "module unload failed after demo"
	say "demo: done (log: $LOG/demo.log); ZTSD disks wiped"
}

# ------------------------------------------------------------------- misc

step_unload() {
	local t
	for t in patched base; do
		if [ -x "$WORK/$t/scripts/zfs.sh" ]; then
			"$WORK/$t/scripts/zfs.sh" -u >"$LOG/unload-$t.log" 2>&1 && break
		fi
	done
	if [ -d /sys/module/zfs ]; then die "modules still loaded; see $LOG/unload-*.log"; fi
	say "modules unloaded"
}

usage() {
	sed -n '/^# Subcommands/,/^#   reset/p' "$SCRIPT" | sed 's/^# \{0,1\}//'
}

main() {
	local cmd=${1:-help}
	case $cmd in
	help | -h | --help) usage; exit 0 ;;
	esac
	[ "$(id -u)" = 0 ] || die "must run as root"
	case $cmd in
	prepare) guard; step_prepare ;;
	build) guard; step_build ;;
	test) guard; step_test ;;
	demo) guard; step_demo ;;
	all)
		guard
		step_prepare
		if [ "$REBOOT_NEEDED" = 1 ]; then exit 3; fi
		step_build
		step_test
		;;
	status)
		guard allow-loaded
		cat "$STATE"
		summary
		;;
	unload) guard allow-loaded; step_unload ;;
	reset)
		guard allow-loaded
		: >"$STATE"
		rm -f "$RESULTS" "$STATE_DIR/inprogress"
		say "state and results cleared"
		;;
	*) usage; exit 2 ;;
	esac
	if [ "$REBOOT_NEEDED" = 1 ]; then exit 3; fi
}

main "$@"
