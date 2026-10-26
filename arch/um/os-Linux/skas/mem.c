// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2021 Benjamin Berg <benjamin@sipsolutions.net>
 * Copyright (C) 2002 - 2007 Jeff Dike (jdike@{addtoit,linux.intel}.com)
 */

#include <stddef.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/mman.h>
#include <init.h>
#include <as-layout.h>
#include <mm_id.h>
#include <os.h>
#include <ptrace_user.h>
#include <registers.h>
#include <skas.h>
#include <sysdep/ptrace.h>
#include <sysdep/archsetjmp.h>
#include <sysdep/stub.h>
#include "../internal.h"

extern char __syscall_stub_start[];

/* How many flushes had to run inside a user-mode entry (docs §12.35.7.3). */
static int flush_inside_entry;

void syscall_stub_dump_error(struct mm_id *mm_idp)
{
	struct stub_syscall *sc;
	int len = mm_idp->syscall_data_len;

	if (len < 0 || len > STUB_QUEUE_MAX)
		panic("Syscall data was corrupted by stub (len is: %d, expected maximum: %d)!",
			len, STUB_QUEUE_MAX);

	sc = len == STUB_QUEUE_MAX ? &mm_idp->queue[STUB_QUEUE_MAX - 1] :
				    &mm_idp->queue[len];

	printk(UM_KERN_ERR "%s : length = %d, last offset = %d",
		__func__, len, (int)(sc - mm_idp->queue));

	printk(UM_KERN_ERR "%s : stub syscall type %d failed, return value = 0x%lx\n",
		__func__, sc->syscall, (unsigned long)sc->mem.addr);

	print_hex_dump(UM_KERN_ERR, "    syscall data: ", 0,
		       16, 4, sc, sizeof(*sc), 0);

	if (using_seccomp) {
		printk(UM_KERN_ERR "%s: FD map num: %d", __func__,
		       mm_idp->syscall_fd_num);
		print_hex_dump(UM_KERN_ERR,
				"    FD map: ", 0, 16,
				sizeof(mm_idp->syscall_fd_map[0]),
				mm_idp->syscall_fd_map,
				sizeof(mm_idp->syscall_fd_map), 0);
	}
}

static inline unsigned long *check_init_stack(struct mm_id * mm_idp,
					      unsigned long *stack)
{
	if (stack == NULL) {
		stack = (unsigned long *) STUB_DATA_SLOT(mm_idp, 0) + 2;
		*stack = 0;
	}
	return stack;
}

static unsigned long syscall_regs[MAX_REG_NR];

static int __init init_syscall_regs(void)
{
	get_safe_registers(syscall_regs, NULL);

	syscall_regs[REGS_IP_INDEX] = STUB_CODE +
		((unsigned long) stub_syscall_handler -
		 (unsigned long) __syscall_stub_start);
	/*
	 * Same ABI question as a fresh kernel stack, and the same answer: how
	 * much to leave below the top is per-architecture. sigstack[] is
	 * page-aligned and page-sized, so subtracting a word unconditionally
	 * would hand the stub an SP that is 8 mod 16 -- fine on x86, where it
	 * stands in for the return address CALL pushes, and a violation of
	 * AAPCS64, which wants 16-byte alignment at all times and has hardware
	 * checking behind it.
	 */
	syscall_regs[REGS_SP_INDEX] = STUB_DATA +
		offsetof(struct stub_data, sigstack) +
		sizeof(((struct stub_data *) 0)->sigstack) -
		ARCH_INIT_SP_RESERVE;

	return 0;
}

__initcall(init_syscall_regs);

/*
 * Hand the pending batch to the control slot and wait for it to be applied.
 *
 * Called when the batch is full or on an explicit flush, never on the fast
 * path: the fast path ships the batch together with the guest's registers, so
 * applying it costs no extra handoff.
 *
 * "Control slot" is slot 0, which no task runs guest code on.  Exactly one
 * caller may use it at a time, so this takes the mm's control lock -- and it
 * does so *without* expecting the batch lock to be held, because it has to
 * block.
 */
