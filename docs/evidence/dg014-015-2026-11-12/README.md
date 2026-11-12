# DG-014 / DG-015 阶段证据 — 2026-11-12

本阶段开发完成，任务记 DONE，不标 VERIFIED。当前源码、测试和报告在同一提交中。源文件指纹见 [source-sha256.csv](source-sha256.csv)，构建环境见 [environment.txt](environment.txt)。表中 source_commit 是测试时的父提交，source_dirty 表明本阶段代码尚在工作区；指纹用于确定实际测试源码。

## 实现

- DG-014：可选保守 4× 峰值保护，采用 257-tap Blackman-sinc，并对短/长重建系数取正权重上界。每个输出样本的增益受到所有相关插值窗口约束，覆盖增益变化和文件尾部。默认 sample-peak 模式保留。
- PCM16/JNI/Media3：压缩后开启 −1 dBFS guard，固定 64 帧上下文；EOS 和关闭处理前 drain，正常结束不丢内容，seek 丢弃旧流。每个处理段增加 64 帧静音延迟，48 kHz 为 1.333 ms，8 kHz 为 8 ms。量化容差低于 0.001 dB。异常回退时可能无法取出最后 64 帧，界面报告错误。
- DG-015：300 行完整矩阵与可复现脚本，记录分布时间、CPU 时间、延迟和内存。C++ 分配审计只覆盖当前线程的 new/new[]，不覆盖 libc、JNI/JVM、aligned new 或其他线程。

## 自动验证

| 项目 | 结果 | 原始记录 |
| --- | --- | --- |
| limiter（含 120 个 fixture 的写出检查） | 3706 checks / 0 failures | [log](dg_limiter_tests.txt) |
| PCM16 compressor + guard + drain | 486 / 0 | [log](dg_pcm16_processor_tests.txt) |
| true-peak analyzer | 215 / 0 | [log](dg_true_peak_tests.txt) |
| compressor | 348 / 0 | [log](dg_compressor_tests.txt) |
| lifecycle | 1328 / 0 | [log](dg_lifecycle_tests.txt) |
| PCM | 139 / 0 | [log](dg_pcm_tests.txt) |
| meter | 207 / 0 | [log](dg_meter_tests.txt) |
| LUFS 既有回归 | 238 / 0 | [log](dg_lufs_tests.txt) |
| 纯 C 头兼容（含新 drain 调用） | PASSED | [log](dg_header_c_compat.txt) |
| JVM | 142 tests / 0 failures / 0 skipped | [summary](android-build.txt) |
| Android Debug/Release | 三 ABI 构建通过 | [summary](android-build.txt) |
| FFmpeg 独立 16× 重建 | 120 PASS / 0 FAIL | [CSV](reference.csv), [settings](reference.txt) |

峰值用例包含 1 帧文件、末帧脉冲、fs/4 相位偏置正弦、Nyquist 附近突发、DC 反转、随机噪声、−60 dBFS ceiling、5 种采样率、单/双声道和链接/独立模式。分块一致性和完整滤波尾部均参与检查。独立参考为 FFmpeg libswresample，16×、filter_size=128、cutoff=0.99，预先补 256 帧零以处理极短文件，float64 输出；验收容差固定 +0.05 dB。全部输出 fixture 的 SHA256 与参考 CSV 已核对。

短窗口 guard 曾在独立对照中失败：101 PASS / 19 FAIL，最大过冲 1.798311 dB。失败结果保存在 [原始 CSV](reference-short-filter-failed.csv) 和 [配置](reference-short-filter-failed.txt)。修正扩大上下文并覆盖短/长滤波权重，验收容差保持不变。有限矩阵不构成模拟波形或广播标准认证。

## 性能

[原始 300 行 CSV](bench.csv)，[计时配置](bench-meta.txt)。NDK Release、x86_64、API 35 模拟器、普通优先级；32 块预热，200 次计时。5 采样率 × 6 块长 × 2 布局 × 5 路径。每种布局同时保留所有 DSP 句柄和输入/输出缓冲，RSS/HWM 是整个进程值，不是单个组件占用。CPU 字段含计时循环开销，不代表手机电量。

48 kHz、512 帧、双声道这一组的测量：

| 路径 | median µs | p95 µs | xRT（越高越快） | 延迟帧 |
| --- | --- | --- | --- | --- |
| compressor | 11.851 | 12.073 | 900.065 | 0 |
| sample_limiter | 4.838 | 5.125 | 2204.768 | 96 |
| true_peak_guard | 130.007 | 410.408 | 82.047 | 96 |
| true_peak_meter | 13.402 | 41.783 | 795.871 | 8 |
| pcm16_chain | 165.226 | 513.982 | 64.558 | 64 |

300 项最小 median xRT 为 **6.353**；观测到的进程 RSS 最大 **4476 KiB**；过程段 C++ 分配调用数均为 **0**。原始 CSV 保留了 max 及调度抖动，不能据此保证实时截止时间。meter 的 8 帧是测量延迟。模拟器结果用于回归，不外推为 iQOO 或其他 arm64 设备性能。

## 应用短测及边界

[模拟器短测记录](app-smoke.txt) 包含 APK SHA256、系统媒体会话和 UI 状态。8 秒 / 44.1 kHz / mono PCM16 从系统文件选择器打开，实际处理进入 ACTIVE，播放到 EOS；系统位置 8001 ms 与 64 帧约 1.45 ms 的新增延迟相符。重播及处理关闭/恢复仍能播放，AndroidRuntime 错误记录为空。尾部样本完整性由原生/JVM 测试验证。

保守上界会对高频或噪声多衰减，未做透明度承诺。iQOO Z9T 未连接；真机听感、蓝牙、arm64 性能、ASan/LSan 和长时间稳定性未验证。用户要求真机留到晚上、所有功能完成后再统一长测。第三方音频捕获与替换能力不由本阶段结果推断。

## 复现

见 [复现步骤](../../../dsp-core/tools/bench/README.md)。源码不依赖私有凭据，参考工具仅需 Python 标准库和已有 FFmpeg。输出文件和设备临时文件保留，脚本不删除应用或用户数据。
