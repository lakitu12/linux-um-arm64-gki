// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2015 Thomas Meyer (thomas@m3y3r.de)
 * Copyright (C) 2002 - 2007 Jeff Dike (jdike@{addtoit,linux.intel}.com)
 */

#include <linux/atomic.h>
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
 * The pending map/unmap batch belongs to the mm now, not to slot 0's stub_data
 * (see mm_id.h), so it needs a lock of its own.  A spinlock rather than a
 * mutex: the TLB-flush path that queues unmaps runs with interrupts disabled,
 * and nothing done under it can block.
 */
void stub_queue_lock(struct mm_id *mm_id, unsigned long *flags)
{
	struct mm_context *ctx = container_of(mm_id, struct mm_context, id);

	spin_lock_irqsave(&ctx->batch_lock, *flags);
}

void stub_queue_unlock(struct mm_id *mm_id, unsigned long flags)
{
	struct mm_context *ctx = container_of(mm_id, struct mm_context, id);

	spin_unlock_irqrestore(&ctx->batch_lock, flags);
}

/*
 * The control slot (slot 0) can serve one stub-syscall batch at a time.  This
 * is a mutex rather than a spinlock because the caller then blocks in the
 * handoff until the stub has applied the batch.
 */
void stub_control_lock(struct mm_id *mm_id)
{
	struct mm_context *ctx = container_of(mm_id, struct mm_context, id);

	mutex_lock(&ctx->control_lock);
}

void stub_control_unlock(struct mm_id *mm_id)
{
	struct mm_context *ctx = container_of(mm_id, struct mm_context, id);

	mutex_unlock(&ctx->control_lock);
}

/*
 * A stub that never got its pool scheduled is broken for good: the threads are
 * spawned before the seccomp filter goes in, so there is no second chance to
 * create them.  The first mm that gives up says so, and every later mm starts
 * serialised instead of paying the same one second.
 */
static bool stub_pool_broken;
/* How many claims had to wait for their own mm's pool to park (docs §12.35.7.1). */
static atomic_t stub_blind_claims = ATOMIC_INIT(0);

/*
 * Has slot @slot's stub thread parked?  Parking is exactly the state the
 * kernel needs before it writes guest registers into the slot: the stub wrote
 * its ctl word and let the host write the signal frame the registers live in,
 * then flipped its futex word to FUTEX_IN_KERN.  Reading both words is cheap
 * enough to do on every claim, and neither can be torn.
 */
static bool stub_slot_parked(struct mm_id *mm_id, int slot)
{
	struct stub_data *sd = STUB_DATA_SLOT(mm_id, slot);

	return READ_ONCE(*STUB_SLOT_CTL_AT((unsigned long)sd)) ==
			STUB_CTL_PARKING &&
	       STUB_FUTEX_OWNER(READ_ONCE(sd->futex)) == FUTEX_IN_KERN;
}

/*
 * Publish the slots whose stub thread has parked.
 *
 * A slot cannot be published when the mm is created, because the pool threads
 * are released by a go word at creation and then have to be scheduled on the
 * host before they can park.  Nothing orders that against the mm's first guest
 * entry: the very task that created the mm comes straight back to userspace()
 * and claims a slot, and if it wins the race the slot's signal frame is still
 * unwritten -- see the comment on @slot_ok.  So publication is deferred to the
 * claim path, which is the first place that needs it and by then runs on the
 * one task of a brand new mm (a second task cannot exist before the first one
 * has run guest code).
 *
 * Lazy is also free: a slot that is in use never looks parked, so the scan is
 * idempotent and a slot is only ever added once.  Slots are never unpublished:
 * once its thread has parked, a slot stays parked until the kernel hands it a
 * guest context, and the kernel puts it back into @slot_free only after the
 * stub has trapped again, i.e. after it parked again.
 */
