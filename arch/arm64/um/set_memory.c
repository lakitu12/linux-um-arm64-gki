// SPDX-License-Identifier: GPL-2.0
/*
 * Page-permission hooks for a UML guest.
 *
 * There are no guest page tables: "kernel memory" is host memory owned by the
 * UML process, and its permissions are whatever the host mapping says.  The one
 * caller that matters for correctness is the BPF JIT: execmem hands back fresh,
 * writable memory, the JIT fills it with aarch64, and bpf_jit_binary_lock_ro()
 * then asks for read-only + executable.  Ignoring that request (as an earlier
 * version of this file did) leaves generated code in a non-executable host
 * mapping, so the first BPF program dispatch takes SIGSEGV inside the guest.
 *
 * os_protect_memory() is UML's mprotect wrapper; both it and the addresses here
 * are host addresses, because a UML guest address *is* the host address.
 */
#include <linux/mm.h>
#include <os.h>

#include <asm/set_memory.h>

/* Read stays on throughout: the kernel never unmaps its own text. */
static int um_set_memory(unsigned long addr, int numpages, int w, int x)
{
	if (os_protect_memory((void *)addr, numpages * PAGE_SIZE, 1, w, x))
		return -EFAULT;
	return 0;
}

int set_memory_x(unsigned long addr, int numpages)
{
	return um_set_memory(addr, numpages, 0, 1);
}

int set_memory_ro(unsigned long addr, int numpages)
{
	return um_set_memory(addr, numpages, 0, 1);
}

int set_memory_rw(unsigned long addr, int numpages)
{
	return um_set_memory(addr, numpages, 1, 0);
}

int set_memory_nx(unsigned long addr, int numpages)
{
	return um_set_memory(addr, numpages, 1, 0);
}