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

## 4K 变体（pages4k，2026-10-17）

同一补丁栈的另一种 guest 页大小，走 CI 输入而非手改树：

```bash
gh workflow run build-um-gki618.yml -f pages4k=true    # 产物名带 -4k 后缀
```

CI 合并 `ci/page4k.config`（16K 关、4K 开），断言改查 `CONFIG_PAGE_SIZE_4KB`。

- ✅ CI run [38993132552](https://github.com/lakitu12/linux-um-arm64-gki/actions/runs/38993132552)：
  日志 `4 KB variant: EXTRA_CONFIG=config/page4k.config`、`expecting guest page size: PAGE_SIZE_4KB`、
  `config ok: PAGE_SIZE_4KB`，apply→build→arm64 boot gate 全绿
- `artifacts/linux-bionic` — 94,851,728 B，banner `Linux version 6.18.32-g7d90328d3e59`
  sha256 `3452655338856e032e1ffb3a15a099bb848a0ede29490921becb8ef41080fb32`
- `artifacts/stub_exe_bionic` — 1,920 B
  sha256 `4972b0e3c96e204a4a98e2e61ffdf0ad76bf74a5d9d72944e787ed02212e27be`
  （与 16K 版**逐字节相同**：stub 由移植代码决定，与内核页大小/版本无关）
- 真机验证（marble，uml-android app）：引导 Debian 12.15 根盘到交互 shell，
  `getconf PAGESIZE`=4096、`dpkg -l`=104、perl 5.36、`sleep 30`+`^C` 2s 回提示
- 用途口径（更正）：4K 变体**不是**因为 Debian/Ubuntu 需要它——实测 16K 版同样跑 Debian
  （glibc 运行时查页大小）；它服务的是「与 4K 宿主逐页对齐」和「构建期写死 4096 的载荷」
  （唯一实测受害者：4K 对齐的静态 bionic `ctty`）

### qcow2 根盘支持（补丁 0061–0063，2026-10-17）

- ubd 可直接引导 qcow2（`ubd0=<img>.qcow2`）：直读 + 分配写 + refcount 全维护，
  boot job 含 `ci/qcow2-gate.sh` 双串判据用例。
- backing / 快照 / 加密 / zstd / 扩展 L2 拒收并给理由；详细口径见 fixlog。
- 7.2 参考树对应提交与宿主 selftest 位于 `lakitu12/linux-um-arm64`
  的 `tools/um-arm64/qcow2-selftest/`。

## 显示链与 guest X（2026-10-18/19，补丁 0064–0072）

- **umfb（0065）**：fb0 脏 tile 以 15 Hz 差分泵镜像进宿主文件（1024 B 头 + 像素区，含 seq/state），
  Linux 端 `umview`（0066/0067）与 uml-android app 的显示屏都能看；`video=vfb:` 老路弃用。
- **0069**：umfb 注册到 platform device —— Xorg 的 fbdev 探针只碰「非 PCI 父设备」的帧缓冲，
  无父设备时静默放弃（"No devices detected"）。
- **0070**：CONFIG_VT 与 UML console 抢 major 4 时容忍 console 注册失败（保 channel 版）。
  最终口径：**VT 不进 guest**（VT 内核下 init 的 fd 1 是 EBADF，见 fixlog 10-19 节）；`-vt` 变体
  仅作诊断产物保留。
- **0071**：UML_HOST_TOP 接受 `0x` 前缀（app loader 用 %#x 打印；拒绝前缀=静默回退→bionic_tls 死）。
- **0072**：stdio console 回答 VT ioctl —— **guest 里跑真 Xorg 的正解**。marble 真机实测：
  X 起（"(--) using VT number 0"、fbdev 绑 umfb、1024x768），静态画面下 guest fb0 与 sink
  像素区 md5 逐字节一致，app 显示屏实时呈现（sink seq 递增）。
