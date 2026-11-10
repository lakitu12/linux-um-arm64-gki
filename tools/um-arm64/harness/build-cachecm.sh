#!/usr/bin/env bash
# Build the cachecm probe and a minimal initramfs holding just /init and
# /dev/console.
#
# Usage: NDK=/path/to/ndk bash harness/build-cachecm.sh /path/to/new-output-dir
#
# Only the probe and its initramfs are built here: no kernel, no device access.
set -euo pipefail

HERE=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
OUT=${1:?give a new output directory}
NDK=${NDK:-${ANDROID_NDK_HOME:-${ANDROID_NDK_ROOT:-}}}
[ -n "$NDK" ] || { printf 'set NDK to the Android NDK.\n' >&2; exit 2; }
CC="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/clang"
[ -x "$CC" ] || { printf 'no NDK clang at %s\n' "$CC" >&2; exit 2; }
mkdir -p -- "$OUT"
OUT=$(realpath -- "$OUT")
for name in cachecm cachecm-initramfs.img; do
    [ ! -e "$OUT/$name" ] || { printf 'refusing to overwrite %s\n' "$OUT/$name" >&2; exit 2; }
done

"$CC" --target=aarch64-linux-android30 -static -O2 -Wall -Wextra -Werror \
    -o "$OUT/cachecm" "$HERE/probe/cachecm.c"

# Write the newc archive directly: creating /dev/console otherwise needs root.
python3 - "$OUT" <<'PY'
import gzip
import pathlib
import stat
import sys

root = pathlib.Path(sys.argv[1])
archive = bytearray()
inode = 1

def entry(name, mode, data=b'', rmajor=0, rminor=0):
    global inode
    name = name.encode() + b'\0'
    fields = [inode, mode, 0, 0, 1, 0, len(data), 0, 0,
              rmajor, rminor, len(name), 0]
    archive.extend(b'070701' + ''.join(f'{x:08x}' for x in fields).encode())
    archive.extend(name)
    archive.extend(b'\0' * (-len(archive) % 4))
    archive.extend(data)
    archive.extend(b'\0' * (-len(archive) % 4))
    inode += 1

entry('dev', stat.S_IFDIR | 0o755)
entry('dev/console', stat.S_IFCHR | 0o600, rmajor=5, rminor=1)
entry('init', stat.S_IFREG | 0o755, (root / 'cachecm').read_bytes())
entry('TRAILER!!!', 0)
with (root / 'cachecm-initramfs.img').open('xb') as output:
    output.write(gzip.compress(bytes(archive), mtime=0))
PY

printf 'probe: %s\ninitramfs: %s\n' "$OUT/cachecm" "$OUT/cachecm-initramfs.img"
printf 'Only CACHECM_PASS is a pass: CACHECM_DONE and a clean shutdown are not.\n'
