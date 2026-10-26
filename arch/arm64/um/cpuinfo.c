// SPDX-License-Identifier: GPL-2.0
/*
 * arm64 subarch half of UML's /proc/cpuinfo.
 *
 * There is no CPUID bitmap to mirror here. What a UML/arm64 guest can honestly
 * claim is what the host actually granted the UML process:
 *
 *   - AT_HWCAP / AT_HWCAP2 from the auxiliary vector, which is what guest
 *     userspace (glibc's ifunc resolvers, OpenSSL, ...) really consults.  The
 *     names printed after "Features" are generated from that sanitised mask,
 *     not echoed from the host's own line, so a library that parses
 *     /proc/cpuinfo and one that reads auxv are told the same thing;
 *   - the host CPU's identity (implementer/part/revision/...), read from the
 *     host's /proc/cpuinfo at boot, because those fields have no auxv
 *     equivalent and userspace expects to find them here.
 *
 * Names for the bits in the mask are a small table rather than the generated
 * arm64 hwcap table: that one is produced by arch/arm64/tools during a normal
 * arm64 build, which does not run for ARCH=um.
 */
#include <linux/array_size.h>
#include <linux/cpumask.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/kstrtox.h>
#include <linux/seq_file.h>
#include <linux/stddef.h>
#include <linux/string.h>

#include <asm/processor.h>
#include <arch.h>
#include <linux/init.h>
#include <uapi/asm/hwcap.h>

extern long elf_aux_hwcap;
extern long elf_aux_hwcap2;

/*
 * Host CPU identity, read from the host's /proc/cpuinfo during boot.
 *
 * UML cannot read an ID register: the "kernel" is EL0 code and a MIDR_EL1 read
 * traps.  The feature set therefore comes from the auxiliary vector, but the
 * identity fields -- implementer, architecture, variant, part, revision --
 * have no auxv equivalent.  Without them the guest can only say "model name:
 * UML": lscpu has no model to print, and userspace that keys on CPU part (SoC
 * quirk and tuning tables) sees a CPU that does not exist.  The host file is
 * readable from every context UML runs in, including an app sandbox, so one
 * pass over it at boot is all this costs.
 *
 * The parse is deliberately tolerant: a host that does not publish these lines
 * (or an unknown key) simply leaves the guest with the old output.
 */
#define UM_HOST_CPUS_MAX	64

struct um_host_cpu_id {
	unsigned int implementer;
	unsigned int architecture;
	unsigned int variant;
	unsigned int part;
	unsigned int revision;
};

static struct um_host_cpu_id um_host_ids[UM_HOST_CPUS_MAX];
static int um_host_nr_cpus;
static bool um_host_id_seen;
/* Host "processor" block being parsed, -1 when the current line is not in one. */
static int um_host_cpu = -1;

static const struct {
	const char *name;
	size_t offset;
} um_host_id_fields[] = {
	{ "CPU implementer",	offsetof(struct um_host_cpu_id, implementer) },
	{ "CPU architecture",	offsetof(struct um_host_cpu_id, architecture) },
	{ "CPU variant",	offsetof(struct um_host_cpu_id, variant) },
	{ "CPU part",		offsetof(struct um_host_cpu_id, part) },
	{ "CPU revision",	offsetof(struct um_host_cpu_id, revision) },
};

/*
 * Which host CPU describes guest CPU @cpu.
 *
 * A vCPU is a host thread and the host scheduler moves it between cores, so
 * there is no true per-vCPU identity.  Rather than hand out the first N host
 * CPUs -- which on this 8-core host would hide the X2 from a 7-vCPU guest --
 * spread the host's cores across the guest's vCPUs with both ends included.
 * The result describes the machine; it is not a claim about where a given vCPU
 * is at any instant.
 */
static int um_host_cpu_for(int cpu)
{
	int nr = num_online_cpus();
	int n = um_host_nr_cpus;

	if (n <= 0)
		return -1;
	if (nr <= 1)
		return 0;
	return cpu * (n - 1) / (nr - 1);
}

