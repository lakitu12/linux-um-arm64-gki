# 未包含：SMP 相关 5 笔（0047–0051）
本补丁栈是 UP-only 口径（目标 6.18 无 UML SMP）。
被暂缓的系列补丁（需连同上游 UML-SMP 系列一起回移时才恢复）：
- 0047 um/arm64: enable SMP（含 rqspinlock.h、smp.config）
- 0048 um: run on one CPU under ptrace instead of refusing to boot
- 0049 um: default ncpus to one less than the host has
- 0050 tools/um-arm64: add the SMP scaling benchmark
- 0051 um: fix the uniprocessor build of the ptrace CPU clamp

上游 UML-SMP 本体（回移成本，实测数据）：
- Tiwei Bie, [PATCH v2 00/10] "um: Add SMP support"（2025-08 RFC→v2；合入 6.19，
  42 文件 +827/−112；新增 kernel/smp.c、os-Linux/smp.c、shared/smp.h、
  linux/smp-internal.h 等；touch 面含 signal/timer/tlb/trap/vdso/percpu 等）。
- 6.19 之后 SMP 相关后续修复（如 7.1 期的 TLB sync race 修复）也需连带评估。
结论：SMP 全量回移 = 中等偏上量级的二次工程（~10+ 上游补丁重落位 + 系列 5 笔 +
编译/上机验证），建议单独立项。
