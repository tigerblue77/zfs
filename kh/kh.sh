#!/bin/bash
# SPDX-License-Identifier: CDDL-1.0
#
# kh.sh: the hybrid RAID-Z parity mover of tigerblue77/zfs
# spike/kernel-hybrid, run in the kernel on the common fixtures and on the
# hard cases.  Disposable VM only: it loads the tree's modules, imports,
# exports and creates pools named jointfixture and khpool from files under
# $KH_WORK, changes metaslab_force_ganging for a moment, and "b-cut" reboots
# the VM through sysrq.  See VM-kernel-hybrid.md.
#
#   kh.sh setup | load | a | b-dry | b-cut <hybrid|redirect> <pct> |
#         b-check | c | d | e | f | summary
#
# Every check prints "CHECK <case> <name> PASS|FAIL" into $KH_WORK/kh.log;
# mover results are logged as "MOVER <case> <key=value ...>".
set -u -o pipefail

T=${KH_TREE:-/root/kh/zfs}
GSRC=${KH_GEN:-/root/kh/fixtures}
W=${KH_WORK:-/var/tmp/kh}
P=/sys/module/zfs/parameters/raidz_spike_mover
MP=/sys/module/zfs/parameters
POOL=jointfixture
FX=$W/bin/fixture
KU=$W/bin/kmover_user
LOG=$W/kh.log
DM='VERIFY|ASSERT|PANIC|BUG:|Oops|WARNING:|hung_task|blocked for more|soft lockup|Large kmem_alloc|Call Trace'
CASE=-
FAILS=0
RES=
DM0=0
mkdir -p "$W/bin" "$W/log"

log() { printf '%s\n' "$*" | tee -a "$LOG"; }
# The first, or with fldl the last, value of a key in a result line: a move
# prints its first census, then its last one.
fld() { printf '%s\n' "$1" | tr ' ' '\n' | sed -n "s/^$2=//p" | head -n 1; }
fldl() { printf '%s\n' "$1" | tr ' ' '\n' | sed -n "s/^$2=//p" | tail -n 1; }
eq() { [ "$1" = "$2" ]; }
gt0() { [ "${1:-0}" -gt 0 ] 2>/dev/null; }
chk() {
	local n=$1
	shift
	if "$@" > /dev/null 2>&1; then
		log "CHECK $CASE $n PASS"
	else
		log "CHECK $CASE $n FAIL: $*"
		FAILS=$((FAILS + 1))
	fi
}
Z() { timeout 300 "$T/zpool" "$@" >> "$LOG" 2>&1; }
F() { timeout 300 "$T/zfs" "$@" >> "$LOG" 2>&1; }
health() { timeout 60 "$T/zpool" get -H -o value health "$POOL" 2>/dev/null; }
dmesg_n() { dmesg | grep -ciE "$DM"; }
begin() {
	CASE=$1
	DM0=$(dmesg_n)
	echo 0 > /proc/spl/kstat/zfs/dbgmsg 2> /dev/null
	log "BEGIN $CASE $(date -Is)"
}
end() {
	chk dmesg_clean eq "$(dmesg_n)" "$DM0"
	cat /proc/spl/kstat/zfs/dbgmsg > "$W/log/$CASE.dbgmsg" 2> /dev/null
	dmesg > "$W/log/$CASE.dmesg"
	log "END $CASE $(date -Is)"
}

# The mover: start a command, wait for its result line (RES).
kstart() { printf '%s\n' "$1" > "$P"; }
kwait() {
	local i=0 s
	while s=$(cat "$P"); [ "${s#running}" != "$s" ]; do
		i=$((i + 1))
		if [ "$i" -gt $(($1 * 5)) ]; then
			RES="timeout"
			log "MOVER $CASE TIMEOUT after $1 s: $s"
			return 3
		fi
		sleep 0.2
	done
	RES=$s
	log "MOVER $CASE $s"
	eq "$(fld "$s" rc)" 0
}
kmv() {
	RES=
	kstart "$1" || { log "MOVER $CASE could not start: $1"; return 2; }
	kwait "$2"
}
# The same code in userspace: the last RESULT line.
ku() {
	local d=$1
	shift
	timeout 1800 env LD_LIBRARY_PATH="$T/.libs" "$KU" "$d" "$POOL" "$@" \
	    2>&1 | tee -a "$LOG" | grep '^RESULT cmd=' | tail -n 1 |
	    sed 's/^RESULT //'
}

