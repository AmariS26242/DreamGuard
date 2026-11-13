# DreamGuard A1/A2 复核报告

复核基线：`acc1d0b` 加 Antigravity 尚未提交的 A1/A2 改动。本文按实际源码和复测结果整理，替代原审计草稿中不准确的实现描述。

## 结论

A1 的 JNI 保留规则、契约测试、CI 原生测试入口，以及 A2 的干湿声对齐和切换包络可以保留。复核发现一处短冷启动切换缺陷，已修复并补充回归。新增 Debug 专用真实 JNI 检查，避免把模拟后端测试等同于完整 native 链路验证。

本次没有实施 DG-040，也没有更改 compressor、limiter 或 PCM16 的原生算法。Part 4 响度匹配仍按原计划登记 TODO。真机听感继续待测。

## 1. 已发现并处理的问题

| 级别 | 发现 | 处理与证据 |
| --- | --- | --- |
| P1 | 从冷启动即开启处理，并在少于 64 帧时关闭：真实尾部不足以走完淡出，接下来的直通段沿用默认或过期增益，边界可能跳变。 | 添加固定增益 0.1 的延迟后端反例；修前在 8 kHz、mono、仅处理 1 帧时失败，边界差为 8013 LSB。现在根据实际 drain 样本更新后续淡出的起始增益。24 种采样率/声道/短流组合均通过 ≤500 LSB 检查。见 [失败记录](evidence/transition-review/reproduction.txt) 和 [回归结果](evidence/transition-review/jvm-summary.txt)。 |
| P2 | 模拟后端的 drain 清空压缩器包络，实际 PCM16 drain 只清空 limiter。原快速切换测试因此不能反映历史压缩量保留。 | 修正模型的 drain 行为并重跑全部 JVM 测试；模型仍只是近似，不承担 native true-peak 验证。 |
| P2 | 原报告将生产回退描述为“纯 Kotlin 压缩器”，并声称项目没有通知权限申请；二者与源码不符。 | 实际回退是逐字节直通；Kotlin DSP 模型仅存在于测试。`SleepTimerCard.kt` 已有 POST_NOTIFICATIONS 请求。本文纠正这些事实，不新增权限逻辑。 |
| P2 | “所有情况输入输出严格 1:1”“包络斜率连续”“回调绝不分配”等陈述超出实现和测试证据。 | 明确一次冷启动延迟、权重折返的保证范围，以及 BaseAudioProcessor 输出缓冲可能扩容的事实。详见下文。 |

复核后未发现仍需阻止本批代码提交的问题。此结论限于当前代码、自动验证和模拟器信号检查，不表示物理听感已通过。

## 2. A1 改动核查

- [proguard-rules.pro](../app/proguard-rules.pro) 增加 NativePcmProcessor 的 native 名称保留规则，两个 JNI 契约测试覆盖声景和 PCM 两组。Release 当前未开启混淆；这些测试证明规则存在，没有冒充实际混淆构建验证。
- [android-ci.yml](../.github/workflows/android-ci.yml) 增加 `:dsp-core:dspCoreTest`，让 Linux CI 执行宿主 CTest。当前提交的远端运行结果以 GitHub Actions 为准，本地 Android 原生结果另列。
- [run_android.ps1](../dsp-core/tools/bench/run_android.ps1) 显式处理 Windows PowerShell 的 native stderr 和退出码。复核以完整必填参数实际执行，九组原生测试和 300 行核心基准均成功完成，没有在参数提示处等待。

## 3. A2 实际信号与时序

链路为 `LocalAudioProcessor → PcmCompressor/JniPcmCompressor → NativePcmProcessor → JNI → PCM16 → compressor → limiter`。JNI 的 create/process/drain 等接口保持不变。

协调层缓存原始干声，使其与 64 帧延迟的湿声对应，再用 64 帧升余弦权重混合。48 kHz 下窗口为 1.333 ms，8 kHz 下为 8 ms。重复切换从当前权重继续向另一方向移动；权重值不会重置，折返处不保证一阶导数连续。

帧数边界：

