#!/bin/bash
# Count label+uberblock writes made while in_op for one mode (gdb print-and-continue).
# Usage: ops2_count.sh <scratch> <evidence> <op>
S=$1; E=$2; OP=$3
D=$S/count_$OP
rm -rf "${D:?}"; mkdir -p "$D"
cat > "$D.gdb" <<GDB
set pagination off
set confirm off
set breakpoint pending on
set \$n = 0
break vdev_label_sync_list if in_op
commands
silent
set \$n = \$n + 1
printf "W%d L%d:%llu\n", \$n, l, txg
continue
end
break vdev_uberblock_sync_list if in_op
commands
silent
set \$n = \$n + 1
printf "W%d U:%llu\n", \$n, ub->ub_txg
continue
end
run
quit
GDB
cd "$E/../wt-cf" && timeout 600 ./libtool --mode=execute gdb -q -batch -x "$D.gdb" --args "$E/bin_ops2_wt-cf" "$D" "$OP" 2>&1 | grep -E "^W[0-9]+ |^$OP: " 
