// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2015 Thomas Meyer (thomas@m3y3r.de)
 * Copyright (C) 2002 - 2007 Jeff Dike (jdike@{addtoit,linux.intel}.com)
 */

#include <linux/mm.h>
#include <linux/sched/signal.h>
#include <linux/slab.h>
#include <linux/ktime.h>
#include <linux/wait.h>

#include <shared/init.h>
#include <shared/irq_kern.h>
#include <asm/pgalloc.h>
#include <asm/sections.h>
#include <asm/mmu_context.h>
#include <as-layout.h>
#include <os.h>
#include <skas.h>
#include <stub-data.h>

/* Ensure the stub_data struct covers the allocated area */
/* The data part of a slot must cover a whole struct stub_data */
static_assert(sizeof(struct stub_data) == STUB_DATA_PAGES * UM_KERN_PAGE_SIZE);

static spinlock_t mm_list_lock;
static struct list_head mm_list;

void enter_turnstile(struct mm_id *mm_id) __acquires(turnstile)
{
	struct mm_context *ctx = container_of(mm_id, struct mm_context, id);

	mutex_lock(&ctx->turnstile);
}

void exit_turnstile(struct mm_id *mm_id) __releases(turnstile)
{
	struct mm_context *ctx = container_of(mm_id, struct mm_context, id);

	mutex_unlock(&ctx->turnstile);
}

/*
 * Hand a task a task slot of its own (docs §12.35).
 *
 * The pool is a bitmap in the mm: a set bit is a slot whose stub thread is
 * parked and free.  Slot 0 is never in it -- it is the mm's control slot (see
 * mm_id.h) -- so a claimed slot is always >= 1, and an mm with no pool (ptrace
 * mode, or a stub built without STUB_SPAWN) always answers 0, which is the
 * single slot it has always had.
 *
 * Finding no free slot is a normal state, not an error: it means every stub
 * thread of this mm is already in guest user mode.  Waiting is bounded by how
 * long a slot holder can stay there, i.e. about one scheduling quantum.  The
 * wait must happen without the turnstile (a holder may need it), which is why
 * callers claim before taking it.
 */
int stub_slot_get(struct mm_id *mm_id)
{
	struct mm_context *ctx = container_of(mm_id, struct mm_context, id);
	unsigned int spins = 0;
	int i;

	if (mm_id->slot_count <= 1)
		return 0;

	for (;;) {
		int hint = current->thread.stub_slot;
		int slot;

		guard(mutex)(&ctx->slot_lock);

		/*
		 * Sticky first: a task that ran guest code before goes back to
		 * the same stub thread whenever it can.  That keeps the guest
		 * task on one host thread, and therefore on one host core,
		 * which is not a micro-optimisation -- measured on this
		 * device, a single-threaded guest workload that was spread
		 * over the pool ran 4.3x slower because most of its entries
		 * landed on little cores.
		 */
		if (hint >= 1 && hint < mm_id->slot_count &&
		    (mm_id->slot_free & (1UL << hint))) {
			mm_id->slot_free &= ~(1UL << hint);
			return hint;
		}

		/* Otherwise the round-robin scan, so new tasks spread out. */
		slot = mm_id->slot_next < 1 ||
		       mm_id->slot_next >= mm_id->slot_count ?
		       1 : mm_id->slot_next;
		for (i = 0; i < mm_id->slot_count - 1; i++) {
			if (slot >= mm_id->slot_count)
				slot = 1;
			if (mm_id->slot_free & (1UL << slot)) {
				mm_id->slot_free &= ~(1UL << slot);
				mm_id->slot_next = slot + 1 < mm_id->slot_count ?
						   slot + 1 : 1;
				current->thread.stub_slot = slot;
				return slot;
			}
			slot++;
		}

		if (++spins < 1000)
			cpu_relax();
		else {
			spins = 0;
			schedule_timeout_interruptible(1);
		}
	}
}

void stub_slot_put(struct mm_id *mm_id, int slot)
{
	struct mm_context *ctx = container_of(mm_id, struct mm_context, id);

	if (slot < 1 || slot >= mm_id->slot_count)
		return;

	scoped_guard(mutex, &ctx->slot_lock)
		mm_id->slot_free |= 1UL << slot;
}

static bool slot_report_done;
/* How many task slots the stub actually has; settled by the first mm. */
static int stub_pool_slots = 1;
/*
 * Upper bound on the pool, from the command line.  The stub always spawns
 * STUB_MAX_SLOTS threads (it cannot know this at the time -- it spawns before
 * the filter goes in, and the cap is a kernel-side choice), but the kernel
 * does not have to *use* them all.  stub_slots=1 keeps the old single-slot
 * behaviour on the same kernel, which is what makes an A/B on one boot image
 * possible; the number also decides how many tasks of an mm may be in guest
 * user mode at once.
 */