static long do_syscall_stub(struct mm_id *mm_idp)
{
	struct stub_data *proc_data = STUB_DATA_SLOT(mm_idp, 0);
	unsigned long flags;
	int nested, took_turnstile;
	int n, i;
	int err, pid = mm_idp->pid;

	/*
	 * The turnstile may already be held, and by this very task.
	 *
	 * With the pool live, a normal entry no longer takes it -- that is the
	 * point -- so the control slot needs its own exclusion and this takes
	 * the turnstile here.  But a flush can also be requested from *inside*
	 * a user-mode entry: the serialised configuration holds the turnstile
	 * across the whole entry, and the fault path that fills the batch runs
	 * in that same task.  A mutex is not recursive, so re-taking it there
	 * is a self-deadlock, and one that only shows up under a workload that
	 * fills the batch -- measured: X + glamor guests stopped with Xorg's
	 * startup wedged (docs §12.35.7.3).
	 *
	 * So take it only when the task is not already inside an entry, and
	 * remember whether it was taken.  Order is always turnstile -> control
	 * lock, and it is never held while the batch lock is.
	 */
	nested = mm_idp->turnstile_owner == gettid();
	took_turnstile = using_seccomp && !nested;
	if (took_turnstile) {
		enter_turnstile(mm_idp);
		mm_idp->turnstile_owner = gettid();
	}
	if (nested && flush_inside_entry++ < 3)
		printk(UM_KERN_INFO "stub: flush on the control slot from inside an entry (tid %d)\n",
		       gettid());
	stub_control_lock(mm_idp);

	/*
	 * Move the batch to the control slot.  Copying rather than sharing is
	 * what lets the batch live in the mm: on the fast path the same batch
	 * is copied into a *task* slot instead, and the copy is what makes the
	 * two uses independent.
	 */
	stub_queue_lock(mm_idp, &flags);
	n = mm_idp->syscall_data_len;
	if (n > 0) {
		memcpy(proc_data->syscall_data, mm_idp->queue,
		       n * sizeof(mm_idp->queue[0]));
		memcpy(mm_idp->slot_fd_map[0], mm_idp->syscall_fd_map,
		       sizeof(mm_idp->syscall_fd_map));
		mm_idp->slot_fd_num[0] = mm_idp->syscall_fd_num;
		mm_idp->syscall_data_len = 0;
		mm_idp->syscall_fd_num = 0;
	}
	proc_data->syscall_data_len = n;
	stub_queue_unlock(mm_idp, flags);

	if (using_seccomp) {
		proc_data->restart_wait = 1;
		wait_stub_done_seccomp(mm_idp, 0, 0, 1);
	} else {
		n = put_host_regs(pid, syscall_regs);
		if (n < 0) {
			printk(UM_KERN_ERR "Registers -\n");
			for (i = 0; i < MAX_REG_NR; i++)
				printk(UM_KERN_ERR "\t%d\t0x%lx\n", i, syscall_regs[i]);
			panic("%s : put_host_regs failed, errno = %d\n",
			      __func__, -n);
		}

		err = ptrace(PTRACE_CONT, pid, 0, 0);
		if (err)
			panic("Failed to continue stub, pid = %d, errno = %d\n",
			      pid, errno);

		wait_stub_done(pid);
	}

	/*
	 * proc_data->err will be negative if there was an (unexpected) error.
	 * In that case, syscall_data_len points to the last executed syscall,
	 * otherwise it will be zero (but we do not need to rely on that).
	 */
	stub_queue_lock(mm_idp, &flags);
	if (proc_data->err < 0) {
		mm_idp->syscall_data_len = n;
		stub_queue_unlock(mm_idp, flags);
		syscall_stub_dump_error(mm_idp);

		/* Store error code in case someone tries to add more syscalls */
		stub_queue_lock(mm_idp, &flags);
		mm_idp->syscall_data_len = proc_data->err;
		mm_idp->syscall_fd_num = 0;
	} else {
		mm_idp->syscall_data_len = 0;
		mm_idp->syscall_fd_num = 0;
	}
	stub_queue_unlock(mm_idp, flags);

	stub_control_unlock(mm_idp);
	if (took_turnstile) {
		mm_idp->turnstile_owner = 0;
		exit_turnstile(mm_idp);
	}

	return mm_idp->syscall_data_len;
}

int syscall_stub_flush(struct mm_id *mm_idp)
{
	unsigned long flags;

	stub_queue_lock(mm_idp, &flags);

	if (mm_idp->syscall_data_len == 0) {
		stub_queue_unlock(mm_idp, flags);
		return 0;
	}

	/* If an error happened already, report it and reset the state. */
	if (mm_idp->syscall_data_len < 0) {
		int res = mm_idp->syscall_data_len;

		mm_idp->syscall_data_len = 0;
		stub_queue_unlock(mm_idp, flags);
		return res;
	}

	stub_queue_unlock(mm_idp, flags);

	return do_syscall_stub(mm_idp);
}

/*
 * How many more stub syscalls fit into the current batch without forcing a
 * flush. The fault-around code in trap.c uses this to cap its window: a
 * prefault that overflowed the batch would trigger a mid-batch handoff in
 * do_syscall_stub, i.e. exactly the extra round trip prefaulting exists to
 * avoid. Worst case each prefaulted page costs one slot (anonymous pages have
 * arbitrary physmem offsets, so map() can rarely merge them).
 */
int syscall_stub_free_slots(struct mm_id *mm_idp)
{
	unsigned long flags;
	int len;

	stub_queue_lock(mm_idp, &flags);
	len = mm_idp->syscall_data_len;
	stub_queue_unlock(mm_idp, flags);

	/* Batch is in an error state; report no room. */
	if (len < 0)
		return 0;

	return STUB_QUEUE_MAX - len;
}

