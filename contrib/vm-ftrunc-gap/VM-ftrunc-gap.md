# VM: does a write that lands inside ftruncate's (or a punch's) logging window replay wrongly?

Disposable Debian 13 VM only (module reloads, pools created and destroyed).
Not run yet. Two builds: A = upstream master `0cdf531257`, B =
PR #19344 head `f37ff01a35`, each with `ftrunc-delay.patch` (experiment only,
never for upstream): two module parameters that sleep inside the window the
code leaves open,

- `zfs_setattr_size_delay_ms`: in `zfs_setattr()` after `zfs_freesp(log=FALSE)`
  has set the size and before the second tx logs `TX_SETATTR`;
- `zfs_freesp_log_delay_ms`: in `zfs_freesp()` after the range is freed (or
  the size set) and before the second tx logs `TX_TRUNCATE`.

Neither sleep holds a lock a write needs (the range lock is released, `z_lock`
is not held yet), so the write is free to run inside the window, as it is
without the patch, only for longer. Files (sha256):

- `ftrunc-delay.patch` ce1562fc43bf977015789b20ab3b44c9c811520010165c0861d6ffa5257ed062
- `ftrunc-gap.sh` d4468c388f9846e22b26b5dc7a86c7cfa1460d6dca2d4fd234c6908a76f8f997

## Steps

```sh
set -u
Z=/var/tmp/ftrunc-gap; L=/var/log/ftrunc-gap
sudo -n mkdir -p $L && sudo -n chown "$(id -un):" $L && mkdir -p $Z
lsmod | grep -E '^(zfs|spl) ' && echo "STOP: a zfs module is loaded"
cp ftrunc-delay.patch ftrunc-gap.sh $Z/   # transferred files
sha256sum $Z/ftrunc-delay.patch $Z/ftrunc-gap.sh   # must match the lines above
git clone https://github.com/openzfs/zfs $Z/src > $L/clone.log 2>&1
cd $Z/src && git fetch origin pull/19344/head > $L/fetch.log 2>&1
for b in A:0cdf531257e977b2f4b31a5823e7a5907c49d5a2 \
         B:f37ff01a3552e71c880ebff968cef92e227e845a; do
	n=${b%%:*}; c=${b#*:}
	git -C $Z/src worktree add --detach $Z/$n $c > $L/wt-$n.log 2>&1
	git -C $Z/$n apply $Z/ftrunc-delay.patch && echo "$n patched"
	(cd $Z/$n && nice -n 10 sh -c './autogen.sh && ./configure --enable-debug &&
	    make -j8') > $L/build-$n.log 2>&1; echo "build $n rc=$?"
done
for n in A B; do
	sudo -n $Z/$n/scripts/zfs.sh > $L/load-$n.log 2>&1 || { echo "load $n failed"; break; }
	ls /sys/module/zfs/parameters/zfs_setattr_size_delay_ms \
	    /sys/module/zfs/parameters/zfs_freesp_log_delay_ms || echo "STOP: not the patched module"
	for c in setattr-buffered setattr-direct punch-buffered; do
		for d in 1000 0; do
			sudo -n env PATH="$Z/$n:/usr/sbin:/sbin:/usr/bin:/bin" \
			    timeout -k 30 900 bash $Z/ftrunc-gap.sh $c $d 10 \
			    > $L/$n-$c-$d.log 2>&1
			echo "$n $c $d rc=$? $(grep -h '^RESULT\|SETUP ERROR' $L/$n-$c-$d.log)"
		done
	done
	sudo -n journalctl -k --no-pager | grep -ciE 'VERIFY|ASSERT|PANIC|BUG:|Oops'
	sudo -n $Z/$n/scripts/zfs.sh -u > $L/unload-$n.log 2>&1; echo "unload $n rc=$?"
done
```

Stop at the first `SETUP ERROR`, kernel `VERIFY`/`PANIC`, or failed unload, and
report. Never reboot without the maintainer.

## Reading the result

Each round prints `live <size>/<sha of block 1> replayed <size>/<sha> log
<order> write_returned_first <0|1>`; `RESULT ... same= gap= other=`.

| Outcome with delay 1000 | Meaning |
| --- | --- |
| `gap=10`, order `WS` (or `WT`), `write_returned_first 1`, live 262144 vs replayed 135168 (setattr), or same size but block 1 zeroed after replay (punch) | The gap is real: the write fsync'ed after the truncate/punch returned is cut or zeroed by replay |
| `same=10`, order `SW`/`TW`, `write_returned_first 0` | The write waited for the op: something serializes them that the code reading missed; the gap is not reachable |
| any `other` | Read the round lines; report them |

Delay 0 is the control: expected mostly `same` (the v3 run on 26 Oct saw
71 of 71 rounds logged truncate-first with O_DIRECT); a `gap` there shows the
window is reachable without the patch. Expected from reading: identical
results on A and B, since #19344 changes neither `zfs_setattr()` nor the
punch path of `zfs_freesp()`.
