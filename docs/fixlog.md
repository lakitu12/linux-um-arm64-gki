
## patch 0001 (initial subarch) — 6.18 落位修复 [2026-10-16]
- asm-offsets.c: 6.18=3 行 stub（配 sysdep/kernel-offsets.h 全表）→ 按 port 最终形重写（v7.2 文件 + 3 处 port 编辑：extable→sysdep include；GDT define→ARCH_ASM_OFFSETS()；删 ALT/EXTABLE define）。
- x86 sysdep/kernel-offsets.h: 整体替换为 port 的 30 行子架构片段（含 ARCH_ASM_OFFSETS 宏）；common-offsets.h 成遗留未引用。
- elf_aux.c: 6.18 旧形（Elf32 auxv + vsyscall + 会清零 hwcap 的失败回退）→ port 目标形（64 位 auxv 分支 + hwcap2 + AT_HWCAP2 case）；vsyscall 三变量保留定义不再赋值（≈上游 70d52694b6 语义）。
- main.c: elf_aux_hwcap 定义迁入 elf_aux.c（对齐上游 293f71435d）。
- um_arch.c: 删 #include <asm/cpufeature.h>（cpufeature 已分裂进 x86）。
- x86/um/Makefile: obj-y +=capflags.o cpuinfo.o, -ptrace_user.o（保留 6.18 mem_$(BITS).o 布局）。

## patch 0018/0022 (AT_MINSIGSTKSZ / 文本修正) — 6.18 落位修复 [2026-10-16]
- 0018 的 min_sigstack 定义块（被 reject）在 0022 落位时一并补入（019-021 已先行提交导致补块随 0022 提交；排序瑕疵已记录）。
- elf_aux.c 最终形 = port 形；vsyscall 遗留块整体移至文件末尾（避免遮挡后续补丁上下文——教训）。

## patch 0026 (loadable modules) [2026-10-16]
- arch/arm64/kernel/module-plts.c: port 的 <asm/insn.h> 插入点适配 6.18（该版多一行 kvm_pkvm include；块插在 sort.h 之后）。
- arch/um/include/asm/Kbuild: port 的 ifneq($(HEADER_ARCH),arm64) 包裹落位（6.18 无 percpu_types.h/ring_buffer.h 行，上下文差异已消化）。

## patch 0027 (bionic) — 6.18 落位修复 [2026-10-16]
- arch/um/Makefile: +compiler_attributes.h force-include（NDK sysroot 遮蔽问题）。
- mm_id.h: __acquires/__releases 兜底块（6.18 无 turnstile 声明，保形保留）。
- skas/mmu.c: 两处错误打印。
- um_arch.c: UM_ADDR_TAG_MASK 地址标签掩码；39-bit task_size 修复查证已在位。
- start_up.c: "Userspace mode: ptrace" 打印（插入点因 6.18 无 SMP fatal 行而适配）。

## patch 0030 (handoff wake elision + spin) [2026-10-16]
- skas/mmu.c: 6.18 尚无 SMP 期的 os_futex_wake（只有"待添加"注记）→ 按 port 目标形直接落位无条件唤醒+注释；旧注记暂保留（6.19 上游已删，后续可清理）。

## patch 0032 (fault-around) [2026-10-16]
- mmu.h / skas/mmu.c: prefault_next/prefault_level 落位；因 6.18 无 SMP 期相邻初始化行（turnstile/sync_tlb_lock），插入点各自适配。

## patch 0034 (README + harness) [2026-10-16]
- 树内已有 ACK/kernel 的 README.md → 按 fork 惯例用 port README 覆盖（原文件可从 base commit 恢复）。

## patch 0052 (dead-stub reaper) [2026-10-16]
- skas/mmu.c: destroy_context 重排为 7.2 终形（list_del 提前为 scoped_guard 单语句），reaper 交接块落位（6.18 尾部 guard 形式与 7.2 不同所致）。

