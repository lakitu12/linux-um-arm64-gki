#!/bin/bash
# boot-gate.sh <linux-binary> [out-dir]
#
# Boots the bionic UML kernel on THIS host (an arm64 runner) with a tiny
# static initramfs whose init prints a marker and powers off. The verdict
# is the marker string in the boot log, never the exit status — same
# discipline as harness/boot.sh carries on the device.
#
# Host page size must be <= the guest's (16K); a 64K host cannot run this
# artifact, which is an environment fact, not a regression: SKIPPED, exit 0.
set -u
BIN=${1:?usage: boot-gate.sh <linux-binary> [out-dir]}
OUT=${2:-boot-out}
HERE=$(cd "$(dirname "$0")" && pwd)

mkdir -p "$OUT"; OUT=$(cd "$OUT" && pwd)

PS=$(getconf PAGESIZE)
echo "host: $(uname -m), page size $PS, kernel $(uname -r)"
case "$PS" in
    4096|8192|16384) : ;;
    *)
        echo "SKIPPED: host page size $PS cannot run a 16K-page guest here"
        echo skipped >"$OUT/verdict.txt"
        exit 0
        ;;
esac

# --- bake the marker initramfs (native gcc: the runner is aarch64) ---
ROOT="$OUT/rootfs"
rm -rf "$ROOT"; mkdir -p "$ROOT"
gcc -O2 -static -o "$ROOT/init" "$HERE/init.c"
(cd "$ROOT" && find . -print0 | cpio --null -o -H newc 2>/dev/null | gzip -9 >"$OUT/initramfs.cpio.gz")

# --- run ---
cd "$OUT"
export HOME="$OUT" TMPDIR="$OUT/tmp"
mkdir -p "$TMPDIR"
set -o pipefail
# fd1 is a pipe (via tee below), not a regular file: UML registers its
# console with epoll and epoll_ctl on a regular file fails with EPERM,
# which stops the console. Pipe through tee, like harness/boot.sh does.
timeout 600 "$BIN" mem=512M initrd="$OUT/initramfs.cpio.gz" init=/init \
    panic=-1 con=null con0=null,fd:1 2>&1 | tee boot.log
rc=$?
set +o pipefail
echo "kernel run rc=$rc (the verdict comes from the marker, not this)"

if grep -q 'UMARM_GKI618_BOOT_OK' boot.log; then
    echo "verdict: PASS"
    echo pass >verdict.txt
    exit 0
fi
echo "verdict: FAIL — last log lines:"
tail -50 boot.log
echo fail >verdict.txt
exit 1