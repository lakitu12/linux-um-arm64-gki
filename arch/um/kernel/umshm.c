// SPDX-License-Identifier: GPL-2.0
/*
 * Runtime registration of host file descriptors as guest physical memory.
 *
 * UML normally maps guest physical memory from one file (setup_physmem()) plus
 * the optional iomem= windows, which are fixed at boot.  A virtio-gpu backend
 * needs more than that: every host-visible blob it hands to the guest is a
 * freshly exported dma-buf, and the guest has to be able to fault those pages
 * in at whatever physical address the GPU wrote to.
 *
 * This file keeps that table.  phys_mapping() consults it before falling back
 * to the boot-time regions, so the existing fault path (arch/um/kernel/tlb.c ->
 * os_map_memory()) needs no change: it asks "which fd and offset backs this
 * physical page", gets the blob's fd, and maps it.
 *
 * The table is filled from the host side through sysfs, because the owner of
 * the fd is an ordinary host process (the virtio-gpu backend), and a host
 * process can only be talked to across a file boundary:
 *
 *   echo "<phys_hex> <len_hex> <path>" > /sys/kernel/umshm/register
 *   echo "<phys_hex>"                  > /sys/kernel/umshm/unregister
 *   cat /sys/kernel/umshm/list
 *
 * <path> is anything the host can open, in practice /proc/<pid>/fd/<n> of the
 * backend that owns the dma-buf.  Opening it yields an independent reference;
 * the backend may close its own copy afterwards.
 */

#include <linux/init.h>
#include <linux/kobject.h>
#include <linux/list.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/sysfs.h>

#include <init.h>
#include <linux/virtio.h>
#include <linux/virtio-uml.h>
#include <os.h>

#include <asm/umshm.h>

#include <mem.h>
#include <umshm.h>

#define UMSHM_PATH_MAX 128

struct umshm_region {
	struct list_head node;
	unsigned long phys;
	unsigned long len;
	int fd;
	char *owner;
};

static LIST_HEAD(umshm_regions);
static DEFINE_SPINLOCK(umshm_lock);

/*
 * Called from phys_mapping(), i.e. from page-fault / mmap context, so it must
 * not sleep: the mutex that guards registration cannot be taken here.
 */
int umshm_lookup(unsigned long phys, unsigned long *offset_out)
{
	struct umshm_region *r;
	unsigned long flags;
	int fd = -1;

	spin_lock_irqsave(&umshm_lock, flags);
	list_for_each_entry(r, &umshm_regions, node) {
		if (phys >= r->phys && phys < r->phys + r->len) {
			fd = r->fd;
			*offset_out = phys - r->phys;
			break;
		}
	}
	spin_unlock_irqrestore(&umshm_lock, flags);

	return fd;
}

static int umshm_add(unsigned long phys, unsigned long len, int fd,
		     const char *owner)
{
	struct umshm_region *r, *n;
	unsigned long flags;

	if (!len || (phys & (PAGE_SIZE - 1)) || (len & (PAGE_SIZE - 1)))
		goto err_fd;

	r = kzalloc(sizeof(*r), GFP_KERNEL);
	if (!r)
		goto err_fd;

	r->owner = kstrdup(owner, GFP_KERNEL);
	if (!r->owner) {
		kfree(r);
		goto err_fd;
	}

	r->phys = phys;
	r->len = len;
	r->fd = fd;

	spin_lock_irqsave(&umshm_lock, flags);
	/* Overlap would make phys_mapping() order-dependent; refuse it. */
	list_for_each_entry(n, &umshm_regions, node) {
		if (phys < n->phys + n->len && n->phys < phys + len) {
			spin_unlock_irqrestore(&umshm_lock, flags);
			os_close_file(fd);
			kfree(r->owner);
			kfree(r);
			return -EEXIST;
		}
	}
	list_add_tail(&r->node, &umshm_regions);
	spin_unlock_irqrestore(&umshm_lock, flags);
	return 0;

err_fd:
	os_close_file(fd);
	return -EINVAL;
}

int umshm_register(unsigned long phys, unsigned long len, const char *path)
{
	int fd;

	/* Opening /proc/<pid>/fd/<n> takes our own reference to the same file,
	 * so the backend may close its end without invalidating the mapping. */
	fd = os_open_file(path, of_read(OPENFLAGS()), 0);
	if (fd < 0)
		return fd;

	return umshm_add(phys, len, fd, path);
}
EXPORT_SYMBOL_GPL(umshm_register);

