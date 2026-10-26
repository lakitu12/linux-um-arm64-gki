/* SPDX-License-Identifier: GPL-2.0 */
/* 
 * Copyright (C) 2002 - 2007 Jeff Dike (jdike@{addtoit,linux.intel}.com)
 */

#ifndef __ARCH_UM_MMU_H
#define __ARCH_UM_MMU_H

#include "linux/types.h"
#include <linux/mutex.h>
#include <linux/wait.h>
#include <linux/spinlock.h>
#include <mm_id.h>

typedef struct mm_context {
	struct mm_id id;
	struct mutex turnstile;
	/* Protects the task-slot pool in id (docs §12.35). */
	struct mutex slot_lock;
	/*
	 * Tasks waiting for one of this mm's task slots to come back (docs
	 * §12.35.7.2).  A task that has to wait must actually sleep: the first
	 * version spun on the slot table, and because a UML timer signal
	 * interrupts a timeout sleep immediately, "spin 1000 times, then sleep
	 * one jiffy" degenerated into a spin that burned host CPU and starved
	 * the task holding the slot.  Measured: four guest threads on a
	 * one-slot pool ran at 0.12x their serial time, with 30x the CPU.
	 */
	wait_queue_head_t slot_wait;
	/* Bumped whenever a slot becomes available; see stub_slot_get(). */
	unsigned int slot_gen;
	/*
	 * Protects the pending map/unmap batch in id.  A spinlock because the
	 * TLB-flush path queues unmaps with interrupts disabled; the batch
	 * itself is a handful of bytes per entry and nothing under this lock
	 * blocks.
	 */
	spinlock_t batch_lock;
	/*
	 * Serialises use of the control slot (slot 0) for stub syscalls.  A
	 * mutex, not a spinlock: holding the control slot means blocking in
	 * the handoff until the stub has applied the batch.
	 */
	struct mutex control_lock;

	struct list_head list;

	/* Address range in need of a TLB sync */
	spinlock_t sync_tlb_lock;
	unsigned long sync_tlb_range_from;
	unsigned long sync_tlb_range_to;

	/*
	 * Anonymous fault-around state (arch/um/kernel/trap.c). A fault costs a
	 * full stub handoff (~51us measured on-device in seccomp mode), so
	 * sequential fault streams are detected and additional pages are
	 * prefaulted into the same handoff. prefault_next is the page a
	 * sequential stream would fault next (end of the last window);
	 * prefault_level indexes the ramp. Purely a heuristic: fields are
	 * accessed racily (READ_ONCE/WRITE_ONCE only) under mmap_read_lock,
	 * a lost update just mispredicts one window.
	 */
	unsigned long prefault_next;
	unsigned int prefault_level;
} mm_context_t;

#define INIT_MM_CONTEXT(mm)						\
	.context = {							\
		.turnstile = __MUTEX_INITIALIZER(mm.context.turnstile),	\
		.slot_lock = __MUTEX_INITIALIZER(mm.context.slot_lock),	\
		.slot_wait = __WAIT_QUEUE_HEAD_INITIALIZER(mm.context.slot_wait), \
		.batch_lock = __SPIN_LOCK_INITIALIZER(mm.context.batch_lock), \
		.control_lock = __MUTEX_INITIALIZER(mm.context.control_lock), \
		.sync_tlb_lock = __SPIN_LOCK_INITIALIZER(mm.context.sync_tlb_lock), \
	}

#endif
