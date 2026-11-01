# No SMP here

This tree is uniprocessor, because the 6.18 GKI baseline has no UML SMP and
bringing it back is a project of its own. The port's SMP work exists upstream
and is deliberately not in this series.

The five commits it would need, in addition to the upstream SMP series itself:

- `um/arm64: enable SMP` (with `rqspinlock.h` and an `smp.config`)
- `um: run on one CPU under ptrace instead of refusing to boot`
- `um: default ncpus to one less than the host has`
- `tools/um-arm64: add the SMP scaling benchmark`
- `um: fix the uniprocessor build of the ptrace CPU clamp`

The upstream work they build on:

- Tiwei Bie, *[PATCH v2 00/10] um: Add SMP support* — RFC to v2 through
  2025-08, merged for 6.19: 42 files, +827/−112, new `kernel/smp.c`,
  `os-Linux/smp.c`, `shared/smp.h`, `linux/smp-internal.h`; it touches signal,
  timer, tlb, trap, vdso and percpu.
- the SMP fixes that landed after 6.19 (the 7.1 TLB sync race among them) have
  to be evaluated with it.

So a full backport is a mid-sized second effort: ten-plus upstream commits
relocated onto this base, the five commits above, then build and hardware
validation. Worth its own branch rather than a corner of this one.

What it would buy a guest: more than one vCPU. Guest user code already runs at
native speed on the host CPU; what SMP changes is how much of that the kernel
lets a guest use at once.
