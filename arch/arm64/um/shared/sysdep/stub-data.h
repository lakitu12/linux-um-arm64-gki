/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __ARCH_STUB_DATA_H
#define __ARCH_STUB_DATA_H

/*
 * Guest TLS on arm64 is TPIDR_EL0, which userspace owns outright: the guest
 * sets it with "msr tpidr_el0, xN" and never asks the kernel. There is no
 * set_thread_area/arch_prctl equivalent to intercept, so the only thing UML has
 * to do is reinstate the register when it resumes a different guest thread in
 * the same stub process.
 */
#define STUB_SYNC_TLS (1 << 0)

struct stub_data_arch {
	int sync;
	unsigned long tls;
	/*
	 * Host CLOCK_MONOTONIC, in nanoseconds, at the moment the guest's
	 * timekeeping took its base. The guest's clocksource is this same host
	 * clock divided by TIMER_MULTIPLIER, so
	 *
	 *	guest CLOCK_MONOTONIC = host CLOCK_MONOTONIC - mono_ns_off
	 *
	 * and the stub can answer clock_gettime(CLOCK_MONOTONIC) with one host
	 * syscall instead of a full trap into the guest kernel (measured:
	 * ~20us per trap against ~1us here, and CLOCK_MONOTONIC is about a
	 * third of every syscall a cold-booting Android makes).
	 *
	 * CLOCK_REALTIME needs no offset: both sides use the same epoch.
	 */
	unsigned long long mono_ns_off;
};

#endif /* __ARCH_STUB_DATA_H */
