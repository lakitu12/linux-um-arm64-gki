# linux-um-arm64-gki

**um-arm64 on the Android 17 GKI kernel baseline** — a patch stack that ports
[`ARCH=um SUBARCH=arm64`](https://github.com/zalexdev/linux-um-arm64) (User-Mode
Linux on arm64) to the GKI tree
[`android17-6.18`](https://android.googlesource.com/kernel/common) (Linux 6.18
LTS), plus CI that applies the stack, builds the bionic UML kernel, and boots
it on an arm64 runner with a marker gate.

**UP-only**: UML SMP is not included — what it would take is quantified in
`docs/EXCLUDED-SMP.md`.

This repository is the **standard kernel for the uml-android sandbox** (the
bionic UML kernel plus its paired stub): pick the page-size variant for the
host — a guest page can never be smaller than its host, so a 4 KB host runs
either build and a 16 KB host needs the 16 KB one.  Root disks may be raw
ext4 or **qcow2** (the stack carries an in-kernel qcow2 engine for the ubd
backend, `0061–0063`).

The guest's screen can leave the VM as well: **umfb** (`0065–0067`, `0069`) mirrors
`/dev/fb0` into a host file or fd that viewers map — `tools/um-arm64/umview`
on a Linux desktop (X11 and Wayland sessions alike), the console app's
display screen on Android.

## Layout

- `patches/` — the 69-patch stack; `git am` in order onto `android17-6.18`.
  49 upstream-port patches (original authors/dates kept), 3 Android guest
  fixes, 8 backports/build fixes, 3 qcow2-engine patches, the loader/multi-
  session fix (`0064`), the display set (`0065–0067`, umfb + umview —
  `0069` puts the framebuffer under a platform device so Xorg's fbdev
  probe accepts it) and the VT coexistence fix (`0070`: keep the stdio
  console when CONFIG_VT owns the tty device space) — the why of each
  is in `docs/fixlog.md`.
- `ci/` — `apply-stack.sh` (stops on conflict, leaves a report),
  `boot-gate.sh` + `init.c` (tiny static initramfs, marker verdict),
  `qcow2-gate.sh` (qcow2 root-disk boot), `umfb-gate.sh` + `umfb-init.c` +
  `umfb-check.py` (display sink: the verdict is the sink file itself),
  `cfg-check.py` (per-symbol fragment audit), and the kconfig fragments this
  repo adds on top of the port's (`page4k`, `docker-completions`,
  `snap-flatpak`, `android-guest`, `umfb`).
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
| `extra_config` | extra kconfig fragments **appended to the default guest feature set** (space separated, resolved inside `tools/um-arm64/`) |
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
# 16 KB default, plus one extra fragment on top of the feature set
gh workflow run build-um-gki618.yml -f extra_config=config/usb-wifi.config
```

## Guest feature set (merged by default)

Every build merges the following fragments, so the artifact the app runs
carries the guests' feature set without a dispatch input:

| fragment | enables | consumers |
|---|---|---|
| `config/docker.config` (the port) | docker's `check-config.sh` set — namespaces, cgroups, overlayfs, veth/bridge/netfilter/nftables, seccomp, keys | docker in Linux guests |
| `ci/docker-completions.config` | companions so none of the port's requests silently drops (`NET_SCHED`, `NFT_FIB_IPV6`) | completeness |
| `ci/snap-flatpak.config` | squashfs (+ zlib/lzo/xz/zstd, xattr) for snap; `FUSE_FS` for flatpak's portals (also the Android sdcard daemon) | snap, flatpak, Android |
| `ci/android-guest.config` | binder + the `FTRACE`/`SCHED_TRACER` pair that brings `EVENT_TRACING` on, `PSI`, netd's remaining netfilter faces | redroid-class Android guests |
| `ci/umfb.config` | the display sink: `FB`/`FB_DEVICE` behind `UML_IOMEM_EMULATION`, plus `UML_FB` (dormant unless `umlfb=` is on the guest command line) | Linux guests (Xorg/weston on fbdev), Android guests (SurfaceFlinger/gralloc), the app's display screen |

The assert step audits all of it: a curated key list plus `ci/cfg-check.py`
over every request line of every fragment — a renamed or dependency-broken
symbol is a red run naming the symbol, not a device surprise.  Rationale,
per-fragment depth, and in-guest verification recipes: `docs/GUEST-FEATURES.md`.

## Validation status

- Baseline `android17-6.18` @ **6.18.32**; the stack applies 70/70 and the
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

- **Guest feature set validated (2026-10-17)**: both page-size variants were
  rebuilt with the default feature set merged and are green end to end —
  [39015377404](https://github.com/lakitu12/linux-um-arm64-gki/actions/runs/39015377404)
  (16 KB default; `linux-bionic` sha256 `f40f4088…04c7c4c8`,
  `stub_exe_bionic` `83f51f7c…0781fc` — the stub is byte-identical to the
  baseline above) and
  [39015169891](https://github.com/lakitu12/linux-um-arm64-gki/actions/runs/39015169891)
  (4 KB variant; `linux-bionic` `d24d6347…ef8e3377`,
  `stub_exe_bionic` `4972b0e3…2e27be`).  The assert step's per-symbol audit
  passed in CI (`cfg-check: ALL-GREEN` over all four fragments) and both
  artifacts passed the arm64 marker boot gate.
- **On a device** (marble, through the app repository's loader) the same
  binaries were exercised end to end: `/proc/config.gz` shows the whole
  26-key set, `unshare -Urmpf` rc=0, squashfs mounts through a loop device,
  bubblewrap sandboxes, and docker.io 26.1.5 runs — `hello-world` completes,
  `nginx:alpine` published on 8080 answers the guest's own loopback,
  storage driver `overlay2`, cgroup2 mounted (controllers cpu/io/memory/pids).
  The 16 KB build checks out the same way (`getconf PAGESIZE` 16384).
- Kernel-image growth note: this feature set pushed the kernel image end past
  the app-side loader's base; the loader was rebased (fix in the app
  repository) — see `docs/GUEST-FEATURES.md`, "Maintenance".
- **qcow2 root disks (2026-10-17, patches `0061–0063`)**: `ubd0=` boots a qcow2
  image directly (read/write engine + refcounts; compressed descriptors on the
  read path).  The boot job grew a qcow2 gate — verdict on the init marker AND
  the kernel's `qcow2 "…"` open line — green on the 16 KB default
  ([39030792171](https://github.com/lakitu12/linux-um-arm64-gki/actions/runs/39030792171),
  re-run [39036465898](https://github.com/lakitu12/linux-um-arm64-gki/actions/runs/39036465898))
  and on the 4 KB variant
  ([39033370998](https://github.com/lakitu12/linux-um-arm64-gki/actions/runs/39033370998)).

## Credits & license

- Upstream arm64 UML port by **Oleksii Zakharov**
  ([`zalexdev/linux-um-arm64`](https://github.com/zalexdev/linux-um-arm64));
  patches keep their original authors (Oleksii Zakharov, Tiwei Bie,
  Thomas Weißschuh, Marcel W. Wysocki, …).
- All patches are GPL-2.0-only, as is the kernel they apply to.