static int stub_slots_max = STUB_MAX_SLOTS;

static int __init uml_stub_slots_setup(char *line, int *add)
{
	int n = simple_strtol(line, NULL, 0);

	if (n < 1)
		n = 1;
	if (n > STUB_MAX_SLOTS)
		n = STUB_MAX_SLOTS;
	stub_slots_max = n;

	*add = 0;
	return 0;
}

__uml_setup("stub_slots=", uml_stub_slots_setup,
"stub_slots=<n>\n"
"    How many task slots of an mm may hold guest user mode at once\n"
"    (docs §12.35). Each slot is one thread of the mm's stub process, so\n"
"    n-1 concurrent guest threads can run; the stub spawns all 8 threads\n"
"    regardless, this only caps how many the kernel hands work to. 1 keeps\n"
"    the single-stub behaviour, including its serialisation.\n"
"    Default: 8 (STUB_MAX_SLOTS).\n\n");

int init_new_context(struct task_struct *task, struct mm_struct *mm)
{
	struct mm_id *new_id = &mm->context.id;
	unsigned long stack = 0;
	int i, ret = -ENOMEM;

	mutex_init(&mm->context.turnstile);
	mutex_init(&mm->context.slot_lock);
	spin_lock_init(&mm->context.sync_tlb_lock);

	/*
	 * Fault-around detector starts cold: prefault_level is used as an
	 * array index in trap.c, so it must never be garbage.
	 */
	mm->context.prefault_next = 0;
	mm->context.prefault_level = 0;

	stack = __get_free_pages(GFP_KERNEL | __GFP_ZERO,
				 ilog2(STUB_DATA_TOTAL_PAGES));
	if (stack == 0) {
		printk(KERN_ERR "%s: failed to allocate %d stub data pages\n",
		       __func__, STUB_DATA_TOTAL_PAGES);
		goto out;
	}

	new_id->stack = stack;
	new_id->syscall_data_len = 0;
	new_id->syscall_fd_num = 0;

	scoped_guard(spinlock_irqsave, &mm_list_lock) {
		/* Insert into list, used for lookups when the child dies */
		list_add(&mm->context.list, &mm_list);
	}

	ret = start_userspace(new_id);
	if (ret < 0) {
		printk(KERN_ERR "%s: start_userspace failed: %d\n",
		       __func__, ret);
		goto out_free;
	}

	/*
	 * The stub thread pool.  Each extra slot has a thread that announced
	 * itself before the seccomp filter went in and is now parked on the go
	 * word in its slot; releasing that word is a latch, not a handshake, so
	 * it happens for every slot up front.  A thread that has not been
	 * scheduled yet simply finds the word already set when it gets there.
	 *
	 * Never leave one unreleased: a released slot parks, an unreleased one
	 * spins for as long as the mm lives, and the guest would pay for it as
	 * stolen host CPUs.
	 */
	for (i = 1; i < STUB_MAX_SLOTS; i++) {
		struct stub_data *sd = STUB_DATA_SLOT(new_id, i);

		WRITE_ONCE(*STUB_SLOT_GO_AT((unsigned long)sd), 1u);
	}
	smp_wmb();

	/*
	 * Best-effort census, for the one line below and for a person reading
	 * a boot log.  The pool is an optimisation, so nothing here blocks the
	 * mm: a thread that is slow to be scheduled is counted next time.
	 */
	new_id->slots_ready = 1;	/* slot 0 is the exec'd stub thread */
	if (!slot_report_done) {
		unsigned long long deadline = ktime_get_ns() +
					       20ull * NSEC_PER_MSEC;

		do {
			new_id->slots_ready = 1;
			for (i = 1; i < STUB_MAX_SLOTS; i++) {
				struct stub_data *sd = STUB_DATA_SLOT(new_id, i);

				if (READ_ONCE(*STUB_SLOT_CTL_AT((unsigned long)sd)) ==
						STUB_CTL_PARKING &&
				    STUB_FUTEX_OWNER(READ_ONCE(sd->futex)) ==
						FUTEX_IN_KERN)
					new_id->slots_ready++;
			}
			if (new_id->slots_ready == STUB_MAX_SLOTS)
				break;
			cpu_relax();
		} while (ktime_get_ns() < deadline);

		slot_report_done = true;
		/*
		 * The pool size is a property of the stub, so the first mm's
		 * census settles it for every later mm too: they all run the
		 * same stub binary on the same kernel. Counting per mm would
		 * cost a 20 ms wait on every process creation.
		 */
		stub_pool_slots = min_t(int, new_id->slots_ready, stub_slots_max);
		printk(KERN_INFO "stub: %d of %d task slots parked for the first mm\n",
		       new_id->slots_ready, STUB_MAX_SLOTS);
		if (new_id->slots_ready != STUB_MAX_SLOTS) {
			for (i = 1; i < STUB_MAX_SLOTS; i++) {
				struct stub_data *sd = STUB_DATA_SLOT(new_id, i);

				printk(KERN_INFO "stub: slot %d ctl=%u futex=0x%x\n",
				       i,
				       READ_ONCE(*STUB_SLOT_CTL_AT((unsigned long)sd)),
				       READ_ONCE(sd->futex));
			}
		}
	}

	/*
	 * Publish the pool.  Slot 0 stays out of it: it is the control slot
	 * (see mm_id.h), so a claimable slot is always >= 1.
	 */
	new_id->slot_count = stub_pool_slots < 1 ? 1 : stub_pool_slots;
	new_id->slot_next = 1;
	new_id->slot_free = (((1UL << new_id->slot_count) - 1) & ~1UL);

	/* Ensure the new MM is clean and nothing unwanted is mapped */
	unmap(new_id, 0, STUB_START);

	return 0;

 out_free:
	free_pages(new_id->stack, ilog2(STUB_DATA_TOTAL_PAGES));
 out:
	return ret;
}

