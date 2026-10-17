#!/bin/bash
# qcow2-gate.sh <linux-binary> [out-dir]
#
# Boots the bionic UML kernel on THIS host (an arm64 runner) with a qcow2
# root disk instead of an initramfs: a minimal ext4 holding the same static
# /init from init.c, converted with `qemu-img convert -c` and attached as
# ubd0.  Two independent strings decide the verdict, both grepped from the
# saved log (never trusted to a pipeline or an exit status):
#   - the marker from /init (the disk booted and userland ran), and
#   - the kernel's own `qcow2 "..."` open line (the qcow2 engine, not some
#     other path, carried the disk).
# Host page size must be <= the guest's (16K); a 64K host cannot run this
# artifact -- an environment fact, not a regression: SKIPPED, exit 0.
set -u
BIN=${1:?usage: qcow2-gate.sh <linux-binary> [out-dir]}
OUT=${2:-qcow2-out}
HERE=$(cd "$(dirname "$0")" && pwd)

mkdir -p "$OUT"; OUT=$(cd "$OUT" && pwd)

# Resolve the kernel binary against the caller's cwd before any cd below
# (same reason as boot-gate.sh), and restore the execute bit artifact
# downloads lose.
BIND=$(cd "$(dirname "$BIN")" 2>/dev/null && pwd) || { echo "qcow2-gate.sh: no directory for binary: $1" >&2; exit 2; }
BIN="$BIND/$(basename "$BIN")"
[ -f "$BIN" ] || { echo "qcow2-gate.sh: no such binary: $1" >&2; exit 2; }
[ -x "$BIN" ] || chmod +x "$BIN"

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

# --- bake the root disk (native gcc + e2fsprogs + qemu-img on the runner) --
ROOT="$OUT/rootfs"
rm -rf "$ROOT"; mkdir -p "$ROOT"
gcc -O2 -static -o "$ROOT/init" "$HERE/init.c"
mke2fs -q -t ext4 -b 4096 -m 0 -d "$ROOT" -F "$OUT/rootfs.ext4" 16M
qemu-img convert -O qcow2 -c "$OUT/rootfs.ext4" "$OUT/rootfs.qcow2"
ls -l "$OUT/rootfs.qcow2"

# --- run ---
cd "$OUT"
export HOME="$OUT" TMPDIR="$OUT/tmp"
mkdir -p "$TMPDIR"
set -o pipefail
# fd1 is a pipe (via tee), not a regular file: UML registers its console
# with epoll and epoll_ctl on a regular file fails with EPERM.  The tee
# side carries its own backstop timeout so a stub that outlives its tracer
# cannot hold this step's pipe open.  rc is the kernel's, never the verdict.
timeout --signal=KILL 300 "$BIN" mem=512M ubd0="$OUT/rootfs.qcow2" \
    root=/dev/ubda rw init=/init panic=-1 con=null con0=null,fd:1 </dev/null 2>&1 \
    | timeout --signal=KILL 400 stdbuf -oL tee boot.log
rc=${PIPESTATUS[0]}
set +o pipefail
echo "kernel run rc=$rc (the verdict comes from the strings, not this)"

MARK=missing; grep -q 'UMARM_GKI618_BOOT_OK' boot.log && MARK=ok
QCOW=missing; grep -q 'qcow2 "' boot.log && QCOW=ok
echo "marker=$MARK qcow2_open_line=$QCOW"
if [ "$MARK" = ok ] && [ "$QCOW" = ok ]; then
    echo "verdict: PASS"
    echo pass >verdict.txt
    exit 0
fi
echo "verdict: FAIL — last log lines:"
tail -50 boot.log
echo fail >verdict.txt
exit 1