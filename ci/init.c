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
 *
 * Alongside the marker, the CPU count this process may run on: SMP is a
 * build option until a boot proves the kernel brought the CPUs up, and
 * boot-gate.sh reads this line rather than inferring it from the config.
 * sched_getaffinity() rather than /proc: no mount is needed for it.
 */
#define _GNU_SOURCE	/* cpu_set_t and sched_getaffinity() */
#include <sched.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <linux/reboot.h>

#include "gate-out.h"

static long gate_online_cpus(void)
{
	cpu_set_t set;

	CPU_ZERO(&set);
	if (sched_getaffinity(0, sizeof(set), &set))
		return -1;
	return CPU_COUNT(&set);
}

int main(void)
{
	static const char msg[] = "UMARM_GKI618_BOOT_OK\n";
	struct timespec ts = { .tv_sec = 0, .tv_nsec = 200 * 1000 * 1000 };
	char line[64];
	long cpus;
	int n;

	gate_out_init();
	cpus = gate_online_cpus();
	if (cpus > 0)
		n = snprintf(line, sizeof(line), "UMARM_GKI618_CPUS=%ld\n", cpus);
	else
		n = snprintf(line, sizeof(line), "UMARM_GKI618_CPUS=unknown\n");
	gate_emit(line, n);
	gate_emit(msg, sizeof(msg) - 1);
	nanosleep(&ts, NULL);
	sync();
	syscall(SYS_reboot, LINUX_REBOOT_MAGIC1, LINUX_REBOOT_MAGIC2,
		LINUX_REBOOT_CMD_POWER_OFF, NULL);

	for (;;)
		pause();
}
