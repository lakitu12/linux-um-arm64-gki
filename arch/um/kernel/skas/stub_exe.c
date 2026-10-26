#include <sys/ptrace.h>
#include <sys/prctl.h>
#include <sys/fcntl.h>
#include <asm/unistd.h>
#include <sysdep/stub.h>
#include <stub-data.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <generated/asm-offsets.h>

void _start(void);

/*
 * Per-task stub threads (docs §12.35).
 *
 * One stub process per mm used to hold exactly one stub thread, and every guest
 * thread of that mm took turns on it.  The slot work turns that into a pool: the
 * first stub thread spawns STUB_MAX_SLOTS-1 more before the seccomp filter goes
 * in, each gets a stack inside its own slot, and each parks in the SIGSYS
 * handler waiting for the kernel to hand it a guest context.
 *
 * clone() cannot be called after the filter is installed: a syscall issued from
 * the stub's code page is checked against the filter's whitelist, and clone is
 * not on it -- KILL_PROCESS, not TRAP.  That is also why these threads park by
 * making a syscall from the *exec'd image*, which is outside the stub's code
 * page and therefore traps: exactly how real_init itself enters the handler.
 *
 * The slot a thread owns needs no argument: its stack lives inside the slot, so
 * get_stub_data() derives the slot from SP (as-layout.h).
 */
#define UML_CLONE_VM		0x00000100
#define UML_CLONE_FS		0x00000200
#define UML_CLONE_FILES		0x00000400
#define UML_CLONE_SIGHAND	0x00000800
#define UML_CLONE_THREAD	0x00010000
#define UML_CLONE_SYSVSEM	0x00040000
#define UML_STUB_CLONE_FLAGS	(UML_CLONE_VM | UML_CLONE_FS | UML_CLONE_FILES | \
				 UML_CLONE_SIGHAND | UML_CLONE_THREAD | \
				 UML_CLONE_SYSVSEM)

/*
 * What a new stub thread does before parking: its own signal stack (per-thread,
 * inside its slot), then a wait for the kernel's go-ahead, then a syscall from
 * outside the stub page, which traps into the SIGSYS handler and parks it there.
 *
 * The wait matters.  Parking is a stub trap, and the kernel's startup handshake
 * only ever watches slot 0; a thread that parks before that handshake finished
 * is a trap nobody is waiting for, and the kernel's view of the stub process
 * would be whatever slot 0 happened to contain.  The go word lives in the
 * slot, so the kernel writes it after start_userspace() has returned.
 */
noinline static void stub_thread_setup(struct stub_data *d)
{
	volatile unsigned int *ctl = STUB_SLOT_CTL_AT((unsigned long)d);
	struct {
		void  *ss_sp;
		int    ss_flags;
		size_t ss_size;
	} stack = {
		.ss_sp = (void *)d->sigstack,
		.ss_size = sizeof(d->sigstack),
	};

	stub_arch_init();
	stub_syscall2(__NR_sigaltstack, (unsigned long)&stack, 0);

	/*
	 * Announce the slot, then wait for the go word.  Plain store, no
	 * compare-exchange: the kernel only writes go *after* it has observed
	 * SETUP, so this store is already ordered before that write by the
	 * kernel's own read.  (Also: __atomic_* on aarch64 would pull in
	 * libatomic, which a -nostdlib stub does not link against.)
	 */
	*ctl = STUB_CTL_SETUP;
	while (!*(volatile unsigned int *)STUB_SLOT_GO_AT((unsigned long)d))
		__asm__ volatile("yield" ::: "memory");

	/* Tell the kernel this trap is a thread parking, not a guest syscall. */
	*ctl = STUB_CTL_PARKING;

	/* Outside the stub page: the filter traps it, the handler parks us and
	 * waits for the kernel to hand this slot a guest context. */
	stub_syscall1(__NR_exit, 31);
	stub_syscall1(__NR_exit_group, 31);
}

/*
 * Wait until every spawned thread has announced itself (SETUP) before the
 * seccomp filter goes in.
 *
 * This is not a nicety: a stub thread's own setup still makes ordinary
 * syscalls -- prctl for PAC, sigaltstack -- and those come from the exec'd
 * image, which the filter turns into a trap.  A thread that reaches sigaltstack
 * after the filter is installed parks in the SIGSYS handler *before* it can
 * announce its slot, so the kernel never sees the slot and, worse, the thread
 * sits in the handler with no slot of its own.  Measured exactly that way:
 * every slot ended up with "ctl=0 futex=0x3", i.e. parked without a name.
 *
 * The bound is real time, not iterations: if a thread never shows up (clone
 * refused, or it died), the boot pays one timeout and carries on with the
 * slots it got.
 */
