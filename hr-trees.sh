#!/bin/bash
# SPDX-License-Identifier: CDDL-1.0
#
# The code trees of the host rig's arms (arms.txt), fetched by commit,
# checked, patched and built for one kernel, kept in DIR across tasks.
#
#   hr-trees.sh fetch DIR        R M P A fetched and checked; RQ, PQ and P3W
#                                made from R and P with the kit's patches;
#                                trees already there are checked again, not
#                                made again
#   hr-trees.sh build DIR KDIR   release build, for the kernel headers KDIR,
#                                of every tree without DIR/NAME.stamp
#   hr-trees.sh check DIR        every tree against its commit, tree and
#                                stamp, and its built module against the stamp
#
# Every check prints "NAME what OK" or "NAME what FAIL ..."; a FAIL ends
# the command with status 1.
set -u
export LC_ALL=C

H=$(dirname "$(readlink -f "$0")")
OPENZFS=https://github.com/openzfs/zfs
FORK=https://github.com/tigerblue77/zfs
TAG_OBJ=f75f3256a0916c5ba090b088e6ea5a9528f6f956
# git am as the patches' author, unsigned, so that the commits are the ones
# below whatever the machine's git configuration
ID=(-c user.name=Tigerblue77
    -c user.email=37409593+tigerblue77@users.noreply.github.com
    -c commit.gpgsign=false)
# NAME BASE COMMIT TREE: BASE "-" is fetched; otherwise the kit's
# patches/<dir> applied on BASE give COMMIT.
LIST="R - 71a9f9578616a90c3c14bb59629fb4d31bfd68d1 d2a62ec4527f872ce840ba60f50248e66fcc080b
M - c913da86156eba3243cc1ef515e98d69887654a7 7e32938edf1a54c02519e2dc0104e9325b5a6f4d
P - 846b9194b4a3eb54f601c1bda0382b68d785b0ef 9a4214cf8f0887e110763ef53c666c8527c2ca9e
A - c9053179c6708aa0231f024a4e294a2a4f1026e8 836581447f1c5a7b2c431e798770c200351896f8
RQ R d14d9bebae01bc853919777c731cac4d416aefd1 e4620bb6a674b72ae1391f946e8c521b7eec7e0c
PQ P f8c8df27e4a729b99c2f0ebf7c517a39fb6a3861 0ee3a37d70b16485259adf6139684279a4b1666c
P3W R 2738e2883136b191618647b2cf1191df745b0e04 e9321e519235f9d63f8a3dd9cc5218a100734279"
RC=0

ok() { echo "$1 $2 OK"; }
fail() { echo "$1 $2 FAIL $3"; RC=1; }
same() {    # same NAME WHAT GOT WANT
    if [ -n "$3" ] && [ "$3" = "$4" ]; then ok "$1" "$2"; else
        fail "$1" "$2" "got '$3', want '$4'"; fi
}
patchdir() { case $1 in RQ) echo r244-rt ;; PQ) echo pr1-rt ;;
    P3W) echo p3w-244 ;; esac; }

entries() { echo "$LIST"; }