static void show_host_cpus(struct seq_file *m)
{
	int i, run;

	seq_printf(m, "host cpus\t: %d (", um_host_nr_cpus);
	for (i = 0; i < um_host_nr_cpus; i = run) {
		for (run = i + 1; run < um_host_nr_cpus &&
		       um_host_ids[run].part == um_host_ids[i].part; run++)
			;
		seq_printf(m, "%s0x%x x%d", i ? ", " : "",
			   um_host_ids[i].part, run - i);
	}
	seq_puts(m, ")\n");
}

/*
 * What the guest is told it can do.
 *
 * UML must not simply forward the host's AT_HWCAP. glibc's ifunc resolvers pick
 * memcpy/strlen/strcmp implementations from these bits, so advertising a
 * feature obliges UML to preserve whatever architectural state that feature
 * uses -- across syscalls, task switches and, above all, signal delivery.
 *
 * This is an allowlist rather than a denylist on purpose: a bit nobody has
 * audited must default to "not advertised", because the failure mode of the
 * other direction is a guest silently using state UML drops on the floor.
 *
 * Included: the FPSIMD baseline plus extensions that are purely new
 * instructions over the X, V and PSTATE state UML already saves and restores.
 * DIT and SSBS are PSTATE bits, and both are in UM_PSTATE_WRITABLE.
 *
 * Excluded, and why:
 *
 *   SVE, SVE2, SME   Z, P and ZA registers, and variable-length sve_context /
 *                    za_context records inside sigcontext.__reserved[].
 *                    arch/arm64/um/signal.c writes an fpsimd_context and
 *                    nothing else, and os-Linux/registers.c deliberately uses
 *                    NT_PRFPREG rather than NT_ARM_SVE. Advertising SVE while
 *                    saving only FPSIMD is precisely how "apt fails somewhere
 *                    different every time" happens.
 *   PACA, PACG       Pointer-authentication keys are per-task kernel state
 *                    (NT_ARM_PACA_KEYS) that UML does not manage; guest threads
 *                    sharing one stub process would silently share keys.
 *   BTI              Needs PROT_BTI, which arch/arm64/um/asm/mman.h explicitly
 *                    does not implement.
 *   MTE              Tag storage and tag-fault delivery, neither of which
 *                    exists here.
 *   GCS              Guarded control stack; needs kernel support.
 *   CPUID            Lets EL0 read the host's ID registers directly, which
 *                    would contradict everything masked out above: a library
 *                    probing CPUID would "discover" SVE after being told it has
 *                    none.
 *
 * Being wrong in the conservative direction costs a less-optimised memcpy.
 * Being wrong in the other direction costs silent corruption.
 */
#define UM_ARM64_HWCAP_ALLOWED						\
	(HWCAP_FP | HWCAP_ASIMD | HWCAP_EVTSTRM |			\
	 HWCAP_AES | HWCAP_PMULL | HWCAP_SHA1 | HWCAP_SHA2 |		\
	 HWCAP_CRC32 | HWCAP_ATOMICS | HWCAP_FPHP | HWCAP_ASIMDHP |	\
	 HWCAP_ASIMDRDM | HWCAP_JSCVT | HWCAP_FCMA | HWCAP_LRCPC |	\
	 HWCAP_DCPOP | HWCAP_SHA3 | HWCAP_SM3 | HWCAP_SM4 |		\
	 HWCAP_ASIMDDP | HWCAP_SHA512 | HWCAP_ASIMDFHM | HWCAP_DIT |	\
	 HWCAP_USCAT | HWCAP_ILRCPC | HWCAP_FLAGM | HWCAP_SSBS |	\
	 HWCAP_SB)

/*
 * Nothing from AT_HWCAP2 is advertised. Its bits are overwhelmingly SVE2, SME,
 * MTE and BTI -- all state-bearing or needing kernel support UML lacks. The few
 * instruction-only bits there are not worth the audit surface until something
 * actually needs them.
 */
#define UM_ARM64_HWCAP2_ALLOWED	0UL

unsigned long um_arm64_elf_hwcap;
unsigned long um_arm64_elf_hwcap2;

