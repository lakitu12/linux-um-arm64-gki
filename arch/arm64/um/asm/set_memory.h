/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __UM_ARM64_SET_MEMORY_H
#define __UM_ARM64_SET_MEMORY_H

/*
 * A UML guest has no page tables of its own: every "kernel page" is a host
 * mapping owned by the UML process, so tightening a page means mprotect() on
 * the host, not a PTE update.  arch/arm64/um/set_memory.c does exactly that --
 * these are not no-ops.  The BPF JIT depends on it: execmem hands back writable
 * memory and bpf_jit_binary_lock_ro() asks for read-only + executable, and if
 * the request is dropped the generated code is never executable.
 */
int set_memory_x(unsigned long addr, int numpages);
int set_memory_ro(unsigned long addr, int numpages);
int set_memory_rw(unsigned long addr, int numpages);
int set_memory_nx(unsigned long addr, int numpages);

#endif
