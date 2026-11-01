#!/bin/bash
# umfb-gate.sh <linux-binary> [out-dir]
#
# Boots the bionic UML kernel on THIS host (an arm64 runner) with a
# umfb sink, and a tiny static initramfs whose init probes /dev/fb0,
# writes a known pattern through the guest's mmap AND through pwrite,
# then powers off (ci/umfb-init.c).
#
# The verdict is evidence left behind, never an exit status:
#   - the boot log must carry the driver's banner and both init markers
#     (the sink was registered with the mode the boot line asked for),
#   - the sink file must carry the published header, a published seq and
#     the exact expected pixels (ci/umfb-check.py) -- the sink is what
#     viewers on every platform read, so the sink is what is checked.
#
# Host page size must be <= the guest's (16K); a 64K host cannot run
# this artifact -- an environment fact, not a regression: SKIPPED, exit 0.
set -u
BIN=${1:?usage: umfb-gate.sh <linux-binary> [out-dir]}
OUT=${2:-umfb-out}
HERE=$(cd "$(dirname "$0")" && pwd)

mkdir -p "$OUT"; OUT=$(cd "$OUT" && pwd)

# Resolve the kernel binary against the caller's cwd before any cd below
# (same reason as boot-gate.sh), and restore the execute bit artifact
# downloads lose.
BIND=$(cd "$(dirname "$BIN")" 2>/dev/null && pwd) || { echo "umfb-gate.sh: no directory for binary: $1" >&2; exit 2; }
BIN="$BIND/$(basename "$BIN")"
[ -f "$BIN" ] || { echo "umfb-gate.sh: no such binary: $1" >&2; exit 2; }
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

# --- bake the pattern initramfs (native gcc: the runner is aarch64) ---
ROOT="$OUT/rootfs"
rm -rf "$ROOT"; mkdir -p "$ROOT"
gcc -O2 -static -o "$ROOT/init" "$HERE/umfb-init.c"
(cd "$ROOT" && find . -print0 | cpio --null -o -H newc 2>/dev/null | gzip -9 >"$OUT/initramfs.cpio.gz")

# --- run ---
cd "$OUT"
export HOME="$OUT" TMPDIR="$OUT/tmp"
mkdir -p "$TMPDIR"
rm -f "$OUT/fb.raw"
set -o pipefail
# fd1 is a pipe (via tee), not a regular file: UML registers its console
# with epoll and epoll_ctl on a regular file fails with EPERM.  The tee
# side carries its own backstop timeout so a stub that outlives its tracer
# cannot hold this step's pipe open.  rc is the kernel's, never the verdict.
timeout --signal=KILL 300 "$BIN" mem=512M initrd="$OUT/initramfs.cpio.gz" init=/init \
    panic=-1 con=null con0=null,fd:1 \
    umlfb="$OUT/fb.raw:320x200-32,hz=60" </dev/null 2>&1 \
    | timeout --signal=KILL 400 stdbuf -oL tee boot.log
rc=${PIPESTATUS[0]}
set +o pipefail
echo "kernel run rc=$rc (the verdict comes from the evidence, not this)"

MARK=missing;  grep -q 'UMFB_GATE_DONE' boot.log && MARK=ok
WRITTEN=missing; grep -q 'UMFB_GATE_WRITTEN' boot.log && WRITTEN=ok
BANNER=missing; grep -q 'umfb: fb0 320x200-32' boot.log && BANNER=ok
echo "marker=$MARK written=$WRITTEN banner=$BANNER"

CHECK=missing
if [ -f "$OUT/fb.raw" ]; then
    python3 "$HERE/umfb-check.py" "$OUT/fb.raw" | tee "$OUT/check.txt"
    [ "${PIPESTATUS[0]}" = 0 ] && CHECK=ok
fi
echo "sink_check=$CHECK"

if [ "$MARK" = ok ] && [ "$WRITTEN" = ok ] && [ "$BANNER" = ok ] && [ "$CHECK" = ok ]; then
    echo "verdict: PASS"
    echo pass >verdict.txt
    exit 0
fi
echo "verdict: FAIL — last log lines:"
tail -50 boot.log
echo fail >verdict.txt
exit 1