# P4-01：响度观测与数据通路 — 审查后证据

日期：2026-11-14。基线为 87c3f4a 加交接工作区改动；已完成独立审查、修正和复测。P4-01 记 DONE，不标 VERIFIED；P4-02～P4-05 仍为 TODO。DG-012 / DG-020 保持 DONE，DG-022 / DG-023 保持 IN_PROGRESS。

具体问题与修正见 [审查报告](REVIEW.md)，执行范围见 [Part 4 计划](../../../PART4_PLAN.md)。本批代码未实施自动响度或预设。

## 实现与保证范围

- dg_lufs_window_snapshot 只读最近 4 / 30 hop，不扫描 integrated 历史。reset 通过逻辑范围清空分析，旧存储块覆盖前不可被读到。
- 输入点在压缩及 −6.0206 dB 补偿后、限幅前；输出点在限幅后的湿声支路，包含 EOS drain 尾部。旁路不累计窗口，恢复处理重新积累；不清除压缩器音频历史。
- nativeSnapshot 使用固定 16 字段数组。Kotlin 验证传输内容、按音频帧约 4 Hz 加边界发布不可变副本；读取异常显示暂不可用，不回退或中断正常音频。UI 不直接访问 native 句柄，关闭开关后即使旧快照返回也只显示直通。
- 界面区分未就绪、静音、低电平、直通与观测不可用，并明确支路读数不等于过渡混音或实际扬声器输出。

## 自动与界面验证

| 项目 | 结果 | 记录 |
| --- | --- | --- |
| JVM（13 类） | 159 tests / 0 failures / 0 skipped；LocalAudioProcessor 38 项 | [summary](jvm-summary.txt) |
| 新遥测异常反例 | 修前 2 项失败，修后通过 | [reproduction](reproduction.txt) |
| 开关与快照并发反例 | 固定时序修前失败，修后通过 | [reproduction](toggle-reproduction.txt) |
| 原生尾部与窗口断点反例 | 修前 4 个断言失败，修后通过 | [reproduction](native-reproduction.txt) |
| LUFS | 302 checks / 0 failures | [log](dg_lufs_tests.txt) |
| PCM16 | 1026 checks / 0 failures | [log](dg_pcm16_processor_tests.txt) |
| 纯 C 头兼容 | PASSED | [log](dg_header_c_compat.txt) |
| limiter | 3706 / 0 | [log](dg_limiter_tests.txt) |
| compressor | 348 / 0 | [log](dg_compressor_tests.txt) |
| lifecycle | 1328 / 0 | [log](dg_lifecycle_tests.txt) |
| PCM | 139 / 0 | [log](dg_pcm_tests.txt) |
| meter | 207 / 0 | [log](dg_meter_tests.txt) |
| true-peak analyzer | 215 / 0 | [log](dg_true_peak_tests.txt) |
| Debug/Release 三 ABI | BUILD SUCCESSFUL | [build and hashes](build-summary.txt) |
| 真实 Kotlin/JNI/native | 100 组切换 + LUFS 观测、旁路恢复、EOS 尾部就绪通过 | [raw result](native-transition.txt) |
| Compose 界面 | 播放时显示有效 LUFS；暂停后关闭处理显示直通；观测区域及开关无重叠 | [screenshot](ui-active.png), [active](ui-active.xml), [paused/bypass](ui-paused-bypass.xml) |

基于第 18 轮已有参考报告，LUFS 与 FFmpeg 9.0.2 metadata 的 14 项可比合成信号最大误差为 0.010 LU，2 项无定义单列 NO-DATA；本轮不重新声称标准认证。原有 K 权重滤波尾部可能使近静音显示极低有限值；reset 后纯静音仍为负无穷。

## 性能

保留三个条件下的原始结果：[原交接](bench-incoming.csv)、[重启前复测](bench-review-before-restart.csv)、[最终串行复测](bench.csv)。原交接有 9 / 10 组、重启前复测有 20 / 19 组 PCM16 / true_peak_guard 的 p95 超预算；没有按通过改判。

最终在冷启动的同一只读 AVD 上，构建完成、应用停止后单独运行核心基准；360 行的 p95 均低于各自块时长，C++ 分配计数均为 0，最低 median xRT 为 8.344。查询链路最大 median / p95 分别为 17.6 / 30.3 µs。该统计结果不能保证每个回调不超时，也不包括 JNI、Kotlin 发布或 UI 成本。

48 kHz / 512 帧 / stereo，块时长 10666.7 µs：

| 链路 | median µs | p95 µs | xRT | 延迟帧 |
| --- | --- | --- | --- | --- |
| compressor | 19.100 | 19.900 | 558.464 | 0 |
| sample_limiter | 11.200 | 12.600 | 952.381 | 96 |
| true_peak_guard | 297.600 | 515.100 | 35.842 | 96 |
| true_peak_meter | 20.500 | 55.100 | 520.325 | 8 |
| pcm16_chain | 153.600 | 477.000 | 69.444 | 64 |
| pcm16_snapshot | 6.700 | 7.000 | 1592.040 | 0 |

测量差异对条件敏感，不能只凭重启复测就唯一归因于宿主调度或排除新增观测开销。[计时配置](bench-meta.txt)、[原测试环境](environment.txt)、[构建及二进制指纹](build-summary.txt) 均已保存。第一次 UI 启动超时及重启复查经过见 REVIEW.md。

## 复现

先使用项目已有 SDK/NDK 和 Gradle 缓存构建、测试：

- gradlew.bat --no-daemon --offline :app:testDebugUnitTest :app:assembleDebug :app:assembleRelease
- powershell.exe -NoProfile -ExecutionPolicy Bypass -File dsp-core/tools/bench/run_android.ps1 -Sdk E:/AgentImeTest/sdk -OutputDirectory E:/DreamGuard/app/build/p401-check
- adb -s emulator-5554 install -r app/build/outputs/apk/debug/app-debug.apk
- adb -s emulator-5554 shell am instrument -w -r com.dreamguard.app/.debug.AudioTransitionCheck

验证 instrumentation 时同时检查 PASS 内容及 INSTRUMENTATION_CODE: -1，不能只凭 adb 进程退出码。核心 benchmark 要串行运行并单独记录当时负载和环境。

源码字节指纹见 [native source](source-sha256.csv) 和 [Android/review source](android-source-sha256.csv)，Windows 换行也参与哈希。测试时父提交加工作区改动的状态是预期情况，所有代码及本记录同批提交。

## 尚未验证

物理真机听感、arm64 实测性能、ASan/LSan、蓝牙/外放和长时间稳定性待补。真机按用户安排短测，长测统一留到全功能完成后。loudness_gain_db 始终为 0，未进入 P4-02。