- 流开始已处于处理模式时，保留一次 64 帧启动延迟，正常 EOS/drain 后总输出为输入 +64 帧。
- 流已经输出直通内容后，中途开启处理会剥离未填满的启动零帧，结束时补回真实尾部；往返切换不再每次增加 64 帧。对应测试中的总帧数为 1:1。
- 稳态直通逐字节一致；切换到直通的过渡段会被淡化，不是逐字节旁路。异常时仍复制当前输入，可能丢弃尚未成功取出的至多 64 帧尾部。
- 64 帧过渡混合了原始干声，可能高于 native 的 −1 dBFS ceiling；只有完全湿声部分具有原生峰值上界。淡化不发生整数回绕，不等于整个过渡已取得 true-peak 保证。

切换状态仅由播放线程访问，UI 写入开关后按块读取。新增环形缓存、scratch 和查找表预分配；BaseAudioProcessor 仍可能在首次使用或更大块到来时分配输出缓冲。C++ 基准的零分配计数不覆盖 Kotlin 协调层或 JVM。

## 4. 独立复测

原始结果见 [证据目录](evidence/transition-review/README.md)。

| 项目 | 结果 | 证明范围 |
| --- | --- | --- |
| 全部 JVM 单测 | 152 tests / 0 failures / 0 skipped；LocalAudioProcessor 31 项 | 生命周期、回退、JNI 名称与规则、模拟信号的边界和切换行为 |
| 九组 C/C++ 可执行测试 | 全部通过，包含 limiter 3706/0、PCM16 486/0 | 未更改的原生算法回归，不能单独证明 Kotlin 混音输出 |
| Debug 与 Release | 三 ABI 构建成功 | 编译、链接及打包；Release 未混淆 |
| Debug 真实 JNI 检查 | 100 cases PASS；短冷启动边界最大 334 LSB | 五采样率、mono/stereo；全开时与直接 native 输出逐字节一致；快速切换无新增启动零帧、分块一致及帧数守恒；冷启动保留一次延迟，稳态直通一致 |
| 核心基准 | 300 行结果，过程段 C++ new/new[] 计数为 0 | 核心处理路径；不包含 Kotlin 淡化的 CPU/分配成本 |

Debug 检查使用真实 `libdreamguard_dsp.so`，不经过 AudioTrack、扬声器、耳机或录音。入口只在 Debug manifest，Release 不包含它。执行方式：

```powershell
adb shell am instrument -w -r com.dreamguard.app/.debug.AudioTransitionCheck
```

成功输出应包含 `PASS 100 real JNI cases` 与 `INSTRUMENTATION_CODE: -1`。执行会结束模拟器上的目标应用进程；不修改用户文件或系统音量。

原先 A2 的正弦指标保留在 [JVM 指标](evidence/transition-review/model-metrics.txt)，它们来自测试模型；不作为真实录音、标准合规或不可闻的证明。不能把模型修前 8013 LSB 与真实 JNI 的 334 LSB 当作同一输入条件的前后对比。

## 5. 已纠正的原草稿事实

- 实际基准文件为 `dsp-core/tools/bench/dg_bench.cpp`，参考工具为 `dsp-core/tools/lufs_compare/dg_lufs_compare.cpp`；不存在草稿所列的两个 `main.cpp`。
- limiter 的 process 按输入帧数输出，开头包含延迟零帧，尾部通过 drain/flush 取得；没有草稿所述 `produced` 变长返回契约。
- 不支持的 PCM 格式已有 `UNSUPPORTED_FORMAT` 与 UI 说明；真实可播放格式仍需按解码协商测试，不能由源文件位深直接推断。
- 未实施草稿建议的 +9 dB 自动增益、−16/−18 LUFS 目标或移除固定补偿。它们不属于本批批准范围，并与 [Part 4 默认只衰减的计划](PART4_PLAN.md) 不一致。

## 6. 仍需后续验证

真机听感、蓝牙/外放、有线输出、Android 音频截止时间和长时间稳定性未由本批测试覆盖。短切换窗口、残留压缩历史和保守 limiter 的高频衰减仍需素材 A/B；不能声称“听感自然”或“所有音频无咔嗒”。按用户安排补真机短测，30 分钟等长测继续留到全功能完成后。
