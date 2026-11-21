#!/usr/bin/env python3
# SPDX-License-Identifier: CDDL-1.0
"""TASK-1121-M29C report: three settings of one binary on the host rig.

  m29c-report.py LOGDIR CALIB_MD BEST

LOGDIR/raw/A<n>/<arm>/ holds the runs (kit v2's layout, the disks'
statistics in LOGDIR/dev/); every run is read and judged valid or not by
kit v2's hr-report.py, imported from this file's directory, unchanged.
CALIB_MD is the calibration's calib.md (TASK-1121-HRIG2) and BEST its
calibrated point (e.g. G3): the runs G/BEST, C1/BEST and C2/BEST of its
"## Runs" table give the calibration spread.

Written before the runs (the decision rule of M-29, 21/11):
  D1 M29C-50 / M29C-0 is the lead (zil_indirect_min_pct=50 against the
  same binary at its default, 0).  In each pass where both ran validly,
  r = guest fsync p99 of M29C-50 / that of M29C-0 (each raised to 1 ms);
  g = the geometric mean of r over the passes.  The calibration spread s
  is max / min of the guest fsync p99 of G/BEST, C1/BEST and C2/BEST (at
  least two of them).  KEEP when g < 1 / s (M29C-50's flush p99 beats
  M29C-0's by more than the spread) and there is no W1 loss (no pass
  where M29C-0's run holds W1, guest media MB/s at least 0.9 of the
  offered 50, and M29C-50's does not); otherwise CLOSE.  No decision with
  fewer than 3 such passes, or without a spread.
  D2 M29C-P1I / M29C-0 (zfs_immediate_write_sz=2M on the same binary) and
  D3 M29C-50 / M29C-P1I are shown with kit v2's comparison and verdict
  rules (as its C5, the M-29 lead), and do not enter the decision.
"""

import importlib.util
import math
import os
import re
import statistics
import sys

H = os.path.dirname(os.path.abspath(__file__))
_spec = importlib.util.spec_from_file_location(
    "hr_report", os.path.join(H, "hr-report.py"))
hr = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(hr)

ARMS = ["M29C-50", "M29C-0", "M29C-P1I"]
LEAD = ("D1", "M29C-50", "M29C-0", "lead",
        "zil_indirect_min_pct=50 against 0, same binary")
PRIM = hr.FSYNC + ["h_syncq_w_p99"]
GUARD = hr.READS + ["w_mbs", "txg_mean"]
COMPARE = [
    LEAD + (PRIM, GUARD),
    ("D2", "M29C-P1I", "M29C-0", "lead",
     "zfs_immediate_write_sz=2M against the default, same binary",
     PRIM, GUARD),
    ("D3", "M29C-50", "M29C-P1I", "info",
     "zil_indirect_min_pct=50 against zfs_immediate_write_sz=2M",
     hr.ALL, []),
]
W1_MIN = 0.9
MIN_PASSES = 3


def calib_spread(path, best):
    """(spread, [(run, value)]) from the first table of calib.md."""
    want = ["G/" + best, "C1/" + best, "C2/" + best]
    cols = None
    vals = []
    try:
        with open(path) as f:
            for line in f:
                if not line.startswith("|"):
                    if cols is not None:
                        break
                    continue
                cells = [c.strip() for c in line.strip().strip("|").split("|")]
                if cols is None and cells[:2] == ["metric", "host"]:
                    cols = cells
                elif cols is not None and cells[0] == "guest fsync p99 ms":
                    for name in want:
                        if name in cols:
                            v = hr.num(cells[cols.index(name)])
                            if v is not None:
                                vals.append((name, float(v)))
                    break
    except OSError:
        return None, []
    vs = [v for _, v in vals if v > 0]
    if len(vs) < 2:
        return None, vals
    return max(vs) / min(vs), vals