## fixup (2026-10-16, 构建暴露): arch/um/Makefile 续行 bug
- 6.18 的 kconfig.h 行无续行反斜杠（7.2 有）→ #27 的 compiler_attributes 行落到了变量外，make 误将其当 -include 指令解析（首次构建即炸）。
- 修复：给 kconfig.h 行补 `\`，新行收尾；已 fixup 归并回 #27 提交；补丁栈已重生成。

## 6.19 上游回移笔（构建暴露，2026-10-16）
- 313d69d/73ff992：Thomas Weißschuh 的原补丁原样套用（__access_ok_vsyscall 两步移除；6.18 上 arm64 编不过的主因之一）。

## 构建三/四轮间修复（NDK r28c, 2026-10-16）
- 本地构建实际使用 /usr/lib/android-ndk = r28c（脚本搜索顺序末位命中；原 CI 为 r27c）。
- arm64_defconfig: +CONFIG_FRAME_WARN=8192（signal 代码栈帧 4.5K，r28c 下超 2048；上游 92d5c5c04e 精神）。
- arch/um/kernel/ptrace.c: i/vp 标 __maybe_unused（x86-only ifdef 块使用）。

## 构建五轮前修复（clang r28c 新旗标, 2026-10-16）
- r28c 对 -Wmissing-prototypes 通过 cc-option → um_arch.c 的 x86 桩缺原型暴雷。
- 新增 arch/arm64/um/asm/text-patching.h 叠加头（镜像主线 aarch64_insn_* + 8 个桩原型 + struct alt_instr fwd）。

## 构建六轮前修复（2026-10-16）
- 回移上游 7.1 修复：um: fix address-of CMSG_DATA() rvalue in stub（r28c 下 &CMSG_DATA() 取 rvalue 地址 = error）。

## 构建七轮前修复（2026-10-16）
- 回移 os_futex_wait/os_futex_wake（6.19 SMP 批次的 host 侧工具；#30 的唤醒调用在 UP-only 下也需要）。取 7.2 原文落位。

## 构建八轮前修复（2026-10-16）
- process.c + #include <linux/futex.h>（FUTEX_WAIT/WAKE 常量）；fixup 归并入 os_futex 回移笔。

## 构建九轮前修复（2026-10-16）
- os-Linux/Makefile: elf_aux.o 无条件构建（293f71435d 的 Makefile 半边；此前挂在 x86-only CONFIG 下，arm64 链不上 4 个符号）。

## ✅ 构建成功（2026-10-16 13:35）
- build9 rc=0；产物 artifacts/linux-bionic（90,765,944B，静态 aarch64）+ artifacts/stub_exe_bionic（1,920B）。
- md5: linux-bionic=e4071fb1 / stub_exe=5028a36a；完整 sha256 见 report/build-success.txt。
- 工具链: /usr/lib/android-ndk r28c (28.2.13676358)，NDK API=30，JOBS=12。

## qcow2 根盘支持（2026-10-17，补丁 0061–0063）

- ubd 内建 qcow2 读写引擎（qcow2_user.c + 裁剪版 tinfl）。读侧：压缩（deflate）/零/未分配
  三分支；写侧：分配簇并维护 L2/refcount，崩溃序 = 泄漏可修、绝不悬引未落盘数据。
  审稿提示：bit0 只在标准描述符上是零标志，压缩描述符的 bit0 属于 offset。
- 拒收口径（开仓印理由）：backing / 快照（只读） / 加密 / 外部数据文件 / extended L2 /
  zstd / 非 16-bit refcount / 簇大小 512..64K 之外；qcow2 作 COW backing 明确拒绝。
- 宿主 selftest 先行：`tools/um-arm64/qcow2-selftest`（qemu-img check 干净 + convert 回
  raw 逐字节相等；四组夹具含压缩/普通/4K 簇空盘跨 refcount 块分配）。
- CI 新增 `ci/qcow2-gate.sh`：ext4 根盘转 qcow2 以 ubd0 引导；判据 = init marker +
  内核 `qcow2 "` 开盘点双串。boot job 已接线（e2fsprogs/qemu-utils）。
