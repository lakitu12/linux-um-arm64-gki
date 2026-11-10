/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * CPU capabilities for ARCH=um SUBARCH=arm64: there are none to discover.
 *
 * On hardware, cpus_have_final_cap() reports a capability bit established at
 * boot by reading ID registers at EL1. A UML guest runs at EL0 as an ordinary
 * process and cannot read them; arch/arm64/Kconfig, where every erratum symbol
 * is defined, is not sourced for ARCH=um, so none of those CONFIG symbols
 * exist here either.
 *
 * The reused arch/arm64/kernel/module-plts.c consults exactly one capability,
 * ARM64_WORKAROUND_843419 (a Cortex-A53 erratum affecting ADRP placement), and
 * consults it unconditionally rather than under an #ifdef. Answering "no" is
 * what the same code does on any part without the erratum, and is the only
 * answer this configuration can give honestly.
 *
 * Recorded as a limitation: a UML guest on an affected Cortex-A53 would not get
 * the PLT slack that hardware would. The guest executes at EL0 either way, so
 * this is the same exposure any userspace program on such a part has.
 */
#ifndef __UM_ARM64_CPUFEATURE_H
#define __UM_ARM64_CPUFEATURE_H

#include <linux/types.h>

#define ARM64_WORKAROUND_843419		0

/*
 * Capabilities the reused arm64 code consults.  The BPF JIT is the other big
 * consumer besides module-plts.c: it batches atomics when ARM64_HAS_LSE_ATOMICS
 * is set, uses the virt-host-extn variant of some sequences, and emits the SB
 * (speculation barrier) instruction when ARM64_HAS_SB is present.  A UML guest
 * runs at EL0 and cannot probe any of this, so "no" is both the honest answer
 * and the one that keeps generated code on the plain instruction subset.
 */
enum {
	ARM64_HAS_VIRT_HOST_EXTN = 0,
	ARM64_HAS_LSE_ATOMICS,
	ARM64_HAS_SB,
};

static inline bool cpus_have_final_cap(int num)
{
	return false;
}

static inline bool cpus_have_cap(int num)
{
	return false;
}

/* No alternatives patching in UML: every capability answer is the compiled one. */
static inline bool alternative_has_cap_likely(int cap)
{
	return false;
}

static inline bool alternative_has_cap_unlikely(int cap)
{
	return false;
}

/* Spectre: a UML guest has no firmware to query and no EL1 code to protect, and
 * the JIT only asks so it can pick a mitigation sequence.  Report "vulnerable"
 * so no mitigation opcodes are emitted. */
enum mitigation_state {
	SPECTRE_UNAFFECTED,
	SPECTRE_MITIGATED,
	SPECTRE_VULNERABLE,
};

#define SCOPE_LOCAL_CPU	0
#define SCOPE_SYSTEM	1

static inline enum mitigation_state arm64_get_spectre_v2_state(void)
{
	return SPECTRE_VULNERABLE;
}

static inline bool is_spectre_bhb_fw_mitigated(void)
{
	return false;
}

static inline u8 get_spectre_bhb_loop_value(void)
{
	return 0;
}

static inline bool supports_clearbhb(int scope)
{
	return false;
}

extern bool __nospectre_bhb;

#endif /* __UM_ARM64_CPUFEATURE_H */
