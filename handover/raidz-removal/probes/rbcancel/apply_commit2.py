import sys
p = sys.argv[1]
s = open(p).read()

def body(name):
    i = s.index("\n" + name + "(void *arg, dmu_tx_t *tx)\n{")
    j = s.index("\n}\n", i) + 3
    return i, j

# initiate
i, j = body("vdev_rebuild_initiate_sync")
b = s[i:j]
old = ("\tspa_feature_incr(vd->vdev_spa, SPA_FEATURE_DEVICE_REBUILD, tx);\n\n"
       "\tmutex_enter(&vd->vdev_rebuild_lock);\n")
new = ("\tspa_feature_incr(vd->vdev_spa, SPA_FEATURE_DEVICE_REBUILD, tx);\n\n"
       "\t/* For vdev_resilver_needed(), see vdev_rebuild_should_cancel(). */\n"
       "\tspa_config_enter(spa, SCL_STATE, FTAG, RW_READER);\n"
       "\tmutex_enter(&vd->vdev_rebuild_lock);\n")
assert b.count(old) == 1
b = b.replace(old, new)
tail = "\tmutex_exit(&vd->vdev_rebuild_lock);\n}\n"
assert b.endswith(tail)
b = b[:-len(tail)] + "\tmutex_exit(&vd->vdev_rebuild_lock);\n\tspa_config_exit(spa, SCL_STATE, FTAG);\n}\n"
s = s[:i] + b + s[j:]

# reset
i, j = body("vdev_rebuild_reset_sync")
b = s[i:j]
old = ("\tvdev_rebuild_phys_t *vrp = &vr->vr_rebuild_phys;\n\n"
       "\tmutex_enter(&vd->vdev_rebuild_lock);\n")
new = ("\tvdev_rebuild_phys_t *vrp = &vr->vr_rebuild_phys;\n\n"
       "\t/* For vdev_resilver_needed(), see vdev_rebuild_should_cancel(). */\n"
       "\tspa_config_enter(spa, SCL_STATE, FTAG, RW_READER);\n"
       "\tmutex_enter(&vd->vdev_rebuild_lock);\n")
assert b.count(old) == 1
b = b.replace(old, new)
assert b.endswith(tail)
b = b[:-len(tail)] + "\tmutex_exit(&vd->vdev_rebuild_lock);\n\tspa_config_exit(spa, SCL_STATE, FTAG);\n}\n"
s = s[:i] + b + s[j:]
open(p, "w").write(s)
print("applied")
