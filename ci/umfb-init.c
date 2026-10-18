/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * umfb gate init: exercise the guest side of the umfb subsystem and
 * leave evidence for the host.
 *
 * What it does: mount devtmpfs (with a mknod fallback), probe
 * /dev/fb0's mode and colour layout, write a known pattern through
 * BOTH guest paths (a base fill and shapes through the guest's own
 * mmap of the framebuffer, one pixel through pwrite), read two pixels
 * back, then power the guest off.
 *
 * What it does not do: judge itself.  The verdict lives on the host
 * (ci/umfb-gate.sh): the markers below must be in the boot log AND the
 * sink file must hold the exact pattern -- the sink is what viewers
 * everywhere read, so the sink is what the gate checks.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>
#include <time.h>
#include <unistd.h>
#include <linux/fb.h>
#include <linux/reboot.h>

/* bytes in memory: R G B A */
#define C_BASE 0xFF332211u	/* R=11 G=22 B=33 */
#define C_SQ   0xFFCCBBAAu	/* R=AA G=BB B=CC */
#define C_PX   0xFF998877u	/* R=77 G=88 B=99 */
#define C_RED  0xFF0000FFu	/* R=FF G=00 B=00 */

#define W_NEED 320
#define H_NEED 200

static void poweroff(void)
{
	sync();
	syscall(SYS_reboot, LINUX_REBOOT_MAGIC1, LINUX_REBOOT_MAGIC2,
		LINUX_REBOOT_CMD_POWER_OFF, NULL);
	for (;;)
		pause();
}

static int mount_dev(void)
{
	mkdir("/dev", 0755);
	if (mount("devtmpfs", "/dev", "devtmpfs", 0, NULL) &&
	    mount("tmpfs", "/dev", "tmpfs", 0, NULL))
		return -1;
	return 0;
}

int main(void)
{
	struct fb_var_screeninfo var;
	struct fb_fix_screeninfo fix;
	unsigned int w, h, bpp, pitch, y, x, rb;
	unsigned char *p;
	int fd;

	setvbuf(stdout, NULL, _IONBF, 0);

	if (mount_dev())
		printf("UMFB_GATE_FAIL mount /dev: %s\n", strerror(errno));

	if (access("/dev/fb0", F_OK))
		mknod("/dev/fb0", S_IFCHR | 0666, makedev(29, 0));

	fd = open("/dev/fb0", O_RDWR);
	if (fd < 0) {
		printf("UMFB_GATE_FAIL open /dev/fb0: %s\n", strerror(errno));
		poweroff();
	}
	if (ioctl(fd, FBIOGET_VSCREENINFO, &var) ||
	    ioctl(fd, FBIOGET_FSCREENINFO, &fix)) {
		printf("UMFB_GATE_FAIL ioctl: %s\n", strerror(errno));
		poweroff();
	}
	w = var.xres;
	h = var.yres;
	bpp = var.bits_per_pixel;
	pitch = fix.line_length;
	printf("UMFB_GATE_INFO %ux%u-%u pitch=%u smem=%u id=%s\n",
	       w, h, bpp, pitch, (unsigned int) fix.smem_len, fix.id);
	printf("UMFB_GATE_VAR r=%u/%u g=%u/%u b=%u/%u a=%u/%u\n",
	       var.red.offset, var.red.length, var.green.offset,
	       var.green.length, var.blue.offset, var.blue.length,
	       var.transp.offset, var.transp.length);
	if (w != W_NEED || h != H_NEED || bpp != 32) {
		printf("UMFB_GATE_FAIL mode %ux%u-%u\n", w, h, bpp);
		poweroff();
	}
	if (!(var.red.offset == 0 && var.red.length == 8 &&
	      var.green.offset == 8 && var.green.length == 8 &&
	      var.blue.offset == 16 && var.blue.length == 8)) {
		printf("UMFB_GATE_FAIL colour layout\n");
		poweroff();
	}

	p = mmap(NULL, (size_t) fix.smem_len, PROT_READ | PROT_WRITE,
		 MAP_SHARED, fd, 0);
	if (p == MAP_FAILED) {
		printf("UMFB_GATE_FAIL mmap: %s\n", strerror(errno));
		poweroff();
	}

	for (y = 0; y < h; y++) {
		unsigned int *row = (unsigned int *)(p + (size_t) y * pitch);

		for (x = 0; x < w; x++)
			row[x] = C_BASE;
	}
	for (y = 64; y < 128; y++)
		for (x = 64; x < 128; x++)
			*(unsigned int *)(p + (size_t) y * pitch + x * 4) = C_SQ;
	for (y = 150; y < 160; y++)
		for (x = 0; x < 32; x++)
			*(unsigned int *)(p + (size_t) y * pitch + x * 4) = C_RED;

	rb = C_PX;
	if (pwrite(fd, &rb, 4, (off_t) 100 * pitch + 10 * 4) != 4) {
		printf("UMFB_GATE_FAIL pwrite: %s\n", strerror(errno));
		poweroff();
	}

	if (pread(fd, &rb, 4, (off_t) 64 * pitch + 64 * 4) != 4 ||
	    rb != C_SQ) {
		printf("UMFB_GATE_FAIL read-back mmap %08x\n", rb);
		poweroff();
	}
	if (pread(fd, &rb, 4, (off_t) 100 * pitch + 10 * 4) != 4 ||
	    rb != C_PX) {
		printf("UMFB_GATE_FAIL read-back pwrite %08x\n", rb);
		poweroff();
	}
	printf("UMFB_GATE_WRITTEN\n");

	/* let the pump publish several rounds */
	{
		struct timespec ts = { .tv_sec = 2, .tv_nsec = 0 };

		nanosleep(&ts, NULL);
	}

	printf("UMFB_GATE_DONE\n");
	poweroff();
}