static void stub_publish_parked(struct mm_id *mm_id)
{
	struct mm_context *ctx = container_of(mm_id, struct mm_context, id);
	int i;

	for (i = 1; i < mm_id->slot_count; i++) {
		unsigned long bit = 1UL << i;

		if ((mm_id->slot_ok & bit) || !stub_slot_parked(mm_id, i))
			continue;

		mm_id->slot_ok |= bit;
		mm_id->slot_free |= bit;
		ctx->slot_gen++;
	}
}

/*
 * Take a free task slot, if there is one; -1 when there is not.  Caller holds
 * slot_lock (docs §12.35).
 *
 * The pool itself is a bitmap in the mm: a set bit in @slot_free is a slot
 * whose stub thread is parked, free and known to be usable.  Slot 0 is never
 * in it -- it is the mm's control slot, see mm_id.h -- so a claimed slot is
 * always >= 1.  An mm with no pool (ptrace mode, a stub built without
 * STUB_SPAWN, or stub_slots=1) never reaches this function at all.
 *
 * Sticky first: a task that ran guest code before goes back to the same stub
 * thread whenever it can.  That keeps the guest task on one host thread, and
 * therefore on one host core, which is not a micro-optimisation -- measured on
 * this device, a single-threaded guest workload that was spread over the pool
 * ran 4.3x slower because most of its entries landed on little cores.
 * Otherwise round-robin, so new tasks spread out.
 */
static int stub_slot_take(struct mm_id *mm_id)
{
	int hint = current->thread.stub_slot;
	int slot, i;

	if (hint >= 1 && hint < mm_id->slot_count &&
	    (mm_id->slot_free & (1UL << hint))) {
		mm_id->slot_free &= ~(1UL << hint);
		return hint;
	}

	slot = mm_id->slot_next < 1 || mm_id->slot_next >= mm_id->slot_count ?
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

	return -1;
}

int stub_slot_get(struct mm_id *mm_id)
{
	struct mm_context *ctx = container_of(mm_id, struct mm_context, id);
	unsigned long long start = 0;
	bool noted = false;

	if (mm_id->slot_count <= 1)
		return 0;

	for (;;) {
		unsigned int gen;
		int slot;

		scoped_guard(mutex, &ctx->slot_lock) {
			stub_publish_parked(mm_id);
			slot = stub_slot_take(mm_id);
			gen = ctx->slot_gen;
		}

		if (slot >= 0)
			return slot;

		/*
		 * Nothing free.  Two very different situations, and the
		 * difference decides how long this may sleep:
		 *
		 *  - @slot_ok is empty: this mm's pool is still being
		 *    scheduled (or its threads never spawned at all).  Nothing
		 *    has ever run on it, so this task is the only one of the
		 *    mm that can be waiting; the wait ends when the pool parks
		 *    and is published, and a pool that never does that after a
		 *    second is broken -- fall back to the control slot for
		 *    good.  The first few such claims are printed, with the
		 *    guest pid rather than the stub's: the point is that a
		 *    brand new guest process hit its own pool mid-startup
		 *    (docs §12.35.7.1).
		 *
		 *  - Some slots are published but all held: ordinary
		 *    contention.  Sleep until one comes back.
		 */
		if (!mm_id->slot_ok) {
			if (!noted) {
				noted = true;
				start = ktime_get_ns();
				if (atomic_inc_return(&stub_blind_claims) <= 5)
					pr_info("stub: pid %d claimed a slot while its pool (%d slots) was still parking\n",
						current->pid, mm_id->slot_count);
			} else if (ktime_get_ns() - start > NSEC_PER_SEC) {
				pr_err("stub: mm %d pool never parked, running serialised\n",
				       mm_id->pid);
				scoped_guard(mutex, &ctx->slot_lock)
					mm_id->slot_count = 1;
				stub_pool_broken = true;
				return 0;
			}
		}

		/*
		 * Sleep, do not poll.  The generation counter makes the wait
		 * race-free: a slot returned (or published) after the take
		 * above bumped slot_gen, so the condition is already true and
		 * this does not sleep at all; if it becomes true after the task
		 * is on the queue, wake_up_all() finds it.
		 *
		 * A timeout as well, so the broken-pool deadline above is
		 * checked even if nothing ever wakes this task.
		 *
		 * Uninterruptible (_timeout, not _interruptible_timeout).  UML
		 * runs its timers off host signals, so an interruptible sleep
		 * here returns -EINTR almost immediately, over and over: the
		 * "sleep" becomes a busy loop in kernel space that never
		 * schedules.  Measured in the app domain, booting systemd with
		 * stub_slots=8: one guest task sat in this loop for 21 s of
		 * jiffies and tripped an "rcu_sched detected expedited stalls"
		 * dump, and in the lab it is the same defect that showed up as
		 * 30x the CPU for a mismatched pool size (docs §12.35.7.2).
		 * Nothing but the timeout wants to interrupt this wait, and
		 * the timeout is ours.
		 */
		wait_event_timeout(ctx->slot_wait,
				   READ_ONCE(ctx->slot_gen) != gen,
				   HZ / 10);
	}
}