/*
 * A stub that has been killed but not yet reaped, and the resources that
 * cannot be released until the host says it is gone. destroy_context() cannot
 * keep a pointer into mm->context for this: the mm_struct is freed as soon as
 * it returns, so the few fields that matter are copied out.
 */
struct dying_stub {
	struct list_head list;
	int pid;
	unsigned long stack;
	/* One descriptor-passing socket per task slot (docs §12.35). */
	int sock[STUB_MAX_SLOTS];
};

static LIST_HEAD(dying_stubs);

/* Release what destroy_context() deferred, now that @pid is confirmed dead. */
static void reap_dying_stub(pid_t pid)
{
	struct dying_stub *d, *tmp;
	int i;

	list_for_each_entry_safe(d, tmp, &dying_stubs, list) {
		if (d->pid != pid)
			continue;
		list_del(&d->list);
		for (i = 0; i < STUB_MAX_SLOTS; i++) {
			if (d->sock[i])
				os_close_file(d->sock[i]);
		}
		free_pages(d->stack, ilog2(STUB_DATA_TOTAL_PAGES));
		kfree(d);
		return;
	}
}

void destroy_context(struct mm_struct *mm)
{
	struct mm_context *mmu = &mm->context;
	int i;

	/*
	 * If init_new_context wasn't called, this will be
	 * zero, resulting in a kill(0), which will result in the
	 * whole UML suddenly dying.  Also, cover negative and
	 * 1 cases, since they shouldn't happen either.
	 *
	 * Negative cases happen if the child died unexpectedly.
	 */
	if (mmu->id.pid >= 0 && mmu->id.pid < 2) {
		printk(KERN_ERR "corrupt mm_context - pid = %d\n",
		       mmu->id.pid);
		return;
	}

	scoped_guard(spinlock_irqsave, &mm_list_lock)
		list_del(&mm->context.list);

	/*
	 * Killing a stub is cheap; waiting for it to die is not, and this path
	 * runs twice for every guest execve. os_kill_ptraced_process() with
	 * reap_child set blocks in waitpid() with signals off, so the whole
	 * kernel stops for as long as the host takes to reap: measured on a
	 * Galaxy S26 at 54.5-55.0us, and flat at 0, 32, 128 and 512 mappings,
	 * so it is the reap latency itself rather than any work being done.
	 *
	 * The wait is not gratuitous, though, which is why this is not simply
	 * dropped: the free_pages() below hands back the page the stub shares
	 * with us, and doing that while the stub is still alive is a
	 * use-after-free on the other side.
	 *
	 * So keep the ordering and lose the wait: hand the pid and its pages to
	 * the SIGCHLD reaper, which already runs and already knows how to
	 * collect dead stubs, and let it free them once the host confirms the
	 * process is gone. In ptrace mode there is no SIGCHLD handler
	 * registered at all (see arch/um/os-Linux/signal.c), so there is nobody
	 * to hand to and the synchronous path stays.
	 */
	if (mmu->id.pid > 0 && using_seccomp) {
		struct dying_stub *d = kmalloc(sizeof(*d), GFP_ATOMIC);

		if (d) {
			d->pid = mmu->id.pid;
			d->stack = mmu->id.stack;
			for (i = 0; i < STUB_MAX_SLOTS; i++)
				d->sock[i] = mmu->id.sock[i];
			scoped_guard(spinlock_irqsave, &mm_list_lock)
				list_add(&d->list, &dying_stubs);
			os_kill_ptraced_process(mmu->id.pid, 0);
			mmu->id.pid = -1;
			return;
		}
		/* Out of memory: do it the slow, safe way below instead. */
	}

	if (mmu->id.pid > 0) {
		os_kill_ptraced_process(mmu->id.pid, 1);
		mmu->id.pid = -1;
	}

	if (using_seccomp) {
		for (i = 0; i < STUB_MAX_SLOTS; i++) {
			if (mmu->id.sock[i])
				os_close_file(mmu->id.sock[i]);
			mmu->id.sock[i] = 0;
		}
	}

	free_pages(mmu->id.stack, ilog2(STUB_DATA_TOTAL_PAGES));
}

