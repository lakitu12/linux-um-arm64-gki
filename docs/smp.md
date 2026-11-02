# SMP

The guest boots more than one vCPU. `CONFIG_SMP=y` and `CONFIG_NR_CPUS=16` are
in the default build (`tools/um-arm64/config/smp.config`, merged by
`harness/build-bionic.sh`; `NO_SMP=1` opts out), and the count actually brought
up is the `ncpus=` boot argument: one fewer than the host has by default,
clamped to `NR_CPUS`. One binary therefore serves a four-core container and a
sixteen-core phone. A host that can only do ptrace userspace runs one CPU and
says so instead of refusing to boot:

    SMP is not supported with ptrace userspace; running on 1 CPU instead of the 7 requested

`NR_CPUS` is a compile-time ceiling for static arrays and cpumasks, not a
runtime count — raising it costs array size, not time.

## What it buys

Four CPU-bound guests measured 3.19–3.76× their serial time on marble (eight
cores, so seven vCPUs), and `/proc/stat` shows the busy jiffies on several CPUs.

Threads of one guest process stay serialised unless the stub pool is opened with
`stub_slots=N`; the pool is opt-in and defaults to 1. With `stub_slots=8` the
same-process four-thread probe measures 3.25–4.02× against 0.93–1.09× with the
pool off, on the same kernel, image and command line. The app domain is not
cleared for it yet — it reaches `graphical.target` but trips an RCU stall dump —
so the default stays 1.

## What it does not buy

- **Guest block I/O.** ubd completions arrive as SIGIO, which is
  process-directed, so the host delivers them to the thread group leader —
  CPU0. A parallel `docker build` therefore scales less than the compile phase
  inside it; `harness/smpbench.sh` measures both, and the gap is that effect.
- **Single-threaded guests.** A slot is a host thread and therefore a host
  core, so spreading one guest thread over the pool measured 4.3× slower than
  keeping it where it started; slots are sticky per task.

## Where it came from

The 6.18 GKI line has no UML SMP at all: `arch/um/Kconfig` pinned `NR_CPUS` to
`range 1 1` and `arch/um/kernel/smp.c` did not exist. This tree re-lands the
series that went into 6.19 by hand, squashed into one commit because it needed
conflict resolution rather than a cherry-pick:

    um: Do not disable kmalloc in initial_thread_cb()
    um: Turn signals_* into thread-local variables
    um: Define timers on a per-CPU basis
    um: Determine sleep based on need_resched()
    um: Add initial SMP support

On top of it the tree carries what those patches assume: signals state per host
thread, per-CPU POSIX timers with `SIGEV_THREAD_ID`, idle sleep on a signalfd so
an IPI can carry data. Then the arm64 subarch half (select
`UML_SUBARCH_SUPPORTS_SMP`, `arch/arm64/um/asm/rqspinlock.h` for the BPF
rqspinlock, which would otherwise want the architected timer's event stream), the
`ncpus` default with its ptrace clamp, the stub pool, and two build fixes a clean
`O=` needed — `asm-offsets.h` reached from its own generator, and the
`PM_SLEEP` tracepoint exports that had only been unconditional because UML
always had suspend.

## Checking a kernel

CI asserts `CONFIG_SMP`, `CONFIG_NR_CPUS=16` and `UML_SUBARCH_SUPPORTS_SMP`, and
the marker gate boots the artifact twice — plain, which is ptrace and must clamp
to one CPU, and `seccomp=on`, which must bring up more than one on a multi-CPU
host. It reads `UMARM_GKI618_CPUS=<n>` out of the guest and compares it with the
mode the kernel printed, so neither a SECCOMP boot with one CPU nor a ptrace
boot with several passes. On a device, `nproc`, `/proc/cpuinfo` and
`/sys/devices/system/cpu/online` are the same check, and `harness/smpbench.sh`
produces the scaling numbers above.
