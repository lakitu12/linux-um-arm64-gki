// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2000 - 2007 Jeff Dike (jdike@{addtoit,linux.intel}.com)
 */

#include <linux/module.h>
#include <linux/memblock.h>
#include <linux/mm.h>
#include <linux/pfn.h>
#include <asm/page.h>
#include <asm/sections.h>
#include <as-layout.h>
#include <init.h>
#include <kern.h>
#include <kern_util.h>
#include <mem_user.h>
#include <mm_id.h>
#include <os.h>

#include <asm/umshm.h>

#include <umshm.h>

static int physmem_fd = -1;

/* Changed during early boot */
unsigned long high_physmem;
EXPORT_SYMBOL(high_physmem);

/*
 * io_remap_pfn_range(), for the pfn-to-descriptor case described in
 * asm/pgtable.h.
 *
 * The pages have no struct page -- they live in a host descriptor registered
 * through umshm.c -- so the generic implementation cannot work: it would go
 * through pfn_to_page().  Installing a host mapping here directly does not
 * work either.  It looks fine until the first syscall returns: UML's page
 * tables know nothing about the range, the sync walk at the syscall boundary
 * finds no PTE, and the freshly installed mapping is dropped before the
 * caller can touch it (measured: the first store after mmap() faulted at the
 * address that had just been mapped).
 *
 * So this does what every other UML mapping does: publish guest PTEs and let
 * the fault/sync path install the host side.  The first access faults, and
 * phys_mapping() -- which consults umshm.c -- supplies the descriptor then.
 */
int io_remap_pfn_range(struct vm_area_struct *vma, unsigned long addr,
		       unsigned long pfn, unsigned long size, pgprot_t prot)
{
	struct mm_struct *mm = vma->vm_mm;
	unsigned long phys = pfn << PAGE_SHIFT;
	unsigned long end = addr + size;
	unsigned long a;

	/* A hole here would fault later with no descriptor behind it, so refuse
	 * the whole range up front instead of mapping most of it. */
	for (a = phys; a < phys + size; a += PAGE_SIZE) {
		unsigned long long off;

		if (phys_mapping(a, &off) < 0) {
			pr_info("uml_io_remap: no backing for phys 0x%lx (len 0x%lx)\n",
				a, size);
			return -EINVAL;
		}
	}

	for (a = addr; a < end; a += PAGE_SIZE, phys += PAGE_SIZE) {
		pgd_t *pgd = pgd_offset(mm, a);
		p4d_t *p4d;
		pud_t *pud;
		pmd_t *pmd;
		pte_t *pte;

		p4d = p4d_alloc(mm, pgd, a);
		if (!p4d)
			return -ENOMEM;
		pud = pud_alloc(mm, p4d, a);
		if (!pud)
			return -ENOMEM;
		pmd = pmd_alloc(mm, pud, a);
		if (!pmd)
			return -ENOMEM;
		pte = pte_alloc_map(mm, pmd, a);
		if (!pte)
			return -ENOMEM;
		if (pte_present(*pte))
			continue;
		/* The physical address is the PTE payload on UML; set_pte() sets
		 * NEEDSYNC, which is what makes the next sync publish it. */
		set_pte(pte, __pte(phys | _PAGE_PRESENT | _PAGE_RW | _PAGE_USER |
				   _PAGE_ACCESSED | _PAGE_DIRTY));
	}

	/*
	 * Publish the first page (and the rest of the range) up front as well.
	 * Without it the first access to every page takes the fault path, and
	 * that path races the sync walk over the very PTE installed above
	 * ("pte changed under the sync walk") for a ring buffer written by a
	 * spinning client.  The sync walk sees these PTEs as already mapped and
	 * leaves them alone, so the two agree from the start.
	 */
	{
		unsigned long long fd_off;
		int fd = phys_mapping(pfn << PAGE_SHIFT, &fd_off);
		int w = !!(vma->vm_flags & VM_WRITE);

		if (fd >= 0) {
			int err;

			/*
			 * The host side has to land in the mm of the task that
			 * owns this vma.  os_map_memory() maps the calling
			 * context, and for an mmap() from a guest process that
			 * context is the vCPU thread of the UML kernel, not the
			 * guest process: the address stays occupied by whatever
			 * the guest's own loader left there (a read-only slice of
			 * the image) and the guest reads that instead of the
			 * blob.  map() hands the mmap to the owning stub, the
			 * same path every guest page fault takes.
			 */
			if (vma->vm_mm == &init_mm) {
				err = os_map_memory((void *)addr, fd, fd_off,
						    size, 1, w, 0);
			} else {
				int prot = 1 /* PROT_READ */ |
					   (w ? 2 /* PROT_WRITE */ : 0);

				err = map(&vma->vm_mm->context.id, addr, size,
					  prot, fd, fd_off);
			}

			if (err)
				return err;
		}
	}

	return 0;
}

/* Called from the driver that mapped a non-RAM range through
 * io_remap_pfn_range(), when its vma is torn down.  os_unmap_memory() is the
 * same primitive the tlb code uses to drop a mapping (kern_unmap()). */
void uml_unmap_memory_range(unsigned long start, unsigned long len)
{
	int err = os_unmap_memory((void *)start, len);

	if (err)
		printk(KERN_WARNING "unmap of host range 0x%lx+0x%lx failed, err = %d\n",
		       start, len, err);
}
EXPORT_SYMBOL_GPL(uml_unmap_memory_range);

