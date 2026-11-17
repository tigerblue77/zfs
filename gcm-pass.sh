#!/bin/sh
# usage: gcm-pass.sh <pass> <gcmbench binary> <cpu> <tsc GHz> <variant>...
# One gcmbench run per variant, pinned to <cpu>, as run.sh runs it. Around
# each run, records load1 and the steal share of the whole VM and of <cpu>
# (/proc/stat) in noise.txt.
set -u
pass=$1 bin=$2 cpu=$3 ghz=$4
shift 4
L=${L:-/var/log/task-1117-gcm}
st() {
	awk -v c="$1" '$1 == c { t = 0; for (i = 2; i <= 9; i++) t += $i
	    print t, $9 }' /proc/stat
}
for v in "$@"; do
	la=$(cut -d' ' -f1 /proc/loadavg)
	ga=$(st cpu)
	ca=$(st "cpu$cpu")
	t0=$(date -u +%H:%M:%S)
	taskset -c "$cpu" "$bin" 400 "$ghz" "$v" > "$L/$pass-$v.txt" \
	    2> "$L/$pass-$v.err"
	rc=$?
	gb=$(st cpu)
	cb=$(st "cpu$cpu")
	t1=$(date -u +%H:%M:%S)
	lb=$(cut -d' ' -f1 /proc/loadavg)
	echo "$ga $gb $ca $cb" | awk -v p="$pass" -v v="$v" -v r="$rc" \
	    -v t0="$t0" -v t1="$t1" -v la="$la" -v lb="$lb" '{
		printf "%s %s rc=%s %s-%s load1 %s->%s steal_vm %.2f%% " \
		    "steal_cpu %.2f%%\n", p, v, r, t0, t1, la, lb,
		    100 * ($4 - $2) / ($3 - $1), 100 * ($8 - $6) / ($7 - $5)
	}' | tee -a "$L/noise.txt"
done
