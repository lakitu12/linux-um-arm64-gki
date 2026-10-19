// SPDX-License-Identifier: GPL-2.0-only
/*
 * vgprobe-init.c -- the init of the virtio-gpu gate (ci/virtio-gpu-gate.sh).
 *
 * Boots inside a bare initramfs, reports what the guest's display stack became
 * (proc/fb, the fb0 geometry), POKES it with pwrite, and powers off.  The
 * verdict is decided outside (the sink the vhost-user backend writes + the
 * backend's own log), never by a status code from here.
 *
 * pwrite, not mmap: a store through an fb mapping is invisible to DRM's fbdev
 * emulation -- damage is only noted in the fb write path, so an mmap write
 * would leave the backend with nothing to transfer and the gate would fail for
 * a reason that has nothing to do with virtio-gpu.  (umfb-init.c discovered the
 * same thing and probes both routes; here only the write path matters.)
 */
#include "gate-out.h"

#include <fcntl.h>
#include <linux/fb.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <unistd.h>

static void emit(const char *fmt, ...)
{
	char b[512];
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(b, sizeof(b), fmt, ap);
	va_end(ap);
	gate_emit(b, n);
}

static void poweroff_now(void)
{
	sync();
	emit("VG_GATE_POWEROFF\n");
	reboot(RB_POWER_OFF);
	for (;;)
		pause();
}

int main(void)
{
	struct fb_fix_screeninfo fix;
	struct fb_var_screeninfo var;
	int fd, n, f;
	char b[512];
	uint32_t red = 0x00ff0000;          /* XRGB8888: red */
	int x, y;

	gate_out_init();

	/* What did the guest end up with?  virtio-gpu registers as fb0 through
	 * DRM fbdev emulation; seeing "virtio_gpu" here is the driver binding. */
	f = open("/proc/fb", O_RDONLY);
	if (f >= 0) {
		n = read(f, b, sizeof(b) - 1);
		if (n > 0) {
			b[n] = 0;
			for (int i = 0; i < n; i++)
				if (b[i] == '\n')
					b[i] = ' ';
			emit("VG_GATE_PROC_FB %s\n", b);
		}
		close(f);
	} else {
		emit("VG_GATE_PROC_FB open errno=%d (%s)\n", errno, strerror(errno));
	}

	fd = open("/dev/fb0", O_RDWR);
	if (fd < 0) {
		emit("VG_GATE_FAIL open /dev/fb0: %s\n", strerror(errno));
		emit("VG_GATE_DRM_LIST\n");
		/* list /dev/dri to say whether DRM came up at all */
		for (int i = 0; i < 8; i++) {
			char p[32];
			snprintf(p, sizeof(p), "/dev/dri/card%d", i);
			if (access(p, F_OK) == 0)
				emit("VG_GATE_HAS %s\n", p);
		}
		poweroff_now();
	}

	if (ioctl(fd, FBIOGET_FSCREENINFO, &fix) ||
	    ioctl(fd, FBIOGET_VSCREENINFO, &var)) {
		emit("VG_GATE_FAIL ioctl: %s\n", strerror(errno));
		poweroff_now();
	}
	emit("VG_GATE_INFO %ux%u-%u pitch=%u smem=%u id=%s\n",
	     var.xres, var.yres, var.bits_per_pixel, fix.line_length,
	     fix.smem_len, fix.id);

	/* Paint a 64x64 red block at the origin through the write path. */
	for (y = 0; y < 64; y++)
		for (x = 0; x < 64; x++)
			if (pwrite(fd, &red, 4,
				   (off_t)y * fix.line_length + (off_t)x * 4) != 4) {
				emit("VG_GATE_FAIL pwrite y=%d x=%d: %s\n",
				     y, x, strerror(errno));
				poweroff_now();
			}
	/* A block far from the origin too: a backend that only handles the first
	 * tile of a resource would still pass a single-block test. */
	for (y = 300; y < 340; y++)
		for (x = 500; x < 540; x++)
			(void)!pwrite(fd, &red, 4,
				      (off_t)y * fix.line_length + (off_t)x * 4);

	emit("VG_GATE_PAINTED 64x64@0,0 and 40x40@500,300\n");
	fsync(fd);
	emit("VG_GATE_PASS\n");
	close(fd);
	poweroff_now();
	return 0;
}
