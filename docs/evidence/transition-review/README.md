# A1/A2 复核证据

基线为 `acc1d0b` 加 Antigravity 的未提交修改，复核补丁与本证据同批提交。结论及限制见 [审查报告](../../AUDIT_REPORT.md)。

## 验证结果

| 项目 | 结果 | 记录 |
| --- | --- | --- |
| JVM 全量测试 | 152 tests / 0 failures / 0 skipped | [summary](jvm-summary.txt) |
| 新增缺陷反例 | 修前短冷启动边界 8013 LSB；修后 24 种组合通过 ≤500 LSB 检查 | [reproduction](reproduction.txt) |
| 真实 Kotlin/JNI/native | 100 cases PASS，短冷启动边界最大 334 LSB | [raw instrumentation output](native-transition.txt) |
| Debug/Release | 三 ABI 构建通过；APK SHA256 已记录 | [build](build-summary.txt) |
| lifecycle | 1328 checks / 0 failures | [log](dg_lifecycle_tests.txt) |
| 纯 C 头兼容 | PASSED | [log](dg_header_c_compat.txt) |
| PCM | 139 / 0 | [log](dg_pcm_tests.txt) |
| meter | 207 / 0 | [log](dg_meter_tests.txt) |
| LUFS | 238 / 0 | [log](dg_lufs_tests.txt) |
| compressor | 348 / 0 | [log](dg_compressor_tests.txt) |
| limiter（含导出检查） | 3706 / 0 | [log](dg_limiter_tests.txt) |
| true-peak analyzer | 215 / 0 | [log](dg_true_peak_tests.txt) |
| PCM16 | 486 / 0 | [log](dg_pcm16_processor_tests.txt) |
| 核心 benchmark | 300 行，过程段 C++ new/new[] 调用数均为 0 | [CSV](bench.csv), [metadata](bench-meta.txt) |

## 复现

```powershell
.\gradlew.bat --no-daemon --offline :app:testDebugUnitTest :app:assembleDebug :app:assembleRelease
adb -s emulator-5554 install -r app/build/outputs/apk/debug/app-debug.apk
adb -s emulator-5554 shell am instrument -w -r com.dreamguard.app/.debug.AudioTransitionCheck
powershell.exe -NoProfile -ExecutionPolicy Bypass -File dsp-core/tools/bench/run_android.ps1 -Sdk E:\AgentImeTest\sdk -OutputDirectory E:\DreamGuard\app\build\native-review
```

需要已有对应 SDK/NDK、Gradle 缓存以及 API 35 x86_64 模拟器。成功 instrumentation 结果包含 PASS 文本及 `INSTRUMENTATION_CODE: -1`；不能仅凭 adb 进程退出码判断测试成功。该入口只在 Debug，运行结束会停止目标应用进程。它只检查内存中的 PCM，不播放或录音。

`AudioTransitionCheck` 覆盖 8/44.1/48/96/192 kHz、mono/stereo：全开链路与直接 native 输出逐字节比较，1/16/63/64/65/128 帧周期切换的帧数与分块一致性，直流信号无新增静音洞，1/16/63 帧冷启动到直通的连续性和稳态原字节检查。

数字模型指标见 [model-metrics.txt](model-metrics.txt)。模型与真实 native 使用不同信号和判据；8013 与 334 LSB 不构成同条件的前后对比。单次冷启动的 64 帧延迟仍保留；已输出直通内容的后续往返切换不增加累计帧数。

## 证据口径

[environment.txt](environment.txt) 记录原生构建环境，[source-sha256.csv](source-sha256.csv) 和 [android-source-sha256.csv](android-source-sha256.csv) 记录测试时源文件字节指纹，Windows 换行也参与哈希。测试时父提交加未提交修改的状态是预期情况，实际源码按指纹识别。

核心 benchmark 不包含 Kotlin 协调层，本轮与模拟器其他检查有并行负载，只作为可运行与核心分配回归，不用来宣布性能提升。CPU 计时存在分辨率限制。真实设备的截止时间、听感和 JVM 分配均未由这些数值证明。

过渡混合了干声，不能将完全湿声的 −1 dBFS 上界推广到过渡或原始直通。原生算法和已有独立 true-peak 报告未修改。本次未运行扬声器、耳机、蓝牙听感或长测，也未实施 Part 4 的自动响度匹配。