imp() {
	timeout 300 "$T/zpool" import -f -N -o cachefile=none \
	    ${2:+-o readonly=on} -d "$1" "$POOL" >> "$LOG" 2>&1
}
xp() { Z export "$POOL"; }
cpin() {
	rm -rf "${2:?}"
	mkdir -p "$2" && cp --sparse=always "$1"/v[0-4] "$1"/objects.txt "$2"/ &&
	    sync
}
fxrun() {
	timeout 900 env LD_LIBRARY_PATH="$T/.libs" "$FX" "$1" "$2" \
	    > "$2.$3.log" 2>&1 && tail -n 1 "$2.$3.log" | grep -q ' PASS$'
}
fxv() { fxrun verify "$1" "$2"; }
fxdrop() { fxrun drop-retained "$1" drop; }
zdbok() {
	timeout 1800 "$T/zdb" -e -p "$1" -bcc "$POOL" > "$1.zdb$2.log" 2>&1 &&
	    grep -q 'No leaks (block sum matches space maps exactly)' \
	    "$1.zdb$2.log"
}
# A scrub that repairs nothing and finds no error on any vdev.
scrubok() {
	timeout 900 "$T/zpool" scrub -w "$POOL" >> "$LOG" 2>&1 &&
	    timeout 60 "$T/zpool" status -v "$POOL" > "$1" 2>&1 &&
	    grep -q 'repaired 0B .* with 0 errors' "$1" &&
	    awk '/READ WRITE CKSUM/ {f = 1; next} /^errors:/ {f = 0}
	    f && NF >= 5 && ($3 + $4 + $5) > 0 {e = 1} END {exit e}' "$1"
}
# Files of a mounted dataset against their sources: "<file>=<source> ...".
same() {
	local d=$1 p
	shift
	for p in "$@"; do
		cmp -s "$d/${p%%=*}" "${p#*=}" || return 1
	done
}
urandom() { [ -f "$1" ] || dd if=/dev/urandom of="$1" bs="$2" count="$3" status=none; }
# A pool the kernel creates, raidz1 of five 256 MiB files, every feature:
# the fixture's root dataset has no ZPL (libzpool creates none) and its
# object 1, where the master node would be, is fixture data, so "zfs create"
# of a file system under it fails (from reading: zfs_get_zplprop() EINVAL).
kpool() {
	local i
	rm -rf "${1:?}"
	mkdir -p "$1"
	for i in 0 1 2 3 4; do truncate -s 256M "$1/v$i"; done
	timeout 300 "$T/zpool" create -f -o ashift=12 -o cachefile=none \
	    -O compression=off -O mountpoint=none "$POOL" raidz1 \
	    "$1/v0" "$1/v1" "$1/v2" "$1/v3" "$1/v4" >> "$LOG" 2>&1
}
# The device of the only zvol (udev links may be missing in-tree).
zvdev() {
	local i
	for i in $(seq 150); do
		[ -b "/dev/zvol/$1" ] && { echo "/dev/zvol/$1"; return 0; }
		[ -b /dev/zd0 ] && { echo /dev/zd0; return 0; }
		sleep 0.2
	done
	return 1
}

load() {
	CASE=load
	if ! lsmod | awk '$1 == "zfs" {f = 1} END {exit !f}'; then
		timeout 300 "$T/scripts/zfs.sh" >> "$LOG" 2>&1
	fi
	chk srcversion eq "$(cat /sys/module/zfs/srcversion)" \
	    "$(modinfo -F srcversion "$T/module/zfs.ko")"
	chk trigger test -w "$P"
	log "INFO load tree=$(git -C "$T" rev-parse HEAD) $(uname -r) mover=$(cat "$P" 2>&1)"
}

