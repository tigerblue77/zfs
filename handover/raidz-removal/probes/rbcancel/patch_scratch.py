import re, sys
p = sys.argv[1]
s = open(p).read()

def fn_span(name):
    i = s.index("\n" + name + "(void *arg, dmu_tx_t *tx)\n")
    j = s.index("\n}\n", i)
    return i, j + 3

hdr = '''
/* SCRATCH toggles and counters for the rebuild probes; not for upstream. */
int rbw_thread_fix = 0;
int rbw_sync_fix = 0;
int rbw_verify_soft = 0;
uint64_t rbw_cancels;
uint64_t rbw_initiates;
uint64_t rbw_resets;
uint64_t rbw_initiate_unneeded;
uint64_t rbw_reset_unneeded;
'''
anchor = "static __attribute__((noreturn)) void vdev_rebuild_thread(void *arg);\n"
assert anchor in s
s = s.replace(anchor, anchor + hdr, 1)

for name, cnt, unneeded in (("vdev_rebuild_initiate_sync", "rbw_initiates", "rbw_initiate_unneeded"),
                            ("vdev_rebuild_reset_sync", "rbw_resets", "rbw_reset_unneeded")):
    i, j = fn_span(name)
    body = s[i:j]
    first = "\tmutex_enter(&vd->vdev_rebuild_lock);\n"
    assert body.count(first) == 1
    body = body.replace(first, "\tif (rbw_sync_fix)\n\t\tspa_config_enter(spa, SCL_STATE, FTAG, RW_READER);\n" + first, 1)
    last = "\tmutex_exit(&vd->vdev_rebuild_lock);\n}\n"
    assert body.endswith(last)
    body = body[:-len(last)] + "\tmutex_exit(&vd->vdev_rebuild_lock);\n\tif (rbw_sync_fix)\n\t\tspa_config_exit(spa, SCL_STATE, FTAG);\n}\n"
    v = "\tVERIFY(vdev_resilver_needed(vd, &vrp->vrp_min_txg, &vrp->vrp_max_txg));\n"
    assert body.count(v) == 1
    body = body.replace(v, "\t%s++;\n\tif (rbw_verify_soft) {\n\t\tif (!vdev_resilver_needed(vd, &vrp->vrp_min_txg,\n\t\t    &vrp->vrp_max_txg))\n\t\t\t%s++;\n\t} else {\n\t" % (cnt, unneeded) + v + "\t}\n", 1)
    s = s[:i] + body + s[j:]

c = "\t\tif (vdev_rebuild_should_cancel(vd)) {\n"
assert s.count(c) == 1
s = s.replace(c, "\t\tif (rbw_thread_fix)\n\t\t\tspa_config_enter(spa, SCL_STATE, FTAG, RW_READER);\n\t\tboolean_t rbw_c = vdev_rebuild_should_cancel(vd);\n\t\tif (rbw_thread_fix)\n\t\t\tspa_config_exit(spa, SCL_STATE, FTAG);\n\t\tif (rbw_c)\n\t\t\trbw_cancels++;\n\t\tif (rbw_c) {\n", 1)
open(p, "w").write(s)
print("patched")
