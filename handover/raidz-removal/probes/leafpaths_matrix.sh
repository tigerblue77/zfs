#!/bin/bash
# Scratch (not for upstream): crash matrix for import-erased-leaf-paths,
# adapted from proto_matrix.sh.  For K in 1..N: kill ops_probe <op> (built
# against <tree>) right before the K-th label or uberblock write from the
# start of the operation to the end of the export.  Import every pool a scan
# finds, in place and renamed (devices copied under other names, recorded
# directory hidden), directly (guidimp2: can_be_active, no tryimport, as
# zdb -e) and through a tryimport (tryimp: as "zpool import" with the
# module), and open the cache file.  All with the code of <tree>.
# Results: [import result, root state, bad=leaves not healthy] per pool.
# GEN=<tree> runs the operation with the code of another tree (an older
# writer); the imports always use <tree>.
# Usage: [GEN=<tree>] leafpaths_matrix.sh <scratch> <tree> <op> <N>
S=$1
TREE=$2
OP=$3
N=$4
E=$SCRATCH/pr/evidence
T=$E/../$TREE
G=${GEN:-$TREE}
cd "$T" || exit 1
restore() {	# the devices in place, or renamed under $RN with $D hidden
	if [ -n "$RN" ]; then
		rm -rf "${RN:?}"; mkdir "$RN"
		for f in "$D.bak"/vd.*; do cp -a "$f" "$RN/m_${f##*/}"; done
	else
		rm -rf "${D:?}"; cp -a "$D.bak" "$D"
	fi
}
imp() {	# <dir> <probe>: import every pool found
	local dir=$1 bin=$E/bin_$2_$TREE res="" g r s nbad
	for g in $(./libtool --mode=execute "$bin" "$dir" 0 2>&1 |
	    awk '/^found pool guid/ {print $4}' | tr -d ','); do
		r=$(./libtool --mode=execute "$bin" "$dir" "$g" 2>&1)
		s=$(echo "$r" | grep -E "^import of|^imported" |
		    sed 's/.*: //;s/guid [0-9]*, root state //' | tr '\n' ' ')
		nbad=$(echo "$r" | grep "^tree:" | grep -E " (file|disk) " |
		    grep -vc " state 7 ")
		res="$res[${s% } bad=$nbad]"
		restore
	done
	echo "${res:-[none]}"
}
both() {	# <probe>: in place and renamed
	local inp rn
	RN=""
	inp=$(imp "$D" "$1")
	RN=$S/rn.$$
	mv "$D" "$D.hid"
	restore
	rn=$(imp "$RN" "$1")
	rm -rf "${RN:?}"
	RN=""
	mv "$D.hid" "$D"
	echo "in $inp ren $rn"
}
for K in $(seq 1 "$N"); do
	D=$S/${OP}_${G}_${TREE}_k$K
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
	where=$(cd "$E/../$G" && timeout 300 ./libtool --mode=execute gdb \
	    -q -batch -x "$D.gdb" --args "$E/bin_ops_$G" "$D" "$OP" 2>&1 |
	    grep -m1 "^KILL")
	if [ -z "$where" ]; then
		rm -rf "${D:?}" "$D.gdb"
		echo "$OP K=$K | no such write"
		break
	fi
	cp -a "$D" "$D.bak"
	d=$(both guidimp2)
	t=$(both tryimp)
	c=$(./libtool --mode=execute "$E/bin_scanimp2_$TREE" "$D" op \
	    cache 2>&1 | grep -E "^cache open|^opened" |
	    sed 's/.*: //;s/guid [0-9]*, root state //' | tr '\n' ' ')
	restore
	echo "$OP K=$K ${where#KILL } | direct $d | tryimport $t | cache[${c% }]"
	rm -rf "${D:?}" "${D:?}.bak" "$D.gdb"
done
