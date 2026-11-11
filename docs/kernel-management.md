# Managing a guest kernel

What a build has to satisfy beyond booting, and how to check it on a device.
`docs/guest-features.md` lists the config fragments and what each is for;
`docs/upstream-port.md` has the build and run commands. This page is about the
properties a running Android guest depends on -- the ones a config edit or a
GKI bump can take away without breaking the build.

A build is one kernel plus the stub it was compiled with. The app keeps them
as a single directory per build, and mixing a kernel with a stub from another
build fails at boot with `start_userspace: expected SIGSTOP`, so ship the pair.

## Host VMAs are the budget

The stub maps every resident guest page with its own
`mmap(MAP_FIXED, MAP_SHARED)` at the guest virtual address. The host folds
those into one VMA only when the address, the protection and the backing
offset all continue from the previous call. Guest physical memory comes from
the buddy allocator page by page, so an order-0 allocation rarely continues
where the last one ended: of the stub's mmaps that do not merge, most offset
mismatches jump far away instead of continuing one page back or forward.

A guest process therefore costs the host one VMA per page. With
`vm.max_map_count` at 65530 -- the phone default -- a guest process dies at
about 256MB when the next stub `mmap()` fails, `report_enomem()` fires, and
the stub is declared untrustworthy; the process is killed. On LineageOS 24.0
that was system_server every 12-14 minutes.

The fix is to make guest pages large and contiguous: an order-N folio is laid
out in N adjacent guest physical pages, so N adjacent guest virtual pages now
have N adjacent host mappings and `map()` merges them. Measured on the same
image: 43k VMAs before, 27k after, and the count stops growing instead of
climbing into the cap.

## What the kernel must keep

Four properties, each of which a rebase can silently drop:

1. **Anon memory in PTE-mapped large folios.** The global THP policy is
   `never`; large folios arrive through the multi-size path, orders 2 through
   `PMD_ORDER-1`, enabled by `hugepage_init_sysfs()` under `CONFIG_UML`.
   Orders 0 and 1 are left off: too few VMAs saved for the fragmentation.
2. **No PMD-sized mappings.** This port's `pmd_t` is a software entry holding
   a guest physical address. There is no huge leaf bit and `pmd_trans_huge()`
   is constant 0, so a PMD mapping written by the generic code is read back as
   a page-table pointer by the next walk. Two places have to say so:
   `__thp_vma_allowable_orders()` drops `BIT(PMD_ORDER)` for UML, and
   `do_set_pmd()` refuses. The second one is not redundant: it asks "is THP
   disabled for this VMA", not "which orders are enabled", so the per-size
   sysfs knob never reached it and order-9 page-cache folios from ext4 built
   PMD mappings, reported as `bad pmd (0xee000c1)` and then cleared.
3. **`set_ptes()` advances by `PAGE_SIZE`.** The loop used to add
   `nr << PFN_PTE_SHIFT` while shifting `nr` down every iteration, so the
   entries cancelled out and every PTE got the first value. Nothing asked for
   more than one page until multi-size THP did; the symptom then was init
   dying on the first SIGSEGV with a garbage fault address.
4. **`arch/um/include/asm/pgtable.h` keeps the pmd stubs.** They exist so the
   generic THP code compiles; they describe a port with no PMD-level huge
   mappings. Do not "fix" them into real huge-leaf helpers.

## The config choice trap

The THP policy is a Kconfig `choice`, and `merge_config.sh` only overwrites
line by line. A fragment that says `CONFIG_TRANSPARENT_HUGEPAGE_NEVER=y` does
not clear the `ALWAYS=y` the defconfig already carries; `olddefconfig` then
sees two members `=y` and picks the choice default, which is `ALWAYS`. The
kernel ships with the policy the fragment meant to remove, and nothing in the
build says so.

Both other members must be explicitly not set:

    CONFIG_TRANSPARENT_HUGEPAGE=y
    # CONFIG_TRANSPARENT_HUGEPAGE_ALWAYS is not set
    # CONFIG_TRANSPARENT_HUGEPAGE_MADVISE is not set
    CONFIG_TRANSPARENT_HUGEPAGE_NEVER=y

The same shape applies to any other choice a fragment overrides. Check the
generated `.config`, not the fragment.

## Checking a build

Before shipping, and again after a rebase or a config change:

- **Banner matches the commit.** `uname -r` inside the guest ends in
  `-g<sha>`; it must be the HEAD you built. `$O/linux` is whatever the last
  build left there -- if more than one build ran in the same object
  directory, it is not necessarily the one you think.
- **Policy is `never`, sizes are right.** Inside the guest:

      cat /sys/kernel/mm/transparent_hugepage/enabled          # always madvise [never]
      grep . /sys/kernel/mm/transparent_hugepage/hugepages-*/enabled
      cat /sys/kernel/mm/transparent_hugepage/hpage_pmd_size

  Every size below `hpage_pmd_size` should read `[always]`; the entry that
  equals it is PMD-sized THP and must read `[never]`. Do not hard-code a
  size: it follows the guest page size, and the build the Android guest runs
  here is 4 KB pages (`CONFIG_PAGE_SIZE_4KB=y`), where the threshold is
  2048kB and the enabled sizes start at 16K. An empty or missing
  `hugepages-*` directory means the fragment did not land.
- **Large folios are actually allocated.** `anon_fault_alloc` under
  `hugepages-64kB/stats/` climbs during boot and stays non-zero.
- **The VMA count levels off.** On the host, `wc -l /proc/<pid>/maps` for a
  `uml-userspace` process. A healthy Android guest settles around 27k-30k
  after boot and no longer climbs; a guest without large folios keeps
  climbing and dies at the cap.
- **No `bad pmd`.** It shows up on the guest console, and the next walk clears
  the page table under the running process.

A baseline that dies at 12-14 minutes makes 30 minutes with a flat VMA count
and zero `bad pmd` meaningful; a run that ends because the task was removed
from recents is not a crash.

## Page size variants

The orders are the same on both builds; only the sysfs names move with the
guest page size. On 4 KB pages the enabled sizes are 16K through 1M and the
PMD-sized entry is 2048K; on 16 KB pages they are 64K through 16M and 32M.
Nothing else in the fragment depends on the page size.