fetch() {
    local n b c t p
    mkdir -p "$D" || exit 1
    [ -d "$D/repo" ] || git init -q "$D/repo" || exit 1
    cd "$D/repo" || exit 1
    git fetch -q --no-tags --depth 1 $OPENZFS refs/tags/zfs-2.4.4 ||
        { fail R fetch "$OPENZFS zfs-2.4.4"; return; }
    same R tag-object "$(git rev-parse FETCH_HEAD)" $TAG_OBJ
    # the three fork commits in one fetch, as TASK-1118-VQ1: A~3 is M
    git fetch -q --no-tags --depth 4 $FORK \
        c9053179c6708aa0231f024a4e294a2a4f1026e8 \
        846b9194b4a3eb54f601c1bda0382b68d785b0ef \
        c913da86156eba3243cc1ef515e98d69887654a7 ||
        { fail M,P,A fetch $FORK; return; }
    same P parent-of-A "$(git rev-parse c9053179c6708aa0231f024a4e294a2a4f1026e8~1)" \
        846b9194b4a3eb54f601c1bda0382b68d785b0ef
    same M third-parent-of-A "$(git rev-parse c9053179c6708aa0231f024a4e294a2a4f1026e8~3)" \
        c913da86156eba3243cc1ef515e98d69887654a7
    while read -r n b c t; do
        if [ "$b" = - ]; then
            same "$n" commit "$(git rev-parse --verify -q "$c^{commit}")" "$c"
            same "$n" tree "$(git rev-parse --verify -q "$c^{tree}")" "$t"
            [ -d "$D/$n" ] || git worktree add -q --detach "$D/$n" "$c" ||
                fail "$n" worktree "$D/$n"
        elif [ ! -d "$D/$n" ]; then
            p=$(patchdir "$n")
            git worktree add -q --detach "$D/$n" \
                "$(awk -v b="$b" '$1 == b {print $3}' <<< "$LIST")" ||
                { fail "$n" worktree "$D/$n"; continue; }
            (cd "$D/$n" && git "${ID[@]}" am -q --committer-date-is-author-date \
                "$H"/patches/"$p"/*.patch) || fail "$n" "git am" "patches/$p"
        fi
        same "$n" head "$(git -C "$D/$n" rev-parse HEAD 2>/dev/null)" "$c"
        same "$n" head-tree "$(git -C "$D/$n" rev-parse 'HEAD^{tree}' 2>/dev/null)" "$t"
        same "$n" clean "$(git -C "$D/$n" status --porcelain \
            --untracked-files=no | wc -l)" 0
    done < <(entries)
}

build() {
    local n b c t s rc k=$1
    [ -r "$k/Makefile" ] || { fail all kernel-headers "$k"; return; }
    while read -r n b c t; do
        [ -d "$D/$n" ] || { fail "$n" build "no tree $D/$n"; continue; }
        [ -f "$D/$n.stamp" ] && { ok "$n" "built before"; continue; }
        s=$(date +%s)
        (cd "$D/$n" && ./autogen.sh && ./configure --with-linux="$k" \
            --with-linux-obj="$k" && nice -n 10 make -j"$(nproc)") \
            > "$D/build-$n.log" 2>&1
        rc=$?
        echo "$n build rc=$rc $(( $(date +%s) - s )) s," \
            "$(grep -c 'warning:' "$D/build-$n.log") warning lines"
        [ $rc -eq 0 ] || { fail "$n" build "see $D/build-$n.log"; continue; }
        echo "$n commit $(git -C "$D/$n" rev-parse HEAD)" \
            "tree $(git -C "$D/$n" rev-parse 'HEAD^{tree}')" \
            "srcversion $(modinfo -F srcversion "$D/$n/module/zfs.ko")" \
            "vermagic $(modinfo -F vermagic "$D/$n/module/zfs.ko" | cut -d' ' -f1)" \
            "kdir $k" > "$D/$n.stamp"
    done < <(entries)
}

check() {
    local n b c t sv seen=" "
    while read -r n b c t; do
        [ -f "$D/$n.stamp" ] || { fail "$n" stamp "not built"; continue; }
        same "$n" stamp-commit "$(awk '{print $3}' "$D/$n.stamp")" "$c"
        same "$n" stamp-tree "$(awk '{print $5}' "$D/$n.stamp")" "$t"
        same "$n" head "$(git -C "$D/$n" rev-parse HEAD 2>/dev/null)" "$c"
        same "$n" clean "$(git -C "$D/$n" status --porcelain \
            --untracked-files=no | wc -l)" 0
        sv=$(modinfo -F srcversion "$D/$n/module/zfs.ko" 2>/dev/null)
        same "$n" srcversion "$sv" "$(awk '{print $7}' "$D/$n.stamp")"
        case $seen in *" $sv "*) fail "$n" srcversion "shared with another tree" ;; esac
        seen="$seen$sv "
        if [ ! -x "$D/$n/zpool" ] || [ ! -x "$D/$n/scripts/zfs.sh" ]; then
            fail "$n" tools "no zpool or scripts/zfs.sh"
        fi
        echo "$n $(cat "$D/$n.stamp")"
    done < <(entries)
}

usage() {
    echo "usage: hr-trees.sh fetch DIR | build DIR KDIR | check DIR" >&2
    exit 2
}

D=${2:-}
case ${1:-}.$# in
fetch.2) fetch ;;
build.3) build "$3" ;;
check.2) check ;;
*) usage ;;
esac
exit $RC
