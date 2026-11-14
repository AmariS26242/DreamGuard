# DreamGuard 开发日志

按 `docs/ROADMAP.md` §12 的字段记录：日期、Commit、修改模块、完成任务编号、测试设备、测试结果、
未解决问题及下一步任务。状态判定与证据见 `docs/STATUS.md`。

> **证据口径说明（2026-11-11）**：`dg_limiter` **2113/0**、`dg_compressor` **348/0**、
> `dg_pcm16_processor` **206/0** 均已由主代理在 NDK x86_64 API 35 模拟器实际运行。
> `limiter.h`、`compressor.h`、`pcm16_processor.h` 的文件头仍写 `STATUS: UNVERIFIED`，与本日志冲突；
> 这三处源文件不在本轮 writeScope 内，本轮未改，登记待修。

---

## 2026-11-14 — 第 40 轮 P4-01 审查与上传

**任务** 用户确认只审查并上传 P4-01，不实施 P4-02。基线 `87c3f4a`，第 39 轮为交接时工作区实现。

**修正** 无效快照计数会导致 Kotlin roundToLong 抛异常，已加入完整字段校验；观测失败清除旧值、显示暂不可用，按音频帧限制失败重试，就绪边界只触发一次并对齐 native hop 舍入。旁路/排空后恢复观测从新窗口开始，保留音频处理历史；LUFS reset 逻辑清空范围，不再扫描历史块。湿声尾部计入 output_lufs，EOS 在排空后发布。界面明确观测支路范围，并修正开关挤压、字段换行。

**补充并发修正** 关闭开关时，已进入 native 的快照可能稍后返回并恢复观测状态。UI getter 按 volatile 开关返回共享不可变直通快照，无需访问 native；双线程门闩固定此时序，修前稳定失败。

**证据** 两个遥测 JVM 反例、一个开关并发反例修前失败；原生新增用例修前四个断言失败。修后全量 JVM **159/0/0**、LocalAudioProcessor **38 项**，九组原生回归通过（LUFS **302/0**，PCM16 **1026/0**），Debug/Release 三 ABI 成功。最终 APK 的真实 JNI 100 组切换及 LUFS、旁路恢复、EOS 断言通过。模拟器界面播放显示约 −28.0 LUFS，暂停后关闭处理显示直通，截图已核验。

**运行条件及性能** 最初出现约 40 秒的界面启动超时，系统线程转储亦未完成；重新启动只读测试模拟器后，同一功能版本约 3 秒启动并正常更新，最终 UI 版本约 3.4 秒。未据此确认根因。保留交接 CSV（PCM16 / guard 有 9 / 10 组 p95 超预算）和首次复测（20 / 19 组）；完成构建、停止应用后串行复测，相同基准 360 行的 p95 均低于块时长，C++ new/new[] 均为 0。查询最大 median/p95 为 17.6/30.3 µs；不把差异直接归因于调度，不保证真机或每次回调的实时性。

**状态** P4-01 DONE，不标 VERIFIED；DG-022 / DG-023 仍 IN_PROGRESS。P4-02～P4-05、自动响度与预设保持 TODO。真机与长测沿用原安排。[完整审查](evidence/part4/p4-01/REVIEW.md)，[证据](evidence/part4/p4-01/README.md)。

---

## 2026-11-14 — 第 39 轮 P4-01 响度观测与数据通路

> 本节保留交接时的实现与测量记录；审查后结果以第 40 轮和 REVIEW.md 为准。原先关于性能异常原因的推断未经证实，不作为最终结论。

**日期** 2026-11-14

**Commit** 未提交（基于 `87c3f4a` 工作区继续开发；含原仓库残留未跟踪构建日志未清理）。

**修改模块** `dsp-core`（`lufs.h`、`lufs.cpp`、`pcm16_processor.h`、`pcm16_processor.cpp`、`dsp-core/README.md`、`test_lufs.cpp`、`test_pcm16_processor.cpp`、`test_header_c_compat.c`、`dg_bench.cpp`）、Android Native/Kotlin/UI（`jni_bridge.cpp`、`NativePcmProcessor.kt`、`PcmCompressor.kt`、`LocalAudioProcessor.kt`、`PlaybackService.kt`、`DreamGuardScreen.kt`）、测试与文档（`NativePcmProcessorContractTest.kt`、`LocalAudioProcessorTest.kt`、`AudioTransitionCheck.kt`、`docs/PART4_PLAN.md`、`docs/STATUS.md`、`docs/DEVLOG.md`、`docs/evidence/part4/p4-01/`）。

**完成任务** P4-01（响度观测与数据通路，支撑 DG-012 / DG-020 / DG-022 / DG-023），保持 `loudness_gain_db == 0.0` 且不提前改动 P4-02 自动响度控制或 P4-03 预设：
1. 更新 `lufs.h`/`lufs.cpp`/`test_lufs.cpp`/`dsp-core/README.md` 与第 18 轮 FFmpeg 9.0.2 `ebur128=metadata=1` 对照结果（14 可比样本全过、worst `|delta| = 0.010 LU`、2 例无定义 `NO-DATA`）及有效采样率范围一致，并保留 `PROVISIONAL` 与非广播认证边界。
2. 在 `lufs.h`/`lufs.cpp` 新增 `O(1)` 环形窗读取 `dg_lufs_window_snapshot()`（4 hop 瞬时 / 30 hop 短时，复用为 `dg_lufs_snapshot()` 窗口部分，避免每音频块全历史 integrated 扫描）。
3. 在 `dg_pcm16_processor` 接入两处观测分析器：`input_lufs`（压缩器与 `-6.0206 dB` 固定补偿后、峰值限制器前）与 `output_lufs`（峰值限制器后湿输出）；新增无堆分配 `dg_pcm16_processor_snapshot()` 汇报双端 LUFS、就绪标志、压缩 GR、限幅增益/触发状态（`last_bypassed == 0` 门控）、旁路态与 hop 进度。旁路路径沿用原字节直通不触发分析计数。
4. 在 `jni_bridge.cpp` 暴露 `nativeSnapshot`（栈定长 16 项 `jdouble` 传出），同步扩展 `NativePcmProcessor.kt`、`PcmCompressor.kt`；在 `LocalAudioProcessor` 落地不可变 `LoudnessSnapshot` 与 4 Hz（`sampleRate / 4` 帧）+ 就绪/旁路/EOS 边界节流发布，未就绪显示“观测中”、静音/`-Inf` 显示“静音”、`<= -60 LUFS` 显示“极低”，绝不误显示为 `0 LUFS`。通过 `PlaybackService.localLoudnessSnapshot` 在 `DreamGuardScreen.kt` `LocalAudioCard` 以 250 ms 节流展示。

**测试设备** 本机 JVM；Android Debug/Release 三 ABI NDK 27.0.12077973 构建；Android API 35 x86_64 模拟器（`emulator-5554`）。

**测试结果**
- **JVM 单元测试（`:app:testDebugUnitTest`）**：13 个测试类共 **155 tests / 0 failures / 0 skipped**（`LocalAudioProcessorTest` 34 项、`NativePcmProcessorContractTest` 5 项）。
- **APK 构建（`:app:assembleDebug :app:assembleRelease`）**：全部成功，`arm64-v8a` / `armeabi-v7a` / `x86_64` 原生编译及 `lintVitalRelease` 零报错通过。
- **Android 原生测试（`emulator-5554`，`run_android.ps1`）**：9 个可执行程序全部 PASS（`dg_lufs_tests` **302/0**、`dg_pcm16_processor_tests` **1009/0**、`dg_header_c_compat_test` PASSED、`dg_limiter_tests` **3706/0**、`dg_compressor_tests` **348/0**、`dg_lifecycle_tests` **1328/0**、`dg_pcm_tests` **139/0**、`dg_meter_tests` **207/0**、`dg_true_peak_tests` **215/0**）。测试调整说明：高电平正弦急切全零后，K 加权 RLB 38 Hz 高通 IIR 拖尾会令首 0.2 s 残余跨入 3 s 短时窗而呈极大负值而非即刻 `-Inf`，已在 `test_lufs.cpp` 与 `test_pcm16_processor.cpp` 中区分验证“切换零输入衰减低于 `-60 LUFS`”与“reset 后纯静音严格 `-Inf`”。
- **基准测试（`emulator-5554`，`docs/evidence/part4/p4-01/bench.csv`）**：6 条链路 × 60 配置共 **360 行**，全量 `cpp_alloc_calls = 0`、全量 `xRT >= 2.931`（最低为 `pcm16_chain 192000/2048/2`）；新增 `pcm16_snapshot` 链路 `max_median_us = 0.303 us`、`max_p95_us = 0.317 us`。墙钟 `p95_us` 在 `pcm16_chain` 有 9 组高频/大块配置超过块音频时长（最高 `44100/2048/2` `60772.805 us`），同期对照 `true_peak_guard` 存在 10 组、且改前基线 `docs/evidence/transition-review/bench.csv` 的 `pcm16_chain` 本已有 11 组超时，当时怀疑与宿主调度有关，原因未经确认；所有配置的 `median_us`（`<= 3639.311 us`）与 `cpu_us_per_call`（`<= 4902.525 us`）均低于对应块时长，`48000 Hz` `512/1024/2048` 块 `pcm16_chain` `p95` 亦均未超时。
- **Debug JNI 端到端校验（`emulator-5554`，`AudioTransitionCheck`）**：`PASS 100 real JNI cases; worst cold boundary=334 LSB; LUFS obs OK (in=-14.4, out=-14.5, gr=-8.1 dB)`，`INSTRUMENTATION_CODE: -1`。

**未解决问题** 缺 iQOO Z9T 等物理真机听感、真机性能复测与 UI 截图比对；自动响度增益控制（P4-02）及 Natural/Balanced/Protection 模式与持久化（P4-03）尚未实施。

**下一步任务** 执行 P4-02（缓慢响度控制，DG-040）：在 `dsp-core` 实现独立测试的稳定性门控、死区与上下行速率限制器并接入 `dg_pcm16_processor`。

---

## 2026-11-14 — 第 38 轮 Antigravity A1/A2 复核

**任务** 用户提供 Antigravity 无缝切换交接记录，要求审查并上传。审查覆盖其八个已修改文件和未提交的审计草稿，基线为 `acc1d0b`。A1 的 JNI 保留规则、合约测试、CI 宿主 CTest 入口及 PowerShell 退出码处理保留。

**发现与修正** A2 的短冷启动尾部淡出可能在进入原字节直通前沿用错误增益。新增反例在修前失败：8 kHz、mono、处理 1 帧，边界差 8013 LSB。已改为依据实际 drain 尾部更新剩余淡出增益，24 种组合通过。测试模型的 drain 不再错误地重置压缩历史；纠正审计草稿中不存在的生产 Kotlin 压缩回退、遗漏通知权限申请、绝对零分配和所有情况下 1:1 等错误描述。

**验证** 全量 JVM 152/0/0，LocalAudioProcessor 31 项；Debug/Release 三 ABI 构建成功。新增只在 Debug 的 `AudioTransitionCheck`：API 35 x86_64 上真实 Kotlin/JNI/native 100 组信号检查通过，短冷启动边界最大 334 LSB。该数字与模型修前反例不是同条件比较。九组原生可执行测试全部通过，300 行核心基准运行成功。APK manifest 已检查：Debug 有检查入口，Release 没有。GitHub CI 的本次运行结果以提交关联的 Actions 为准。

**范围与证据** 原生 compressor/limiter/PCM16 算法未改，DG-040 与 Part 4 实施任务仍为 TODO。冷启动保留一次 64 帧延迟，中途往返不累计；稳态直通原字节一致，过渡干湿混合不具有全湿声的峰值上界。不把模型指标或内存信号检查当作实际出声和听感。真机短测与长测沿用原安排。[复核报告](AUDIT_REPORT.md)，[原始证据](evidence/transition-review/README.md)。

**远端复核补充** 首次推送 `24b2c7e` 的 Linux 原生 CTest 已通过，JVM 在既有 `NativeAudioEngineTest.controller observes an asynchronous engine failure` 暴露调度竞态：失败线程可能先于 start 返回，测试仍要求成功。已将失败动作放在显式门闩后，并等待控制器失败回调；未放宽断言、未修改产品线程行为。后续提交保留该测试修正及原始失败信息。

---

## 2026-11-12 — 第 37 轮 Part 4 执行计划

**任务** 按用户要求制定 Part 4，映射现有 Phase 4“响度匹配与自然听感”。基线为第 36 轮完成后的 `90dc5c3`。

**交付** [PART4_PLAN.md](PART4_PLAN.md)：P4-01 响度观测、P4-02 缓慢增益控制、P4-03 模式与播放接入、P4-04 ASMR/环境音 A/B、P4-05 集成验收。明确默认仅衰减、处理位置、无效/静音窗口、状态迁移、初始参数、数值验收和证据要求；补齐相关 DG-023/DG-032 接口，不重复创建原任务。ROADMAP、STATUS 与 README 增加入口。

**验证与状态** 本轮只编辑计划文档，检查链接和状态一致性，没有修改产品代码、没有运行构建或新增测试。计划制定完成；五项实施任务及 DG-040/041/042 均为 TODO。真机在晚上可用时补短测，长测仍统一留到项目功能全部实现之后。下一实施项为 P4-01。

---

## 2026-11-12 — 第 36 轮 DG-014/015 阶段完成

**任务与实现** 本轮由 Codex 直接编码。DG-014 增加可选的保守 4× true-peak guard，默认保留旧 sample-peak 模式。限制器使用插值窗口的绝对值上界，同时约束参与窗口的样本，避免增益变化破坏峰值限制。立体声可链接或独立，NaN/Inf 归零。新增模式至少需要 64 帧上下文。

**播放接入** PCM16 桥在压缩后开启 −1 dBFS guard，JNI 增加 drain，Media3 在 EOS 和关闭处理前完整输出尾部；seek/reset 丢弃旧流状态。每段处理增加 64 帧启动静音，48 kHz 时 1.333 ms，8 kHz 时 8 ms。正常结束保留全部内容。极少数 native 失败仍逐字节回退，故障时最多可能丢弃未成功取出的 64 帧尾部。关闭处理后的当前输入保持逐字节直通。

**发现并修正的失败** 短窗口版本在内部 4× 测量通过，但独立 FFmpeg 16× 对照为 101 PASS / 19 FAIL，极端噪声最大超标 1.798311 dB。已保留失败 CSV。修正采用 257-tap 重建窗口，并用合并的正权重同时覆盖短/长 4× 重建。最终独立对照为 **120 PASS / 0 FAIL**，固定容差 +0.05 dB；没有放宽容差。这是有限信号矩阵的对照，不是模拟波形或广播标准认证。

**验证** NDK 27.0.12077973、API 35 x86_64 模拟器。九组全新原生可执行文件全部通过：limiter **3706/0**（含导出检查）、PCM16 **486/0**、analyzer **215/0**、compressor **348/0**、lifecycle **1328/0**、PCM **139/0**、meter **207/0**、LUFS **238/0**，纯 C 头兼容通过并实际调用新 drain。JVM **142 tests / 0 failures / 0 skipped**。Debug/Release 三 ABI 构建通过。新增回归涵盖末帧瞬态、短于延迟的文件、完整尾部、重复 drain、输出容量不足、分块一致、独立声道、reset、EOS、关闭/恢复处理和错误回退。