static int __init um_arm64_init_hwcap(void)
{
	unsigned long host = (unsigned long)elf_aux_hwcap;
	unsigned long host2 = (unsigned long)elf_aux_hwcap2;

	um_arm64_elf_hwcap = host & UM_ARM64_HWCAP_ALLOWED;
	um_arm64_elf_hwcap2 = host2 & UM_ARM64_HWCAP2_ALLOWED;

	pr_info("arm64: hwcap host 0x%lx/0x%lx -> guest 0x%lx/0x%lx (masked out 0x%lx/0x%lx)\n",
		host, host2, um_arm64_elf_hwcap, um_arm64_elf_hwcap2,
		host & ~UM_ARM64_HWCAP_ALLOWED, host2 & ~UM_ARM64_HWCAP2_ALLOWED);

	if (host & HWCAP_SVE)
		pr_info("arm64: host has SVE; not advertised, UML saves only FPSIMD state\n");

	return 0;
}
/*
 * early_initcall: ELF_HWCAP is read while building the auxv of the first user
 * process, so this has to have run before init is exec'd.
 */
early_initcall(um_arm64_init_hwcap);

/*
 * Names for the bits we advertise, in the spelling arm64's own /proc/cpuinfo
 * uses. The list is generated from the mask rather than echoed from the host's
 * Features line on purpose: some libraries parse /proc/cpuinfo instead of
 * reading auxv, and a guest told "no SVE" through AT_HWCAP but "sve" through
 * /proc/cpuinfo would be free to pick an SVE code path anyway -- reintroducing
 * exactly the corruption the mask exists to prevent. The two must agree, so
 * they come from the same place.
 */
static const struct {
	unsigned long bit;
	const char *name;
} um_arm64_hwcap_names[] = {
	{ HWCAP_FP,		"fp" },
	{ HWCAP_ASIMD,		"asimd" },
	{ HWCAP_EVTSTRM,	"evtstrm" },
	{ HWCAP_AES,		"aes" },
	{ HWCAP_PMULL,		"pmull" },
	{ HWCAP_SHA1,		"sha1" },
	{ HWCAP_SHA2,		"sha2" },
	{ HWCAP_CRC32,		"crc32" },
	{ HWCAP_ATOMICS,	"atomics" },
	{ HWCAP_FPHP,		"fphp" },
	{ HWCAP_ASIMDHP,	"asimdhp" },
	{ HWCAP_ASIMDRDM,	"asimdrdm" },
	{ HWCAP_JSCVT,		"jscvt" },
	{ HWCAP_FCMA,		"fcma" },
	{ HWCAP_LRCPC,		"lrcpc" },
	{ HWCAP_DCPOP,		"dcpop" },
	{ HWCAP_SHA3,		"sha3" },
	{ HWCAP_SM3,		"sm3" },
	{ HWCAP_SM4,		"sm4" },
	{ HWCAP_ASIMDDP,	"asimddp" },
	{ HWCAP_SHA512,		"sha512" },
	{ HWCAP_ASIMDFHM,	"asimdfhm" },
	{ HWCAP_DIT,		"dit" },
	{ HWCAP_USCAT,		"uscat" },
	{ HWCAP_ILRCPC,		"ilrcpc" },
	{ HWCAP_FLAGM,		"flagm" },
	{ HWCAP_SSBS,		"ssbs" },
	{ HWCAP_SB,		"sb" },
};

void arch_show_cpuinfo_ident(struct seq_file *m)
{
	/*
	 * Deliberately nothing.
	 *
	 * A real arm64 /proc/cpuinfo has no "vendor_id" and no "model name"
	 * line -- the CPU's identity is the CPU implementer/part/revision block
	 * emitted by arch_show_cpuinfo() -- and keeping the x86-style lines here
	 * is worse than cosmetic: lscpu turns "vendor_id" into a cputype, then
	 * "CPU implementer" clashes with that same field and starts a second
	 * cputype, which no processor ends up referencing.  The summary then
	 * carries a phantom "Model name: UML" entry with zero sockets.
	 */
	(void)m;
}

