#!/bin/bash
# For K in 1..N: kill ops_probe <op> right before the K-th label or
# uberblock write from the start of the operation to the end of the export,
# then import by guid every pool a scan finds, and open the pool from the
# cache file, with the master and the fixed libzpool.
# Usage: ops_matrix.sh <scratch> <evidence> <op> <N>
S=$1
E=$2
OP=$3
N=$4
for K in $(seq 1 "$N"); do
	D=$S/${OP}_k$K
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
	where=$(cd "$E/../wt-cf" && timeout 300 ./libtool --mode=execute gdb -q -batch \
	    -x "$D.gdb" --args "$E/bin_ops2_wt-cf" "$D" "$OP" 2>&1 | grep -m1 "^KILL")
	[ -z "$where" ] && { echo "$OP K=$K: no such write"; break; }
	cp -a "$D" "$D.bak"
	line="$OP K=$K ${where#KILL }"
	for t in wt-cf wt-vtxg; do
		cd "$E/../$t" || exit 1
		guids=$(./libtool --mode=execute "$E/bin_guidimp_$t" "$D" 0 2>&1 |
		    awk '/^found pool guid/ {print $4}' | tr -d ',')
		res=""
		for g in $guids; do
			r=$(./libtool --mode=execute "$E/bin_guidimp_$t" "$D" "$g" 2>&1 |
			    grep -E "^import of|^imported" | sed 's/.*: //;s/guid [0-9]*, root state //' |
			    tr '\n' ' ')
			res="$res[$r]"
			rm -rf "${D:?}"; cp -a "$D.bak" "$D"
		done
		r=$(./libtool --mode=execute "$E/bin_scanimp2_$t" "$D" op cache 2>&1 |
		    grep -E "^cache open|^opened" | sed 's/.*: //;s/guid [0-9]*, root state //' | tr '\n' ' ')
		rm -rf "${D:?}"; cp -a "$D.bak" "$D"
		line="$line | $t: $res cache[$r]"
	done
	echo "$line"
done
