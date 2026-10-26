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

struct mm_id {
	int pid;
	unsigned long stack;
	int syscall_data_len;
	/* Task slots whose stub thread has parked (docs §12.35). */
	int slots_ready;
	/*
	 * Task slots this mm may hand guest code to: bit @i set means slot @i
	 * is free.  Slot 0 is never in the pool.  It is the mm's control slot:
	 * the pending map/unmap batch is built in it and stub syscalls are
	 * served on it, and both of those happen before a task has a slot of
	 * its own (a fault queues a mapping; a new mm queues its first unmap
	 * before any stub thread exists).
	 */
	unsigned long slot_free;
	/* Where the next claim starts looking, so slots are used evenly. */
	int slot_next;
	int slot_count;

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
	int syscall_fd_num;
	int syscall_fd_map[STUB_MAX_FDS];
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

void enter_turnstile(struct mm_id *mm_id) __acquires(turnstile);
void exit_turnstile(struct mm_id *mm_id) __releases(turnstile);

void notify_mm_kill(int pid);

#endif