void arch_show_cpuinfo(struct seq_file *m, int cpu)
{
	const struct um_host_cpu_id *id;
	unsigned int i;
	int host = um_host_cpu_for(cpu);

	/*
	 * "Features" is the arm64 spelling; guest userspace and lscpu look for
	 * that key, not for x86's "flags".
	 */
	seq_puts(m, "Features\t:");
	for (i = 0; i < ARRAY_SIZE(um_arm64_hwcap_names); i++)
		if (um_arm64_elf_hwcap & um_arm64_hwcap_names[i].bit)
			seq_printf(m, " %s", um_arm64_hwcap_names[i].name);
	seq_puts(m, "\n");

	seq_printf(m, "hwcap\t\t: 0x%lx\n", um_arm64_elf_hwcap);
	seq_printf(m, "hwcap2\t\t: 0x%lx\n", um_arm64_elf_hwcap2);
	seq_printf(m, "host hwcap\t: 0x%lx/0x%lx\n",
		   (unsigned long)boot_cpu_data.arch.hwcap,
		   (unsigned long)boot_cpu_data.arch.hwcap2);

	/*
	 * The host's identity, in the same spelling and order the host's own
	 * /proc/cpuinfo uses, so lscpu and anything else that reads these keys
	 * gets a real MIDR instead of nothing.
	 */
	if (host >= 0 && um_host_id_seen) {
		id = &um_host_ids[host];
		seq_printf(m, "CPU implementer\t: 0x%02x\n", id->implementer);
		seq_printf(m, "CPU architecture: %u\n", id->architecture);
		seq_printf(m, "CPU variant\t: 0x%x\n", id->variant);
		seq_printf(m, "CPU part\t: 0x%x\n", id->part);
		seq_printf(m, "CPU revision\t: %u\n", id->revision);
	}

	/*
	 * One summary line, on the first block only: with fewer vCPUs than host
	 * cores the spread mapping cannot show every core, and this is the
	 * complete set in run-length form.
	 */
	if (cpu == 0 && um_host_id_seen)
		show_host_cpus(m);
}

int arch_parse_host_cpu_flags(char *line)
{
	unsigned int val, i;
	char *key, *value, *colon, *end;
	struct um_host_cpu_id *id;

	/*
	 * auxv is the authoritative feature source and it does not depend on how
	 * this file is formatted.  Record it on every call; the values are
	 * constants for the life of the process.
	 */
	boot_cpu_data.arch.hwcap = elf_aux_hwcap;
	boot_cpu_data.arch.hwcap2 = elf_aux_hwcap2;

	/*
	 * Lines look like "processor\t: 5" or "CPU part\t: 0xd46": split at the
	 * colon and trim the blanks around the key.  Anything without a colon is
	 * not a field this parser understands.
	 */
	colon = strchr(line, ':');
	if (!colon)
		return 0;
	*colon = '\0';
	value = skip_spaces(colon + 1);
	end = colon;
	while (end > line && (end[-1] == ' ' || end[-1] == '\t'))
		end--;
	*end = '\0';
	key = line;

	if (!strcmp(key, "processor")) {
		if (kstrtouint(value, 10, &val) || val >= UM_HOST_CPUS_MAX)
			um_host_cpu = -1;
		else {
			um_host_cpu = val;
			if (um_host_cpu >= um_host_nr_cpus)
				um_host_nr_cpus = um_host_cpu + 1;
		}
		return 0;
	}

	if (um_host_cpu < 0)
		return 0;
	id = &um_host_ids[um_host_cpu];
	for (i = 0; i < ARRAY_SIZE(um_host_id_fields); i++) {
		if (strcmp(key, um_host_id_fields[i].name))
			continue;
		if (!kstrtouint(value, 0, (unsigned int *)
				((char *)id + um_host_id_fields[i].offset)))
			um_host_id_seen = true;
		break;
	}

	/*
	 * Keep reading the whole file: the identity lines are per processor and
	 * come after the line that names it.  The Features line is deliberately
	 * not parsed here -- what the guest is told comes from the sanitised
	 * mask, not from the host's own list; see arch_show_cpuinfo().
	 */
	return 0;
}
