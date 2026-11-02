#!/bin/sh
# run.sh BUILD MODE LABEL: one run of rzub-BUILD in a fresh directory
R=$(cd "$(dirname "$0")" && pwd)
b=$1; m=$2; l=$3
d=$R/runs/$b-$m-$l
rm -rf "$d"; mkdir -p "$d"
cd "$d" && timeout 1200 "$R/rzub-$b" "$d" "$m" > out.txt 2> err.txt
rc=$?
rm -f "$d"/dev-*
echo "$b $m $l rc=$rc $(grep '^RESULT' "$d/out.txt")"
