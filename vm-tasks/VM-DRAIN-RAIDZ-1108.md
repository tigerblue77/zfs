# VM task: drain and remove a RAID-Z top-level vdev, and mirror and disk vdevs, with the rewrite series (8/11)

These instructions are for one 16-vCPU test VM: VM 105, VM 106 or VM 107,
whichever is free, as long as it runs Debian 13 with a 6.12 kernel (STEP 0
checks). Reply with `/var/log/drain-raidz-1108/REPORT.txt` (STEP 6). Nothing
is posted, pushed or installed.

**A functional test.** It checks what happens, not how fast: it may run while
the other VMs run functional tests (ztest, zloop, ZTS, replays), but never
while one of them runs a performance test, since the three VMs share one
hypervisor and this task's debug build and pool I/O would skew a measurement
there. Nothing else runs on this VM meanwhile.

**Background.** The fork branch `rewrite-series-v3-rb1108`, head
`32333813acab5a901c3b43e4f32ef4179300ffca` ("ztest: drain a special or dedup
vdev under load"), is upstream master `2796b5da69` ("zfs_znode (Linux):
Optimize hold insertion") with #19373, #19367 and #19388 merged (merge
`59b712ebb2`) and 14 commits on top. Together they empty a top-level vdev and
take it out of the pool without an indirect vdev:

1. `zpool set allocating=off <pool> <vdev>` (an existing vdev property) stops
   new allocations on the vdev.
2. `zpool rewrite -A <pool>` (new) rewrites, in every file system and volume
   and in the pool's own metadata (the MOS: space maps, deadlists, the
   configuration, the history...), every block that has a copy on a vdev
   whose `allocating` is `off`, so that it moves to vdevs that allocate.
   Snapshots keep their blocks where they are; `-v` names the datasets whose
   snapshots do.
