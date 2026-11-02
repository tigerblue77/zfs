#!/usr/bin/env python3
# Print the frames of the interesting threads of a gdb "thread apply all bt".
import re
import sys

txt = open(sys.argv[1]).read()
keys = ['op_thread', 'other_thread', 'clear_thread', 'spa_sync']
for t in re.split(r'(?m)^(?=Thread )', txt):
    if not any(k in t for k in keys):
        continue
    lines = t.strip().splitlines()
    frames = []
    for line in lines[1:]:
        m = re.match(r'#\d+\s+(?:0x[0-9a-f]+ in )?(\S+)', line)
        if m and not m.group(1).startswith(('__', '___', 'futex',
                                            'lll_', 'start_thread',
                                            'clone3', 'zk_thread')):
            frames.append(m.group(1))
    print(' <- '.join(frames))
