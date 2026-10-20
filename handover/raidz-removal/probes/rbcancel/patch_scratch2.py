import sys
p = sys.argv[1]
s = open(p).read()
helper = '''uint64_t rbw_unneeded_closed;
uint64_t rbw_unneeded_other;

/* SCRATCH: classify the leaves holding DTL_MISSING when a check fails. */
static void
rbw_note_unneeded(vdev_t *vd)
{
	int closed = 0, other = 0;
	vdev_t *stack[64];
	int n = 0;

	stack[n++] = vd;
	while (n > 0) {
		vdev_t *v = stack[--n];
		if (v->vdev_ops->vdev_op_leaf) {
			if (zfs_range_tree_is_empty(v->vdev_dtl[DTL_MISSING]))
				continue;
			if (v->vdev_state == VDEV_STATE_CLOSED)
				closed++;
			else
				other++;
			continue;
		}
		for (uint64_t c = 0; c < v->vdev_children && n < 64; c++)
			stack[n++] = v->vdev_child[c];
	}
	if (closed > 0 && other == 0)
		rbw_unneeded_closed++;
	else
		rbw_unneeded_other++;
}
'''
a = "uint64_t rbw_reset_unneeded;\n"
assert s.count(a) == 1
s = s.replace(a, a + helper, 1)
for c in ("rbw_initiate_unneeded++;\n", "rbw_reset_unneeded++;\n"):
    assert s.count(c) == 1
    s = s.replace("\t\t\t" + c, "\t\t\t" + c[:-2] + ", rbw_note_unneeded(vd);\n" if False else "\t\t\t{ " + c[:-1] + " rbw_note_unneeded(vd); }\n", 1)
open(p, "w").write(s)
print("ok")