3. `zpool remove <pool> <vdev>` (#19373): a top-level vdev with no allocated
   space, of any type and in any pool, is replaced by a hole at once: nothing
   is copied, no indirect vdev is created, `device_removal` does not become
   active, and the labels of its devices are rewritten with txg 0, which an
   import ignores. A vdev that still holds data takes the old path, which
   refuses any vdev of a pool that has a RAID-Z top-level vdev ("invalid
   config; all top-level vdevs must have the same sector size and not be
   raidz."), and evacuates it to an indirect vdev in a pool that has none.

So far the whole sequence has run end to end only on a special mirror: the
series' ZTS tests `zpool_rewrite_drain` and `zpool_rewrite_snapshot`, and one
VM run. The tests of the hole path remove vdevs that never held data
(`remove_empty_vdev`, `remove_raidz_empty`). Nothing has yet drained a
normal-class vdev that held data and removed it, nor the pool's first vdev,
nor a RAID-Z vdev. An earlier measurement on 15/10, with `zfs rewrite` alone
(no pool metadata step yet), moved all live data off a raidz1 vdev but left
22 metadata blocks on it once the snapshots were gone. If the series now
empties and removes a RAID-Z vdev as is, that is a way to remove a RAID-Z
top-level vdev (openzfs/zfs#9013) whenever the rest of the pool can hold its
data and no snapshot holds blocks on it. This task finds out whether it works
and what breaks.

**What runs.** One debug build of the head, loaded with `insmod`. Four cases,
one after the other, each on its own pool of sparse 1 GiB files under
`/var/tmp` (no block device, no RAM disk), destroyed at the end of the case.
`drain.sh` runs each case: every step prints what it checks, `ok` or `STOP`,
and the first unexpected result stops everything and leaves the pool imported
for diagnosis.

| Case | Pool (ashift 12, lz4) | Steps | Expected |
| --- | --- | --- | --- |
| 1 | `dr1`: `raidz1-0`, `raidz1-1`, 3 files each | fill; recursive snapshots `@s1` and `@s2` with changes between them, both destroyed; `allocating=off` on `raidz1-0`, the pool's first vdev; `zpool rewrite -A -v`; wait until nothing is allocated on it; zdb census of the blocks with a copy on vdev 0; `zpool remove`; scrub; export and import; `zdb -bcc`; then a new raidz1 vdev is added and written | `raidz1-0` empties and becomes a hole (id 0), the labels on its files have txg 0, `device_removal` and `obsolete_counts` stay `enabled`, contents identical throughout, scrub and zdb clean; the new vdev is `raidz1-2` (a new vdev is appended, holes are not reused) and allocates |
| 2 | `dr2`: as case 1 | fill; recursive snapshot `@keep`; then four 64 MiB files rewritten, 1000 small files deleted and 32 MiB of the volume rewritten, so that `@keep` holds blocks; `allocating=off` on `raidz1-1`; `zpool rewrite -A -v` twice; `zpool remove raidz1-1`; `@keep` destroyed; then as case 1 | the rewrite reports `1 snapshots keep their blocks` for the 5 datasets; `raidz1-1` keeps only blocks of `@keep` (census: none outside snapshots); the second rewrite changes nothing; the remove is refused with the raidz message above and changes nothing; once `@keep` is gone, as case 1, the hole being id 1 |
| 3 | `dr3`: `mirror-0`, `mirror-1` (2 files each) and one plain file vdev, id 2 | smaller fill; snapshots taken and destroyed; `mirror-0` drained and removed; then the plain vdev drained and removed | holes 0 and 2, `mirror-1` alone holds everything, contents identical |
| 4 | `dr4`: as case 1 | fill, then 64 MiB files until the pool is 60 % full; `allocating=off` on `raidz1-0`; `zpool rewrite -A -v`; `allocating=on` again | the rewrite fails with "No space left on device", nothing is lost, the pool stays healthy, and writes work again once `allocating` is back `on` |

**The fill.** `<pool>/fs` (recordsize 128K): 3000 small files of 1 to 64 KiB,
half random and half compressible; a directory of 2000 entries; 200 files of
4 KiB each with a 1500-byte extended attribute, which `xattr=sa` with legacy
dnodes puts in a spill block; a 1 GiB sparse file with 8 MiB written.
`<pool>/big` (recordsize 1M): twelve 64 MiB random files, two of them cloned
with `cp --reflink=always`. `<pool>/enc` (aes-256-gcm, raw key in a file):
thirty-two 2 MiB files. `<pool>/vol`: a 128 MiB volume (volblocksize 16K),
written whole. About 1.1 GiB in all, about 30 % of a two-raidz1 pool. Case 3
has four 64 MiB files, 1500 small files and a 64 MiB volume.

**Why 30 % and not 60 %.** The work item says "fill to about 60 %". Two
raidz1 vdevs 60 % full cannot be drained into one: the remaining vdev would
need 120 % of its size. Cases 1 and 2 therefore fill about 30 % of the pool,
which leaves the remaining vdev about 60 % full after the drain (a little
more, since the rewrite gives the cloned files their own blocks). Case 4
keeps the 60 % to show what someone who tries it gets: `allocating=off` takes
the whole size of the vdev out of the space the datasets may use
(`spa_update_dspace()`), so at 60 % their available space drops to 0 (from
reading, not run).

**Duration**: STEP 0 2 min; STEP 1 about 8 min (one debug build with
`make -j8`); STEP 2 1 min; STEP 3 about 40 min (case 1 about 12 min, case 2
about 12, case 3 about 8, case 4 about 8); STEPS 5 and 6 about 5 min. About
an hour in all.

## Rules

- Disposable VM only, with nothing else running on it. This task loads and
  unloads the ZFS module stack it builds, and creates the pools `dr1` to
  `dr4`, one at a time, on sparse files under W only: no block device, no
  other pool.
- One debug build (`--enable-debug`), under `nice -n 10 ionice -c3` with
  `make -j8`.
- Post nothing, push nothing. Install nothing (no `make install`, no
  `zfs-tests.sh`, no `zfs-helpers.sh`, no package). Never reboot.
- Work only under `/var/tmp/drain-raidz-1108` (W) and
  `/var/log/drain-raidz-1108` (L). Never run `zpool`, `zfs` or `zdb` while
  this task's modules are not loaded.
- Run each block as your usual user in bash; the blocks call `sudo -n`
  themselves where root is needed, and `drain.sh` runs as root.
- If a check does not print what its comment says, stop and report; do not
  rebuild, repair or retry. A `STOP` line of `drain.sh`, a panic, oops,
  `VERIFY`, `ASSERT`, `BUG:`, `WARNING` or hung-task message: stop, run STEP 4,
  write the REPORT (STEP 6) and reply, and leave everything as it is: the pool
  imported, the modules loaded, W in place, for diagnosis. Run STEP 5 (the
  cleanup) only when the reply asks for it.
- If `drain.sh` prints nothing for 15 minutes, treat it as a STOP: STEP 4
  records the kernel log and the stuck processes. Do not kill a process in
  state `D`.
- If your shell tool cannot wait for a long command in the foreground, start
  that one command with the tool's own background mode and wait until it
  exits; do not use `&` or `nohup`, and run nothing else meanwhile.

## STEP 0: the VM must have the right shape and be free

```sh
{
date -u '+%F %T'; hostname; id -un; sudo -n true && echo "sudo ok"
uname -r; nproc; . /etc/os-release; echo "$PRETTY_NAME"
grep -E 'MemTotal|MemAvailable' /proc/meminfo
ls -d /lib/modules/$(uname -r)/build
for t in git make gcc autoconf automake libtool python3 udevadm sha256sum truncate awk; do command -v $t > /dev/null || echo "missing $t"; done
cp --help | grep -c -- '--reflink'                            # 1 or more
grep -E '^(zfs|spl) ' /proc/modules                           # nothing
ls /lib/modules/$(uname -r)/extra 2>&1 | grep -E 'zfs|spl'    # nothing (no installed ZFS)
command -v zpool zfs zdb zed                                  # nothing
pgrep -af 'zfs-tests|test-runner|ztest|zloop|configure|make|cc1|fio|bench' | grep -v pgrep  # nothing
ls -d /var/tmp/drain-raidz-1108 /var/log/drain-raidz-1108 2>&1  # No such file or directory, twice
df -h /var/tmp | tail -1                                      # 12G free or more
} 2>&1 | tee /var/tmp/drain-raidz-1108-step0.txt
```

Stop, changing nothing, if: `sudo -n true` fails; `uname -r` does not start
with `6.12.`; `nproc` is not 16; the OS is not Debian 13; MemAvailable is
under 8 GiB; the kernel headers or a tool are missing; `cp` has no
`--reflink`; a zfs or spl module is loaded; a ZFS is installed; a build, test
or benchmark runs; W or L exists; less than 12G is free on `/var/tmp` (the
build takes about 2G; each pool's sparse files take a few GiB, 9 GiB at
most, and are removed when the case passes).

## STEP 1: sources and debug build (about 8 min)

```sh
export PATH=/usr/sbin:/sbin:$PATH
W=/var/tmp/drain-raidz-1108 L=/var/log/drain-raidz-1108
sudo -n mkdir -p $L && sudo -n chown "$(id -un):" $L && mkdir -p $W || exit 1
mv /var/tmp/drain-raidz-1108-step0.txt $L/step0.txt
cat > $L/env.sh <<'EOF2'
export PATH=/usr/sbin:/sbin:/usr/bin:/bin
W=/var/tmp/drain-raidz-1108 L=/var/log/drain-raidz-1108
Z=/var/tmp/drain-raidz-1108/S DEV=1G
EOF2
. $L/env.sh
{
git init -q $W/repo && git -C $W/repo fetch -q --depth 1 https://github.com/tigerblue77/zfs 32333813acab5a901c3b43e4f32ef4179300ffca &&
git -C $W/repo worktree add -q --detach $Z 32333813acab5a901c3b43e4f32ef4179300ffca || echo "fetch failed: stop"
git -C $Z log -1 --format='%H %s'
grep -c 'rewrite \[-Av\] <pool>' $Z/cmd/zpool/zpool_main.c
grep -c '^spa_vdev_remove_empty(vdev_t' $Z/module/zfs/vdev_removal.c
(cd $Z && t0=$(date +%s) &&
 nice -n 10 ionice -c3 sh -c './autogen.sh && ./configure --enable-debug && make -j8' > $L/build.log 2>&1
 echo "build rc=$? wall=$(( $(date +%s) - t0 ))s")
grep -c -- '--enable-debug' $Z/config.log | xargs echo "config.log lines with --enable-debug:"
modinfo -F vermagic $Z/module/zfs.ko | cut -d' ' -f1 | xargs echo "built for:"
for m in spl zfs; do modinfo -F srcversion $Z/module/$m.ko | xargs echo "$m.ko srcversion:"; done
ls $Z/zpool $Z/zfs $Z/zdb $Z/udev/zvol_id $Z/scripts/zfs.sh
} 2>&1 | tee $L/step1.txt
```

Expected: `32333813acab5a901c3b43e4f32ef4179300ffca ztest: drain a special or
dedup vdev under load`; `1` twice (the series' `zpool rewrite` and the hole
path of #19373 are there); `build rc=0`; 1 or more lines with
`--enable-debug`; built for `uname -r`; two srcversions; the five paths
listed. On a failed build quote `grep -nE 'error:|Error [0-9]' $L/build.log |
head -20` and stop.

## STEP 2: the scripts and the modules (1 min)

`census.awk` reads the output of `zdb -bbbbb` (one line per block pointer,
with its DVAs `<vdev:offset:asize>`) and counts the blocks that have a copy on
one top-level vdev, grouped by dataset (names from `zdb -d`) and object type,
and how many of them are outside snapshots; it also counts the deferred frees
on the vdev. `drain.sh` is the four cases. Then the modules are loaded.

```sh
. /var/log/drain-raidz-1108/env.sh
cat > $L/census.awk <<'EOF2'
# census.awk: count, in zdb -bbbbb output, the blocks with a copy on top-level
# vdev id, grouped by dataset and type; names come from zdb -d output.
function hex(s,    i, v) {
	v = 0
	for (i = 1; i <= length(s); i++)
		v = v * 16 + index("0123456789abcdef", substr(s, i, 1)) - 1
	return v
}
BEGIN {
	while ((getline l < names) > 0) {
		if (l !~ /^Dataset .*, ID [0-9]+,/)
			continue
		nm = l; sub(/^Dataset /, "", nm); sub(/ \[.*/, "", nm)
		i = l; sub(/.*, ID /, "", i); sub(/,.*/, "", i)
		ds[i] = nm
	}
}
/DVA\[[0-9]\]=</ {
	s = $0; on = 0; asz = 0
	while (match(s, /DVA\[[0-9]\]=<[0-9]+:[0-9a-f]+:[0-9a-f]+>/)) {
		d = substr(s, RSTART, RLENGTH)
		s = substr(s, RSTART + RLENGTH)
		split(d, p, /[<:>]/)
		if (p[2] == id) { on = 1; asz += hex(p[4]) }
	}
	if ($1 == "[deferred") { if (on) nd++; next }
	if ($1 != "objset") next
	nt++
	if (!on) next
	o = ($2 in ds) ? ds[$2] : "objset " $2
	t = $0
	if (sub(/.*\[L[0-9]+ /, "", t)) sub(/\] .*/, "", t); else t = "?"
	k = o " | " t; n[k]++; b[k] += asz; nb++; bb += asz
	if (o !~ /@/) nl++
}
END {
	printf "%d %d %d\n", nb, nl, nd > num
	printf "%s: %d of %d blocks have a copy on vdev %s (%d bytes there), " \
	    "%d of them outside snapshots; %d deferred frees on it\n",
	    tag, nb, nt, id, bb, nl, nd
	for (k in n)
		printf "%9d blocks %13d bytes  %s\n", n[k], b[k], k
}
EOF2
cat > $L/drain.sh <<'EOF2'
#!/bin/bash
# drain.sh <1|2|3|4>: one case of VM-DRAIN-RAIDZ-1108, run as root.  Prints
# one line per check, "ok" or "STOP", and exits 1 at the first unexpected
# result, leaving the pool imported and its files in place for diagnosis.
. /var/log/drain-raidz-1108/env.sh
set -u
[ "$(id -u)" = 0 ] || { echo "drain.sh: run as root"; exit 2; }
C=${1:-}
case $C in 1 | 2 | 3 | 4) ;; *) echo "usage: drain.sh 1|2|3|4"; exit 2 ;; esac
P=dr$C D=$W/case$C M=$W/mnt$C CF=$W/case$C.cache O=$L/case$C
ZP=$Z/zpool ZF=$Z/zfs ZDB=$Z/zdb
mkdir -p "$O" || exit 1

log() { echo "[$(date -u +%T)] case $C: $*" | tee -a "$O/log.txt"; }
note() { log "note $*"; }
stop() {
	log "STOP: $*"
	echo "case $C: $*" >> "$L/stops.txt"
	exit 1
}
# ck <what> <expected> <actual>
ck() {
	if [ "$2" = "$3" ]; then
		log "ok   $1: $3"
	else
		stop "$1: expected '$2', got '$3'"
	fi
}
# run <what> <command...>: the command must exit 0; its output goes to the log
run() {
	local what=$1 out rc
	shift
	out=$("$@" 2>&1)
	rc=$?
	[ -n "$out" ] && printf '%s\n' "$out" >> "$O/log.txt"
	[ $rc -eq 0 ] ||
	    stop "$what: exit $rc: $(printf '%s' "$out" | tail -3 | tr '\n' ' ')"
	log "ok   $what"
}
# busy_run <what> <command...>: as run, but tried again while the pool is busy
busy_run() {
	local what=$1 out i
	shift
	udevadm settle
	for ((i = 0; i < 5; i++)); do
		out=$("$@" 2>&1) && { log "ok   $what"; return 0; }
		printf '%s\n' "$out" >> "$O/log.txt"
		printf '%s' "$out" | grep -q busy || break
		sleep 2
	done
	stop "$what: $(printf '%s' "$out" | tail -3 | tr '\n' ' ')"
}
yn() { if "$@"; then echo yes; else echo no; fi; }
alloc() { $ZP get -Hpo value allocated $P "$1"; }
prop() { $ZP get -Hpo value "$1" $P; }

# Kernel messages since the start of the case.
kcheck() {
	local k
	k=$(dmesg | sed -n "/drain-raidz-1108: case $C start/,\$p" |
	    grep -E 'VERIFY|ASSERT|PANIC|BUG:|Oops|Call Trace|blocked for more|WARNING:|general protection')
	if [ -n "$k" ]; then
		printf '%s\n' "$k" >> "$O/log.txt"
		stop "kernel message: $(printf '%s' "$k" | head -1)"
	fi
	log "ok   no kernel message since the start of the case"
}

# wait_empty <vdev> <seconds>: succeed once nothing is allocated on the vdev.
wait_empty() {
	local i
	for ((i = 0; i < $2; i++)); do
		$ZP sync $P
		[ "$(alloc "$1")" = 0 ] && return 0
		sleep 1
	done
	return 1
}

# The block device of $P/vol, once udev has created it.
zvdev() {
	local i d
	for ((i = 0; i < 30; i++)); do
		udevadm settle
		for d in /dev/zd*; do
			[ -b "$d" ] &&
			    [ "$("$Z/udev/zvol_id" "$d" 2>/dev/null)" = "$P/vol" ] &&
			    { echo "$d"; return 0; }
		done
		sleep 1
	done
	return 1
}

# content <file>: the sha256 of every file under $M, and of the volume.
content() {
	local v
	v=$(zvdev) || stop "no device for $P/vol"
	(cd "$M" && find . -type f -print0 | sort -z | xargs -0 sha256sum) \
	    > "$1" || stop "checksums of the files"
	echo "$(sha256sum < "$v" | cut -c1-64)  volume" >> "$1"
	log "checksums of $(($(wc -l < "$1") - 1)) files and the volume"
}
samecontent() { # tag
	content "$O/c-$1.txt"
	ck "contents ($1) identical to the start" yes \
	    "$(yn cmp -s "$O/c-0.txt" "$O/c-$1.txt")"
}

# census <vdev id> <tag>: the blocks that zdb finds with a copy on the vdev,
# grouped by dataset and type.  Sets NB (blocks), NL (blocks outside
# snapshots) and ND (deferred frees on it).
census() {
	local rc
	$ZP sync $P
	$ZDB -U "$CF" -d $P > "$O/zdbd-$2.txt" 2>&1
	$ZDB -U "$CF" -bbbbb $P > "$O/zdbb-$2.txt" 2>&1
	rc=$?
	note "zdb -bbbbb ($2) exit $rc, $(grep -c '^objset' "$O/zdbb-$2.txt") block lines, $(grep -cE 'No leaks|leaked' "$O/zdbb-$2.txt") leak lines"
	awk -v id="$1" -v tag="census $2" -v names="$O/zdbd-$2.txt" \
	    -v num="$O/census-$2.num" -f "$L/census.awk" "$O/zdbb-$2.txt" |
	    { IFS= read -r h; echo "$h"; sort -k1,1nr | head -25; } |
	    tee "$O/census-$2.txt" | tee -a "$O/log.txt"
	read -r NB NL ND < "$O/census-$2.num" || stop "census $2"
}

# holes <zdb options...>: the ids of the holes in the MOS configuration.
holes() {
	$ZDB "$@" -C $P 2>/dev/null | awk '/^MOS Configuration:/ { m = 1 }
	    m && h && /^ *id: / { printf "%s%s", s, $2; s = " " }
	    { h = 0 } m && /type: .hole./ { h = 1 }'
}

level() { # tag: the space of the pool and of its vdevs
	$ZP list -v $P > "$O/list-$1.txt" 2>&1
	cat "$O/list-$1.txt" >> "$O/log.txt"
	note "($1) size $(prop size) allocated $(prop allocated)" \
	    "capacity $(prop capacity)% bcloneused $(prop bcloneused)" \
	    "available $($ZF get -Hpo value available $P)"
}

mkpool() { # raidz | mirror
	$ZP list $P > /dev/null 2>&1 && stop "pool $P exists already"
	rm -rf "$D" "$M"
	mkdir -p "$D" "$M" || stop "mkdir"
	echo "drain-raidz-1108: case $C start" > /dev/kmsg
	if [ "$1" = raidz ]; then
		truncate -s "$DEV" "$D/a0" "$D/a1" "$D/a2" "$D/b0" "$D/b1" \
		    "$D/b2" || stop "truncate"
		run "zpool create $P, two raidz1 vdevs" $ZP create -f \
		    -o ashift=12 -o cachefile="$CF" -O mountpoint="$M" \
		    -O compression=lz4 -O xattr=sa -O atime=off $P \
		    raidz1 "$D/a0" "$D/a1" "$D/a2" raidz1 "$D/b0" "$D/b1" "$D/b2"
	else
		truncate -s "$DEV" "$D/m0" "$D/m1" "$D/n0" "$D/n1" "$D/d0" ||
		    stop "truncate"
		run "zpool create $P, two mirrors" $ZP create -f \
		    -o ashift=12 -o cachefile="$CF" -O mountpoint="$M" \
		    -O compression=lz4 -O xattr=sa -O atime=off $P \
		    mirror "$D/m0" "$D/m1" mirror "$D/n0" "$D/n1"
		# A separate add: listed after a mirror, d0 would join it.
		run "zpool add $P, a plain disk" $ZP add -f $P "$D/d0"
	fi
	$ZP status $P >> "$O/log.txt"
	$ZP events -c > /dev/null
}

# fill <64 MiB files> <volume MiB> <small files>
fill() {
	local nl=$1 vm=$2 ns=$3 i v
	run "create $P/fs" $ZF create -o recordsize=128k $P/fs
	run "create $P/big" $ZF create -o recordsize=1m $P/big
	head -c 32 /dev/urandom > "$W/case$C.key"
	run "create $P/enc" $ZF create -o encryption=aes-256-gcm \
	    -o keyformat=raw -o keylocation="file://$W/case$C.key" $P/enc
	mkdir -p "$M/fs/small" "$M/fs/fat" "$M/fs/xattr" || stop "mkdir"
	for ((i = 0; i < ns; i++)); do
		if ((i % 2)); then
			yes "case $C file $i, a line that compresses well" |
			    head -c $(((i * 7919 % 64 + 1) * 1024)) \
			    > "$M/fs/small/$i"
		else
			head -c $(((i * 7919 % 64 + 1) * 1024)) /dev/urandom \
			    > "$M/fs/small/$i"
		fi || stop "write $M/fs/small/$i"
	done
	for ((i = 0; i < 2000; i++)); do
		: > "$M/fs/fat/a-long-name-to-grow-the-directory-zap-$i" ||
		    stop "create entry $i of $M/fs/fat"
	done
	python3 -I -c '
import os, sys
for i in range(200):
    p = os.path.join(sys.argv[1], str(i))
    with open(p, "wb") as f:
        f.write(os.urandom(4096))
    os.setxattr(p, "user.big", os.urandom(1500))
' "$M/fs/xattr" || stop "xattrs"
	truncate -s 1G "$M/fs/sparse" || stop "sparse file"
	for i in 1 100 300 500 700 900 1000 1023; do
		dd if=/dev/urandom of="$M/fs/sparse" bs=1M count=1 seek=$i \
		    conv=notrunc status=none || stop "sparse file write"
	done
	for ((i = 0; i < nl; i++)); do
		head -c 64M /dev/urandom > "$M/big/L$i" || stop "write L$i"
	done
	for ((i = 0; i < 32; i++)); do
		head -c 2M /dev/urandom > "$M/enc/e$i" || stop "write e$i"
	done
	# A file with blocks still dirty in the open txg is not cloned.
	$ZP sync $P
	if cp --reflink=always "$M/big/L0" "$M/big/C0" &&
	    cp --reflink=always "$M/big/L1" "$M/big/C1"; then
		$ZP sync $P
		note "L0 and L1 cloned, bcloneused $(prop bcloneused)"
	else
		note "cp --reflink=always failed: no cloned blocks in this case"
	fi
	run "create $P/vol" $ZF create -V "${vm}M" -o volblocksize=16k $P/vol
	v=$(zvdev) || stop "no device for $P/vol"
	dd if=/dev/urandom of="$v" bs=1M count="$vm" oflag=direct conv=fsync \
	    status=none || stop "write $v"
	run "zpool sync" $ZP sync $P
}

# churn <small files>: snapshots taken with changes between them, destroyed.
churn() {
	local ns=$1 i v
	v=$(zvdev) || stop "no device for $P/vol"
	run "snapshot $P@s1" $ZF snapshot -r $P@s1
	for i in 2 3; do
		dd if=/dev/urandom of="$M/big/L$i" bs=1M count=16 \
		    seek=$((i * 8)) conv=notrunc status=none || stop "rewrite L$i"
	done
	for ((i = 0; i < ns / 3; i++)); do
		rm -f "$M/fs/small/$i"
	done
	for ((i = ns; i < ns + ns / 6; i++)); do
		head -c 8K /dev/urandom > "$M/fs/small/$i" || stop "write $i"
	done
	dd if=/dev/urandom of="$v" bs=1M count=16 seek=16 oflag=direct \
	    conv=fsync status=none || stop "rewrite $v"
	run "snapshot $P@s2" $ZF snapshot -r $P@s2
	dd if=/dev/urandom of="$M/big/L2" bs=1M count=8 seek=40 conv=notrunc \
	    status=none || stop "rewrite L2"
	run "destroy $P@s1" $ZF destroy -r $P@s1
	run "destroy $P@s2" $ZF destroy -r $P@s2
	run "zpool wait -t free" $ZP wait -t free $P
	ck "snapshots left" 0 "$($ZF list -H -t snapshot -r $P | wc -l)"
}

# rewrite <tag>: zpool rewrite -A -v of the pool; sets RC.
rewrite() {
	log "zpool rewrite -A -v $P ($1)"
	$ZP rewrite -A -v $P > "$O/rewrite-$1.txt" 2>&1
	RC=$?
	cat "$O/rewrite-$1.txt" >> "$O/log.txt"
}
rewrite_done() { # tag: every dataset and the pool metadata done
	local ds
	ck "zpool rewrite -A -v ($1) exit status" 0 "$RC"
	for ds in $P $P/fs $P/big $P/enc $P/vol; do
		ck "$ds rewritten ($1)" 1 "$(grep -c "^$ds: done" "$O/rewrite-$1.txt")"
	done
	ck "pool metadata rewritten ($1)" 1 \
	    "$(grep -c "^$P: pool metadata: done" "$O/rewrite-$1.txt")"
}

# drain <vdev> <vdev id> <tag>: allocating=off, rewrite -A, nothing left.
drain() {
	run "zpool set allocating=off $1" $ZP set allocating=off $P "$1"
	ck "allocating of $1" off "$($ZP get -Hpo value allocating $P "$1")"
	rewrite "$3"
	rewrite_done "$3"
	if ! wait_empty "$1" 60; then
		census "$2" "$3-left"
		stop "$(alloc "$1") bytes still allocated on $1 60 s after the rewrite"
	fi
	log "ok   nothing allocated on $1"
	census "$2" "$3"
	ck "census $3: blocks and deferred frees on vdev $2" "0 0" "$NB $ND"
}

hole_checks() { # vdev expected-holes
	ck "$1 listed by zpool status" 0 "$($ZP status $P | grep -cw -- "$1")"
	ck "indirect vdevs" 0 "$($ZP status $P | grep -c indirect)"
	ck "removal line of zpool status" 0 \
	    "$($ZP status $P | grep -c '^ *remove:')"
	ck "holes in the pool configuration" "$2" "$(holes -U "$CF")"
	ck "feature@device_removal" enabled "$(prop feature@device_removal)"
	ck "feature@obsolete_counts" enabled "$(prop feature@obsolete_counts)"
}

# labeltxg <file>: the label lines of the file with a txg other than 0; an
# erased label, which an import ignores, has txg 0.
labeltxg() { $ZDB -l "$1" 2>/dev/null | grep -cE '^ *txg: [1-9]'; }

remove_hole() { # vdev expected-holes leaf-files...
	local v=$1 h=$2 out rc s0 f
	shift 2
	s0=$(prop size)
	ck "label of ${1##*/} has a txg before the removal" yes \
	    "$(yn [ "$(labeltxg "$1")" -gt 0 ])"
	out=$($ZP remove $P "$v" 2>&1)
	rc=$?
	log "zpool remove $P $v: exit $rc${out:+: $out}"
	$ZP status $P > "$O/status-removed-${v##*/}.txt" 2>&1
	$ZDB -U "$CF" -C $P > "$O/config-removed-${v##*/}.txt" 2>&1
	cat "$O/status-removed-${v##*/}.txt" >> "$O/log.txt"
	ck "zpool remove $v exit status" 0 "$rc"
	hole_checks "$v" "$h"
	for f in "$@"; do
		ck "labels with a txg left on ${f##*/}" 0 "$(labeltxg "$f")"
	done
	note "pool size $s0 -> $(prop size)"
}

scrubcheck() { # tag
	run "zpool scrub -w ($1)" $ZP scrub -w $P
	$ZP status -v $P > "$O/status-$1.txt" 2>&1
	cat "$O/status-$1.txt" >> "$O/log.txt"
	ck "scrub ($1) repaired nothing, found no error" 1 \
	    "$(grep -cE 'scrub repaired 0B .* with 0 errors' "$O/status-$1.txt")"
	ck "zpool status -x ($1)" "pool '$P' is healthy" "$($ZP status -x $P 2>&1)"
	ck "data errors ($1)" 1 \
	    "$(grep -c 'errors: No known data errors' "$O/status-$1.txt")"
}

reimport() { # tag
	busy_run "zpool export ($1)" $ZP export $P
	run "zpool import ($1)" $ZP import -l -d "$D" -o cachefile="$CF" $P
}

zdbcheck() { # tag: zdb -bcc of the exported pool
	local rc
	busy_run "zpool export for zdb ($1)" $ZP export $P
	$ZDB -e -p "$D" -bcc $P > "$O/zdb-bcc-$1.txt" 2>&1
	rc=$?
	tail -40 "$O/zdb-bcc-$1.txt" >> "$O/log.txt"
	ck "zdb -bcc ($1) exit status" 0 "$rc"
	ck "zdb -bcc ($1) finds no leak" 1 \
	    "$(grep -c 'No leaks (block sum matches space maps exactly)' "$O/zdb-bcc-$1.txt")"
	run "zpool import after zdb ($1)" $ZP import -l -d "$D" \
	    -o cachefile="$CF" $P
}

verify() { # removed-vdev expected-holes tag
	samecontent "$3-removed"
	scrubcheck "$3-removed"
	reimport "$3"
	hole_checks "$1" "$2"
	samecontent "$3-imported"
	zdbcheck "$3"
}

finish() {
	ck "ereports" 0 "$($ZP events -H | grep -c ereport)"
	ck "pool property leaked" 0 "$(prop leaked)"
	kcheck
	busy_run "zpool destroy" $ZP destroy $P
	rm -rf "$D" "$M" "$CF" "$W/case$C.key"
	echo "drain-raidz-1108: case $C end" > /dev/kmsg
	log "PASSED"
}

# 1: two raidz1 vdevs, about 60 % of one vdev filled, snapshots taken and
# destroyed; raidz1-0, the first vdev, is drained and removed.
case1() {
	mkpool raidz
	fill 12 128 3000
	churn 3000
	content "$O/c-0.txt"
	level before
	ck "raidz1-0 holds data" yes "$(yn [ "$(alloc raidz1-0)" -gt 0 ])"
	census 0 before
	ck "census before: blocks on vdev 0" yes "$(yn [ "$NB" -gt 0 ])"
	drain raidz1-0 0 drain
	level drained
	remove_hole raidz1-0 0 "$D/a0" "$D/a1" "$D/a2"
	verify raidz1-0 0 c1
	truncate -s "$DEV" "$D/c0" "$D/c1" "$D/c2" || stop "truncate"
	run "zpool add raidz1" $ZP add $P raidz1 "$D/c0" "$D/c1" "$D/c2"
	ck "new vdev raidz1-2 listed" 1 "$($ZP status $P | grep -cw raidz1-2)"
	ck "holes after the add" 0 "$(holes -U "$CF")"
	head -c 256M /dev/urandom > "$M/fs/after-add" || stop "write after add"
	run "zpool sync" $ZP sync $P
	ck "raidz1-2 allocates" yes "$(yn [ "$(alloc raidz1-2)" -gt 0 ])"
	level added
	scrubcheck added
	finish
}

# 2: the same, with a snapshot of every dataset that holds blocks on
# raidz1-1, the vdev drained; it is destroyed after the attempts.
case2() {
	local i v a out rc
	mkpool raidz
	fill 12 128 3000
	run "snapshot $P@keep" $ZF snapshot -r $P@keep
	v=$(zvdev) || stop "no device for $P/vol"
	for i in 2 3 4 5; do
		head -c 64M /dev/urandom > "$M/big/L$i" || stop "rewrite L$i"
	done
	for ((i = 0; i < 1000; i++)); do
		rm -f "$M/fs/small/$i"
	done
	dd if=/dev/urandom of="$v" bs=1M count=32 oflag=direct conv=fsync \
	    status=none || stop "rewrite $v"
	run "zpool sync" $ZP sync $P
	content "$O/c-0.txt"
	level before
	census 1 before
	ck "census before: blocks on vdev 1" yes "$(yn [ "$NB" -gt 0 ])"
	run "zpool set allocating=off raidz1-1" $ZP set allocating=off $P raidz1-1
	ck "allocating of raidz1-1" off \
	    "$($ZP get -Hpo value allocating $P raidz1-1)"
	rewrite snap
	rewrite_done snap
	ck "datasets whose snapshot keeps blocks" 5 \
	    "$(grep -c ': 1 snapshots keep their blocks$' "$O/rewrite-snap.txt")"
	for ((i = 0; i < 20; i++)); do
		$ZP sync $P
		sleep 1
	done
	a=$(alloc raidz1-1)
	ck "raidz1-1 still holds the blocks of the snapshots" yes \
	    "$(yn [ "$a" -gt 0 ])"
	level snap
	census 1 snap
	ck "census snap: blocks on vdev 1 outside the snapshots" 0 "$NL"
	note "census snap: $NB blocks on vdev 1, all in snapshots, $a bytes allocated"
	rewrite again
	rewrite_done again
	for ((i = 0; i < 5; i++)); do
		$ZP sync $P
		sleep 1
	done
	ck "allocated on raidz1-1 after a second rewrite" "$a" \
	    "$(alloc raidz1-1)"
	samecontent snap
	out=$($ZP remove $P raidz1-1 2>&1)
	rc=$?
	log "zpool remove $P raidz1-1: exit $rc${out:+: $out}"
	ck "zpool remove raidz1-1 refused" 1 "$rc"
	ck "refusal says raidz" 1 "$(printf '%s\n' "$out" | grep -c 'not be raidz')"
	ck "raidz1-1 still listed" 1 "$($ZP status $P | grep -cw raidz1-1)"
	ck "allocating of raidz1-1" off \
	    "$($ZP get -Hpo value allocating $P raidz1-1)"
	ck "removal line of zpool status" 0 \
	    "$($ZP status $P | grep -c '^ *remove:')"
	ck "holes in the pool configuration" "" "$(holes -U "$CF")"
	ck "feature@device_removal" enabled "$(prop feature@device_removal)"
	run "destroy $P@keep" $ZF destroy -r $P@keep
	run "zpool wait -t free" $ZP wait -t free $P
	if ! wait_empty raidz1-1 60; then
		census 1 after-left
		stop "$(alloc raidz1-1) bytes still allocated on raidz1-1 60 s after the snapshots went"
	fi
	log "ok   nothing allocated on raidz1-1"
	census 1 after
	ck "census after: blocks and deferred frees on vdev 1" "0 0" "$NB $ND"
	remove_hole raidz1-1 1 "$D/b0" "$D/b1" "$D/b2"
	verify raidz1-1 1 c2
	finish
}

# 3: two mirrors and a plain disk; mirror-0 is drained and removed, then the
# disk, which leaves mirror-1 alone between two holes.
case3() {
	mkpool mirror
	fill 4 64 1500
	churn 1500
	content "$O/c-0.txt"
	level before
	ck "mirror-0 holds data" yes "$(yn [ "$(alloc mirror-0)" -gt 0 ])"
	ck "the disk holds data" yes "$(yn [ "$(alloc "$D/d0")" -gt 0 ])"
	census 0 before
	ck "census before: blocks on vdev 0" yes "$(yn [ "$NB" -gt 0 ])"
	drain mirror-0 0 drain0
	remove_hole mirror-0 0 "$D/m0" "$D/m1"
	samecontent removed0
	drain "$D/d0" 2 drain2
	remove_hole "$D/d0" "0 2" "$D/d0"
	level drained
	verify "$D/d0" "0 2" c3
	finish
}

# 4: two raidz1 vdevs, the pool filled to 60 %, which the other vdev cannot
# hold: allocating=off on raidz1-0, the rewrite, then allocating=on again.
case4() {
	local i rc
	mkpool raidz
	fill 12 128 3000
	for ((i = 12; i < 52; i++)); do
		[ "$(prop capacity)" -ge 60 ] && break
		head -c 64M /dev/urandom > "$M/big/L$i" || stop "write L$i"
		$ZP sync $P
	done
	level full
	ck "pool capacity 60 % or more" yes "$(yn [ "$(prop capacity)" -ge 60 ])"
	content "$O/c-0.txt"
	run "zpool set allocating=off raidz1-0" $ZP set allocating=off $P raidz1-0
	ck "allocating of raidz1-0" off \
	    "$($ZP get -Hpo value allocating $P raidz1-0)"
	level noalloc
	rewrite full
	note "zpool rewrite -A -v exit $RC," \
	    "$(grep -c '^cannot rewrite' "$O/rewrite-full.txt") 'cannot rewrite' lines," \
	    "first: $(grep -m1 '^cannot rewrite' "$O/rewrite-full.txt")"
	ck "zpool rewrite -A -v fails" yes "$(yn [ "$RC" -ne 0 ])"
	for ((i = 0; i < 5; i++)); do
		$ZP sync $P
		sleep 1
	done
	level after
	ck "pool health" ONLINE "$(prop health)"
	ck "zpool status -x" "pool '$P' is healthy" "$($ZP status -x $P 2>&1)"
	samecontent full
	if (head -c 1M /dev/urandom > "$M/fs/probe") 2> "$O/probe.txt"; then
		note "a 1 MiB write with allocating=off succeeded"
	else
		note "a 1 MiB write with allocating=off failed: $(head -1 "$O/probe.txt")"
	fi
	rm -f "$M/fs/probe"
	run "zpool set allocating=on raidz1-0" $ZP set allocating=on $P raidz1-0
	ck "allocating of raidz1-0" on \
	    "$($ZP get -Hpo value allocating $P raidz1-0)"
	head -c 64M /dev/urandom > "$M/fs/after" ||
	    stop "a 64 MiB write after allocating=on failed"
	run "zpool sync" $ZP sync $P
	rm -f "$M/fs/after"
	level on
	samecontent on
	scrubcheck full
	finish
}

case$C
exit 0
EOF2
chmod +x $L/drain.sh
{
sha256sum $L/census.awk $L/drain.sh
sudo -n modprobe -a zlib_deflate zlib_inflate 2> /dev/null
echo "drain-raidz-1108: load" | sudo -n tee /dev/kmsg > /dev/null
sudo -n insmod $Z/module/spl.ko && sudo -n insmod $Z/module/zfs.ko && echo "insmod ok"
for m in spl zfs; do echo "$m srcversion tree=$(modinfo -F srcversion $Z/module/$m.ko) loaded=$(cat /sys/module/$m/srcversion)"; done
sudo -n dmesg | sed -n '/drain-raidz-1108: load/,$p' | grep 'ZFS: Loaded module'
} 2>&1 | tee $L/step2.txt
```

Expected:

```
80ff3b9005e4d9211aabf5c8ef361e55c862cdb62c0d60eeafad90ff94847a6b  /var/log/drain-raidz-1108/census.awk
fda663fda1dded03c8aef0976d93027a70f2fb53f2e4126c2f5ffb0941ee720a  /var/log/drain-raidz-1108/drain.sh
```

then `insmod ok`, `tree=` equal to `loaded=` for `spl` and `zfs`, and one
`ZFS: Loaded module v... (DEBUG mode)` line. A different hash, no `DEBUG
mode` or a srcversion mismatch: stop.

## STEP 3: the four cases (about 40 min)

One command; it stops at the first case that does not pass.

```sh
. /var/log/drain-raidz-1108/env.sh
for c in 1 2 3 4; do
    sudo -n $L/drain.sh $c || break
done 2>&1 | tee $L/cases.txt
grep -c 'PASSED$' $L/cases.txt     # 4
cat $L/stops.txt 2>/dev/null       # nothing
```

Each case writes its files to `$L/case<N>/`: `log.txt` (every check and every
command output), `rewrite-<tag>.txt` (the output of each `zpool rewrite -A
-v`), `census-<tag>.txt` (the census tables), `list-<tag>.txt` (`zpool list
-v` at each stage), `status-*.txt`, `config-removed-*.txt` (`zdb -C` after
each removal), `zdb-bcc-*.txt`, and the raw `zdb` outputs (`zdbb-*`,
`zdbd-*`), which stay out of the REPORT.

## STEP 4: after a STOP only: diagnostics (2 min)

```sh
. /var/log/drain-raidz-1108/env.sh
c=$(grep -o 'case [1-4]:' $L/cases.txt | tail -1 | tr -dc 1-4); P=dr$c
{
echo "== case $c, stop: $(tail -1 $L/stops.txt 2>/dev/null || echo 'none written (hang?)')"
sudo -n $Z/zpool status -v $P
sudo -n $Z/zpool list -v $P
sudo -n $Z/zpool get allocated,allocating,removing,state $P all-vdevs
sudo -n $Z/zpool get all $P | grep -E 'feature@(device_removal|obsolete_counts|allocation_classes|block_cloning)|freeing|leaked|capacity|health'
sudo -n $Z/zfs list -o name,used,avail,refer,mounted -r -t all $P
sudo -n $Z/zpool import -d $W/case$c 2>&1 | head -40
sudo -n $Z/zpool events -v | tail -150
sudo -n $Z/zpool history -il $P | tail -60
sudo -n tail -300 /proc/spl/kstat/zfs/dbgmsg
sudo -n dmesg | sed -n "/drain-raidz-1108: case $c start/,\$p" | tail -150
ps -eo pid,stat,wchan:32,cmd | grep -E 'zpool|zfs |zdb|drain.sh' | grep -v grep
} > $L/diag.txt 2>&1
wc -l $L/diag.txt
```

The `zpool import -d` line lists the pool if the stop came while it was
exported; it imports nothing. If a command of this block hangs, leave it and
say so in the REPORT. Then go to STEP 6, not STEP 5.

## STEP 5: cleanup (only when the four cases passed, or when the reply asks for it)

```sh
. /var/log/drain-raidz-1108/env.sh
{
if grep -q '^zfs ' /proc/modules; then
    for p in dr1 dr2 dr3 dr4; do
        sudo -n $Z/zpool list -H -o name $p > /dev/null 2>&1 && sudo -n $Z/zpool destroy -f $p && echo "destroyed $p"
    done
    sudo -n $Z/scripts/zfs.sh -u
fi
grep -E '^(zfs|spl) ' /proc/modules           # nothing
sudo -n rm -rf $W && echo "work dir removed; logs and scripts kept in $L"
sudo -n fstrim -av
df -h /var/tmp | tail -1
} 2>&1 | tee $L/step5.txt
```

## STEP 6: REPORT.txt

Write `/var/log/drain-raidz-1108/REPORT.txt` yourself first, with, in this
order:

1. One verdict line per case: `PASSED`, or the `STOP` line, or "not run".
   Each with the figures it rests on: the space allocated on the drained vdev
   before and after (`list-*.txt`), the census counts before and after, what
   `zpool remove` printed, the holes, the scrub and `zdb -bcc` results. For
   case 2 also: the refusal message, the `census snap` line and the space
   left on `raidz1-1` while `@keep` existed. For case 4: the available space
   before and with `allocating=off` (`note (full)`, `note (noalloc)`), the
   exit status and first error line of the rewrite, the 1 MiB write, and the
   space per vdev after.
2. Your reading against "Reading the result" below, a few lines.
3. Every deviation from this document, and anything that looked wrong.

Then append the logs:

```sh
. /var/log/drain-raidz-1108/env.sh
sudo -n dmesg | sed -n '/drain-raidz-1108: load/,$p' > $L/kmsg.txt
{
for s in 0 1 2; do echo "== STEP $s"; cat $L/step$s.txt; done
echo "== STEP 3: cases.txt"; cat $L/cases.txt
for c in 1 2 3 4; do
    for f in $L/case$c/rewrite-*.txt $L/case$c/census-*.txt $L/case$c/list-*.txt $L/case$c/status-removed-*.txt; do
        [ -f "$f" ] && { echo "== case $c: ${f##*/}"; cat "$f"; }
    done
done
echo "== stops"; cat $L/stops.txt 2>/dev/null || echo none
echo "== STEP 4"; cat $L/diag.txt 2>/dev/null || echo "not run (no STOP)"
echo "== kernel messages other than the markers and the module lines"
grep -vE 'drain-raidz-1108: |ZFS: (Loaded|Unloaded) module|taints kernel|module verification failed' $L/kmsg.txt || echo none
echo "== STEP 5"; cat $L/step5.txt 2>/dev/null || echo "not run: everything left in place after the STOP"
} >> $L/REPORT.txt
wc -l $L/REPORT.txt
```

Reply with the whole of `REPORT.txt`. The raw `zdb` outputs stay in
`$L/case<N>/`.

## Reading the result

1. **Case 1 passes**: the series empties and removes a normal-class RAID-Z
   top-level vdev as it is, the pool's first vdev included, after snapshots
   came and went; what was shown on a special mirror holds for RAID-Z and the
   normal class, for live data. Otherwise:
   - a STOP at "still allocated ... 60 s after the rewrite" is the main thing
     this task looks for: `census-drain-left.txt` names what stayed (dataset
     or MOS object type, for example space maps or deadlists), which the
     rewrite does not cover yet;
   - a STOP at `zpool remove` with the raidz message: the vdev was no longer
     empty when the removal checked it; compare with the census just before;
   - a STOP after the removal (hole checks, labels, scrub, import, `zdb
     -bcc`): a defect of the hole path on a vdev that held data or sits at
     index 0, which no test covered;
   - a STOP at `raidz1-2`: the pool does not take a new vdev after the hole.
2. **Case 2** shows the expected limitation: snapshots keep their blocks, and
   `zpool remove` refuses with a message about sector sizes and raidz, which
   does not name the real cause (the fork branch
   `remove-not-empty-message-rb1104`, stacked on #19373 and not in this
   series, changes that message). Blocks outside snapshots in `census
   snap`, or space on `raidz1-1` that the second rewrite changes, mean the
   first rewrite missed live blocks: a finding. After `@keep` is destroyed
   the vdev must empty and go as in case 1.
3. **Case 3** is the same on mirror and plain normal-class vdevs. There a
   vdev that is not empty would be evacuated by the old removal path, with an
   indirect vdev, instead of refused: this is why `drain.sh` never runs
   `zpool remove` before the census shows the vdev empty.
4. **Case 4**, expected from reading: the available space drops to 0 with
   `allocating=off`, the rewrite fails with "No space left on device" as soon
   as a dataset (or the pool metadata) has a block to move, nothing moves,
   the pool stays healthy, the data unchanged, and writes work again with
   `allocating=on`. That is a usability gap, not damage: neither command
   warns beforehand that the other vdevs cannot hold the data. Anything else
   (a suspended pool, I/O errors, changed data, a rewrite that exits 0) is
   the finding.
5. **Throughout**: no ereport, `leaked` 0, and no kernel message other than
   the markers and the module lines.

Not covered here: deduplicated blocks (they stay where their DDT entry has
them; the fix for that, #19389, is not in this series), a checkpoint (`zpool
rewrite` refuses it), writes during the drain (the series' last commit makes
ztest drain a special or dedup vdev under load, in user space), RAID-Z
expansion, dRAID, a release build, FreeBSD, and an import by an older
release.
