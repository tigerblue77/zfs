#!/bin/sh
# SPDX-License-Identifier: CDDL-1.0
#
# One table row per anyraid_adopt_map log: run, blocks, inside one tile
# (map / xlate mismatches), of which inside one metaslab (map / xlate
# mismatches), crossing a tile (map mismatches, crossing blocks inside one
# metaslab), asize/psize mismatches, verdict.
#
printf '| run | blocks | in tile | map bad | xlate bad | in tile+ms | map bad | xlate bad | cross tile | map bad | cross in ms | map bad |  asize/psize bad | verdict |\n'
printf '|---|---|---|---|---|---|---|---|---|---|---|---|---|---|\n'
for f in "$@"; do
	n=$(basename "$f" .log)
	awk -v n="$n" '
	/^blocks/ { gsub(/[;,]/, ""); b = $2; as = $5; ps = $8 }
	/^  inside one tile/ { gsub(/,/, ""); it = $4; itm = $7; itx = $10 }
	/^    of which/ { gsub(/,/, ""); im = $6; imm = $9; imx = $12 }
	/^  crossing/ { gsub(/[,;:]/, " "); ct = $5; ctm = $8; cms = $16; cmb = $19 }
	/^IDENTITY/ { v = ($2 == "HOLDS") ? "holds" : "FAILS" }
	END { printf "| %s | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s/%s | %s |\n",
	    n, b, it, itm, itx, im, imm, imx, ct, ctm, cms, cmb, as, ps, v }' "$f"
done
