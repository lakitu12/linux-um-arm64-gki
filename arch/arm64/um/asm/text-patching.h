/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Overlay for arm64's <asm/text-patching.h> in the ARCH=um SUBARCH=arm64
 * build.
 *
 * Two sets of users share this header here:
 *
 *  - The reused arm64 module code (patching.c, module.c) needs the
 *    aarch64_insn_* declarations. They are copied verbatim from
 *    arch/arm64/include/asm/text-patching.h, which this file shadows
 *    (arch/arm64/um/asm/ wins over arch/arm64/include/asm/).
 *
 *  - The shared arch/um/kernel/um_arch.c defines no-op text-patching
 *    stubs -- apply_alternatives(), the retpoline/return-thunk appliers,
 *    text_poke() and friends. On x86 the subarch's alternative.h and
 *    text-patching.h declare them; on arm64 nothing did, so the
 *    definitions failed -Wmissing-prototypes with clang versions that
 *    support the flag. Declare them here.
 *
 * struct alt_instr is only needed for the prototype; it is deliberately
 * not defined here (no ALTERNATIVE sequences exist for this subarch; see
 * alternative.h).
 */
#ifndef __UM_ARM64_TEXT_PATCHING_H
#define __UM_ARM64_TEXT_PATCHING_H

#include <linux/types.h>

/* Mirrors arch/arm64/include/asm/text-patching.h (which this shadows). */
int aarch64_insn_read(void *addr, u32 *insnp);
int aarch64_insn_write(void *addr, u32 insn);

int aarch64_insn_write_literal_u64(void *addr, u64 val);
void *aarch64_insn_set(void *dst, u32 insn, size_t len);
void *aarch64_insn_copy(void *dst, void *src, size_t len);

int aarch64_insn_patch_text_nosync(void *addr, u32 insn);
int aarch64_insn_patch_text(void *addrs[], u32 insns[], int cnt);

/* Prototypes for the no-op stubs in arch/um/kernel/um_arch.c. */
struct alt_instr;

void apply_seal_endbr(s32 *start, s32 *end);
void apply_retpolines(s32 *start, s32 *end);
void apply_returns(s32 *start, s32 *end);
void apply_fineibt(s32 *start_retpoline, s32 *end_retpoline,
		   s32 *start_cfi, s32 *end_cfi);
void apply_alternatives(struct alt_instr *start, struct alt_instr *end);
void *text_poke(void *addr, const void *opcode, size_t len);
void *text_poke_copy(void *addr, const void *opcode, size_t len);
void smp_text_poke_sync_each_cpu(void);

#endif /* __UM_ARM64_TEXT_PATCHING_H */
