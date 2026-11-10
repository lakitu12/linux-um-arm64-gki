// SPDX-License-Identifier: GPL-2.0
/*
 * Regression for how arm64 classifies cache-maintenance aborts: cache ops on an
 * untouched executable alias must be allowed, an ordinary store into an RX
 * mapping must still be refused, and a genuine anonymous write must still be
 * fork-COW.  Runs natively on Android or as /init of a minimal initramfs.
 * Only CACHECM_PASS counts as success; CACHECM_DONE and a clean shutdown do not.
 *
 * Build: aarch64-linux-gnu-gcc -static -O2 -Wall -Wextra -o cachecm cachecm.c
 *    or: NDK clang --target=aarch64-linux-android30 -static -O2 -o cachecm cachecm.c
 */
#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/reboot.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef __aarch64__
#error "cachecm requires aarch64"
#endif

static size_t page_size;
static const void *volatile expected_fault_address;
#define RX_FAULT_VERIFIED 80

static void rx_fault_handler(int signal_number, siginfo_t *info, void *context)
{
	static const char verified[] = "CACHECM_RX_FAULT VERIFIED\n";
	static const char unexpected[] = "CACHECM_RX_FAULT UNEXPECTED\n";
	int ok = signal_number == SIGSEGV && info->si_code == SEGV_ACCERR &&
		 info->si_addr == expected_fault_address;

	(void)context;
	/* async-signal-safe calls only; a mismatch has to reach the parent as FAIL. */
	if (ok)
		(void)write(STDOUT_FILENO, verified, sizeof(verified) - 1);
	else
		(void)write(STDOUT_FILENO, unexpected, sizeof(unexpected) - 1);
	_exit(ok ? RX_FAULT_VERIFIED : 81);
}

static void expect_rx_fault(const void *address)
{
	struct sigaction action = { 0 };

	expected_fault_address = address;
	action.sa_sigaction = rx_fault_handler;
	action.sa_flags = SA_SIGINFO;
	sigemptyset(&action.sa_mask);
	if (sigaction(SIGSEGV, &action, NULL)) {
		perror("sigaction RX");
		_exit(2);
	}
}

static void fail(const char *what)
{
	perror(what);
	_exit(2);
}

static void alias_case(int which)
{
	int fd = syscall(SYS_memfd_create, "cachecm", 0);
	volatile unsigned char *rw, *rx;
	void *address;

	if (fd < 0)
		fail("memfd_create");
	if (ftruncate(fd, page_size))
		fail("ftruncate");
	rw = mmap(NULL, page_size, PROT_READ | PROT_WRITE,
		  MAP_SHARED, fd, 0);
	if (rw == MAP_FAILED)
		fail("mmap RW");
	/* Low hint only so the diagnostic log is easy to read: no MAP_FIXED, no MAP_POPULATE. */
	address = mmap((void *)0x64000000UL, page_size,
		       PROT_READ | PROT_EXEC, MAP_SHARED, fd, 0);
	if (address == MAP_FAILED)
		fail("mmap RX");
	rx = address;
	close(fd);
	rw[0x3c0] = 0x5a;
	printf("CACHECM_CASE %d rw=%p rx=%p offset=0x3c0\n",
	       which, (const void *)rw, (const void *)rx);
	fflush(stdout);

	switch (which) {
	case 0:
		asm volatile("dc cvau, %0\n\tdsb ish"
			     : : "r"(rx + 0x3c0) : "memory");
		break;
	case 1:
		asm volatile("ic ivau, %0\n\tdsb ish\n\tisb"
			     : : "r"(rx + 0x3c0) : "memory");
		break;
	case 2:
		__builtin___clear_cache((char *)rx + 0x3c0,
				       (char *)rx + 0x400);
		break;
	case 3:
		/* The first access is an ordinary store: an unfaulted RX page must not slip through. */
		expect_rx_fault((const void *)(rx + 0x3c0));
		rx[0x3c0] = 0xa5;
		_exit(3);
	case 4:
		/* Same for an RX page that has already been read in. */
		if (rx[0x3c0] != 0x5a)
			_exit(4);
		expect_rx_fault((const void *)(rx + 0x3c0));
		rx[0x3c0] = 0xa5;
		_exit(3);
	}

	if (rx[0x3c0] != 0x5a)
		_exit(4);
	munmap((void *)rw, page_size);
	munmap((void *)rx, page_size);
	_exit(0);
}

static int run_case(int which, const char *name, int expected_signal)
{
	int status;
	pid_t child = fork();

	if (child < 0) {
		perror("fork");
		return 1;
	}
	if (!child) {
		signal(SIGSEGV, SIG_DFL);
		alias_case(which);
	}
	if (waitpid(child, &status, 0) != child) {
		perror("waitpid");
		return 1;
	}
	int expected_exit = expected_signal ? RX_FAULT_VERIFIED : 0;
	int ok = WIFEXITED(status) && WEXITSTATUS(status) == expected_exit;
	printf("CACHECM_RESULT %s %s status=0x%x expected_exit=%d\n",
	       name, ok ? "PASS" : "FAIL", status, expected_exit);
	fflush(stdout);
	return !ok;
}

static int cow_case(void)
{
	volatile unsigned char *p = mmap(NULL, page_size,
			PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS,
			-1, 0);
	int status;
	pid_t child;

	if (p == MAP_FAILED) {
		perror("mmap COW");
		return 1;
	}
	p[0] = 0x31;
	child = fork();
	if (child < 0) {
		perror("fork COW");
		munmap((void *)p, page_size);
		return 1;
	}
	if (!child) {
		if (p[0] != 0x31)
			_exit(6);
		p[0] = 0x72;
		_exit(p[0] == 0x72 ? 0 : 5);
	}
	if (waitpid(child, &status, 0) != child) {
		perror("waitpid COW");
		munmap((void *)p, page_size);
		return 1;
	}
	int ok = WIFEXITED(status) && WEXITSTATUS(status) == 0 && p[0] == 0x31;
	printf("CACHECM_RESULT fork-cow %s status=0x%x parent=0x%x\n",
	       ok ? "PASS" : "FAIL", status, (unsigned int)p[0]);
	fflush(stdout);
	munmap((void *)p, page_size);
	return !ok;
}

int main(void)
{
	struct rlimit core_limit = { 0, 0 };
	int failures = 0;
	long size = sysconf(_SC_PAGESIZE);

	setvbuf(stdout, NULL, _IONBF, 0);
	setrlimit(RLIMIT_CORE, &core_limit);
	if (size < 1024) {
		fprintf(stderr, "CACHECM_FAIL invalid page size %ld\n", size);
		return 2;
	}
	page_size = size;
	printf("CACHECM_START page_size=%zu pid=%d\n", page_size, getpid());
	failures += run_case(0, "dc-cvau-unfaulted-rx", 0);
	failures += run_case(1, "ic-ivau-unfaulted-rx", 0);
	failures += run_case(2, "clear-cache-unfaulted-rx", 0);
	failures += run_case(3, "store-unfaulted-rx", SIGSEGV);
	failures += run_case(4, "store-populated-rx", SIGSEGV);
	failures += cow_case();
	printf("CACHECM_DONE failures=%d\n", failures);
	printf("CACHECM_%s\n", failures ? "FAIL" : "PASS");
	if (getpid() == 1) {
		reboot(RB_POWER_OFF);
		for (;;)
			pause();
	}
	return failures ? 1 : 0;
}