setup() {
	CASE=setup
	if [ ! -d "$W/genbuild" ]; then
		chk generator python3 "$GSRC/build.py" --tree "$T" \
		    --profile debug --output "$W/genbuild"
	fi
	cp "$W/genbuild/fixture" "$FX"
	chk kmover_user sh "$T/contrib/raidz-redirect-spike/build-tool.sh" \
	    "$T" "$T/contrib/raidz-redirect-spike/kmover_user.c" "$KU"
	load
	CASE=setup
	local rec pct u d
	for spec in 128:0:100 128:10:100 128:30:100 128:100:100 16:0:100 \
	    16:10:100 16:30:100 16:100:100 16:30:1000 16:100:1000; do
		IFS=: read -r rec pct u <<< "$spec"
		d=$W/inputs/r$rec-p$pct
		[ "$u" = 100 ] || d=$d-u$u
		[ -f "$d/objects.txt" ] && continue
		rm -rf "${d:?}"
		mkdir -p "$d"
		chk "input-${d##*/}" timeout 900 env LD_LIBRARY_PATH="$T/.libs" \
		    "$FX" create "$d" "$rec" "$pct" "$u"
	done
	(cd "$W/inputs" && sha256sum ./*/v? ./*/objects.txt) > "$W/inputs.sha256"
	sync
}

# fixture-results-20261101.txt, tree D: case mode entries entries-after-drop
# rewritten redirected kept-KiB.
EXPECT="r128-p0 hybrid 0 0 100 0 0
r128-p0 redirect 116 116 0 116 480
r128-p10 hybrid 26 0 90 26 120
r128-p10 redirect 132 106 0 132 560
r128-p30 hybrid 46 0 70 46 200
r128-p30 redirect 132 86 0 132 560
r128-p100 hybrid 130 14 0 130 552
r128-p100 redirect 130 14 0 130 552
r16-p0 hybrid 0 0 800 0 0
r16-p0 redirect 816 816 0 816 9696
r16-p10 hybrid 96 0 720 96 1040
r16-p10 redirect 832 736 0 832 9776
r16-p30 hybrid 256 0 560 256 2976
r16-p30 redirect 832 576 0 832 9776
r16-p100 hybrid 830 14 0 830 9768
r16-p100 redirect 830 14 0 830 9768"

case_a() {
	local c m e1 e2 e3 e4 e5 R U MOVE C1 C2 UM UC k got
	while read -r c m e1 e2 e3 e4 e5 <&3; do
		begin "a-$c-$m"
		R=$W/a/$c-$m
		cpin "$W/inputs/$c" "$R"
		chk import imp "$R"
		chk feature Z set feature@physical_rewrite=enabled "$POOL"
		chk epoch kmv "epoch $POOL" 120
		kmv "move $POOL $m" 900
		MOVE=$RES
		chk move_rc0 eq "$(fld "$MOVE" rc)" 0
		chk census0 eq "$(fld "$MOVE" census_left)" 0
		chk export xp
		chk verify1 fxv "$R" verify1
		chk cold_import imp "$R" ro
		kmv "census $POOL" 300
		C1=$RES
		chk cold_export xp
		chk cold_census0 eq "$(fld "$C1" census_left)" 0
		chk cold_entries eq "$(fld "$C1" entries)" "$(fld "$MOVE" entries)"
		chk cold_reached eq "$(fld "$C1" live)" "$(fld "$C1" entries)"
		chk cold_damaged0 eq "$(fld "$C1" damaged)" 0
		chk drop_retained fxdrop "$R"
		chk cold2_import imp "$R" ro
		kmv "census $POOL" 300
		C2=$RES
		chk cold2_export xp
		chk verify2 fxv "$R" verify2
		chk rw_import imp "$R"
		chk scrub0 scrubok "$R.status"
		chk rw_export xp
		chk zdb zdbok "$R" 1
		# The same input through the same code in userspace.
		U=$W/a/$c-$m-user
		cpin "$W/inputs/$c" "$U"
		UM=$(ku "$U" rw rewrite-feature epoch "move $m")
		fxdrop "$U"
		UC=$(ku "$U" ro census)
		for k in entries rewritten redirected kept census_left; do
			chk "as_user_$k" eq "$(fld "$MOVE" $k)" "$(fld "$UM" $k)"
		done
		chk as_user_after_drop eq "$(fld "$C2" entries)" "$(fld "$UC" entries)"
		got="$(fld "$MOVE" entries) $(fld "$C2" entries) $(fld "$MOVE" rewritten) $(fld "$MOVE" redirected) $(( $(fld "$MOVE" kept) / 1024 ))"
		eq "$got" "$e1 $e2 $e3 $e4 $e5" && k=SAME || k=DIFFERENT
		log "RESULT $CASE kernel: entries after_drop rewritten redirected kept_KiB = $got; 1 Nov: $e1 $e2 $e3 $e4 $e5 ($k); mover_ms=$(fld "$MOVE" mover_ms) user_mover_ms=$(fld "$UM" mover_ms)"
		end
	done 3<<< "$EXPECT"
}