/* The last entry, if it is the same kind of call and contiguous with @virt. */
static struct stub_syscall *queue_previous(struct mm_id *mm_idp,
					   int syscall_type,
					   unsigned long virt)
{
	if (mm_idp->syscall_data_len > 0) {
		struct stub_syscall *sc =
			&mm_idp->queue[mm_idp->syscall_data_len - 1];

		if (sc->syscall == syscall_type &&
		    sc->mem.addr + sc->mem.length == virt)
			return sc;
	}

	return NULL;
}

/* Commit one entry; the caller holds the batch lock. */
static void queue_push(struct mm_id *mm_idp, struct stub_syscall *sc)
{
	mm_idp->queue[mm_idp->syscall_data_len] = *sc;
	mm_idp->syscall_data_len++;
}

/*
 * Reserve a descriptor index for @fd, or flush and start a new map.
 *
 * Called with the batch lock held; returns -1 if the caller has to flush
 * first, which is the only reason it cannot just do the work.  The index is
 * into mm_idp->syscall_fd_map, which the handoff copies to the running slot's
 * list and then sends over that slot's socket.
 */
static int resolve_stub_fd(struct mm_id *mm_idp, int fd)
{
	int i;

	if (!using_seccomp)
		return fd;

	for (i = 0; i < mm_idp->syscall_fd_num; i++) {
		if (mm_idp->syscall_fd_map[i] == fd)
			return i;
	}

	if (mm_idp->syscall_fd_num == STUB_MAX_FDS)
		return -1;

	i = mm_idp->syscall_fd_num++;
	mm_idp->syscall_fd_map[i] = fd;

	return i;
}

int map(struct mm_id *mm_idp, unsigned long virt, unsigned long len, int prot,
	int phys_fd, unsigned long long offset)
{
	unsigned long flags;

	for (;;) {
		struct stub_syscall *sc, entry;
		int idx;

		stub_queue_lock(mm_idp, &flags);

		/* The batch is in an error state; drop the call, the flush
		 * that set it was already reported. */
		if (mm_idp->syscall_data_len < 0) {
			stub_queue_unlock(mm_idp, flags);
			return mm_idp->syscall_data_len;
		}

		/* Compress with the previous call if that is possible */
		sc = queue_previous(mm_idp, STUB_SYSCALL_MMAP, virt);
		if (sc && sc->mem.prot == prot &&
		    sc->mem.offset == MMAP_OFFSET(offset - sc->mem.length)) {
			int prev_fd = sc->mem.fd;

			if (using_seccomp)
				prev_fd = mm_idp->syscall_fd_map[sc->mem.fd];

			if (phys_fd == prev_fd) {
				sc->mem.length += len;
				stub_queue_unlock(mm_idp, flags);
				return 0;
			}
		}

		if (mm_idp->syscall_data_len == STUB_QUEUE_MAX) {
			stub_queue_unlock(mm_idp, flags);
			do_syscall_stub(mm_idp);
			continue;
		}

		idx = resolve_stub_fd(mm_idp, phys_fd);
		if (idx < 0) {
			/* Descriptor list full: flush, then start a new batch. */
			stub_queue_unlock(mm_idp, flags);
			do_syscall_stub(mm_idp);
			continue;
		}

		entry.syscall = STUB_SYSCALL_MMAP;
		entry.mem.addr = virt;
		entry.mem.length = len;
		entry.mem.prot = prot;
		entry.mem.fd = idx;
		entry.mem.offset = MMAP_OFFSET(offset);
		queue_push(mm_idp, &entry);

		stub_queue_unlock(mm_idp, flags);
		return 0;
	}
}

int unmap(struct mm_id *mm_idp, unsigned long addr, unsigned long len)
{
	unsigned long flags;

	for (;;) {
		struct stub_syscall *sc, entry;

		stub_queue_lock(mm_idp, &flags);

		if (mm_idp->syscall_data_len < 0) {
			stub_queue_unlock(mm_idp, flags);
			return mm_idp->syscall_data_len;
		}

		/* Compress with the previous call if that is possible */
		sc = queue_previous(mm_idp, STUB_SYSCALL_MUNMAP, addr);
		if (sc) {
			sc->mem.length += len;
			stub_queue_unlock(mm_idp, flags);
			return 0;
		}

		if (mm_idp->syscall_data_len == STUB_QUEUE_MAX) {
			stub_queue_unlock(mm_idp, flags);
			do_syscall_stub(mm_idp);
			continue;
		}

		entry.syscall = STUB_SYSCALL_MUNMAP;
		entry.mem.addr = addr;
		entry.mem.length = len;
		queue_push(mm_idp, &entry);

		stub_queue_unlock(mm_idp, flags);
		return 0;
	}
}