void map_memory(unsigned long virt, unsigned long phys, unsigned long len,
		int r, int w, int x)
{
	__u64 offset;
	int fd, err;

	fd = phys_mapping(phys, &offset);
	err = os_map_memory((void *) virt, fd, offset, len, r, w, x);
	if (err) {
		if (err == -ENOMEM)
			printk(KERN_ERR "try increasing the host's "
			       "/proc/sys/vm/max_map_count to <physical "
			       "memory size>/%lu\n", PAGE_SIZE);
		panic("map_memory(0x%lx, %d, 0x%llx, %ld, %d, %d, %d) failed, "
		      "err = %d\n", virt, fd, offset, len, r, w, x, err);
	}
}

/**
 * setup_physmem() - Setup physical memory for UML
 * @start:	Start address of the physical kernel memory,
 *		i.e start address of the executable image.
 * @reserve_end:	end address of the physical kernel memory.
 * @len:	Length of total physical memory that should be mapped/made
 *		available, in bytes.
 *
 * Creates an unlinked temporary file of size (len) and memory maps
 * it on the last executable image address (uml_reserved).
 *
 * The offset is needed as the length of the total physical memory
 * (len) includes the size of the memory used be the executable image,
 * but the mapped-to address is the last address of the executable image
 * (uml_reserved == end address of executable image).
 *
 * The memory mapped memory of the temporary file is used as backing memory
 * of all user space processes/kernel tasks.
 */
void __init setup_physmem(unsigned long start, unsigned long reserve_end,
			  unsigned long len)
{
	unsigned long reserve = reserve_end - start;
	unsigned long map_size = len - reserve;
	int err;

	if (len <= reserve) {
		os_warn("Too few physical memory! Needed=%lu, given=%lu\n",
			reserve, len);
		exit(1);
	}

	physmem_fd = create_mem_file(len);

	err = os_map_memory((void *) reserve_end, physmem_fd, reserve,
			    map_size, 1, 1, 1);
	if (err < 0) {
		os_warn("setup_physmem - mapping %lu bytes of memory at 0x%p "
			"failed - errno = %d\n", map_size,
			(void *) reserve_end, err);
		exit(1);
	}

	/*
	 * Special kludge - This page will be mapped in to userspace processes
	 * from physmem_fd, so it needs to be written out there.
	 */
	os_seek_file(physmem_fd, __pa(__syscall_stub_start));
	os_write_file(physmem_fd, __syscall_stub_start, PAGE_SIZE);

	memblock_add(__pa(start), len);
	memblock_reserve(__pa(start), reserve);

	min_low_pfn = PFN_UP(__pa(reserve_end));
	max_low_pfn = min_low_pfn + (map_size >> PAGE_SHIFT);
}

int phys_mapping(unsigned long phys, unsigned long long *offset_out)
{
	int fd = -1;
	unsigned long off;

	/*
	 * Backend-registered descriptors come first: the virtio-gpu host-visible
	 * window is carved out of guest physical memory that already has an
	 * owner, and the blob that replaces a piece of it must win.
	 */
	fd = umshm_lookup(phys, &off);
	if (fd >= 0) {
		*offset_out = off;
		return fd;
	}

	if (phys < physmem_size) {
		fd = physmem_fd;
		*offset_out = phys;
	}
	else if (phys < __pa(end_iomem)) {
		struct iomem_region *region = iomem_regions;

		while (region != NULL) {
			if ((phys >= region->phys) &&
			    (phys < region->phys + region->size)) {
				fd = region->fd;
				*offset_out = phys - region->phys;
				break;
			}
			region = region->next;
		}
	}

	return fd;
}
EXPORT_SYMBOL(phys_mapping);

static int __init uml_mem_setup(char *line, int *add)
{
	char *retptr;

	*add = 0;
	physmem_size = memparse(line,&retptr);
	return 0;
}
__uml_setup("mem=", uml_mem_setup,
"mem=<Amount of desired ram>\n"
"    This controls how much \"physical\" memory the kernel allocates\n"
"    for the system. The size is specified as a number followed by\n"
"    one of 'k', 'K', 'm', 'M', which have the obvious meanings.\n"
"    This is not related to the amount of memory in the host.  It can\n"
"    be more, and the excess, if it's ever used, will just be swapped out.\n"
"	Example: mem=64M\n\n"
);

__uml_setup("iomem=", parse_iomem,
"iomem=<name>,<file>\n"
"    Configure <file> as an IO memory region named <name>.\n\n"
);

/*
 * This list is constructed in parse_iomem and addresses filled in
 * setup_iomem, both of which run during early boot.  Afterwards, it's
 * unchanged.
 */
struct iomem_region *iomem_regions;

/* Initialized in parse_iomem and unchanged thereafter */
int iomem_size;

unsigned long find_iomem(char *driver, unsigned long *len_out)
{
	struct iomem_region *region = iomem_regions;

	while (region != NULL) {
		if (!strcmp(region->driver, driver)) {
			*len_out = region->size;
			return region->virt;
		}

		region = region->next;
	}

	return 0;
}
EXPORT_SYMBOL(find_iomem);

static int setup_iomem(void)
{
	struct iomem_region *region = iomem_regions;
	unsigned long iomem_start = high_physmem + PAGE_SIZE;
	int err;

	while (region != NULL) {
		err = os_map_memory((void *) iomem_start, region->fd, 0,
				    region->size, 1, 1, 0);
		if (err)
			printk(KERN_ERR "Mapping iomem region for driver '%s' "
			       "failed, errno = %d\n", region->driver, -err);
		else {
			region->virt = iomem_start;
			region->phys = __pa(region->virt);
		}

		iomem_start += region->size + PAGE_SIZE;
		region = region->next;
	}

	return 0;
}

__initcall(setup_iomem);
