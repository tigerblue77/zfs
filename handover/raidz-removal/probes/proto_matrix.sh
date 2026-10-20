#!/bin/bash
# Scratch (not for upstream): crash matrix for the detach DEGRADED prototype
# (tree wt-proto = detach-renamed-import + validate-label-txg + PROTO code).
# For K in 1..N: kill ops_probe <op> right before the K-th label or
# uberblock write from the start of the operation to the end of the export,
# once with the standard writer and once with ZPROTO_DELAYERASE.  Import every
# pool a scan finds, in place and renamed (devices copied under other names,
# recorded directory hidden), with three importers: base, ZPROTO_LEAFPATHS
# (LP), and ZPROTO_LEAFPATHS + ZPROTO_TXG0 (LPT); and open the cache file.
# Results: [import result, root state, bad=leaves not healthy] per pool.
# Usage: proto_matrix.sh <scratch> <op> <N>
S=$1
OP=$2
N=$3
E=$SCRATCH/pr/evidence
T=$E/../wt-proto
cd "$T" || exit 1
restore() {	# the devices in place, or renamed under $RN with $D hidden
	if [ -n "$RN" ]; then
		rm -rf "${RN:?}"; mkdir "$RN"
		for f in "$D.bak"/vd.*; do cp -a "$f" "$RN/m_${f##*/}"; done
	else
		rm -rf "${D:?}"; cp -a "$D.bak" "$D"
	fi
}
imp() {	# <dir> <env...>: import every pool found
	local dir=$1 res="" g r s nbad
	shift
	for g in $(env "$@" ./libtool --mode=execute "$E/bin_guidimp2_wt-proto" \
	    "$dir" 0 2>&1 | awk '/^found pool guid/ {print $4}' | tr -d ','); do
		r=$(env "$@" ./libtool --mode=execute "$E/bin_guidimp2_wt-proto" \
		    "$dir" "$g" 2>&1)
		s=$(echo "$r" | grep -E "^import of|^imported" |
		    sed 's/.*: //;s/guid [0-9]*, root state //' | tr '\n' ' ')
		nbad=$(echo "$r" | grep "^tree:" | grep -E " (file|disk) " |
		    grep -vc " state 7 ")
		res="$res[${s% } bad=$nbad]"
		restore
	done
	echo "${res:-[none]}"
}
both() {	# <env...>: in place and renamed
	local inp rn
	RN=""
	inp=$(imp "$D" "$@")
	RN=$S/rn.$$
	mv "$D" "$D.hid"
	restore
	rn=$(imp "$RN" "$@")
	rm -rf "${RN:?}"
	RN=""
	mv "$D.hid" "$D"
	echo "in $inp ren $rn"
}
for K in $(seq 1 "$N"); do
	line="$OP K=$K"
	for W in std delay; do
		D=$S/${OP}_${W}_k$K
		RN=""
		rm -rf "${D:?}" "${D:?}.bak"
		mkdir -p "$D"
		cat > "$D.gdb" <<GDB
set pagination off
set confirm off
set breakpoint pending on
set \$n = 0
break vdev_label_sync_list if in_op
commands
silent
set \$n = \$n + 1
if \$n == $K
printf "KILL L%d:%llu\n", l, txg
kill
quit
end
continue
end
break vdev_uberblock_sync_list if in_op
commands
silent
set \$n = \$n + 1
if \$n == $K
printf "KILL U:%llu\n", ub->ub_txg
kill
quit
end
continue
end
run
quit
GDB
		WENV=()
		[ $W = delay ] && WENV=(ZPROTO_DELAYERASE=1)
		where=$(env "${WENV[@]}" timeout 300 ./libtool --mode=execute \
		    gdb -q -batch -x "$D.gdb" --args "$E/bin_ops_wt-proto" "$D" \
		    "$OP" 2>&1 | grep -m1 "^KILL")
		if [ -z "$where" ]; then
			rm -rf "${D:?}" "$D.gdb"
			line="$line | $W: no such write"
			continue
		fi
		cp -a "$D" "$D.bak"
		b=$(both X=0)
		lp=$(both ZPROTO_LEAFPATHS=1)
		lpt=$(both ZPROTO_LEAFPATHS=1 ZPROTO_TXG0=1)
		c=$(./libtool --mode=execute "$E/bin_scanimp2_wt-proto" "$D" op \
		    cache 2>&1 | grep -E "^cache open|^opened" |
		    sed 's/.*: //;s/guid [0-9]*, root state //' | tr '\n' ' ')
		restore
		line="$line || $W ${where#KILL }: base $b | LP $lp | LPT $lpt | cache[${c% }]"
		rm -rf "${D:?}" "${D:?}.bak" "$D.gdb"
	done
	echo "$line"
	case "$line" in *"std: no such write"*"delay: no such write"*) break ;; esac
done
