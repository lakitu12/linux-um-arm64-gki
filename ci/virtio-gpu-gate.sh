#!/bin/bash
# virtio-gpu-gate.sh <linux-binary> [out-dir]
#
# Boots the bionic UML kernel on THIS host (an arm64 runner) with a
# virtio-gpu device over virtio_uml's vhost-user socket, and a tiny static
# initramfs (ci/vgprobe-init.c) that reports what the guest's display stack
# became, paints two blocks through /dev/fb0's write path, and powers off.
#
# The counterpart is ci/vugpu2d.py: a 2D-only vhost-user-gpu backend listening
# on a unix socket.  virtio_uml.device=<sock>:16 makes the kernel connect to it
# and instantiate virtio-gpu; with no VIRGL feature the guest driver takes the
# 2D path, which is all this backend implements.
#
# The verdict is evidence, never an exit status:
#   - the boot log must carry the driver's bind line and the probe markers,
#   - the backend log must show a create/transfer/flush cycle,
#   - the sink must hold the painted pixels at the painted coordinates
#     (ci/vg-sink-check.py) -- that is the same sink the app's viewer reads.
#
# Host page size must be <= the guest's; a host that cannot run the artifact
# SKIPs (environment fact, not a regression) exactly like umfb-gate.sh.
set -u
BIN=${1:?usage: virtio-gpu-gate.sh <linux-binary> [out-dir]}
OUT=${2:-virtio-gpu-out}
HERE=$(cd "$(dirname "$0")" && pwd)

mkdir -p "$OUT"; OUT=$(cd "$OUT" && pwd)

BIND=$(cd "$(dirname "$BIN")" 2>/dev/null && pwd) || { echo "no directory for binary: $1" >&2; exit 2; }
BIN="$BIND/$(basename "$BIN")"
[ -f "$BIN" ] || { echo "no such binary: $1" >&2; exit 2; }
[ -x "$BIN" ] || chmod +x "$BIN"

PS=$(getconf PAGESIZE)
echo "host: $(uname -m), page size $PS, kernel $(uname -r)"
case "$PS" in
    4096|8192|16384) : ;;
    *)
        echo "SKIPPED: host page size $PS cannot run this guest here"
        echo skipped >"$OUT/verdict.txt"
        exit 0
        ;;
esac
case "$(uname -m)" in
    aarch64|arm64) : ;;
    *)
        echo "SKIPPED: the bionic artifact is arm64; host is $(uname -m)"
        echo skipped >"$OUT/verdict.txt"
        exit 0
        ;;
esac

# unix socket paths are limited to ~108 bytes; keep this out of the (long)
# runner workspace path.
SOCK="/tmp/uml-vgpu-$$.sock"
rm -f "$SOCK"

# --- bake the probe initramfs (native gcc: the runner is aarch64) ---
ROOT="$OUT/rootfs"
rm -rf "$ROOT"; mkdir -p "$ROOT"
gcc -O2 -static -Wall -I"$HERE" -o "$ROOT/init" "$HERE/vgprobe-init.c" || exit 2
(cd "$ROOT" && find . -print0 | cpio --null -o -H newc 2>/dev/null | gzip -9 >"$OUT/initramfs.cpio.gz")

# --- start the backend ---
rm -f "$OUT/vg-sink.raw"
python3 "$HERE/vugpu2d.py" --socket "$SOCK" --width 1024 --height 768 \
        --sink "$OUT/vg-sink.raw" >"$OUT/backend.log" 2>&1 &
BE=$!
trap 'kill $BE 2>/dev/null' EXIT

for _ in $(seq 100); do
    [ -S "$SOCK" ] && break
    sleep 0.1
done
if [ ! -S "$SOCK" ]; then
    echo "backend never created $SOCK" | tee "$OUT/verdict.txt"
    exit 1
fi
echo "backend listening on $SOCK"

# --- run the guest ---
cd "$OUT"
export HOME="$OUT" TMPDIR="$OUT/tmp"
mkdir -p "$TMPDIR"
set -o pipefail
timeout --signal=KILL 300 "$BIN" mem=512M initrd="$OUT/initramfs.cpio.gz" init=/init \
    panic=-1 con=null con0=null,fd:1 \
    virtio_uml.device="$SOCK":16 </dev/null 2>&1 | tee "$OUT/boot.log"
RC=$?
sleep 1
kill $BE 2>/dev/null
wait $BE 2>/dev/null

echo "--- kernel: virtio/drm lines ---"
grep -aiE "virtio|drm|fb0" "$OUT/boot.log" | head -40 || true
echo "--- backend ---"
cat "$OUT/backend.log"

# --- verdict ---
fail=0
chk() { if "$@" >/dev/null 2>&1; then echo "  ok   ${*: -1}"; else echo "  FAIL ${*: -1}"; fail=1; fi; }
grep -q "VG_GATE_INFO" "$OUT/boot.log" || { echo "  FAIL probe never saw /dev/fb0"; fail=1; }
grep -q "VG_GATE_PASS" "$OUT/boot.log" || { echo "  FAIL probe did not finish"; fail=1; }
grep -qi "virtio_gpu" "$OUT/boot.log" || { echo "  FAIL no virtio_gpu in the kernel log"; fail=1; }
grep -q "create_2d" "$OUT/backend.log" || { echo "  FAIL backend saw no RESOURCE_CREATE_2D"; fail=1; }
grep -q "flush" "$OUT/backend.log" || { echo "  FAIL backend saw no RESOURCE_FLUSH"; fail=1; }
python3 "$HERE/vg-sink-check.py" "$OUT/vg-sink.raw" || fail=1

if [ $fail = 0 ]; then
    echo "VERDICT PASS (kernel rc=$RC)" | tee "$OUT/verdict.txt"
else
    echo "VERDICT FAIL (kernel rc=$RC)" | tee "$OUT/verdict.txt"
fi
exit $fail
