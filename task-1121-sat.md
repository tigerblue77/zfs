TASK-1121-SAT (from the coordinator session "main"): functional, about 2 h 15 with three builds. Run it on the VM that ran TASK-1121-SAI: it reads the pool image SAI left in `/var/log/task-1121-sai`. The regression test of our PR #19985 ("Linux: don't log the size change while replaying TX_SETATTR"), `slog_replay_fs_setattr_size`, built on our zinject named points (N-01, fork branch `zinject-delay-points-v2`). (A) With the fix, debug build: the test three times, `zinject_args` once (the commit adds a point to it), the group `-T slog` once. (B1) Fail-before, release build: the test three times, each must FAIL cleanly (the record's mode lost). (B2) Fail-before, debug build: the test once; an assertion must stop txg_sync, which wedges the pool and needs one reboot (pre-approved). Other VMs may run functional tasks at the same time: this task measures no performance.

End this task with your final report as your last message (the full text, nothing after it), then stay idle until main writes.

The transport may show the characters greater-than, less-than and ampersand as HTML entities: read them as the characters.

Shell variables do not survive from one of your commands to the next: start each command with the assignments it needs. In this task `D=/var/tmp/task-1121-sat`, `L=/var/log/task-1121-sat`, `I=/var/log/task-1121-sai/setattr_size.dat.bz2` (SAI's image).

## Why

Since #19706, Linux `zfs_setattr()` has `zfs_freesp()` log a size change as `TX_TRUNCATE`, also while the ZIL is replayed. When it replays a `TX_SETATTR` that carries `ATTR_SIZE` (which Linux wrote before #19706), `zfs_freesp()` changes the size and then, in a transaction of its own, `zfs_log_truncate()` marks the record replayed (`zil_replaying()`); `zfs_setattr()` then sets the other attributes in a second transaction, which marks it again. If the first txg reaches disk and the system stops before the second does, `zh_replay_seq` on disk already covers the record and the next replay skips it. #19985 passes `log=FALSE` to `zfs_freesp()` while replaying, so only the second transaction marks the record.

The test (commit 2 below) imports SAI's image, written by OpenZFS 2.4.4: its log holds one `TX_SETATTR` (mask 0x2069: `ATTR_MODE`, `ATTR_SIZE`, `ATTR_MTIME`, `ATTR_CTIME`, `ATTR_FILE`) that truncates `file` from 128 KiB to 4096 bytes and clears its setuid bit (mode 4777 to 777). It imports the pool with `-N`, arms `zinject -w zfs_setattr:size_changed` (a point the commit adds in `zfs_setattr()` right after `zfs_freesp()` returns), mounts the file system in the background so that the replay stops at the point, runs `zpool sync` and `zpool freeze`, releases the replay, checks the file (4096, 777, an mtime older than the test), exports, imports again and checks the file again (the same three values).

Expected:

- With the fix (part A): PASS in well under a minute. The test's log shows the record (`TX_SETATTR len ..., seq S` then `foid ..., mask 0x2069`), `After the first replay: size 4096, mode 777, mtime T`, `On disk: ZIL header: ... replay_seq 0, ...` and `After the second replay: size 4096, mode 777, mtime T`, with T equal to SAI's T4.
- Without the fix, release (part B1): FAIL. The same first lines, but `replay_seq S` on disk (the record's own seq), then `After the second replay: size 4096, mode 4777, mtime <a time during the run>` and the failure `After the second replay: size 4096 and mode 4777, not 4096 and 777`. The test's cleanup then destroys the pool normally.
- Without the fix, debug (part B2): both transactions mark the same record in two txgs, so the sync of the second txg trips `ASSERT(zh->zh_replay_seq < *replayed_seq)` in `zil_sync()`: `VERIFY(zh->zh_replay_seq < *replayed_seq) failed` and `PANIC at zil.c:4274:zil_sync()` in txg_sync, seconds after `zinject -c`. txg_sync then sleeps forever, the mount waits for it in D state, and the test hangs at `log_must wait $mountpid` until the runner's 600 s timeout; its cleanup then blocks too. This expectation comes from reading the code: report what you see.

## What is tested

Fetched: https://github.com/tigerblue77/zfs branch `zinject-delay-points-v2`, head `4a98dfeb57bddb64160b6c39f111e2e46b971ea6` (N-01: four commits on upstream master `a1daebfec3f5ca46ac261ef339efdf0dbeae997c`), and branch `setattr-replay-one-record` (#19985), head `d8f94d6359507ce38a796b3cfe1c9b65a8f2dbdb`. The test commit is not on GitHub: it is the patch below, plus SAI's image, which you add to it.

| Tree | Built from | Tree hash |
| --- | --- | --- |
| `fb-dbg`, `fb-rel` | N-01 + the test commit (with the image) | `d32bdf97254ee99e321afa08b351fb4cd89a9361` |
| `fix-dbg` | N-01 + #19985's commit + the test commit | `c75c8296708d26e0646f726a47504b2ac034c930` |

The image: `f68ea0ae6753ad8c9a1354b2091221953ed5351b9c2f0f6dcaf5bb50ccbeb8fb`, `63801` bytes (SAI's `setattr_size.dat.bz2`).

## VM rules

- Nothing is published on GitHub (fetching is fine). Never display, copy or create a credential file.
- ZTS uses its own file vdevs; touch no block device of the VM and no pool this task did not create; never `zpool import` anything yourself (the test imports its own image, with `-o cachefile=none`); never `zfs-tests.sh -x`.
- Before every ZTS run: `lsmod | grep -E '^zfs '` shows the module of the current step only, `timeout 30 sudo -n <tree>/zpool list -H -o name` shows no pool, `ls -d /var/tmp/file-vdev* /var/tmp/dev-* /var/tmp/tunable-* /var/tmp/disk-slog /var/tmp/disk2-slog /testpool_setattr 2>&1` shows nothing, and `test ! -s /etc/zfs/zpool.cache && echo CACHE-OK` prints CACHE-OK. Anything else is a stop: report its `ls -l`, touch nothing, wait for main.
- Every build and ZTS run runs detached (`setsid nohup ... &`) and is polled. A hang is: a build still going after 90 minutes; a single test after 15 minutes; a group after 60 minutes. In parts A and B1, capture `ps -eLo pid,tid,stat,comm,wchan:32 | grep -E ' D|txg|zfs|zpool|ksh'`, the kernel stacks of every D task (`sudo -n cat /proc/<pid>/stack`) and `sudo -n dmesg | tail -n 150` into `$L/hang-<tag>.txt`, then stop and report.
- Reboots: exactly one is pre-approved, in step 6, after the B2 run. In parts A and B1, a hang, a panic or a module that will not unload: capture as above, stop, report and wait for main. Before the step 6 reboot, arrange your own resumption the way DBM4 did (a scheduled task of your own session), say which, and write in `$L/progress.md` where you are. A global `sync` may block once txg_sync is wedged: use `sync -f $L/progress.md`, then the sysrq reboot through a transient unit, as in DBM4.
- If a fetch fails, wait 1 minute and try once more. If a commit, a tree, a hash or a count differs, stop and report the outputs as they are; do not edit any file of the trees beyond what step 1 says.
- `sudo -n fstrim -av` after deleting trees. Modules unloaded at the end (after the reboot they are not loaded).

## STEP 0: state

`hostname` (must be SAI's VM), `uname -r`, `uptime -s`, `nproc`, `free -m`, `df -h / /var/tmp` (at least 25 GiB free on /var/tmp: three trees), `lsmod | grep -E '^(zfs|spl) '` (must be empty), `ls -l /dev/zfs 2>&1` (must not exist), `sysctl kernel.panic_on_oops kernel.panic kernel.hung_task_panic` (quote; if hung_task_panic is 1, set it to 0 for this task and say so), `id zts`, `ls -d $D $L 2>&1` (both absent), the pre-run file check of the VM rules, `ls -l /etc/zfs/zpool.cache 2>&1` (absent expected; size 0: move it to `$L/zpool.cache.step0` in step 1 and say so; above 0: stop and report), and `sha256sum $I; stat -c %s $I`, which must print `f68ea0ae6753ad8c9a1354b2091221953ed5351b9c2f0f6dcaf5bb50ccbeb8fb` and `63801` (otherwise stop and report).

## STEP 1: the trees

The test commit, as a patch (gzip, then base64). Decode it and check it:

```sh
D=/var/tmp/task-1121-sat; L=/var/log/task-1121-sat
sudo -n mkdir -p $D $L && sudo -n chown "$(id -un):" $D $L
base64 -d > $D/test.patch.gz <<'EOF_B64'
H4sIAAAAAAACA806a3faSLKfrV9RQ9jEDiCeBkzWc+Ib4yx3EzvHMLMzyebo6NECrYXEqAV+TPLf
t6q6BeJlO3v3w/VJEPSjqrre1aWLJJ5C2+12RKt23KgJp10Twjtpdur+idPp1mxxbLfbfrdWb7dq
8DGOYChmUO9Ardbjf9Co1erGBYLpwSgYi8QJ56LTgb82O63ayfFJs5SuRt/OpUikGcWJmIX35jhI
J3PHdOPpz8a5nYoeDO20DI06XMYLBNxoQ73Va7R7jQ6UavhnDOfOv4Sb9uDLp7PRu799hc+jYQ8I
mn0PNox+s4b90dlodA0yjWcz4YHtpyKBIJUggwcB7sSOxsIwRkKmkE4E+MEdxD5/nSViEcRzCUjQ
NEAkt5MgFDz1efABAqkRCa9Mg0YOIIFYQ58IN048mM4RTRSnMLWTG4akJzJIpnGFo8ltIEUZAkVH
ejfOSMqjkPeRK8GOPDVzL1Mx5WNKwxE+snRzc0yQwU7TJHDmqZDgxUIy6RCJuzRjm7wJZpIPpGgr
Mw7i2DT2kKr4NhJJGcZJPJ8p9MEUgdmIMIxlahrGhyCa38HElnzU2yRIUxGBnLsTZIo+sAwiV8CL
+kmn1i6DjBWxJIVgOouTVBo2zOI4xJ82HvZ2EkuCP4ZJHHoS4ggpySA793A1E9HniyE0zJbZwllg
Goxb1CglSwLlJ/GDiHpIBOkd0GQ8T+Hd2SfrAgU1OIc0mUcuah7qCUiRzgPP8FHmSwI18a6dJIGQ
mnW3LBbFSGLREjDLRUFxgnQpqikxzAQYZSeW9r3Ec93yrD4vMm9qe7RskKozG4oApdoIjTE/BBEZ
AJ4viNBUHnxpIUaScY+IspSuoAxjZLeRmz48YgHp+aVylXNq5SdCPOhTEv/KqBChsKUeUqTwsSMQ
dyw0pSpKgJDGGbm0nGRnj+0gMo0R2xmaEpuDTIMwRGIWIkcF85GAMbMyDVbsRw37R47DaLFlzWo3
jrwtNda7enoxor0RYqbYqaQDW9LJEPpBIjPDQFGcC2c+BmcekAqSqa0sjRyVrQiyJWpXGqASIpSH
ILSIpchvWgTCRisgoyQPgPyCENUtMTbcAK2NlvarkZALcmJS6MSOpO0SCrn0JIa0p9lZ0XXg7tuY
dsulQcZRiOKyM4GiwiwPTV4PDRiXDoNxJLxK7PsV5/7/5sIrlYqBhLjh3BNV9E/VhyA2J/DY3zf8
X4eSgaeKqvi/W9UKbnaf2nYMJUSHejMPRTWW1ZDOXEWNp//WIkK/aMXSdDf3NRid2sarg9jSON2n
qCTrlVX0GaRWGqWJP/dvazCVah9iq6hv6vOjfSMIkGlPYSeVpmlW3TCwkjhOM8ZkT8tOUNQ3crID
He2TaH/8YSkds1auwCKTW239BvUmxtetPwO6bD5Lp1HGaHyCAtbaLg9LR2VogodOQv2uHBngJgIV
XNlzvVbrHB/vOb2Pnpe22eGzaDUML/B9qFTGZLvVbTVztseMIPLEHXjeSc1rd5tut2malOC02sde
3fOJvnarRXq7C6KBPNgJ9e1bqHSOO+U2lOjRARzAeCqSCCYJeRMrBdIq5JoXCjyT7d1byCX7/pCG
U3iNj6M3qBb3mKEIH0Q0n8JKDy1lq38acPB5cGUNLv+3/25kfboaXI6sXz69vz4771vnV5f98q4F
F9f9/vCT9e5vZ5fv++dlo7S9ROco1nDwub9auA1raMD3LbKsFAlfl8UO23V2DGpp+I1j22k06g0H
peF1XfxrOK5dW5fGju1KHDsmSB71FsqhxJ9v36IF/LPAH4Ammlbimx784+z6cnD5vkdRQzlSiifo
sDGGLESkNBHQl4oE09AJDH+/vPo0HAwVrIp57lFSKqYOJhL1TpmzU6O0Ntyo62Ewz1NMGZmV0MWf
VxI/FJ3dJukNPU5Ib6bxPErJtFSeyhHAljccq3X0VR7bs3FcKA++jA+4JJ0nEbp8MDFtGN5zQkBx
XM562moNuNLZEUcJO8t5OF5hekjJBoYHHJvkqeDAGawSBsRrQC7P1EkprkC7pQVBlGXKJvJlRc2u
9MQobdCUy6JX6SrHT21+MpcrrIik30Zpnco8kVsZMMc8MPshyYN59lu8ocxPBxTnGYu0sjfQ5zTa
wnU6DdO0692Tjt1xO8LZUPanwWnlf3ohKVmj3uqSlvGzS2qGqUsPLfxAJAmc5pTk8GFWhoU9q/y8
sC3F3Rr/G13/0icXdXCARckhbjui7wfjGJM8NJzmG3QsB5u+4dCbzq3Y+ReK3JIz+zCWR+SADijK
PMsLMcbv6F+A0U7JEl7CIa87Gw0+9r/x14/09Qi+fdspuq2g7uybyeJDzT1xuqJ17NdRSDWU1bHf
aPjtnULagrEmma1Ztvn6Mds8PjhWSDSIwIUbyl4XdmKRkuccrLtALug1uATzUdTuBF6r71v+OMI0
UH7Z8t1fUc4UQb48GkJoVSEntflsnGAVYpEPslyn52HdVSjvBLMeaBjQtvMpkPy/PEv2Swi7HAbR
8J0CD1Rfrwt9b0rm7J3SYj9p2cfdVs3z3RPTbPrixLe7ru0118W+F4iS+95ptsNWm6KSepAV0mI8
5pdXMph6lpzPqHAS3quvmAvYYzW1yotelYEX0rQBX3amTT32ol+NSg72eiKlBGJxZENIpectJNT7
E7LHCMZdewmO7JkXJBnNO+T4WIrsPL1GS7bV8kUbLbjZxBSj7jY7tfZJ+/ikvUuyj0HLi/ixdSTr
5gm7XHqwrKPYwXhteYFMLYrdeG6LtJt368/zs9EZlE7hn2hgeTa5ydyxpkGSxMnaD4supuKI0uEw
cLa2iciz7rzEEpEbe0E03h4xXX/8n2xbIiwdbKbtThi7N0r511J2PLPpPDS2sGWp/k5SlpN7Tqj0
J3tqEMrS6pxX4bP1fP4P310PPo2Gu0SwoyKp1epcND1raeO5S/NWp/eUDn6wNnoOHjkLA0ze9X3X
j+2ie7fnbUH7CxKKTSL29YZnWHkO4tPlrvNfAJL5iVrTaXleve2jn2h2O41u99izG01duD7qJ34I
3WOO5IcAcTrR5KiiHjiQAeDwYs3sKHAt356HmBqIsbE5zTkDTxucH1ANSnVFxVZXRPL08MmEAKVa
4aRuV8Q/Qv3dN/dIgKf8EvN2mFFBUPxT0fLl7dfvb8CLKfckLeP7Q7HAQq2QXYRWbqE4g4pEmUFx
1B+OPl1dfSjQhmzFNxijdkLlDygUZwXMGn9YKZ9lg85/B45Bt7zqsnR1haL1tbb6w7iGQa7u1qk3
w4pa9cSiGs3D8Lna9ixqSOFq5RqqW5luibC2Lr34qeoEUZXOXJnhTxh+Ov+t8iFwRSRFZeBRMe0H
VEa/Oz//UKmbNVxE60YTrNT4aFmDAVPaFJerZgKlQ2GARdwcD6tLYZFMpb6bJQjvOP7BuViIMJ5N
hb7RPEcnzxVewI0AJgQOC4S+cIT1jUgkzSAlJkH5PZ7D1L7n+1HqDCD0jDAqRl26UbWpW7HsJizp
0KAICkLLbo0JkakPeYYGF4Z4stl9Np1SsyW3FOQknoeeugIndFMyWq7zA0kwZDxPXGoFnK3B4b1I
qR3KGOyFHYS2gzQvApunBxFdQdEVQUpAJmk6k71qNZ4hOxTAOBljhsrsqWayUXRjxQ7F4ejCGv4y
GPUf0RUdmWkHH/e8r4Lo4Oqyh78PfhVJ4N+r0n1XQ4wnlM2vSvp8a42AcNuAZMGs1F2xfMNrd1cP
YdjuRIHAuH+zflcid/TDAi8T23B0fTbqv/+dD1E3YcA9DXi0J7XW4CJABw0TPtKFzuoGRVO91RpR
TTaCA2ttHoKCzpPG9rnKrPdDHZ4VC3PNHSaliaQIO+IWpmry5Ho8qxaPanTpRka+Q6roUBwlTml2
RshEzAiXAlj1FKPsvip3IFODWW8A/VDrBwG0TOjfKXEsu017m02byLjbpMnIEE8zqa8al/oSy17T
JxxHDR5d/3L5DpVjZwuToKw6eE82KElqiKTd+qhIXKDT7iliDh5Y1fT9eeUaimcfRhy0aTzTBCj+
et7/lZf7Mlu8uajqkxs5cD2GUd0zfchpHdQb3b8TN1XfkwXpTlAm0Op0Ojx6xAaFnPdPOcg8iCRG
MZ2yUjnytI5eCpWen9Hi1Kfx1YF41SYJq+lMIXcskHMvhsqcUvnYu1/2ainYt2pYzRFtG6RR/CPS
mLk50mqKNNReAlLeIFE1M3fRoMJW1hdERMwzudS5ibApVuWuHNV52HfnLlqde4K0vw/touUkEtZb
lHu60Bxl+Brs6ryPqGVA16PL+xQ2ktU1WU9pNDFMmxqJVfcBCVTe6FYUZ2Fhwf7cwvGIg01hHGJh
FRZoDh3zT+i6Lb70eMOqg7kfJRTzaHm3sT/l41Cm+5KIX9kiFpwCgfsBIaBU7nRbKhfD0yJNsR4P
Pp69759ulp5G6X8+XL37+8XgQx8XPxXYVjUsYV0mzCSUaD5bS4qM0p94xmXyiU6VvMsBESjuArpZ
YdLg5UtAqaRJfG+xivEoLkymUEl8suLBtVH6voyjFxjNMfFBEuQqeEzsXIxkD0nyW3OQ5fU+NoEq
Nsp0sY/SKTaIx2I6S6kPHGkHS9l+/oURfiMhtUlYqLZDnFUvLDBlK2ZMhHtjaU18gbFQRBlK4khW
RqigR/QSrXS8Axo6LR6OsZqYJfFMtTyU2IsXw6OqMuMD2oTL6OKT+PoX5KR2PgcEKj9l56aIhPzc
/WqONBEtHpW2WNdmUMyFnKJ6vUUxpciPAt1WflGr4PRUuRkUJS+lAbKdr19VDaHKHELik/A2kKzk
pfHQqzEMjyYQDuFCGSG6SkSS+vp1aUJMg6ILcRaKjYJGuo4sT7mCjytxuwglMfTg8BByjPldMwb+
qqQNR0fZOXYcZB02SdabJxT4M5UpsGS1CtNe9RYEFK51KsPJvHx+EkhVZYkLR5X+qQzD5oQP9TW9
Fahxy4wvqyA2czqzoKjBMvUOs5PdJmyUmAOoNh7Fk9Jf5JHaxRnDuokuh6c3XpBgxbM1oepR5wEd
JxRXbqdaZNfEl2A/q016qLC0+gfPgVlClW7ez6PVF3JMC/Hc9W6rzK+GdEgYf0CzUNbK8gIKfozx
Ao2e2yW1u0atfVLQeRAPqWyVECgfy8HhNfihPZY93FCHYCOoEHdrd13yHsu4ovzBQxAi24juilgy
AyrBYsHGvK6ktMy1I9JMaoKvXnfSQtLGeSjcSYxPhF1YVutndXi1YoL5Gk/96gj3sVLv2uDuWE/m
U18p+rqaZzFc+1OVxSleRTEle2N0ZVvKiyQQU083ibBvb+BVdYOCKvwJ6PcwQIo3StBQvLyA76+O
+CBZg6t2d8Kk4vMxYrV6TPhVMdxYpE9l+LuiPwqykBknK+qDrmU42anEKBp06uydIzQXqPhQuYSK
p2WqI1bgkbyzePcH3bfsDeiYmLVraiMeUHs21M1N55W7u6EBQUyldJYjA+kRvEQuc5gg9D8ZpVs7
SNffjkC4bEas5psVUE/laVSfYGZLxUt5rXphB+THYRjfSlxkbjGJk1bNgY0pnbBuTa5SAiZtOUGk
UwxQpzFKa4G0cJZruK9ePUN+4AJLBzcdZrco0WmrpuQFbL0kp+91yGFgBOcajq8P+I0UnWz+sVVw
rSzzSm3qwS6LZzk1fn5ZzyzwFb0kqxJittQf0bsNpdvHo7U3/gpofEsuLTV9hnHo/0EQqlTAwNKv
adYM49+CnxD9ZC0AAA==
EOF_B64
sha256sum $D/test.patch.gz        # 12de763b9d012c98a7c240505105f0720c4b5c07252a72ccef799d8782d78439
gunzip $D/test.patch.gz && sha256sum $D/test.patch   # a5b0f1c8c998810c251cd36241f5b13fb4bed7600d7ad152ab59ae31aed5b92f
grep -c '^diff --git' $D/test.patch   # 8
```

Stop and report if a hash or the count differs. Then the fail-before tree (N-01 + the test commit, the image added to it):

```sh
D=/var/tmp/task-1121-sat; I=/var/log/task-1121-sai/setattr_size.dat.bz2
git clone -q https://github.com/openzfs/zfs $D/fb-dbg && cd $D/fb-dbg
git fetch -q https://github.com/tigerblue77/zfs zinject-delay-points-v2 && git checkout -q FETCH_HEAD
echo "$(git rev-parse HEAD) $(git rev-parse HEAD~4) $(git status --porcelain | wc -l)"
#   4a98dfeb57bddb64160b6c39f111e2e46b971ea6 a1daebfec3f5ca46ac261ef339efdf0dbeae997c 0
git -c user.name=t -c user.email=t@t am -q $D/test.patch
mkdir -p tests/zfs-tests/tests/functional/slog/blockfiles
cp $I tests/zfs-tests/tests/functional/slog/blockfiles/setattr_size.dat.bz2
git add tests/zfs-tests/tests/functional/slog/blockfiles/setattr_size.dat.bz2
git -c user.name=t -c user.email=t@t commit -q --amend --no-edit
git rev-parse HEAD^{tree}          # d32bdf97254ee99e321afa08b351fb4cd89a9361
git log --oneline -2
#   <new sha> ZTS: replay a TX_SETATTR stopped after its size change
#   4a98dfeb57b ZTS: free a dataset while its upgrade task returns
grep -c 'zfs_freesp(zp, vap->va_size, 0, 0, TRUE);' module/os/linux/zfs/zfs_vnops_os.c                     # 1 (master's call)
grep -c 'ZIO_INJECT_POINT_SETATTR_SIZE_CHANGED' module/os/linux/zfs/zfs_vnops_os.c include/sys/zio.h module/zfs/zio_inject.c   # 1, 1, 1
```

The tree with the fix (N-01 + #19985's commit + the same test commit):

```sh
D=/var/tmp/task-1121-sat
T=$(git -C $D/fb-dbg rev-parse HEAD)
git clone -q $D/fb-dbg $D/fix-dbg && cd $D/fix-dbg
git checkout -q 4a98dfeb57bddb64160b6c39f111e2e46b971ea6
git fetch -q https://github.com/tigerblue77/zfs setattr-replay-one-record
test "$(git rev-parse FETCH_HEAD)" = d8f94d6359507ce38a796b3cfe1c9b65a8f2dbdb && echo fix head OK
git -c user.name=t -c user.email=t@t cherry-pick FETCH_HEAD $T
git rev-parse HEAD^{tree}          # c75c8296708d26e0646f726a47504b2ac034c930
grep -c 'zfs_freesp(zp, vap->va_size, 0, 0, !zfsvfs->z_replay);' module/os/linux/zfs/zfs_vnops_os.c        # 1 (the fix)
git clone -q $D/fb-dbg $D/fb-rel && git -C $D/fb-rel rev-parse HEAD^{tree}   # d32bdf97254ee99e321afa08b351fb4cd89a9361
```

Stop and report if a hash, a count or a subject differs, or if `git am` or `git cherry-pick` reports a conflict.

## STEP 2: builds (the three at once, about 40 minutes)

```sh
D=/var/tmp/task-1121-sat; L=/var/log/task-1121-sat
setsid nohup sh -c "cd $D/fix-dbg && ./autogen.sh && ./configure --enable-debug && nice make -j2; echo build-rc=\$?" > $L/build-fix-dbg.log 2>&1 < /dev/null &
setsid nohup sh -c "cd $D/fb-dbg && ./autogen.sh && ./configure --enable-debug && nice make -j2; echo build-rc=\$?" > $L/build-fb-dbg.log 2>&1 < /dev/null &
setsid nohup sh -c "cd $D/fb-rel && ./autogen.sh && ./configure && nice make -j2; echo build-rc=\$?" > $L/build-fb-rel.log 2>&1 < /dev/null &
```

Poll `grep build-rc= $L/build-*.log`. Each must end `build-rc=0`; quote every `warning:` line outside objtool notes (none expected). Note S1, S2, S3 = `modinfo -F srcversion <tree>/module/zfs.ko` for fix-dbg, fb-dbg and fb-rel; they must differ.

## STEP 3: part A, debug build with the fix

`sudo -n $D/fix-dbg/scripts/zfs.sh`; quote `cat /sys/module/zfs/srcversion` (must equal S1) and the "(DEBUG mode)" load line. Note N0 = `sudo -n dmesg | wc -l`.

1. The new test, three times, one after the other, each after the pre-run checks:

   ```sh
   D=/var/tmp/task-1121-sat; L=/var/log/task-1121-sat; n=1   # then 2, 3
   cd $D/fix-dbg && setsid nohup timeout 900 sudo -n -u zts ./scripts/zfs-tests.sh -vKO -t slog_replay_fs_setattr_size > $L/zts-a-$n.log 2>&1 < /dev/null &
   ```

   Expected PASS each time. For each run quote the result line and duration and, from the test's own log under the results directory the runner prints, every `NOTE:` line and the `FAIL`/`ERROR` lines if any: the record (`TX_SETATTR ... seq S`, `foid ..., mask 0x2069`), `After the first replay: ...`, `On disk: ZIL header: ...` and `After the second replay: ...`. Also quote, from dmesg since the previous count, every line with `VERIFY`, `ASSERT`, `PANIC`, `BUG`, `Oops`, `general protection` or `blocked for more than` (none expected). A FAIL: quote the last 40 lines of its log and of the test's own log, then go on to the next run; two FAILs in a row: stop part A and report.
2. `-t zinject_args` once (tag az), after the pre-run checks, as in 1: expected PASS; quote the result line.
3. `-T slog` once (tag as), after the pre-run checks: `cd $D/fix-dbg && setsid nohup sudo -n -u zts ./scripts/zfs-tests.sh -vKO -T slog > $L/zts-as.log 2>&1 < /dev/null &`. Expected: every test PASS, `slog_replay_fs_freesp_order` and `slog_replay_fs_setattr_size` among them. Quote every `Test:` line with its result and duration and the Results Summary. A FAIL is quoted with the 20 lines before it and reported as is, without rerun, saying whether `tests/test-runner/bin/zts-report.py.in` lists it as known.

After them: the pre-run checks once more; every line of `sudo -n dmesg | tail -n +$((N0+1))` matching `VERIFY|ASSERT|PANIC|BUG|Oops|blocked for more than|list_del` is quoted with 10 lines before it (none expected). Unload with `sudo -n $D/fix-dbg/scripts/zfs.sh -u` (quote rc) and save `sudo -n dmesg > $L/dmesg-fix-dbg.txt`.

## STEP 4: part B1, fail-before, release build

`sudo -n $D/fb-rel/scripts/zfs.sh`; srcversion equal to S3 and a load line WITHOUT "(DEBUG mode)". Note N1. The test three times (tags b-1 .. b-3), exactly as in step 3.1 but from `$D/fb-rel` and into `$L/zts-b-$n.log`. Expected FAIL each time, in well under a minute, as described in "Why"; quote the same lines as in part A plus the `ERROR`/`FAIL` line of the test's log, and check that the `replay_seq` on the `On disk:` line equals the `seq` of the record. A run that PASSes, hangs or panics is a finding: capture as in the VM rules, stop and report. After the three: the pre-run checks once more (the test's cleanup must have destroyed `testpool_setattr`), the dmesg check since N1, unload (quote rc), `sudo -n dmesg > $L/dmesg-fb-rel.txt`.

## STEP 5: part B2, fail-before, debug build

`cd $D/fb-dbg && sudo -n ./scripts/zfs.sh`; srcversion equal to S2 and "(DEBUG mode)"; note N2. Start a capture flushed line by line that survives your commands: `L=/var/log/task-1121-sat; setsid nohup sudo -n sh -c "dmesg --follow > $L/panic-follow.txt" > /dev/null 2>&1 < /dev/null &`. Then `touch $L/fb-start`, write `$L/progress.md` ("step 5 run started, reboot follows") and arrange your resumption now.

Run the test ONCE, after the pre-run checks, detached:

```sh
D=/var/tmp/task-1121-sat; L=/var/log/task-1121-sat
cd $D/fb-dbg && setsid nohup timeout 900 sudo -n -u zts ./scripts/zfs-tests.sh -vKO -t slog_replay_fs_setattr_size > $L/zts-c.log 2>&1 < /dev/null &
```

Poll dmesg every 10 s for at most 5 minutes. Expected within a minute: `VERIFY(zh->zh_replay_seq < *replayed_seq) failed` and `PANIC at zil.c:4274:zil_sync()`. Quote both lines and the first 15 frames of the call trace that follows them (expected: `spl_panic`, `zil_sync`, `dmu_objset_sync`, `dsl_dataset_sync`, `dsl_pool_sync`, `spa_sync`, `txg_sync_thread`). Then wait 2 more minutes and capture into `$L/fb-state.txt`: `ps -eLo pid,tid,stat,comm,wchan:32,args | grep -E ' D|zpool|zfs|zfs-tests|ksh'`, the kernel stacks of every D task, `sudo -n tail -n 60 /proc/spl/kstat/zfs/dbgmsg` and `sudo -n dmesg | tail -n 200`; save `sudo -n dmesg > $L/dmesg-fb-dbg.txt`. Quote the D tasks (command and wait channel; the `zfs mount` of the test and txg_sync are expected among them) and the last lines of the test's own log (expected to end at `zinject -c`). Do not wait for the runner; if it has returned, quote its result line. No panic within 5 minutes: capture the same, wait for the runner (at most 15 minutes in all) and report its result and the test's own log: the scenario did not occur.

## STEP 6: reboot and end

`sync -f $L/progress.md`, then the sysrq reboot through a transient unit (the one pre-approved reboot). After it: `lsmod | grep -E '^(zfs|spl) '` empty; `sysctl kernel.panic_on_oops kernel.hung_task_panic` back to their step 0 values; `find /var/tmp -mindepth 1 -maxdepth 1 -newer $L/fb-start -printf '%M %u %s %TT %p\n'` and `ls -ld /testpool_setattr 2>&1` (quote). The hung run cannot clean up, so these are expected among them: `disk-slog` (the test's copy of the image), `file-vdev0` to `file-vdev2`, possibly `testdir` and `constrained_path.*`, and the empty mount point directories `/testpool_setattr` and `/testpool_setattr/fs`. Remove exactly those of them that the listing shows (`sudo -n rm -rf` with their names; for `/testpool_setattr`, only if `find /testpool_setattr -type f | wc -l` prints 0); anything else it lists is reported and left, `/var/tmp/test_results` stays. `ls -l /etc/zfs/zpool.cache 2>&1`: absent expected (the test imports with `cachefile=none`); anything else is reported and left. Then `rm -rf $D`; `sudo -n fstrim -av`; `df -h /var/tmp`. Keep `/var/log/task-1121-sai`.

## Report

Write `$L/report.md` and give the same text as your final message. Part A: a table (run, result, duration, the record's seq and mask, the mtime T of both `After` lines, the `replay_seq` on disk), then the `zinject_args` result, the `-T slog` group (every `Test:` line, Results Summary, every non-PASS with its lines), then "no VERIFY/ASSERT/PANIC/BUG/Oops/hung task" or what was seen. Part B1: the same table plus the failure line of each run. Part B2: the assertion and PANIC lines, the first 15 frames, the D tasks, the last lines of the test's log, whether the runner returned, the reboot (time, how you resumed), what was removed. Srcversions and load lines of the three builds. One-line verdict: "SAT PASS 3/3 debug with the fix, zinject_args PASS, -T slog PASS; fail-before release FAIL 3/3 (mode 4777, replay_seq = record seq); fail-before debug VERIFY in zil_sync confirmed" or what differs.