b_prep() {
	cpin "$W/inputs/r16-p30-u1000" "$2" && imp "$2" &&
	    Z set feature@physical_rewrite=enabled "$POOL" &&
	    kmv "epoch $POOL" 120
}
b_dry() {
	local m R
	for m in hybrid redirect; do
		begin "b-dry-$m"
		R=$W/b/dry-$m
		chk prep b_prep "$m" "$R"
		chk move kmv "move $POOL $m" 1800
		fld "$RES" ms > "$W/b-dry-$m.ms"
		chk census0 eq "$(fld "$RES" census_left)" 0
		chk export xp
		chk verify fxv "$R" verify1
		chk zdb zdbok "$R" 1
		end
	done
}
b_cut() {
	local m=$1 pct=$2 ms delay R
	ms=$(cat "$W/b-dry-$m.ms") || { echo "run b-dry first"; return 1; }
	begin "b-cut-$m-$pct"
	R=$W/b/cut-$m-$pct
	b_prep "$m" "$R" || { log "CHECK $CASE prep FAIL"; return 1; }
	delay=$(awk -v ms="$ms" -v p="$pct" 'BEGIN {printf "%.2f", ms * p / 100000}')
	echo "$m $pct $R $delay" > "$W/b-pending"
	log "CUT $CASE: echo b > /proc/sysrq-trigger ${delay} s into the move (dry run ${ms} ms)"
	sync
	kstart "move $POOL $m"
	sleep "$delay"
	cat "$P" > /dev/kmsg 2>&1
	echo b > /proc/sysrq-trigger
}
b_check() {
	local m pct R delay C
	read -r m pct R delay < "$W/b-pending" || { echo "no pending cut"; return 1; }
	load
	begin "b-check-$m-$pct"
	chk import imp "$R"
	kmv "census $POOL" 600
	C=$RES
	chk census_after_cut eq "$(fld "$C" rc)" 0
	chk damaged0 eq "$(fld "$C" damaged)" 0
	gt0 "$(fld "$C" census_left)" && k=during || k=AFTER
	log "RESULT $CASE cut ${delay} s in: census_left=$(fld "$C" census_left) entries=$(fld "$C" entries) live=$(fld "$C" live): the cut came $k the move"
	chk export xp
	chk verify1 fxv "$R" verify1
	chk zdb1 zdbok "$R" 1
	chk import2 imp "$R"
	chk move_again kmv "move $POOL $m" 1800
	chk census0 eq "$(fld "$RES" census_left)" 0
	chk scrub0 scrubok "$R.status"
	chk export2 xp
	chk verify2 fxv "$R" verify2
	chk drop_retained fxdrop "$R"
	chk verify3 fxv "$R" verify3
	chk zdb2 zdbok "$R" 2
	mv "$W/b-pending" "$W/b-done-$m-$pct"
	end
}

