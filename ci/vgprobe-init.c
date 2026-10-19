// SPDX-License-Identifier: GPL-2.0-only
/*
 * vgprobe-init.c -- the init of the virtio-gpu gate (ci/virtio-gpu-gate.sh).
 *
 * Boots inside a bare initramfs, reports what the guest's display stack became
 * (proc/fb, the fb0 geometry), PAINTS the framebuffer, forces one modeset and
 * powers off.  The verdict is decided outside (the sink the vhost-user backend
 * writes + the backend's own log), never by a status code from here.
 *
 * Painting goes through mmap: a store to a MISSING pte calls into
 * fb_deferred_io's page_mkwrite, which is what queues the deferred work whose
 * dirty callback becomes RESOURCE_TRANSFER_TO_HOST_2D + RESOURCE_FLUSH.  The
 * pwrite path only updates the shadow buffer (fb_sys_write; no pageref, so no
 * work is ever queued) -- run 39238602845 proved it: fb0 was up, the probe
 * passed, and the backend still recorded flush=0.
 *
 * The forced FBIOPUT_VSCREENINFO is the other half: fb_set_var() bails out
 * unless the var differs or FB_ACTIVATE_FORCE is set, and a plane that never
 * got a set_par() commit holds no fb, so a damage-only commit has no plane
 * state to update and nothing reaches the wire.
 */
#include "gate-out.h"

#include <fcntl.h>
#include <linux/fb.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
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
	uint32_t *pix;
	size_t stride;
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

	pix = mmap(NULL, fix.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (pix == MAP_FAILED) {
		emit("VG_GATE_FAIL mmap fb0: %s\n", strerror(errno));
		poweroff_now();
	}
	stride = fix.line_length / 4;

	/* Paint a 64x64 red block at the origin, and one far from it: a backend
	 * that only handles the first tile of a resource would still pass a
	 * single-block test. */
	for (y = 0; y < 64; y++)
		for (x = 0; x < 64; x++)
			pix[y * stride + x] = red;
	for (y = 300; y < 340; y++)
		for (x = 500; x < 540; x++)
			pix[y * stride + x] = red;

	emit("VG_GATE_PAINTED 64x64@0,0 and 40x40@500,300\n");

	/* The mode did not change, so ask for the commit explicitly. */
	var.activate = FB_ACTIVATE_FORCE;
	if (ioctl(fd, FBIOPUT_VSCREENINFO, &var)) {
		emit("VG_GATE_WARN FBIOGET_VSCREENINFO force: %s\n", strerror(errno));
	}
	/* msync/fsync run the deferred-IO work now instead of 50 ms from now:
	 * fb_deferred_io_fsync() is flush_delayed_work() on the damage work. */
	msync(pix, fix.smem_len, MS_SYNC);
	fsync(fd);
	emit("VG_GATE_PASS\n");
	close(fd);
	poweroff_now();
	return 0;
}
