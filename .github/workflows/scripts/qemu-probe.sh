#!/usr/bin/env bash

######################################################################
# Fork-only probe of the runner losses on AWS-hosted runners.
#
# called on runner by the probe branch's zfs-qemu.yml:
#   qemu-probe.sh host     print host facts, pick this copy's variant
#   qemu-probe.sh variant  apply the variant, record the disks after setup
#   qemu-probe.sh start    start the tests in the background
#   qemu-probe.sh slice N  sample the host each minute until minute N
#   qemu-probe.sh collect  last sample, dmesg and VM consoles
######################################################################

set -u

# The workflow uploads this directory; PROBE_DIR is for trying the script out.
DIR="${PROBE_DIR:-/var/tmp/probe}"
STATS="$DIR/stats.txt"
TESTS="$DIR/tests.txt"

# Print a command and its output; a command that fails is still a fact.
function show() {
  echo "\$ $*"
  "$@" 2>&1
  echo
}

# Cumulative I/O of the whole disks and of the device-mapper devices.
function disk_io() {
  awk '$3 ~ /^(dm-[0-9]+|nvme[0-9]+n[0-9]+|sd[a-z]+|vd[a-z]+)$/ {
    printf "%s read=%dMiB written=%dMiB inflight=%d\n",
      $3, $6 / 2048, $10 / 2048, $12 }' /proc/diskstats
}

# What "/" is made of, how full it is, and the files this workflow puts on
# it: the pool's file vdev and the swap file exist once Setup QEMU has run.
function disk_facts() {
  show findmnt -no SOURCE,FSTYPE,SIZE /
  show lsblk -o NAME,TYPE,SIZE,FSTYPE,MOUNTPOINT
  show df -h /
  show sudo dmsetup table
  show sudo dmsetup status
  show sudo ls -ls --block-size=M /test.ssd2 /swapfile.ssd
  show swapon --show
  show disk_io
}

function host_facts() {
  echo "copy=$PROBE_COPY image=${ImageOS:-?} ${ImageVersion:-?}"
  disk_facts
  show free -m
  show nproc
  show uname -r
  show cat /proc/cmdline
  show grep -m1 "model name" /proc/cpuinfo
  show grep -c -w -E "vmx|svm" /proc/cpuinfo
  show ls -l /dev/kvm
  show cat /sys/class/dmi/id/sys_vendor /sys/class/dmi/id/product_name
  show systemd-detect-virt
  show zramctl
  show grep -H . /sys/kernel/mm/ksm/run \
    /sys/kernel/mm/transparent_hugepage/enabled \
    /proc/sys/vm/swappiness /proc/sys/vm/overcommit_memory \
    /sys/module/kvm_intel/parameters/nested \
    /sys/module/kvm_amd/parameters/nested
  show grep -H . /sys/fs/cgroup/system.slice/memory.max \
    /sys/fs/cgroup/system.slice/memory.swap.max
}

# The variants other copies of this run took, one "claim-<variant>-<copy>"
# artifact each.
function claims() {
  local page
  for page in 1 2 3 4 5; do
    curl -fsS --retry 3 \
      -H "Authorization: Bearer $GH_TOKEN" \
      -H "Accept: application/vnd.github+json" \
      "$GITHUB_API_URL/repos/$GITHUB_REPOSITORY/actions/runs/$GITHUB_RUN_ID/artifacts?per_page=100&page=$page" \
      | jq -r '.artifacts[].name' | grep . || break
  done
}

function pick_variant() {
  local root claimed variants n k v count best="" least="$PROBE_PER_VARIANT"

  root=$(findmnt -no SOURCE /)
  if [ "$PROBE_HOST" != "any" ] && [ "$root" != "/dev/mapper/cow-root" ]; then
    echo "Root is on $root, not /dev/mapper/cow-root: nothing to probe."
    return 0
  fi

  claimed=$(claims)
  read -r -a variants <<< "$PROBE_VARIANTS"
  n=${#variants[@]}
  # Least-covered variant first.  Copies that start together and see the
  # same counts are spread by walking the list from their own offset.
  for ((k = 0; k < n; k++)); do
    v=${variants[$(((PROBE_COPY + k) % n))]}
    count=$(grep -c "^claim-$v-" <<< "$claimed")
    if [ "$count" -lt "$least" ]; then
      best="$v"
      least="$count"
    fi
  done
  if [ -z "$best" ]; then
    echo "Every variant has its $PROBE_PER_VARIANT jobs: nothing left to probe."
    return 0
  fi

  case "$best" in
    control|noksm)
      ;;
    ram[0-9]*)
      echo "PROBE_RAM_GIB=${best#ram}" >> "$GITHUB_ENV"
      ;;
    *)
      echo "Unknown variant '$best'"
      return 1
      ;;
  esac
  echo "PROBE_VARIANT=$best" >> "$GITHUB_ENV"
  echo "variant=$best (taken $least time(s) before this copy)" | tee -a "$DIR/host.txt"
}