case_c() {
	local R=$W/c/full M dev
	begin c-refused
	cpin "$W/inputs/r16-p100-u1000" "$R"
	chk import imp "$R"
	chk epoch kmv "epoch $POOL" 120
	chk fill_zvol F create -s -V 1G -b 128K -o compression=off "$POOL/fill"
	dev=$(zvdev "$POOL/fill")
	chk fill_dev test -b "$dev"
	timeout 900 dd if=/dev/urandom of="$dev" bs=1M count=450 \
	    conv=fsync status=none
	M=$?
	log "INFO $CASE fill rc=$M size,alloc,free=$(timeout 60 "$T/zpool" list -Hp -o size,alloc,free "$POOL" | tr '\t' ,)"
	kmv "move $POOL redirect" 900
	M=$RES
	chk refused_enospc eq "$(fld "$M" rc)" 28
	chk by_preflight eq "$(fld "$M" why)" refused_by_preflight
	kmv "census $POOL" 300
	chk nothing_moved eq "$(fld "$RES" entries)" 0
	chk export xp
	chk verify1 fxv "$R" verify1
	chk zdb1 zdbok "$R" 1
	end
	begin c-forced
	chk import imp "$R"
	kmv "move $POOL redirect force" 2400
	M=$RES
	chk rc_eagain eq "$(fld "$M" rc)" 11
	chk only_enospc eq "$(fld "$M" errs | sed 's/:[0-9]*//g')" 28
	chk partial gt0 "$(fld "$M" census_left)"
	chk not_suspended eq "$(health)" ONLINE
	chk export xp
	chk verify2 fxv "$R" verify2
	chk zdb2 zdbok "$R" 2
	end
	begin c-freed
	chk import imp "$R"
	chk destroy_fill F destroy -r "$POOL/fill"
	chk move kmv "move $POOL redirect" 1800
	chk census0 eq "$(fld "$RES" census_left)" 0
	chk scrub0 scrubok "$R.status"
	chk export xp
	chk verify3 fxv "$R" verify3
	chk zdb3 zdbok "$R" 3
	end
}

case_d() {
	local v R MNT SRC=$W/d/src g0 g1 i o M POOL=khpool
	local -a head=() snap=()
	mkdir -p "$SRC"
	for i in 0 1 2 3 4 5 6 7; do
		urandom "$SRC/a$i" 128k 8
		snap+=("f$i=$SRC/a$i")
		[ "$i" -lt 4 ] && head+=("f$i=$SRC/b$i") || head+=("f$i=$SRC/a$i")
	done
	for i in 0 1 2 3; do urandom "$SRC/b$i" 128k 8; done
	for v in gang nogang; do
		begin "d-$v"
		R=$W/d/$v
		MNT=$W/d/mnt-$v
		chk create_pool kpool "$R"
		chk create F create -o copies=2 -o compression=off \
		    -o recordsize=128k -o mountpoint="$MNT" "$POOL/g"
		g0=$(cat "$MP/metaslab_force_ganging")
		g1=$(cat "$MP/metaslab_force_ganging_pct")
		echo 32769 > "$MP/metaslab_force_ganging"
		echo 100 > "$MP/metaslab_force_ganging_pct"
		for i in 0 1 2 3 4 5 6 7; do
			dd if="$SRC/a$i" of="$MNT/f$i" bs=128k conv=fsync status=none
		done
		Z sync "$POOL"
		chk snapshot F snapshot "$POOL/g@s"
		for i in 0 1 2 3; do
			dd if="$SRC/b$i" of="$MNT/f$i" bs=128k conv=fsync,notrunc status=none
		done
		Z sync "$POOL"
		echo "$g0" > "$MP/metaslab_force_ganging"
		echo "$g1" > "$MP/metaslab_force_ganging_pct"
		chk unmount F unmount "$POOL/g"
		chk export0 xp
		timeout 900 "$T/zdb" -e -p "$R" -bb "$POOL" > "$R.zdb0.log" 2>&1
		chk gang_blocks gt0 "$(sed -n 's/^.*ganged count: *//p' "$R.zdb0.log" | head -n 1)"
		chk import2 imp "$R"
		chk epoch kmv "epoch $POOL" 120
		o=""
		[ "$v" = nogang ] && o=" nogang"
		kmv "move $POOL hybrid head=$POOL/g$o" 1800
		M=$RES
		chk gang_found gt0 "$(fld "$M" gang_dvas)"
		if [ "$v" = gang ]; then
			chk move_rc0 eq "$(fld "$M" rc)" 0
			chk census0 eq "$(fld "$M" census_left)" 0
		else
			chk move_rc_eagain eq "$(fld "$M" rc)" 11
			chk gang_left eq "$(fld "$M" census_left)" "$(fld "$M" gang_left)"
			chk gang_left_gt0 gt0 "$(fld "$M" gang_left)"
		fi
		chk mount F mount "$POOL/g"
		chk data_head same "$MNT" "${head[@]}"
		chk clone F clone -o mountpoint="$MNT-s" "$POOL/g@s" "$POOL/gs"
		chk data_snapshot same "$MNT-s" "${snap[@]}"
		chk clone_destroy F destroy "$POOL/gs"
		chk scrub0 scrubok "$R.status"
		chk export xp
		chk zdb zdbok "$R" 1
		end
	done
}

