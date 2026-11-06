/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __UM_UMSHM_H
#define __UM_UMSHM_H

/*
 * Host file descriptors registered as guest physical memory at runtime.
 * See arch/um/kernel/umshm.c; phys_mapping() in physmem.c consults this
 * before the boot-time regions.
 */
int umshm_lookup(unsigned long phys, unsigned long *offset_out);
int umshm_register(unsigned long phys, unsigned long len, const char *path);
int umshm_register_fd(unsigned long phys, unsigned long len, int fd,
		      const char *owner);
void umshm_unregister(unsigned long phys);

#endif /* __UM_UMSHM_H */
