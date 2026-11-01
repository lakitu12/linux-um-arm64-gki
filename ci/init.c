/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * CI boot-gate init: an initramfs /init that prints the gate marker and
 * then powers the (UML) guest off.
 *
 * The workflow's verdict is the marker string in the boot log, never this
 * process's exit status — a truncated log must not pass. If the poweroff
 * syscall fails (or UML does not implement poweroff), park here so the
 * marker is already on the console and the run ends by timeout instead.
 *
 * The marker goes out on two paths (ci/gate-out.h): /dev/kmsg, i.e. printk,
 * and fd 1.  A VT-enabled kernel takes major 4 away from the UML console,
 * so /dev/console cannot be opened and init's stdio stays whatever the
 * kernel inherited — the log then lost the marker entirely while the boot
 * itself was fine (run 39141339058).  printk keeps a working console
 * either way, so kmsg is the path that cannot depend on tty setup.
 */
#include <time.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <linux/reboot.h>

#include "gate-out.h"

int main(void)
{
	static const char msg[] = "UMARM_GKI618_BOOT_OK\n";
	struct timespec ts = { .tv_sec = 0, .tv_nsec = 200 * 1000 * 1000 };

	gate_out_init();
	gate_emit(msg, sizeof(msg) - 1);
	nanosleep(&ts, NULL);
	sync();
	syscall(SYS_reboot, LINUX_REBOOT_MAGIC1, LINUX_REBOOT_MAGIC2,
		LINUX_REBOOT_CMD_POWER_OFF, NULL);

	for (;;)
		pause();
}
