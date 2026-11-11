# linux-um-arm64-gki

The Android 17 GKI kernel with `ARCH=um SUBARCH=arm64` ported onto it: a kernel
that runs as an ordinary aarch64 userspace process — in an Android app or an
`adb shell` — on the 6.18 baseline that current phones ship.

This is a kernel tree, not a patch set. The first commit is kernel/common
`android17-6.18` at 6.18.32; the port is 95 commits on top, so a clone
builds as it is. The tree is the standard kernel for the uml-android sandbox
(the bionic UML kernel binary and the stub that goes with it); the app side is
a separate repository.

SMP is on: the guest brings up one fewer CPU than its host (`ncpus=` overrides,
`NR_CPUS=16` is the ceiling) and runs one CPU on a host that can only do ptrace.
[`docs/smp.md`](docs/smp.md) has the backport, the numbers, and what it does not
speed up.

## Build

Needs the Android NDK's clang (CI pins **r28c**) and the usual kernel build
tools (`flex`, `bison`, `libelf-dev`, `zlib1g-dev`, `bc`, `cpio`). This is a
full kernel tree — 1.7 GB checked out — so a build box that only consumes it
can clone with `--depth=1`.

```sh
cd tools/um-arm64
NDK=/path/to/android-ndk-r28c \
  O=/tmp/um-gki618-build \
  ART=$PWD/../../../artifacts \
  bash harness/build-bionic.sh
```

Output: `artifacts/linux-bionic` (static aarch64, bionic API 30) and
`artifacts/stub_exe_bionic`, the stub that has to sit next to it. Both are
ordinary unprivileged processes — no root, no KVM, no device node.

## Page size

Guest pages default to **16 KB**, and a guest page can never be smaller than
its host's: a 4 KB guest does not boot on a 16 KB host, and it fails by hanging
rather than by saying so. 16 KB runs on both hosts — four host pages per guest
page at 4 KB, one-to-one at 16 KB.

The 4 KB variant is one fragment away:

```sh
EXTRA_CONFIG="config/page4k.config" bash harness/build-bionic.sh
```

Its loader-side requirement is per `PT_LOAD`:
`(p_vaddr - p_offset) % PAGE_SIZE == 0`. `-z max-page-size=4096` lets the
linker split a segment that is congruent modulo 4 KB but not 16 KB, and that
segment then maps where the program was not linked — it dies with SIGSEGV on a
16 KB guest and runs on a 4 KB one.

Debian and Ubuntu arm64 userlands do not need the 4 KB variant: glibc reads the
page size at runtime and runs on the 16 KB build as well.

## What the tree carries beyond the GKI tag

- the arm64 UML port itself: the subarch, the stub, syscall interception, the
  signal-frame and FP/SIMD fixes found on hardware, loadable modules, bionic
  builds;
- **SMP**, re-landed from the 6.19 series: up to `NR_CPUS=16` vCPUs,
  `ncpus=`-controlled, `stub_slots=` for threads of one process;
- **qcow2 root disks**: `ubd0=` takes a qcow2 image, read and write, refcounts
  and compressed descriptors on the read path;
- **umfb**, a framebuffer whose changed tiles are mirrored into a host file or
  fd, plus `tools/um-arm64/umview` to show it in an X11 or Wayland window — the
  same sink the app's display screen reads;
- **virtio-gpu** over virtio-uml's PCI transport, with a 2D backend
  (`ci/vugpu2d.py`) for the gate; **virtio-snd**;
- the console and `UML_HOST_TOP` fixes a guest X server and a multi-session
  loader need.

Why each of them looks the way it does is in the commit messages. Recurring
pitfalls for moving to a newer GKI drop are in
[`docs/gki-notes.md`](docs/gki-notes.md).

## Guest feature set

CI merges a fragment set for container workloads on every build: docker's
`check-config.sh` requirements, squashfs and FUSE for snap and flatpak, binder
and PSI for redroid-class guests, the display sink. A local build opts in with
`EXTRA_CONFIG`, one path per fragment, resolved from `tools/um-arm64/`:

```sh
EXTRA_CONFIG="config/docker.config config/docker-completions.config \
              config/snap-flatpak.config config/android-guest.config \
              config/umfb.config" \
  bash harness/build-bionic.sh
```

The rationale per fragment, what is deliberately left out, and in-guest
verification recipes are in [`docs/guest-features.md`](docs/guest-features.md).
`ci/cfg-check.py` audits every request line of every fragment symbol by symbol,
so a renamed or dependency-broken option fails a run by name instead of
surprising a device.

## Layout

- `tools/um-arm64/` — the harness (build and run scripts, probes, benchmarks),
  the Kconfig fragments, `umview`.
- `ci/` — the boot gates and their fixtures: `boot-gate.sh` (marker gate),
  `qcow2-gate.sh`, `umfb-gate.sh`, `virtio-gpu-gate.sh`, `cfg-check.py`. They
  run on an arm64 host and judge on a marker or on the sink itself, never on an
  exit status.
- `.github/workflows/build-um-gki618.yml` — build, assert the config, then boot
  the artifact on an arm64 runner. `pages4k`, `vt`, `virtiogpu`, `extra_config`
  and `run_boot` are dispatch inputs.
- `docs/` — [`gki-notes.md`](docs/gki-notes.md) (maintenance and rebasing),
  [`guest-features.md`](docs/guest-features.md),
  [`kernel-management.md`](docs/kernel-management.md) (what a guest kernel
  build has to keep, and how to check it), [`smp.md`](docs/smp.md), and
  [`upstream-port.md`](docs/upstream-port.md) (the port's own README, imported
  unchanged).

## Moving to a newer GKI drop

The base is one commit so that this stays a rebase:

```sh
# 1. import the new drop as one base commit (recipe: docs/gki-notes.md)
# 2. replay the port on top of it
git rebase --onto gki-next <old-base> main
```

Most of the commits only touch `arch/um`, `arch/arm64/um` and
`tools/um-arm64` and replay clean. The two groups that need attention are the
backports — 6.18 predates the upstream fix they carry, so they are dropped
once the new base has it — and the build fixes for warnings r28c turns on.
Both are listed in [`docs/gki-notes.md`](docs/gki-notes.md).

## Validation

Baseline `android17-6.18` at **6.18.32**, NDK **r28c**, 16 KB guest pages, on
an arm64 runner:

- **the tree as published**: build → marker, qcow2 root-disk and umfb gates
  all green
  ([41392909848](https://github.com/lakitu12/linux-um-arm64-gki/actions/runs/41392909848),
  2026-11-02). The marker gate boots the artifact twice and reads the CPU count
  back from the guest: plain (ptrace) clamped itself to 1 CPU of the runner's 4,
  `seccomp=on` brought up 3. `linux-bionic` sha256 `212772ea…ae499`,
  `stub_exe_bionic` `5ad0f1ee…cdfa7` — the stub carries the SMP stub pool now,
  so its hash changed with that landing and is byte-identical across builds of
  this tree; a different one means the stub sources changed;
- both page sizes with the guest feature set
  ([39015377404](https://github.com/lakitu12/linux-um-arm64-gki/actions/runs/39015377404) 16 KB,
  [39015169891](https://github.com/lakitu12/linux-um-arm64-gki/actions/runs/39015169891) 4 KB).
  Those are patch-stack runs from before this repository became the tree; a
  4 KB build of the tree itself has not been re-run.

On a phone (through the app's loader): `/proc/config.gz` carries the whole
feature set, `nproc` reports 7 vCPUs on marble's eight cores, `unshare -Urmpf`
returns 0, and docker 26.1.5 runs `hello-world` and publishes `nginx:alpine` on
the guest's own loopback, `overlay2` on cgroup v2.

## Credits & license

The arm64 UML port is by **Oleksii Zakharov**
([`zalexdev/linux-um-arm64`](https://github.com/zalexdev/linux-um-arm64)); the
commits in this tree keep their original authors (Oleksii Zakharov, Tiwei Bie,
Thomas Weißschuh, Marcel W. Wysocki, …). Everything is GPL-2.0-only, as is the
kernel this tree is based on.