noinline static void wait_stub_threads_ready(unsigned long slot_base, int slots)
{
	unsigned long start = stub_cycles();
	unsigned long budget = 20000UL * stub_cycles_per_us();	/* 20 ms */
	int i;

	for (i = 1; i < slots; i++) {
		struct stub_data *d = (void *)(slot_base +
					       (unsigned long)i * STUB_SLOT_SIZE);
		volatile unsigned int *ctl = STUB_SLOT_CTL_AT((unsigned long)d);

		while (*ctl != STUB_CTL_SETUP) {
			__asm__ volatile("yield" ::: "memory");
			if (stub_cycles() - start > budget)
				return;
		}
	}
}

noinline static void spawn_stub_threads(unsigned long slot_base, int slots)
{
	int i;

	for (i = 1; i < slots; i++) {
		unsigned long base = slot_base + (unsigned long)i * STUB_SLOT_SIZE;
		unsigned long stack_top = base + STUB_SLOT_SIZE - 16;
		long res;

		/* Where the trampoline left this slot's socket, and which
		 * descriptors it may use to pass descriptors. */
		((struct stub_data *)base)->sock_fd = STUB_SOCK_BASE + i;

		res = stub_syscall5(__NR_clone, UML_STUB_CLONE_FLAGS,
				    stack_top, 0, 0, 0);
		if (res == 0) {
			/* Child: SP is inside slot i's stack area, so the slot
			 * is derived from it -- no argument needed. */
			stub_thread_setup(get_stub_data());
			stub_syscall1(__NR_exit, 32);
		}
	}
}


