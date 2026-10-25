/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2005 Jeff Dike (jdike@karaya.com)
 */

#ifndef __MM_ID_H
#define __MM_ID_H

#include <linux/compiler_types.h>

#define STUB_MAX_FDS 4

struct mm_id {
	int pid;
	unsigned long stack;
	int syscall_data_len;

	/* Only used with SECCOMP mode */
	int sock;
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

void enter_turnstile(struct mm_id *mm_id) __acquires(turnstile);
void exit_turnstile(struct mm_id *mm_id) __releases(turnstile);

void notify_mm_kill(int pid);

#endif