def main(argv):
    if len(argv) != 4:
        sys.stderr.write(__doc__)
        sys.exit(2)
    logdir, calib_md, best = argv[1], argv[2], argv[3]
    raw = os.path.join(logdir, "raw")
    devdir = os.path.join(logdir, "dev")
    runs = {}
    rows = []
    for ps in sorted(os.listdir(raw)) if os.path.isdir(raw) else []:
        if not re.match(r"^A[0-9]+$", ps):
            continue
        runs[ps] = {}
        for arm in sorted(os.listdir(os.path.join(raw, ps))):
            d = os.path.join(raw, ps, arm)
            if not os.path.isdir(d):
                continue
            m = hr.run_metrics(d, devdir)
            a = hr.arm_record(d)
            end, err = hr.guest_state(d)
            why = hr.run_valid(arm, m, a, end, err)
            if arm not in ARMS:
                why.append("not an arm of this task")
            rows.append((ps, arm, m, why))
            if not why:
                runs[ps][arm] = m
    if not rows:
        sys.exit("no runs under %s" % raw)

    print("## Runs\n")
    print("| pass | arm | valid | srcversion | fsync p99 ms | guest MB/s |"
          " W1 | read p99 ms | syncq w p99 ms | why not valid |")
    print("|---|---|---|---|---|---|---|---|---|---|")
    for ps, arm, m, why in rows:
        w = m.get("w_ratio", hr.NAN)
        print("| %s | %s | %s | %s | %s | %s | %s %s | %s | %s | %s |" % (
            ps, arm, "no" if why else "yes",
            m["lay"].get("srcversion", "-"), hr.fmt(m.get("fsync_p99", hr.NAN)),
            hr.fmt(m.get("w_mbs", hr.NAN), 2),
            "-" if hr.isnan(w) else ("ok" if w >= W1_MIN else "FAIL"),
            hr.fmt(w, 3), hr.fmt(m.get("read_p99", hr.NAN)),
            hr.fmt(m.get("h_syncq_w_p99", hr.NAN), 2), "; ".join(why)))

    print("\n## Per arm: median (lowest-highest, valid runs)\n")
    print("| metric | " + " | ".join(ARMS) + " |")
    print("|---|" + "---|" * len(ARMS))
    keys = [(k, lb) for k, lb, _, _ in hr.ARM_METRICS] + \
        [("w_ratio", "guest MB/s / offered (W1)")] + hr.ARM_INFO
    for key, label in keys:
        cells = []
        for arm in ARMS:
            v = [runs[ps][arm][key] for ps in runs if arm in runs[ps] and
                 isinstance(runs[ps][arm].get(key), (int, float)) and
                 not hr.isnan(runs[ps][arm][key])]
            cells.append("%s (%s-%s, %d)" % (
                hr.fmt(statistics.median(v), 2), hr.fmt(min(v), 1),
                hr.fmt(max(v), 1), len(v)) if v else "-")
        print("| %s | %s |" % (label, " | ".join(cells)))

    print("\n## Comparisons\n")
    verdicts = []
    lead_res = None
    for cid, x, ref, kind, what, prim, guard in COMPARE:
        res = {}
        for key, label, direction, floor in hr.ARM_METRICS:
            res[key] = hr.compare(runs, x, ref, key, direction, floor)
        if cid == LEAD[0]:
            lead_res = res
        n = max(r[0] for r in res.values())
        print("### %s %s/%s: %s (%s, %d pairs)\n" % (cid, x, ref, what,
                                                     kind, n))
        print("| metric | role | g | p | below 1 / above 1 | by pass |"
              " verdict |")
        print("|---|---|---|---|---|---|---|")
        for key, label, direction, floor in hr.ARM_METRICS:
            nn, g, p, pairs, v = res[key]
            role = ("primary" if key in prim and kind == "lead" else
                    "guard" if key in guard else "")
            print("| %s | %s | %s | %s | %d / %d | %s | %s |" % (
                label, role, hr.fmt(g, 3), hr.fmt(p, 3),
                sum(1 for _, r in pairs if r < 1),
                sum(1 for _, r in pairs if r > 1),
                " ".join("%s:%.2f" % (ps, r) for ps, r in pairs), v))
        lv = hr.lead_verdict(kind, prim, guard, res)
        if n < 6:
            lv += "; %d pairs: below 6 the kit's rule can show nothing" % n
        verdicts.append("%s %s/%s %s: %s" % (cid, x, ref, what, lv))
        print("\n%s\n" % verdicts[-1])

    print("## Spreads\n")
    s, cal = calib_spread(calib_md, best)
    print("- calibration (%s, point %s): %s -> spread %s" % (
        calib_md, best,
        ", ".join("%s %.2f ms" % (k, v) for k, v in cal) or "no value found",
        "%.3f" % s if s else "unavailable"))
    for arm in ARMS:
        v = [runs[ps][arm]["fsync_p99"] for ps in sorted(runs)
             if arm in runs[ps] and not hr.isnan(runs[ps][arm]["fsync_p99"])]
        print("- %s fsync p99 over this task's passes: %s%s" % (
            arm, " ".join("%.1f" % x for x in v) or "-",
            " -> max/min %.3f" % (max(v) / min(v)) if len(v) >= 2 and
            min(v) > 0 else ""))

    print("\n## Decision (M-29 rule, fixed before the runs)\n")
    n, g, p, pairs, _ = lead_res["fsync_p99"]
    loss = []
    for ps in sorted(runs):
        a, b = runs[ps].get("M29C-50"), runs[ps].get("M29C-0")
        if a is None or b is None:
            continue
        if b.get("w_ratio", 0) >= W1_MIN and not \
                a.get("w_ratio", 0) >= W1_MIN:
            loss.append("%s (M29C-50 %.3f, M29C-0 %.3f)" % (
                ps, a.get("w_ratio", hr.NAN), b.get("w_ratio", hr.NAN)))
    print("- fsync p99 M29C-50 / M29C-0 by pass: %s; g %s over %d passes"
          " (p %s)" % (" ".join("%s:%.3f" % (ps, r) for ps, r in pairs) or
                       "-", hr.fmt(g, 3), n, hr.fmt(p, 3)))
    print("- threshold 1 / spread: %s" % ("%.3f" % (1.0 / s) if s else "-"))
    print("- W1 losses: %s" % (", ".join(loss) or "none"))
    if n < MIN_PASSES:
        print("\nDECISION NONE: %d passes with both arms valid, %d needed" % (
            n, MIN_PASSES))
    elif not s:
        print("\nDECISION NONE: no calibration spread in %s for %s" % (
            calib_md, best))
    elif g < 1.0 / s and not loss:
        print("\nDECISION KEEP: M29C-50's fsync p99 is %.3f of M29C-0's,"
              " below 1/spread %.3f, no W1 loss" % (g, 1.0 / s))
    else:
        why = []
        if not g < 1.0 / s:
            why.append("fsync p99 ratio %.3f not below 1/spread %.3f" % (
                g, 1.0 / s))
        if loss:
            why.append("W1 lost in %d passes" % len(loss))
        print("\nDECISION CLOSE: %s" % "; ".join(why))


if __name__ == "__main__":
    main(sys.argv)