case_e() {
	local R=$W/e/run MNT=$W/e/mnt SRC=$W/e/src KEY=$W/e/key i M POOL=khpool
	local -a acked=() base=("base0=$SRC/base0b")
	begin e-keyless-zil
	mkdir -p "$SRC"
	urandom "$KEY" 32 1
	urandom "$SRC/base0b" 128k 8
	for i in 0 1 2 3; do
		urandom "$SRC/base$i" 128k 8
		urandom "$SRC/big$i" 128k 4
		acked+=("big$i=$SRC/big$i")
		[ "$i" = 0 ] || base+=("base$i=$SRC/base$i")
	done
	for i in 0 1 2 3 4 5 6 7; do
		urandom "$SRC/small$i" 4k 4
		acked+=("small$i=$SRC/small$i")
	done
	chk create_pool kpool "$R"
	chk create F create -o encryption=aes-256-gcm -o keyformat=raw \
	    -o keylocation="file://$KEY" -o compression=off \
	    -o mountpoint="$MNT" "$POOL/enc"
	for i in 0 1 2 3; do
		dd if="$SRC/base$i" of="$MNT/base$i" bs=128k status=none
	done
	Z sync "$POOL"
	chk snapshot F snapshot "$POOL/enc@s1"
	dd if="$SRC/base0b" of="$MNT/base0" bs=128k conv=notrunc status=none
	dd if=/dev/zero of="$MNT/sync" bs=1 count=1 conv=fdatasync,fsync status=none
	Z sync "$POOL"
	chk freeze Z freeze "$POOL"
	for i in 0 1 2 3 4 5 6 7; do
		dd if="$SRC/small$i" of="$MNT/small$i" bs=4k conv=fsync status=none
	done
	for i in 0 1 2 3; do
		dd if="$SRC/big$i" of="$MNT/big$i" bs=128k conv=fsync status=none
	done
	chk unmount_frozen F unmount "$POOL/enc"
	chk export_frozen xp
	timeout 600 "$T/zdb" -e -p "$R" -ivv "$POOL/enc" > "$R.zil.log" 2>&1
	log "INFO $CASE zdb -ivv: $(grep -cE 'TX_WRITE' "$R.zil.log") TX_WRITE lines"
	chk import_nokey imp "$R"
	chk keyless eq "$(timeout 60 "$T/zfs" get -H -o value keystatus "$POOL/enc")" unavailable
	chk epoch kmv "epoch $POOL" 120
	kmv "census $POOL" 300
	chk zil_old_before gt0 "$(fld "$RES" zil_old)"
	kmv "move $POOL hybrid" 1800
	M=$RES
	chk move_rc0 eq "$(fld "$M" rc)" 0
	chk census0 eq "$(fld "$M" census_left)" 0
	chk zil_kept gt0 "$(fldl "$M" zil_old)"
	chk export xp
	chk cold_import imp "$R"
	kmv "census $POOL" 300
	chk cold_census0 eq "$(fld "$RES" census_left)" 0
	chk cold_damaged0 eq "$(fld "$RES" damaged)" 0
	chk load_key F load-key "$POOL/enc"
	chk mount_replays F mount "$POOL/enc"
	chk acked_writes same "$MNT" "${acked[@]}"
	chk base_files same "$MNT" "${base[@]}"
	chk clone F clone -o mountpoint="$MNT-s" "$POOL/enc@s1" "$POOL/encs"
	chk snapshot_files same "$MNT-s" "base0=$SRC/base0"
	chk clone_destroy F destroy "$POOL/encs"
	kmv "census $POOL" 300
	chk zil_replayed eq "$(fld "$RES" zil_old)" 0
	chk census0_after eq "$(fld "$RES" census_left)" 0
	chk scrub0 scrubok "$R.status"
	chk export2 xp
	chk zdb zdbok "$R" 1
	end
}

