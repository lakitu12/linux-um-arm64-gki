/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2021 Intel Corporation
 * Author: Johannes Berg <johannes@sipsolutions.net>
 */

#ifndef __VIRTIO_UML_H__
#define __VIRTIO_UML_H__

#include <linux/types.h>

struct virtio_device;

void virtio_uml_set_no_vq_suspend(struct virtio_device *vdev,
				  bool no_vq_suspend);

#if IS_ENABLED(CONFIG_VIRTIO_UML)
/*
 * Ask the host for the descriptor behind a virtio-gpu host-visible blob.  On
 * success the caller owns *fd.  Returns -ENODEV when the device is not a
 * virtio-uml device (a kernel without the transport, or another transport).
 */
int virtio_uml_get_blob_fd(struct virtio_device *vdev, u32 res_id, int *fd);
#else
static inline int virtio_uml_get_blob_fd(struct virtio_device *vdev, u32 res_id,
					 int *fd)
{
	return -ENODEV;
}
#endif

#endif /* __VIRTIO_UML_H__ */