noinline static void real_init(void)
{
	struct stub_init_data init_data;
	unsigned long res;
	struct {
		void  *ss_sp;
		int    ss_flags;
		size_t ss_size;
	} stack = {
		.ss_size = STUB_SLOT_SIZE,
	};
	struct {
		void *sa_handler_;
		unsigned long sa_flags;
		void *sa_restorer;
		unsigned long long sa_mask;
	} sa = {
		/* Need to set SA_RESTORER (but the handler never returns) */
		.sa_flags = SA_ONSTACK | SA_NODEFER | SA_SIGINFO | 0x04000000,
	};

	/* set a nice name */
	stub_syscall2(__NR_prctl, PR_SET_NAME, (unsigned long)"uml-userspace");

	/* Make sure this process dies if the kernel dies */
	stub_syscall2(__NR_prctl, PR_SET_PDEATHSIG, SIGKILL);

	/* Needed in SECCOMP mode (and safe to do anyway) */
	stub_syscall5(__NR_prctl, PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0);

	/*
	 * Whatever this architecture has to settle in a fresh stub, before the
	 * seccomp filter is installed and so still able to make any syscall.
	 */
	stub_arch_init();

	/* read information from STDIN and close it */
	res = stub_syscall3(__NR_read, 0,
			    (unsigned long)&init_data, sizeof(init_data));
	if (res != sizeof(init_data))
		stub_syscall1(__NR_exit, 10);

	/* In SECCOMP mode, FD 0 is a socket and is later used for FD passing */
	if (!init_data.seccomp)
		stub_syscall1(__NR_close, 0);
	else
		stub_syscall3(__NR_fcntl, 0, F_SETFL, O_NONBLOCK);

	/* map stub code + data */
	res = stub_syscall6(STUB_MMAP_NR,
			    init_data.stub_start, UM_KERN_PAGE_SIZE,
			    PROT_READ | PROT_EXEC, MAP_FIXED | MAP_SHARED,
			    init_data.stub_code_fd, init_data.stub_code_offset);
	if (res != init_data.stub_start)
		stub_syscall1(__NR_exit, 11);

	res = stub_syscall6(STUB_MMAP_NR,
			    init_data.stub_start + STUB_DATA_OFFSET,
			    STUB_DATA_TOTAL_PAGES * UM_KERN_PAGE_SIZE,
			    PROT_READ | PROT_WRITE, MAP_FIXED | MAP_SHARED,
			    init_data.stub_data_fd, init_data.stub_data_offset);
	if (res != init_data.stub_start + STUB_DATA_OFFSET)
		stub_syscall1(__NR_exit, 12);

	/*
	 * In SECCOMP mode, only the signalling descriptors are needed from now
	 * on: fd 0 for slot 0 and [STUB_SOCK_BASE, STUB_SOCK_BASE+STUB_MAX_SLOTS)
	 * for the rest of the pool (userspace_tramp() put them there).  The
	 * memory fd is closed on purpose -- it maps all of physical memory, and
	 * guest code can reach the stub's page, so leaving it open would hand
	 * a guest an escape primitive rather than save a syscall.
	 */
	if (init_data.seccomp) {
		res = stub_syscall3(__NR_close_range, 1, STUB_SOCK_BASE - 1, 0);
		if (res == 0)
			res = stub_syscall3(__NR_close_range,
					    STUB_SOCK_BASE + STUB_MAX_SLOTS,
					    ~0U, 0);
		if (res != 0)
			stub_syscall1(__NR_exit, 13);
	}

	/*
	 * setup signal stack inside slot 0 of the stub data: the same sigstack
	 * every other stub thread uses, so slot 0 can never be an exception
	 * when a frame is written.
	 */
	{
		struct stub_data *d0 = (void *)((unsigned long)init_data.stub_start +
						STUB_DATA_OFFSET);

		stack.ss_sp = (void *)d0->sigstack;
		stack.ss_size = sizeof(d0->sigstack);
		d0->sock_fd = 0;
	}
	stub_syscall2(__NR_sigaltstack, (unsigned long)&stack, 0);

	/* register signal handlers */
	sa.sa_handler_ = (void *) init_data.signal_handler;
	sa.sa_restorer = (void *) init_data.signal_restorer;
	if (!init_data.seccomp) {
		/* In ptrace mode, the SIGSEGV handler never returns */
		sa.sa_mask = 0;

		res = stub_syscall4(__NR_rt_sigaction, SIGSEGV,
				    (unsigned long)&sa, 0, sizeof(sa.sa_mask));
		if (res != 0)
			stub_syscall1(__NR_exit, 14);
	} else {
		/* SECCOMP mode uses rt_sigreturn, need to mask all signals */
		sa.sa_mask = ~0ULL;

		res = stub_syscall4(__NR_rt_sigaction, SIGSEGV,
				    (unsigned long)&sa, 0, sizeof(sa.sa_mask));
		if (res != 0)
			stub_syscall1(__NR_exit, 15);

		res = stub_syscall4(__NR_rt_sigaction, SIGSYS,
				    (unsigned long)&sa, 0, sizeof(sa.sa_mask));
		if (res != 0)
			stub_syscall1(__NR_exit, 16);

		res = stub_syscall4(__NR_rt_sigaction, SIGALRM,
				    (unsigned long)&sa, 0, sizeof(sa.sa_mask));
		if (res != 0)
			stub_syscall1(__NR_exit, 17);

		res = stub_syscall4(__NR_rt_sigaction, SIGTRAP,
				    (unsigned long)&sa, 0, sizeof(sa.sa_mask));
		if (res != 0)
			stub_syscall1(__NR_exit, 18);

		res = stub_syscall4(__NR_rt_sigaction, SIGILL,
				    (unsigned long)&sa, 0, sizeof(sa.sa_mask));
		if (res != 0)
			stub_syscall1(__NR_exit, 19);

		res = stub_syscall4(__NR_rt_sigaction, SIGFPE,
				    (unsigned long)&sa, 0, sizeof(sa.sa_mask));
		if (res != 0)
			stub_syscall1(__NR_exit, 20);
	}

	/*
	 * Spawn the rest of the stub thread pool now: clone is only callable
	 * while no filter is installed, and the threads must exist before
	 * TSYNC so the filter covers them in one go.  How many is the kernel's
	 * decision (stub_slots=): with the pool off there is nothing to spawn
	 * and the mm keeps the single stub thread it always had.
	 */
#ifndef STUB_SPAWN
#define STUB_SPAWN 0
#endif
	if (init_data.seccomp && STUB_SPAWN && init_data.spawn_slots > 1) {
		unsigned long slots = init_data.stub_start + STUB_DATA_OFFSET;

		/*
		 * Exactly the count the kernel asked for, no more: the kernel
		 * releases one go word per slot it knows about, so a thread
		 * spawned past that would sit spinning on a go word nobody
		 * writes for the life of the mm -- a stolen host core per
		 * thread, which is exactly what this pool must never cost.
		 */
		spawn_stub_threads(slots, init_data.spawn_slots);
		/* Their setup syscalls must run before the filter exists. */
		wait_stub_threads_ready(slots, init_data.spawn_slots);
	}

	/*
	 * If in seccomp mode, install the SECCOMP filter and trigger a syscall.
	 * Otherwise set PTRACE_TRACEME and do a SIGSTOP.
	 */
	if (init_data.seccomp) {
		struct sock_filter filter[] = {
#if __BITS_PER_LONG > 32
			/* [0] Load upper 32bit of instruction pointer from seccomp_data */
			BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
				 (offsetof(struct seccomp_data, instruction_pointer) + 4)),

			/* [1] Jump forward 3 instructions if the upper address is not identical */
			BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, (init_data.stub_start) >> 32, 0, 3),
#endif
			/* [2] Load lower 32bit of instruction pointer from seccomp_data */
			BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
				 (offsetof(struct seccomp_data, instruction_pointer))),

			/*
			 * [3] Mask out lower bits.
			 *
			 * This was 0xfffff000, i.e. a hardcoded 4K page. The
			 * filter is checking whether the trapping instruction
			 * pointer lies on the stub's code page, so the mask has
			 * to be the *host's* page mask -- on a 16K-page host
			 * (Android 15 and later) a 4K mask would compare
			 * against the wrong page and reject legitimate stub
			 * syscalls. BPF_K is 32-bit and so is seccomp_data's
			 * view here, hence the cast.
			 */
			BPF_STMT(BPF_ALU | BPF_AND | BPF_K,
				 (unsigned int)UM_KERN_PAGE_MASK),

			/* [4] Jump to [6] if the lower bits are not on the expected page */
			BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K,
				 (init_data.stub_start) & (unsigned int)UM_KERN_PAGE_MASK,
				 1, 0),

			/* [5] Trap call, allow */
			BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_TRAP),

			/* [6,7] Check architecture */
			BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
				 offsetof(struct seccomp_data, arch)),
			BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K,
				 UM_SECCOMP_ARCH_NATIVE, 1, 0),

			/* [8] Kill (for architecture check) */
			BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS),

			/* [9] Load syscall number */
			BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
				 offsetof(struct seccomp_data, nr)),

			/* [10-16] Check against permitted syscalls */
			BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_futex,
				 7, 0),
			BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K,__NR_recvmsg,
				 6, 0),
			BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K,__NR_close,
				 5, 0),
			BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, STUB_MMAP_NR,
				 4, 0),
			BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_munmap,
				 3, 0),
			/*
			 * The syscall the stub needs in order to reinstate
			 * guest TLS, which is architecture-specific:
			 * set_thread_area on i386, arch_prctl on x86-64, and
			 * nothing at all on arm64, where TPIDR_EL0 is written
			 * directly by the stub. STUB_SECCOMP_TLS_SYSCALL is -1
			 * in that case, which no real syscall number can match,
			 * so the slot is inert while every jump offset below
			 * stays as it is.
			 */
			BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K,
				 (unsigned int)STUB_SECCOMP_TLS_SYSCALL,
				 2, 0),
			BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_rt_sigreturn,
				 1, 0),

			/* [17] Not one of the permitted syscalls */
			BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS),

			/* [18] Permitted call for the stub */
			BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
		};
		struct sock_fprog prog = {
			.len = sizeof(filter) / sizeof(filter[0]),
			.filter = filter,
		};

		if (stub_syscall3(__NR_seccomp, SECCOMP_SET_MODE_FILTER,
				  SECCOMP_FILTER_FLAG_TSYNC,
				  (unsigned long)&prog) != 0)
			stub_syscall1(__NR_exit, 21);

		/*
		 * Fall through, the exit syscall will cause SIGSYS.  The pool
		 * is released by the kernel, not from here: see
		 * stub_thread_setup().
		 */
	} else {
		stub_syscall4(__NR_ptrace, PTRACE_TRACEME, 0, 0, 0);

		stub_syscall2(__NR_kill, stub_syscall0(__NR_getpid), SIGSTOP);
	}

	stub_syscall1(__NR_exit, 30);

	__builtin_unreachable();
}

__attribute__((naked)) void _start(void)
{
	/*
	 * Since the stack after exec() starts at the top-most address,
	 * but that's exactly where we also want to map the stub data
	 * and code, this must:
	 *  - push the stack by the code page and every data slot page
	 *  - call real_init()
	 * This way, real_init() can use the stack normally, while the
	 * original stack further down (higher address) will become
	 * inaccessible after the mmap() calls above.
	 */
	stub_start(real_init);
}
