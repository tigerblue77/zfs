# Kernel-hybrid RAID-Z parity mover: run logs, 3-4 November 2026

Text logs of two runs of `VM-kernel-hybrid.md` on a disposable Debian 13 VM,
kernel `6.12.113+deb13-cloud-amd64`, modules built with `--enable-debug`.

| Run | Tree (tigerblue77/zfs) | Result |
|---|---|---|
| v2 | `d07e30bdb3929c9f303d065de47af0987b362634` | 734 checks PASS, 0 FAIL, six crash cuts |
| v1 | `91df27c68d49809eba85f1167f7a8c958f7a24dd` | first run, two of the six crash cuts |

## Layout

- `v2/summary-final.txt`: output of `kh.sh summary` after the last cut.
- `v2/kh.log`: every CHECK, MOVER, RESULT and INFO line of the run.
- `v2/run-*.out`: output of each `kh.sh` step (setup, a, b-dry, c, d, e, f,
  each b-cut and b-check).
- `v2/log/`: per case, `*.dmesg-filtered.txt`. The per-case `*.dbgmsg`
  files (`/proc/spl/kstat/zfs/dbgmsg`, about 3 MB for both runs) are not
  included; they are available on request.
- `v2/runs/`: per-run logs next to each run directory, by case
  (`*.verify*.log`, `*.zdb*.log`, `*.status*`, `*.drop.log`, `*.zil.log`).
- `v1/`: the same for the first run, with `summary-before-cuts.txt` (written
  before its cuts) in place of `summary-final.txt`. It has no `runs/`: the
  run directories were deleted once v2's inputs existed.

## Changes from the original logs

- `*.dmesg-filtered.txt` keep only the lines matching
  `ZFS|SPL|zfs|spl|VERIFY|ASSERT|WARNING|Call Trace`.
- The `*.dbgmsg` files are left out (see above). The files here contain
  no host name, domain, IP or MAC address.

Nothing else was edited.
