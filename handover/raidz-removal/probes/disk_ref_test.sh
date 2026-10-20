set -eEuo pipefail
by_id() {
	local l
	for l in /dev/disk/by-id/*"$2"*; do
		[ -e "$l" ] || continue
		case $l in *-part*) continue ;; esac
		if [ "$(readlink -f "$l")" = "$1" ]; then
			printf '%s\n' "$l"
			return 0
		fi
	done
	return 1
}
disk_ref() {
	by_id "$1" "$2" 2>/dev/null || printf '%s\n' "$1"
}
x=$(disk_ref /dev/sdb ZTSTESTS)
echo "got: $x"