/* Takes ownership of @fd: it is closed here both on success (when the region
 * is unregistered) and on failure. */
int umshm_register_fd(unsigned long phys, unsigned long len, int fd,
		      const char *owner)
{
	return umshm_add(phys, len, fd, owner);
}
EXPORT_SYMBOL_GPL(umshm_register_fd);

void umshm_unregister(unsigned long phys)
{
	struct umshm_region *r, *t;
	unsigned long flags;

	spin_lock_irqsave(&umshm_lock, flags);
	list_for_each_entry_safe(r, t, &umshm_regions, node) {
		if (r->phys != phys)
			continue;
		list_del(&r->node);
		spin_unlock_irqrestore(&umshm_lock, flags);
		os_close_file(r->fd);
		kfree(r->owner);
		kfree(r);
		return;
	}
	spin_unlock_irqrestore(&umshm_lock, flags);
}
EXPORT_SYMBOL_GPL(umshm_unregister);

/*
 * virtio-gpu host-visible blobs.
 *
 * A host3d blob's pages live in a descriptor the backend owns, so the guest
 * side of the driver asks the host for that descriptor when the blob is
 * mapped (virtio-uml carries the request, see vhost_user.h) and hands it over
 * here.  Registering it is all the fault path needs: phys_mapping() then
 * answers with this fd and the generic os_map_memory() path does the rest.
 */
int uml_virtio_gpu_blob_map(struct virtio_device *vdev, unsigned long paddr,
			    unsigned long len, u32 res_id)
{
	char owner[32];
	int fd, ret;

	ret = virtio_uml_get_blob_fd(vdev, res_id, &fd);
	if (ret)
		return ret;

	snprintf(owner, sizeof(owner), "virtio-gpu:%u", res_id);
	return umshm_register_fd(paddr, len, fd, owner);
}
EXPORT_SYMBOL_GPL(uml_virtio_gpu_blob_map);

void uml_virtio_gpu_blob_unmap(unsigned long paddr)
{
	umshm_unregister(paddr);
}
EXPORT_SYMBOL_GPL(uml_virtio_gpu_blob_unmap);

static ssize_t register_store(struct kobject *kobj, struct kobj_attribute *attr,
			      const char *buf, size_t count)
{
	unsigned long phys, len;
	char path[UMSHM_PATH_MAX];
	int n, ret;

	n = sscanf(buf, "%lx %lx %127s", &phys, &len, path);
	if (n != 3)
		return -EINVAL;

	ret = umshm_register(phys, len, path);
	return ret ? ret : count;
}

static ssize_t unregister_store(struct kobject *kobj, struct kobj_attribute *attr,
				const char *buf, size_t count)
{
	unsigned long phys;
	int n;

	n = sscanf(buf, "%lx", &phys);
	if (n != 1)
		return -EINVAL;

	umshm_unregister(phys);
	return count;
}

static ssize_t list_show(struct kobject *kobj, struct kobj_attribute *attr,
			 char *buf)
{
	struct umshm_region *r;
	unsigned long flags;
	ssize_t n = 0;

	spin_lock_irqsave(&umshm_lock, flags);
	list_for_each_entry(r, &umshm_regions, node) {
		if (n >= PAGE_SIZE - 128)
			break;
		n += sysfs_emit_at(buf, n, "%lx %lx %s\n", r->phys, r->len,
				   r->owner);
	}
	spin_unlock_irqrestore(&umshm_lock, flags);
	return n;
}

static struct kobj_attribute register_attr =
	__ATTR(register, 0200, NULL, register_store);
static struct kobj_attribute unregister_attr =
	__ATTR(unregister, 0200, NULL, unregister_store);
static struct kobj_attribute list_attr =
	__ATTR(list, 0444, list_show, NULL);

static struct attribute *umshm_attrs[] = {
	&register_attr.attr,
	&unregister_attr.attr,
	&list_attr.attr,
	NULL,
};

static const struct attribute_group umshm_group = {
	.attrs = umshm_attrs,
};

static struct kobject *umshm_kobj;

static int __init umshm_init(void)
{
	umshm_kobj = kobject_create_and_add("umshm", kernel_kobj);
	if (!umshm_kobj)
		return -ENOMEM;

	if (sysfs_create_group(umshm_kobj, &umshm_group)) {
		kobject_put(umshm_kobj);
		return -ENOMEM;
	}
	return 0;
}
late_initcall(umshm_init);
