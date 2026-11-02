#!/bin/bash
# boot-gate.sh <linux-binary> [out-dir] [extra kernel arguments]
#
# Boots the bionic UML kernel on THIS host (an arm64 runner) with a tiny
# static initramfs whose init prints a marker and powers off.
#
# The verdict is the marker string in the boot log plus the CPU count the init
# reports, checked against the userspace mode the kernel printed: SECCOMP has
# to bring up more than one CPU when the host has more than one, and ptrace has
# to bring up exactly one (that is what its clamp promises). The exit status
# never decides anything — same discipline as harness/boot.sh carries on the
# device.
#
# Host page size must be <= the guest's (16K); a 64K host cannot run this
# artifact, which is an environment fact, not a regression: SKIPPED, exit 0.
set -u
BIN=${1:?usage: boot-gate.sh <linux-binary> [out-dir] [extra kernel arguments]}
OUT=${2:-boot-out}
# Word-split on purpose: these are kernel command line arguments.
KARGS=${3:-}
HERE=$(cd "$(dirname "$0")" && pwd)

mkdir -p "$OUT"; OUT=$(cd "$OUT" && pwd)

# Resolve the kernel binary against the caller's cwd before any cd below:
# callers pass it relative to where they stand (ci/boot-gate.sh
# ci-artifacts/artifacts/linux-bionic boot-out). Artifact downloads store
# files 0644, so restore the execute bit the build had set.
BIND=$(cd "$(dirname "$BIN")" 2>/dev/null && pwd) || { echo "boot-gate.sh: no directory for binary: $1" >&2; exit 2; }
BIN="$BIND/$(basename "$BIN")"
[ -f "$BIN" ] || { echo "boot-gate.sh: no such binary: $1" >&2; exit 2; }
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
# The kernel is under its own timeout; the tee side carries a backstop so
# a stub that outlives its tracer cannot hold this step's pipe open
# (SIGPIPE reaps it). rc is the kernel's, never the pipeline's.
timeout --signal=KILL 600 "$BIN" mem=512M initrd="$OUT/initramfs.cpio.gz" init=/init \
    panic=-1 con=null con0=null,fd:1 $KARGS </dev/null 2>&1 | timeout --signal=KILL 700 stdbuf -oL tee boot.log
rc=${PIPESTATUS[0]}
set +o pipefail
echo "kernel run rc=$rc (the verdict comes from the marker, not this)"

if ! grep -q 'UMARM_GKI618_BOOT_OK' boot.log; then
    echo "verdict: FAIL — last log lines:"
    tail -50 boot.log
    echo fail >verdict.txt
    exit 1
fi

# Userspace ran.  What the CPU count should be depends on the mode the kernel
# reported: SECCOMP drives one host thread per vCPU, so ncpus= (default: one
# fewer than the host) has to show up; ptrace can only drive one, and the
# kernel promises to clamp rather than refuse to boot.  Checking the count
# against the mode is the point -- a build that says SECCOMP and brings up one
# CPU is broken, and so is one that says ptrace and brings up more.
CPUS=$(sed -n 's/.*UMARM_GKI618_CPUS=\([0-9][0-9]*\).*/\1/p' boot.log | tail -1)
HOSTCPUS=$(nproc)
USERS=$(sed -n 's/.*Userspace mode: \(SECCOMP\|ptrace\).*/\1/p' boot.log | tail -1)
verdict_fail() {
    echo "verdict: FAIL — $*"
    echo fail >verdict.txt
    exit 1
}
if [ -z "$CPUS" ]; then
    tail -20 boot.log
    verdict_fail "the gate init reported no usable CPU count"
fi
echo "userspace mode: ${USERS:-<none>}, guest CPUs: $CPUS (host: $HOSTCPUS)"
case "$USERS" in
SECCOMP)
    if [ "$HOSTCPUS" -ge 2 ] && [ "$CPUS" -lt 2 ]; then
        verdict_fail "SECCOMP mode brought up $CPUS CPU(s) on a $HOSTCPUS-CPU host"
    fi
    ;;
ptrace)
    if [ "$HOSTCPUS" -ge 2 ]; then
        [ "$CPUS" -eq 1 ] ||
            verdict_fail "ptrace mode brought up $CPUS CPUs; the clamp should leave 1"
        grep -q 'SMP is not supported with ptrace userspace' boot.log ||
            verdict_fail "ptrace mode without the clamp message — ncpus= was not honoured as a request"
    fi
    ;;
*)
    verdict_fail "no 'Userspace mode' line in the boot log"
    ;;
esac

echo "verdict: PASS"
echo pass >verdict.txt
exit 0
