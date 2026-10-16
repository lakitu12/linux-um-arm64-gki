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

## Validation status

- Baseline `android17-6.18` @ **6.18.32**; the stack applies 60/60 and the
  bionic build is green with NDK **r28c** (upstream references r27c; r28c
  turns on extra warning flags, and the stack is fixed up for them).
- Reference artifacts: `linux-bionic` sha256 `03c3ef3b…a9410c`,
  `stub_exe_bionic` sha256 `83f51f7c…0781fc`. 16K pages by default: runs on
  4K and 16K host kernels alike.

## Credits & license

- Upstream arm64 UML port by **Oleksii Zakharov**
  ([`zalexdev/linux-um-arm64`](https://github.com/zalexdev/linux-um-arm64));
  patches keep their original authors (Oleksii Zakharov, Tiwei Bie,
  Thomas Weißschuh, Marcel W. Wysocki, …).
- All patches are GPL-2.0-only, as is the kernel they apply to.
