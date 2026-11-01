/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * gate-out.h — one output path every CI gate init can trust.
 *
 * The gates boot the kernel with `con=null con0=null,fd:1` and grep the
 * boot log for markers that the initramfs /init prints.  With CONFIG_VT
 * enabled that assumption breaks in a way that looked like "the kernel
 * died before init" (run 39141339058): the marker never reached the log,
 * yet the guest had booted and powered itself off cleanly.
 *
 * Writing to /dev/kmsg (major 1, minor 11) goes through printk instead of
 * a tty, and printk's console list in UML still carries the stdio console
 * (patch 0070 keeps its channel alive when VT owns major 4) plus mc-1.
 * So every line is emitted twice: through /dev/kmsg and through fd 1.
 * If fd 1 is unusable the reason is reported over kmsg, because a silent
 * "one of the two paths died" is exactly the failure this header exists
 * to rule out.
 *
 * Diagnostics (never part of a verdict): GATE_STDIO reports what init's
 * fd 1 actually is — a FIFO (the tee pipe), a character device (the
 * kernel re-pointed stdio at /dev/console), or gone (EBADF).
 */
#ifndef GATE_OUT_H
#define GATE_OUT_H

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

static int gate_kmsg = -1;

/* devtmpfs is normally mounted by the kernel (CONFIG_DEVTMPFS_MOUNT); the
 * tmpfs fallback plus the explicit mknod below cover a bare initramfs. */
static int gate_mount_dev(void)
{
	mkdir("/dev", 0755);
	if (mount("devtmpfs", "/dev", "devtmpfs", 0, NULL) &&
	    mount("tmpfs", "/dev", "tmpfs", 0, NULL))
		return -1;
	return 0;
}

static void gate_kmsg_puts(const char *buf, int n)
{
	if (gate_kmsg >= 0 && n > 0)
		(void)!write(gate_kmsg, buf, n);
}

static void gate_report_stdio(void)
{
	char b[160];
	struct stat st;
	int n;

	if (fstat(1, &st) == 0) {
		const char *kind = S_ISFIFO(st.st_mode) ? "fifo" :
				   S_ISCHR(st.st_mode) ? "chr" :
				   S_ISREG(st.st_mode) ? "reg" : "other";
		n = snprintf(b, sizeof(b),
			     "GATE_STDIO fd1 kind=%s mode=%o rdev=%u:%u\n",
			     kind, (unsigned)st.st_mode,
			     (unsigned)major(st.st_rdev), (unsigned)minor(st.st_rdev));
	} else {
		n = snprintf(b, sizeof(b), "GATE_STDIO fd1 fstat errno=%d (%s)\n",
			     errno, strerror(errno));
	}
	gate_kmsg_puts(b, n);
}

/* Call first in main(): unbuffered stdout, /dev/dev + /dev/kmsg open, and
 * the fd1 identity line in the log. */
static void gate_out_init(void)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	(void)gate_mount_dev();
	if (access("/dev/kmsg", F_OK))
		(void)mknod("/dev/kmsg", S_IFCHR | 0600, makedev(1, 11));
	gate_kmsg = open("/dev/kmsg", O_WRONLY);
	gate_report_stdio();
	if (gate_kmsg < 0) {
		char b[96];
		int n = snprintf(b, sizeof(b), "GATE_WARN no /dev/kmsg: %s\n",
				 strerror(errno));
		(void)!write(1, b, n);
	}
}

/* Emit one chunk on both paths; report a failing fd 1 over kmsg only, so
 * the report itself cannot loop. */
static void gate_emit(const char *buf, int n)
{
	if (n <= 0)
		return;
	gate_kmsg_puts(buf, n);
	if (write(1, buf, n) < 0) {
		char b[96];
		int m = snprintf(b, sizeof(b), "GATE_FD1_LOST errno=%d (%s)\n",
				 errno, strerror(errno));
		gate_kmsg_puts(b, m);
	}
}

#endif /* GATE_OUT_H */
