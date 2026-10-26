/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2005 Jeff Dike (jdike@karaya.com)
 */

#ifndef __MM_ID_H
#define __MM_ID_H

#include <linux/compiler_types.h>
/* STUB_MAX_SLOTS: mm_id carries one descriptor-passing socket per slot. */
#include <as-layout.h>

#define STUB_MAX_FDS 4

/*
 * One queued stub call: an mmap or an munmap the stub process has to make in
 * its own address space.  Defined here rather than in stub-data.h because the
 * mm, not a slot, owns the pending batch now (see struct mm_id).
 */
enum stub_syscall_type {
	STUB_SYSCALL_UNSET = 0,
	STUB_SYSCALL_MMAP,
	STUB_SYSCALL_MUNMAP,
};

struct stub_syscall {
	struct {
		unsigned long addr;
		unsigned long length;
		unsigned long offset;
		int fd;
		int prot;
	} mem;

	enum stub_syscall_type syscall;
};

struct mm_id {
	int pid;
	unsigned long stack;
	int syscall_data_len;
	/*
	 * Slots that are usable: the stub thread that owns slot @i has parked
	 * at least once, so the slot holds a real signal frame to write guest
	 * registers into.
	 *
	 * This has to be tracked separately from @slot_free.  A slot is spawned
	 * -- and its go word released -- by the mm that will use it, and the
	 * thread then has to be scheduled on the host before it can park.
	 * Nothing orders that against the mm's first guest entry, so a slot is
	 * published into @slot_free only once it has been seen parked; see
	 * stub_publish_parked().  Handing one out before that is a real bug and
	 * not a theoretical one: set_stub_state() then copies guest registers
	 * into a signal frame nobody has written yet, find_ctx() rejects it,
	 * and the guest process dies with "failed to set regs: -22"
	 * (docs §12.35.7.1).
	 */
	unsigned long slot_ok;
	/*
	 * Task slots this mm may hand guest code to: bit @i set means slot @i
	 * is free and usable (see @slot_ok).  Slot 0 is never in the pool.  It
	 * is the mm's control slot:
	 * the pending map/unmap batch is built in it and stub syscalls are
	 * served on it, and both of those happen before a task has a slot of
	 * its own (a fault queues a mapping; a new mm queues its first unmap
	 * before any stub thread exists).
	 */
	unsigned long slot_free;
	/* Where the next claim starts looking, so slots are used evenly. */
	int slot_next;
	int slot_count;
	/*
	 * Host tid of the task holding the turnstile, 0 when nobody does.
	 *
	 * The turnstile is a plain mutex, so re-taking it is not a wait but a
	 * deadlock.  A stub-syscall flush has to exclude the entry path from
	 * the control slot, and in the serialised configuration the entry path
	 * *is* the task asking for the flush -- so the flush has to be able to
	 * tell "nobody holds it" from "I hold it" (docs §12.35.7.3).
	 */
	int turnstile_owner;

	/* Only used with SECCOMP mode */
	/*
	 * One descriptor-passing socket per task slot (docs §12.35).  The
	 * kernel's end of slot @i is sock[@i]; the stub's end is
	 * STUB_SOCK_BASE + @i in its own descriptor table.  They have to be
	 * per slot: the pairing between "the kernel sent these descriptors"
	 * and "the thread that will mmap them" is carried by which socket the
	 * message went to, and nothing else can carry it once two slots are
	 * in flight.
	 */
	int sock[STUB_MAX_SLOTS];
	/*
	 * The pending map/unmap batch, and the descriptors it refers to.
	 *
	 * These used to live in slot 0's stub_data.  They cannot stay there
	 * once several tasks may be in guest user mode at the same time: a
	 * task shipping its batch to its own slot would copy from the control
	 * slot while another task appends to or services it.  Per-slot
	 * descriptor lists fall out of the same reasoning -- the kernel sends
	 * a batch's descriptors on that slot's socket, so the list it sends
	 * has to be that slot's list.
	 *
	 * mm->context.batch_lock protects all of it.
	 */
	struct stub_syscall queue[STUB_QUEUE_MAX];
	/*
	 * The descriptors the pending batch refers to, and the per-slot copy
	 * the kernel sends on a slot's socket just before that slot runs.
	 */
	int syscall_fd_map[STUB_MAX_FDS];
	int slot_fd_num[STUB_MAX_SLOTS];
	int slot_fd_map[STUB_MAX_SLOTS][STUB_MAX_FDS];
	int syscall_fd_num;
};

/*
 * Sparse's lock annotations, normally from <linux/compiler_types.h>. This
 * header is reached by USER_CFLAGS objects too, and a sysroot that ships its
 * own linux/compiler_types.h shadows the kernel's -- the Android NDK does.
 * Without these the declarations below parse as function definitions.
 */
#ifndef __acquires
#define __acquires(x)
#endif
#ifndef __releases
#define __releases(x)
#endif

/* Task-slot pool (docs §12.35): see kernel/skas/mmu.c. */
int stub_slot_get(struct mm_id *mm_id);
void stub_slot_put(struct mm_id *mm_id, int slot);

/* The pending map/unmap batch's lock: see kernel/skas/mmu.c. */
void stub_queue_lock(struct mm_id *mm_id, unsigned long *flags);
void stub_queue_unlock(struct mm_id *mm_id, unsigned long flags);

/* How many stub threads an mm should have; see stub_slots= in skas/mmu.c. */
int stub_slots_in_use(void);

/* Serialises the use of the control slot for stub syscalls. */
void stub_control_lock(struct mm_id *mm_id);
void stub_control_unlock(struct mm_id *mm_id);

void enter_turnstile(struct mm_id *mm_id) __acquires(turnstile);
void exit_turnstile(struct mm_id *mm_id) __releases(turnstile);

void notify_mm_kill(int pid);

#endif