static irqreturn_t mm_sigchld_irq(int irq, void* dev)
{
	struct mm_context *mm_context;
	pid_t pid;
	int status;

	guard(spinlock)(&mm_list_lock);

	while ((pid = os_reap_child_status(&status)) > 0) {
		/*
		 * Most reaped children are stubs destroy_context() killed and
		 * handed over rather than waited for. Release those first; the
		 * search below is for a stub that died without being asked to,
		 * and a handed-over one is no longer on mm_list to be found.
		 */
		reap_dying_stub(pid);

		/*
		* A child died, check if we have an MM with the PID. This is
		* only relevant in SECCOMP mode (as ptrace will fail anyway).
		*
		* See wait_stub_done_seccomp for more details.
		*/
		list_for_each_entry(mm_context, &mm_list, list) {
			if (mm_context->id.pid == pid) {
				struct stub_data *stub_data;
				printk("Unexpectedly lost MM child! Affected tasks will segfault.");
				/* Raw wait(2) encoding: low 7 bits are the
				 * signal, bits 8..15 the exit code. glibc's
				 * W* macros are not in scope here. */
				if (status & 0x7f)
					printk(KERN_ERR "stub %d was killed by signal %d\n",
					       pid, status & 0x7f);
				else
					printk(KERN_ERR "stub %d exited with %d\n",
					       pid, (status >> 8) & 0xff);

				/* Marks the MM as dead */
				mm_context->id.pid = -1;

				stub_data = (void *)mm_context->id.stack;
				stub_data->futex = FUTEX_IN_KERN;
				/*
				 * Unconditional on purpose -- the waiter-bit
				 * wake elision must never be used on this
				 * path. The waiter can be anywhere between
				 * spinning and parking, and a dead stub will
				 * never issue another handoff to correct a
				 * skipped wake: a spurious FUTEX_WAKE here is
				 * noise, a missed one is a task hung forever.
				 * The plain store can race with the waiter's
				 * fetch_or of its waiter bit; that is fine,
				 * because FUTEX_WAIT revalidates the value it
				 * was passed and this wake always fires. The
				 * syscall also carries the barrier the plain
				 * store lacks. (This was SMP-only before the
				 * spin existed; now the wake is the one kick
				 * that works no matter where the waiter is.)
				 */
				os_futex_wake(&stub_data->futex);

				/*
				 * NOTE: Currently executing syscalls by
				 * affected tasks may finish normally.
				 */
				break;
			}
		}
	}

	return IRQ_HANDLED;
}

static int __init init_child_tracking(void)
{
	int err;

	/*
	 * The handoff word must own its cacheline outright: both sides poll
	 * it, and a field the peer writes sharing the line would turn the
	 * spin in stub-futex.h into a stream of coherence misses. Assert the
	 * layout so a reorder of struct stub_data cannot quietly regress
	 * this -- it would still be correct, just slow in a way nothing
	 * functional would ever catch.
	 */
	BUILD_BUG_ON(offsetof(struct stub_data, futex) % STUB_FUTEX_ALIGN != 0);
	BUILD_BUG_ON(offsetofend(struct stub_data, syscall_data_len) >
		     offsetof(struct stub_data, futex));
	BUILD_BUG_ON(offsetof(struct stub_data, syscall_data) <
		     offsetof(struct stub_data, futex) + STUB_FUTEX_ALIGN);

	spin_lock_init(&mm_list_lock);
	INIT_LIST_HEAD(&mm_list);

	err = request_irq(SIGCHLD_IRQ, mm_sigchld_irq, 0, "SIGCHLD", NULL);
	if (err < 0)
		panic("Failed to register SIGCHLD IRQ: %d", err);

	return 0;
}
early_initcall(init_child_tracking)
