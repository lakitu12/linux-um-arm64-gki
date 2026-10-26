/* SPDX-License-Identifier: GPL-2.0 */
/*

 * Copyright (C) 2015 Thomas Meyer (thomas@m3y3r.de)
 * Copyright (C) 2005 Jeff Dike (jdike@karaya.com)
 */

#ifndef __STUB_DATA_H
#define __STUB_DATA_H

#include <linux/compiler_types.h>
#include <as-layout.h>
#include <sysdep/tls.h>
#include <sysdep/stub-data.h>
#include <mm_id.h>

/* See the note in skas/mm_id.h: a sysroot's linux/compiler_types.h can shadow
 * the kernel's, leaving the attribute macros undefined for USER_CFLAGS objects.
 */
#ifndef __aligned
#define __aligned(x)	__attribute__((__aligned__(x)))
#endif
#ifndef __section
#define __section(x)	__attribute__((__section__(x)))
#endif
#ifndef __used
#define __used		__attribute__((__used__))
#endif

/*
 * The stub <-> kernel handoff word. The low bit names the side that currently
 * owns the CPU ("the child may run" / "the kernel may run"); ownership strictly
 * ping-pongs, so at any moment at most one side is waiting.
 */
#define FUTEX_IN_CHILD 0
#define FUTEX_IN_KERN 1
/*
 * ORed in by a waiter that is about to park in FUTEX_WAIT. A waker hands
 * ownership over with an atomic exchange and calls FUTEX_WAKE only if the old
 * value carried this bit: on this host a FUTEX_WAKE that actually has someone
 * to wake costs a voluntary context switch (the measured 37k voluntary
 * switches per benchmark run are exactly these), and even an empty one is a
 * syscall. When the peer was caught mid-spin, the wake is pure waste.
 *
 * The bit can go stale in one benign way: the waiter sets it just after the
 * waker's exchange already flipped ownership. The waiter then sees the flip in
 * the fetch_or's old value and never parks, but the bit stays set until the
 * next exchange clears it, costing that exchange one spurious FUTEX_WAKE.
 * Rare and harmless; a scheme that cleans the bit up would need a second
 * atomic on every wait, which is the common path.
 */
#define STUB_FUTEX_WAITER 2

/* The ownership half of the protocol word, waiter bit masked off. */
#define STUB_FUTEX_OWNER(v) ((v) & FUTEX_IN_KERN)

/*
 * 64 covers the cacheline size of every x86-64 and arm64 host this can run on
 * (Snapdragon's Cortex/Kryo cores included).
 */
#define STUB_FUTEX_ALIGN 64

/*
 * Adaptive-spin bookkeeping, one instance per side, each written only by its
 * owner (no atomics needed):
 *
 *  - streak: consecutive spins that observed the reply within the budget.
 *    While nonzero, keep spinning: the peer is answering fast.
 *  - backoff: after a miss, the number of upcoming waits that go straight to
 *    FUTEX_WAIT before the spin is probed again. Without this an idle or
 *    compute-heavy guest would burn the full budget on every single wait; with
 *    it the steady-state cost of a never-hitting spin is one budget per
 *    STUB_SPIN_BACKOFF waits, while a workload that turns syscall-heavy again
 *    is picked back up within that many waits.
 */
struct stub_spin_state {
	unsigned short streak;
	unsigned short backoff;
};

struct stub_init_data {
	int seccomp;

	unsigned long stub_start;

	int stub_code_fd;
	unsigned long stub_code_offset;
	int stub_data_fd;
	unsigned long stub_data_offset;

	unsigned long signal_handler;
	unsigned long signal_restorer;

	/*
	 * How many stub threads to spawn (docs §12.35).  The kernel decides:
	 * the pool is opt-in (stub_slots=), and a stub that spawned threads its
	 * mm will never hand work to would be paying host threads for nothing.
	 */
	int spawn_slots;
};

#define STUB_NEXT_SYSCALL(s) \
	((struct stub_syscall *) (((unsigned long) s) + (s)->cmd_len))

struct stub_data {
	long err;

	int syscall_data_len;

	/*
	 * Number of descriptors sent with this handoff (kernel-side, written
	 * before the word flips).  The stub reads it after waking and only
	 * calls recvmsg() when it is non-zero: a thread that recvmsg()s
	 * unconditionally can consume a message addressed to another slot,
	 * because a socket carries whatever the kernel sent next, not what
	 * this slot was promised.
	 */
	int syscall_fd_num;

	/*
	 * This slot's descriptor-passing socket, as the stub sees it.  Written
	 * once per slot before it parks, next to the sigaltstack.  One socket
	 * per slot is what keeps descriptor passing correct once several slots
	 * are in flight at the same time: with a single mm-wide socket the
	 * kernel's sendmsg() and the woken thread's recvmsg() are not paired,
	 * and two batches needing different descriptors could swap them --
	 * a silently wrong mapping, not a crash.
	 */
	int sock_fd;

