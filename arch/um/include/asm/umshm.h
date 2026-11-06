/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __UM_ASM_UMSHM_H
#define __UM_ASM_UMSHM_H

#include <linux/types.h>

struct virtio_device;

/*
 * virtio-gpu host-visible blobs.  A host3d blob lives in host memory that the
 * backend allocated, and the guest reaches it by mapping the host file
 * descriptor behind it at the physical address the guest reserved for that
 * blob.  The two calls below tell arch/um/kernel/umshm.c about that
 * descriptor; see the file for the transport.
 *
 * @paddr: guest physical address of the blob inside the host-visible window
 * @len:   size in bytes, page aligned
 * @res_id: virtio-gpu resource id, i.e. the name the backend published
 */
#if IS_ENABLED(CONFIG_UML)
int uml_virtio_gpu_blob_map(struct virtio_device *vdev, unsigned long paddr,
			    unsigned long len, u32 res_id);
void uml_virtio_gpu_blob_unmap(unsigned long paddr);
/*
 * io_remap_pfn_range() installs the host mapping directly, so the guest's own
 * page tables never learn about it and the generic teardown has nothing to
 * undo.  Whoever mapped such a range has to drop the host mapping when the
 * vma goes away.
 */
void uml_unmap_memory_range(unsigned long start, unsigned long len);
#else
static inline int uml_virtio_gpu_blob_map(struct virtio_device *vdev,
					  unsigned long paddr, unsigned long len,
					  u32 res_id)
{
	return -ENODEV;
}

static inline void uml_virtio_gpu_blob_unmap(unsigned long paddr)
{
}

static inline void uml_unmap_memory_range(unsigned long start, unsigned long len)
{
}
#endif

#endif /* __UM_ASM_UMSHM_H */
