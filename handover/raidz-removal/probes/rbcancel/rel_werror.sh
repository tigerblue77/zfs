#!/bin/bash
# usage: rel_werror.sh <tree> <file.c>...  compile without debug, -Werror
S=$SCRATCH
cd "$1" || exit 1
shift
CF=$(sed -e 's/-DDEBUG -UNDEBUG//g; s/-DZFS_DEBUG//g; s/ -f *$//' $S/cflags.txt)
rc=0
for f in "$@"; do
	eval gcc $CF -UDEBUG -DNDEBUG -c -o /dev/null "$f" || rc=1
	echo "$f rc=$?"
done
exit $rc