function sample() {
  local start t seen total i
  start=$(cat "$DIR/start" 2>/dev/null || date +%s)
  t=$(($(date +%s) - start))
  {
    printf '==== t+%02d:%02d %s %s\n' $((t / 60)) $((t % 60)) \
      "$PROBE_VARIANT" "$(date -u +%FT%TZ)"
    cat /proc/loadavg
    free -m | sed -n '2,3p'
    grep -E "^(MemAvailable|Dirty|Writeback|AnonPages|SUnreclaim|PageTables|Committed_AS):" \
      /proc/meminfo | tr -s ' \n' ' '
    echo
    grep -H . /proc/pressure/memory /proc/pressure/io /proc/pressure/cpu
    grep -E "^(pswpin|pswpout|pgmajfault|oom_kill|allocstall_normal|pgscan_direct|ksm_swpin_copy|cow_ksm) " \
      /proc/vmstat | tr '\n' ' '
    echo
    (cd /sys/kernel/mm/ksm && grep -H . run full_scans pages_scanned \
      pages_shared pages_sharing pages_unshared pages_volatile) | tr '\n' ' '
    echo
    # A snapshot or thin device reports here how much of its real space
    # is used, which can be less than what df shows for "/".  A command
    # that hangs on a stalled disk or pool must not stop the sampling.
    sudo timeout 10 dmsetup status
    timeout 10 df -h / | tail -n 1
    sudo timeout 10 ls -ls --block-size=M /test.ssd2 /swapfile.ssd
    disk_io
    sudo timeout 10 zpool list -Hp -o name,size,alloc,free
    ps -eo rss,pcpu,etimes,comm --sort=-rss | sed -n '2,4p'
    for i in /var/tmp/test_results/vm*/console.txt; do
      [ -e "$i" ] && sudo timeout 10 tail -n 2 "$i" | cut -c1-160 |
        sed "s|^|${i#/var/tmp/test_results/}: |"
    done
    echo "-- dmesg"
    sudo timeout 10 dmesg | tail -n 20
  } 2>&1 | tee -a "$STATS"

  # What the tests printed since the last sample; all of it is in $TESTS.
  seen=$(cat "$DIR/tests.seen" 2>/dev/null || echo 0)
  total=$(wc -l < "$TESTS" 2>/dev/null || echo 0)
  echo "-- tests ($total lines)"
  tail -n +$((seen + 1)) "$TESTS" 2>/dev/null | tail -n 30
  echo "$total" > "$DIR/tests.seen"
}

mkdir -p "$DIR"

case "${1:-}" in
  host)
    host_facts 2>&1 | tee "$DIR/host.txt"
    pick_variant || exit 1
    ;;

  variant)
    case "$PROBE_VARIANT" in
      noksm)
        # qemu-2-start.sh has just started ksmd.  Writing 2 stops it and
        # unmerges what it merged; nothing starts it again.
        echo 2 | sudo tee /sys/kernel/mm/ksm/run > /dev/null
        ;;
    esac
    {
      echo "==== after Setup QEMU and Start build machine"
      echo "variant=$PROBE_VARIANT guest RAM=${PROBE_RAM_GIB:-default} GiB"
      grep -H . /sys/kernel/mm/ksm/run
      disk_facts
      show free -m
    } 2>&1 | tee -a "$DIR/host.txt"
    ;;

  start)
    date +%s > "$DIR/start"
    # qemu-6-tests.sh waits for the VMs to finish, so it is detached from
    # this step and the steps that follow watch the host instead.
    CI_TYPE="default" setsid nohup \
      .github/workflows/scripts/qemu-6-tests.sh > "$TESTS" 2>&1 < /dev/null &
    echo "Tests started in the background."
    ;;

  slice)
    start=$(cat "$DIR/start")
    end=$((start + $2 * 60))
    while :; do
      now=$(date +%s)
      [ "$now" -ge "$end" ] && break
      next=$((start + ((now - start) / 60 + 1) * 60))
      [ "$next" -gt "$end" ] && next="$end"
      sleep $((next - now))
      sample
    done
    ;;

  collect)
    sample
    sudo dmesg | tee "$DIR/dmesg.txt" > /dev/null
    for i in /var/tmp/test_results/vm*/console.txt; do
      [ -e "$i" ] && sudo cp "$i" "$DIR/$(basename "$(dirname "$i")")-console.txt"
    done
    sudo cp /var/tmp/vm*log.txt /var/tmp/stats.txt /var/tmp/env.txt "$DIR" 2>/dev/null
    sudo chown -R "$(id -u)" "$DIR"
    ;;

  *)
    echo "usage: $0 host|variant|start|slice N|collect"
    exit 1
    ;;
esac

# A failed sample must not end the probe.
exit 0
