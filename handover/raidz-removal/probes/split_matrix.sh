#!/bin/bash
# For K in 1..18: kill split_probe right before the K-th label or uberblock
# write of the split, then import by guid every pool a scan finds, with the
# master and the fixed libzpool.  Usage: split_matrix.sh <scratch> <evidence>
S=$1
E=$2
for K in $(seq 1 18); do
	D=$S/k$K
	rm -rf "${D:?}" "${S:?}/k$K.bak"
	mkdir -p "$D"
	cat > "$S/k$K.gdb" <<GDB
set pagination off
set confirm off
set breakpoint pending on
set \$n = 0
break vdev_label_sync_list if in_split
commands
silent
set \$n = \$n + 1
if \$n == $K
printf "KILL at labels spa=%s l=%d txg=%llu\n", spa->spa_name, l, txg
kill
quit
end
continue
end
break vdev_uberblock_sync_list if in_split
commands
silent
set \$n = \$n + 1
if \$n == $K
printf "KILL at ub spa=%s txg=%llu\n", spa->spa_name, ub->ub_txg
kill
quit
end
continue
end
run
quit
GDB
	where=$(cd "$E/../wt-cf" && timeout 300 ./libtool --mode=execute gdb -q -batch \
	    -x "$S/k$K.gdb" --args "$E/bin_split_wt-cf" "$D" 2>&1 | grep -m1 "^KILL")
	rm -f "${D:?}/zpool.cache"
	cp -a "$D" "$S/k$K.bak"
	for t in wt-cf wt-vtxg; do
		cd "$E/../$t" || exit 1
		guids=$(./libtool --mode=execute "$E/bin_guidimp_$t" "$D" 0 2>&1 |
		    awk '/^found pool guid/ {print $4}' | tr -d ',')
		res=""
		for g in $guids; do
			r=$(./libtool --mode=execute "$E/bin_guidimp_$t" "$D" "$g" 2>&1 |
			    grep -E "^import of|^imported" | sed 's/.*: //' |
			    tr '\n' ' ')
			res="$res [$g: $r]"
			rm -rf "${D:?}"
			cp -a "$S/k$K.bak" "$D"
		done
		echo "K=$K ${where#KILL at } | $t:$res"
	done
done
