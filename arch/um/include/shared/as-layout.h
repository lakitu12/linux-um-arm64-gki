/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2007 Jeff Dike (jdike@{addtoit,linux.intel}.com)
 */

#ifndef __START_H__
#define __START_H__

#include <generated/asm-offsets.h>

/*
 * Stolen from linux/const.h, which can't be directly included since
 * this is used in userspace code, which has no access to the kernel
 * headers.  Changed to be suitable for adding casts to the start,
 * rather than "UL" to the end.
 */

/* Some constant macros are used in both assembler and
 * C code.  Therefore we cannot annotate them always with
 * 'UL' and other type specifiers unilaterally.  We
 * use the following macros to deal with this.
 */
#define STUB_START stub_start
#define STUB_CODE STUB_START
/* STUB_DATA is defined below, after the slot geometry it depends on. */
/*
 * One page of bookkeeping plus a signal stack. In SECCOMP mode the host kernel
 * writes a complete signal frame onto that stack when it delivers SIGSYS to the
 * stub, so it has to be big enough for the largest frame this host can produce.
 *
 * On x86 one page is comfortable. On arm64 it is not: sizeof(mcontext_t) alone
 * is 4384 bytes, because the architecture reserves __reserved[4096] for the
 * FPSIMD/SVE/SME records, and a measured frame is 4576 bytes -- larger than a
 * 4 KiB page before siginfo is even counted. arm64 publishes the real
 * requirement in AT_MINSIGSTKSZ, which is 4720 on a plain Cortex-A76 and 9984
 * on a host with 512-bit SVE, so the size is a property of the host rather than
 * a constant that can be measured once.
 *
 * Four pages therefore, giving a 12 KiB stack at 4 KiB pages, which covers
 * every AT_MINSIGSTKSZ seen so far with room to spare. At 16 KiB pages a single
 * page is already larger than that, so the layout is unchanged there.
 *
 * A power of two is required, not merely tidy: init_new_context() allocates
 * this with __get_free_pages(..., ilog2(STUB_DATA_PAGES)), so a non-power-of-two
 * would silently allocate fewer pages than the structure occupies.
 *
 * The remaining risk -- a host whose AT_MINSIGSTKSZ exceeds even this -- is not
 * papered over: check_stub_sigstack() compares the two at boot and refuses
 * SECCOMP mode with a message naming both numbers.
 */
#if defined(__aarch64__) && UM_KERN_PAGE_SIZE < 16384
#define STUB_DATA_PAGES 4
#else
#define STUB_DATA_PAGES 2
#endif
/*
 * Per-task stub data slots (docs §12.35).  One mm has STUB_MAX_SLOTS copies of
 * struct stub_data, laid out back to back after the one shared code page, so a
 * guest thread can be handed its own register handoff / futex / pending
 * syscall queue / signal stack.  B0 keeps only slot 0 in use; the point of
 * this stage is the layout and the accessors, not concurrency.
 *
 * 8 because the ceiling is nearly free (it is only address space and a
 * per-mm allocation) and a thread that has to be refused a slot is a failure
 * mode nobody wants to debug.
 */
#define STUB_MAX_SLOTS 8
/*
 * Where the stub's per-slot descriptor-passing sockets live in its own
 * descriptor table.  High on purpose: the stub's close_range() sweep keeps
 * [STUB_SOCK_BASE, STUB_SOCK_BASE + STUB_MAX_SLOTS) and closes everything
 * else, and a low fixed number could collide with the physmem fd that the
 * trampoline deliberately leaves open for the first two mappings.
 */
#define STUB_SOCK_BASE 200
/*
 * A slot is the stub_data (handoff word, register save area, pending syscall
 * queue, signal stack) plus a stack for the stub thread that owns it.  The
 * thread needs a stack of its own while it is parked waiting for work; the
 * guest's own stack is not usable until the kernel has handed it a context.
 *
 * The stack area is not optional bookkeeping.  Without it the thread's initial
 * stack pointer sits at the top of the slot, i.e. inside sigstack[], and the
 * very first SIGSYS frame the host writes there (4.6 KB on arm64) lands on top
 * of the thread's live frames.  That happens to survive a thread that parks
 * forever, which is exactly why it would have been found much later, by a
 * handler that returned.
 *
 * It also gives the slot's control words a home: they live at the bottom of
 * the stack area, far below any frame this thread will ever use.
 *
 * STUB_DATA_PAGES is a power of two, so the slot stays one.
 */
#define STUB_SLOT_STACK_PAGES STUB_DATA_PAGES
#define STUB_SLOT_PAGES (STUB_DATA_PAGES + STUB_SLOT_STACK_PAGES)
#define STUB_SLOT_SIZE (STUB_SLOT_PAGES * UM_KERN_PAGE_SIZE)
#define STUB_DATA_TOTAL_PAGES (STUB_SLOT_PAGES * STUB_MAX_SLOTS)

/*
 * Where the slot region starts.
 *
 * arm64 puts it a whole slot after the code page and aligns stub_start to a
 * slot, so that every slot base is STUB_SLOT_SIZE-aligned.  That alignment is
 * what lets the stub work out which slot it owns from its own stack pointer --
 * the only per-thread handle a process-wide SIGSYS disposition leaves it, since
 * the handler runs on the thread's own sigaltstack, which lives inside its
 * slot (docs §12.35).
 *
 * x86 keeps the historical "data immediately follows the code page" layout:
 * the slot work is arm64-only for now, and moving x86's data page would change
 * the ADR/LEA-based get_stub_data() it still uses.
 */
#if defined(__aarch64__)
#define STUB_DATA (STUB_CODE + STUB_SLOT_SIZE)
#define STUB_SIZE (STUB_SLOT_SIZE + STUB_DATA_TOTAL_PAGES * UM_KERN_PAGE_SIZE)
#else
#define STUB_DATA (STUB_CODE + UM_KERN_PAGE_SIZE)
#define STUB_SIZE ((1 + STUB_DATA_TOTAL_PAGES) * UM_KERN_PAGE_SIZE)
#endif
#define STUB_END (STUB_START + STUB_SIZE)
/* Offset of the slot region from STUB_START (what the stub mmaps). */
#define STUB_DATA_OFFSET (STUB_DATA - STUB_CODE)

#ifndef __ASSEMBLER__

#include <sysdep/ptrace.h>

struct task_struct;
extern struct task_struct *cpu_tasks[];

extern unsigned long long physmem_size;

extern unsigned long high_physmem;
extern unsigned long uml_physmem;
extern unsigned long uml_reserved;
extern unsigned long end_vm;
extern unsigned long start_vm;

extern unsigned long brk_start;

extern unsigned long host_task_size;
extern unsigned long stub_start;

extern int linux_main(int argc, char **argv, char **envp);
extern void uml_finishsetup(void);

struct siginfo;
extern void (*sig_info[])(int, struct siginfo *si, struct uml_pt_regs *, void *);

#endif

#endif
