# um-arm64 @ android17-6.18 — 移植状态（GKI 6.18 / UP-only）

_2026-10-16 · 由 Hermes 执行，来源口径见 report/fixlog.md · SMP 相关见 patches-gki618/EXCLUDED-SMP.md_

## 结果
- ✅ 源码树 `ack618`（分支 `um-arm64-gki618`，基线 **android17-6.18 @ 6.18.32**，googlesource 直取）
- ✅ **bionic 内核构建成功**：
  - `artifacts/linux-bionic` — 90,765,944 B，静态链接 aarch64，`for Android 30 / NDK r28c`，含 debug_info
    sha256 `03c3ef3bd9b695efcaeb9f0cb910652958b829a1bf72303a65d476ea94a9410c`
  - `artifacts/stub_exe_bionic` — 1,920 B
    sha256 `83f51f7c45133daa135b595562b09c5e2829f1d0f1e00b7fce2a7695370781fc`
  - 内核 banner：`Linux version 6.18.32-g3836ac3a3ef9 (um@arm64)`，clang 19.0.1 (LLD 19.0.1)
- ✅ 补丁栈 `patches-gki618/` — **60 笔**（49 系列 + 3 android + 8 回移/修复）+ `apply-gki618.sh`
- 📋 全部修复记录 `report/fixlog.md`（9 轮构建迭代，逐条根因与处置）；构建日志 `artifacts/build-bionic-*.log`
- ✅ **CI 全链路绿**（2026-10-16）：公开仓 `lakitu12/linux-um-arm64-gki` apply→bionic 构建→**arm64 runner 真引导**
  （marker `UMARM_GKI618_BOOT_OK` + Power down，verdict PASS；stub 与参考产物逐位相同）——run
  [38875955444](https://github.com/lakitu12/linux-um-arm64-gki/actions/runs/38875955444)；
  月度 cron 对分支 tip 自动试套（漂移=带证据包的红 run）

## 复现
```bash
cd ack618/tools/um-arm64/harness
TREE=$PWD/../../.. JOBS=12 bash build-bionic.sh
# 工具链自动发现；本机命中 /usr/lib/android-ndk (r28c)。原 CI 为 r27c——见 fixlog 的差异条目。
```

## 修复分类（详见 fixlog）
1. r28c 新启用旗标类：CONFIG_FRAME_WARN=8192；text-patching.h 叠加头（-Wmissing-prototypes）；ptrace __maybe_unused
2. 6.18 缺的 6.19/7.1 上游小修：__access_ok_vsyscall 移除(×2)、CMSG_DATA rvalue、elf_aux.o 无条件构建
3. SMP 批次小件单独抽取：os_futex_wait/wake + <linux/futex.h>
4. 续行坑：6.18 kconfig.h 行无 `\`，-include 插入需补续行符

## 后续建议
1. **真机 gate**：按 `tools/um-arm64/harness/android.sh`（页大小匹配→探针→marker 判定）
2. **CI 化**：apply→build→boot，绑定 GKI release tag 自动试套（可代搭 workflow）
3. **接入 uml-android**：本产物为 16K 页版（16K guest 可跑 4K/16K 宿主）
