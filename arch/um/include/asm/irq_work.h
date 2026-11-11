/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_UM_IRQ_WORK_H
#define _ASM_UM_IRQ_WORK_H

/*
 * UML delivers inter-processor interrupts as host signals, so irq_work can be
 * run from an IPI instead of waiting for the next timer tick.  Without this
 * the raised list is only drained from irq_work_tick(), which is far too late
 * for callers that need the work between two syscalls -- bpf_mem_alloc
 * refilling its per-cpu caches is one, and it shows up as a spurious ENOMEM
 * from bpf_map_update_elem() on the first batch of updates.
 */
static inline bool arch_irq_work_has_interrupt(void)
{
	return IS_ENABLED(CONFIG_SMP);
}

#endif /* _ASM_UM_IRQ_WORK_H */
