// SPDX-License-Identifier: GPL-2.0
/*
 * Glue for the reused arch/arm64 BPF JIT backend.
 *
 * The JIT asks the platform two things a UML guest cannot answer: whether any
 * Spectre-BHB mitigation is active, and which SMCCC conduit the firmware uses.
 * Neither exists here (a guest is an ordinary EL0 process with no firmware), so
 * the answers are constants and no mitigation sequence is ever emitted.
 */
#include <linux/types.h>
#include <linux/arm-smccc.h>
#include <asm/cpufeature.h>

bool __nospectre_bhb;

enum arm_smccc_conduit arm_smccc_1_1_get_conduit(void)
{
	return SMCCC_CONDUIT_NONE;
}