case_f() {
	local R=$W/f/run s M
	begin f-offline
	cpin "$W/inputs/r16-p30-u1000" "$R"
	chk import imp "$R"
	chk feature Z set feature@physical_rewrite=enabled "$POOL"
	chk epoch kmv "epoch $POOL" 120
	kstart "move $POOL redirect"
	sleep 3
	s=$(cat "$P")
	chk offline_during_move eq "${s%% *}" running
	chk offline Z offline "$POOL" "$R/v2"
	kwait 1800
	M=$RES
	chk move_rc0 eq "$(fld "$M" rc)" 0
	chk census0 eq "$(fld "$M" census_left)" 0
	chk degraded eq "$(health)" DEGRADED
	chk online Z online "$POOL" "$R/v2"
	chk resilver timeout 900 "$T/zpool" wait -t resilver "$POOL"
	timeout 60 "$T/zpool" status -v "$POOL" > "$R.status.resilver" 2>&1
	chk healthy eq "$(health)" ONLINE
	chk scrub0 scrubok "$R.status"
	kmv "census $POOL" 300
	chk census0_after eq "$(fld "$RES" census_left)" 0
	chk export xp
	chk verify1 fxv "$R" verify1
	chk zdb1 zdbok "$R" 1
	end
	begin f-replace
	chk import imp "$R"
	truncate -s 256M "$R/v5"
	chk replace Z replace "$POOL" "$R/v3" "$R/v5"
	chk resilver timeout 900 "$T/zpool" wait -t resilver "$POOL"
	chk scrub0 scrubok "$R.status2"
	chk export xp
	mv "$R/v3" "$W/f/v3.replaced"
	chk verify2 fxv "$R" verify2
	chk zdb2 zdbok "$R" 2
	end
}

summary() {
	grep -E '^(CHECK .* FAIL|RESULT|INFO|CUT|MOVER .*TIMEOUT)' "$LOG"
	echo "checks: $(grep -c '^CHECK .* PASS' "$LOG") PASS," \
	    "$(grep -c '^CHECK .* FAIL' "$LOG") FAIL"
}

case "${1:-}" in
setup) setup ;;
load) load ;;
a) load; case_a ;;
b-dry) load; b_dry ;;
b-cut) load; b_cut "${2:?mode}" "${3:?pct}" ;;
b-check) b_check ;;
c) load; case_c ;;
d) load; case_d ;;
e) load; case_e ;;
f) load; case_f ;;
summary) summary; exit 0 ;;
*) sed -n '2,15p' "$0"; exit 2 ;;
esac
log "DONE ${1} $(date -Is): $FAILS checks failed"
[ "$FAILS" = 0 ]