void stub_slot_put(struct mm_id *mm_id, int slot)
{
	struct mm_context *ctx = container_of(mm_id, struct mm_context, id);

	if (slot < 1 || slot >= mm_id->slot_count)
		return;

	scoped_guard(mutex, &ctx->slot_lock) {
		mm_id->slot_free |= 1UL << slot;
		ctx->slot_gen++;
	}

	wake_up_all(&ctx->slot_wait);
}

static bool slot_report_done;
/*
 * Pool size, from the command line.  The stub spawns the threads it is told to
 * spawn (stub_init_data::spawn_slots) because clone is only callable before the
 * seccomp filter goes in, so the kernel has to decide before the stub starts;
 * stub_slots=1 spawns none and keeps the old single-slot behaviour on the same
 * kernel, which is what makes an A/B on one boot image possible.  The number is
 * also how many tasks of one mm may be in guest user mode at once.
 */
static int stub_slots_max = 1;

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

/*
 * The pool size the stub should build.  Used by start_userspace() to tell the
 * stub how many threads to spawn, so that an mm never carries threads the
 * kernel will not hand work to.
 */
int stub_slots_in_use(void)
{
	return stub_slots_max;
}

__uml_setup("stub_slots=", uml_stub_slots_setup,
"stub_slots=<n>\n"
"    How many task slots an mm's stub process carries, and therefore how\n"
"    many of its tasks may be in guest user mode at once (docs §12.35).\n"
"    The extra n-1 stub threads are spawned before the seccomp filter is\n"
"    installed -- clone is not available afterwards -- so they are eager:\n"
"    every guest process pays n-1 parked host threads, not just threaded\n"
"    ones. 1 is the classic single stub thread, including its serialisation.\n"
"\n"
"    Default: 1.  With n>1, threadscale measures 3.1-3.4x against 1.0x for\n"
"    the same four-thread workload (docs §12.35.7), and a single-threaded\n"
"    guest is held to one host core by task->slot affinity.\n\n");

