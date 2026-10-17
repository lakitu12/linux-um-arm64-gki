# linux-um-arm64-gki

**um-arm64 on the Android 17 GKI kernel baseline** — a patch stack that ports
[`ARCH=um SUBARCH=arm64`](https://github.com/zalexdev/linux-um-arm64) (User-Mode
Linux on arm64) to the GKI tree
[`android17-6.18`](https://android.googlesource.com/kernel/common) (Linux 6.18
LTS), plus CI that applies the stack, builds the bionic UML kernel, and boots
it on an arm64 runner with a marker gate.

**UP-only**: UML SMP is not included — what it would take is quantified in
`docs/EXCLUDED-SMP.md`.

## Layout

- `patches/` — the 60-patch stack; `git am` in order onto `android17-6.18`.
  49 upstream-port patches (original authors/dates kept), 3 Android guest
  fixes, 8 backports/build fixes — the why of each is in `docs/fixlog.md`.
- `ci/` — `apply-stack.sh` (stops on conflict, leaves a report),
  `boot-gate.sh` + `init.c` (tiny static initramfs, marker verdict).
- `.github/workflows/build-um-gki618.yml` — apply → build → boot; the
  scheduled run re-tests the stack against the branch tip monthly, so a GKI
  drift shows up as a red run carrying a conflict report.
- `docs/` — `fixlog.md` (9 build iterations, every root cause),
  `PORT-STATUS.md` (validated baseline + artifact sha256), `EXCLUDED-SMP.md`.

## Use it

```sh
git clone --depth=1 -b android17-6.18 \
    https://android.googlesource.com/kernel/common linux
bash ci/apply-stack.sh linux patches
cd linux/tools/um-arm64
NDK=/path/to/android-ndk bash harness/build-bionic.sh
```

Output: `linux-bionic` + `stub_exe_bionic` — static aarch64 binaries that run
as ordinary unprivileged processes (adb shell or an Android app; the app-side
work lives in a separate repository).

## Build variants (workflow_dispatch inputs)

| input | effect |
|---|---|
| `gki_ref` | kernel/common branch or tag the stack is applied to (default `android17-6.18`) |
| `extra_config` | extra kconfig fragments, space separated, resolved inside `tools/um-arm64/` |
| `pages4k` | build the **4 KB guest-page** variant: merge `ci/page4k.config`, assert `CONFIG_PAGE_SIZE_4KB` instead of `_16KB`, suffix the artifacts `-4k` |
| `run_boot` | run the arm64 marker boot gate (default on) |

The port defaults to **16 KB** guest pages (`patches/0025`): Android 15+ hosts are
16 KB, and a guest page can never be smaller than the host's. Build the 4 KB
variant when guest pages must match a 4 KB host exactly, or when a payload's
segments are not congruent modulo 16 KB. The loader's requirement is per
PT_LOAD: `(p_vaddr - p_offset) % PAGE_SIZE == 0`. `-z max-page-size=4096` lets
the linker split a segment that is congruent modulo 4 KB but not 16 KB, and
that segment then maps where the program was not linked (measured: a
bionic-static probe built that way dies with SIGSEGV 139 on a 16 KB guest and
runs on a 4 KB one, while its 16 KB-aligned twin runs on both; the old
4 KB-aligned `ctty` helper had exactly that shape). It must not be run on a
16 KB host:
the kernel refuses at boot (`os_early_checks`), which is correct, not a
regression.

Debian/Ubuntu arm64 userland does **not** need this variant: glibc queries the
page size at runtime and runs on the 16 KB build just as well (measured
2026-10-17 — Debian 12.15 and Debian 13.7 root disks both booted to a shell on
the 16 KB and the 4 KB kernels).

```sh
# 4 KB guest pages (artifact: um-gki618-4k-<run>, kernel + its stub)
gh workflow run build-um-gki618.yml -f pages4k=true
# 16 KB default, plus an extra fragment
gh workflow run build-um-gki618.yml -f extra_config=config/docker.config
```

## Validation status

- Baseline `android17-6.18` @ **6.18.32**; the stack applies 60/60 and the
  bionic build is green with NDK **r28c** (upstream references r27c; r28c
  turns on extra warning flags, and the stack is fixed up for them).
- Reference artifacts: `linux-bionic` sha256 `03c3ef3b…a9410c`,
  `stub_exe_bionic` sha256 `83f51f7c…0781fc`. 16K pages by default: runs on
  4K and 16K host kernels alike.
- CI end-to-end is green: apply → bionic build → **boot gate on a GitHub
  arm64 runner** (the built kernel boots to userspace, prints the marker
  and powers down; verdict PASS; the stub is bit-identical to the
  reference above) — run
  [38875955444](https://github.com/lakitu12/linux-um-arm64-gki/actions/runs/38875955444),
  2026-10-16.

## Credits & license

- Upstream arm64 UML port by **Oleksii Zakharov**
  ([`zalexdev/linux-um-arm64`](https://github.com/zalexdev/linux-um-arm64));
  patches keep their original authors (Oleksii Zakharov, Tiwei Bie,
  Thomas Weißschuh, Marcel W. Wysocki, …).
- All patches are GPL-2.0-only, as is the kernel they apply to.