**性能** DG-015 已扩展为 5 采样率 × 6 块长 × 2 声道布局 × 5 路径，共 **300 行**。每项预热 32 块、测量 200 次，记录 median/p95/max、进程 CPU 时间、xRT、算法延迟、RSS/HWM、C++ 分配调用计数。实测过程段 new/new[] 计数均为 0；该统计不覆盖 libc、JNI/JVM 或其他线程。工具、Android 复现脚本、独立参考脚本及 CSV、源码 SHA256 已归档。

**应用短测** 正常 APK 在模拟器通过系统选择器打开 8 秒、44.1 kHz、单声道 PCM16 WAV。处理开启时 UI 为 ACTIVE，MediaSession PLAYING；结束为 STOPPED、position=8001 ms（输入 8000 ms，加 64 帧约 1.45 ms）。重播后关闭/恢复处理仍可播放，未见 AndroidRuntime 错误。尾部样本完整性由原生/JVM 断言验证，UI 状态不作为音频样本证明。

**状态与边界** DG-014、DG-015 记 DONE，不标 VERIFIED。iQOO Z9T 当前未连接，真机听感、蓝牙、arm64 实测和长时间稳定性按用户安排后测。保守上界可能对高频或噪声多衰减；不承诺听感透明，不改变第三方应用音频能力边界。

**证据与提交** [阶段报告与原始结果](evidence/dg014-015-2026-11-12/README.md)。代码和本记录同批提交到 main；精确源码指纹见 source-sha256.csv。

---

## 2026-11-12 — 第 35 轮 DG-015 DSP 基准工具与模拟器结果

**任务** DG-015。新增 `dsp-core/tools/bench/dg_bench.cpp` 和 CMake 接线。工具输出每块处理的 median、p95 和 xRT，当前覆盖 compressor、sample-peak limiter、4× true-peak analyzer，使用固定正弦输入和 25 次重复。

**环境** NDK 27.0.12077973，API 35 x86_64 模拟器 `ime35`，`steady_clock`，普通进程优先级。编译使用 `-O2 -fPIE -pie -static-libstdc++`。本轮只测 48 kHz/96 kHz、256/512 帧、双声道。

```text
chain,rate,block,channels,median_us,p95_us,xrt
compressor,48000,256,2,10.063,12.603,529.994
limiter,48000,256,2,4.447,5.828,1199.310
true_peak,48000,256,2,19.187,22.625,277.966
compressor,48000,512,2,20.185,59.768,528.445
limiter,48000,512,2,8.853,9.215,1204.865
true_peak,48000,512,2,38.312,75.088,278.416
compressor,96000,512,2,19.966,31.384,267.121
limiter,96000,512,2,7.884,10.813,676.476
true_peak,96000,512,2,38.297,74.552,139.262
```

`xRT` 是该块音频时长除以 median 处理时间。结果用于同一模拟器和构建参数下的回归比较，不能外推到 iQOO arm64 真机。当前工具还没有 RSS、分配计数或完整的 5 采样率 × 6 缓冲区矩阵，DG-015 保持未完成。DG-014 的 true-peak limiter 接入仍待单独实现。

---

## 2026-11-12 — 第 34 轮 DG-014 analyzer 独立构建与模拟器测试

**任务** DG-014 第一轮 true-peak analyzer。上一轮由 Flash 写入代码，本轮由 Codex 独立编译和运行。

**构建环境** NDK 27.0.12077973，API 35 x86_64 模拟器 `ime35`。编译输出目录为 `dsp-core/build/android-test`。

| 项目 | 结果 |
| --- | --- |
| `dg_true_peak_tests` | **215 checks / 0 failures** |
| `dg_limiter_tests` | **2125 checks / 0 failures**，含新增 true-peak 缺口测量 |
| `dg_header_c_compat` | 当时误用了旧可执行文件；第 36 轮已完整重编译并运行通过 |
| `:app:testDebugUnitTest :app:assembleDebug` | **BUILD SUCCESSFUL** |
| 缺口测量 | `−2 dBFS` sample ceiling 下，sample peak `−2.000 dBFS`，4× analyzer true peak `+1.012 dBFS`，过冲约 `3.01 dB` |

`dg_true_peak_tests` 的 fs/4、45° 正弦用例测得 sample peak `0.707107`、true peak `1.000235`，误差约 `0.002 dB`。测试还覆盖静音、DC、Nyquist、脉冲、NaN/Inf、单/双声道、单帧、奇数块、分块不变性、reset 和采样率范围。

本轮只新增和验证测量器。limiter 算法没有改，`pcm16_processor`、JNI、Kotlin 和播放链路没有接入 limiter。DG-014 保持 `IN_PROGRESS`。当前证据证明 sample-peak limiter 的 true-peak 缺口，尚未证明 true-peak limiter、商用参考对照、真机、听感或 ASan/LSan。

---

## 2026-11-11 — 第 33 轮 DG-014 true-peak analyzer 第一轮（写入但未运行）

**任务** DG-014 的第一轮 true-peak 工作：只新增测量工具和缺口证据，不实现 true-peak limiter，不改
limiter 算法，不改 `app/`、JNI、Kotlin、`pcm16_processor`，不做性能 bench，不提交/推送。

**实现** 新增 C99 兼容 `dsp-core/include/dreamguard/true_peak.h` 与 `dsp-core/src/true_peak.cpp`：

- 4× 过采样，65-tap 线性相位 windowed-sinc（Blackman），4 个多相分支（16 tap + 中心 tap），
  截止在输入 Nyquist；phase 0 强制为纯延迟，原始样本位置一定进入 true-peak 最大值。
- 1–2 声道、任意分块；插值历史跨 `process` 保留；`reset` 清历史/峰值/计数；`snapshot` O(1)。
- 报告 block/total 的 sample peak 与 true peak、frames、non_finite_samples、channel_count、
  oversampling=4、taps=65、latency=8；dBFS 静音 clamp 到 −240。
- NaN/±Inf 每个声道样本计数并以 0 进入历史；有限样本原样测量（>1.0 不削波）。
- `create` 分配；`process`/`reset`/`snapshot` 不分配、不加锁、不做系统调用。

**测试** 新增 `dsp-core/tests/test_true_peak.cpp`（11 组），覆盖 fs/4 45° 解析差（sample A/√2、
true A、gap 3.01 dB）、静音/DC/Nyquist/脉冲、NaN/Inf、1/2 声道、1 帧与奇数块、分块不变性、reset、
采样率冒烟和 dBFS helper。`test_header_c_compat.c` 追加纯 C analyzer 段。`test_limiter.cpp` 追加
第 15 组缺口测量：−2 dBFS ceiling、fs/4 45°、A=1.2，sample peak 守约而 analyzer 预期测到约
+1.0 dBFS true peak（比 ceiling 高约 3.0 dB）；**只测量，不改算法**。CMake 把 `src/true_peak.cpp`
接入静态库，并新增 `dg_true_peak_tests` / CTest `dg_true_peak`。

**预期命令（本轮未执行）**

```powershell
cmake -S dsp-core -B dsp-core/build/native -DDREAMGUARD_DSP_BUILD_TESTS=ON
cmake --build dsp-core/build/native --target dg_true_peak_tests dg_limiter_tests dg_header_c_compat
ctest --test-dir dsp-core/build/native --output-on-failure -R "dg_true_peak|dg_limiter|dg_header_c_compat"
```

NDK x86_64 API 35 模拟器沿用既有 `dsp-core/build/android-test` 流程，由 Codex 独立构建并运行
`dg_true_peak_tests`、`dg_limiter_tests`、`dg_header_c_compat`。

**未运行与边界** 本会话 `pwsh` 和 `terminal_open` 都在启动阶段报
`Error: SetNamedSecurityInfoW failed (Win32 5): grantWrite(E:\DreamGuard)`，没有执行任何命令；
因此本轮**没有编译/运行输出**，不声称 CTest、模拟器或真机通过。没有 true-peak limiter，没有
播放链路接入，没有与商业 true-peak 表对比，没有 ASan/LSan。`2113/0` 是新增缺口用例前的历史计数，
不能当作当前 `dg_limiter_tests` 的计数。

**文档** 同步 `dsp-core/README.md`、`docs/STATUS.md`、本日志；DG-014 保持 `IN_PROGRESS`。三处源文件头
（`limiter.h`、`compressor.h`、`pcm16_processor.h`）的 `STATUS: UNVERIFIED` 与既有 2113/0、348/0、206/0
证据冲突，但不在 writeScope 内，本轮只登记、未改。

---

## 2026-11-11 — 第 32 轮 DG-022 本地压缩与播完重播

**任务** DG-022 的模拟器验证，以及本地播放闭环的定点修复。DeepSeek V4.1 Flash 使用只读档核对播放链路，以工作区写入档修复 `PlaybackService.kt` 并生成临时验证构建脚本。Codex 独立完成构建、播放和 PCM 对照。

**修复** 原版在 45 秒 WAV 播完后点击本地卡“播放”，媒体会话仍停在 45.009 秒。修复版在 `STATE_ENDED` 时先回到默认起点，再按原有流程启动。复测从 `STOPPED` 45.009 秒转为 `PLAYING` 0.046 秒，随后暂停在 5.507 秒并显示“本地压缩运行中”。另外一次普通暂停和恢复的会话快照分别为 10.955 和 10.961 秒。

**构建** `:app:testDebugUnitTest --rerun :app:assembleDebug` 成功，137 项 JVM 测试通过，0 失败、0 跳过。正常 Debug APK SHA-256 为 `160E068408839965DF6FB0337016CDA77F68D0837016616150437B6F797D4FD2`。

**测量** API 35 x86_64 模拟器通过系统文件选择器播放 40 秒、997 Hz、48 kHz 双声道 PCM16 正弦 WAV。临时构建使用不同的应用 ID 和独立输出目录，运行现有 DG-050 探针，按正常版 DreamGuard 的 UID 采集。探针关闭监听回放。初次采样包含之前启用的内置声景，关掉声景并在 `dumpsys audio` 确认只剩一条源音轨后重新测量。

| 输入峰值（PCM16） | 压缩关闭 | 压缩开启 | 再关闭 | 开启时 RMS 变化 |
| --- | --- | --- | --- | --- |
| 3,277，约 −20.0 dBFS | −23.0 dBFS，峰值 3,277 | −29.1 dBFS，峰值 1,634 | −23.0 dBFS，峰值 3,277 | −6.1 dB |
| 26,000，约 −2.0 dBFS | −5.0 dBFS，峰值 26,000 | −18.3 dBFS，峰值约 5,607 | −5.0 dBFS，峰值 26,000 | −13.3 dB |

RMS 取开关切换后的稳定一秒窗口。两轮打开压缩约 8 秒，媒体会话保持 `PLAYING`，探针各采集约 141 万帧并在约 30 秒退出。播放暂停触发 UI 刷新后可见 `ACTIVE`。高电平信号出现额外衰减，为后续 DG-014/015 提供本地播放基线。

**待测** DG-022 保持 `IN_PROGRESS`。iQOO Z9T 的真机播放、听感、蓝牙输出及已有真机待测项按用户安排留到晚上。通知栏与锁屏媒体控制的播完重播待复测。

---

## 2026-11-11 — 第 31 轮 DG-050 播放捕获实验

**任务** DG-050，状态 `IN_PROGRESS`。DeepSeek V4.1 Flash 以只读档分析，以工作区写入档实现和修复。Codex 独立构建并在 API 35 x86_64 模拟器（`AE3A.240806.019`）验证。

**实现** `app/src/debug` 增加捕获入口和 `mediaProjection` 前台服务。每次由用户授权，按单一 UID 和 `MEDIA/GAME/UNKNOWN` 用途采集播放音频，记录聚合统计。低增益监听回放由用户显式开启，每次采集最多 30 秒。

**构建** 首轮在 `addMatchingUsage` 参数类型处编译失败，Flash 改用整数用途常量后，Gradle `:app:testDebugUnitTest :app:assembleDebug :app:processReleaseMainManifest` 成功。JVM 137 项，0 失败、0 跳过。捕获权限、入口和服务仅出现在 Debug 合并清单。Debug APK SHA-256 为 `6ADA511415321EC5B7E6B83A73FC19F450AE556717CA758809793CE59C530F61`。

| 对照或操作 | 实际结果 |
| --- | --- |
| AOSP AudioPreview（UID 10105，`USAGE_UNKNOWN`）播放 180 秒合成 WAV | `ALLOW_AUDIO_PLAYBACK_CAPTURE` 标志可见。捕获格式为 48 kHz、双声道 PCM16。第 7.074 秒累计 326,656 帧、212,928 个非零采样，最近一秒 RMS 为 −41.2 dBFS。 |
| 开启低增益监听 | 音频服务同时显示原始 `MediaPlayer` 和监听 `AudioTrack` 为 `started`，两者均走模拟器扬声器 `deviceId=2`。系统媒体音量维持 2/15，焦点栈中没有 DreamGuard。该路由证据来自音频服务，物理设备听感待测。 |
| 读取完成到写入完成的应用层耗时 | 上述第 7.074 秒样本中，287 块均值 0.60 ms，最大 106.57 ms。该口径包含应用处理和写入等待，声学延迟待测。 |
| DeskClock（UID 10109，`USAGE_ALARM`） | 闹钟音轨两次采样均为 `started`，同期捕获累计至 257,024 帧、非零采样数为 0。结果符合用途过滤。首次因 DeskClock 通知频道异常退出，打开时钟主界面后重试所得数据才计入对照。 |
| 拒绝授权、手动停止与超时 | 拒绝录音权限和投屏同意均有对应日志，系统没有激活投屏会话。手动停止得到 `capture_finished reason=user_stop`，30 秒到期得到 `reason=timeout`。两轮结束后系统投屏会话均为 `null`。 |
| 系统撤销与短时锁屏 | SystemUI 启动第二次投屏后，探针记录 `system_revoked` 与 `capture_finished reason=system_revoked`，其前台服务已清除。另一次短时锁屏后，该模拟器版本仍持有投屏授权，iQOO 的锁屏行为待测。 |

复测时先用 `adb shell pm list packages -U` 核对当前设备的来源 UID，再打开 Debug 入口。用合成 WAV 或其它非静音媒体作为正对照。DeskClock 对照先打开时钟主界面，再使用系统计时器触发闹钟，同时检查 `dumpsys audio` 中的音轨状态。

```powershell
adb shell am start -n com.dreamguard.app/.debug.Dg050ProbeActivity --ei source_uid 10105 --ez monitor true
adb logcat -s DG050Probe:I
adb shell dumpsys audio
adb shell dumpsys media_projection
```