	/*
	 * The handoff word, alone in a 64-byte slot. Both sides poll it now
	 * (see stub-futex.h); if it shared a cacheline with anything the peer
	 * writes -- syscall_data, signal, the spin state -- each such write
	 * would yank the line from the polling CPU and the spin would spend
	 * its budget on coherence misses instead of observing the flip. The
	 * explicit pad keeps the tail of the line empty; init_child_tracking()
	 * BUILD_BUG_ONs the isolation so a reorder here cannot silently undo
	 * it.
	 */
	unsigned int futex __aligned(STUB_FUTEX_ALIGN);
	unsigned char futex_pad[STUB_FUTEX_ALIGN - sizeof(unsigned int)];

	/*
	 * 256 leaves enough room for the fields around the array: 128 bytes
	 * above, the rest below. The static_assert on sizeof(struct stub_data)
	 * in mmu.c catches this reserve becoming too small on any page size
	 * or subarch, because the tail would then push the page-aligned
	 * sigstack into an extra page.
	 */
	struct stub_syscall syscall_data[(UM_KERN_PAGE_SIZE - 256) / sizeof(struct stub_syscall)] __aligned(16);

	/* data shared with signal handler (only used in seccomp mode) */
	short restart_wait;
	int signal;
	unsigned short si_offset;
	unsigned short mctx_offset;

	/*
	 * Spin budget in ticks of the timebase behind stub_cycles(), written
	 * once by the kernel side before the stub first runs; 0 means "park
	 * immediately" and is what seccomp_spin=0 and hosts without a usable
	 * constant-rate EL0 counter (x86) get. Ticks of wall time rather than
	 * an iteration count is deliberate: this class of SoC scales
	 * 300 MHz - 3.2 GHz, so a loop count would mean a 10x different
	 * duration depending on the governor's mood, while CNTVCT_EL0 runs at
	 * a constant rate regardless.
	 */
	unsigned int spin_ticks;
	struct stub_spin_state kern_spin;	/* written by the kernel side only */
	struct stub_spin_state stub_spin;	/* written by the stub only */

	/* seccomp architecture specific state restore */
	struct stub_data_arch arch_data;

	/*
	 * Stack for our signal handlers and for calling into .
	 *
	 * Sized from STUB_DATA_PAGES rather than fixed at one page: in SECCOMP
	 * mode the host writes a whole signal frame here, and on arm64 that
	 * frame is larger than a 4 KiB page. See the comment on STUB_DATA_PAGES
	 * in as-layout.h.
	 */
	unsigned char sigstack[(STUB_DATA_PAGES - 1) * UM_KERN_PAGE_SIZE]
		__aligned(UM_KERN_PAGE_SIZE);
};

/*
 * Slot control words, at the bottom of the slot's stack area (as-layout.h).
 *
 *   ctl  written by the stub thread, read by the kernel:
 *          IDLE     the thread has not reached its parking code yet
 *          SETUP    its own signal stack is installed, ready for work
 *          PARKING  it is about to make the trap that parks it
 *        The kernel only ever counts PARKING, so a slot that stopped in
 *        SETUP is visibly different from one that never started.
 *   go   written by the kernel, read by the stub thread: "this mm is ready
 *        for you".  A thread that parked before the initial handshake
 *        completed would be a stub trap the kernel is not waiting for -- the
 *        handshake looks only at slot 0 -- so the pool is released explicitly
 *        instead of racing the first trap.
 *
 * Plain words on purpose: both sides already order against the futex handoff,
 * and a stale read here costs a bounded spin, never a lost wakeup.
 */
#define STUB_SLOT_WORDS_AT(base) \
	((volatile unsigned int *)((base) + STUB_DATA_PAGES * UM_KERN_PAGE_SIZE))
#define STUB_SLOT_CTL_AT(base)	(STUB_SLOT_WORDS_AT(base))
#define STUB_SLOT_GO_AT(base)	(STUB_SLOT_WORDS_AT(base) + 1)
#define STUB_CTL_IDLE		0
#define STUB_CTL_PARKING	1
#define STUB_CTL_SETUP		2

/*
 * Slot @slot of mm @id: the slot region starts at STUB_DATA (the code page's
 * neighbour) and slots are STUB_SLOT_SIZE apart.  mm_id->stack is the kernel's
 * mapping of that region, so this is also the pointer the stub process sees.
 */
#define STUB_DATA_SLOT(id, slot) \
	((struct stub_data *)((id)->stack + \
			      (unsigned long)(slot) * STUB_SLOT_SIZE))

#endif
