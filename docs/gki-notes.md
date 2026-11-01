# Maintenance notes

What this tree is made of and what to expect when it moves to a newer GKI drop.
The commit messages carry the per-change detail; this page is the map.

## The base

`android17-6.18` at 6.18.32, imported as a single commit from the
googlesource archive (md5 `f2eac9986f130c17849eb07fd880487c`, 2026-10-16). The
archive has no history on purpose: kernel/common's is several gigabytes, and
nothing in this tree needs it.

To move to a newer drop, commit the new archive the same way on a branch and
`git rebase --onto` the port on top of it. Keeping the base to one commit is
what makes that a replay of 75 commits instead of a merge across two unrelated
histories.

## The 75 commits, in groups

| group | what it is | on rebase |
|---|---|---|
| the port | `um: arm64:` subarch, stub, syscall interception, signal-frame and FP/SIMD state handling, modules, bionic | replays; conflicts only where GKI itself touched `arch/um` or `arch/arm64` |
| found on hardware | pointer authentication in the stub, FP/SIMD across signals in SECCOMP mode, `PTRS_PER_PTE` from `PAGE_SHIFT`, 16 KB pages | keep |
| Android guest fixes | `hw_random` node name and mode (the `prng_seeder` boot race), top-down mmap and `mmap_rnd_bits` for Android init's `SetMmapRndBits` | keep; drop only if the base grows its own |
| backports | `__access_ok_vsyscall` removal (×2), `CMSG_DATA` rvalue, `elf_aux.o` unconditional, `os_futex_wait()`/`os_futex_wake()` | **drop when the base has the upstream commit** — these conflict by construction, they carry a fix 6.18 predates |
| build fixes for r28c | `CONFIG_FRAME_WARN=8192`, `arch_ptrace()` locals, the `text-patching.h` overlay, no-port-I/O → no `HAS_IOPORT_MAP` | redo against the new base: these exist because the newer clang enables warnings the old one did not |
| features | qcow2 ubd backend, `UML_HOST_TOP`, umfb + umview, virtio-gpu damage clips, VT ioctls on the stdio console, virtio-snd fragment | keep |

## Pitfalls that have cost a round already

- **`kconfig.h` has no trailing backslash in 6.18.** Appending `-include`
  through it silently breaks the file unless the last line gets one.
- **`asm-offsets.c` and the sysdep offset table differ between 6.18 and the
  port's target.** The port's shape (exceptions into `sysdep` includes, the
  `ARCH_ASM_OFFSETS()` macro, the port's 30-line x86 table) has to be carried
  over as a whole, not patched line by line.
- **`elf_aux.c` in 6.18 is the pre-`70d52694b6` shape** (32-bit auxv entries,
  vsyscall variables, an hwcap-clearing failure path). The port wants the
  64-bit auxv form with `AT_HWCAP2`, and the vsyscall variables kept as dead
  definitions rather than removed.
- **bionic needs `compiler_attributes.h` force-included** from
  `arch/um/Makefile`: the NDK sysroot shadows it.
- **The stub's shared prototypes need a `text-patching.h` overlay** for the
  arm64 subarch; without it the build stops on `-Wmissing-prototypes`.

## What "still working" means

Build, assert, boot. The config assert and the four gates in `ci/` are the
definition — they were written for failures that did not show up in a build:

- `boot-gate.sh` — marker from a static initramfs, then power off;
- `qcow2-gate.sh` — boots a qcow2 root disk and checks the kernel's open line;
- `umfb-gate.sh` — the verdict is the sink file: header the kernel published,
  pixels the guest wrote;
- `virtio-gpu-gate.sh` — driver bind line plus a create/transfer/flush cycle on
  the backend side and the painted pixels in the sink.

Run them on an arm64 host before and after a rebase; a rebase that builds but
fails a gate is not done.