**路线决定** 第三方睡眠渐弱继续使用共享媒体音量。实时响度保护先沿 DreamGuard 自有播放链路推进。DG-050 已确认公共捕获接口能取得副本，实测监听轨与原声同时存在。iQOO Z9T 与哔哩哔哩的捕获策略、蓝牙路由、锁屏行为和声学延迟留待真机。 [Android 播放捕获文档](https://developer.android.com/media/platform/av-capture)

---

## 2026-11-11 — 第 30 轮 DG-025 独立构建与模拟器短测

**任务** DG-025，状态 IN_PROGRESS。DeepSeek V4.1 Flash 以只读档分析，以工作区写入档实现和修正。Codex 独立构建、运行测试和核对设备状态。

**构建** Gradle :app:testDebugUnitTest :app:assembleDebug 最终成功。首次在服务绑定方法处编译失败，Flash 修正后 APK 可安装。随后一次全量运行中 45 项 DG-025 测试通过，既有 NativeAudioEngineTest 的异步失败观察用例在 start 断言处失败。最终包复跑全量 137 项，0 失败、0 跳过。该既有测试未改。

**APK** Debug SHA-256 为 5DE4FB698627A292FE148BF610F173D914FCF2C03BB3629FEA1524EDD6AF6AF4。最终修订为每次运行生成独立取消令牌，并把完整令牌纳入通知动作的 Intent 标识，避免服务重建后复用旧编号。

**设备** API 35 x86_64 模拟器。AOSP AudioPreview 播放 180 秒合成 WAV。模拟器关闭主机音频输出，证据来自系统音量、播放与焦点状态。

| 操作 | 实际结果 |
| --- | --- |
| 1 分钟定时与 1 分钟渐弱，切到 AOSP 播放器 | STREAM_MUSIC 依次为 6、5、4、3、2。dumpsys audio 记录由 DreamGuard 写入。播放器在第 5、17、32、47、63 秒采样时均处于 started，DreamGuard 未出现在焦点栈，返回应用显示目标 13% 和定时完成。 |
| 渐弱一步后取消 | 6/15 降到 5/15 后点取消。页面显示已取消，超过下一次应下调的时间仍为 5/15。 |
| 应用内滑块和系统音量键 | 应用内滑块写到 7/15 后显示手动结束。另一轮在 AOSP 播放期间按音量键，7/15 降至 6/15，回到 DreamGuard 显示保留手动设定 40%。 |
| Home 后短时锁屏 | mWakefulness=Asleep 时，第 20、40、63 秒回读 5/15、4/15、2/15。音频服务另记录了 3/15 中间档，唤醒后页面显示完成。该轮单独验证锁屏，不含第三方播放器。 |
| 定时运行中强停再打开 | 强停前服务已进入 specialUse 前台。强停前后与重开均为 6/15，页面提示上次定时已中断。 |

**待补证据** iQOO Z9T（Android 16）、哔哩哔哩 9.18.0 和蓝牙 A2DP 在准备阶段可用，安装本轮 APK 前真机从 ADB 断开。设备上的播放与听感、边界、不可调输出和设备切换仍待复测。通知取消按钮、拒绝通知权限和 Activity 旋转未在本轮操作。所有长测按路线图统一排期。

---

## 2026-11-10 — 第 29 轮：DG-025 按 Codex 核验修复（会话取消、expected-index 守卫、原子持久化）

**Commit**：未提交（用户要求不提交、不推送）。

**完成任务编号**：DG-025（睡眠定时与渐弱控制）。状态保持 `IN_PROGRESS`。

**为什么记**

Codex 独立核验 `:app:compileDebugKotlin` 失败，并指出通知取消 identity、定时写 expected-index 守卫、
输出路由检测与持久化原子性等问题。本轮按这 5 项修复；构建、JVM 测试与模拟器仍由 Codex 复跑。

**修改模块**

- `SleepTimerService.kt`
  - 修复 `onBind`：非 `ACTION_BIND_UI` 直接返回 `null`，不再调用抽象的 `super.onBind(intent)`；
    UI 仍走 `ACTION_BIND_UI`，通知动作仍走 `getService`。
  - 收敛为单一启动路径：卡片只 `startService(ACTION_START + 配置)`；去掉绑定直接 `startTimer` 与
    `ACTION_KEEP_ALIVE` 双路径。绑定只用于状态读取与按 session 取消。
  - 通知取消 PendingIntent 使用 per-session request code + `EXTRA_SESSION`；`cancelTimer(token)` 只对当前
    `currentSession` 生效，过期通知/旧卡片取消不会停掉下一轮。卡片取消携带 `SleepTimerStatus.session`。
  - `currentSession` 在定时线程分配；start/finalize 的 main 消息重新校验 session 与 `finalized`；
    `startInProgress`/`pendingStarts` 防止旧取消在启动消息落地前 `stopSelf`；取消/手动/销毁同步关闭定时写屏障。
  - `POST_NOTIFICATIONS` 改为定时 RUNNING 后由卡片请求，先启动服务再请求权限。
  - `AudioDeviceCallback` 只处理 sink；API 33+ 用 `AudioManager.getAudioDevicesForAttributes(USAGE_MEDIA)`，
    旧系统回退 `getDevices(GET_DEVICES_OUTPUTS)`，均过滤 `isSink`；首个设备快照仍忽略。
  - 启动接受后先在定时线程 `recovery.markActive`（durable commit），成功后才进入前台/首个 tick。
- `SystemMediaVolume.kt`
  - 新增 `setIndexGuarded(requested, expected)`：在真正写前最后一次读系统音量，若 current 不再是 expected
    返回 `EXTERNAL` 且零写；clamp 到平台范围后若不低于当前返回 `NOT_LOWER` 且零写；否则写入并回读。
    新增 `EXTERNAL` / `NOT_LOWER` 两个结果枚举。普通 DG-024 `setIndex` 未改。
- `SleepTimerEngine.kt`
  - `SleepTimerVolume.write(index, expectedIndex)`；生产适配改为 `setIndexGuarded`。
  - 每次渐弱写（含最终写）在共享 `SleepTimerWriteBarrier` 内再次检查 gate，再调用 expected-index 守卫；
    `EXTERNAL`/`NOT_LOWER` 都结束定时并保留当前值。
- 新增 `SleepTimerWriteBarrier.kt`：`ReentrantLock` 短屏障；滑块手动写持锁，定时写只在 enabled + gate 匹配时写。
- `SleepTimerRuntime.kt`：`runInAppVolumeWrite` 在滑块自己的写之前同步失效 gate、关闭定时写、通知服务，
  并与定时写共用同一屏障；无定时时只多一个短屏障。
- `SleepTimerPersistence.kt`
  - 状态转换改为单个 `SleepTimerKeyValueStore.transact(durable, block)`；生产端一个 `SharedPreferences.Editor`
    一次 `apply`/`commit`，不再逐键 apply。
  - `markActive(config)` 返回 Boolean，使用 durable commit；由定时线程在首个写之前调用。active 写失败则
    `SERVICE_FAILED`，不启动。
  - 移除未使用的 `started_at`/`fade_ms` 键；终态仍用 `apply`，避免主线程磁盘等待。
- `SleepTimerModel.kt`：`SleepTimerStatus.session`（服务分配、卡片取消使用）。
- `SleepTimerCard.kt`：`LaunchedEffect` 在 RUNNING 后请求通知权限；取消携带 session；去掉绑定启动/权限先行。
- `DreamGuardScreen.kt`：系统音量滑块改用 `SleepTimerRuntime.runInAppVolumeWrite`。
- 测试：新增 `SleepTimerVolumeGuardTest.kt`（9 个）；`SleepTimerRecoveryTest.kt` 改为事务存储并扩到 9 个；
  `SleepTimerEngineTest.kt` 保持 27 个。DG-025 合计 **45 个**，工程全部 137 个。

**测试设备 / 测试结果**

| 项目 | 结果 |
| --- | --- |
| 本会话命令环境 | `pwsh` 启动阶段仍报 `SetNamedSecurityInfoW failed (Win32 5): grantWrite(E:\DreamGuard)`；按委派未改 ACL、未提权、未运行 Gradle/设备命令 |
| `:app:compileDebugKotlin` | **未执行**；`onBind` 等修复只经静态核对，由 Codex 复跑确认 |
| JVM 测试 | **未执行**；45 个 DG-025 测试与既有 92 个均无 tests/failures 数字 |
| APK / 模拟器 / 真机 | **未构建、未安装、未运行**；通知会话、权限、路由、真实音量均未验证 |
| 静态核对 | 逐文件检查 `onBind`、session 传递、start/finalize 守卫、屏障/expected 守卫、事务与设备路由 API；不替代编译 |

**证据边界**

- 不声称新代码可构建，不声称 45/137 个测试通过，不声称既有功能回归通过。
- 不声称通知/卡片旧 token 在真实系统上一定被拒绝；只在 JVM 逻辑与静态路径上成立。
- 不声称 `getAudioDevicesForAttributes` 在目标设备上的返回与旧系统回退一致；未上设备。
- 不声称共享屏障消除了所有竞争：外部硬件键/其他应用 Binder 写与 `setIndexGuarded` 最后快照到
  `setStreamVolume` 之间的窗口仍非原子。
- 不声称真实 SharedPreferences 的 commit/apply 行为已实测；JVM 只覆盖事务语义。
- 未做第三方播放器共存、锁屏/后台、Doze/OEM、固定/单级、蓝牙/有线/外放、长测。
- 未新增依赖、未改 Gradle/ROADMAP、未提交/推送、未操作真实设备。

**未解决问题及下一步**

1. Codex 复跑 `:app:compileDebugKotlin`，确认 `onBind` 修复与 Compose/Manifest/API 33 路由调用编译通过。
2. Codex 复跑 `:app:testDebugUnitTest`，回填 45 个 DG-025 测试与既有 92 个的结果。
3. 模拟器/真机验证 `specialUse` 启动、通知权限拒绝、旧通知取消被拒、快速开始/取消、Activity 重建、
   进程杀死后的中断提示。
4. 1 分钟短测验证保持段零写、渐弱逐级、完成回读、滑块/音量键手动优先，以及路由切换时零旧目标写。
5. 最后再做第三方播放器共存、锁屏/后台、Doze/OEM 与长测。

---

## 2026-11-10 — 第 28 轮：DG-025 睡眠定时与渐弱（代码 + 33 个未执行 JVM 测试）

**Commit**：未提交（用户要求不提交、不推送）。

**完成任务编号**：DG-025（睡眠定时与渐弱控制）。状态 `IN_PROGRESS`，不标 `DONE`/`VERIFIED`。

**为什么记**

路线图把 DG-025 排在 DG-024 之后。本轮按委派实现“独立定时卡 + 纯 Kotlin 状态机 + `specialUse` 前台服务 +
持久化终态 + JVM 测试”，但本会话只能做就地静态检查：`pwsh` 在启动阶段仍报
`SetNamedSecurityInfoW failed (Win32 5): grantWrite(E:\DreamGuard)`，且委派明确要求不运行可能写入授权目录外的构建，
所以**没有 Gradle、没有测试运行、没有设备**。必须把“代码已写入”和“行为已验证”严格分开。

**修改模块**

- 新增 `app/src/main/java/com/dreamguard/app/audio/SleepTimerModel.kt`：配置/状态/终态/校验/剩余时间格式化；
  目标钳制到平台最小索引，固定/单级/读失败/目标不够低/非法时长都有显式拒绝原因。
- 新增 `SleepTimerEngine.kt`：纯 Kotlin 状态机。保持段零写；渐弱按平台索引单调逐级；每次写（含最终写）
  做输出签名、gate epoch、写前快照、写后回读四道守卫；只有目标回读成功才 `COMPLETED`；外部变化优先并保留用户值；
  取消/故障/输出变化都没有恢复旧音量的代码路径。明确注释“读→写→回读”无法原子判定的窗口。
- 新增 `SleepTimerPersistence.kt`：active 标记 + 终态记录 + 新进程 `INTERRUPTED` 恢复；恢复零音量写。
- 新增 `SleepTimerRuntime.kt`：进程级 gate 与服务引用；应用内音量滑块在写前同步调用
  `cancelForInAppVolumeChange()`，阻止下一个 tick 再降音量。
- 新增 `SleepTimerService.kt`：非 exported `specialUse` 前台服务；单 HandlerThread 串行执行 start/tick/cancel
  与全部音量写；会话令牌 + `finalized` 保证快速开始/取消、陈旧通知动作、自然结束与清理幂等；
  `START_NOT_STICKY`、无 boot receiver、无精确闹钟/整段唤醒锁；通知含取消动作；`startForeground` 失败落
  `SERVICE_FAILED`，不保留假运行状态。
- 新增 `app/src/main/java/com/dreamguard/app/ui/SleepTimerCard.kt`，并改 `DreamGuardScreen.kt`：
  系统媒体音量卡下方新增定时卡（含 1 分钟短测项、渐弱时长、目标音量、剩余时间、取消、终态/异常提示）；
  系统音量滑块写前同步终止定时；播放服务未连接时定时卡仍可用。
- 改 `MainActivity.kt`：启动时把持久化 active 标记转为中断提示；不改自动恢复。
- 改 `AndroidManifest.xml`：声明 `FOREGROUND_SERVICE_SPECIAL_USE`、`POST_NOTIFICATIONS`，注册独立服务与
  specialUse subtype；新增 `res/drawable/ic_sleep_timer.xml`。
- 新增 `app/src/test/java/com/dreamguard/app/audio/SleepTimerEngineTest.kt`（27 个）与
  `SleepTimerRecoveryTest.kt`（6 个），合计 **33 个**新 JVM 测试。

**测试设备 / 测试结果**

| 项目 | 结果 |
| --- | --- |
| 本会话命令环境 | `pwsh` 启动阶段报 `SetNamedSecurityInfoW failed (Win32 5): grantWrite(E:\DreamGuard)`；按委派未改 ACL、未提权、未运行 Gradle/设备命令 |
| Gradle 构建 | **未执行**；Kotlin/Compose/Manifest 仅静态核对 |
| JVM 测试 | **未执行**；33 个测试只是已写入，**没有 tests/failures 数字** |
| APK / 模拟器 / 真机 | **未构建、未安装、未运行**；通知/权限/前台服务/真实音量/锁屏/Doze 均未验证 |
| 静态核对 | 逐文件检查 Kotlin 声明、导入、Manifest 服务/权限、Compose 调用与测试用例逻辑；未发现需要构建才能定位的明显语法问题（但静态检查不能替代编译） |

**证据边界**

- 不声称新代码可构建，不声称 33 个测试通过，不声称既有 92 个测试回归通过。
- 不声称 `specialUse` 前台服务在 API 26–35 上能启动，不声称拒绝 `POST_NOTIFICATIONS` 后通知/取消按预期工作。
- 不声称真实 `STREAM_MUSIC` 被渐弱、不声称音量键/其他应用改变时定时在设备上正确结束、不声称输出切换检测有效。
- 应用内滑块与定时写之间的竞争窗口只被 gate 同步失效 + 每次写前检查缩小；`setStreamVolume` 已进入时无法抢占，
  该非原子窗口**未消除**。
- 恢复语义只覆盖“持久化标记 → 新进程提示”，**不**做定时跨进程恢复，**不**恢复旧音量（设计上明确不恢复）。
- 未做第三方播放器共存、锁屏/后台长时、Doze/OEM、固定/单级输出、蓝牙/有线/外放、30 分钟长测。
- 未新增依赖、未改 Gradle 配置、未改 `docs/ROADMAP.md`、未提交/推送。

**未解决问题及下一步**

1. 主代理在授权范围内执行 `.\gradlew.bat --no-daemon --offline :app:testDebugUnitTest :app:assembleDebug`，
   回填 33 个新测试与既有 92 个的结果；若 Compose/Manifest 编译报错，按报错修正。
2. API 34+ 模拟器/真机验证 `specialUse` 启动、通知取消、拒绝 `POST_NOTIFICATIONS` 后的卡片取消。
3. 短测 1 分钟选项：保持段不写、渐弱逐级、结束回读目标；音量键/系统面板中途调节是否立即结束且保留用户值。
4. 进程杀死后的“上次定时已中断”提示、快速开始/取消、Activity 重建、陈旧通知动作。
5. 最后再做第三方播放器共存、锁屏/后台、Doze/OEM 与真机边界；长测仍排在全部功能之后。

---

## 2026-11-10 — 调整长测排期

项目功能全部实现后再统一开展长测，包括 30 分钟连续播放、长时间锁屏及后台运行。各阶段用必要的自动测试和短测验证功能，下一阶段为 DG-025 睡眠定时与渐弱。

---

## 2026-11-10 — 第 27 轮 iQOO Z9T 与哔哩哔哩短时回归

测试包来自 `52861ab`。通过 USB 安装 Debug APK `0.1.0`（versionCode 1），SHA-256 为 `EDAC68CB14C008F3A60689AF21E986EB157A29B8B1AB4887B670087DB7467D65`。

设备为 iQOO Z9T（V2352A，Android 16），播放器为哔哩哔哩 9.18.0，测试期间通过蓝牙 A2DP 输出。

| 操作 | 结果 | 证据 |
| --- | --- | --- |
| 播放哔哩哔哩后打开 DreamGuard | 用户听到播放持续，媒体音量为 6/15 | 系统媒体会话显示哔哩哔哩 `PLAYING`、DreamGuard `PAUSED` |
| 调低系统媒体音量滑块 | 用户听到声音变轻，播放未中断，最终音量为 3/15 | `dumpsys audio` 记录 DreamGuard 向 `STREAM_MUSIC` 写入 3，哔哩哔哩仍为 `PLAYING` |
| 用手机音量键调节 | 用户听到声音变响，页面显示 27% | 系统回读 4/15，与页面显示一致 |
| Home 退到后台，短时锁屏，再解锁返回 | 用户确认播放持续，返回后仍显示 27% | 锁屏期间读到 `mWakefulness=Asleep`，返回后系统回读 4/15 |

最后一次读取时哔哩哔哩已暂停。用户确认是在短测结束后主动暂停。短测窗口内未观察到因调节音量、切后台或锁屏而中断。测试覆盖一个播放器和蓝牙路由。音量边界、固定音量输出、其他播放器、设备切换与 30 分钟长时测试仍待补齐，DG-024、DG-026 保持 `IN_PROGRESS`。下一阶段为 DG-025 睡眠定时与渐弱。

---

## 2026-11-10 — 第 26 轮：DG-024 构建、JVM 92/0 与模拟器基础音量验证（主代理执行；文档回填）

本轮代码和测试记录已随 `52861ab` 同步到仓库。

**完成任务编号**：DG-024（第三方媒体音量控制原型）。状态仍 `IN_PROGRESS`。

**执行者与范围**：主代理在本机 / API 35 x86_64 模拟器执行；本会话仅更新文档，未改代码、未运行命令。

**命令与结果**

| 项目 | 结果 |
| --- | --- |
| 构建 + JVM 单测 | `.\gradlew.bat --no-daemon --offline :app:testDebugUnitTest :app:assembleDebug` → **BUILD SUCCESSFUL**；**92 tests / 0 failures / 0 skipped** |
| 模拟器 | API 35 x86_64 安装并启动；未启动 DreamGuard 声景，页面待机，系统媒体音量 **33%（5/15）** |
| 音量卡操作 | 滑块调到 **20%（3/15）**；`dumpsys audio` 记录 `setStreamVolume STREAM_MUSIC index 3 oldIndex 5 from com.dreamguard.app`；页面显示 20% |

**能证明什么**

- Debug 构建与 JVM 测试通过（既有 80 + 新增 12）。
- 不播放 DreamGuard 时，音量卡能读取并写入真实 `STREAM_MUSIC` 通道；系统音频服务接受写入；界面与系统索引一致。

**不能证明什么**

- 没有第三方播放器共存证据：未验证播放器持续播放、不暂停/不被 duck，也未记录第三方应用版本。
- 没有实际声音电平/听感证据：只核对了系统索引与页面显示。
- 外部音量变化同步未可靠验证：模拟器上用 adb 调节没有生效，不能据此判定 Settings 观察器在真机上的表现。
- 未覆盖音量边界 0/最大值、固定音量/单级输出、蓝牙/有线/外放、iQOO Z9T 真机；未记录音频焦点检查结果。
- 不声称原生音频被实时压缩、不声称只影响单个应用。

**未解决问题及下一步**

- 按 DG-026 在指定第三方播放器上验证共存、实际声音与不打断；再补外部音量键同步、边界/固定输出和真机。
- DG-024 保持 `IN_PROGRESS`，不标 `DONE` / `VERIFIED`。
- 本轮文档回填未提交；按要求完成后暂停，主代理后续提交并同步。

---

## 2026-11-10 — 第 25 轮：DG-024 系统媒体音量控制原型（代码 + 待执行测试）

**Commit**：未提交（用户要求不提交、不推送）。

**完成任务编号**：DG-024（第三方媒体音量控制原型）。DG-025 / DG-026 / DG-050 未开始。

**为什么记**

路线图把“第三方媒体播放期间的系统媒体音量控制”列为当前优先目标，但本轮之前 DG-024 仍为 `TODO`，界面也没有任何
系统音量入口。本轮补上原型：只控制 Android 的 `STREAM_MUSIC` 通道，不启动 DreamGuard 声景、不申请音频
焦点，从而不打断第三方播放器。

**修改模块**

- 新增 `app/src/main/java/com/dreamguard/app/audio/SystemMediaVolume.kt`：纯 Kotlin 的
  `MediaVolumeBackend` / `SystemVolumeSnapshot` / `SystemVolumeWriteResult` / `SystemVolumeController`。
  读失败降级为 `isAvailable=false`；固定设备只读；单级输出不可调；写入请求钳制到平台最小/最大；写后回读，
  平台静默忽略写入判 `FAILED`；`refresh()` 仅在快照变化时通知，外部音量键/其他应用的变更与自身写入共用同一路径。
- 新增 `app/src/main/java/com/dreamguard/app/audio/AudioManagerMediaVolume.kt`：`STREAM_MUSIC` 的
  `AudioManager` 适配（API 28 以下 `minIndex=0`）与 `Settings.System` ContentObserver。**没有新增权限**，
  没有 `requestAudioFocus`，`setStreamVolume(..., 0)` 不请求系统音量面板。
- `app/src/main/java/com/dreamguard/app/ui/DreamGuardScreen.kt`：新增“系统媒体音量”卡（百分比、最小/最大、
  分级滑块、固定/单级/读失败/写失败文案与“重试”），并在服务未连接时也可用；`ON_START` 注册观察者并刷新、
  `ON_STOP` 反注册、`onDispose` 清理；原 DSP 增益卡改名“声景音量”。
- 新增 `app/src/test/java/com/dreamguard/app/audio/SystemMediaVolumeTest.kt`：12 个 JVM 测试，覆盖范围/钳制、
  固定设备、单级输出、读失败、写异常、平台静默忽略、外部变化通知去重、成功/失败写入的通知行为、失败恢复。

**测试设备 / 测试结果**

| 项目 | 结果 |
| --- | --- |
| 本会话命令环境 | 所有 `pwsh` / terminal 调用在启动阶段失败：`Error: SetNamedSecurityInfoW failed (Win32 5): grantWrite(E:\DreamGuard)`；按用户要求未改 ACL、未申请 `danger-full-access` |
| Gradle / JVM 测试 | **未执行**（无构建、无 `testDebugUnitTest`；12 个测试只是已写入） |
| APK / 模拟器 / 真机 | **未执行**；无音量读写、第三方播放器、固定音量设备或外部音量键证据 |

**证据边界**

- 没有编译，因此不能声称新代码可构建；没有运行测试，不能声称任何测试通过。
- 没有在 iQOO Z9T 或模拟器实测 `STREAM_MUSIC` 读写、固定音量/单级输出、外部变化同步，也没有验证
  “调节时第三方播放器继续播放”。
- 只控制系统媒体通道，**不**做第三方音频捕获、按应用分流、动态压缩或响度保护；打开控制卡不会自动启动或
  暂停第三方播放器。

**未解决问题及下一步**

- 主代理执行 `.\gradlew.bat --no-daemon --offline :app:testDebugUnitTest` 与 `:app:assembleDebug`，
  回填测试总数/失败数与构建结果；再按用户时间做模拟器/真机及指定第三方播放器实测。
- 真机验收前 DG-024 保持 `IN_PROGRESS`，不标 `DONE` / `VERIFIED`。

---

## 2026-11-10 — 调整第三方媒体音量控制优先级

- **Commit：**本条记录随路线图调整提交；代码版本以 Git 历史为准。
- **修改模块：**`docs/ROADMAP.md`、`docs/STATUS.md`、`README.md`。
- **任务编号：**新增 DG-024、DG-025、DG-026；DG-050 提前到原型后进行可行性验证。
- **测试设备：**本次为计划修订，未执行设备测试。
- **测试结果与证据：**本次未修改应用代码或执行功能测试；系统音量与播放捕获限制的官方文档链接见路线图。新增任务均为 `TODO`。
- **未解决问题：**目标第三方播放器、蓝牙及设备音量行为尚需实测；音频捕获能否替换原始输出尚未知。
- **下一步：**实现 DG-024，再按用户时间安排真机回归；功能构建完成后再开展 30 分钟长时测试。

---

## 2026-11-10 — 第 24 轮：API 35 x86_64 模拟器安装当前 Debug APK（播放状态 / 本地压缩就绪）

**Commit**：代码 `4b7ace322da31f06417f3dc09a591795ba14207f`（短 `4b7ace3`，已推送）；
本次日志/状态更新未提交。

**完成任务编号**：DG-021 / DG-022 / DG-023 的补充验证；三者状态仍为 `IN_PROGRESS`。

**操作与结果（主代理）**

- API 35 x86_64 模拟器安装并启动当前 Debug APK。
- 系统文件选择器选择合成的 **40 s / 44.1 kHz / 单声道 PCM16 WAV**。
- UI 显示播放中；格式提示出现“本地压缩就绪”；打开处理开关后 `checked=true`。
- `MediaSession` 为 `PLAYING`；应用进程存活。
- `AndroidRuntime:E` 日志未见 `FATAL EXCEPTION` / `UnsatisfiedLinkError`。

**证据边界**

- 只证明应用播放状态与 native 处理器准备就绪：`READY` 对应 native 库加载与句柄创建路径未报错。
- 不证明每个 block 调用 `nativeProcess` 或 JNI 返回值正确，不证明实际音频输出电平、听感、延迟/underrun、
  资源泄漏或真机通过；“就绪”是 UI 最近一次派发的状态，音频线程进入 `ACTIVE` 不会触发重组。
- 真机新版压缩与 30 分钟长稳未测；用户明确要求延后。

**未解决问题及下一步**

- 仍需设备端逐块 native 证据（计数/日志）、波形或电平、听感与真机回归；ASan/LSan 未做。
- 本次只更新日志/状态，未改代码、未提交/推送。

---

## 2026-11-10 — 第 23 轮：DG-022 契约修正、Debug 构建与原生测试实测

**Commit**：`4b7ace322da31f06417f3dc09a591795ba14207f`（短 `4b7ace3`，已推送）；
本日志第 24 轮更新未提交。

**完成任务编号**：DG-022（AudioProcessor 生命周期/格式切换）、DG-023（未支持格式的 UI 明示与开关可用性）。
DG-020 的 JNI 符号与句柄类型未改。

**为什么记**

第二十二轮代码从未编译。本轮对照 media3 **1.5.1** 官方源码复核发现：`BaseAudioProcessor.onFlush()`
返回 `void`，而第二十二轮写成 `override fun onFlush(): Boolean`，**会直接编译失败**。同时 media3 明确允许
`configure()` 之后、`flush()` 之前继续排队旧格式 buffer，而原实现在 `onConfigure()` 就销毁旧句柄并创建
新句柄，会在这段时间用新格式句柄处理旧格式音频。两处都必须先修，构建才有意义。

**修改模块**

- `app/src/main/java/com/dreamguard/app/audio/LocalAudioProcessor.kt`：
  `onFlush()` 改为 Unit；`onConfigure()` 只记录 pending 格式与协商结果，`onFlush()` 才创建/替换 native
  句柄（同格式 flush 只 `reset`）；新增 `layoutSupported`/`nativeLibraryAvailable`/`nativeFailure`，
  `nativeCapable` 纳入 `nativeError`；native 失败回退时同时回卷输入/输出 position；`close()` 终止处理。
- `PlaybackService.kt`：格式提示在开关关闭时也明示“多声道/缺库/失败 → 直通”。
- `ui/DreamGuardScreen.kt`：`Switch` 在已开启时即使 `processingAvailable=false` 也可关闭。
- `dsp-core/tests/test_pcm16_processor.cpp`：24→27 组（bypass 不改变压缩器状态、立体声残帧、
  输出容量大于输入）；`dsp-core/tests/test_header_c_compat.c` 已包含 PCM16 纯 C 段，但该测试本身
  本轮未单独编译/运行/报告（Debug 构建只编 dsp-core 库源码）。
- `app/src/test/.../LocalAudioProcessorTest.kt`：17→18 个（失败后仍可关闭处理），configure 用例改为
  `configure → flush` 的真实契约，并断言 configure 后未 flush 时旧句柄仍在用。

**构建反馈修复（主代理交叉编译反馈）**

- `pcm16_processor.cpp:231` 向 `dg_compressor_process` 传入 `dg_pcm_buffer`，而 `compressor.h` 第三参声明为
  `dg_pcm_view`，NDK 交叉编译报无匹配函数、构建中断。
- 修正方向取“让 API 匹配写语义”，而不是在调用方把 output 退化成只读 view：
  `dg_compressor_process` 的输出参数改为 `dg_pcm_buffer`（与 `dg_limiter_process`、`dg_pcm_*` 一致），
  删除 `compressor.cpp`/`limiter.cpp` 里残留的 `const_cast`；`pcm16_processor.cpp` 的输出描述符改为非 const。
- 同步修改 `test_compressor.cpp`（`mutable_view_of` → `mutable_buffer_of`，输出变量改 `dg_pcm_buffer`，
  in-place 用 `view_of` 输入 + `mutable_buffer_of` 输出）与 `test_header_c_compat.c` 的 `compressor_out`。
- 这是编译期类型修正，**不改变算法/行为**；修复后主代理重新编译并跑通（结果见下表）。

**测试设备 / 测试结果**

| 项目 | 结果 |
| --- | --- |
| 本会话命令环境 | 所有 `pwsh` 调用仍在启动阶段失败：`Error: SetNamedSecurityInfoW failed (Win32 5): grantWrite(E:\DreamGuard)`；未改 ACL、未申请 `danger-full-access` |
| media3 API 复核 | 以官方 1.5.1 源码逐项核对 `onFlush()`/`flush()`/`replaceOutputBuffer()`；随后 **Gradle 编译通过** |
| Gradle（主代理） | `.\gradlew.bat --no-daemon --offline :app:testDebugUnitTest :app:assembleDebug` **成功**；Debug NDK **arm64-v8a / armeabi-v7a / x86_64** 编译通过 |
| JVM 测试（主代理） | **80 tests / 0 failures / 0 skipped**（含 18 个 `LocalAudioProcessorTest` + 4 个 `NativePcmProcessorContractTest`） |
| C++ native（主代理，API 35 x86_64 模拟器） | `dg_pcm16_processor_tests` **206 checks / 0 failures**（27 组）；`dg_compressor_tests` **348 checks / 0 failures** |
| APK / 模拟器 / 真机播放 | **未安装、未运行**；没有真实 JNI 调用、音频输出、波形/听感证据 |

**未解决问题及下一步**

- 即使构建/测试通过，也只证明信号级、JNI 合约与接线正确；DG-022 验收“真实音频通过 DSP 后正常播放”仍需
  设备播放、波形/电平或听感证据，第二十一轮真机“正常”不能移用。
- JVM JNI 合约测试与 `FakeCompressor` mock **不等于实测 JNI**；`nativeProcess` 的真实调用、direct buffer
  行为与音频线程时序尚未在设备上验证。
- 用户明确长时间真机测试暂缓；未做 true peak、长时间听感、多设备、真实音频输出、多格式/多路由、
  进程重建、ASan/LSan、延迟/underrun。
- 本轮修改尚未提交；主代理将整理提交并推送。

---

## 2026-11-10 — 第 22 轮：DG-020/022 本地 PCM16 → native compressor 功能构建

**Commit**：未提交（用户要求不提交、不推送）。

**完成任务编号**：DG-020（新增 PCM JNI surface）、DG-022（AudioProcessor 接入 DG-013 压缩器）。
DG-021 / DG-023 的开关文案随 DG-022 更新，状态不变。

**为什么记**

第二十一轮的真机“正常”对应的是旧 **Kotlin 整数 −6 dB** 链路，`libdreamguard_dsp.so` 从未处理过本地文件。
用户要求先完成功能构建、再做真机测试，因此本轮把“PCM16 → C++ 压缩器 → PCM16”真正接进 Media3 播放链路，
并把不支持的格式/声道、库缺失与处理失败全部定义为**位精确直通**，避免本地播放被 DSP 故障静音。

**修改模块**

- `dsp-core/`：新增 `include/dreamguard/pcm16_processor.h`、`src/pcm16_processor.cpp`
  （PCM16→float→`dg_compressor`→PCM16；512 帧定长 scratch；`memmove` bypass；残帧/奇字节原样；
  半远离零量化 + 饱和；无 lookahead、无逐样本分配）、`tests/test_pcm16_processor.cpp`（24 组）、
  `CMakeLists.txt` 与 `tests/CMakeLists.txt` 接线。
- `app/src/main/cpp/jni_bridge.cpp`：新增 `NativePcmProcessor_nativeCreate/Process/Reset/Destroy/Version`
  5 个符号；direct buffer 校验与负错误码。
- `app/src/main/java/com/dreamguard/app/audio/`：新增 `NativePcmProcessor.kt`、`PcmCompressor.kt`；
  重写 `LocalAudioProcessor.kt`（native 主路径、字节数完全匹配才采用、失败/缺库/非 direct/多声道位精确回退、
  `NativeState`/`nativeError`、flush reset、format 变化重建句柄、reset/close 释放、>2 声道安全直通）；
  `PlaybackService.kt`（格式提示状态后缀、`localProcessingAvailable` 要求 `nativeCapable`、
  `onDestroy` 在 `player.release()` 后 `close()`）；`DreamGuardScreen.kt`（开关改名“本地压缩”与副标题）。
- `app/src/test/`：重写 `LocalAudioProcessorTest.kt`（17 个 fake 后端测试）、新增
  `NativePcmProcessorContractTest.kt`（4 个）。

**测试设备 / 测试结果**

| 项目 | 结果 |
| --- | --- |
| 本会话命令环境 | 所有 `pwsh` 调用在启动阶段失败：`Error: SetNamedSecurityInfoW failed (Win32 5): grantWrite(E:\DreamGuard)`；未改 ACL、未申请 `danger-full-access` |
| C++ 测试 | 已写入、**未编译、未运行**（无 checks 数） |
| JVM 测试 | 17 + 4 个已写入，**未运行**（无 tests 数） |
| APK / 模拟器 / 真机 | **未构建、未安装、未运行**；未做真机测试 |
| 其他 | 未提交、未推送、未删除任何用户文件或日志；旧未提交改动保留 |

**未解决问题及下一步**

- 主代理需执行 `docs/STATUS.md` 复现命令第 12 节：宿主 `ctest`（`dg_pcm16_processor` 与其他 7 个）、
  `:app:testDebugUnitTest --rerun-tasks assembleDebug`、必要时 NDK 交叉编译路径；结果回填后才可谈验收。
- 即使测试通过，也只证明信号级与接线正确；DG-022 验收“真实音频通过 DSP 后正常播放”仍需设备播放、
  波形/电平或听感证据，第二十一轮真机“正常”不能移用。
- 未验证：JNI 实际加载/符号解析、压缩器跨块大小/采样率的输出、arm64/armeabi 运行、ASan/LSan、
  延迟/underrun、多格式/多路由/进程重建/30 分钟长稳。
- 开关从“−6 dB”改为“本地压缩”：makeup −6.02 dB 保留安静素材的旧衰减下限，但这是**设计替代**，
  不是听感验证；不得声称音质、响度或 true peak 已通过。

---

## 2026-11-10 — 第 21 轮：真机（iQOO Z9T）本地音频端到端确认

**Commit**：未提交（用户要求不提交、不推送）。

**完成任务编号**：DG-021 / DG-022 / DG-023 的**真机证据补充**（状态仍 `IN_PROGRESS`）；DG-020 未改。

**为什么记**

第 20 轮的本地音频闭环只在 API 35 x86_64 模拟器上验证，模拟器无音频输出，等于“没听到声、没上过目标机”。
本轮主代理在目标机 iQOO Z9T 上安装当前 Debug APK，并由用户本人听、操作，首次拿到真机端到端确认；
但“正常”是对多项问题的**合并答复**、且未录音频/波形，所以必须把“确认了什么”与“仍未确认什么”分开写死。

**修改模块**

只改文档：`docs/STATUS.md`（第二十一轮摘要、总览 DG-021..023 行、新增“第二十一轮真机验证”节、环境记录第 16 条、
复现命令第 11 节、边界声明）、`docs/ROADMAP.md`（DG-021..023 状态登记与末尾总览）、`docs/DEVLOG.md`（本条）、
`README.md`（验证状态、本地音频说明、未验证清单）。**代码一行未改**；未提交、未推送；
旧的真机记录（锁屏约 20 s、`kill -9` 重开、环境音叠加）**原样保留**。

**测试设备**：iQOO Z9T（ADB `model` `V2352A`、`manufacturer` `vivo`、Android 16 / API 36）。

**测试结果**

| 项目 | 结果 |
| --- | --- |
| APK | Debug，`versionName 0.1.0` / `versionCode 1` / SHA256 `3ABD75F122320CCE3A7BD1BD493FE6B0A867C1B8996296DD6E44621C8F4C97D2` |
| 安装 / 启动 | `adb install` 成功；进程 pid **22780** |
| 同批证据 | 与该 APK 同批的 JVM 测试 **68 tests / 0 failures / 0 skipped**（第 20 轮）；本轮未重新构建 |
| 本地文件 | 用户在 Downloads 用系统选择器打开主代理生成的 40 s / 44100 Hz / 单声道 PCM16 WAV |
| 听感（合并答复） | 声音、−6 dB 开关变化、暂停恢复、爆音卡顿 → “**正常**”（未逐项展开） |
| 锁屏 | 约 15 秒声音持续 |
| 其他媒体 | 播放其他媒体后 DreamGuard 暂停 |
| 关闭本地音轨 | 原有 DSP 声景播放；调音量/音色、暂停正常，无爆音卡顿 |
| 日志 | 进程存活；MediaSession `PAUSED`；过滤 `AndroidRuntime:E` / `nativeloader` 无 `FATAL EXCEPTION`、无 `UnsatisfiedLinkError`；**无 nativeloader 成功行**（以实际出声为间接证据） |
| 30 分钟 / 多路由 / 多格式 / 进程重建 / 真实峰值 | **未执行** |

**未解决问题及下一步**

- `DG-021` 的验收原文“iQOO Z9T 本地音频正常播放”已由用户在目标机确认；但交付物“播放进度”仍未实现，
  故仍 `IN_PROGRESS`。下一步补播放进度与错误路径，再做多格式/多采样率（非 44.1 kHz 单声道 PCM16）。
- `DG-022` 仍缺输出波形/电平与逐缓冲证据；“DSP Core 集成”未做（`libdreamguard_dsp.so` 是生成式声景，
  不是 PCM 效果器），−6 dB 是 Kotlin 整数增益，**不得**描述成 C++ 压缩/限幅。
- `DG-023` 主路径已真机走通；错误路径、进程重建、通知栏/锁屏媒体键未测，“自然模式/响度显示”未做。
- 真机长稳（30 分钟）、蓝牙/有线/外放路由、来电打断、系统主动回收仍是独立剩余项。

---

## 2026-11-10 — 第 20 轮：本地音频闭环的真实验证（构建/单测/模拟器）

**Commit**：未提交（用户要求不提交、不推送）。

**完成任务编号**：DG-021 / DG-022 / DG-023 的验证记录（状态仍 `IN_PROGRESS`）；DG-020 未改。

**为什么记**

第 19 轮只有源码与静态核对；本轮主代理在本机真实执行构建与单测，并在模拟器上走通文件选择与播放控制，
需要把“已执行到什么程度”和“仍旧没有的证据”分开登记，避免把模拟器 UI 状态当成完整验收或听感验证。

**修改模块**

只改文档：`docs/STATUS.md`（第二十轮摘要、总览、DG-021/022/023 节、环境记录、复现命令第 10 节）、
`docs/ROADMAP.md`（DG-021/022/023 状态登记与末尾总览）、`docs/DEVLOG.md`（本条）、`README.md`
（测试与本地音频说明）。**代码一行未改**。

**测试设备**：主代理本机 Gradle；Android API 35 **x86_64 模拟器**（无音频输出）。

**测试结果**

| 项目 | 结果 |
| --- | --- |
| `.\gradlew.bat --no-daemon :app:testDebugUnitTest --rerun-tasks :app:assembleDebug` | **成功** |
| JVM 测试 | **68 tests / 0 failures / 0 skipped**（既有 58 + `LocalAudioProcessorTest` 10） |
| 模拟器安装/启动 | 成功 |
| 文件选择 | 系统选择器从 Downloads 选合成 40 s / 44.1 kHz / 单声道 PCM16 WAV |
| UI | 文件名、`16-bit PCM · 44100 Hz · 1 ch`、播放中 |
| MediaSession | 播放/恢复均为 `PLAYING` |
| −6 dB 开关 | `checked=true` |
| 暂停 | UI 显示已暂停；恢复后 `PLAYING` |
| 日志 | 无 `AndroidRuntime FATAL` |
| 听感 / 波形 / 真机 | **未执行**；模拟器无音频输出，未在 iQOO Z9T 运行 |

**未解决问题及下一步**

- 仍缺 iQOO Z9T 真机播放、听感、输出波形/电平测量、延迟与 underrun 统计。
- 当前 −6 dB 是 Kotlin 整数增益级，**不是** C++ 压缩器/限幅器，也未接入 `libdreamguard_dsp.so`；
  “真实音频通过 DSP”仍未达成，DG-022 保持 `IN_PROGRESS`。
- 未实测“关闭本地回内置轨”“错误路径”“通知栏暂停/恢复”与进程重建恢复。
- `DG-014` 的 true peak 与听感、DG-012 的 `--libebur128`/真机仍是独立剩余项；LUFS 的 14 PASS 只是
  相对 FFmpeg 9.0.2 metadata（约 0.01 LU 分辨率）的通过，不构成标准认证。

---


**Commit**：未提交（用户要求不提交、不推送）。

**完成任务编号**：DG-021（本地文件播放/暂停/恢复）、DG-022（AudioProcessor）、DG-023（最小 UI）；
DG-020 未改 JNI。

**为什么记**

本轮把 Phase 2 的“选本机音频 → 播放 → PCM 处理 → 真 bypass”写成可运行的最小闭环，但本会话 `pwsh`
与 `terminal_open` 仍在启动阶段失败（`Error: SetNamedSecurityInfoW failed (Win32 5): grantWrite(E:\DreamGuard)`），
按用户要求未改 ACL、未提权，因此**没有编译、没有单测、没有安装、没有播放**。代码结论全部来自静态核对，
必须由主代理独立跑构建与设备验证后才能升级状态。

**修改模块**

- 新增 `app/src/main/java/com/dreamguard/app/audio/LocalAudioProcessor.kt`（`BaseAudioProcessor` 子类：
  16-bit PCM 协商、逐样本 −6 dB 整数减半、逐字节 bypass、flush/reset/EOS）。
- 新增 `app/src/main/java/com/dreamguard/app/audio/LocalAudioRenderersFactory.kt`
  （重写 `buildAudioSink`，用 `DefaultAudioSink.Builder.setAudioProcessors` 注入）。
- `app/src/main/java/com/dreamguard/app/audio/PlaybackService.kt`：本地 URI 选择/持久化授权/进程重建恢复、
  播放/暂停/恢复/关闭、与内置 loop 互斥、层逻辑本地守卫、错误文案。
- `app/src/main/java/com/dreamguard/app/ui/DreamGuardScreen.kt`：`OpenDocument` 选择器与本地音频卡
  （选择/更换、播放/暂停、关闭、处理开关、格式提示）。
- 新增 `app/src/test/java/com/dreamguard/app/audio/LocalAudioProcessorTest.kt`（10 个 JVM 测试）。
- 文档：`docs/STATUS.md`（第十九轮摘要、总览 DG-020..023、四节、环境记录第 15 条、复现命令第 10 节）、
  `docs/ROADMAP.md`（DG-020..023 状态登记）、`docs/DEVLOG.md`（本条）、`README.md`（架构与文件树）。

**测试设备**：无。本会话无法运行任何命令。

**测试结果**

| 项目 | 结果 |
| --- | --- |
| `:app:testDebugUnitTest`（含新增 10 个） | **未运行**；不登记 checks 数 |
| `assembleDebug` / 安装 / 播放 | **未执行** |
| 真机（iQOO Z9T）/ 听感 / 延迟 | **未执行**，不得声称已验证 |
| 静态核对 | 已逐行核对四个 Kotlin 文件与测试；media3 1.5.1 的 `setAudioProcessors` / 三参 `buildAudioSink` / `BaseAudioProcessor` 契约已对源码确认 |

**未解决问题及下一步**

主代理跑 `.\gradlew.bat --no-daemon :app:testDebugUnitTest --rerun-tasks` 与 `assembleDebug`，
再按 `docs/STATUS.md` 复现命令第 10 节手工验证：选择文件、播放/暂停/恢复、处理开关可听差异、关闭回内置轨、
错误文案。通过后 DG-021/022/023 才能从 `IN_PROGRESS` 升级；iQOO Z9T 与听感证据仍需单独取得。

---

## 2026-11-10 — 第 18 轮：DG-012 高精度 metadata 对照（14 PASS / 0 FAIL，worst 0.010 LU）

**Commit**：未提交（用户要求不提交、不推送）。

**完成任务编号**：DG-012（LUFS 与可信参考工具对比）。

**为什么记**

第 17 轮的高精度脚本在 ffmpeg probe 处假失败，修复后尚未重跑；本轮主代理用同一 CSV 与 16 个 WAV
实际执行 metadata 路径，首次拿到 0.01 LU 量级的对照数字，需要把“数值通过”和“静音/短文件 NO-DATA”
分开登记，避免把 `-70.000` 哨兵当成测量值。

**修改模块**

- 只改文档：`docs/STATUS.md`（第十八轮摘要、DG-012 行与状态、总览）、`docs/ROADMAP.md`（DG-012 状态登记）、
  `docs/DEVLOG.md`（本条）。代码、脚本、判定规则与容差未改；既有 CSV/WAV/报告全部保留。
- 报告由主代理生成：`dsp-core/build/lufs-compare-android/reference-report-metadata.md`。

**测试设备**：主代理本机；FFmpeg 9.0.2 构建目录内副本（非系统安装）；输入为同一次 `adb pull` 的 CSV + 16 WAV。

**测试结果**

| 项目 | 结果 |
| --- | --- |
| 可比数值样本 | **14 PASS / 0 FAIL / 0 SKIP** |
| 最大显示差 | **0.010 LU**（`stereo_asym_L-6_R-12dB_48k`、`stereo_1k_-20dB_192k`） |
| NO-DATA | 2（silence、sub-400 ms）：`lavfi.r128.I=-70.000` 为 `ABS_THRES` 哨兵，全部 M 块 < −70；单列不计入 PASS |
| 有效分辨率 | 约 0.01 LU（`HIST_GRAIN=100`）；三位小数只是格式 |
| 真机 / 泄漏 / `--libebur128` | **未做**；DG-012 记 `DONE`、**不标 `VERIFIED`** |

**未解决问题及下一步**

LUFS 仍标 `PROVISIONAL`（未做标准认证式验证）；true-peak 与听感不在本轮范围。

---


**Commit**：未提交（用户要求不提交、不推送）。

**完成任务编号**：DG-014（sample-peak 限制器首轮实测）；DG-012（高精度脚本 probe 修复，未重跑）。

**为什么记**

DG-014 的代码、测试与文档在上一轮写入但本会话无法执行命令。本轮主代理在 NDK x86_64 / API 35 模拟器
实际运行，给出了 checks 数；同时主代理两次执行 DG-012 高精度对照，脚本都在 ffmpeg probe 处 exit 3
（一次是管道提前终止，一次是版本正则的 CRLF 行尾问题），需要把“真实执行结果”和“脚本故障”分开登记，
避免把 probe 假失败当成参考工具不可用。

**修改模块**

- `dsp-core/src/limiter.cpp`、`dsp-core/tests/test_limiter.cpp`、`dsp-core/CMakeLists.txt`、
  `dsp-core/tests/CMakeLists.txt`、`dsp-core/tests/test_header_c_compat.c`（上一轮写入，本轮未改）。
- `dsp-core/tools/lufs_compare/compare_with_ffmpeg.ps1`：新增 `Invoke-FfmpegProbe`，完整捕获
  stdout+stderr、先保存真实 exit code、校验 `ffmpeg version` 行；版本检查改为全文搜索、去掉在 Windows
  CRLF 下不成立的行锚点；`-filters` 同样检查 exit code。
- 文档：`docs/STATUS.md`、`docs/ROADMAP.md`、`dsp-core/README.md`、`dsp-core/tools/lufs_compare/README.md`。

**测试设备**：主代理本机 + NDK x86_64 / API 35 模拟器；FFmpeg 9.0.2 仍在构建目录内，非系统安装。

**测试结果**

| 项目 | 结果 |
| --- | --- |
| `dg_limiter`（x86_64 模拟器） | **2113 checks / 0 failures** |
| Gradle `testDebugUnitTest assembleDebug assembleRelease` | 成功；Debug/Release 三 ABI 构建通过，JVM `up-to-date` |
| DG-014 true-peak 过采样 / 真机 / 听感 / 播放链路 | **未做**，不得宣称 true-peak 验收通过 |
| DG-012 高精度脚本 | 两次执行均 exit 3（管道提前终止 / CRLF 行尾正则）；第二次修复后**未重跑**，无新报告 |

**未解决问题及下一步**

DG-014 仍需至少 4× 过采样 true-peak 测量、真机/听感与播放链路验证；DG-012 需主代理用新
`-Report dsp-core/build/lufs-compare-android/reference-report-metadata.md` 重跑，确认静音/0.3 s
是否满足 `NO-DATA` 四条件。

---

## 2026-11-10 — 第 15 轮：DG-012 参考对照首次真实执行（14 PASS / 2 FAIL，FFmpeg 9.0.2）

**Commit**：未提交（用户要求不提交、不推送）。

**完成任务编号**：DG-012（LUFS 与可信参考工具对比）；DG-013 本轮无改动。

**为什么记**

第九至十四轮把对照工具链写完了，但一条对照都没跑过。本轮主代理用第十四轮新增的 `-Csv/-WavDir`
预生成模式，对 `adb pull` 回来的 16 个 WAV 真正调用 FFmpeg `ebur128`。本会话 `pwsh` 仍在启动阶段失败
（`SetNamedSecurityInfoW failed (Win32 5): grantWrite(E:\DreamGuard)`），所以**本会话没有执行任何命令**，
只逐行读取并核对了落盘的报告与 CSV，然后更新台账；代码零改动。

**修改模块**

- 只改文档：`docs/STATUS.md`（第十五轮小节、总览行、证据边界改写、环境记录第 14 条、复现命令 8c-2 标注、
  边界声明新增三条）、`docs/DEVLOG.md`（本条）、`docs/ROADMAP.md`（DG-012 状态登记）、
  `dsp-core/README.md`、`dsp-core/tools/lufs_compare/README.md`（运行历史与 −70 说明、精度限制）。
- 代码零改动：`dsp-core/src/lufs.cpp`、`dsp-core/include/dreamguard/lufs.h`、`dg_lufs_compare.cpp`、
  `compare_with_ffmpeg.ps1` 的判定规则与容差均未改；`dsp-core/build/lufs-compare-android/` 下的 CSV/WAV/报告一律保留。

**测试设备**：主代理本机；工具由 NDK x86_64 交叉编译后在 API 35 模拟器上生成数据；参考为
`dsp-core/build/reference-deps/imageio_ffmpeg/binaries/ffmpeg-win-x86_64-v9.0.2.exe`（构建目录内，非系统安装）。

**测试结果**

| 项目 | 结果 |
| --- | --- |
| 对照用例 | 16（单/立体声；8k/44.1k/48k/96k/192k；−6…−60 dBFS；100 Hz/1 kHz/10 kHz；非对称；噪声；电平阶跃；静音；0.3 s） |
| PASS / FAIL / SKIP | **14 / 2 / 0** |
| 最大绝对差 | **0.049 LU**（`stereo_10kHz_-20dB_48k`：我方 −16.649118 vs ffmpeg −16.6） |
| 其余 | 1 kHz 各电平 +0.007 / −0.004；44.1k +0.009；96k −0.011；192k −0.020；100 Hz −0.025；非对称 −0.030；电平阶跃 +0.030 |
| 整体 verdict | **FAIL**，脚本 exit 1 |
| 报告 | `dsp-core/build/lufs-compare-android/reference-report.md`（91 行）+ 同名 `.csv`（17 行） |

- 两项 FAIL 是**定义问题**，不是数值比较：`stereo_silence_2s_48k` 与 `stereo_1k_-20dB_0p3s_48k`
  我方 `integrated_ready=0`、`integrated=-inf`，ffmpeg 打印 **−70.0**。
- **−70.0 的含义（按 FFmpeg 源码核实）**：`libavfilter/f_ebur128.c` 有 `#define ABS_THRES -70`；
  `init()` 里 `ebur128->integrated_loudness = ABS_THRES;`；`filter_frame()` 仅当 `loudness_400 >= ABS_THRES`
  且 `nb_integrated > 0` 时才覆盖该字段。数字静音时 `power_400` 钳到 `1e-12` → `-120.7 LUFS`，永不进门槛；
  0.3 s 时 400 ms 缓存 `filled` 恒为 0，同样不进门槛 ⇒ 字段停在初值并以 `I: %5.1f LUFS` 打印成 −70.0。
  所以它是“无合格块”的哨兵，不是测量值，也不构成任何标准认证。
- 处置：我方 `-inf` 与 ffmpeg 哨兵指向同一事实但表达不同 ⇒ **不是数值一致**；两项保持 `FAIL` / 待人工裁定，
  不改规则、不改实现、不调容差。
- 精度限制：ffmpeg 汇总只打印 **0.1 LU**，任何单例 delta 自带 ±0.05 LU 取整误差；0.049 与取整同量级，
  因此这些偏差既不能证明实现差异，也不能证明高度一致。0.1 LU 容差 = 1 个量化步。

**状态变化**：DG-012 仍 `IN_PROGRESS`、LUFS 仍 `PROVISIONAL` —— 误差表有了，但整体未通过（2 项待裁定），
且参考精度不足以定论窄带/采样率差异；真机、泄漏检测、`--libebur128` 仍未做，故不标 `DONE`/`VERIFIED`。
DG-013 不受影响：独立测试链 `DONE`（不 `VERIFIED`），348/0 未重跑、无新证据、未接入播放链路。

**未解决问题**

1. 静音与 0.3 s 两项的**约定差异**需要书面裁定（采纳“ffmpeg 的 −70.0 ≡ 无定义”单独立证据，或继续 FAIL 留档）。
2. 参考精度不足：需要三位小数读数（`ebur128=metadata=1` + `ametadata`/`ffprobe`；有效分辨率约 0.01 LU，
   三位小数只是格式，见第十六轮修正），或装 libebur128 跑 `--libebur128`，才能在**不调容差**的前提下判断
   1 kHz 的 0.024 LU 偏差与 100 Hz/10 kHz 的 0.025/0.049 偏差。
3. 非 48 kHz 的系数推导仍是一阶近似（192k 上 −0.020 LU），未被证伪也未被证实。
4. 真机、arm64/v8、ASan/LSan、宿主 `ctest` 仍未执行；6 声道以上、超 4 s 长节目、< 8 kHz 采样率未覆盖。

**下一步**：先让主代理决定第 1 项的裁定口径（需要用户授权改判定规则才能动脚本）；同时用 metadata/`ffprobe`
路径取三位小数读数（有效约 0.01 LU），对 1 Hz–10 kHz、8k–192k 的同一批 WAV 再做一遍对照，把误差表补到
3 位小数后再谈 DG-012 的 `DONE`。DG-013 保持不动。

---

## 2026-11-10 — 第 8 轮：DG-012 修复后复测通过、三 ABI 构建（仅更新文档）

**Commit**：未提交（用户要求不提交、不推送）。

**为什么记**

第 7 轮修正了 `test_lufs.cpp` 的 gating 期望（单声道 3.0103 LU），需要主代理复测确认修复有效；
同时要记录 Gradle 构建对三个 ABI 的结论。本轮**没有改任何代码**：命令环境仍不可用
（`SetNamedSecurityInfoW failed (Win32 5): grantWrite(E:\DreamGuard)`），全部结果由主代理在本机执行后回报。

**修改模块**

- 只改文档：`docs/STATUS.md`（第八轮复测小节、ABI 证据边界、环境记录第 7 条、总览行、验收状态与边界声明订正）、
  `docs/ROADMAP.md`（DG-012 状态登记）、`docs/DEVLOG.md`（本条）、`README.md`、`dsp-core/README.md`。
- 代码零改动；`dg_lufs`/`dg_meter` 实现与测试保持第 7 轮结束时的状态。

**测试设备**：主代理机器，Android NDK 交叉编译 x86_64，API 35 模拟器；Gradle 构建 Debug/Release。

**测试结果**

| 项目 | 结果 |
| --- | --- |
| `dg_lufs_tests`（修复后，12 组） | **238 checks / 0 failures** |
| gating 诊断 | `blocks=67 after_abs=61 used=40 integrated=-23.1696` |
| `dg_meter_tests`（10 组） | **207 checks / 0 failures** |
| `dg_header_c_compat`（扩展后，C 编译 / C++ 链接） | **PASSED** |
| `testDebugUnitTest` | 成功，JVM 任务 `up-to-date`（沿用 58 tests / 0 failures） |
| `assembleDebug` / `assembleRelease` | 成功，Debug/Release 三种 ABI 的 native 库均构建通过 |
| 真机 / ASan-LSan / 参考工具误差对比 | 未执行（本机 PATH 无 `ffmpeg`/`ffprobe`） |

- 数值复核：`integrated = −23.1696` 与解析预期 `20log10(0.1) − 10log10(2) = −23.0103` 相差 0.159 LU，来源是
  3 个 75/50/25 % 过渡块拉低约 0.17 LU 再叠加 ≈0.024 LU 校准偏差，和第 7 轮的预估一致，不是容差掩盖。
- `after_abs = 61`：证实 loud→silence 的滤波振铃尾块高于 −70 LUFS。测试按 `>= 60` 断言正是为了兼容
  60/61 两种实现行为，实测落在 61。

**状态变化**：DG-012 仍为 `IN_PROGRESS`、LUFS 仍为 `PROVISIONAL`——路线图验收项“与可信参考工具对比并记录误差”
未执行，真机与泄漏检测也未做，因此不标 `DONE`、不标 `VERIFIED`。三 ABI 只证明“能编译链接”，
只有 x86_64 跑过测试。

**未解决问题**

1. LUFS 无参考工具读数：`ffmpeg`/`ffprobe` 不在 PATH，需要安装或用 libebur128 写一个小对比程序，
   对同一批 WAV 记录逐条误差。
2. arm64-v8a / armeabi-v7a 未运行测试（只编译）。
3. 真机回归、ASan/LSan 泄漏检测未做；宿主 `cmake`/`ctest` 仍未跑。

**下一步**：准备 3–5 个 WAV（1 kHz 校准、分级电平、静音混段、44.1/48/96 kHz、单/立体声），
用 libebur128 或 ffmpeg `ebur128` 得到参考读数，填出误差表后再讨论 DG-012 从 `IN_PROGRESS` → `DONE`。

---

## 2026-11-10 — 第 7 轮：DG-012 首跑失败定位与修复（测试期望错误，非门限算法）

**Commit**：未提交（用户要求不提交、不推送）。

**为什么记**

主代理用 Android NDK 交叉编译 x86_64 目标并在 API 35 模拟器上首跑 DG-012 测试：
`dg_meter` **207 checks / 0 failures**，`dg_lufs` **227 checks / 2 failures**，失败都落在
`dsp-core/tests/test_lufs.cpp` 的 gating 用例（修复前第 326、327 行）。按要求先查根因再改，
不许靠放大容差掩盖。

**根因**

- 该用例的信号是**单声道**、峰值 0.1 的 1 kHz 正弦，期望却写成 `20*log10(0.1) = −20 LUFS`。
- 正弦的 LUFS 是其 RMS 电平（比峰值低 3.0103 dB），且 BS.1770 对单声道与立体声都用 G = 1.0、
  功率按声道求和，所以**单声道正弦比同峰值立体声正弦低 10log10(2) = 3.0103 LU**。正确读数 **−23.01 LUFS**。
- 同一文件较早的 `test_stereo_calibration_and_channel_sum()` 本来就断言
  `mono_values.integrated_lufs ≈ peak_dbfs − 10log10(2)`：校准约定一直是写对的，**只有 gating 用例漏了这一项**。
- 实现侧复核：相对门阈值 = 过绝对门块均值 / 10（−10 LU，即能量 1/10）≈ 0.069×loud 块能量；
  quiet 段比 loud 低 30 dB（能量 0.001×），低约 68 倍，被正确剔除；`blocks_used == 40`
  （37 个全 loud 块 + 3 个 75/50/25 % 过渡块）；绝对门后 ≥ 60 块（60 个有效块，若 loud→silence 的
  滤波振铃尾块仍高于 −70 LUFS 则为 61，测试只断言 `>= 60`）。**门限算法本身未改。**

**修改模块**

- `dsp-core/tests/test_lufs.cpp`：
  1. `test_gating()`：期望改为 `20*log10(0.1) − 10log10(2)`，并加 ±1 LU 区间；打印实际
     blocks/after_abs/used/integrated 便于下次定位；计数断言收紧为精确值 `blocks == 67`、
     `blocks_used == 40`（及 `blocks_after_absolute_gate >= 60` 且 `> blocks_used`）。
  2. 新增 `test_relative_gate_keeps_near_blocks()`：4 s 峰值 0.1 + 2 s 峰值为 −9 dB（功率 0.126）的段，
     57 个块全部高于相对门，断言 `blocks_used == blocks_after_absolute_gate == 57` 且
     integrated 与手算块模型（37 + 1.5 个 loud 能量 + (17 + 1.5)×0.126 个 quiet 能量）差 ≤ 0.5 LU。
     这是“门限不能被调得过严”的回归，与 gating 用例形成两侧夹逼。
- 文档：`docs/STATUS.md` DG-012 节新增“第七轮：首次设备运行”小节并订正 ABI 证据边界
  （x86_64 有真实编译/运行证据；arm64-v8a、armeabi-v7a 仍未构建）、总览行、边界声明；
  本条目；`README.md`/`dsp-core/README.md` 的 LUFS 组数与运行记录同步。

**测试设备**：主代理机器，Android NDK x86_64，API 35 模拟器（本会话无命令环境，未参与执行）。

**测试结果**

| 项目 | 结果 |
| --- | --- |
| `dg_meter_tests` | **207 checks / 0 failures**（第七轮，x86_64 模拟器） |
| `dg_lufs_tests`（修复前） | **227 checks / 2 failures**，`test_lufs.cpp:326`、`:327`（gating） |
| `dg_lufs_tests`（修复后） | **未执行**（新增 1 个用例，check 总数会变；修复后数字尚不存在） |
| `dg_header_c_compat`（扩展后） | 未重编译、未运行 |
| arm64-v8a / armeabi-v7a | 未构建 |
| 真机 / ASan / 参考工具对比 | 未执行 |

**状态变化**：DG-012 仍为 `IN_PROGRESS`，不标 `DONE`、不标 `VERIFIED`；LUFS 仍为 `PROVISIONAL`。

**未解决问题**

1. 修复后需重跑 `dg_lufs_tests`，确认 2 处失败消失且新增用例通过（预期约 −24.48 LUFS）。
2. 仍未与 libebur128 / ffmpeg ebur128 对比，误差表为空。
3. x86_64 之外的两个 ABI 未构建；真机与泄漏检测未做。

**下一步**：主代理重跑 x86_64 的 `dg_meter_tests`/`dg_lufs_tests`；随后用 NDK 构建 arm64-v8a；
再择机用 `ffmpeg -filter_complex ebur128` 或 libebur128 对同一批 WAV 做误差对比，把结果写进
`docs/STATUS.md` 的 DG-012 节。

---

## 2026-11-10 — 第 6 轮：DG-012 计量与 LUFS（只写代码，未编译/未运行）

**Commit**：未提交（用户要求不提交、不推送）。

**为什么记**

按 `docs/ROADMAP.md` 推进 DG-012（RMS、Peak、分析窗口、Meter API、LUFS）。用户已声明**当前无实机**，
只处理可独立验证的工作；本轮命令环境仍在启动阶段失败
（`Error: SetNamedSecurityInfoW failed (Win32 5): grantWrite(E:\DreamGuard)`，任何命令包括 `pwd` 都是这个错误），
因此产出物是**代码、测试与文档**，构建与测试留给本机执行。

**修改模块**

- `dsp-core/include/dreamguard/meter.h` + `dsp-core/src/meter.cpp`（新）：流式计量器
  - `dg_meter_create/destroy/reset/reset_peak_hold/process/snapshot` + `dg_meter_linear_to_dbfs`；
  - 指标：块 peak/RMS（最近一次非空 process）、滚动窗口 peak/RMS、peak-hold、累计 RMS 与帧数、
    clip 计数（`|x| > 1.0` 的有限样本）、非有限样本计数；
  - 窗口 `window_ms` 默认 400、范围 1–1000，窗口长度 = `round(fs*window_ms/1000)` 帧；
    `|x|` 存在环里，窗口统计由环扫描得出，48 kHz/400 ms 立体声占 153,600 B；
  - 明确**采样峰值不是 true peak**；NaN/±Inf 计数后按 0 参与统计；零帧 process 为 no-op。
- `dsp-core/include/dreamguard/lufs.h` + `dsp-core/src/lufs.cpp`（新，**PROVISIONAL**）：
  - BS.1770-4 式 K 权重（模拟原型 + 双线性变换，48 kHz 与公布表 1e-6 内一致）、
    400 ms 块 / 75 % 重叠 / −70 LUFS 绝对门 + 相对门（均值 −10 LU）、
    momentary（400 ms）、short-term（3 s，不足 3 s 为 −∞）、integrated；
  - 声道 1/2，采样率 8000–192000，G = 1.0，单位 LUFS，offset −0.691，静音/无数据一律 −∞ 且 `integrated_ready = 0`；
  - 立体声 1 kHz 正弦峰值 A 的读数 = `20log10(A) − 0.024 LU`（由系数在 1 kHz 复算 K ≈ +0.667 dB 得到，
    0.024 是已登记的系统偏差，不写成 0）。
- `dsp-core/tests/test_meter.cpp`（新，10 组）与 `dsp-core/tests/test_lufs.cpp`（新，11 组）；
  `dsp-core/tests/test_header_c_compat.c` 增加 meter/LUFS 两颗头的纯 C 冒烟；
  `dsp-core/CMakeLists.txt` 并入两个新源，`dsp-core/tests/CMakeLists.txt` 注册 `dg_meter`、`dg_lufs`。
  - 写完后做了一遍只读复核，修掉测试自身的两处错误（源码不需要改）：
    (a) 窗口用例原来用 10 ms 窗口配 480 帧块，零块一到窗口就整段变零，`sqrt(0.75)` 的断言不可能成立；
    改成 40 ms（1920 帧）窗口 + 三块 1.0 + 一块 0.0，才真正验证 75 % 满窗；
    (b) 分块一致性用例原来还比较 `block_*`，而 `block_*` 按契约就是“最近一次调用”的统计，
    整块与分块本来就该不同；比较范围改为流态字段（window/peak_hold/total/clip/非有限计数）。
    另外补了一个 `denorm_min` 次正规样本用例，验证平方面在 double 中不会下溢归零。
    (c) 块存储原来是“装满就丢弃新块”，与 `lufs.h`/`STATUS.md` 写的“丢最旧”相反，也会让超过 1 小时的
    流把 integrated 冻结在第一小时；改成固定数组环形覆盖（`write_index = (block_count - 1) % 36000`），
    `blocks_dropped` 记被覆盖的最旧块，头文件里 `blocks_dropped` 的注释同步改成“被环形覆盖的最旧块”。
- `docs/STATUS.md`（新增 DG-012 节、总览行、环境记录第 6 条、复现命令第 7 节、边界声明）、
  `docs/ROADMAP.md`（DG-012 状态登记 + 末尾当前状态）。

**测试设备**

- 本轮**没有设备**：未在本会话执行任何命令。

**测试结果**

| 项目 | 结果 |
| --- | --- |
| `cmake` 宿主构建 + `ctest`（`dg_meter`、`dg_lufs`、`dg_pcm`、`dg_lifecycle`、`dg_header_c_compat`） | **未执行** |
| NDK x86_64 交叉编译 + API 35 模拟器 | **未执行** |
| `assembleDebug` / `assembleRelease`（三 ABI） | **未执行**；只有静态核对（`abiFilters` = arm64-v8a / armeabi-v7a / x86_64） |
| 与可信参考工具（libebur128、ffmpeg 等）对比 LUFS 并记录误差 | **未执行**，也没有可引用的第三方读数 |
| 泄漏检测（ASan/LSan）、真机回归 | **未执行** |

**状态变化**

- DG-012：`TODO` → **`IN_PROGRESS`**。不标 `DONE`：验收“与可信参考工具对比并记录误差”未做；
  也不标 `VERIFIED`：没有任何构建或运行证据。

**未解决问题**

1. 三个新 `.cpp` 与两个新测试**从未被编译器检查过**（本会话 `pwsh` 不可用），存在编译错误/警告的可能；
   特别是 `-Wall -Wextra -Wpedantic` 与 MSVC `/W4` 下未验证。
2. `dg_header_c_compat` 的链接行已加入 `lufs.cpp`/`meter.cpp`，未重编译。
3. LUFS 只在 48 kHz 系数上与公布表自检；44.1/96 kHz 与参考工具的误差未知；true peak 与 LRA 未实现。
4. 1 kHz 校准点 ≈0.024 LU 的系统偏差（K(1 kHz) ≈ +0.667 dB 与 −0.691 offset 不抵消）是否需要补偿，
   要与参考实现对比后再决定，本轮不改。

**下一步**

1. 在本机执行 `docs/STATUS.md` 复现命令第 7 节（宿主 CMake + ctest；无宿主编译器则走 NDK + 模拟器路径），
   拿到 `dg_meter`/`dg_lufs` 的真实 checks/failures。
2. 用 libebur128（或 ffmpeg `ebur128` 滤镜）对同一批 1 kHz 正弦、静音、噪声、−20/−50 dBFS 门控信号测 LUFS，
   把差值写进 DG-012 证据；只有误差可接受时才把 LUFS 从 PROVISIONAL 升级。
3. 三 ABI `assembleDebug assembleRelease` 通过后，再把 ABI 兼容从“静态核对”升级为构建证据。

---

## 2026-11-10 — 第 5 轮：记录主代理对最新代码的复测（未改代码）

**Commit**：未提交（用户要求不提交、不推送）。本轮**只更新记录**，没有修改任何源码、测试或 Gradle 配置。

**为什么记**

第 4 轮修正了 PCM 缓冲别名/重叠与索引溢出，并把 `dg_pcm` 测试扩到 24 组，但这些改动在本会话无法编译运行
（`pwsh` 仍报 `Error: SetNamedSecurityInfoW failed (Win32 5): grantWrite(E:\DreamGuard)`）。
主代理在其本机对**别名修正后的最新代码**复测，本轮把这些结果按实际写入台账。

**测试设备（主代理本机）**

- 构建机：Windows，JDK 17，Android SDK `E:\AgentImeTest\sdk`，NDK 27.0.12077973
- 运行设备：Android API 35 x86_64 模拟器

**测试结果**

| 命令 | 结果 |
| --- | --- |
| `testDebugUnitTest assembleDebug assembleRelease`（最新别名修正代码） | **BUILD SUCCESSFUL**；三个 ABI 的原生库均构建通过 |
| JVM 单测 | `up-to-date`（未重跑；58 tests / 0 failures 仍是第 4 轮报告的结果） |
| NDK x86_64 交叉编译 + 模拟器 `dg_pcm_tests`（最新代码，24 组） | **139 checks / 0 failures / PASSED** |
| `dg_lifecycle_tests` | 1328 checks / 0 failures —— 属**本轮未修改部分**，为此前结果，本轮未重跑 |
| `dg_header_c_compat` | 通过 —— 同上，未修改部分 |
| 泄漏检测（ASan/LSan） | **未执行** |
| 真机（V2352A / Android 16）回归 | **未执行**（未针对当前代码） |

**状态变化**

- DG-011：`IN_PROGRESS` → **`DONE`**（不标 `VERIFIED`）。理由：验收两项（输出格式正确、旁路输入输出一致）
  对当前代码有 139 checks / 0 failures 的可核实证据；但 PCM 助手**未接入播放链路**、**未处理真实音频**，
  泄漏检测与真机回归也未做，因此不满足 `VERIFIED` 的定义。
- DG-002 / DG-003：证据补上第 5 轮构建（Debug + Release、三 ABI 原生库）；JVM 测试为 `up-to-date`，
  没有新的测试报告，故仍引用第 4 轮的 58 tests / 0 failures。
- DG-010：`DONE` 不变；1328 checks 是第 4 轮结果，本轮未重跑。

**未解决问题**

- 泄漏检测（ASan/LSan）从未运行，DG-010 验收第 2 条仍无证据。
- 真机回归未针对当前代码；PCM 助手未接入播放链路，真实音频/设备格式未验证。
- 本会话仍无法执行命令（同一 ACL 错误），以上全部是主代理侧的结果，本会话未做独立复现。

**下一步任务**

1. 若要更新 JVM 报告：`.\gradlew.bat :app:testDebugUnitTest --rerun-tasks`。
2. 可选：ASan/LSan 构建补齐 DG-010 验收第 2 条（命令见 `docs/STATUS.md` 第 6 步）。
3. 真机回归（V2352A / Android 16）针对当前 APK 再跑一轮。

---

## 2026-11-10 — 第 4 轮：主代理实测通过 + PCM 缓冲别名 / 索引溢出修正

> 后续记录：第 5 轮（上一节）已由主代理复测本轮改动 —— 构建成功、`dg_pcm_tests` 139 checks / 0 failures，
> DG-011 已升为 `DONE`。本节“下一步任务”中的第 1、2 项已完成。

**Commit**：未提交（用户要求不提交、不推送）。

**为什么改**

第 3 轮交付后，主代理在本机执行了第三轮代码的完整验证，全部通过（见下表）。随后评审指出三处必须收口的风险：

1. `dg_pcm_bypass` / `dg_pcm_map_channels` 没有定义输入输出为同一缓冲或部分重叠时的契约。
   `memcpy` 在相同或重叠地址上是**未定义行为**，mono→stereo 原地写还会覆盖尚未读取的输入。
2. `frame * 2` 里 `frame` 是 `uint32_t`：在 64 位主机上先按 32 位相乘再提升，可能回绕后写到缓冲区之外。
3. `git diff --check` 报 `settings.gradle.kts` 末尾多一个空行。

**测试设备（主代理本机，第三轮代码）**

- 构建机：Windows，JDK 17，Android SDK `E:\AgentImeTest\sdk`，NDK 27.0.12077973，CMake 3.22.1
- 运行设备：Android API 35 x86_64 模拟器（跑 NDK 交叉编译产物）

**测试结果（主代理执行，第三轮代码；本轮改动尚未运行）**

| 命令 | 结果 |
| --- | --- |
| `clean testDebugUnitTest assembleDebug assembleRelease` | **成功**，Debug 与 Release 均产出；JVM **58 tests / 0 failures**（Gradle 从 build cache 恢复） |
| NDK x86_64 交叉编译 + 模拟器 `dg_lifecycle_tests` | **1328 checks / 0 failures** |
| NDK x86_64 交叉编译 + 模拟器 `dg_pcm_tests` | **110 checks / 0 failures** |
| NDK x86_64 交叉编译 + 模拟器 `dg_header_c_compat` | **通过** |
| 泄漏检测（ASan/LSan） | **未执行** |
| 真机（V2352A / Android 16）回归 | **未执行**（未针对当前代码） |

**改成了什么**

- `dsp-core/include/dreamguard/pcm.h`：新增 “Buffer aliasing” 契约段，并把规则写进每个函数注释：
  同指针原地仅允许样本数不变的操作（`dg_pcm_bypass`、`dg_pcm_apply_gain`、等声道 `dg_pcm_map_channels`）；
  声道转换必须使用两块不相交缓冲，同一指针也返回 `DG_ERR_INVALID_ARGUMENT`；任意部分重叠一律拒绝且不写样本；
  0 帧块与别名无关。另注明描述符承诺其指针确实容纳 `frame_count * channel_count` 个 float。
- `dsp-core/src/pcm.cpp`：新增 `in_place()` 与 `ranges_intersect()`（`uintptr_t` 区间比较，地址加法回绕时保守判为重叠）；
  旁路同指针直接返回，**不再对相同指针调用 `memcpy`**；增益原地逐样本“读一次写一次”；`map_channels` 先判别名再转换；
  mono→stereo / stereo→mono 的 `frame * 2` 改为 `static_cast<std::size_t>(frame) * 2`。
- `dsp-core/tests/test_pcm.cpp`：17 组 → 24 组，新增 7 组——同指针旁路、旁路部分重叠拒绝、同指针增益、
  增益部分重叠拒绝、等声道同指针映射、声道转换别名拒绝、65 536 帧大块往返。
  \>2^31 帧无法在测试中分配（需要 ≥8 GiB 缓冲），只由代码检查保证，测试注释已写明。
- `settings.gradle.kts`：删除文件末尾多余空行（`git diff --check` 不再报 “new blank line at EOF”），Kotlin 内容未变。

**未解决问题**

- 本轮新增/修改的代码**没有在本会话编译或运行过**：`pwsh` 仍报
  `Error: SetNamedSecurityInfoW failed (Win32 5): grantWrite(E:\DreamGuard)`。
- `dg_pcm_tests` 的断言数会从 110 增加，文档中的 110 是修正前的数字，复测后需更新。
- 泄漏检测、真机回归仍未做；PCM 助手仍未接入播放链路，也不是采集实现（DG-011 范围未扩张）。

**下一步任务**

1. 复测：`.\gradlew.bat clean testDebugUnitTest assembleDebug`；NDK 交叉编译并重跑 `dg_pcm_tests`
   （重点看 7 组新用例），同时重跑 `dg_lifecycle_tests` 与 `dg_header_c_compat`。
2. 用新数字更新 `docs/STATUS.md`，把 DG-011 从 `IN_PROGRESS` 升为 `DONE`（泄漏检测补齐前仍不写 `VERIFIED`）。
3. 可选：ASan/LSan 构建，补齐 DG-010 验收第 2 条。

---

## 2026-11-10 — DG-010 契约修正 + DG-011 PCM 助手（第 3 轮：仅静态核对，未执行构建/测试）

> 后续记录：第 4 轮（上一节）由主代理实测本轮代码，并修正了 PCM 缓冲别名与索引加宽问题。

**Commit**：未提交（沿用用户指示：不提交、不推送）。

**为什么改**

第 2 轮的 DG-010 实现用 `std::mutex` + `std::unordered_set` 活句柄注册表识别“已销毁/外来”句柄，有两个实际问题：
句柄就是裸指针，销毁后地址被新引擎复用会产生 **ABA**（旧句柄命中新实例），注册表能回答“地址在不在表里”，
却回答不了“这个地址换没换过世代”；另外每个 `dg_render` 都要进一次全局锁，`dg_destroy` 仍与在途调用竞速。
用 64 位世代 ID 修补的方案被评审否决：`armeabi-v7a` 是 32 位构建，不能把 64 位 ID 塞进 32 位指针再转回，
也不应为检测误用引入固定容量无锁注册表。

**改成了什么**

- `dsp-core/include/dreamguard/dsp_core.h`：错误码收敛为 6 个（删除 `DG_ERR_INVALID_HANDLE`）；句柄仍为
  不透明 `dg_engine*`；顶部契约改写为——只在 create→destroy 间有效，NULL 返 `DG_ERR_NULL_HANDLE`，
  销毁后/伪造句柄是未定义行为且库不检测；独立句柄无共享状态；`dg_render`/`dg_reset` 同句柄必须串行
  （单音频线程）；setter 原子、可并发、下一 block 生效；`dg_destroy` 需与所有调用外部同步。
  换来的是渲染路径零锁、零引用计数、零系统调用。
- `dsp-core/src/dg_engine.cpp`：删除注册表与 mutex，只保留 `new`/`delete` 与参数校验，无任何全局状态。
- `app/src/main/cpp/jni_bridge.cpp`：句柄进出 JNI 走 `intptr_t`（`jlong` ↔ 指针，32 位 ABI 高位为 0）；
  9 个符号名与返回约定不变。
- `dsp-core/tests/test_dg_lifecycle.cpp`：整体重写为 17 组，只断言契约内行为（不再测二次销毁 / stale / 伪造句柄，
  这些已属未定义行为）；新增“setter 与 render 并发”冒烟与“4 线程各自 create/render/destroy、首块与参考引擎逐字节相同”。
- DG-011 新增：`dsp-core/include/dreamguard/pcm.h`、`dsp-core/src/pcm.cpp`、`dsp-core/tests/test_pcm.cpp`
  （17 组，32 位目标多一组溢出拒绝）、`dsp-core/tests/CMakeLists.txt` 注册 `dg_pcm`、
  `dg_header_c_compat.c` 增加一次 PCM 旁路调用。

**测试设备**：无。本轮 `pwsh` 在启动阶段即失败：
`Error: SetNamedSecurityInfoW failed (Win32 5): grantWrite(E:\DreamGuard)`，命令体从未执行。

**测试结果**

| 项目 | 结果 |
| --- | --- |
| Gradle 构建 / JVM 单测（`clean testDebugUnitTest assembleDebug`） | **未执行**（本轮执行次数 0） |
| `dg_lifecycle_tests` / `dg_pcm_tests` / `dg_header_c_compat` | **未执行** |
| APK 安装 / 模拟器 / 真机 | **未执行** |
| 泄漏检测 | **未执行**（第 2 轮同样未做） |

**状态**：DG-010、DG-011 均为 `IN_PROGRESS`。第 2 轮“1524 checks 通过”的记录对应已删除的注册表实现，
测试也已整体重写，不再适用。

**未解决问题**

- 本轮所有改动都没经过编译器：可能存在编译错误，或断言与实际行为不符，必须先跑一次构建与测试。
- 「无资源泄漏」至今未验证。
- PCM 助手尚未接入播放链路（Android 仍直接把生成器输出写 AudioTrack），也不是采集实现；是否接入需单独设计，
  不建议在没有音频线程预算的情况下直接塞进 render 路径。
- 第 2 轮文档里“二次 destroy 返回错误码”“stale 句柄被安全拒绝”的表述已作废，`docs/STATUS.md` 已同步更正。

**下一步任务**

1. 跑 `.\gradlew.bat --no-daemon clean testDebugUnitTest assembleDebug`，确认 JNI 桥与 Kotlin 测试仍通过。
2. 用 NDK 交叉编译 + 模拟器（或宿主 CTest）跑 `dg_lifecycle_tests`、`dg_pcm_tests`、`dg_header_c_compat`。
3. 通过后把 DG-010 / DG-011 从 `IN_PROGRESS` 升为 `DONE`；泄漏检测补齐之前不得写 `VERIFIED`。

---

## 2026-11-10 — DG-003 模块拆分 / DG-010 生命周期接口（第 2 轮：构建与测试已执行）

> 历史记录：本轮的 DG-010 注册表实现已被第 3 轮替换，“7 个错误码 / stale 句柄检测”等描述不再适用。

**Commit**：未提交。改动留在工作区（用户明确要求本次不提交、不推送）。

**修改模块**（与第 1 轮相同，未再改动源码）

- 新增 `dsp-core/`（Gradle 模块 `:dsp-core`，纯 CMake/C++20，无 Android 依赖）
  - `include/dreamguard/dsp_core.h`：C99 公共 API（不透明句柄、7 个错误码、参数边界、所有权与错误行为）
  - `src/dg_engine.cpp`：C API 实现 + 活句柄注册表
  - `src/soundscape.{h,cpp}`：由 `app/src/main/cpp/dsp_core.{h,cpp}` 原样迁移的声景发生器（音频行为未改）
  - `tests/test_dg_lifecycle.cpp`、`tests/test_header_c_compat.c`、`tests/CMakeLists.txt`
  - `CMakeLists.txt`：静态库 `dreamguard_dsp_core`，顶层默认开测试并 `enable_testing()`
  - `build.gradle.kts`：`dspCoreConfigure` / `dspCoreBuild` / `dspCoreTest`
  - `README.md`：模块说明、API 摘要、测试入口、能力边界（不是 PCM 处理器）
- `settings.gradle.kts`：新增 `include(":dsp-core")`（此前只有 `:app`）
- `app/src/main/cpp/CMakeLists.txt`：改为 `add_subdirectory(<repo>/dsp-core)` + 只编译 `jni_bridge.cpp`，
  仍输出 `dreamguard_dsp`，仍链接 `android log`
- `app/src/main/cpp/jni_bridge.cpp`：改写为只调用 C API；9 个导出符号与库名不变，
  `nativeCreate` 失败返回 0、`nativeRender` 容量不足返回 -1、其余约定不变
- `app/src/main/cpp/dsp_core.{h,cpp}`：内容已迁移到 `dsp-core/src/soundscape.{h,cpp}`，
  两个旧文件已从工作区删除（本轮执行，`assembleDebug` 复验通过，说明已无任何 target 引用它们）
- `docs/STATUS.md`、`docs/DEVLOG.md`：新增并随本轮验证结果更新；`README.md`、`docs/ROADMAP.md` §12 同步

**完成任务编号**：DG-003（多模块结构 + JNI/CMake + 测试目录，验收端到端通过但未标 VERIFIED）、
DG-010（实现 + 生命周期测试通过；无泄漏未检测）

**测试设备**

- 构建机：Windows，JDK 17.0.21.0.2，Android SDK `E:\AgentImeTest\sdk`，NDK 27.0.12077973，CMake 3.22.1
- 运行设备：Android 模拟器 `emulator-5554`（AVD `ime35`，API 35，x86_64）
- **未使用真机**（本次无 USB 设备）；无泄漏检测工具

**测试结果（全部为本次实际执行）**

| 命令 | 结果 |
| --- | --- |
| `.\gradlew.bat --no-daemon --stacktrace clean testDebugUnitTest assembleDebug` | **BUILD SUCCESSFUL in 32s**（51 tasks，28 executed）；三个 ABI 的 `libdreamguard_dsp.so` 于 13:14:23–26 重新产出；`app-debug.apk` 16 479 469 字节 / 13:14:27 |
| `.\gradlew.bat --no-daemon :app:testDebugUnitTest --rerun-tasks` | **BUILD SUCCESSFUL in 36s**（22 tasks executed）；7 个测试类 **58 tests / 0 failures / 0 skipped**（含 `JniContractTest`） |
| 删除旧 `app/src/main/cpp/dsp_core.{h,cpp}` 后 `.\gradlew.bat --no-daemon clean testDebugUnitTest assembleDebug` | **BUILD SUCCESSFUL in 16s**（51 tasks，29 executed）；APK 16 479 469 字节 / 13:19:47；58 tests / 0 failures |
| `.\gradlew.bat :dsp-core:tasks --all` | 三个任务 `dspCoreConfigure/dspCoreBuild/dspCoreTest` 正常列出 → 模块脚本可配置 |
| `.\gradlew.bat :dsp-core:dspCoreConfigure -PdspCmake=<sdk cmake>` | **失败（环境）**：CMake 已启动，但本机无 Visual Studio/nmake → `CMake Error: CMAKE_CXX_COMPILER not set, after EnableLanguage` |
| NDK 交叉编译 native 测试 + 模拟器执行 | `dg_lifecycle_tests`：16 组用例，**1524 checks, 0 failure(s), PASSED**；`dg_header_c_compat`：**PASSED** |
| 端到端冒烟（APK 安装 + 启动 + 播放） | `adb install` Success；`nativeloader: Load .../libdreamguard_dsp.so ... ok`；Media3 会话 `state=PLAYING(3)`、`requestAudioFocus()` 获得 `USAGE_MEDIA`、`PlaybackService` 前台服务启动；播放 10s+ 位置 0→20→30ms 推进，无崩溃 |

**未解决问题**

- DG-010 验收第二条「无资源泄漏」**未验证**：仓库无 ASan/LSan 作业，本轮也未跑泄漏检测；测试文件已声明不是泄漏检测。
- DG-010 的宿主 `:dsp-core:dspCoreTest`（cmake + ctest）因本机缺 MSVC 未跑通；现有通过证据来自
  NDK 交叉编译产物在 x86_64 模拟器上的执行，不是 Windows 宿主的 ctest 输出。
- DG-003 的「Kotlin 能成功调用 Native 函数」只有端到端证据（库加载 + 播放链路持续运行），
  没有 instrumented 测试直接断言 JNI 返回值；真机回归未做。
- `app/src/main/cpp/dsp_core.{h,cpp}` 已删除；若提交时 Git 仍跟踪这两个路径，删除会自然进入本次改动。
- 未提交：工作区改动等待用户审阅后自行提交。

**下一步任务**

1. 在有 Visual Studio（或 clang）的机器上跑 `.\gradlew.bat :dsp-core:dspCoreTest`，补齐宿主 ctest 输出。
2. 加一次 ASan/LSan 构建（命令见 `docs/STATUS.md`），单独记录泄漏检测结果；通过后才可把 DG-010 记为 `VERIFIED`。
3. 真机（iQOO Z9T / V2352A）做一次播放回归，补 DG-003 的真机证据。
4. 提交时确认 `app/src/main/cpp/dsp_core.h`、`dsp_core.cpp` 的删除被一并纳入（文件已从工作区移除）。
5. 之后才进入 DG-011（PCM 基础处理）——本次未实现、未声明。

---

## 2026-11-10 — 第 1 轮：结构落地（命令被环境阻塞，无构建/测试）

**记录性质**：历史记录。当轮 `pwsh` / `terminal_open` 启动即失败：

```
Error: SetNamedSecurityInfoW failed (Win32 5): grantWrite(E:\DreamGuard)
```

命令体未执行，提权修复被以超出任务范围为由拒绝。当轮只完成文件写入与静态核对：
`dsp-core` 模块、C API、native 测试、`include(":dsp-core")`、JNI 改写为 C API 桥、docs/README 更新；
没有 `clean`、`assembleDebug`、`testDebugUnitTest`、`ctest`，未更新 APK。
该阻塞已在第 2 轮（会话文件策略切换为 `danger-full-access`）解除，上述结果即为解除后的实测。
