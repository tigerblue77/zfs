#!/bin/bash
# For K in 1..N: kill reguid_probe right before the K-th label or uberblock
# write made once the root vdev has the new guid (the reguid txg, then the
# export), then scan-import by name and import by guid every pool found,
# with the master and the fixed libzpool.  Usage: reguid_matrix.sh <scratch> <evidence> <N>
S=$1
E=$2
N=$3
for K in $(seq 1 "$N"); do
	D=$S/m$K
	rm -rf "${D:?}" "${S:?}/m$K.bak"
	mkdir -p "$D"
	cat > "$S/m$K.gdb" <<GDB
set pagination off
set confirm off
set breakpoint pending on
set \$n = 0
break vdev_label_sync_list if spa->spa_root_vdev->vdev_guid == 81985529216486895ULL
commands
silent
set \$n = \$n + 1
if \$n == $K
printf "KILL at labels l=%d txg=%llu\n", l, txg
kill
quit
end
continue
end
break vdev_uberblock_sync_list if svd[0]->vdev_spa->spa_root_vdev->vdev_guid == 81985529216486895ULL
commands
silent
set \$n = \$n + 1
if \$n == $K
printf "KILL at ub txg=%llu\n", ub->ub_txg
kill
quit
end
continue
end
run
quit
GDB
	where=$(cd "$E/../wt-cf" && timeout 300 ./libtool --mode=execute gdb -q -batch \
	    -x "$S/m$K.gdb" --args "$E/bin_reguid_wt-cf" "$D" 2>&1 | grep -m1 "^KILL")
	[ -z "$where" ] && { echo "K=$K: no such write, done"; break; }
	cp -a "$D" "$S/m$K.bak"
	for t in wt-cf wt-vtxg; do
		cd "$E/../$t" || exit 1
		guids=$(./libtool --mode=execute "$E/bin_guidimp_$t" "$D" 0 2>&1 |
		    awk '/^found pool guid/ {print $4}' | tr -d ',')
		res=""
		for g in $guids; do
			r=$(./libtool --mode=execute "$E/bin_guidimp_$t" "$D" "$g" 2>&1 |
			    grep -E "^import of|^imported" | sed 's/.*: //' |
			    sed 's/root state //' | tr '\n' ' ')
			res="$res [$r]"
			rm -rf "${D:?}"; cp -a "$S/m$K.bak" "$D"
		done
		r=$(./libtool --mode=execute "$E/bin_scanimp2_$t" "$D" rg cache 2>&1 |
		    grep -E "^cache open|^opened" | sed 's/.*: //' | tr '\n' ' ')
		res="$res cache:[$r]"
		rm -rf "${D:?}"; cp -a "$S/m$K.bak" "$D"
		echo "K=$K ${where#KILL at } $t:$res"
	done
done