int init_new_context(struct task_struct *task, struct mm_struct *mm)
{
	struct mm_id *new_id = &mm->context.id;
	unsigned long stack = 0;
	int i, ret = -ENOMEM;

	mutex_init(&mm->context.turnstile);
	mutex_init(&mm->context.slot_lock);
	init_waitqueue_head(&mm->context.slot_wait);
	mm->context.slot_gen = 0;
	spin_lock_init(&mm->context.batch_lock);
	mutex_init(&mm->context.control_lock);
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
	 * stolen host CPUs.  Only the slots this mm was told to spawn are
	 * released; with the pool off there are no threads to release.
	 */
	for (i = 1; i < stub_slots_max; i++) {
		struct stub_data *sd = STUB_DATA_SLOT(new_id, i);

		WRITE_ONCE(*STUB_SLOT_GO_AT((unsigned long)sd), 1u);
	}
	smp_wmb();

	/*
	 * Slot 0 is the exec'd stub thread and start_userspace() has already
	 * waited for it to trap, so it is usable immediately.  Slot 0 stays out
	 * of the pool: it is the control slot (see mm_id.h), so a claimable slot
	 * is always >= 1.
	 */
	new_id->slot_count = stub_pool_broken ? 1 : stub_slots_max;
	new_id->slot_next = 1;
	new_id->slot_free = 0;
	new_id->slot_ok = 0;

#ifdef STUB_SLOT_BLIND
	/*
	 * Diagnostic build only, never ship this (docs §12.35.7.1): publish every
	 * slot up front, which is the behaviour that made guests die with
	 * "failed to set regs: -22" a few times per boot.  It exists so the
	 * failure path can print what an unparked slot actually looks like when
	 * it is handed out, instead of the mechanism being argued from code.
	 */
	for (i = 1; i < new_id->slot_count; i++) {
		new_id->slot_ok |= 1UL << i;
		new_id->slot_free |= 1UL << i;
	}
#endif

	/*
	 * Best-effort census, for the one line below and for a person reading
	 * a boot log, and -- more than that -- the one chance to publish the
	 * pool without a claim path paying for it.  If the threads park inside
	 * this 20 ms window the mm starts with a full pool; if they do not,
	 * stub_publish_parked() picks them up on the first claim instead.  A
	 * thread that is slow to be scheduled is not a reason to serialise the
	 * mm, which is why this waits one boot only.
	 */
	if (!slot_report_done && new_id->slot_count > 1) {
		unsigned long long deadline = ktime_get_ns() +
					       20ull * NSEC_PER_MSEC;
		int ready;

		/* slot 0 is the exec'd stub thread and start_userspace()
		 * has already waited for it. */
		do {
			ready = 1;
			for (i = 1; i < new_id->slot_count; i++)
				if (stub_slot_parked(new_id, i))
					ready++;
			if (ready == new_id->slot_count)
				break;
			cpu_relax();
		} while (ktime_get_ns() < deadline);

		slot_report_done = true;
		stub_publish_parked(new_id);

		printk(KERN_INFO "stub: %d of %d task slots parked for the first mm\n",
		       ready, new_id->slot_count);
		if (ready != new_id->slot_count) {
			for (i = 1; i < new_id->slot_count; i++) {
				struct stub_data *sd = STUB_DATA_SLOT(new_id, i);

				printk(KERN_INFO "stub: slot %d ctl=%u futex=0x%x\n",
				       i,
				       READ_ONCE(*STUB_SLOT_CTL_AT((unsigned long)sd)),
				       READ_ONCE(sd->futex));
			}
		}
	}

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

#ifdef STUB_POOL_PROBE
				/*
				 * Diagnostic build only: whichever slot took a
				 * self-kill path says why in its probe words
				 * (stub.c, stub-data.h).
				 */
				{
					int i;

					for (i = 0; i < mm_context->id.slot_count; i++) {
						struct stub_data *sd =
							STUB_DATA_SLOT(&mm_context->id, i);
						volatile unsigned int *pr =
							STUB_SLOT_PROBE_AT((unsigned long)sd);

						if (!pr[1])
							continue;
						printk(KERN_ERR
						       "stub: dead slot %d (%s) errno %d sock_fd %d promised %u ctl %u futex 0x%x\n",
						       i,
						       pr[1] == STUB_PROBE_RECV ? "recvmsg" :
						       pr[1] == STUB_PROBE_FUTEX ? "futex" : "?",
						       (int)pr[0], (int)pr[2], pr[3],
						       READ_ONCE(*STUB_SLOT_CTL_AT((unsigned long)sd)),
						       READ_ONCE(sd->futex));
					}
				}
#endif

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
