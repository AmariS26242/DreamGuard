# DreamGuard 任务状态台账

更新日期：2026-11-10（**第二十六轮：主代理执行 DG-024 构建与 JVM 单测 → BUILD SUCCESSFUL、92 tests / 0 failures / 0 skipped；API 35 x86_64 模拟器安装启动，未启动 DreamGuard 声景时页面待机、系统媒体音量 33%（5/15），用音量卡滑块调到 20%（3/15），dumpsys audio 记录 setStreamVolume STREAM_MUSIC index 3 oldIndex 5 from com.dreamguard.app，页面显示 20%**。DG-024 仍 `IN_PROGRESS`：无第三方播放器共存证据、外部音量同步未可靠验证（模拟器 adb 调节未生效）、无固定输出/蓝牙/真机；不声称原生音频实时压缩或单应用音量。**本轮仅更新文档，未提交/推送**。此前第二十五轮：代码与 12 个 JVM 测试写入，当时本会话命令环境不可用、未构建/未运行。此前第二十四轮记录：API 35 x86_64 模拟器安装当前 Debug APK，验证播放状态与“本地压缩就绪”。主代理在代码提交 `4b7ace322da31f06417f3dc09a591795ba14207f`（短 `4b7ace3`）推送后，新做模拟器安装/启动，系统文件选择器选中合成的 **40 s / 44.1 kHz / 单声道 PCM16 WAV**；UI 显示播放中与“本地压缩就绪”，打开处理开关后 `checked=true`；MediaSession `PLAYING`，进程存活，`AndroidRuntime:E` 日志未见 `FATAL EXCEPTION`/`UnsatisfiedLinkError`。**边界**：只证明应用播放状态与 native 处理器准备就绪（`NativeState.READY`，库加载/句柄创建未报错），**不证明**逐块调用 `nativeProcess`、实际音频输出电平、听感、延迟/underrun、泄漏或真机通过；真机新版压缩与 30 分钟长稳未测（用户要求延后）。DG-021/022/023 仍 `IN_PROGRESS`，DG-020/013 仍 `DONE` 不标 `VERIFIED`。**第二十三轮代码已随 `4b7ace3` 提交并推送；第二十四轮日志/状态更新未提交**。**第二十三轮：DG-022 契约修正、Debug 构建与原生测试实测**。对照 media3 1.5.1 官方源码逐项复核 `BaseAudioProcessor`：其 `onFlush()` 返回 **void**，第二十二轮写成 `override fun onFlush(): Boolean` 会**编译失败**，已改为 Unit；`flush()` 由基类先清空待输出并把 pending format 写入 `inputAudioFormat`，因此新增的 `LocalAudioProcessorTest` 中“flush 后 output 为空”不是靠返回值。句柄生命周期改为：`onConfigure()` 只记录 pending 格式、**不碰**在用的 native 句柄；`onFlush()` 才在格式真正生效时创建/替换句柄，同格式 flush 只 reset 压缩机 —— 这样 media3 明确允许的“configure 之后、flush 之前仍可排队旧格式 buffer”窗口内，旧 buffer 仍使用旧句柄（新增测试断言该窗口）。`nativeCapable` 纳入 `nativeError` 判定，新增 `layoutSupported`/`nativeLibraryAvailable`/`nativeFailure`；格式提示在开关关闭时也会明示多声道/缺库/失败直通；已开启的开关始终可关闭（避免失败后 UI 把用户锁在“开”状态）；回退时同时回卷 input position；`close()` 终止并关闭处理。C++ 测试 24→**27 组**（bypass 不改变压缩器状态、立体声残帧、输出容量大于输入），C 头兼容已含 PCM16 纯 C 段（本轮未单独编译/报告）；JVM 测试 17→**18 个**。主代理交叉编译又发现 `dg_compressor_process` 输出参数类型不一致（`dg_pcm_view` vs `dg_pcm_buffer`），已把 compressor 输出改为 `dg_pcm_buffer` 并同步测试、删除残留 `const_cast`。**主代理实测（第二十三轮）**：`.\gradlew.bat --no-daemon --offline :app:testDebugUnitTest :app:assembleDebug` 成功，**80 JVM tests / 0 failures / 0 skipped**，Debug NDK 三个 ABI 编译通过；Android API 35 x86_64 模拟器交叉编译并运行 `dg_pcm16_processor_tests` **206 checks / 0 failures**、`dg_compressor_tests` **348 checks / 0 failures**；首轮 native 类型错误已修。**边界**：本轮**未运行 APK 的模拟器/真机播放**，JVM JNI 合约与 mock **不等于**实测 JNI；未做 true peak、长时间听感、多设备、真实音频输出；用户明确长时间真机测试暂缓。**第二十三轮代码已随 `4b7ace3` 提交并推送；第二十四轮日志/状态更新未提交**。DG-022/023 仍 `IN_PROGRESS`，DG-020 仍 `DONE` 不标 `VERIFIED`。**第二十二轮：DG-022 功能构建——本地 PCM16 经 JNI 进入 native C++ 压缩器**。新增 `dsp-core/include/dreamguard/pcm16_processor.h` + `src/pcm16_processor.cpp`（PCM16→float→DG-013 compressor→PCM16；512 帧定长 scratch、处理路径无逐样本分配、bit-exact bypass、残帧/奇数字节原样、饱和量化、无 lookahead）；新增 JNI surface `NativePcmProcessor`（5 符号，独立契约测试）；`LocalAudioProcessor` 改为 native 主路径，开关关闭或 native 库缺失/句柄失败/处理失败/多声道/非 direct buffer 时逐字节回退，`nativeState` 与 UI 文案可见；`PlaybackService.onDestroy` 在 `player.release()` 后 `close()` 释放句柄。测试：新增 C++ `dg_pcm16_processor`（24 组）与 JVM `LocalAudioProcessorTest`（17 个，fake 后端）+ `NativePcmProcessorContractTest`（4 个）。**边界**：本会话命令环境仍不可用（任何命令返回 `SetNamedSecurityInfoW failed (Win32 5): grantWrite(E:\DreamGuard)`），以上代码与测试在该轮未执行；第二十三轮主代理已补构建/测试（结果见开头），但仍**未运行设备播放**、未验证真实 JNI 与听感；不声称 native 真处理已在设备上通过、不声称真机已验证新版本、不声称 limiter/true peak；DG-020/022 仍不标 `VERIFIED`，DG-022 仍 `IN_PROGRESS`。**第二十一轮：iQOO Z9T 真机本地音频验证**——主代理在 iQOO Z9T（ADB model `V2352A`、manufacturer `vivo`、Android 16 / API 36）安装当前 Debug APK 成功：`versionName 0.1.0`、`versionCode 1`、SHA256 `3ABD75F122320CCE3A7BD1BD493FE6B0A867C1B8996296DD6E44621C8F4C97D2`，启动进程 pid **22780**；该 APK 与此前同批构建、**68 tests / 0 failures / 0 skipped** 的 JVM 结果对应（本轮未重新构建）。用户在 Downloads 用系统选择器打开主代理生成的 **40 s / 44100 Hz / 单声道 PCM16 WAV**，对“声音、−6 dB 开关变化、暂停恢复、爆音卡顿”的**合并询问**统一答复“**正常**”；另答复锁屏约 **15 秒**声音持续、播放其他媒体后 DreamGuard 暂停；关闭本地音轨后原有 DSP 声景正常播放，调音量/音色、暂停正常且无爆音卡顿。日志侧：进程存活、MediaSession `PAUSED`，过滤 `AndroidRuntime:E` 与 `nativeloader` 未见 `FATAL EXCEPTION` 或 `UnsatisfiedLinkError`；**未取得明确的 nativeloader 成功行**，以实际声景出声为间接证据。**边界**：不把“正常”逐项扩展成更细的听感结论；C++ PCM DSP **仍未接入**（−6 dB 是 Kotlin 整数增益，不是 C++ 压缩器/限幅器）；本次**未执行** 30 分钟长稳、蓝牙/有线/外放等多路由、多格式（仅 44.1 kHz 单声道 PCM16）、进程重建、真实峰值测量。按“交付物全部完成”口径，DG-021/022/023 仍记 `IN_PROGRESS`，不标 `DONE`/`VERIFIED`。第二十轮：**主代理真实验证**——`.\gradlew.bat --no-daemon :app:testDebugUnitTest --rerun-tasks :app:assembleDebug` 成功，**68 tests / 0 failures / 0 skipped**；Android API 35 x86_64 模拟器安装启动，系统文件选择器从 Downloads 选取合成 40 s / 44.1 kHz / 单声道 PCM16 WAV，UI 显示文件名、`16-bit PCM · 44100 Hz · 1 ch`、播放中，MediaSession `PLAYING`；打开 −6 dB 开关后 `checked=true`，暂停 UI 显示已暂停，恢复后 MediaSession `PLAYING`，无 `AndroidRuntime FATAL` 输出。**模拟器无音频输出，未听感、未真机、未实证输出波形**；当前 Kotlin 基础 −6 dB **不是** C++ 压缩器/限幅器，DSP Core 未接入。故 DG-021/022/023 仍记 `IN_PROGRESS`，只登记已实际执行的证据与剩余项。第十九轮：**本地音频最小闭环代码落地**——系统文件选择器（`ACTION_OPEN_DOCUMENT` + 持久化读授权）、复用现有 `ExoPlayer` 的本地文件播放/暂停/恢复、`LocalAudioProcessor`（`BaseAudioProcessor` 子类：16-bit PCM 逐样本 −6 dB 整数减半、bypass 逐字节 bit-exact、格式协商/flush/reset/EOS）、`LocalAudioRenderersFactory`（`DefaultAudioSink.Builder.setAudioProcessors` 注入）、最小 UI（本地音频卡 / 处理开关 / 格式提示 / 错误复用）与 10 个 JVM 测试。**本会话 pwsh 仍在启动阶段失败，构建、单测、真机、听感均未执行**，代码只经静态核对，故 DG-021/022/023 记 `IN_PROGRESS`；DG-020 沿用现有 9 符号桥、本轮无新增 JNI。第十八轮：①DG-012 高精度对照完成——主代理用 `ebur128=metadata=1` 对同一批 16 个 WAV 真实执行，**14 个可比样本 14 PASS / 0 FAIL / 0 SKIP，worst |delta| 0.010 LU**；2 例静音/0.3 s 判 **NO-DATA**（−70.000 是 `ABS_THRES` 哨兵，所有 M 块 < −70），**单列不计通过**；metadata 受 FFmpeg 0.01 LU 直方图量化约束，三位小数只是格式；DG-012 记 `DONE`、不标 `VERIFIED`；该 PASS 仅是相对 FFmpeg 9.0.2 metadata、在约 0.01 LU 分辨率下的通过，不构成标准认证。第十七轮：DG-014 峰值限制器的头文件、实现、测试与构建接线已写入；主代理实测 NDK x86_64 模拟器 **dg_limiter 2113 checks / 0 failures**，Gradle `testDebugUnitTest assembleDebug assembleRelease` 成功、三个 ABI 构建通过、JVM `up-to-date`；仅保证 sample peak，**未做过采样 true-peak 测量**，无真机/听感，代码未接入播放链路，故仍为 `IN_PROGRESS`。第十六轮：DG-012 高精度 metadata 脚本由主代理执行两次，均在 ffmpeg probe 处 exit 3（同一 ffmpeg 手工 `-version` 成功）：第一次是 `Select-Object -First 1` 提前终止管道，第二次是版本正则的 `$` 在 Windows CRLF 下不匹配；已修为完整捕获 + 真实 exit code + 无行锚点的全文版本行搜索，**修复后尚未重跑，无新报告**。第十五轮：**参考对照首次真实执行**——主代理用预生成模式对同一批 16 个 WAV 调用 FFmpeg 9.0.2 `ebur128`，得 **14 PASS / 2 FAIL / 0 SKIP，最大绝对差 0.049 LU（`stereo_10kHz_-20dB_48k`），整体 exit 1**。两项 FAIL 都是 `kind=undefined` 的定义问题（2 s 数字静音、0.3 s 短于一个 400 ms 块）：我方 `integrated_ready=0` 且 `integrated=-inf`，ffmpeg 打印 **−70.0**——按 FFmpeg 源码该值是 `ABS_THRES` 哨兵而不是测量结果（`libavfilter/f_ebur128.c`：`#define ABS_THRES -70`；`init()` 里 `ebur128->integrated_loudness = ABS_THRES;`；`filter_frame()` 只在某个 400 ms 块 `loudness_400 >= ABS_THRES` 且 `nb_integrated > 0` 时才覆盖它，静音时该块为 −120.7 LUFS、0.3 s 时 400 ms 缓存根本没填满，于是字段停在初值），因此两项**保持未通过 / 需人工裁定**，不放宽、不改判、不改核心实现。同时登记精度事实：ffmpeg 汇总行是 `I: %5.1f LUFS`，分辨率只有 **0.1 LU**，0.049 LU 与打印取整同量级，0.1 LU 容差只等于 1 个量化步。DG-012 仍 `IN_PROGRESS`、LUFS 仍 `PROVISIONAL`；DG-013 是独立的压缩器测试链（348/0、无听感/真机证据、未接播放），不受本轮影响。第十四轮：①`dg_lufs_compare --selfcheck` 修正后由主代理重跑，**115 checks / 0 failures**（比修前多 2 条，正是新增的静音段 `momentary`/`isfinite` 检查，计数变化本身就是“跑的是新二进制”的证明），DG-012 的**工具自检**首次全绿。②对照侧出现现实条件：FFmpeg **9.0.2** 已位于 `dsp-core/build/reference-deps/imageio_ffmpeg/binaries/ffmpeg-win-x86_64-v9.0.2.exe`（仅构建目录内，非系统安装）；但本机无 Windows C++ 编译器，故 `compare_with_ffmpeg.ps1` 新增**预生成输入模式**（`-Csv` + `-WavDir`，直接吃“NDK 编译的 Android 工具在模拟器上生成的 CSV + WAV，再 `adb pull` 回来”的产物，保留原生工具模式），并把静音/0.3 s 两类用例改为**定义问题**处理（`kind=undefined`：双方都无值记 `SKIP` 且**不计入通过**，我方给出数值或参考给出数值则 `FAIL`，绝不静默通过），报告新增 CSV/WAV 的 SHA256 与同名 `.csv` 副产物，且明确**不删除任何输入文件**。本会话 `pwsh` 仍在启动阶段失败，**一次对照都没跑**，`reference-report.md` 尚不存在 ⇒ DG-012 仍 `IN_PROGRESS`、LUFS 仍 `PROVISIONAL`。第十三轮：两件真实验证——①DG-013 重跑得 **348 checks / 0 failures**，扩展后的 `dg_header_c_compat` PASSED，Gradle `testDebugUnitTest assembleDebug assembleRelease` 成功、Debug/Release 三 ABI 构建通过、JVM `up-to-date`；压缩器的信号级验收达成，但**无听感、无真机、未接入播放链路**，故登记 `DONE` 而**不标 `VERIFIED`**。②DG-012 的对照工具 `dg_lufs_compare --selfcheck` 首次由主代理用 NDK x86_64 编译并在 API 35 模拟器执行，得 **113 checks / 1 failure**：`mono_levelstep_4s_1s_2s_48k: expected >= 60 blocks above the absolute gate`。根因是**工具自身的信号生成缺陷**（`fill_sine` 乘的是 case 的 −20 dBFS 电平，把 −50 dB 当相对比例传入，安静段实际落到 −70 dBFS，整段掉到 −70 LUFS 绝对门以下），本会话已改为绝对 −50 dBFS（与 `tests/test_lufs.cpp` 的 gating 用例同一构造），并新增“安静段 `momentary` 等于解析值”的检查以防同类错误复现；**修正后待重跑**，DG-012 仍 `IN_PROGRESS`、LUFS 仍 `PROVISIONAL`，与 ffmpeg/libebur128 的对照依旧零执行。第十二轮：DG-013 首次真实运行——主代理在 NDK x86_64 + API 35 模拟器上跑出 **334 checks / 2 failures**；两处失败经分析**均为测试期望错误**（`output_peak_db` 是“整块峰值，含 attack”，被错当稳态值；浮点样本无法恰好落在阈值上），已按解析值改写测试、新增“明确低于阈值 bitwise 0”与“double 精确阈值”检查，**修正后待重跑**，DG-013 仍只登记 `IN_PROGRESS`。第十一轮：主代理首次在 **NDK x86_64 真实编译** DG-013 测试即**失败**——`compressor.h:161` 报 `warning: '/*' within block comment`，声明被注释吞掉后引发多处 `use of undeclared identifier 'dg_compressor_process'`。根因是本会话第十轮合并 `dg_compressor_process` 注释时漏掉注释结尾 `*/`，已修复（补回 `*/` 并复核头文件全部注释闭合）；修正后**尚待主代理重测**，DG-013 仍只登记 `IN_PROGRESS`，压缩器仍未接入播放链路。第十轮：按用户授权推进 DG-013 最小压缩器——新增 `dsp-core/include/dreamguard/compressor.h`、`dsp-core/src/compressor.cpp`、`dsp-core/tests/test_compressor.cpp`（16 组）、CMake 接线与 C 头兼容冒烟。第十轮本会话 `pwsh` 仍在启动阶段报同一 ACL 错误，这三个文件从未在本会话编译、运行。DG-012 的对照工具链同样仍未执行，DG-012 保持 `IN_PROGRESS`、LUFS 仍 `PROVISIONAL`。第九轮：写好 DG-012 的参考工具对照工具链但**未执行**——新增 `dsp-core/tools/lufs_compare/`（`dg_lufs_compare` 16 例确定性合成信号 + `--wav-dir/--csv/--selfcheck/--libebur128`、`compare_with_ffmpeg.ps1` 驱动 ffmpeg `ebur128` 并按 0.1 LU 判定、CTest `dg_lufs_compare`），本会话 `pwsh` 仍在启动阶段报同一 ACL 错误，无法编译/运行/跑 ffmpeg，因此**零个对照数字**被登记，DG-012 保持 `IN_PROGRESS`、LUFS 仍 `PROVISIONAL`。第八轮：主代理复测修复后的 DG-012——`dg_lufs` **238 checks / 0 failures**（gating 诊断 `blocks=67 after_abs=61 used=40 integrated=-23.1696`）、`dg_meter` 207/0、扩展后的 `dg_header_c_compat` PASSED；Gradle `testDebugUnitTest assembleDebug assembleRelease` 成功，Debug/Release 三 ABI 均已构建；JVM `up-to-date`。第七轮曾首跑得 227/2，两处失败为 gating 用例漏算单声道 −3.0103 LU 的测试期望错误，已修正。第五轮由主代理复测别名修正代码：Gradle 构建成功、PCM 测试 **139 checks / 0 failures**，DG-011 为 `DONE`，不标 `VERIFIED`）

本文件记录 Phase 0 / Phase 1 已触碰任务的**实际**状态、目标、交付物、验收条件与可核实证据。
`docs/ROADMAP.md` 定义任务，本文件登记执行结果；两者冲突时以本文件的证据为准。

## 2026-11-10 路线优先级调整

当前优先开发第三方媒体播放时的**系统媒体音量控制**，详见 [路线图当前优先目标](ROADMAP.md#当前优先目标第三方媒体音量控制p0)。

| 任务 | 状态 | 证据 |
| --- | --- | --- |
| DG-024 第三方媒体音量控制原型 | `IN_PROGRESS` | 第二十六轮主代理实测：`testDebugUnitTest assembleDebug` **BUILD SUCCESSFUL、92 tests / 0 failures / 0 skipped**；API 35 x86_64 模拟器未启动声景时 33%（5/15），音量卡调到 20%（3/15），`dumpsys audio` 记录 `setStreamVolume STREAM_MUSIC index 3 oldIndex 5 from com.dreamguard.app`，页面 20%。无第三方播放器共存、外部同步未可靠验证、无固定输出/蓝牙/真机 |
| DG-025 睡眠定时与渐弱控制 | `TODO` | 尚未实现或测试 |
| DG-026 第三方播放共存与最小回归 | `TODO` | 尚未针对该功能执行 |
| DG-050 外部音频捕获与替换可行性 | `TODO` | 尚未执行实验 |

2026-11-10 的旧版短时真机回归仍按下文原样保留，不作为上述新增任务的验收证据。新版 30 分钟长时测试按用户要求推迟到功能构建之后。

## 状态口径

| 标记 | 含义 |
| --- | --- |
| `TODO` | 未开始 |
| `IN_PROGRESS` | 交付物已部分或全部落地，但验收条件尚未被证明满足 |
| `DONE` | 交付物已完成并存在于工作区，**不代表验收已运行通过** |
| `VERIFIED` | 验收条件已被可核查的证据实际执行通过（命令输出、构建产物、真机记录） |
| `BLOCKED` | 存在明确外部阻塞，无法继续推进 |

`DONE` 与 `VERIFIED` 不得互相替代。凡验收依赖运行（构建、测试、真机、泄漏检测）而**实际未执行**的，
一律只登记为 `DONE` 或 `IN_PROGRESS`，并写明未执行的精确原因。

## 总览

| 任务 | 状态 | 验收 | 证据类型 |
| --- | --- | --- | --- |
| DG-001 GitHub 仓库 | `DONE` | 可访问性已核实；"初始文件正确提交"未核实（`.git` 禁读） | 本次 HTTP 抓取 + 本地文件 |
| DG-002 Android 开发环境 | `DONE` | 第五轮主代理执行 `testDebugUnitTest assembleDebug assembleRelease` 成功（三个 ABI 原生库均构建）；第二十三轮再次执行 `--offline :app:testDebugUnitTest :app:assembleDebug` 成功（**80 tests / 0 failures / 0 skipped**，Debug 三个 ABI 编译通过）；Android Studio 未核实 | 第五轮命令输出 + **第二十三轮主代理命令输出** |
| DG-003 工程骨架 | `DONE` | 第五轮 Debug/Release 构建成功、JVM 测试 `up-to-date`（58 tests 未重跑）；第二十三轮 Debug 构建实际重跑，JVM **80 tests / 0 failures / 0 skipped**；"Kotlin 调用 Native" 仍为端到端证据，无 instrumented 断言 | 第五轮构建输出 + **第二十三轮 Gradle 输出** |
| DG-010 DSP 生命周期 | `DONE` | 生命周期测试在 API 35 模拟器 **1328 checks / 0 failures**（该部分本轮未修改，沿用第四轮结果）；「无资源泄漏」仍未验证 | 第四轮 NDK 交叉编译 + 模拟器输出 |
| DG-011 PCM 基础处理 | `DONE` | PCM 测试 **139 checks / 0 failures / PASSED**（最新别名修正代码，24 组，API 35 模拟器）；未接入播放链路、未处理真实音频，故不标 `VERIFIED` | 第五轮 NDK 交叉编译 + 模拟器输出 |
| DG-012 基础响度分析 | `DONE`（**不标 `VERIFIED`**） | 交付物已写入；x86_64 修复后 `dg_lufs` **238/0**、`dg_meter` **207/0**、扩展 C 头兼容 PASSED；Debug/Release 三 ABI 均构建通过，JVM up-to-date。参考对照工具链（`dsp-core/tools/lufs_compare/`）第十三轮首次真实执行 `--selfcheck` 得 113/1（工具信号缺陷：电平阶跃安静段实为 −70 dBFS），已修并加安静段 `momentary` 解析值检查，**第十四轮重跑 115 checks / 0 failures**（计数 +2 即新检查项）——工具自检全绿。但自检证明的是“实现与自身公式自洽 / 工具自身无缺陷”，**不是**与标准一致；**第十五轮完成首次真实对照**（`-Csv/-WavDir` 预生成模式 + FFmpeg 9.0.2 `ebur128`，16 例全部有读数：**14 PASS / 2 FAIL / 0 SKIP**，最大绝对差 **0.049 LU**，整体 **exit 1**，报告 `dsp-core/build/lufs-compare-android/reference-report.md`）。2 项 FAIL 均为 `kind=undefined` 的定义问题（静音、0.3 s：我方 `-inf`/`ready=0`，ffmpeg 报绝对门哨兵 −70.0），**保持未通过、待人工裁定**。ffmpeg 汇总只打印到 0.1 LU（`%5.1f`），0.049 与取整同量级 ⇒ 误差表已有数字，但对照**整体未通过**、分辨率就是 0.1 LU。第十六轮改用三位小数 metadata 主判据；主代理首次执行在高精度路径的 ffmpeg probe 处 exit 3，已定位为管道提前终止导致的假失败并修复。**第十八轮：高精度 metadata 对照真实执行完成**——同一 CSV（SHA256 `B4106C5E…18B138C`）+ 16 个 WAV，FFmpeg 9.0.2 `ebur128=metadata=1`，**14 个可比样本 14 PASS / 0 FAIL / 0 SKIP，worst |delta| 0.010 LU**（`stereo_asym_L-6_R-12dB_48k` 与 `stereo_1k_-20dB_192k`）；2 例静音/0.3 s 判 **NO-DATA**（silence 1、sub-400ms 1：metadata `I=-70.000` 是 `ABS_THRES` 哨兵，全部 M 块 < −70），**单列、不计入 PASS、不是数值通过**；有效分辨率约 0.01 LU（`HIST_GRAIN=100`），三位小数只是格式。报告 `reference-report-metadata.md`。真机与泄漏检测未做，故记 `DONE`、**不标 `VERIFIED`** | 第八轮三 ABI 构建 + 第十三轮 selfcheck 113/1 + 第十四轮 selfcheck 115/0 + 第十五轮 14/2/0（0.049 LU，summary-only）+ **第十八轮 metadata 14/0/2（worst 0.010 LU，verdict PASS）**
| DG-013 动态压缩器 | `DONE`（**不标 `VERIFIED`**） | 真实验证链：第十一轮首次编译失败（`compressor.h` 注释未闭合，已修）→ 第十二轮 **334/2**（两处均为测试期望错误，已改写）→ 第十三轮重跑 **348 checks / 0 failures**，`dg_header_c_compat` PASSED，Gradle Debug/Release 三 ABI 构建成功、JVM up-to-date。**第二十三轮输出参数类型修正后（`dg_pcm_view` → `dg_pcm_buffer`）在 API 35 x86_64 模拟器重跑 `dg_compressor_tests`：348 checks / 0 failures**。测试信号级验收达成；“无明显爆音或抽吸”仍**无听感/真机证据**，虽已接入 PCM16 桥但没有设备播放验证，故不标 `VERIFIED` | 第十二轮 334/2 + 第十三轮 348/0 + **第二十三轮类型修正后 348/0** 模拟器输出 |
| DG-014 峰值限制器 | `IN_PROGRESS` | 头文件、实现、测试与 CMake/CTest 接线已写入；设计明确只约束 **sample peak**，无过采样、无 true-peak 保证。主代理实测（NDK x86_64 API 35 模拟器）**dg_limiter 2113 checks / 0 failures**；Gradle `testDebugUnitTest assembleDebug assembleRelease` 成功、三个 ABI 构建通过、JVM `up-to-date`。但 true-peak 过采样测量、真机、听感、播放链路接入均未做，验收“峰值目标得到实际测量验证”只覆盖 sample peak，不标 `DONE`/`VERIFIED` | 主代理第十七轮模拟器输出 + Gradle 构建输出；无 true-peak/真机证据 |
| DG-020 JNI Bridge | `DONE`（**不标 `VERIFIED`**） | 第二十二轮新增独立 PCM surface：`NativePcmProcessor` 的 5 个 `native*` 符号（create/process/reset/destroy/version），与声景引擎 9 符号按类前缀隔离。**第二十三轮实测**：`NativePcmProcessorContractTest` 包含在 80 JVM tests / 0 failures 中，Debug 三个 ABI 的 `libdreamguard_dsp.so` 编译链接通过；但**没有设备上的 JNI 运行证据**——JVM 合约测试与 mock 不等于实测 JNI。**第二十四轮**模拟器播放显示“本地压缩就绪”，可间接说明库加载与 `nativeCreate` 未报错，但 `nativeProcess`/`reset`/`destroy` 仍无设备证据。既有 9 符号的创建/处理/释放证据仍是第四轮模拟器 1328/0 | 80 JVM tests + 三 ABI 构建 + **第二十四轮 READY 间接证据**；**无 nativeProcess 设备证据** |
| DG-021 Media3 播放器 | `IN_PROGRESS` | 第二十轮主代理在 Android API 35 x86_64 模拟器安装启动，系统选择器从 Downloads 选 40 s / 44.1 kHz / 单声道 PCM16 WAV；UI 显示文件名、`16-bit PCM · 44100 Hz · 1 ch`、播放中，`dumpsys media_session` 为 `PLAYING`；暂停显示已暂停、恢复后 `PLAYING`。第二十一轮已在 iQOO Z9T（V2352A / vivo / Android 16 API 36）真机安装同批 Debug APK（0.1.0 / versionCode 1 / SHA256 `3ABD75F1…F4C97D2`，pid 22780），用户用系统选择器播放 40 s / 44.1 kHz / 单声道 PCM16 WAV，对声音、−6 dB、暂停恢复、爆音卡顿**合并答复“正常”**；锁屏约 15 s 持续、播其他媒体后暂停。**第二十三轮只构建了 Debug APK，未安装/未运行**；**第二十四轮已安装启动当前 Debug APK，系统选择器选 40 s / 44.1 kHz / mono PCM16 WAV，UI 播放中、MediaSession `PLAYING`、进程存活、`AndroidRuntime:E` 无 FATAL/UnsatisfiedLinkError**；仍无音频输出/波形/听感、播放进度 UI、多格式/多路由/进程重建/30 分钟长稳，故不满足 `DONE` | 第二十轮模拟器记录；**第二十一轮 iQOO Z9T 用户操作记录**（“正常”为合并答复，对应旧 Kotlin −6 dB 链路）；**第二十四轮模拟器播放状态证据（非音频输出）** |
| DG-022 AudioProcessor | `IN_PROGRESS` | 第二十二轮把 local PCM16 真正接入 C++ 压缩器：`LocalAudioProcessor` 协商 PCM16（1–2 ch 走 native，>2 ch 直通）、native 字节数完全匹配才采用、失败/缺库/非 direct/创建失败全部逐字节回退并置 `ERROR`、`reset`/`close` destroy。**第二十三轮修正**：`onFlush()` 按 media3 1.5.1 真实签名改为 Unit；句柄在 `flush()`（格式生效点）创建/替换，configure 与 flush 之间旧格式仍用旧句柄；失败后开关仍可关闭，格式提示始终明示直通原因；回退回卷输入位置；`close()` 终止处理；compressor 输出类型统一为 `dg_pcm_buffer`。**实测**：80 JVM tests / 0 failures / 0 skipped（含 18 个 `LocalAudioProcessorTest` + 4 个契约测试）、Debug 三 ABI；API 35 x86_64 模拟器 `dg_pcm16_processor_tests` **206 checks / 0 failures**（27 组）。**边界**：第二十三轮未运行 APK；**第二十四轮已安装运行，打开处理开关 `checked=true`，提示“本地压缩就绪”（`READY`）**；未观察到 `ACTIVE`、无逐块日志/波形/录音，仍不证明 `nativeProcess` 调用或实际音频输出；无听感、真机、true peak。验收“真实音频通过 DSP 后正常播放”未满足 | 80 JVM tests + 三 ABI 构建 + 模拟器 206/0 + **第二十四轮 READY 状态**；**无真实音频/设备处理证据** |
| DG-023 Android 基础 UI | `IN_PROGRESS` | 第二十轮在 API 35 x86_64 模拟器手工走通：文件选择器选 WAV → 显示文件名与格式 → 播放中；处理开关可开（`checked=true`）；暂停显示已暂停、恢复后播放；无 FATAL。第二十一轮在 iQOO Z9T 真机走通“选 40 s WAV → 播放 → −6 dB 开关 → 暂停/恢复 → 关闭回 DSP 声景”，用户答复“正常”、无爆音卡顿；**无截图比对、无波形**，错误路径与进程重建未测；第二十二/二十三轮改了开关与格式提示；**第二十四轮模拟器 UI 显示播放中、格式提示“本地压缩就绪”、开关 `checked=true`**，但仍无截图比对/真实听感/错误路径/进程重建/真机；“自然模式/响度显示”未做 | 第二十轮模拟器记录 + **第二十一轮真机用户操作记录**（对应旧版链路）+ **第二十四轮模拟器 UI 状态证据** |
| DG-024 第三方媒体音量控制原型 | `IN_PROGRESS` | 第二十五轮写入 `SystemMediaVolume.kt`（纯 Kotlin 范围/固定/失败/通知规则）、`AudioManagerMediaVolume.kt`（`STREAM_MUSIC` 读写 + Settings 观察）、`DreamGuardScreen.kt` 音量卡与 `SystemMediaVolumeTest.kt`（12 个 JVM 测试）。**第二十六轮主代理实测**：`.\gradlew.bat --no-daemon --offline :app:testDebugUnitTest :app:assembleDebug` **BUILD SUCCESSFUL**、**92 tests / 0 failures / 0 skipped**；API 35 x86_64 模拟器安装启动，未启动 DreamGuard 声景时页面待机、系统媒体音量 33%（5/15），用音量卡滑块调到 20%（3/15），`dumpsys audio` 记录 `setStreamVolume STREAM_MUSIC index 3 oldIndex 5 from com.dreamguard.app`，页面显示 20%。**仍缺**：第三方播放器共存/不被打断、实际声音电平、外部音量变化同步（模拟器 adb 调节未生效）、边界/固定输出/蓝牙/真机。故仍 `IN_PROGRESS`，不标 `DONE`/`VERIFIED` | 主代理 Gradle 输出 + 92/0 报告 + API 35 x86_64 模拟器记录；**无第三方播放器/真机证据** |

---

## DG-001 GitHub 仓库

**目标**：确认 `AmariS26242/DreamGuard` 存在；具备 MIT License、README、.gitignore、docs 目录与 GitHub Actions。

**交付物**：仓库本身 + `LICENSE`、`README.md`、`.gitignore`、`docs/`、`.github/workflows/android-ci.yml`。

**验收**：仓库可访问，初始文件正确提交。

**证据**

- 2026-11-10 抓取 `https://github.com/AmariS26242/DreamGuard` 返回 **HTTP 200**，页面标题为
  "Android-first real-time sleep soundscape with Kotlin, Jetpack Compose, Media3, C++20 NDK/CMake DSP core, and JNI" → 可访问性成立。
- 本地工作区存在 `LICENSE`、`README.md`、`.gitignore`、`docs/ROADMAP.md`、`.github/workflows/android-ci.yml`。
- **未核实**："初始文件正确提交"要求检查提交历史；`.git` 属于本任务的禁读路径（`excludedPaths`），本次未读取，
  因此不判断具体 commit 内容，也不把该子条件记为已核实。

**状态**：`DONE`（可访问性 + 文件存在）。不标 `VERIFIED`，因为提交历史未核对。

---

## DG-002 Android 开发环境

**目标**：Android Studio、JDK、Android SDK、NDK、CMake 可用；Gradle 构建与真机 USB 调试可用。

**交付物**：可构建的工程 + 工具链版本锁定（`gradle/libs.versions.toml`、`app/build.gradle.kts` 的
`compileSdk 35` / `ndkVersion 27.0.12077973` / CMake `3.22.1`）。

**验收**：空白 Android 工程成功构建并安装。

**证据（2026-11-10 第二轮实际执行；第三轮未重跑）**

- 工具链实测：JDK `17.0.21.0.2`（Microsoft build）、Gradle Wrapper（仓库自带 8.11.1）、Android SDK
  `E:\AgentImeTest\sdk`（`ANDROID_HOME`/`ANDROID_SDK_ROOT`）、NDK `27.0.12077973`、CMake `3.22.1`
  （`E:\AgentImeTest\sdk\cmake\3.22.1\bin\cmake.exe`）。
- `.\gradlew.bat --no-daemon --stacktrace clean testDebugUnitTest assembleDebug` → **BUILD SUCCESSFUL in 32s**
  （51 tasks：28 executed / 22 from-cache / 1 up-to-date），`buildCMakeDebug` 三个 ABI 均真实编译。
- APK 安装成功（`adb install -r` → `Success`），应用启动并进入播放（见 DG-003）。
- **未核实**：Android Studio 本机未安装/未启动（不需要它也能完成构建）；真机 USB 调试本次未使用（用模拟器）。

**状态**：`DONE`（构建 + 安装 + 运行已执行，但"Android Studio 可用"未核实，故不写 `VERIFIED`）。

---

## DG-003 工程骨架

**目标**：Gradle 多模块结构、Kotlin 主应用、Compose UI、C++ Native Library；配置 JNI、CMake，建立测试目录。

**验收**：Kotlin 能成功调用 Native 函数。

**本次交付物（2026-11-10）**

- **Gradle 多模块**：`settings.gradle.kts` 现为 `include(":app")` + `include(":dsp-core")`；
  `dsp-core/build.gradle.kts` 提供 `dspCoreConfigure` / `dspCoreBuild` / `dspCoreTest` 三个任务
  （`:dsp-core:tasks --all` 已列出，说明模块脚本可正常配置）。
- **独立 C++ 原生模块** `dsp-core/`：可移植静态库 `dreamguard_dsp_core`（`include/dreamguard/dsp_core.h`、
  `include/dreamguard/pcm.h`、`src/dg_engine.cpp`、`src/pcm.cpp`、`src/soundscape.{h,cpp}`），不依赖 Android SDK。
- **app 侧原生代码**改为单一薄桥 `app/src/main/cpp/jni_bridge.cpp`，只调用 C API；
  `app/src/main/cpp/CMakeLists.txt` 通过 `add_subdirectory` 复用 `dsp-core` 的源码，仍输出 `libdreamguard_dsp.so`。
- **测试目录**：Kotlin `app/src/test`（原有）+ native `dsp-core/tests`（CTest 注册 `dg_lifecycle`、
  `dg_header_c_compat`、`dg_pcm`）。
- **旧文件清理**：`app/src/main/cpp/dsp_core.{h,cpp}` 已从工作区删除（内容迁到 `dsp-core/src/soundscape.*`）；
  删除后又做了一次 `clean testDebugUnitTest assembleDebug` 复验，构建仍成功，说明已无 target 引用它们。

**验收证据（第二轮、第四轮、第五轮，2026-11-10 实际执行）**

- 构建：`clean` 后 `assembleDebug` 成功；`app/build/intermediates/cxx/Debug/1i3x666u/obj/{arm64-v8a,armeabi-v7a,x86_64}/libdreamguard_dsp.so`
  均为 2026-11-10 13:14:23–26 重新产出 → `dsp-core` 源码确实经 `add_subdirectory` 进入三个 ABI 的编译。
- APK：`app/build/outputs/apk/debug/app-debug.apk`，16 479 469 字节；最终 clean 构建于 2026-11-10 13:19:47
  （删除旧 `dsp_core.{h,cpp}` 之后，`BUILD SUCCESSFUL in 16s`，58 tests / 0 failures 复跑通过）。
- 运行：模拟器 `emulator-5554`（AVD `ime35`，API 35，x86_64）安装 Debug APK 后启动 `com.dreamguard.app/.MainActivity`：
  - `nativeloader: Load .../lib/x86_64/libdreamguard_dsp.so ... ok`（JNI 库加载成功）；
  - Media3 会话建立（`com.dreamguard.app/androidx.media3.session.id./7`），点按播放后状态变为
    `state=PLAYING(3)`，`requestAudioFocus()` 获得 `USAGE_MEDIA` 焦点，前台服务 `PlaybackService` 启动，进程存活无崩溃；
  - 播放持续 10 秒以上，位置从 0 → 20 → 30（毫秒计数）推进。
- JVM 单测：`--rerun-tasks` 重跑 → BUILD SUCCESSFUL（22 tasks executed）；报告 7 个测试类、
  **58 tests / 0 failures / 0 skipped**（含 `JniContractTest` 的 9 符号比对，符号名未变）。
- 第四轮（针对第三轮代码，主代理执行）：`clean testDebugUnitTest assembleDebug assembleRelease`
  全部成功；JVM 仍为 **58 tests / 0 failures**（Gradle 从 build cache 恢复测试结果，未强制 `--rerun-tasks`）；
  `assembleRelease` 通过，说明 Release 变体同样能编译并链接原生库。
- 第五轮（针对**别名修正后的最新代码**，主代理执行）：`testDebugUnitTest assembleDebug assembleRelease`
  → **BUILD SUCCESSFUL**，三个 ABI 的原生库均构建通过；JVM 任务为 `up-to-date`（未重跑，58 tests 仍是第四轮的报告结果）。
  该轮同时跑通最新版 `dg_pcm_tests`（见 DG-011），说明 PCM 改动没有破坏 Android 侧构建与 JNI 链接。

**状态**：`DONE`。**不标 `VERIFIED`**：
"Kotlin 能成功调用 Native 函数"目前是端到端证据（库加载 + 播放链路持续运行），
没有 instrumented 测试直接断言 JNI 返回值；本次也未在真机上回归。

**迁移后与旧实现的行为差异（有意保留或契约收窄）**

1. `nativeCreate` 现在把非法配置（采样率不在 8000–192000、声道不在 1–2）判为失败返回 `0`，
   而旧实现会把声道静默钳到 1–2 并返回句柄。生产路径只传 `48000 / 1`，不受影响。
2. JNI 返回约定未变：`nativeRender` 缓冲区不足仍返回 `-1`，其余错误仍返回 `0`，`nativeVersion` 仍为 `0.1.0`。
3. 第三轮删除了“已销毁句柄可被安全识别”的能力。按当前契约（`dsp_core.h` 顶部），销毁后再用同一句柄是
   **未定义行为且库不检测**（没有注册表、没有世代标签）。Kotlin 侧从不这样调用：渲染线程退出后才
   `destroy`，且销毁前已把 handle 清零，所以生产路径行为不变；但“二次 destroy 返回错误码”这一说法已作废。

---

## DG-010 DSP 生命周期

**目标**：实现 `dg_create`、`dg_destroy`、`dg_reset`、参数校验与错误码定义。

**验收**：生命周期测试通过，无资源泄漏。

**第三轮改动（2026-11-10）：契约简化，不再做误用检测**

第二轮实现用 `std::mutex` + `std::unordered_set` 活句柄注册表识别“已销毁/外来”句柄。该设计有两个问题：

1. **ABA**：句柄就是 `new` 出来的裸指针。销毁后地址被新引擎复用时，旧句柄值会命中注册表并操作**新实例**；
   注册表只能回答“这个地址当前在表里”，无法回答“这个地址是不是换过世代”。真正修掉需要槽位 + 世代 ID，
   而 64 位 ID 无法安全塞进 32 位 `armeabi-v7a` 的指针宽度（评审中已由用户指出）。
2. **音频路径代价**：每个 `dg_render` 都要进一次全局 mutex，`dg_destroy` 仍要与在途调用竞速，
   与“音频线程零锁”的目标冲突。

第三轮按“简单可靠的实现约定”重写：句柄仍是不透明指针，库内部**零全局状态**；误用（销毁后再用、伪造指针）
明确写为未定义行为，库**不检测、也不承诺检测**。生命周期由调用方保证，换来的是渲染路径无锁、无引用计数、无系统调用。

**当前交付物**

- `dsp-core/include/dreamguard/dsp_core.h`：纯 C99 公共头。不透明 `dg_engine*`；**6 个**可区分错误码
  （`DG_OK`、`DG_ERR_INVALID_ARGUMENT`、`DG_ERR_SAMPLE_RATE_OUT_OF_RANGE`、
  `DG_ERR_CHANNEL_COUNT_OUT_OF_RANGE`、`DG_ERR_OUT_OF_MEMORY`、`DG_ERR_NULL_HANDLE`）；
  边界 `DG_SAMPLE_RATE_MIN/MAX = 8000/192000`、`DG_CHANNELS_MIN/MAX = 1/2`。
  顶部写明：句柄只在 create→destroy 之间有效；NULL → `DG_ERR_NULL_HANDLE`；销毁后/伪造句柄是未定义行为；
  独立句柄无共享状态；`dg_render` 与 `dg_reset` 在同一句柄上必须串行（通常单音频线程，Kotlin 即如此）；
  setter 只写原子值、可与渲染并发、下一 block 生效；`dg_destroy` 必须与所有调用外部同步。
- `dsp-core/src/dg_engine.cpp`：无全局状态。create 校验顺序为 out 指针 → config → sample rate → channel count，
  失败一律清零 `*out_engine`，成功用 `new (std::nothrow)`；destroy 直接 `delete`；render 先清零
  `*frames_written` 再按序校验；`dg_channel_count(NULL) == 0`；`dg_reset` 清相位/滤波记忆/噪声种子/淡入，保留参数。
- `dsp-core/tests/test_dg_lifecycle.cpp`：17 组用例，覆盖创建校验矩阵（8000/44100/48000/192000 × 1/2 声道、
  预置 `*out_engine` 证明被清零、采样率先于声道校验）、并存句柄互不相同且首块逐字节相同、
  NULL 入口全部 `DG_ERR_NULL_HANDLE`、destroy 正常释放一次、render 参数校验与输出形状、
  `dg_reset` 后首块与新建引擎首块按位相同（先证明连续两块不同）、reset 保留参数、setter 钳制与 NaN/Inf 回退、
  “setter 与 render 并发”冒烟、“4 线程各自 create/render/destroy 且首块与参考引擎逐字节相同”、
  200 轮确定性、版本号。
- `dsp-core/tests/test_header_c_compat.c`：以纯 C 编译，跑最小生命周期 + 一次 PCM 旁路，保证两个公共头在 C 下可用。
- 测试入口（可重复执行）：`./gradlew :dsp-core:dspCoreTest`（内部 cmake + ctest），
  或 `cmake -S dsp-core -B dsp-core/build/native -DDREAMGUARD_DSP_BUILD_TESTS=ON` + `cmake --build` + `ctest`。

**验收状态（第四轮，2026-11-10，主代理在本机实际执行）**

- 生命周期测试：用 NDK `27.0.12077973` 的 `x86_64-linux-android26-clang++.cmd` 交叉编译**同一份**
  `dsp-core` 源码，在 Android API 35 x86_64 模拟器上执行 → **1328 checks / 0 failures / PASSED**（17 组用例）。
  这满足验收第 1 条“生命周期测试通过”。
- 构建证据：同一轮 `clean testDebugUnitTest assembleDebug assembleRelease` 成功，说明 `dg_engine.cpp` 在
  `arm64-v8a` / `armeabi-v7a` / `x86_64` 三个 ABI 下都能编译，并经 `jni_bridge.cpp` 链接进 `libdreamguard_dsp.so`。
- **“无资源泄漏”：仍未验证。** 仓库没有 ASan/LSan 作业，本轮也没有运行任何泄漏检测工具；
  `test_dg_lifecycle.cpp` 文件头明确写了它不是泄漏检测。
- 真机（V2352A / Android 16）未针对当前代码回归；宿主 `ctest` 路径仍受“本机无 MSVC”限制
  （`CMake Error: CMAKE_CXX_COMPILER not set, after EnableLanguage`），任务接线本身有效。

**状态**：`DONE`，**不标 `VERIFIED`** —— 验收第 1 条有证据，第 2 条（无资源泄漏）没有证据。

---

## DG-011 PCM 基础处理

**目标**（`docs/ROADMAP.md:53-56`）：支持 Float32 PCM、单声道、立体声、不同帧数缓冲区、静音输入与 Bypass。

**验收**：输出格式正确；旁路模式保持输入输出一致。

**交付物（2026-11-10 第三轮写入；第四轮加入缓冲别名与索引契约）**

- `dsp-core/include/dreamguard/pcm.h` + `dsp-core/src/pcm.cpp`：无状态、零分配、无锁的 Float32 PCM 助手，
  与声景引擎完全分离（不共享状态）：
  - `dg_pcm_bypass(view, buffer)`：形状必须一致，`memcpy` 逐位复制——NaN payload、±Inf、denormal、±0 全部保留；
  - `dg_pcm_apply_gain(view, buffer, gain)`：只接受有限且 ∈ [0, 1] 的 gain，其它值返回 `DG_ERR_INVALID_ARGUMENT`
    （不静默钳制，因为调用方传 2.0 或 NaN 时应该知道）；
  - `dg_pcm_map_channels(view, buffer)`：同声道逐位复制；mono→stereo 复制样本；stereo→mono 取 `(l+r)*0.5`；
  - `dg_pcm_is_silent(view, &silent)`：所有样本恰为 ±0 才算静音，0 帧块视为静音；
  - 形状规则：`frame_count` 任意（含 0）；`channel_count` 仅 1/2；`frame_count*channel_count` 在目标平台
    不可表示时返回 `DG_ERR_INVALID_ARGUMENT`。
  - **缓冲别名（第四轮明确，不再交给 C 库决定）**：输入输出为同一指针时，`dg_pcm_bypass`、
    `dg_pcm_apply_gain`、等声道的 `dg_pcm_map_channels` 都是定义良好的原地操作（旁路与等声道映射校验后直接返回，
    增益逐样本“读一次写一次”），实现用 `in_place()` 判断，**绝不对相同指针调用 `memcpy`**（相同指针的
    `memcpy` 在 C 标准下就是未定义行为）。**声道转换**（mono↔stereo）会改变样本数量，要求两块缓冲完全不相交，
    同一指针同样返回 `DG_ERR_INVALID_ARGUMENT`——mono→stereo 原地写会覆盖尚未读取的输入。
    两个不同指针但区间部分重叠的调用，三个函数一律拒绝且不写任何样本
    （`ranges_intersect()` 用 `uintptr_t` 比较区间，并对地址加法回绕做保守处理）。
  - **索引加宽（第四轮）**：`frame * 2` 一律先 `static_cast<std::size_t>` 再乘，`sample_count()` 也先加宽再乘，
    避免 64 位主机上 `uint32_t` 乘法回绕后写到缓冲区之外。
- `dsp-core/tests/test_pcm.cpp`：24 组用例（32 位目标上多一组溢出拒绝），覆盖逐位旁路（含带 payload 的 NaN、
  signed zero、denormal）、gain 1.0 逐位等于旁路、gain 缩放与静音保持、越界 gain 被拒且不写输出、
  mono↔stereo 映射、静音判定、0 帧块、1/2/3/63/64/65/1024/4096 帧 × 1/2 声道的旁路恒等；
  第四轮新增：同指针旁路/增益/等声道映射为原地且不改位、部分重叠被拒且缓冲不变、声道转换的同指针与重叠均被拒、
  65 536 帧立体声大块往返。
- `dsp-core/tests/CMakeLists.txt`：新增 `dg_pcm_tests` 并注册 `add_test(NAME dg_pcm ...)`；
  `dg_header_c_compat` 扩展为同时 include `dreamguard/pcm.h` 并跑一次旁路。

**验收状态（第五轮，2026-11-10，主代理在本机实际执行）**

- **最新别名修正代码**：主代理用 NDK `27.0.12077973` 交叉编译（x86_64）后在 Android API 35 x86_64 模拟器执行
  `dg_pcm_tests` → **139 checks / 0 failures / PASSED**，覆盖全部 **24 组**（含同指针原地、部分重叠拒绝、
  声道转换别名拒绝、65 536 帧立体声大块往返）。这证明验收两项（输出格式正确、旁路输入输出一致）对**当前代码**成立。
- `dg_lifecycle_tests`（1328 checks）与 `dg_header_c_compat` 属于**本轮未修改的部分**，结果是此前的；
  本轮没有重跑它们，也没有重跑 JVM 测试（Gradle `up-to-date`）。
- **>2^31 帧的索引溢出**无法在测试里分配（需要 ≥8 GiB 缓冲），只由代码检查保证；测试注释已写明这一点，
  不用一个“能过”的测试假装覆盖了它。
- **未接入播放链路**：Android 仍只把生成器输出直接写入 AudioTrack；PCM 助手没有改变任何现有音频行为，
  也没有新增 JNI 符号。
- **不是采集实现**：没有 AudioRecord / OpenSL / AAudio / 麦克风权限路径，也没有声称实现第三方音频捕获。

**状态**：`DONE`，**不标 `VERIFIED`** —— 验收两项对当前代码有可核实证据（139 checks / 0 failures），
但“实际播放链路 / 真实音频”没有证据，并且泄漏检测（ASan/LSan）与真机回归仍未做。

**历史记录（第四轮，别名修正之前的代码）**：`dg_pcm_tests` → 110 checks / 0 failures / PASSED，
`dg_header_c_compat` 同样通过。该数字已被第五轮的 139 checks 取代。

---

## DG-012 基础响度分析

**目标**（`docs/ROADMAP.md:72-75`）：对输入 PCM 做流式 RMS / peak 计量、计量窗口与 meter API；LUFS 只在标准依据充分时实现，否则明确标为未完成。

**验收**（路线图原文）：与可信参考工具对比并记录误差。

**交付物（2026-11-10 第六轮写入；第七/八轮已在 x86_64 上编译运行，见本节末“第七轮”“第八轮”）**

- `dsp-core/include/dreamguard/meter.h` + `dsp-core/src/meter.cpp`：有状态、流式、锁外串行化的响度计量器。
  - `dg_meter_create` 复用 `DG_ERR_*` 错误码：`sample_rate` 必须 ∈ [8000, 192000]（`DG_ERR_SAMPLE_RATE_OUT_OF_RANGE`），
    `channel_count` 必须为 1/2（`DG_ERR_CHANNEL_COUNT_OUT_OF_RANGE`），`window_ms` 为 0（默认 400 ms）
    或 ∈ [1, 1000]（否则 `DG_ERR_INVALID_ARGUMENT`）。
  - 输出（`dg_meter_values`）：`block_peak/block_rms`（最近一次**非空** `process` 调用的块）、
    `window_peak/window_rms`（最近 `window_frames` 帧的滚动窗口）、`peak_hold`（自上次 `reset_peak_hold` 的峰值）、
    `total_rms/total_frames`（自 create/reset）、`clip_samples`、`non_finite_samples`、
    `window_fill_frames/window_capacity_frames`。
  - **约定**：peak 是采样峰值，**不是 true peak**——本实现不做过采样/重建，头文件与 README 都这么写，不声称符合
    EBU R128 的 true-peak 要求；dBFS 为 `20*log10(x)`，0 映射到 `-INFINITY`；`|x| > 1.0` 的有限样本才计 clip，
    恰好 1.0 不计；NaN/±Inf 计入 `non_finite_samples`，对能量/峰值/窗口按 0 处理；零帧 `process` 是 no-op
    （`block_*` 保持上一次非空块的值）。
  - 内存：窗口环存 `|x|`，`window_frames * channels * 4` 字节（48 kHz/400 ms 立体声 153,600 B；
    192 kHz/1000 ms 立体声 1,536,000 B，是 API 允许的上限）。库内不做分配以外的系统调用，无锁无原子，
    同一句柄由调用方串行化。
- `dsp-core/include/dreamguard/lufs.h` + `dsp-core/src/lufs.cpp`：**PROVISIONAL** 的 BS.1770-4 式积分响度分析器，
  头文件第一段就写明“未与任何参考实现对比、不声称标准兼容”。
  - 适用：1/2 声道，8000–192000 Hz；时间窗口 momentary = 最近 400 ms、short-term = 最近 3 s（不足 3 s 为 `-INFINITY`）、
    integrated = 全部 400 ms 块（75 % 重叠）经两级门控后的均值。单位 LUFS，绝对门 −70 LUFS，
    相对门 = 过绝对门后块的均值响度 −10 LU；静音/无数据一律 `-INFINITY` 且 `integrated_ready == 0`。
  - K 权重：**不硬编码 48 kHz 表**，而是按 BS.1770 的模拟原型（高架 f0 = 1681.974450955533 Hz、
    G = 3.999843853973347 dB、Q = 0.7071752369554196、Vb = Vh^0.4996667741545416；高通 f0 = 38.13547087602444 Hz、
    Q = 0.5003270373238773）做双线性变换，双精度直接 II 型 biquad，逐声道独立状态。48 kHz 时与 BS.1770-4
    公布表在 **1e-6** 内一致（由测试断言，不是靠文档声明）。
  - 声道加权 G = 1.0（BS.1770 对 L/R 的定义），因此**同样峰值幅度的单声道信号比立体声低 10log10(2) = 3.0103 LU**；
    这是标准定义的结果，不是补偿缺失。
  - 校准依据（可复算，见下）：立体声 1 kHz 正弦、峰值幅度 A 的读数 = `20log10(A) - 0.691 + K(1 kHz)`。
    K 权重在 1 kHz 的增益由系数在 ω = 2π·1000/48000 处复算得 ≈ **+0.667 dB**，故读数 ≈ `20log10(A) - 0.024 LU`；
    测试容差取 0.1 LU，不把 0.024 的偏差说成 0。
  - 块容量 `DG_LUFS_BLOCKS_CAPACITY 36000`（= 1 小时），超出丢最旧并计入 `blocks_dropped`；块能量以 double 存，
    避免长时积分在 float 下丢精度。
- `dsp-core/tests/test_meter.cpp`（10 组）、`dsp-core/tests/test_lufs.cpp`（12 组）、
  `dsp-core/tests/test_header_c_compat.c`（新增纯 C 的 meter 与 LUFS 冒烟）：
  - meter 覆盖：create 校验矩阵（7999/192001 Hz、0/3 声道、window −1/1001 ms、0 = 默认 400 ms、8 kHz/1 ms → 8 帧、
    192 kHz/1000 ms → 192000 帧）、NULL 句柄、拒绝时不改句柄、DC 1.0 → 0 dBFS 且不计 clip、±0 静音、
    1 kHz 峰值 0.5 正弦 4800 帧 → RMS 解析值 0.353553（容差 1e-4）、立体声 L/R 独立、40 ms 窗口（1920 帧）满窗/滑出、
    clip 与非有限计数（1.0/1.5/−2.0/Inf/NaN/−Inf）、**任意分块与整块在流态字段上逐位一致**
    （`total_frames`/`clip_samples`/`non_finite_samples`/`window_*`/`peak_hold`/`total_rms`；`block_*` 按契约是
    “最近一次调用”的统计，故不参与分块比较）、
    最小次正规样本（`denorm_min`）平方在 double 中仍可表示，RMS/峰值不因下溢归零、`reset`/`reset_peak_hold` 语义、dBFS 辅助函数边界。
  - LUFS 覆盖：48 kHz 系数对照公布表（1e-6）、8k–192k 极点稳定、立体声 1 kHz 正弦 4 s 的 integrated 与
    `20log10(A)` 差 ≤ 0.1 LU 且与解析式 −6.044 一致、momentary/short/integrated 在稳态互差 ≤ 0.01、
    blocks 计数与门控前后计数关系、单声道比立体声低 10log10(2)（容差 1e-9，因为块能量严格成 2 倍）、
    窗口未就绪时的状态（0.35 s / 0.5 s / 3.2 s）、静音输入、
    门控两侧：4 s 峰值 0.1 + 1 s 静音 + 2 s −50 dBFS 时相对门必须剔除全部 quiet 块
    （精确断言 `blocks == 67`、绝对门后 ≥ 60、`blocks_used == 40`，期望值按单声道正弦约定取
    `20log10(0.1) − 10log10(2)`，见第七轮记录）、以及 4 s 峰值 0.1 + 2 s −9 dB（功率 0.126，
    在相对门之上）时 `blocks_used == blocks_after_absolute_gate == 57` 且 integrated 与手算块模型差 ≤ 0.5 LU、
    NaN 输入计数与恢复、分块与整块逐位一致。
- `dsp-core/CMakeLists.txt` 把 `src/lufs.cpp`、`src/meter.cpp` 编进同一个 `dreamguard_dsp_core` 静态库
  （App 的三个 ABI 因此自动包含它们）；`dsp-core/tests/CMakeLists.txt` 注册 `dg_meter`、`dg_lufs` 两个 CTest。

**ABI 静态核对（第六轮，非构建证据）**

- `app/build.gradle.kts:21` 的 `abiFilters` 是 `arm64-v8a`、`armeabi-v7a`、`x86_64`（含 32 位 armv7）；
  `app/src/main/cpp/CMakeLists.txt` 通过 `add_subdirectory` 引入 `dsp-core`，新源文件不需要任何 ABI 专属配置。
- 新代码只 include `<cmath> <cstddef> <cstdint> <limits> <new>` 与公共 C 头；不使用 JNI、Android NDK、线程、文件、
  locale、`long`、VLA 或指针转整数（`uintptr_t` 只出现在 `pcm.cpp` 与测试中）。只用定宽整数，
  `SIZE_MAX` 来自 `<cstdint>`，32/64 位都成立。
- **构建证据的实际情况**：第六轮本会话命令不可用（见环境记录第 6 条）；第七轮主代理用 Android NDK
  交叉编译 **x86_64** 并在 API 35 模拟器执行；第八轮修复后复测仍为 x86_64，同时 Gradle
  `assembleDebug assembleRelease` 让 **三个 ABI 全部构建通过**（编译 + 链接）。因此现在可以说“三 ABI 都能编过”，
  但**只有 x86_64 被运行过测试**：arm64-v8a 与 armeabi-v7a 的运行时行为仍然没有任何证据。

**验收状态（第六轮时；第七/八轮已部分改变，见本节末）**

- ~~**未执行**：没有 `cmake`/`ctest`/Gradle/NDK 交叉编译，没有模拟器或真机运行；`dg_meter_tests`、`dg_lufs_tests`
  与扩展后的 `dg_header_c_compat` 的 checks/failures 数字**不存在**~~ → 第七轮起已执行 x86_64 编译与模拟器运行，
  第八轮取得 `dg_lufs` 238/0、`dg_meter` 207/0、C 头兼容 PASSED，并完成三 ABI 构建；宿主 `cmake`/`ctest` 与真机仍未做。
- **未满足**：路线图验收要求“与可信参考工具对比并记录误差”。本轮没有 libebur128 / ffmpeg / 商业工具的可用参考数据，
  也没有可信的第三方读数可引用，因此**误差对比记为未完成**，DG-012 不进 `DONE`（第八轮结论相同：主代理 PATH 上无
  `ffmpeg`/`ffprobe`）。
- 已知的设计近似（如实登记，不掩盖）：
  1. 48 kHz 以外的采样率用双线性变换重算系数，与 BS.1770 在 48 kHz 的**原始模拟响应**有一阶近似差；
     没有与参考工具在 44.1/96 kHz 上对比过。
  2. 1 kHz 校准点本身有 ≈0.024 LU 的系统偏差（K(1 kHz) ≈ +0.667 dB 与 −0.691 offset 不完全抵消）。
  3. 积分响度用 400 ms 块均值，未实现 BS.1770 允许的 3 s 短时积分变体；没有 true peak 与 loudness range（LRA）。
  4. 块容量 1 小时后会丢最旧块（`blocks_dropped` 计数），不声称“无限长”。

**第七轮：首次设备运行（2026-11-10，主代理执行，本会话只改测试）**

- 主代理用 Android NDK 交叉编译 x86_64 目标，在 API 35 模拟器上运行两个测试程序，结果：
  `dg_meter` **207 checks / 0 failures**；`dg_lufs` **227 checks / 2 failures**。
- 两处失败都在 `dsp-core/tests/test_lufs.cpp` 的 gating 用例（修复前在第 326、327 行）。**根因是测试期望写错，
  不是门限算法错**：该用例用单声道、峰值 0.1 的 1 kHz 正弦，却期望读到 `20log10(0.1) = −20 LUFS`；
  正弦的响度是其 RMS 电平，且单声道比立体声低 10log10(2) = 3.0103 LU，正确值应为 **−23.01 LUFS**。
  同一文件较早的单声道校准用例本来就断言 `peak_dbfs − 10log10(2)`，即校准约定早已写对，只有 gating 用例漏了这一项。
  实现侧复核：相对门阈值 = 过绝对门块均值 / 10（即 −10 LU）≈ 0.069×loud 块能量，quiet 块（0.001×）低约 68 倍，
  被正确剔除，`blocks_used == 40`（37 全 loud + 3 个 75/50/25 % 过渡块），绝对门后 **≥ 60**
  （60 个有效块；若 loud→silence 的滤波振铃尾块仍高于 −70 LUFS 则为 61，测试按 `>= 60` 断言）。
  **算法未改。**
- 修复内容：`test_gating()` 期望改为 `20log10(0.1) − 10log10(2)` 并加 ±1 LU 区间、打印实际
  blocks/after_abs/used/integrated、把计数断言收紧为 `blocks == 67`、`blocks_used == 40`；
  新增 `test_relative_gate_keeps_near_blocks()`（4 s 0.1 峰值 + 2 s −9 dB 段，断言 57 块全部过相对门，
  `blocks_used == blocks_after_absolute_gate == 57`，integrated 与手算块模型差 ≤ 0.5 LU），作为“门限不能被
  调到过严”的回归。

**第八轮：修复后复测与三 ABI 构建（2026-11-10，主代理执行；本会话只改文档）**

- `dg_lufs_tests`（修复后）：**238 checks / 0 failures**，12 组全过，含新增回归。gating 用例的实际诊断输出为
  `blocks=67 after_abs=61 used=40 integrated=-23.1696`：
  - 三处计数与断言完全一致（67 / ≥60→61 / 40）；
  - `integrated = −23.1696` 与解析预期 `20log10(0.1) − 10log10(2) = −23.0103` 差 **0.159 LU**，方向与来源都和
    预估一致——3 个 75/50/25 % 过渡块把幸存块均值拉低约 0.17 LU（预估 0.17 LU），此外 1 kHz 校准点本身有
    ≈0.024 LU 系统偏差；两项合计解释了这个 0.16 LU，**不是靠放宽容差掩盖**；
  - `after_abs = 61` 证实了 loud→silence 的滤波振铃尾块确实高于 −70 LUFS（这正是测试按 `>= 60` 断言的原因——两种
    实现行为都合法，实测落在 61）。
- `dg_meter_tests`：同轮 **207 checks / 0 failures**（10 组）。
- 扩展后的 `dg_header_c_compat`（含 meter 与 LUFS 纯 C 冒烟）：**PASSED**——四个公共头在 C 下可编译、
  以 C++ 链接，`dg_meter`/`dg_lufs` 的 create→process→snapshot→destroy 路径在纯 C 下可调用。
- Gradle：`testDebugUnitTest assembleDebug assembleRelease` **BUILD SUCCESSFUL**；Debug 与 Release 的
  **三 ABI（arm64-v8a、armeabi-v7a、x86_64）** native 库全部构建通过（编译 + 链接，含新增 `lufs.cpp`/`meter.cpp`）；
  JVM 任务为 `up-to-date`（沿用第四轮 58 tests / 0 failures，本轮未重跑）。
- 仍未执行：真机运行、资源泄漏检测（ASan/LSan）、与 libebur128 / ffmpeg `ebur128` 的误差对比
  （主代理本机 PATH 上未找到 `ffmpeg`/`ffprobe`，因此参考工具对比仍为空）。

**第九轮：参考工具对照工具链（2026-11-10，本会话只写代码与文档；对照本身未执行）**

先审后写。审查结论：`src/lufs.cpp` 的 K 权重推导（`lufs.cpp:56-87`）、400 ms/75 % 重叠块（`lufs.cpp:177-206`）、
两级门控（`lufs.cpp:341-375`）与 libebur128 公开的推导形式一致；`tests/test_lufs.cpp` 的 12 组断言没有靠放宽容差
掩盖问题。**未发现需要修的算法缺陷**，因此本轮没有为了让对照“好看”而改库代码。

交付物（`dsp-core/tools/lufs_compare/`，host-only，不进 Android 构建）：

- `dg_lufs_compare.cpp`：16 例确定性合成信号的对照入口。
  - 逐样本自行合成，无时钟、无线程、无 fast-math、无重采样：xorshift32 固定种子、音调频率取整数周期，因此同一构建
    每次写出**逐字节相同**的 PCM。
  - 用例：立体声/单声道 1 kHz −20 dBFS、−23 dBFS、−6 dBFS、−60 dBFS（贴近 −70 LUFS 绝对门）、100 Hz 与 10 kHz
    （K 权重高通/高架区）、L−6/R−12 dB 非对称立体声、44.1k/96k/192k/8k 四个采样率、确定性噪声、单声道电平阶跃
    （4 s −20 dBFS + 1 s 静音 + 2 s −50 dBFS，共 7 s，专测相对门）、2 s 数字静音、0.3 s（不足一 400 ms 块）。
  - 输出本实现的 momentary / short-term / integrated / blocks / 过绝对门块数 / 存活块数 / pending 帧；
    `--wav-dir` 导出 32-bit IEEE float WAV（RIFF/fmt 字段与文件长度由 selfcheck 断言）、`--csv` 导出机器可读读数、
    `--selfcheck` 供 CTest、`--libebur128` 为可选的进程内第二参考（默认不编译）。
  - **解析期望只对稳态、左右等幅的正弦给出**：用 `dg_lufs_k_weighting_for_rate` 导出的系数在信号频率处复算
    `|H(e^{jω})|`，得 `20log10(A) + K_dB − 0.691 − (单声道 ? 10log10(2) : 0)`；噪声、非对称、电平阶跃记 `n/a`。
    因此它证明的是实现与自身公式自洽，**不证明与标准一致**——后者只能由外部工具给。
- `compare_with_ffmpeg.ps1`：参考侧驱动，**两种输入模式**。
  - 原生模式：自己运行 `dg_lufs_compare --wav-dir --csv`（`-Tool`，或按 `dsp-core/build/**` 搜索）。
  - **预生成模式（第十四轮新增）**：`-Csv <ours.csv> [-WavDir <dir>]` 直接读取别处生成的 CSV 与 WAV
    （典型来源：NDK 编译的 Android 版 `dg_lufs_compare` 在模拟器上生成，再 `adb pull` 回本机；`-WavDir`
    缺省取 CSV 所在目录）。此模式下脚本对输入**只读**：不改写 CSV、不移动/删除 WAV，且拒绝 `-CleanWav`。
    WAV 删除因此从“默认删除”改为“默认保留”，`-KeepWav` 仅为旧命令行兼容保留。
  - 定位 ffmpeg：`-Ffmpeg` → PATH → `dsp-core/build/reference-deps/**/ffmpeg*.exe`（构建目录内的副本，
    非系统安装，原样使用）。对每个 WAV 执行
    `ffmpeg -hide_banner -nostdin -nostats -i <wav> -af ebur128=peak=true -f null -`，取最后一条 `I: … LUFS`；
    ffmpeg 非零退出、或缺 `I:` 行 → `FAIL`，**不伪造 PASS**。
  - 数值判定：`delta = ours − ffmpeg`，**容差 0.1 LU（EBU Tech 3341 的表计精度），脚本里没有任何放宽路径**；
    `|delta| > 0.1` 即 `FAIL`；期望有值而双方都报无值 → `FAIL`；单边有值单边无值 → `FAIL`。
  - **定义问题（不是数值比较）**：静音（`stereo_silence_2s_48k`）与不足一 400 ms 块的 0.3 s 流
    （`stereo_1k_-20dB_0p3s_48k`）标 `kind=undefined`——双方都报无值记 `SKIP`，**明确不计入通过数**
    （“都同意值不存在”不等于数值一致）；我方给出数值（自身 bug）或参考侧给出数值（静音/短流的约定差异，
    需人工裁定或留档）一律 `FAIL`，不接受静默忽略。
  - 退出码：`0` 全过（且至少有一个 `PASS`）、`1` 有未通过项或无可比项、`2` 缺工具/CSV/WAV、`3` 缺 ffmpeg
    或 ffmpeg 跑不起来。缺 ffmpeg 时打印手工执行命令，不伪造 PASS。
  - 报告：默认 `<输入目录>/reference-report.md`（模式、CSV 的 SHA256、ffmpeg 路径与版本、容差、每例
    `kind/ours/ffmpeg/delta/PASS|FAIL|SKIP/note`、每例 WAV 的 SHA256、最大偏差及其用例、结论），旁边再写一份
    同名 `.csv` 机器可读副产物；报告末尾明确写“未经实际运行生成的报告不能作为证据”。
- `dsp-core/tools/lufs_compare/README.md`：前置条件、构建命令、selfcheck、ffmpeg 对照、libebur128 对照、
  用例表、已知偏差（1 kHz ≈0.024 LU、非 48 kHz 系数为一阶推导）、以及“不得为过关调宽容差”。
- `dsp-core/CMakeLists.txt`：新增 `DREAMGUARD_DSP_BUILD_TOOLS`（顶层 checkout 默认 ON，作为 Android 子目录时 OFF，
  与 tests 同规则）；`tools/lufs_compare/CMakeLists.txt` 注册 `dg_lufs_compare` 可执行文件与 CTest
  `dg_lufs_compare`（跑 `--selfcheck`），并新增 `DREAMGUARD_LUFS_COMPARE_EBUR128`（默认 OFF；开启时
  `find_path`/`find_library` 带 `REQUIRED`，找不到就在 configure 阶段失败，而不是静默编出没有参考路径的二进制）。
- `dsp-core/README.md`：Layout 增加 tools 目录；新增“Reference comparison harness (DG-012)”小节并明确
  “harness 已写、未执行”；测试清单增加 `dg_lufs_compare` 一条，结果记为 **not yet run**。

自检断言（`--selfcheck`；第十三轮与第十四轮已在 Android x86_64 上真实执行，此前未运行）：

- 任意分块（1 / 13 / 997 / 4800 / 4799 / 19200 / 7 / 65536 帧）与整块处理在 `momentary/short/integrated/blocks/
  after_abs/used/dropped/pending/ready` 上逐位一致；
- 每个稳态正弦的 integrated 与解析 K 权重值差 ≤ 1e-3 LU；
- 单声道比立体声低 10log10(2)（容差 1e-9，块能量严格 2 倍）；
- 2 s 静音与 0.3 s 流 `integrated_ready == 0` 且 integrated 为 −inf；
- 电平阶跃 `blocks == 67`、`after_abs >= 60`、`used == 40`、integrated 与块模型差 ≤ 0.5 LU（与 `test_lufs.cpp` 的
  固定断言一致，防止两处各自漂移）；**安静段 `momentary` 必须等于单声道 −50 dBFS 1 kHz 正弦的解析值（0.1 LU）**；
- WAV 容器：`RIFF/WAVE/data` 标记、`fmt = 3`（IEEE float）、32 bit、文件长度 = 44 + 帧数×声道×4。

**第十轮（首次真实执行）：`--selfcheck` 113 checks / 1 failure（2026-11-10，主代理 NDK x86_64 + API 35 模拟器）**

- 现象：唯一失败项 `mono_levelstep_4s_1s_2s_48k: expected >= 60 blocks above the absolute gate`。
- 关键指纹：同一用例的 `blocks == 67` 与 `blocks_used == 40` **都通过**，只有“过绝对门块数”不足。按 −50 dBFS 的
  安静段推算，过门块应为 37 个全响 + 3 个响→静音过渡 + 3 个静音→安静过渡 + 17 个安静块 ≈ 60（`test_lufs.cpp` 实测 61，
  多一块是响段高通滤波拖尾）；存活块数 40 不变而少了 17 块，只能说明这 17 块被**绝对门**拦掉，即安静段电平落在
  −70 LUFS 以下。
- 根因：`dg_lufs_compare.cpp` 的 `fill_sine` 内部乘的是用例的 `peak`（本用例 −20 dBFS = 0.1），而电平阶跃把
  `pow(10, −50/20)` 当作**相对比例**传入 → 安静段幅度 = 0.1 × 0.00316 = 3.16e-4，即 **−70 dBFS**，比文档与本工具
  自己 README 写的 −50 dBFS、以及 `tests/test_lufs.cpp:311` 的 `quiet_peak = std::pow(10.0, -50.0/20.0)`
  （**绝对** −50 dBFS）低 20 dB。−70 dBFS 单声道 1 kHz 正弦约 −73.7 LUFS，低于 −70 绝对门，于是 17 个安静块
  与 3 个静音→安静过渡块全部出局，过门块数只剩 ~43。
- 结论：这是**对照工具的信号生成缺陷**，不是 `dg_lufs` 的算法缺陷；既有 gating 测试的电平是对的，两者此前不一致
  （工具注释声称 −50 dBFS，代码给出 −70 dBFS）。因此**不改测试期望、不放宽 `>= 60`**。
- 修正：新增 `constexpr double kLevelStepQuietDbfs = -50.0`，安静段改为复制用例并覆盖 `peak/right_peak` 为
  `dbfs_to_linear(-50)` 后生成，与 `test_lufs.cpp` 的 gating 用例同一构造；同时**加强**自检——新增“安静段 `momentary`
  等于单声道 −50 dBFS 正弦解析值（0.1 LU）”一项，因为 −70 dBFS 那个错误恰好被 `blocks_used == 40` 掩盖，
  只有绝对电平读数能直接钉住它。
- 修正后**尚未重跑**：本节只登记“已修、待重跑”。113/1 不能算通过，112 条已通过项也不能算验收。

**第十四轮：修正后重跑 `--selfcheck` 115 checks / 0 failures（2026-11-10，主代理 NDK x86_64 + API 35 模拟器）**

- 结果：**0 failures**。计数从 113 变 115，正是第十三轮新增的两条“安静段 `momentary` 等于单声道 −50 dBFS
  解析值 / 有限性”检查——计数变化本身证明跑的是修正后的二进制，而不是旧产物重放。
- 这条证据能说明什么：工具自身的信号生成与自检逻辑无缺陷，且 `dg_lufs` 与“工具内部解析公式”自洽。
  它**不能**说明与标准一致：解析期望用的是本实现导出的 K 权重系数，属于同源自证。
- 本条不改 DG-012 的状态：自检全绿 ≠ 对照已执行。`reference-report.md` 仍然不存在。

**对照执行条件的现实变化（第十四轮）**

- FFmpeg **9.0.2** 已存在于 `dsp-core/build/reference-deps/imageio_ffmpeg/binaries/ffmpeg-win-x86_64-v9.0.2.exe`
  ——位于构建目录内，**不是**系统级安装；脚本会把它当作 PATH 之后的兜底候选，原样调用、不复制到系统路径。
- 本机**没有 Windows C++ 编译器**，因此“host 编译 → 跑工具 → 跑 ffmpeg”这条原生路线走不通。可行的路线是
  “NDK 编译 Android `dg_lufs_compare` → 模拟器上生成 CSV + 16 个 WAV → `adb pull` 回本机 → 本机脚本对这批
  真实文件调用 ffmpeg”。为此 `compare_with_ffmpeg.ps1` 新增 `-Csv`/`-WavDir` 预生成模式（见上），
  报告会记录 CSV 与每个 WAV 的 SHA256，从而能说清“两个表计量的是同一批字节”。
- 该路线**本会话仍未执行**：`pwsh` 在启动阶段即失败，脚本一行都没跑过，唯一能确认的是脚本可以逐行读懂、
  且缺 ffmpeg / 缺输入时它选择报错退出而不是给出结论。

**第十五轮：参考对照首次真实执行（2026-11-10 15:35:50；主代理在本机运行脚本，本会话只读报告并核对）**

- 输入：`dsp-core/build/lufs-compare-android/ours.csv`（SHA256 `B4106C5E…18B138C`）+ 同目录 16 个 32-bit float WAV，
  由 NDK 编译的 Android 版 `dg_lufs_compare` 在 API 35 模拟器生成后 `adb pull` 回来；本会话**没有重新生成、没有删除**任何输入。
- 参考：`ffmpeg version 9.0.2-essentials_build-www.gyan.dev`，路径
  `dsp-core/build/reference-deps/imageio_ffmpeg/binaries/ffmpeg-win-x86_64-v9.0.2.exe`（构建目录内，**非系统安装**）；
  每例执行 `ffmpeg -hide_banner -nostdin -nostats -i <wav> -af ebur128=peak=true -f null -`，取最后一条 `I: … LUFS`。
- 采样率/声道/信号覆盖：8k、44.1k、48k、96k、192k × 单声道/立体声；−6 / −20 / −23 / −60 dBFS 的 1 kHz、100 Hz、10 kHz；
  L−6/R−12 dB 非对称立体声；确定性噪声；单声道电平阶跃（4 s −20 dBFS + 1 s 静音 + 2 s −50 dBFS）；2 s 数字静音；
  0.3 s 短流（不足一个 400 ms 块）。
- 结果：**14 PASS / 2 FAIL / 0 SKIP**，最大绝对差 **0.049 LU**（`stereo_10kHz_-20dB_48k`：我方 −16.649118，
  ffmpeg −16.6）；其余 |delta| ≤ 0.030（100 Hz −0.025、非对称 −0.030、电平阶跃 +0.030、192k −0.020）。
  整体 verdict 为 `FAIL`，脚本 **exit 1**。
- 两项 FAIL：`stereo_silence_2s_48k` 与 `stereo_1k_-20dB_0p3s_48k`，`kind=undefined`；我方 `integrated_ready=0`、
  `integrated=-inf`（静音用例 `blocks=17 / after_abs=0 / used=0`；0.3 s 用例 `blocks=0`），ffmpeg 报 **−70.0**。
- **−70.0 是什么（按 FFmpeg 源码核实，不是推测）**：`libavfilter/f_ebur128.c` 定义 `#define ABS_THRES -70`（绝对门），
  `init()` 里 `ebur128->integrated_loudness = ABS_THRES;`，`filter_frame()` 只在
  `loudness_400 >= ABS_THRES` 且 `nb_integrated > 0` 时重写该字段。数字静音时 `power_400` 被钳到 `1e-12`，
  `loudness_400 = -0.691 + 10·log10(1e-12) ≈ -120.7 LUFS` < −70，永不进门槛；0.3 s 时 400 ms 缓存 `filled`
  始终为 0，同样不进门槛。两种情况下字段都停在初值，`uninit()` 用 `I: %5.1f LUFS` 打出 **−70.0**。
  所以 `−70.0` 是“**没有任何块通过绝对门**”的哨兵值（与门限常量同值），**不是** ffmpeg 测得该节目为 −70 LUFS，
  也不是标准认证；本文件不据此声称“通过”。另需说明这个哨兵**本身有歧义**：若某节目真被测成 −70.0 LUFS，也会
  被绝对门剔除并打印同一个数，因此单看 `I: -70.0` 无法区分“没有合格块”与“恰好停在门限上”——这正是两项不能由
  脚本自动裁定、必须人工定性的原因。
- 处置：我方输出 `-inf` + `integrated_ready=0` 表达的是 BS.1770 意义上“无合格块 ⇒ 不可定义”，与 ffmpeg 的哨兵
  指向同一事实，但**表达方式不同 ⇒ 不是数值一致**。按既定规则（`kind=undefined` 且单边有值 ⇒ `FAIL`），脚本判
  `FAIL` 是正确的，两项**保持未通过、待人工裁定**：要么书面采纳“ffmpeg 的 −70.0 等价于无定义”并单独立证据改分类，
  要么继续按 FAIL 留档。本轮**不改规则、不改 `lufs.cpp`、不调容差**。
- 精度事实（必须与数字一起读）：ffmpeg 汇总只用 `%5.1f` 打印，分辨率 **0.1 LU**，任何单例 delta 都自带 ±0.05 LU
  的取整误差；0.049 LU 与取整幅度同量级，因此这个偏差**不能**直接当作实现差异，反过来也不能说两者在 1e-3 LU 内一致。
  0.1 LU 容差正好等于 1 个量化步：这套对照只能发现“大于约 0.1 LU 的实现差异”。
- 本会话仍未运行任何命令（`pwsh` 启动即失败），上述数字全部来自主代理执行后落盘的文件；本会话逐行读取核对：
  `reference-report.md`（91 行）、`reference-report.csv`（17 行）、`ours.csv`（16 行）数值一致，未见伪造或漏判。

**第十六轮：高精度 metadata 路径落地（2026-11-10，脚本已改；首次执行 probe exit 3，修复后尚未重跑）**

- 动机：第十五轮的参考读数来自 ffmpeg 汇总行 `I: %5.1f LUFS`，只有 **0.1 LU** 分辨率；0.1 LU 容差正好
  等于一个量化步，0.049 LU 的最差值与打印取整同量级，无法区分“实现差异”与“取整噪声”。
- 脚本 `compare_with_ffmpeg.ps1` 已改为：每例执行
  `ffmpeg -hide_banner -nostdin -nostats -i <wav> -af "ebur128=metadata=1:peak=true,ametadata=mode=print:file=<temp>.txt" -f null -`，
  读取最后一条 `lavfi.r128.I`（`%.3f`）作为参考值；汇总 `I:` 仍解析，但只作一致性校验，二者差超过
  **0.0501 LU** 即 `FAIL`，防止解析错位静默给出结论。`metadata=1` 会把帧强制为 100 ms，`ametadata`
  逐帧写出 `lavfi.r128.I/M/S/LRA` 等键；临时文件名用相对路径（Windows 绝对路径在 filtergraph 里需要
  转义，容易出错），子进程 cwd 设为系统临时目录，读完即删。
- 精度声明必须克制：三位小数是文本格式；ffmpeg 的 integrated 值来自 `HIST_GRAIN=100` 的 0.01 LU 直方图，
  因此**有效分辨率约 0.01 LU，不是 0.001 LU**。这仍比旧的 0.1 LU 汇总强一个数量级。
- `NO-DATA` 语义（替代旧的 `SKIP`）：静音与短于一个 400 ms 块的流，ffmpeg 的 `lavfi.r128.I` 也会是
  `-70.000`，因为 `init()` 把 `integrated_loudness` 初始化为 `ABS_THRES`，且 `SET_META` 无条件写出 `I`。
  因此脚本要求**同时满足**：①我方无值；②metadata 与 summary 一致；③`I` 是 `-70.000` 哨兵；
  ④所有 `lavfi.r128.M` 严格 `< -70.000`。任一 `M ≥ -70` 即 `FAIL`（哨兵与真实 −70 LUFS 有歧义）。
  数字分支同样防哨兵：即使 `ours` 是数值，只要 metadata `I=-70.000` 且所有 `M` 为有限值并低于门限，
  就判 `FAIL`，不会因为数值接近 −70 而 `PASS`；`M` 缺失或非有限也判 `FAIL`。
  `NO-DATA` 按 `silence` / `sub-400ms` / `no-gating-block` 分类，**单独报告、不计入 PASS**；脚本只有在
  `failed=0` 且 `passed>0` 时才 exit 0。旧的两项 FAIL 因此会变成两项 `NO-DATA`（前提是实际运行确认
  M 见证成立），但**这不等于数值验收通过**。
- 已按 FFmpeg master 源码核对 `filter_frame()`：`M` 每 100 ms 帧都写出；前 3 帧 400 ms 窗口未填满时
  `loudness_400 = LOUDNESS(1e-12) ≈ -120.7 LUFS`，所以 0.3 s 用例仍会有 `M` 行可作见证，不会因为
  “没有 M”而误判。该核对是源码阅读，不是运行证据。
- **执行状态：主代理已尝试两次，均在 probe 阶段 exit 3；第二次修复后尚未重跑。** 2026-11-10 主代理用
  `-Csv dsp-core\build\lufs-compare-android\ours.csv -WavDir ... -Report ...\reference-report-metadata.md`
  直接执行：第一次脚本报 `ffmpeg ... does not run`；第二次报
  `ffmpeg -version probe exit 0; no 'ffmpeg version' line`，输出尾部是 libavfilter/libswscale/libswresample。
  但同一绝对路径的 `ffmpeg -hide_banner -version` 手工执行成功，版本 9.0.2，直接 exit 0。
  根因有两层：①旧 probe 把原生命令输出接进 `Select-Object -First 1`，提前结束管道会在 ffmpeg 正常退出前
  拆掉进程，使 `$LASTEXITCODE` 非零；②改用 `Invoke-FfmpegProbe` 后仍保留行尾 `$`，而 Windows 的
  `Out-String` 用 CRLF 连接，.NET 多行模式下 `$` 不在 `\r` 之前，所以 exit 0 且版本行在文本里也会被判
  “没有版本行”。现已完整捕获 stdout+stderr、先保存真实 exit code，并在全文搜索
  `(?i)(ffmpeg version[^\r\n]*)`：**不依赖行首/行尾锚点**，因此 CRLF 换行、`Out-String` 的行连接方式
  以及 stderr 合并后的 `ffmpeg.exe :` 前缀都不会再让版本行“消失”；exit 非零或没有版本行都继续 exit 3，
  `-filters` 也同样先判 exit code。**没有跳过 FFmpeg 检查，也没有把失败改成通过。**
  修复后尚未重跑，**没有生成 `reference-report-metadata.md` 或新 CSV**；旧 `reference-report.md` 未覆盖，
  仍是第十五轮 summary-only 产物，不能当作高精度证据。脚本另加报告防覆盖：目标已存在且未传 `-Force` 时
  exit 2，避免误覆盖旧证据。本会话 `pwsh` 仍在启动阶段失败，不能代为执行。
- 复跑入口（主代理在本机执行；不要加宽 `-Tolerance`；用新 `-Report` 路径避免覆盖第十五轮证据）：
  `pwsh -File dsp-core/tools/lufs_compare/compare_with_ffmpeg.ps1 -Csv dsp-core/build/lufs-compare-android/ours.csv -WavDir dsp-core/build/lufs-compare-android -Report dsp-core/build/lufs-compare-android/reference-report-metadata.md`
  预期生成 `reference-report-metadata.md` 与同名 `.csv` sidecar；旧的 `reference-report.md` 不动。运行后才能把两项旧 FAIL
  改判为 `NO-DATA`，届时仍需人工确认“NO-DATA 不计通过”的结论。

**第十八轮：metadata 高精度对照真实执行完成（2026-11-10，主代理执行；本会话只逐行核对报告）**

- 主代理用修复后的脚本、预生成模式对**同一批字节**执行：`-Csv dsp-core/build/lufs-compare-android/ours.csv`
  （SHA256 `B4106C5E3D84F81BE3E178C8B5FA6F37E23DA04B880B8E892B22510DD18B138C`，与第十五轮相同）+
  `-WavDir` 同目录 16 个 32-bit float WAV；参考为 FFmpeg 9.0.2（`dsp-core/build/reference-deps/...`，
  非系统安装）。产出 `dsp-core/build/lufs-compare-android/reference-report-metadata.md`（126 行）与同名
  `.csv` sidecar；第十五轮的 `reference-report.md` **未被覆盖**，输入 WAV/CSV **未被删除**。
- **数值结果：`comparable numeric cases 14`，`passed / failed / no-data = 14 / 0 / 2`，verdict `PASS`**。
  14 例 |delta| 在 0.001–0.010 LU 之间；worst 0.010 LU 出现在
  `stereo_asym_L-6_R-12dB_48k`（ours −8.030376 vs metadata −8.040）与
  `stereo_1k_-20dB_192k`（−20.020349 vs −20.030）。逐例：1k 48k 0.007、mono 0.006、−23 dB 0.007、
  −6 dB 0.007、−60 dB 0.007、100 Hz 0.006、10 kHz 0.001、44.1k 0.009、96k 0.009、8k mono 0.009、
  噪声 0.006、电平阶跃 0.006。全部在 0.1 LU 容差内（容差未放宽）。
- **2 例 NO-DATA（不是数值通过）**：`stereo_silence_2s_48k`（silence；20 个 M 块范围
  −163.524..−120.691）与 `stereo_1k_-20dB_0p3s_48k`（sub-400ms；3 个 M 块均为 −120.691）。
  metadata `I=-70.000` 仍是 `init()` 的 `ABS_THRES` 哨兵，不是测量值；报告要求“我方无值 + metadata 与
  summary 一致 + I 为哨兵 + 所有 M 严格 < −70”四条同时成立才记 NO-DATA，任一 M ≥ −70 即 FAIL。
  **NO-DATA 单独报告、不使退出码变 0、不计入 PASS**；其含义是“BS.1770 意义上无合格块 ⇒ 不可定义”，
  与我方 `-inf`/`integrated_ready=0` 表达同一事实，但不是数值一致。
- **量化约束（必须与数字一起读）**：`lavfi.r128.I` 是 `%.3f` 文本，但 FFmpeg 的 integrated 值在
  `HIST_GRAIN=100` 的 **0.01 LU** 直方图里累计，因此有效分辨率约 **0.01 LU，不是 0.001 LU**；
  0.010 LU 的 worst 与一个直方图 bin 同量级。metadata 与 summary（0.1 LU）差全部 ≤ 0.0501 LU 的一致性
  门槛（实测最大 0.02），没有解析错位。该结果说明“在这 14 个合成信号上，我方 integrated 与 FFmpeg
  ebur128 的差 ≤ 0.010 LU”，**不是**标准组织认证，也不覆盖真实音乐/节目素材。
- 本会话未执行命令（`pwsh` 启动即 `SetNamedSecurityInfoW failed (Win32 5)`），以上全部来自主代理落盘文件；
  本会话逐行读取报告核对：16 行结果表、14 PASS、0 FAIL、2 NO-DATA 与表头统计一致，worst 0.010 与两例
  0.010 行一致，未见漏判或伪造。
- 状态变更依据（对照上一节列出的三个 DONE 条件）：①两例定义差异已由脚本的 NO-DATA 规则与报告文字
  书面固定，并由用户本轮确认“NO-DATA 非数值通过”；②更高精度参考已复核，窄带/采样率偏差（最大 0.010 LU）
  不再落在 0.1 LU 取整噪声内；③metadata 路径已真实运行并留存新报告。因此 DG-012 记 **`DONE`**；
  真机、泄漏检测、`--libebur128` 仍未做，**不标 `VERIFIED`**；LUFS 头文件继续标 `PROVISIONAL`（不声称
  标准兼容），本文件也不把这次对照称为标准认证。

**证据边界（第十八轮之后，不得含糊）**

- 本会话 `pwsh` 仍在启动阶段失败：任何命令（含 `pwd`）返回
  `Error: SetNamedSecurityInfoW failed (Win32 5): grantWrite(E:\DreamGuard)`，命令体从未执行；按用户要求不改 ACL、
  不做系统级安装，因此本会话**无法 cmake、无法 ctest、无法调用 ffmpeg**。第十八轮的对照是主代理用自己的
  NDK/FFmpeg 环境跑出来的；本会话只读文件、写代码与文档。
- 第八轮已记录主代理本机 PATH 上无 `ffmpeg`/`ffprobe`；第十四轮确认 FFmpeg 9.0.2 只在构建目录
  `dsp-core/build/reference-deps/` 内（非系统安装），本会话同样没有安装任何工具。
- 已执行（累计）：`dg_lufs_compare --selfcheck` 两次（第十三轮 113/1 → 修正 → 第十四轮 115/0）；
  `compare_with_ffmpeg.ps1` 三次：第十五轮 summary-only（14 PASS / 2 FAIL / 0 SKIP，整体 exit 1）；
  第十六轮高精度路径在 **ffmpeg probe 阶段 exit 3**（只证明脚本能拒绝执行，不算一次对照）；
  **第十八轮高精度 metadata 路径成功（14 PASS / 0 FAIL / 2 NO-DATA，verdict PASS）**。
  仍未执行：`--libebur128` 进程内对照（本机无该库、未安装）、宿主 `ctest`、真机（只有 API 35 模拟器 x86_64）、
  ASan/LSan、arm64-v8a / armeabi-v7a 运行测试。
- 第十五轮 summary-only 对照的判别力有限：0.1 LU 容差 = ffmpeg 打印的 1 个量化步，只能发现
  “大于约 0.1 LU 的实现差异”。第十八轮的 metadata 主判据（`lavfi.r128.I`，0.01 LU 直方图分辨率）
  已复核：1 kHz、100 Hz、10 kHz、44.1 kHz、96 kHz、192 kHz、8 kHz、非对称、噪声与电平阶跃共 14 例的
  |delta| 全部 ≤ 0.010 LU。该结论仍只覆盖这 14 个合成信号与这个 FFmpeg build。
- `ebur128=metadata=1` + `ametadata=print` 已是脚本默认路径并**产生了有效读数**；`--libebur128`
  仍无法执行（本机无库）。两者不再混为一谈：前者已有报告，后者仍是未执行项。
- 预生成模式的意义在于把“缺 host 编译器”这个阻塞从对照流程里去掉：工具在模拟器上跑、参考在本机跑，
  两边测同一批 WAV 字节（报告记录 SHA256）。它**不降低**验收标准：仍然是 0.1 LU、仍然是每例 `ours` 与
  `ebur128` 的数值比较，静音/0.3 s 仍不计入通过。
- 路线图验收项“与可信参考工具对比并记录误差”现由第十八轮报告满足（14/14 数值 PASS，误差表完整）；
  两例 NO-DATA 不计入 PASS，也不伪装成数值一致。因此 DG-012 记 `DONE`；真机与泄漏检测仍缺，**不标
  `VERIFIED`**；LUFS 头文件保留 `PROVISIONAL`，不声称标准兼容，也不把这次对照称为认证。

**状态**：`DONE`（**不标 `VERIFIED`**）、LUFS `PROVISIONAL` —— 代码在 x86_64 上编译、运行并通过全部 238 checks，
三 ABI 均能构建，C 头兼容通过；`--selfcheck` 修正后 **115/0**；**高精度参考对照已真实执行**：14 个可比
数值样本 **14 PASS / 0 FAIL**、worst |delta| **0.010 LU**，2 例静音/0.3 s 判 **NO-DATA**（ABS_THRES 哨兵，
全部 M 块 < −70），报告 `reference-report-metadata.md` 留存。有效分辨率约 0.01 LU（`HIST_GRAIN=100`），
三位小数只是格式。真机、泄漏检测、`--libebur128` 仍缺，因此不标 `VERIFIED`。

---

## DG-013 动态压缩器

**目标**（`docs/ROADMAP.md`）：实现 Soft Knee、Threshold、Ratio、Attack、Release、立体声联动和增益平滑。
**验收**：测试信号处理符合预期，无明显爆音或抽吸。

**交付物（第十轮写入；编译/运行状态以下文第十二/十三轮与第二十三轮为准）**

| 文件 | 内容 |
| --- | --- |
| `dsp-core/include/dreamguard/compressor.h` | C99 兼容 API：`dg_compressor_config` / `dg_compressor_values` / `dg_compressor_create·destroy·reset·process·snapshot` / 纯函数 `dg_compressor_static_gain_db`；参数区间与错误码逐条注释 |
| `dsp-core/src/compressor.cpp` | 前馈实现：瞬时样本幅度检测器 → dBFS（−240 下限）→ 软拐点静态曲线 → 对**增益衰减**做单极点 attack/release 平滑 → `10^((gr+makeup)/20)` |
| `dsp-core/tests/test_compressor.cpp` | 16 组用例（见下表），风格与 `test_lufs.cpp` 一致（`CHECK`/`EXPECT_STATUS`/`CHECK_CLOSE`，末行打印 `checks, failures`，失败返回 1） |
| `dsp-core/tests/test_header_c_compat.c` | 追加 DG-013 段：纯 C 调用 `dg_compressor_static_gain_db`（−10 dBFS / 4:1 → −7.5 dB）、create→process→snapshot→destroy |
| `dsp-core/CMakeLists.txt`、`dsp-core/tests/CMakeLists.txt` | `src/compressor.cpp` 进静态库；`dg_compressor_tests` + CTest `dg_compressor` |

**算法定义（与头文件注释一致，可独立复核）**

1. 检测器 = 每帧的瞬时样本幅度 `|x|`（`link_stereo=1` 时取各声道最大值）。**无 hold/decay 包络、无过采样**，因此不是 true peak 检测器。
2. 电平 `20·log10(max(peak, 1e-12))`：数字静音读 −240 dBFS，曲线不会遇到 −∞。
3. 静态曲线（`x = level_db − threshold_db`，`k = knee_db`）：
   `x ≤ −k/2 → 0`；`x ≥ k/2 → (1/ratio − 1)·x`；区间内 `(1/ratio − 1)·(x + k/2)² / (2k)`。
   `k = 0` 退化为硬拐点；拐点始终以 threshold 为中心；`ratio = 1` 且 makeup = 0 时增益恒为 `exp(0) = 1.0`，**逐位直通**。
4. 弹道：对增益衰减做一阶平滑，`coef = 1 − exp(−1/(ms/1000·fs))`，`target < gr` 用 attack，否则用 release。
   由此增益逐帧变化 ≤ `(target − gr)·coef`，**不会因增益跳变产生爆音**——这是本设计能给出的“无爆音”论证（输入本身的
   阶跃仍会原样通过，因为本组件没有 lookahead，也不做限幅）。
5. 立体声：linked 时两声道共用同一 target 与同一增益（声道间比例不变）；unlinked 时各自独立检测、独立增益。
6. **无限幅器**：makeup 可为正，输出可超 0 dBFS，超幅样本计入 `clip_samples`；峰值限制属 DG-014。

**测试矩阵（16 组；第十三轮实际运行 348 checks / 0 failures；第二十三轮输出参数类型修正后复跑仍 348/0）**

- create 校验矩阵：NULL config/out、采样率 7999/192001、声道 0/3、六个参数各自的 min/max 接受与 ±0.5 拒绝、NaN/±Inf 拒绝、`link_stereo` 只接受 0/1、失败时 `*out` 被置 NULL；
- NULL 句柄与描述符：五个入口的 `DG_ERR_NULL_HANDLE`/`DG_ERR_INVALID_ARGUMENT`、零帧 no-op（任意声道数、不改计数）、声道与帧数不匹配、空 samples、**部分重叠拒绝且不写**；
- 静态曲线闭式值：硬拐点 −20 dBFS/4:1 → 0/−7.5/−15，2:1 → −10，100:1 → −19.8，ratio 1 处处 0；软拐点 6 dB → −0.5625（阈值处）/−1.0（−19 dBFS）/−2.25（拐点上沿，与硬拐点连续）；24 dB 拐点在阈值 → −2.25；单调性；**恰好 −20 dBFS（double，硬拐点）用 `== 0.0` 断言精确零**，不用容差；
- ratio 1 直通：输出与输入**逐位相等**；
- 稳态与块峰值表（两个 1 s 块跑同一 handle）：第一块 `output_peak_db` = `level_db + target·(1−e^(−1/(attack·fs)))`（恰好一个 attack 步，1e-5），第二块 = `level_db + target`（settled，1e-5）；`gain_reduction_db` → −10.4845（1e-6）、`static_gain_reduction_db` 精确到 1e-12、`frames == 96000`、无 clip；
- 软拐点流与阈值边界：−14 dBFS（弯心上沿）→ −4.5；**浮点阈值附近**（`float(10^(−1))` = 0.100000001490116，实际 −19.99999987 dBFS，比阈值高 1.3e-7 dB）软拐点 `static` → −0.5625 ± 1e-7、硬拐点 → `(实际电平 − 阈值)·(−0.75)` ± 1e-12（≈ −9.7e-8）；−20.5 dBFS（明确低于阈值）→ `gain_reduction_db` 与 `static` **bitwise 0.0**；
- attack/release 定律：10 ms = 480 帧，1τ → `target·(1−e⁻¹)`，6τ → `(1−e⁻⁶)`，收敛后 1τ 释放 → `×e⁻¹`；8 kHz 下 80 帧同样成立；
- 正弦界：1 kHz 正弦的增益衰减必须在 `[peak_based − 0.5, −6]` 内，输出不超输入包络（波形稀疏，不能宣称精确值）；
- 立体声联动：L −6 dBFS / R −60 dBFS，linked 时声道比例严格保持 501.2 且静音声道也被压缩；unlinked 时静音声道**逐位不变**；
- reset：gr/static/frames/clip/峰值全归零，重跑逐位相同；
- 分块不变性：1/7/997/3/480/13 帧任意切分 → 输出与快照**逐位一致**；
- 无爆音：0 dBFS 阶跃 + 10 ms attack，增益单步 ≤ `15·(1−e^(−1/480))`、单调、无过冲，输出相邻样本跳变 < 0.005；
- makeup 与 clip：+24 dB → 稳态输出峰值 9 dBFS、第二块 9600 个样本全部计入 `clip_samples`，数值保持有限；makeup 0 → −15 dBFS 且 0 clip；
- 非有限输入：NaN/±Inf 样本计数并输出 0（其余样本仍正常压缩）；全 NaN 流 → 计数 = 样本数、gr 仍为 0、输出全零；
- 原地别名：`in.samples == out.samples` 与分路处理**逐位相同**；
- 采样率/声道冒烟：8k/44.1k/48k/96k/192k × 单/双声道，0.1 s，全部 `DG_OK` 且快照有限。

**第十轮当时的未执行边界（已被第十二/十三/二十三轮的真实运行取代，保留以说明历史）**

- （第十轮当时）本会话 `pwsh` 在启动阶段失败，命令体从未执行；未改 ACL、未做系统级安装，因此当时没有编译、
  没有跑过任何一条断言。
- 因此“无明显爆音或抽吸”**没有音频证据**：只有“增益单极点平滑 ⇒ 逐帧增益变化有界”的数学论证 + 一组后来在
  第十三/二十三轮实际运行的断言。没有监听测试、没有与商业压缩器的对比、没有真机、没有 true peak 行为、
  没有程序相关（auto）release。
- （第十轮当时）压缩器未接入播放链路：`app/` 的 Kotlin/JNI 一行未改，没有新增 JNI 符号。但 `src/compressor.cpp`
  已进 `dreamguard_dsp_core` 静态库，而 Android 构建以子目录方式引入 dsp-core，因此它会被编进
  `libdreamguard_dsp.so`。现在它已通过 DG-022 桥接入本地 PCM16 路径（第二十二/二十三节），但**仍未做设备播放验证**。

**首次真实编译：失败（第十一轮，主代理在其本机 NDK x86_64 执行，已修待重测）**

- 现象：编译 `test_compressor.cpp` 时 `compressor.h:161` 报 `warning: '/*' within block comment`，
  随后出现多处 `use of undeclared identifier 'dg_compressor_process'`（`test_compressor.cpp` 里每一次
  `dg_compressor_process` 调用都报），`test_header_c_compat.c` 同样受影响。
- 根因：第十轮把 `dg_compressor_process` 的返回值注释从两条 `DG_ERR_INVALID_ARGUMENT` 合并成一条时，
  **漏掉了注释块结尾的 `*/`**。于是从 `/* Compresses frame_count ...` 开始的块注释一直吞到
  `dg_compressor_snapshot` 注释末尾的 `*/`（那里面又出现一个 `/*`，这就是 161 行告警的来源），
  `dg_compressor_process` 的声明整段变成注释。
- 修正：补回 `*/`（`dsp-core/include/dreamguard/compressor.h:157`），把四条失败原因合并进同一条注释；
  并复核了头文件其余注释块的闭合情况，`compressor.cpp` / `test_compressor.cpp` / `test_header_c_compat.c`
  的注释也已通读确认无嵌套。
- **测试的真实期望一条都没有改**：这次只动注释分隔符，没有调整任何断言、容差或验收标准。
- 状态：头文件修正后主代理重测，进入下面“第二次运行”。

**第二次运行：334 checks / 2 failures（第十二轮，主代理 NDK x86_64 + API 35 模拟器）**

- 现象：编译通过，`dg_compressor_tests` 跑出 **334 checks / 2 failures**：
  - `test_compressor.cpp` 稳态用例 `output_peak_db` 实际 **−6.236768**，期望 −16.505150（±1e-5）；
  - 软拐点用例“同电平硬拐点”`gain_reduction_db` 实际 **−0.000000**（约 −9.7e-8），期望 0（±1e-12）。
- 根因分析（都是**测试期望错**，实现按文档行为正确，故未改实现）：
  1. `output_peak_db` / `input_peak_db` 的定义是“**最近一个块内**的峰值”，不区分 attack。
     0.5 幅方波 + 1 ms attack 的第一块，增益从 0 dB 单调下降，因此最响样本是**块内第一个样本**，
     其值 = `level_db + target·(1 − e^(−1/(attack·fs)))` = −6.0206 + (−10.4846·0.0206176) = **−6.23677**，
     与实测 −6.236768 吻合到 6 位小数——这反证了“先平滑后写样本”的实现顺序正确。
     旧期望把“块峰值”当成了“稳态峰值”，属于指标语义误用。
  2. 阈值边界不可用浮点样本精确表达：`float(10^(−20/20))` = 0.100000001490116，比 −20 dBFS **高 1.3e-7 dB**，
     于是硬拐点给出 `(1.3e-7)·(−0.75)` ≈ **−9.7e-8 dB** 的合法衰减。旧期望要求 bitwise 0，物理上不成立。
     这不是实现缺陷，也不能靠放宽容差掩盖——正确做法是把“恰好阈值”放到 double 的纯函数上验证。
- 修正（测试侧，实现一行未改）：
  - `TwoBlockResult` 增加 `output`（保存第二块输出），稳态用例改为同一 handle 连跑两个 1 s 块：
    第一块断言**攻击峰值**解析值（1e-5），第二块断言**稳态峰值** `level_db + target`（1e-5），
    并保留 `frames == 96000`、`static` 精确 1e-12、`output.back()` 稳态幅度（1e-6）；
  - 软拐点用例改用能从样本反算的**真实电平**做期望：软拐点 `static` = −0.5625 ± 1e-7，
    硬拐点 = `(实际电平 − 阈值)·(−0.75)` ± 1e-12（≈ −9.7e-8），并先断言该电平确实只高出阈值 1.3e-7 dB；
  - 新增“明确低于阈值（−20.5 dBFS）”用例，`gain_reduction_db` 与 `static` 用 `== 0.0` **bitwise** 断言；
  - 静态曲线测试在**恰好 −20 dBFS（double）** 加 `CHECK(gain == 0.0)`，用精确零替代 1e-12 容差；
  - `compressor.h` 把 `input_peak_db/output_peak_db` 的注释改为“覆盖整个块、包含 attack 过程”，
    `dsp-core/README.md` 补充这两条语义说明。
- 状态：修正后主代理重跑，进入下面“第三次运行”。

**第三次运行：348 checks / 0 failures（第十三轮，主代理 NDK x86_64 + API 35 模拟器）**

- 结果：`dg_compressor_tests` **348 checks / 0 failures**；扩展后的 `dg_header_c_compat`（含 DG-013 冒烟）**PASSED**；
  Gradle `testDebugUnitTest assembleDebug assembleRelease` 成功，Debug/Release 三 ABI 构建通过，JVM `up-to-date`。
- 计数变化 334 → 348：第十二轮改写与新增的断言（攻击峰值解析式、稳态峰值、静音下限、bitwise 零、精确阈值、
  真实电平推导的硬拐点期望）计入其中，说明这次跑的是修正后的测试，而不是旧二进制。
- 这说明什么、不说明什么：**说明**静态曲线、软/硬拐点、attack/release 定律、块峰值计量语义、分块不变性、
  原地别名、非有限输入、makeup 与 clip 计数在当前实现上按定义自洽；**不说明**听感好、不说明与商业压缩器一致、
  不说明真机表现、不涉及 true peak/限幅（DG-014）、未接入播放链路。
- 状态：验收第 1 条“测试信号处理符合预期”**已达成**；第 2 条“无明显爆音或抽吸”只有设计论证 + 无爆音断言，
  **没有听感证据**，故登记 `DONE` 但不标 `VERIFIED`。

**状态**：`DONE`（**不标 `VERIFIED`**）—— 已真实编译并运行四次：第十一轮编译失败（注释未闭合，已修）、
第十二轮 334/2（两处测试期望错误，已改写）、第十三轮 **348/0** 且 C 头兼容 PASSED、三 ABI 构建成功；
**第二十三轮输出参数类型修正（`dg_pcm_view` → `dg_pcm_buffer`）后，主代理在 API 35 x86_64 模拟器复跑
`dg_compressor_tests` 仍为 348 checks / 0 failures**，把证据对到当前签名。
验收的信号级部分有证据；“无明显爆音或抽吸”仍缺听感/真机证据，压缩器虽已接入 PCM16 桥，
但**没有设备播放/输出验证**，因此不标 `VERIFIED`，也不得把本组件描述成“已发布/已验收音质”。

**后续状态（第二十二/二十三节）**：压缩器已通过新的 `dg_pcm16_processor` 桥接入本地文件 PCM16 播放链路
（`LocalAudioProcessor` → JNI → `dg_compressor`）；第二十三轮已编译出 Debug 三 ABI、PCM16 桥 native 测试
**206/0**、压缩器 **348/0**，但**未运行 APK 播放、未在设备上验证真实音频**；DG-013 的 `DONE`（不标 `VERIFIED`）
状态不变，并**不得**把接线/单测当作“压缩器已通过听感验收”。

---

## DG-014 峰值限制器

**目标**（`docs/ROADMAP.md:154-157`）：实现峰值检测、True Peak 检测策略、限制器、过采样验证和极端输入测试。
**验收**：峰值目标得到实际测量验证。

**交付物（第十七轮写入；主代理已在 NDK x86_64 模拟器运行测试）**

| 文件 | 内容 |
| --- | --- |
| `dsp-core/include/dreamguard/limiter.h` | C99 兼容 API、sample-peak/true-peak 边界说明、参数区间、别名与延迟契约 |
| `dsp-core/src/limiter.cpp` | 前馈 sample-peak lookahead 限制器：每帧所需增益 → 滑动最小值 → 即时 attack/单极点 release → 延迟线输出 |
| `dsp-core/tests/test_limiter.cpp` | 14 组主机测试：参数、NULL/描述符/重叠、延迟、脉冲、跨块/声道、flush/reset、原地、NaN/Inf、极端参数 |
| `dsp-core/tests/test_header_c_compat.c` | 追加纯 C DG-014 段：create → process → snapshot → flush → destroy |
| `dsp-core/CMakeLists.txt`、`dsp-core/tests/CMakeLists.txt` | `src/limiter.cpp` 进静态库；`dg_limiter_tests` + CTest `dg_limiter` |

**sample peak 与 true peak 的区别（设计前提，不是验收结果）**

- **sample peak**：`max |x[n]|`，只检查存储样本；本限制器测量并约束的就是它。
- **true peak / inter-sample peak**：重建连续波形后的最大值，可能落在两个样本之间。带限内容、有损编码或采样率转换后，
  即使所有样本都 ≤ ceiling，重建波形仍可能过冲。
- 本实现**没有过采样、没有重建滤波器**，因此不检测、也不衰减这种过冲；`limiter.h` 明确禁止把它描述成
  true-peak limiter 或“标准认证”。路线图要求的“过采样验证”**未实现**，所以 DG-014 的验收项不能算通过。

**算法与安全论证**

1. 每帧每声道 `req = min(1, ceiling_linear / max(|x|, 0))`；非有限样本替换为 0 并计数，不进入检测器或输出。
   linked 立体声用两声道最大幅度生成一个共享 `req`，保持声道比例；unlinked 各声道独立。
2. 每个声道维护最近 `latency_frames + 1` 个 `req` 的单调队列最小值。gain 高于窗口最小值时立即降到最小值
   （instant attack），否则按 `1 − exp(−1/(release_ms·0.001·fs))` 向最小值释放，并每帧 clamp 到最小值。
3. 延迟线输出 `y[n] = x[n − latency] · gain[n]`；由于 `gain[n] <= req[n − latency]`，有限输入下
   `|y[n]| <= ceiling_linear`（float 舍入用 1e-6 容差单独计数）。这是**采样峰值界**，不是 true-peak 界。
4. 延迟 `latency = ceil(lookahead_ms · rate / 1000)`；前 `latency` 帧输出 0，末尾 `latency` 个输入帧要由
   `dg_limiter_flush` 取出。flush 写满 `latency` 帧后把 envelope/delay/window 复位，计数与块峰值保留。
5. `process` / `flush` / `snapshot` 不分配、不加锁、不做系统调用；`create`/`destroy` 才分配。原地处理允许
   （`output.samples == input.samples`），其他任何重叠拒绝且不写。一个 handle 只能由一个线程驱动。

**参数与极端输入**

- 参数范围：采样率 8000–192000、1–2 声道、ceiling −60…0 dBFS、lookahead 0…20 ms、release 1…5000 ms、
  `link_stereo` 只接受 0/1；非法值按 sample rate → channels → ceiling → lookahead → release → link 顺序拒绝。
- 覆盖的极端输入：0 dBFS ceiling、−60 dBFS ceiling、20 ms @ 192 kHz（3840 帧延迟）、0 延迟、最长/最短 release、
  满幅脉冲落在最后一帧（必须经 flush 后仍不超 ceiling）、NaN/±Inf、纯静音、5 种采样率 × 单/双声道。

**测试矩阵（14 组；主代理实测 2113 checks / 0 failures）**

- create 全参数边界与 NaN/Inf；NULL 句柄、空 samples、声道/帧数不匹配、部分重叠拒绝、零帧 no-op；
- 延迟精确等于 `ceil(...)`，低于 ceiling 的信号逐位延迟直通；脉冲只出现在第 `latency` 帧且等于 ceiling；
- 跨 ceiling（−0.1/−1/−6/−20 dBFS）× 采样率（8k/48k/192k）× lookahead（0/1/10 ms）× 单双声道的采样峰值界；
- linked/unlinked 立体声比例；1 次 vs 37 帧分块逐位一致；flush 尾块、flush 后新流；reset；
- 原地与分路逐位一致；NaN/Inf 计数并输出 0；最后一帧脉冲；5 种采样率 × 单/双声道冒烟。
- 静态复核时发现并修正了一个**满窗口单调队列覆盖 front** 的潜在 bug（原实现先插入后过期，窗口满时
  `tail == head` 会覆盖最小值），并新增“required gain 单调递增填满窗口”的用例。该修正已随主代理本轮
  2113/0 的运行一起执行；仍未做变异测试或逐行覆盖率统计。

**未执行与证据边界（不得含糊）**

- 本会话 `pwsh` 仍在启动阶段失败（`Error: SetNamedSecurityInfoW failed (Win32 5):
  grantWrite(E:\DreamGuard)`），因此**本会话没有编译/运行**；下面的实测数字来自主代理，不是本会话。
- 主代理实测：NDK x86_64 API 35 模拟器 `dg_limiter` **2113 checks / 0 failures**；Gradle
  `testDebugUnitTest assembleDebug assembleRelease` 成功，Debug/Release 三个 ABI 构建通过，JVM `up-to-date`。
  这证明 `dg_limiter` 的 sample-peak 测试链在该配置下通过，不证明 true peak；`dg_header_c_compat` 的
  DG-014 纯 C 段虽已写入，但本轮没有单独给出它的运行输出，不能把 2113 这个数字移用到 C 头兼容上。
- 没有 true-peak 过采样测量、没有与商业限制器对比、没有听感测试、没有真机、没有 ASan/LSan。
- **未接入播放链路**：`app/` 的 Kotlin/JNI 一行未改，没有新增 JNI 符号；但 `src/limiter.cpp` 已进
  `dreamguard_dsp_core` 静态库，Android 构建会把它编进 `libdreamguard_dsp.so`，本轮 Gradle 三 ABI 构建成功
  说明编译兼容性已实际验证，但运行期播放链路仍未接入。
- 因此“峰值目标得到实际测量验证”**只完成了 sample-peak 部分**：测试测量了 sample peak，但路线图要求的
  true-peak 检测策略/过采样验证仍未实现，不能宣称 DG-014 验收通过，更不能宣称 true-peak 验收通过。

**状态**：`IN_PROGRESS`（不标 `DONE`，不标 `VERIFIED`）。sample-peak 测试已在 x86_64 模拟器通过；下一步仍需
至少 4× 过采样测量输出 true peak，并补真机/听感/播放链路证据。

---

## DG-020 JNI Bridge

**目标**（`docs/ROADMAP.md:174-182`）：Native Engine 创建、销毁、参数传递、PCM 缓冲区传输、错误处理。
**验收**：反复创建、处理、释放不发生崩溃。

**第二十二轮新增 PCM surface（第二十三轮已编译；无设备 JNI 运行证据）**

- Kotlin 侧 `com.dreamguard.app.audio.NativePcmProcessor` 对应 `jni_bridge.cpp` 新增 5 个独立符号：
  `nativeCreate(Int, Int, Float×6, Boolean) → Long`、`nativeProcess(Long, ByteBuffer, Int, Int, ByteBuffer, Int, Int) → Int`、
  `nativeReset/Destroy(Long) → Int`、`nativeVersion() → String`。与声景引擎的 9 个
  `NativeAudioEngine_native*` 符号不共享类前缀、不共享句柄类型。
- Process 只接受 direct ByteBuffer：非 direct 返回 `ERROR_NOT_DIRECT (-3)`，偏移/容量越界返回
  `ERROR_INVALID_ARGUMENT (-1)`，DSP 状态错误返回 `ERROR_DSP (-4)`，成功返回写入字节数（等于输入字节数）。
  Kotlin 常量与 C++ 常量逐项对应；任何失败都由 `LocalAudioProcessor` 转为逐字节回退，避免本地播放静音。
- 句柄安全：`jlong` ↔ 指针经 `intptr_t`，`dg_pcm16_processor*` 与 `dg_engine*` 不互转；Kotlin 侧只在
  播放线程 create/process/reset/destroy；`PlaybackService.onDestroy` 在 `player.release()` 之后调用
  `LocalAudioProcessor.close()`，此时播放线程已停止，不会与 `queueInput` 竞争。
- `NativePcmProcessorContractTest`（4 个 JVM 测试）反射检查声明名、断言无 Kotlin 模块后缀（`$`）、
  从 `jni_bridge.cpp` 抽取符号集合比对、并断言 PCM 符号没有借用 `NativeAudioEngine` 前缀。
  既有 `JniContractTest` 的 9 符号期望未改。

**证据边界（第二十三轮实测后，第二十四轮补模拟器状态）**：JNI 代码已随 Debug 构建编译并链接进三个 ABI 的
`libdreamguard_dsp.so`；`NativePcmProcessorContractTest` 在 **80 JVM tests / 0 failures** 中通过，
但它验证的是**声明名、符号集合、无 Kotlin 模块后缀**，**不是 JNI 运行**。第二十四轮模拟器播放当前 APK 时
UI 显示“本地压缩就绪”，可**间接**说明 native 库加载与 `nativeCreate` 路径未报错；但**没有** `nativeProcess`/
`nativeReset`/`nativeDestroy` 在设备侧被调用且正确的证据，JVM 合约与 mock 不能替代实测 JNI。
“反复创建、处理、释放不发生崩溃”仍只有第四轮 API 35 模拟器 **1328 checks / 0 failures**
（DG-010 声景引擎 + 旧桥）的历史证据，不能移用给新的 PCM surface。DG-020 保持 `DONE`，**不标 `VERIFIED`**。

---

## DG-021 Media3 播放器

**目标**（`docs/ROADMAP.md:183-194`）：配置 Media3、创建 ExoPlayer，实现本地文件播放、暂停与恢复、
播放进度和音频格式识别。**验收**：iQOO Z9T 本地音频正常播放。

**交付物（第十九轮写入）**

| 文件 | 内容 |
| --- | --- |
| `app/src/main/java/com/dreamguard/app/ui/DreamGuardScreen.kt` | `ActivityResultContracts.OpenDocument()` 选 `audio/*`；本地音频卡；错误复用 `playback.error` |
| `app/src/main/java/com/dreamguard/app/audio/PlaybackService.kt` | `setLocalAudio(uri)`、`toggleLocalPlayback()`、`closeLocalAudio()`；本地/内置轨互斥切换；URI 授权持久化与恢复 |

**实现要点**

- 选择器用 `ACTION_OPEN_DOCUMENT`（`OpenDocument`），不是 `GET_CONTENT`：系统授予可持久化的读权限，
  服务在 Activity 销毁或进程重建后仍能打开文件；`takePersistableUriPermission` 失败只降级为本次会话可用，
  不崩溃、不请求存储权限。
- 文件与内置 `dream_loop` 复用**同一个 ExoPlayer**：本地模式 `REPEAT_MODE_OFF`、`volume = 1f`；
  关闭本地时恢复 `REPEAT_MODE_ONE` 与 `layers.ambientVolume`，并按层选择决定播放或暂停。
- 进程重建：`onCreate` 读取 `local_uri`/`local_title`，校验
  `contentResolver.persistedUriPermissions` 后恢复媒体项；授权被撤销则清除记录并回退内置 loop。
- 暂停本地文件时置 `localPauseKeepsSession`，消费于 `onPlayWhenReadyChanged`，避免通知栏“暂停”被当成
  结束会话；恢复走 `startPlayback()`，播放器处于 error 时先 `prepare()`。
- 现有声景行为保持：本地激活时 `toggleAmbient()` 只持久化选择、不碰 player/volume；`toggleDsp()`、
  `syncPlayerWithRenderFailure()`、`startDsp()` 的失败暂停都加了本地守卫。本地文件播放中不因“无层”被暂停。

**第二十轮实际验证（主代理在本机执行）**

- `.\gradlew.bat --no-daemon :app:testDebugUnitTest --rerun-tasks :app:assembleDebug` **成功**：
  **68 tests / 0 failures / 0 skipped**（既有 58 + `LocalAudioProcessorTest` 10）。
- Android **API 35 x86_64 模拟器**安装启动；系统文件选择器从 Downloads 选取合成的 **40 s / 44.1 kHz /
  单声道 PCM16 WAV**；UI 显示文件名、`16-bit PCM · 44100 Hz · 1 ch`、播放中；
  `dumpsys media_session` 显示 `PLAYING`。
- 打开 −6 dB 开关后 UI `checked=true`；暂停后 UI 显示已暂停，再恢复后 MediaSession 回到 `PLAYING`；
  日志无 `AndroidRuntime FATAL` 输出。

**第二十四轮补记（主代理，API 35 x86_64 模拟器）**：安装当前 Debug APK 并启动；系统文件选择器选择合成的
40 s / 44.1 kHz / 单声道 PCM16 WAV；UI 显示播放中，MediaSession `PLAYING`，进程存活，
`AndroidRuntime:E` 日志无 `FATAL EXCEPTION` / `UnsatisfiedLinkError`。**未**录音、未采集波形、未做听感；
播放进度 UI 仍未实现。

**未执行 / 边界**：第二十轮未在 iQOO Z9T 真机执行；第二十一轮已在 iQOO Z9T 真机由用户操作确认
（见总览与第二十一轮记录，但“正常”是合并答复、对应旧 Kotlin −6 dB 链路，且无波形/量化听感）。
第二十三轮只构建 Debug APK、未安装/未运行；**第二十四轮已安装启动并进入播放状态，但没有真实音频输出、
波形/听感或真机证据**；模拟器无音频输出，未听感、未实证输出波形；播放进度仍未做 UI 呈现。
验收“iQOO Z9T 本地音频正常播放”尚未对当前 native 链路满足，故仍 `IN_PROGRESS`。

---

## DG-022 AudioProcessor

**目标**（`docs/ROADMAP.md:195-210`）：实现 configure、queueInput、getOutput、queueEndOfStream、isEnded、
flush、reset 和 DSP Core 集成。**验收**：真实音频通过 DSP 后正常播放。

**第二十二轮交付物（第二十三轮已构建/测试；未做设备播放）**

| 文件 | 内容 |
| --- | --- |
| `dsp-core/include/dreamguard/pcm16_processor.h` + `dsp-core/src/pcm16_processor.cpp` | PCM16→float→DG-013 compressor→PCM16；`bypass` 走 `memmove`；512 帧定长 scratch；残帧/奇字节原样；无 lookahead、无逐样本分配 |
| `dsp-core/tests/test_pcm16_processor.cpp` + `dsp-core/tests/CMakeLists.txt` | **27 组** C++ 测试（第二十三轮 +3：bypass 不改变压缩器状态、立体声残帧、输出容量大于输入）；注册 CTest `dg_pcm16_processor`；API 35 x86_64 模拟器实测 **206 checks / 0 failures**；`test_header_c_compat.c` 含 PCM16 纯 C 段（本轮未单独报告结果） |
| `app/src/main/cpp/jni_bridge.cpp` | 新增 `NativePcmProcessor_nativeCreate/Process/Reset/Destroy/Version`；direct buffer 校验与负错误码 |
| `app/src/main/java/com/dreamguard/app/audio/NativePcmProcessor.kt`、`PcmCompressor.kt` | JNI 封装、保守默认参数、`PcmCompressor` 测试接缝 |
| `app/src/main/java/com/dreamguard/app/audio/LocalAudioProcessor.kt` | native 主路径、回退、`NativeState`、flush 时创建/替换句柄、`nativeCapable`/`layoutSupported`/`nativeLibraryAvailable`/`nativeFailure`、`reset`/`close` |
| `app/src/main/java/com/dreamguard/app/audio/PlaybackService.kt`、`ui/DreamGuardScreen.kt` | 格式提示始终明示直通原因；开关已开启时始终可关闭；teardown 时 `close()` |
| `app/src/test/.../LocalAudioProcessorTest.kt`、`NativePcmProcessorContractTest.kt` | **18 个** fake 后端 JVM 测试（第二十三轮 +1：失败后仍可关闭处理；configure 测试按 flush 契约执行）+ 4 个 JNI 契约测试 |

**DSP 链路与参数（默认保守）**

- 输入限定 `C.ENCODING_PCM_16BIT` 且 `sampleRate > 0`、`channelCount > 0`；输出 format 与输入相同（不重采样、不混声道）。
- 处理链路：native-endian PCM16 → `float = s / 32768` → DG-013 压缩器
  （threshold **−18 dBFS**、ratio **2:1**、knee **6 dB**、attack **20 ms**、release **250 ms**、stereo linked、
  makeup **−6.0206 dB**）→ 半远离零量化并饱和到 int16。
- `makeup` 为 −6.02 dB 使安静素材保持旧开关的固定 −6 dB 衰减下限；超过阈值的峰值再被压缩。
  这是**有意的体验替代**，所以 UI 名称从“−6 dB”改为“本地压缩”，不是声称音质已被验证。
- 512 帧定长 scratch 位于句柄内，任意块大小分块处理；`bytes_written == input_bytes`，尾部残帧/奇数字节
  `memmove` 原样保留；零算法延迟，**不是 limiter**，无 true-peak 保证。

**行为语义**

- **bypass（逐字节位精确）**：开关关闭、native 库缺失、create 失败、process 返回非预期字节数/负码、
  非 direct buffer、>2 声道——全部 `output.put(inputBuffer)`，奇尾字节也保留。
- 1–2 声道才创建句柄；>2 声道 `configure` 仍接受（返回同 format，sink 直通），但 `nativeCapable=false`，
  UI 禁用开关并在格式提示写“多声道不支持压缩，直通”。不静默 down-mix。
- `process` 只有在返回字节数**完全等于**输入长度时才采用输出；失败后整块重写回退、置 `ERROR`，
  同一格式内不再逐块重试 JNI，直到 `flush()` 才恢复；这样不会在音频线程上反复付出失败成本。
- **生命周期（第二十三轮按 media3 1.5.1 契约修正）**：`configure()` 只记录 pending 格式与协商结果，**不碰**
  在用的 native 句柄；`flush()`（格式真正生效的位置）才创建/替换句柄——旧格式 buffer 在 configure 与 flush
  之间仍可继续排队并继续使用旧句柄；同格式 flush（普通 seek）保留句柄、只 `reset` 压缩器并清掉临时失败；
  `reset()`/`close()` → destroy 句柄（`close()` 只在 `player.release()` 后由 `PlaybackService.onDestroy` 调用，
  并置 `processingEnabled=false`，终止处理）。`onFlush()` 的真实签名是 **void**（不是 Boolean）。
- 回退路径在 native 返回非预期字节数时同时回卷输出与输入 position，保证整块逐字节复制完整。
- UI：开关“处理（本地压缩）”，副标题“开启时 PCM16 经 native 压缩器（−6 dB 补偿）；关闭时逐字节旁路”；
  格式提示在**开关关闭时也会**明示“多声道不支持压缩，直通 / native 库不可用，直通 / 处理失败，直通”，
  开关已开启时即使 `processingAvailable=false` 也保持可关闭，不会把用户锁在“开”状态。
- 开关 `@Volatile` 每块读一次，在 buffer 边界生效；句柄只在播放线程 create/use/reset/destroy，
  音频路径无逐样本分配、无逐样本锁、无 UI 回调。
- 原有声景引擎（`NativeAudioEngine`）与其 9 个 JNI 符号、播放层逻辑未改；没有新增权限、没有捕获第三方音频。

**测试（第二十三轮主代理实测）**

- C++ `dg_pcm16_processor`（**27 组**，Android API 35 x86_64 模拟器）：**206 checks / 0 failures**。覆盖
  create 校验（0/3 声道、坏采样率/ratio/link）、NULL 句柄/指针、零字节 no-op、bypass 位精确含残帧与原地、
  65 536 个 int16 全量往返（ratio 1 位精确）、−6.02 dB makeup、饱和不翻转、−20 dBFS/2:1 静态曲线
  （末样本约 10362、`gain_reduction_db ≈ −10`）、reset 回 unity、stereo link 与 unlink、分块与整块位一致、
  残帧原样、越界容量/NULL/部分重叠拒绝且不写、原地与分离缓冲一致、格式变化新句柄、latency=0；
  第二十三轮新增：bypass 前后压缩器状态逐字节不变、立体声残帧（整帧转换 + 残样原样）、输出容量大于输入时只写输入字节。
- Compressor `dg_compressor_tests`（同一模拟器）：**348 checks / 0 failures**（第二十三轮输出参数类型修正后重跑）。
- JVM（`--offline :app:testDebugUnitTest`）：**80 tests / 0 failures / 0 skipped**，其中本任务相关为
  `LocalAudioProcessorTest` **18 个** + `NativePcmProcessorContractTest` **4 个**。
  `LocalAudioProcessorTest` 用注入的 `FakeCompressor` 覆盖：native 后端收到整块且输出到 sink、bypass 位精确且
  零调用、奇尾字节、toggle、1/2/6 声道句柄策略、拒绝格式、同格式 flush 保留句柄+reset、EOS 排空、格式变化在
  flush 时 destroy+create、configure 后未 flush 仍用旧句柄、reset 清格式、缺库直通、process 失败回退且 flush 前
  不重试、失败后仍可关闭处理、非 direct 回退、create 失败不清音、多声道直通、未 configure 不建句柄、close 幂等。
- `NativePcmProcessorContractTest`（4 个）：5 个声明名、无 `$`、C++ 符号集合一致、类前缀隔离。
- Debug NDK 三个 ABI（arm64-v8a / armeabi-v7a / x86_64）编译链接通过；本轮**没有** Release 构建。
- media3 1.5.1 的 `onFlush()` void 契约已按源码核对并修正；**编译通过**，但这不等于媒体会话/真实音频路径已被验证。

**第二十四轮模拟器补记**：当前 Debug APK 在 API 35 x86_64 模拟器打开合成 40 s / 44.1 kHz / mono PCM16 WAV；
UI 显示“播放中”与格式提示“本地压缩就绪”，处理开关 `checked=true`。这验证 UI 文案/开关与 native `READY`
状态，**不验证**逐块 `nativeProcess` 或真实音频输出（见第二十四轮边界）。

**未执行 / 边界（第二十三轮；第二十四轮仅补模拟器状态）**：第二十三轮未安装/未运行 APK；第二十四轮已在
模拟器安装运行到“播放中 + 本地压缩就绪”，但**仍没有**“真实音频经过 native 压缩器后正常播放”的证据——
UI 报的是 `READY`（未观察到 `ACTIVE`），无逐块日志/波形/录音，`nativeProcess` 是否被真实播放线程调用、
返回值是否正确均未验证；JVM 测试使用 `FakeCompressor`，**不等于实测 JNI**。没有音频输出录音、波形/电平、
听感、多格式/多路由、进程重建、30 分钟长稳；没有 true peak 或 limiter 结论。用户明确**长时间真机测试暂缓**。
第二十/二十一轮证据对应**旧 Kotlin −6 dB 链路**，不能移用。验收“真实音频通过 DSP 后正常播放”未满足，
故仍 `IN_PROGRESS`，不标 `DONE`/`VERIFIED`。原声景引擎未改，不声称第三方媒体控制，也不声称 limiter/true peak。
**第二十三轮代码已随 `4b7ace3` 推送；第二十四轮日志/状态更新未提交**。

---

## DG-023 Android 基础 UI

**目标**（`docs/ROADMAP.md:211-220`）：主界面、文件选择、播放控制、自然模式开关、Bypass、响度显示和错误提示。
**验收**：完成完整本地播放闭环。

**交付物（第十九轮写入；第二十二轮更新开关文案；第二十三轮修正可用性）**：`DreamGuardScreen.kt` 新增 `LocalAudioCard`——状态（未选择/播放中/已暂停）、
文件名、格式协商提示、“选择文件/更换文件”、“暂停/播放”、“关闭”、`处理（本地压缩）` 开关
（副标题“开启时 PCM16 经 native 压缩器（−6 dB 补偿）；关闭时逐字节旁路”）；开关在文件协商为 16-bit PCM
且 native 压缩器可用（1–2 声道、库可加载、无失败）时可开；**已开启时始终可关闭**。格式提示在开关关闭时
也会明示“多声道不支持压缩，直通 / native 库不可用，直通 / 处理失败，直通”；开启并实际处理时显示
“本地压缩运行中/就绪”。
错误直接复用现有 `playback.error` 文案（本地错误前缀“本地音频播放失败：”）。
本地模式禁用环境音按钮并显示“本地音频使用中 · 关闭后恢复环境音轨”。底部说明改为
“无网络 / 麦克风 / 存储权限 · 本地文件经系统选择器读取”。

**第二十轮实际验证（主代理在 API 35 x86_64 模拟器执行）**：手工走通“选择文件 → 显示文件名与
`16-bit PCM · 44100 Hz · 1 ch` → 播放中”；处理开关打开后 `checked=true`；暂停显示已暂停，恢复后播放；
`dumpsys media_session` 在播放/恢复时为 `PLAYING`；无 `AndroidRuntime FATAL`。

**未执行 / 边界**：第二十三轮只构建 Debug APK、未安装/未运行；**第二十四轮已安装运行并在 UI 看到“播放中 +
本地压缩就绪”、开关 `checked=true`**，但仍没有设备截图比对、真实听感（模拟器无音频输出）、错误路径/进程重建
测试，也没有真机验证；“自然模式/响度显示”仍是后续项（本轮的开关是处理 bypass，不是自然模式）。
验收“完整本地播放闭环”只在模拟器 UI 层面部分满足，故仍 `IN_PROGRESS`。

**明确不声称**：本任务（基础 UI）原不包含第三方媒体音量控制；第二十五轮新增的 DG-024 系统媒体音量卡是独立范围，
第二十六轮已完成构建与 **92/0** JVM 测试、并做了模拟器基础音量验证，但尚无第三方播放器/真机证据（见下节）；
不把该卡计入本任务验收，也不声称任何第三方音频捕获、按应用分流或动态压缩。

---

## DG-024 第三方媒体音量控制原型

**目标**（`docs/ROADMAP.md`）：第三方媒体应用播放时，用户可在 DreamGuard 查看并调节系统媒体音量，
且不因打开控制区而启动/暂停第三方播放器或申请音频焦点。

**第二十五轮交付物（2026-11-10 写入；本会话未编译、未运行）**

- `app/src/main/java/com/dreamguard/app/audio/SystemMediaVolume.kt`：纯 Kotlin 控制核心，无 Android 类型。
  - `MediaVolumeBackend` 抽象读/写；`SystemVolumeSnapshot` 携带 `isAvailable` / `isFixed` / `minIndex` /
    `maxIndex` / `currentIndex`，`isAdjustable` 只在“可读 + 非固定 + `max > min`”时为真；
    `fraction` / `percent` 按平台真实索引范围计算，不假设固定档数。
  - `SystemVolumeController.setIndex` 把请求钳制到 `[min, max]`；固定设备返回 `FIXED` 且**不写**；
    范围不可读或单级输出返回 `UNAVAILABLE`；写入抛异常返回 `FAILED`；写后**重新读取**，
    平台静默忽略写入时返回 `FAILED` 而不是假成功。
  - `refresh()` 只在快照真正变化时通知监听器（外部音量键/系统面板/其他应用的变更与自身写入共用一条路径；
    重复的设置回调不会造成 UI 抖动）。
- `app/src/main/java/com/dreamguard/app/audio/AudioManagerMediaVolume.kt`：
  - `AudioManagerVolumeBackend` 只操作 **`STREAM_MUSIC`**：`getStreamVolume` / `getStreamMaxVolume` /
    `getStreamMinVolume`（API 28+，以下回退 0）/ `setStreamVolume(..., 0)` / `isVolumeFixed`。
    **未新增任何权限**（无 `WRITE_SETTINGS`、无录音、无网络）；**没有 `requestAudioFocus` 调用**。
  - `SystemVolumeObserver` 观察公开的 `content://settings/system` 表，用于硬件键/系统面板/其他应用
    引起的外部音量变化；注册失败只降级为“无实时外部回调”，不崩溃，界面在 ON_START 与每次写入后仍会重读。
- `app/src/main/java/com/dreamguard/app/ui/DreamGuardScreen.kt`：
  - 新增“系统媒体音量”卡：百分比、最小/最大索引、按平台档位分级的滑块、固定/单级/读失败/写失败文案与“重试”。
  - 文案明确“调节的是系统媒体音量通道（STREAM_MUSIC），会影响所有使用该通道的应用，不是只调 DreamGuard
    或某一个播放器；调整不会启动、暂停或抢占第三方播放器的音频焦点”。
  - 生命周期：`ON_START` 注册观察者并刷新，`ON_STOP` 反注册，`onDispose` 移除生命周期观察者、反注册、
    清空 listener；Composition 晚于 `ON_START` 时也立即补一次注册，避免漏掉生命周期事件。
  - 音量卡在播放服务尚未连接时也渲染，**不依赖 DSP/Media3 播放**；打开应用不会自动开始播放。
  - 原 DSP 增益卡标题从“音量”改为“声景音量”，避免与系统媒体通道混淆。
- `app/src/test/java/com/dreamguard/app/audio/SystemMediaVolumeTest.kt`：**12 个 JVM 测试**，覆盖
  范围读取/越界回读钳制/写入钳制、固定音量设备只读且不写、单级输出、读失败降级、写异常、
  平台静默忽略写入、外部变化只通知一次且空刷新不通知、成功写入发布新值、失败写入不发布假值、读失败恢复。

**第二十六轮执行证据（主代理在本机 / API 35 x86_64 模拟器执行；本会话只登记，未运行命令）**

- 构建与测试：`.\gradlew.bat --no-daemon --offline :app:testDebugUnitTest :app:assembleDebug`
  → **BUILD SUCCESSFUL**；测试报告 **92 tests / 0 failures / 0 skipped**。
- 模拟器：安装并启动成功；**未启动 DreamGuard 声景**，页面待机、系统媒体音量 **33%（5/15）**；
  用“系统媒体音量”卡滑块调到 **20%（3/15）**，`dumpsys audio` 记录
  `setStreamVolume STREAM_MUSIC index 3 oldIndex 5 from com.dreamguard.app`，页面显示 **20%**。
- **能证明**：Debug 构建与 JVM 测试通过；不播放 DreamGuard 时音量卡能读取并写入真实 `STREAM_MUSIC`
  通道；系统音频服务接受了这次写入（oldIndex 5 → index 3）；界面与系统索引一致。
- **不能证明**：第三方播放器共存或不被打断、实际声音电平/听感变化、外部音量变化同步
  （模拟器上用 adb 调节没有生效，未可靠验证）、音量边界与固定音量/单级输出 UI、蓝牙/固定输出设备、
  iQOO Z9T 真机、音频焦点行为；也不证明原生音频被实时压缩或只影响单个应用。

**已执行命令（主代理；供复核）**

```powershell
.\gradlew.bat --no-daemon --offline :app:testDebugUnitTest :app:assembleDebug
adb install -r app\build\outputs\apk\debug\app-debug.apk
adb shell am start -n com.dreamguard.app/.MainActivity
adb shell dumpsys audio | findstr /i "STREAM_MUSIC"
```

**下一轮需要补的验证**

- 让指定第三方播放器持续播放，拖动音量卡确认系统音量与实际声音变化、播放器不暂停；记录播放器及版本。
- 用音量键/系统面板/其他应用改变音量，确认真实设备上界面跟随（本轮 adb 调节未生效，不能据此判定）。
- 覆盖 0/最大值、固定音量或单级输出、蓝牙/有线/外放；最后在 iQOO Z9T 真机执行 DG-026。
- 对“未申请音频焦点”做设备侧记录（本次未记录 focus stack 检查结果）。

**回归边界**：本轮**未修改** `PlaybackService`、`PlaybackController`、DSP、`LocalAudioProcessor` 或
播放服务默认行为；改动是新增文件、在 `DreamGuardScreen` 插入音量卡（服务未连接分支也放一张）、把原 DSP
增益卡标题“音量”改成“声景音量”。主代理已跑 **92/0** 并完成一次“安装启动 + 不播放时调音量”的冒烟，
但**没有**启动 DreamGuard 声景、没有本地音频/DSP 播放冒烟，因此“现有播放不回归”仍只由 JVM 测试支持，
不能声称已在设备上回归。

**状态**：`IN_PROGRESS`。构建与 JVM 测试已通过，音量卡也在模拟器上完成了 5/15→3/15 的基础读写验证；
但路线图验收要求的第三方播放器共存、实际声音一致、边界/不可调状态、外部音量同步与 iQOO Z9T 真机
仍未完成，故不标 `DONE` / `VERIFIED`。

**明确不声称**：没有实现第三方音频捕获（`AudioPlaybackCapture` / `MediaProjection`）、没有按应用
分流音量、没有动态压缩或响度保护，也不声称原生音频已被实时压缩；DG-024 只控制系统 `STREAM_MUSIC`
通道，必然同时影响其他媒体应用。

**提交状态**：第二十五/二十六轮代码与文档更新**尚未提交、未推送**；用户要求本轮完成后暂停，提交与
推送由主代理后续同步。

---

## 第二十四轮：模拟器安装当前 Debug APK（播放状态与本地压缩就绪）

**日期**：2026-11-10　**代码 Commit**：`4b7ace322da31f06417f3dc09a591795ba14207f`（短 `4b7ace3`，已推送）；
**本日志/状态更新**：未提交

**执行（主代理，API 35 x86_64 模拟器）**

- 安装并启动当前 Debug APK。
- 系统文件选择器选择合成的 **40 s / 44.1 kHz / 单声道 PCM16 WAV**。
- UI 显示“播放中”；格式提示出现“本地压缩就绪”；打开处理开关后 `checked=true`。
- `MediaSession` 状态为 `PLAYING`；应用进程存活。
- `logcat` 过滤 `AndroidRuntime:E`：未见 `FATAL EXCEPTION` / `UnsatisfiedLinkError`。

**能证明什么**

- 当前 Debug APK 在 API 35 x86_64 模拟器可安装、启动，能通过系统选择器打开本地 PCM16 WAV 并进入播放状态。
- “本地压缩就绪”对应 `NativeState.READY`：native 库加载与 native 句柄创建路径没有报错
  （库缺失会显示“native 库不可用，直通”，创建失败会显示“处理失败，直通”）。
- `MediaSession PLAYING`、进程存活、无 FATAL/UnsatisfiedLinkError 是应用层/日志层的健康信号。

**不能证明什么**

- 不证明 `queueInput` 每个 block 都调用了 `nativeProcess`、不证明 JNI 返回值正确，也不证明输出字节被正确写入
  AudioTrack。
- 不证明实际音频输出电平、听感、延迟/underrun、资源泄漏（ASan/LSan）；没有录音、波形或逐块日志。
- “本地压缩就绪”是 UI 最近一次派发的状态；音频线程之后进入 `ACTIVE` 不会触发重组，因此不能把该文案当作
  逐块处理成功的证据。
- 未在 iQOO Z9T 真机验证新版压缩；用户要求延后真机与 30 分钟长稳测试，本轮未做。

**状态**：DG-021/022/023 仍 `IN_PROGRESS`；DG-020/013 仍 `DONE`，不标 `VERIFIED`。

---

## 第二十三轮：media3 契约修正、构建与原生测试实测（DG-020 / DG-022）

**日期**：2026-11-10　**Commit**：`4b7ace322da31f06417f3dc09a591795ba14207f`（短 `4b7ace3`，已推送）

**范围**：用户要求先完成开发再真机测试。本轮对第二十二轮未编译的代码做静态复核与必要修正、补测试，
并由主代理实际执行 Debug 构建、JVM 测试与模拟器 native 测试；**仍不运行 APK 播放**（用户明确长时间真机测试暂缓）。

**发现并修正**

1. **media3 1.5.1 API 契约错误（会编译失败）**：官方 `BaseAudioProcessor.onFlush()` 返回 `void`，不是
   `Boolean`。第二十二轮的 `override fun onFlush(): Boolean` 已改为 `override fun onFlush()`。同时确认
   `flush()` 由基类先清空待输出、把 pending format 写入 `inputAudioFormat`；测试里“flush 后 output 为空”
   是基类行为，不依赖 `onFlush` 返回值。
2. **句柄生命周期与 configure/flush 契约**：media3 允许 `configure()` 之后、`flush()` 之前继续排队旧格式
   buffer。原实现在 `onConfigure()` 立即 destroy 旧句柄并 create 新句柄，会在这段时间用新句柄处理旧格式
   音频。现改为：`onConfigure()` 只记录 pending 格式/协商结果；`onFlush()` 才按生效格式创建/替换句柄，
   同格式 flush 只 reset。新增 JVM 断言“configure 后未 flush 时旧句柄仍在用”。
3. **失败后 UI 死角**：`nativeCapable` 现在包含 `nativeError == null`；新增 `layoutSupported` /
   `nativeLibraryAvailable` / `nativeFailure`。格式提示在开关关闭时也明示直通原因；Switch 在已开启时
   即使 `processingAvailable=false` 也可关闭，避免处理失败后用户无法关掉压缩。
4. **回退完整性**：native 返回非预期字节数时同时回卷输出与输入 position；`close()` 置
   `processingEnabled=false`，终止处理，迟到的 UI 回调不会重新打开 native 路径。
5. **测试补强**：C++ 24→27 组（bypass 不改变压缩器状态、立体声残帧、输出容量大于输入）；C 头兼容
   已含 PCM16 纯 C 段（随 Debug 构建编译，未单独报告运行结果）；JVM 17→18 个（失败后仍可关闭处理），
   configure 相关用例改为 `configure → flush` 的真实契约。
6. **主代理交叉编译反馈（同一轮补丁）**：`dg_compressor_process` 的第三个参数在 `pcm16_processor.cpp`
   里按 `dg_pcm_buffer` 传入，但 `compressor.h` 声明为 `dg_pcm_view`，导致无匹配函数、编译中断。已把
   compressor API 的输出参数改为 **`dg_pcm_buffer`**（与 `dg_limiter_process`、`dg_pcm_*` 的 writable
   约定一致），删除 `compressor.cpp` 与 `limiter.cpp` 里残留的 `const_cast`，并同步 `test_compressor.cpp`
   （`mutable_view_of` → `mutable_buffer_of`，输出描述符改为 `dg_pcm_buffer`）与 `test_header_c_compat.c`。
   这是**编译期类型修正，不改变算法或行为**。修复后主代理重跑 `dg_compressor_tests`：**348 checks / 0 failures**
   （API 35 x86_64 模拟器），把 DG-013 的证据对到了当前签名；`dg_header_c_compat` 本轮未单独报告结果。

**实测结果（主代理执行）**

- `.\gradlew.bat --no-daemon --offline :app:testDebugUnitTest :app:assembleDebug` **成功**：
  **80 JVM tests / 0 failures / 0 skipped**；Debug NDK **三个 ABI（arm64-v8a / armeabi-v7a / x86_64）** 编译通过。
- Android API 35 x86_64 模拟器交叉编译并运行：`dg_pcm16_processor_tests` **206 checks / 0 failures**（27 组）；
  `dg_compressor_tests` **348 checks / 0 failures**。
- 首轮 native 编译的 `dg_pcm_view` / `dg_pcm_buffer` 类型错误已修正并复测。

**未执行 / 边界（第二十三轮；第二十四轮补模拟器状态）**：第二十三轮未安装/未运行 APK；第二十四轮已在
模拟器安装运行到“播放中 + 本地压缩就绪”，但**没有**真实音频输出、波形/电平、听感、多格式/多路由、
进程重建、30 分钟长稳；JVM JNI 合约测试与 `FakeCompressor` mock **不等于实测 JNI**，`nativeProcess` 的
真实调用、direct buffer 行为、音频线程时序仍未验证；没有 true peak 或 limiter 结论。用户明确
**长时间真机测试暂缓**。第二十/二十一轮的“68 tests / 0 failures”与真机“正常”只对应旧 Kotlin −6 dB 链路，
不能移用。DG-022/023 保持 `IN_PROGRESS`，DG-020 保持 `DONE`、不标 `VERIFIED`。
**第二十三轮代码已随 `4b7ace3` 推送；第二十四轮日志/状态更新未提交**。

---

## 第二十二轮功能构建：本地 PCM16 → native compressor（DG-020 / DG-022）

**日期**：2026-11-10　**Commit**：未提交（用户要求不提交、不推送）

**范围**：按用户指示先完成功能构建，真机/听感测试后置；不改系统 ACL、不申请 `danger-full-access`、不做真机测试。

**改动**

- `dsp-core`：新增 `pcm16_processor.h/.cpp`（PCM16↔float↔DG-013 压缩器；bypass 用 `memmove`；512 帧定长
  scratch；残帧原样；饱和量化；无 lookahead、无逐样本分配）；`CMakeLists.txt` 源列表与
  `tests/CMakeLists.txt` 的 `dg_pcm16_processor_tests` 接线；新增 `test_pcm16_processor.cpp`（24 组）。
- `app`：`jni_bridge.cpp` 新增 `NativePcmProcessor` 5 符号；新增 `NativePcmProcessor.kt`、`PcmCompressor.kt`；
  `LocalAudioProcessor.kt` 改为 native 主路径 + 位精确回退 + `NativeState`/`nativeError` +
  flush/reset/format-change/close 句柄管理；`PlaybackService.kt` 状态文案与 `onDestroy` close；
  `DreamGuardScreen.kt` 开关改名“本地压缩”并改副标题。
- 测试：`LocalAudioProcessorTest.kt` 重写为 17 个 fake 后端测试；新增 `NativePcmProcessorContractTest.kt`（4 个）。
- 保留全部既有未提交改动与原文件；未提交、未推送、未删除任何用户文件/日志。

**测试与运行**：本会话命令环境仍在启动阶段失败（`SetNamedSecurityInfoW failed (Win32 5): grantWrite(E:\DreamGuard)`），
第二十二轮当时没有执行构建、C++ 测试、JVM 测试或设备安装；**该轮没有 checks/tests 数字，没有 APK，没有设备运行**。
第二十三轮已回填：**80 JVM tests / 0 failures / 0 skipped**、Debug 三 ABI、模拟器 `dg_pcm16_processor` **206/0**、
`dg_compressor` **348/0**；仍**未运行 APK 播放**。按用户指示由主代理独立构建与测试，结果以第二十三轮记录为准。

**状态认定**：DG-020 保持 `DONE`（新 JNI surface 只经静态契约测试核对，不标 `VERIFIED`）；DG-022 保持
`IN_PROGRESS`（验收“真实音频通过 DSP 后正常播放”及波形/听感/真机证据均未取得）；DG-021/023 不受本轮影响。
第二十一轮真机“正常”答复对应旧 Kotlin −6 dB 链路，**不得**移用为本轮 native 压缩器的证据。

**明确不声称**：不声称 native 压缩器已在真机/模拟器实际运行，不声称输出音质/响度/延迟已被测量或听感验证，
不声称 limiter 或 true peak 已完成，不声称实现第三方媒体控制。

---

## 第二十一轮真机验证（iQOO Z9T / V2352A / Android 16 API 36）

**日期**：2026-11-10　**Commit**：未提交（用户要求不提交、不推送）

**设备与安装**

| 项 | 值 | 来源 |
| --- | --- | --- |
| 设备 | iQOO Z9T，ADB `model` = `V2352A`，`manufacturer` = `vivo` | 主代理 ADB 读取 |
| 系统 | Android 16，API 36 | 主代理 ADB 读取 |
| APK | Debug `app-debug.apk`，`versionName 0.1.0`、`versionCode 1` | `app/build.gradle.kts:16-17` + 设备安装查询 |
| APK SHA256 | `3ABD75F122320CCE3A7BD1BD493FE6B0A867C1B8996296DD6E44621C8F4C97D2` | 主代理计算 |
| 安装 | `adb install` 成功 | 主代理记录 |
| 启动进程 | pid **22780** | 主代理记录 |
| 同批证据 | 该 APK 的构建与 **68 tests / 0 failures / 0 skipped** 的 JVM 结果同批（第二十轮） | 第二十轮记录 |

**用户操作与回答（2026-11-10，用户本人操作）**

1. 在 Downloads 用**系统文件选择器**打开主代理生成的 **40 s / 44100 Hz / 单声道 PCM16 WAV**；对“声音本身、−6 dB 开关打开后的变化、暂停/恢复、是否爆音/卡顿”的**合并询问**，用户答复“**正常**”。（逐项细节未展开，本记录不扩展。）
2. 锁屏后约 **15 秒**声音持续。
3. 播放其他媒体后，DreamGuard 暂停。
4. 关闭本地音轨后，原有 DSP 声景播放；调节音量/音色、暂停均正常，无爆音卡顿。

**日志检查（主代理执行）**

- 进程存活（pid 22780）。
- MediaSession 状态为 **`PAUSED`**。
- 过滤 `AndroidRuntime:E` 与 `nativeloader` 的日志中**未见** `FATAL EXCEPTION`、`UnsatisfiedLinkError`。
- **未取得明确的 nativeloader 成功行**（如 `Load ... libdreamguard_dsp.so ... ok`）；本记录以“关闭本地音轨后 DSP 声景实际出声”作为库已加载/可用的**间接证据**，不宣称有加载成功日志。

**状态认定（谨慎口径）**

- DG-021 的验收原文是“iQOO Z9T 本地音频正常播放”，本轮已由用户在目标真机上实际执行并确认；但交付物“播放进度”仍未实现，且多格式、多路由、进程重建、30 分钟长稳未测 ⇒ 仍 `IN_PROGRESS`。
- DG-022 的验收是“真实音频通过 DSP 后正常播放”；本轮 −6 dB 开关为 **Kotlin 整数增益**，`libdreamguard_dsp.so` 未接入，无波形/电平 ⇒ 仍 `IN_PROGRESS`。
- DG-023 的验收是“完整本地播放闭环”；主路径（选择 → 播放 → 处理开关 → 暂停/恢复 → 关闭回 DSP 声景）已在真机走通，但错误路径、进程重建、通知栏控制未测，“自然模式/响度显示”未做 ⇒ 仍 `IN_PROGRESS`。
- 三项均**不标** `DONE` / `VERIFIED`；本记录只把“已有真机用户听感/操作确认”作为新证据补上。

**证据边界（不得含糊）**

- “正常”是对四件事的**合并答复**，不能拆成四条独立、可量化的结论；本文件不据此声称具体失真度、延迟或响度指标。
- 未录音频、未测输出波形/电平/真实峰值，未做 A/B，未与商业产品对比。
- 未覆盖：30 分钟连续播放、蓝牙/有线耳机/扬声器路由切换、44.1 kHz 单声道 PCM16 之外的其他格式与采样率、系统主动回收/进程重建、来电打断、通知栏与锁屏媒体键控制。
- 未提交、未推送，代码一行未改；本轮改动只限文档。
- 旧“2026-11-10 真机基础回归”记录（锁屏约 20 s、`kill -9` 重开、环境音叠加）**继续有效并保留**，与本轮不是同一次测试。

---

## 环境记录（截至第二十五阶段）

1. 第一阶段：`pwsh` / `terminal_open` 在启动阶段失败，
   `Error: SetNamedSecurityInfoW failed (Win32 5): grantWrite(E:\DreamGuard)`，无法执行任何命令；
   期间只做了静态核对与文件写入，未做任何构建或测试。
2. 第二阶段：会话文件策略切到 `danger-full-access` 后命令恢复正常，第二轮全部构建与测试均在此时执行。
   后台任务仍有一条限制：`pwsh` 后台作业会因 `spawn ...\WindowsApps\pwsh.exe ENOENT` 失败，
   因此长命令改为前台执行（第二轮最长的构建 32s，无影响）。
3. 第三阶段：文件策略回到 `workspace-write` + 审批提示后，`pwsh` 再次在启动阶段报同一
   `SetNamedSecurityInfoW failed (Win32 5): grantWrite(E:\DreamGuard)`，命令体从未执行。
   因此第三轮全部改动只经过静态核对：**没有编译、没有测试、没有 APK**。
   用户已明确主代理可独立运行构建与测试，故未尝试任何 ACL/权限变更。
4. 第四阶段：本会话仍无法执行命令（再次实测同一错误），但**主代理在其本机实际执行**了
   第三轮代码的 `clean testDebugUnitTest assembleDebug assembleRelease`、NDK 交叉编译与模拟器 native 测试，
   结果见上面各节（58 JVM tests、1328 + 110 checks、C 头兼容通过）。
   该阶段对 PCM 别名/溢出的修正与 `settings.gradle.kts` 末尾空行修正只经过静态核对。
5. 第五阶段（当前）：主代理复测**别名修正后的最新代码**——Gradle 构建成功、三个 ABI 原生库构建通过、
   JVM 任务 `up-to-date`，NDK 交叉编译的 `dg_pcm_tests` 在 API 35 模拟器上 **139 checks / 0 failures**。
   本会话依然无法执行命令（同一 ACL 错误），第五轮只做记录更新，**没有修改任何代码**；
   `dg_lifecycle_tests`（1328）与 `dg_header_c_compat` 沿用此前未修改部分的结果，本轮未重跑。
6. 第六阶段（DG-012 首版代码）：本会话 `pwsh` 在启动阶段即失败——任何命令（包括 `pwd`）返回
   `Error: SetNamedSecurityInfoW failed (Win32 5): grantWrite(E:\DreamGuard)`，命令体从未执行；
   未尝试 ACL/权限修复（用户明确禁止），因此第六轮**只写代码与文档**：`meter`/`lufs` 两个头、两个实现、
   两组测试与全部台账更新都**没有在本会话编译过、运行过**。三 ABI 的构建兼容当时只有静态核对结论。
7. 第七/八阶段（DG-012 实机化，均由主代理在其本机执行，本会话只改代码/文档、命令环境始终不可用）：
   - 第七轮：NDK 交叉编译 x86_64 + API 35 模拟器首跑 → `dg_meter` 207/0、`dg_lufs` 227/2；
     本会话据此定位两处失败为 gating 用例的测试期望错误并修正 `test_lufs.cpp`（未改算法），新增一个反向回归。
   - 第八轮：修复后复测 → `dg_lufs` **238/0**、`dg_meter` **207/0**、扩展 C 头兼容 **PASSED**；
     `testDebugUnitTest assembleDebug assembleRelease` 成功，Debug/Release 三 ABI 全部构建通过，JVM `up-to-date`。
     本会话只更新文档，未改任何代码。仍未执行：宿主 `ctest`、真机、ASan/LSan、参考工具误差对比。
8. 第九阶段（DG-012 参考对照工具链）：本会话 `pwsh` 再次实测失败——`pwd` 即返回
   `Error: SetNamedSecurityInfoW failed (Win32 5): grantWrite(E:\DreamGuard)`，命令体从未执行；
  按用户要求未改 ACL、未做系统级安装。因此第九轮**只写工具源码与文档**：`dsp-core/tools/lufs_compare/`
  下三个文件、`dsp-core/CMakeLists.txt` 的 tools 选项、`dsp-core/README.md` 与本节记录都**没有编译过、运行过**；
  对照（ffmpeg `ebur128` 或 libebur128）**零次执行**。主代理本机 PATH 无 ffmpeg 的结论沿用第八轮。
9. 第十阶段（DG-013）：`pwsh` 仍以同一错误在启动阶段失败，未改 ACL、未安装任何东西。因此第十轮**只写代码与文档**：
   `dsp-core/include/dreamguard/compressor.h`、`dsp-core/src/compressor.cpp`、`dsp-core/tests/test_compressor.cpp`、
   CMake 两处接线、`test_header_c_compat.c` 的 DG-013 段、`dsp-core/README.md` 与本节记录**全部未编译、未运行**。
   DG-012 的对照工具链本轮同样未执行（状态不变）。
   该轮漏掉了一处注释块结尾 `*/`，是第十一阶段编译失败的直接原因（见下一条）。
10. 第十一阶段（DG-013 第一次真实编译，由主代理在其本机 NDK x86_64 执行）：编译**失败**——
    `compressor.h:161 warning: '/*' within block comment` + 多处 `use of undeclared identifier 'dg_compressor_process'`。
    本会话据此定位根因（第十轮合并注释时漏写 `*/`，声明被注释吞掉）并修复，复核了四个文件的注释闭合；
    **测试期望一字未改**。本会话 `pwsh` 仍不可用，因此修正后**未重测**；DG-012 对照同样仍未执行。
    结论：DG-013 仍为 `IN_PROGRESS`，等主代理重跑复现命令第 9 节。
11. 第十二阶段（DG-013 第二次真实运行，主代理 NDK x86_64 + API 35 模拟器）：编译通过，
    `dg_compressor_tests` 得 **334 checks / 2 failures**（`output_peak_db` 实得 −6.236768 vs 期望 −16.505150；
    硬拐点阈值处 `gain_reduction_db` 实得约 −9.7e-8 vs 期望 0）。本会话定位两处均为**测试期望错误**并改写：
    块峰值指标含 attack（第一块峰值 = 第一个 attack 步，解析式 −6.23677 与实测吻合到 6 位小数），
    浮点样本无法恰好落在 −20 dBFS（比阈值高 1.3e-7 dB）。实现 `compressor.cpp` **一行未改**；
    `compressor.h` 仅更新峰值字段注释语义，`dsp-core/README.md` 与本节记录同步。本会话 `pwsh` 仍不可用，
    修正后**未重跑**；DG-012 对照同样仍未执行（状态不变）。
12. 第十三阶段（两件真实执行，均由主代理在其本机完成；本会话 `pwsh` 仍报同一启动错误）：
    ①DG-013 重跑 **348 checks / 0 failures**，`dg_header_c_compat` PASSED，Gradle Debug/Release 三 ABI 构建成功、
    JVM `up-to-date` → 信号级验收达成，登记 `DONE` 但不标 `VERIFIED`。
    ②DG-012 的 `dg_lufs_compare --selfcheck` 首次执行，得 **113 checks / 1 failure**（电平阶跃安静段落在绝对门以下）。
    本会话定位为**工具信号生成缺陷**（`fill_sine` 乘 `c.peak`，把 −50 dB 当相对比例 → 实际 −70 dBFS，比 `test_lufs.cpp`
    的绝对 −50 dBFS 低 20 dB），改为绝对 −50 dBFS 并新增“安静段 `momentary` 解析值”检查，**待重跑**；
    ffmpeg/libebur128 对照仍零执行，DG-012 保持 `IN_PROGRESS`、LUFS `PROVISIONAL`。
13. 第十四阶段（主代理在 Android x86_64 + API 35 模拟器执行；本会话 `pwsh` 仍报同一启动错误、命令体从未执行）：
    - DG-012 自检修正后重跑 **115 checks / 0 failures**（113 → 115 的增量正是新增的安静段 `momentary` 与
      `isfinite` 检查），工具自检首次全绿；但它仍是“同源自证”，不等于与标准一致。
    - 条件变化：FFmpeg **9.0.2** 只在构建目录 `dsp-core/build/reference-deps/imageio_ffmpeg/binaries/` 内
      （非系统安装）；本机**无 Windows C++ 编译器**，原生对照路线不可行。
    - 本会话据此给 `compare_with_ffmpeg.ps1` 增加 `-Csv`/`-WavDir` **预生成模式**（读取 `adb pull` 回来的
      CSV + WAV；只读输入、不删文件、拒绝 `-CleanWav`），把静音/0.3 s 改成 `kind=undefined` 的**定义检查**
      （双方无值记 `SKIP` 且不计入通过；单边有值记 `FAIL`），报告新增 CSV/WAV 的 SHA256 与 `.csv` 副产物，
      ffmpeg 定位新增“构建目录内副本”兜底；同步更新 `dsp-core/tools/lufs_compare/README.md` 的 3a/3b 两节与
      运行历史、`dsp-core/README.md`、本节与 `docs/ROADMAP.md`。
    - 本会话**脚本一行未运行**（无 pwsh、无编译器、无模拟器），因此 DG-012 仍然没有任何对照数字：
      `reference-report.md` 不存在，状态保持 `IN_PROGRESS`、LUFS `PROVISIONAL`。
14. 第十五阶段（参考对照首次真实执行，由主代理在本机运行；本会话 `pwsh` 仍以同一错误在启动阶段失败，命令体从未执行）：
    - 主代理用第十四轮加的 `-Csv/-WavDir` 预生成模式，对 `adb pull` 回来的 16 个 WAV 调用 FFmpeg **9.0.2**
      `ebur128=peak=true`：**14 PASS / 2 FAIL / 0 SKIP**，最大绝对差 **0.049 LU**（10 kHz），整体 **exit 1**；
      报告与 `.csv` 副产物落在 `dsp-core/build/lufs-compare-android/`，本会话逐行读取核对。
    - 两项 FAIL 是 `kind=undefined` 的定义问题：我方 `integrated_ready=0` / `-inf`，ffmpeg 打印 **−70.0**。
      本会话按 FFmpeg 源码核实该值来源：`f_ebur128.c` 的 `#define ABS_THRES -70` 与 `init()` 里
      `ebur128->integrated_loudness = ABS_THRES;`，仅在 400 ms 块 ≥ 绝对门时被覆盖 ⇒ **−70.0 是“无合格块”的哨兵，
      不是测量值**。因此两项**保持未通过 / 待人工裁定**，不改判定规则、不改核心实现、不调容差。
    - 本会话只改文档（`docs/STATUS.md`、`docs/DEVLOG.md`、`docs/ROADMAP.md`、`dsp-core/README.md`、
      `dsp-core/tools/lufs_compare/README.md`），代码零改动；未提交、未推送、未改 ACL、未安装任何东西，
      原有 CSV/WAV/报告一律保留。
    - DG-012 仍 `IN_PROGRESS`/`PROVISIONAL`；DG-013 是独立测试链，保持 `DONE`（不 `VERIFIED`）、348/0 未重跑、无新证据。
15. 第十六至十九阶段（本会话 `pwsh` 与 `terminal_open` 始终在启动阶段失败：`Error: SetNamedSecurityInfoW failed
    (Win32 5): grantWrite(E:\DreamGuard)`；按用户要求未改 ACL、未提权、未安装）：
    - 第十六轮在本会话只改脚本（metadata 高精度路径 + probe 修复），未运行；主代理随后执行，第十八轮产出
      `dsp-core/build/lufs-compare-android/reference-report-metadata.md`：14 可比样本 14 PASS / 0 FAIL、
      worst 0.010 LU，2 例 NO-DATA 单列；数字由本会话逐行核对报告与 CSV，不是本会话跑出来的。
    - 第十九轮本会话只写代码/文档（本地音频选择、播放/暂停/恢复、`LocalAudioProcessor`、
      `LocalAudioRenderersFactory`、最小 UI、10 个 JVM 测试），**未编译、未跑测试、未安装、未播放**；
      所有“实现”陈述均为源码静态核对，DG-021/022/023 只能记 `IN_PROGRESS`。
    - 第二十轮由主代理在本机执行命令：`testDebugUnitTest --rerun-tasks :app:assembleDebug` 成功
      （**68 tests / 0 failures / 0 skipped**）；API 35 x86_64 模拟器安装启动，系统选择器从 Downloads
      选 40 s / 44.1 kHz / 单声道 PCM16 WAV，UI 文件名、`16-bit PCM · 44100 Hz · 1 ch`、播放中，
      MediaSession `PLAYING`，−6 dB 开关 `checked=true`，暂停/恢复通过，无 `AndroidRuntime FATAL`。
      模拟器无音频输出，未听感、未真机、未录波。本会话仍无法执行命令（同一 ACL 错误），本轮只改文档、
      未改代码；证据均为主代理执行记录。
16. 第二十一阶段（真机验证）：本会话 `pwsh` 仍以同一错误在启动阶段失败
    （`Error: SetNamedSecurityInfoW failed (Win32 5): grantWrite(E:\DreamGuard)`），未改 ACL、未提权、未安装任何东西；
    因此本轮**没有执行任何命令**，也没有重新构建 APK。以下事实来自主代理 2026-11-10 在
    iQOO Z9T（`V2352A` / `vivo` / Android 16 API 36）的安装、启动与用户操作记录，以及主代理的日志检查；
    本会话只做文档登记与逐条核对（版本号已对 `app/build.gradle.kts:16-17` 复核为 `versionCode 1` / `versionName 0.1.0`）。
    未提交、未推送，代码一行未改；旧的真机记录保留。
17. 第二十二阶段（DG-022 功能构建）：本会话 `pwsh` 仍以同一错误在启动阶段失败
    （`Error: SetNamedSecurityInfoW failed (Win32 5): grantWrite(E:\DreamGuard)`），所有命令（含 `pwd`）均未进入命令体。
    按用户要求**未修改 ACL、未申请 `danger-full-access`、未安装任何东西、未做真机测试**；本轮只做源码/测试/文档写入，
    C++ 桥、JNI、Kotlin 处理器与两组测试**全部未编译、未运行**。构建与测试由主代理独立执行。
18. 第二十五阶段（DG-024 系统媒体音量控制）：本会话 `pwsh` 与 `terminal_open` 仍以同一错误在启动阶段失败
    （`Error: SetNamedSecurityInfoW failed (Win32 5): grantWrite(E:\DreamGuard)`），未改 ACL、未申请
    `danger-full-access`、未安装任何东西；因此新代码与 12 个 JVM 测试**未编译、未运行**，没有 APK、模拟器或
    真机证据。本轮只做源码/测试/文档写入；构建、单测与设备验证命令见“DG-024”节与 README，后由主代理执行。
19. 第二十六阶段（DG-024 主代理实测 + 文档回填）：主代理在本机执行
    `.\gradlew.bat --no-daemon --offline :app:testDebugUnitTest :app:assembleDebug` → **BUILD SUCCESSFUL**，
    测试报告 **92 tests / 0 failures / 0 skipped**；在 API 35 x86_64 模拟器安装启动，未启动 DreamGuard
    声景时系统媒体音量 33%（5/15），用音量卡调到 20%（3/15），`dumpsys audio` 记录
    `setStreamVolume STREAM_MUSIC index 3 oldIndex 5 from com.dreamguard.app`，页面显示 20%。
    本会话（第二十六阶段）**只更新文档，未运行命令、未改代码**；第三方播放器共存、外部音量同步
    （模拟器 adb 调节未生效）、固定输出/蓝牙/真机均未验证。第二十五/二十六轮更新**未提交、未推送**；
    用户要求本轮完成后暂停，提交与推送由主代理后续同步。

## 复现命令（第二至第五轮实际使用；第六至第二十三轮按标注部分执行）

```powershell
# 0) 结构迁移后先清理旧 ninja 规则（旧 app/.cxx 指向已移走的 dsp_core.cpp）
.\gradlew.bat clean

# 1) Android 构建 + JVM 单测（DG-003）
.\gradlew.bat --no-daemon --stacktrace clean testDebugUnitTest assembleDebug

# 2) 强制重跑单测（跳过 build cache；第二轮用它拿到真实 58/0 报告）
.\gradlew.bat --no-daemon :app:testDebugUnitTest --rerun-tasks

# 3) native 生命周期测试：有宿主 C++ 编译器的机器走这条
.\gradlew.bat :dsp-core:dspCoreTest

# 4) 本机替代路径（无宿主编译器）：NDK 交叉编译 + 模拟器执行
$ndk = "E:\AgentImeTest\sdk\ndk\27.0.12077973\toolchains\llvm\prebuilt\windows-x86_64\bin"
& "$ndk\x86_64-linux-android26-clang++.cmd" -std=c++20 -O1 -fPIE -pie -static-libstdc++ `
    -I dsp-core\include -o dsp-core\build\android-test\dg_lifecycle_tests `
    dsp-core\src\dg_engine.cpp dsp-core\src\soundscape.cpp dsp-core\tests\test_dg_lifecycle.cpp
& "$ndk\x86_64-linux-android26-clang.cmd" -std=c99 -O1 -I dsp-core\include -c `
    -o dsp-core\build\android-test\c_compat.o dsp-core\tests\test_header_c_compat.c
& "$ndk\x86_64-linux-android26-clang++.cmd" -std=c++20 -fPIE -pie -static-libstdc++ -I dsp-core\include `
    -o dsp-core\build\android-test\dg_header_c_compat dsp-core\build\android-test\c_compat.o `
    dsp-core\src\compressor.cpp dsp-core\src\dg_engine.cpp dsp-core\src\lufs.cpp dsp-core\src\meter.cpp `
    dsp-core\src\pcm.cpp dsp-core\src\pcm16_processor.cpp dsp-core\src\soundscape.cpp
# 第二十二轮补：`pcm16_processor.cpp` 必须进这条链接行——DG-022 之后 C 头兼容程序会调用
# `dg_pcm16_processor_*`，漏掉它同样只会得到 undefined reference。
# 第十一轮补：`compressor.cpp` 必须进这条链接行——DG-013 之后 C 头兼容程序会调用
# `dg_compressor_*`，漏掉它就只会得到 undefined reference（不是测试失败，是链接错误）。
# 第三轮新增：PCM 助手测试（DG-011）
& "$ndk\x86_64-linux-android26-clang++.cmd" -std=c++20 -O1 -fPIE -pie -static-libstdc++ `
    -I dsp-core\include -o dsp-core\build\android-test\dg_pcm_tests `
    dsp-core\src\pcm.cpp dsp-core\tests\test_pcm.cpp
$adb = "E:\AgentImeTest\sdk\platform-tools\adb.exe"
& $adb push dsp-core\build\android-test\dg_lifecycle_tests /data/local/tmp/
& $adb push dsp-core\build\android-test\dg_header_c_compat /data/local/tmp/
& $adb push dsp-core\build\android-test\dg_pcm_tests /data/local/tmp/
& $adb shell "chmod 755 /data/local/tmp/dg_lifecycle_tests /data/local/tmp/dg_header_c_compat /data/local/tmp/dg_pcm_tests"
& $adb shell /data/local/tmp/dg_lifecycle_tests
& $adb shell /data/local/tmp/dg_header_c_compat
& $adb shell /data/local/tmp/dg_pcm_tests

# 5) 端到端冒烟（DG-003）
& $adb install -r app\build\outputs\apk\debug\app-debug.apk
& $adb shell am start -n com.dreamguard.app/.MainActivity
# 播放键在 1080x2340 上位于 (540, 850)
& $adb shell input tap 540 850
& $adb shell dumpsys media_session | Select-String "com.dreamguard|PlaybackState"

# 6) 可选：泄漏检测（DG-010 验收第 2 条，至今完全未做）
cmake -S .\dsp-core -B .\dsp-core\build\asan -DCMAKE_BUILD_TYPE=Debug `
      -DDREAMGUARD_DSP_BUILD_TESTS=ON -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined"
cmake --build .\dsp-core\build\asan --config Debug
ctest --test-dir .\dsp-core\build\asan --output-on-failure -C Debug

# 7) DG-012 计量 / LUFS（第六轮新增；7b 的 x86_64 部分与 7c 已在第七/八轮执行，7a 未执行）

# 7a) 宿主 CMake：注册了 dg_meter、dg_lufs 两个 CTest
cmake -S .\dsp-core -B .\dsp-core\build\host -DDREAMGUARD_DSP_BUILD_TESTS=ON
cmake --build .\dsp-core\build\host --config Debug
ctest --test-dir .\dsp-core\build\host --output-on-failure -C Debug

# 7b) 无宿主编译器时的 NDK 交叉编译 + 模拟器路径
& "$ndk\x86_64-linux-android26-clang++.cmd" -std=c++20 -O1 -fPIE -pie -static-libstdc++ `
    -I dsp-core\include -o dsp-core\build\android-test\dg_meter_tests `
    dsp-core\src\meter.cpp dsp-core\tests\test_meter.cpp
& "$ndk\x86_64-linux-android26-clang++.cmd" -std=c++20 -O1 -fPIE -pie -static-libstdc++ `
    -I dsp-core\include -o dsp-core\build\android-test\dg_lufs_tests `
    dsp-core\src\lufs.cpp dsp-core\tests\test_lufs.cpp
& $adb push dsp-core\build\android-test\dg_meter_tests /data/local/tmp/
& $adb push dsp-core\build\android-test\dg_lufs_tests /data/local/tmp/
& $adb shell "chmod 755 /data/local/tmp/dg_meter_tests /data/local/tmp/dg_lufs_tests"
& $adb shell /data/local/tmp/dg_meter_tests
& $adb shell /data/local/tmp/dg_lufs_tests

# 7c) 三 ABI 构建（DG-012 源码随 dreamguard_dsp_core 进入每个 ABI）
.\gradlew.bat --no-daemon assembleDebug assembleRelease
# 检查 app\build\intermediates\cxx\Debug\*\*\obj\{arm64-v8a,armeabi-v7a,x86_64}\libdreamguard_dsp.so 均存在。

# 8) DG-012 参考工具对照（第九轮新增；第十四轮新增 8c-2 预生成模式；本会话一条都没执行）
# 8a) 构建对照工具（host，需宿主 C++20 编译器；没有宿主编译器时先看 8c-2）
cmake -S .\dsp-core -B .\dsp-core\build\host `
      -DDREAMGUARD_DSP_BUILD_TESTS=ON -DDREAMGUARD_DSP_BUILD_TOOLS=ON
cmake --build .\dsp-core\build\host --config Release

# 8b) 工具自检（不需要 ffmpeg）：分块不变性、稳态正弦解析值、mono-stereo 差 3.0103 LU、
#     静音/0.3 s 未就绪、门控 67/60/40、安静段 momentary 解析值、WAV 容器字段
#     现状：第十三轮 113/1（工具缺陷）→ 修正 → 第十四轮 115/0（已全绿）
ctest --test-dir .\dsp-core\build\host -R dg_lufs_compare --output-on-failure -C Release
#   或：.\dsp-core\build\host\tools\lufs_compare\Release\dg_lufs_compare.exe --selfcheck

# 8c) 与 ffmpeg ebur128 对照（原生模式；容差 0.1 LU，超差即 FAIL，脚本不会放宽）
#     前置：ffmpeg 在 PATH、或传 -Ffmpeg、或放在 dsp-core\build\reference-deps 下（脚本会自动兜底）
#     第十四轮确认：E:\DreamGuard\dsp-core\build\reference-deps\imageio_ffmpeg\binaries\ffmpeg-win-x86_64-v9.0.2.exe
& .\dsp-core\build\reference-deps\imageio_ffmpeg\binaries\ffmpeg-win-x86_64-v9.0.2.exe `
    -hide_banner -filters | Select-String ebur128
powershell -NoProfile -ExecutionPolicy Bypass -File `
    .\dsp-core\tools\lufs_compare\compare_with_ffmpeg.ps1

# 8c-2) 无 Windows C++ 编译器时的预生成模式（第十四轮新增；推荐路线）
#   NDK 编译 Android 版工具 → 模拟器上生成 CSV + 16 个 WAV → adb pull → 本机只跑 ffmpeg 侧
& "$ndk\x86_64-linux-android26-clang++.cmd" -std=c++20 -O2 -fPIE -pie -static-libstdc++ `
    -I dsp-core\include -o dsp-core\build\android-test\dg_lufs_compare `
    dsp-core\tools\lufs_compare\dg_lufs_compare.cpp dsp-core\src\lufs.cpp dsp-core\src\pcm.cpp
& $adb push dsp-core\build\android-test\dg_lufs_compare /data/local/tmp/
& $adb shell "chmod 755 /data/local/tmp/dg_lufs_compare"
& $adb shell "mkdir -p /data/local/tmp/lufs-compare"
& $adb shell "/data/local/tmp/dg_lufs_compare --selfcheck"
& $adb shell "cd /data/local/tmp && ./dg_lufs_compare --wav-dir lufs-compare --csv lufs-compare/ours.csv"
& $adb pull /data/local/tmp/lufs-compare dsp-core\build\lufs-compare-android
powershell -NoProfile -ExecutionPolicy Bypass -File `
    .\dsp-core\tools\lufs_compare\compare_with_ffmpeg.ps1 `
    -Csv .\dsp-core\build\lufs-compare-android\ours.csv `
    -WavDir .\dsp-core\build\lufs-compare-android
# 产出：<输入目录>\reference-report.md（模式、CSV 与每个 WAV 的 SHA256、每例 kind/ours/ffmpeg/delta/status、
#       最大偏差及其用例、结论）+ 同名 .csv 副产物
# 退出码：0 全过（且至少一个 PASS）/ 1 有未通过项或无可比项 / 2 缺工具或 CSV/WAV / 3 缺 ffmpeg 或跑不起来
# 语义：静音与 0.3 s 标 kind=undefined，双方无值记 SKIP 且不计入通过；单边有值记 FAIL
# 输入保护：预生成模式只读，不删 CSV/WAV；-CleanWav 在该模式被拒绝
# 第十五轮**已实际执行本条**：16 例 → 14 PASS / 2 FAIL / 0 SKIP，最大绝对差 0.049 LU，整体 exit 1；
#   2 项 FAIL 为 kind=undefined（静音、0.3 s：我方 -inf/ready=0，ffmpeg 报绝对门哨兵 -70.0），保持未通过；
#   注意 ffmpeg 汇总只打印 0.1 LU（%5.1f），故 0.02–0.05 LU 的偏差在取整噪声内，需更高精度参考才能定论
# 第十八轮**已实际执行 metadata 模式**（脚本现在的默认路径：`ebur128=metadata=1` + `ametadata=print`，
#   读 `lavfi.r128.I`；见第 8 节脚本 32/487/515 行）：14 可比样本 14 PASS / 0 FAIL、worst 0.010 LU，
#   静音与 0.3 s 判 NO-DATA（单列、不计入 PASS）；报告
#   dsp-core\build\lufs-compare-android\reference-report-metadata.md。有效分辨率约 0.01 LU。

# 8d) 可选：libebur128 进程内第二参考（需 ebur128.h + 库）
cmake -S .\dsp-core -B .\dsp-core\build\ebur128 `
      -DDREAMGUARD_DSP_BUILD_TOOLS=ON -DDREAMGUARD_LUFS_COMPARE_EBUR128=ON
cmake --build .\dsp-core\build\ebur128 --config Release
.\dsp-core\build\ebur128\tools\lufs_compare\Release\dg_lufs_compare.exe --libebur128 --tolerance 0.1

# 9) DG-013 压缩器（第十轮新增；第十一轮首次真实编译失败并已修，修正后待重测）
# 9a) host：构建并跑 CTest（dg_compressor + 重新编译的 C 头兼容）
cmake -S .\dsp-core -B .\dsp-core\build\host `
      -DDREAMGUARD_DSP_BUILD_TESTS=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build .\dsp-core\build\host --config Debug
ctest --test-dir .\dsp-core\build\host -R "dg_compressor|dg_header_c_compat" --output-on-failure -C Debug
#   或直接：.\dsp-core\build\host\tests\Debug\dg_compressor_tests.exe
#   期望：末行 "  N checks, 0 failures"，进程退出码 0

# 9b) 无宿主编译器时的替代路径（NDK 交叉编译 + 模拟器，与第 4 步同法）
& "$ndk\x86_64-linux-android26-clang++.cmd" -std=c++20 -O1 -fPIE -pie -static-libstdc++ `
    -I dsp-core\include -o dsp-core\build\android-test\dg_compressor_tests `
    dsp-core\src\compressor.cpp dsp-core\src\pcm.cpp dsp-core\tests\test_compressor.cpp
& $adb push dsp-core\build\android-test\dg_compressor_tests /data/local/tmp/
& $adb shell "chmod 755 /data/local/tmp/dg_compressor_tests"
& $adb shell /data/local/tmp/dg_compressor_tests
#   注意：9b 只链接 compressor/pcm；C 头兼容二进制（第 4 步）已在第十一轮把
#   dsp-core\src\compressor.cpp 加进链接行，直接重跑第 4 步即可。

# 10) 第十九/二十轮：本地音频 + Media3 AudioProcessor（**第二十轮已由主代理执行**；本会话仍无法运行）
# 10a) 编译 + 既有 JVM 单测 + 新增 10 个 LocalAudioProcessorTest（同一命令会执行全部 JVM 测试）
.\gradlew.bat --no-daemon :app:testDebugUnitTest --rerun-tasks
#   期望：BUILD SUCCESSFUL；若新增测试失败，先看是否是 media3 AudioFormat/Util 的 JVM 依赖问题，
#   不得通过删测试或降低断言来“修好”。
.\gradlew.bat --no-daemon assembleDebug
#   第二十轮实际执行（主代理）：BUILD SUCCESSFUL；68 tests / 0 failures / 0 skipped
#   （既有 58 + 新增 LocalAudioProcessorTest 10），assembleDebug 成功。
& $adb install -r app\build\outputs\apk\debug\app-debug.apk
& $adb shell am start -n com.dreamguard.app/.MainActivity
# 10b) 手工闭环（第二十轮主代理在 API 35 x86_64 模拟器执行；本会话未做）：
#   “本地音频”→“选择文件”→系统选择器选一个 WAV/MP3 → 应显示文件名与“16-bit PCM · 采样率 · 声道”，
#   自动播放；“暂停/播放”应可往返；打开“处理（−6 dB）”后同一文件应明显变轻，关闭后恢复；
#   “关闭”应回到内置 dream_loop / 环境音轨，且处理开关复位；错误路径看“本地音频播放失败：…”文案。
#   第二十轮实测：Downloads 选 40 s 44.1k mono PCM16 WAV → UI 文件名 + “16-bit PCM · 44100 Hz · 1 ch” +
#   播放中，MediaSession PLAYING；−6 dB 开关 checked=true；暂停显示已暂停、恢复后 PLAYING；无 FATAL。
#   未做：“关闭”回内置轨、错误路径、听感对比；模拟器无音频输出，未在 iQOO Z9T 上执行。
#   因此本段只证明模拟器 UI/会话状态闭环，不当作完整验收。

# 11) 第二十一轮：真机（iQOO Z9T）验证（主代理执行；本会话未运行任何命令）
#    前置：设备已连接（ADB model V2352A / manufacturer vivo / Android 16 API 36），APK 已 `adb install` 成功；
#    已记录：versionName 0.1.0 / versionCode 1 / SHA256 3ABD75F1...F4C97D2；启动 pid 22780。
#    用户步骤：系统选择器打开 Downloads 中 40 s / 44.1 kHz / 单声道 PCM16 WAV → 播放 →
#    开/关“处理（−6 dB）”→ 暂停/恢复 → 锁屏约 15 s → 播放其他媒体（DreamGuard 应暂停）→
#    关闭本地音轨（原有 DSP 声景应继续出声）。
#    已记录结果：用户对声音/开关/暂停恢复/爆音卡顿**合并答复“正常”**；锁屏约 15 s 持续；
#    其他媒体播放时 DreamGuard 暂停；关闭本地后 DSP 声景出声、调音量/音色与暂停正常。
#    日志：进程存活、MediaSession PAUSED、过滤 AndroidRuntime:E / nativeloader 无 FATAL EXCEPTION 与
#    UnsatisfiedLinkError（**无 nativeloader 成功行**，以实际出声为间接证据）。
#    未执行：30 分钟长稳、蓝牙/有线/外放路由、其他格式/采样率、进程重建、真实峰值/波形测量。
#    不得把“正常”拆成四项独立量化结论，也不得声称 C++ PCM DSP 已接入。

# 12) 第二十二/二十三轮：DG-022 native PCM16 链路（主代理已实测；本轮未运行 APK）
# 12a) 先确认第二十三轮 media3 契约修正可编译（`onFlush()` 必须是 Unit）：已被 12c 的 Gradle 任务覆盖
.\gradlew.bat --no-daemon :app:compileDebugKotlin

# 12b) 宿主 CMake + CTest（新增 dg_pcm16_processor；同时会重跑其余测试）
cmake -S .\dsp-core -B .\dsp-core\build\native -DDREAMGUARD_DSP_BUILD_TESTS=ON
cmake --build .\dsp-core\build\native --config Debug
ctest --test-dir .\dsp-core\build\native --output-on-failure -C Debug

# 12c) Android 构建 + JVM 单测（18 个 LocalAudioProcessorTest + 4 个 NativePcmProcessorContractTest
#      与既有测试一并执行）—— 主代理实测：80 tests / 0 failures / 0 skipped，
#      Debug NDK arm64-v8a / armeabi-v7a / x86_64 编译通过
.\gradlew.bat --no-daemon --offline :app:testDebugUnitTest :app:assembleDebug

# 12d) 无宿主 C++ 编译器时的 NDK x86_64 替代路径（pcm16_processor.cpp 必须进链接行；
#      $ndk / $adb 沿用第 4 节的变量定义）—— 模拟器实测 206/0（PCM16）、348/0（compressor）
& "$ndk\x86_64-linux-android26-clang++.cmd" -std=c++20 -O1 -fPIE -pie -static-libstdc++ `
    -I dsp-core\include -o dsp-core\build\android-test\dg_pcm16_processor_tests `
    dsp-core\src\compressor.cpp dsp-core\src\pcm16_processor.cpp `
    dsp-core\tests\test_pcm16_processor.cpp
& $adb push dsp-core\build\android-test\dg_pcm16_processor_tests /data/local/tmp/
& $adb shell chmod 755 /data/local/tmp/dg_pcm16_processor_tests
& $adb shell /data/local/tmp/dg_pcm16_processor_tests

# 12e) 设备上打开本地文件后，处理开关应显示“本地压缩”，格式提示出现
#      “本地压缩运行中/就绪”；>2 声道或库缺失时应显示对应“直通”文案并保持出声。
#      **本轮未执行**（未安装/未运行 APK）；不得把 JVM 测试或 native 单测当作真实 JNI/音频验证。
#      checks/tests 数已回填；真实设备播放/波形/听感仍未取得。
```

> Windows 路径含 `'` 时 Gradle 测试 worker 可能因 `@argfile` 解析失败而报
> `ClassNotFoundException: ...GradleWorkerMain`。本次直接在 `E:\DreamGuard` 构建未复现该问题；
> 若复现，可 `subst X: E:\DreamGuard` 后从 `X:\DreamGuard` 构建。

> 第一至第五轮的实际执行情况：第 1–2 步与第 4 步的 native 测试由主代理在本机跑过（第四轮 58 JVM tests、
> 1328 checks、110 checks、C 头兼容通过；第五轮构建成功 + **139 checks / 0 failures**）。
> 本会话**没有**执行其中任何一条。第 6 步（泄漏检测）至今完全未做。
> 第五轮复测之后，第 3/4 步的 native 测试已对当前代码成立；JVM 测试在第五轮为 `up-to-date`，
> 若要新报告可加 `--rerun-tasks`。
> 第六轮新增的第 7 节（DG-012）：本会话一条都没有执行。第七轮主代理执行了 **7b 的 x86_64 部分**
> （NDK 交叉编译 + API 35 模拟器推送运行，首跑 227/2，失败为测试期望错误）；
> 第八轮在修复后复测（`dg_lufs` 238/0、`dg_meter` 207/0），并用 **7c 的 `assembleDebug assembleRelease`**
> 构建 Debug/Release 三 ABI（全部通过，JVM `up-to-date`）。**7a（宿主 `cmake`/`ctest`）仍未执行**；
> 第 4 步的 `dg_header_c_compat` 已重新编译并 **PASSED**（第八轮）。
> 第十轮新增的第 9 节（DG-013）：主代理已在第十二/十三轮编译并跑通（348/0）；第二十三轮输出参数类型
> 修正后复跑 `dg_compressor_tests` 仍为 **348 checks / 0 failures**，`dg_header_c_compat` 本轮未单独报告。

## 边界声明

- `dsp-core` 目前由五部分组成：**声景发生器 + 生命周期接口**（DG-010）、**无状态 Float32 PCM 助手**
  （DG-011，`dreamguard/pcm.h`）、**有状态的响度计量与 LUFS 分析器**（DG-012，`dreamguard/meter.h`、
  `dreamguard/lufs.h`）、**前馈压缩器**（DG-013，`dreamguard/compressor.h`），以及第二十二轮新增的
  **PCM16↔float↔压缩器桥**（DG-022，`dreamguard/pcm16_processor.h`）。五者不共享状态；
  PCM 助手、计量器、压缩器与桥都不参与声景渲染。
- PCM 助手**不是音频采集实现**：没有 AudioRecord / OpenSL / AAudio / 麦克风权限路径，没有接入播放链路，
  也没有改变现有音频行为；它只处理内存中的 Float32 交错缓冲（逐位复制、增益、声道映射、静音判定）。
- DG-010 的“误用检测”是**刻意放弃**的能力（见 DG-010 第三节）：本文件、公共头与测试都不声称能识别
  已销毁或伪造句柄，只承诺 NULL 被报告为 `DG_ERR_NULL_HANDLE`。
- PCM 助手的**缓冲别名**规则是公开契约（`dsp-core/include/dreamguard/pcm.h` 顶部）：同指针原地仅限样本数不变的
  三个操作；声道转换与任何部分重叠一律 `DG_ERR_INVALID_ARGUMENT`，且拒绝时不写任何样本。
  这条规则是为了避免把重叠缓冲交给 `memcpy`（C 标准下重叠即未定义行为）以及 mono→stereo 原地覆写输入。
- 没有改动 JNI 符号名、库名、包名或 Kotlin 原生方法名；9 个
  `Java_com_dreamguard_app_audio_NativeAudioEngine_native*` 符号与 `JniContractTest` 的期望集合保持不变
  （第四轮的 58 个 JVM 测试包含该测试；本轮没有新增、改名或删除任何 JNI 符号）。
- DG-012 的 meter 与 LUFS 分析器**没有接入 Android 播放链路**：没有新增 JNI 符号，没有修改任何 Kotlin 调用，
  也没有改变现有音频行为；它们目前只是 `dreamguard_dsp_core` 里的可移植 C++ API 与测试。
- **没有第三方音量控制**：本轮没有实现、也没有声称实现任何对其它应用/系统的音量或音频会话控制。
  计量器只读取调用方传入的内存缓冲。
- **peak 不是 true peak**：`dg_meter` 报告的是采样峰值，未做过采样插值；不声称满足 EBU R128 的 true-peak 要求。
- **LUFS 是 PROVISIONAL**：`dreamguard/lufs.h` 明确标注未与任何参考实现对比。它与 BS.1770-4 的一致性证据有三条——
  48 kHz 下 K 权重系数与公布表在 1e-6 内一致、1 kHz 正弦的解析校准（−0.024 LU 的系统偏差已写明），以及第十五轮
  对 16 个真实 WAV 的 ffmpeg `ebur128` 对照（14 PASS / 2 FAIL / 0 SKIP，最大绝对差 0.049 LU，整体未通过）。
  **不再**是“没有参考读数、没有误差表”，但对照未通过（两项约定差异待裁定）、且参考精度只有 0.1 LU，
  因此仍不声称“标准兼容”，`docs/ROADMAP.md` 的验收项仍未满足。第十八轮改用 metadata 路径后为
  **14 个可比样本 14 PASS / 0 FAIL、worst 0.010 LU、2 例 NO-DATA**；该 PASS 只表示相对 FFmpeg 9.0.2
  metadata（约 0.01 LU 分辨率）一致，两例 NO-DATA 不计入通过，**不构成标准认证**。
- 第八轮证据边界：x86_64 在 API 35 模拟器上跑过全部测试（`dg_lufs` **238/0**、`dg_meter` **207/0**、
  扩展 C 头兼容 **PASSED**）；Debug/Release 三 ABI 的 native 库均已构建通过，但 **arm64-v8a 与 armeabi-v7a
  只被编译链接过、没有运行过任何测试**。仍未执行：宿主 `ctest`、真机、泄漏检测（ASan/LSan）、
  与参考工具的误差对比（本机 PATH 无 ffmpeg/ffprobe）。
- 第九轮证据边界：`dsp-core/tools/lufs_compare/` 的源码、JSON 化的 CSV 列、PS 判定逻辑与 CMake 接线都只经过
  **静态审查**——本会话无法执行任何命令，因此 `dg_lufs_compare` **没有编译过**，`--selfcheck` 与
  `compare_with_ffmpeg.ps1` **没有运行过**，`reference-report.md` **不存在**（此结论对第九轮当时成立；第十五轮该报告
  已由主代理真实生成，后续证据见下一条）。任何“对照已通过”的说法在跑出该报告之前都是假的。工具链本身不含库代码，
  不改变 `dreamguard_dsp_core` 的行为，也不进 Android 构建。
- 对照工具不安装任何东西：不调用包管理器、不改 ACL/权限、不请求提权；缺 ffmpeg 时只打印手工命令并退出 3。
- 第十轮证据边界（DG-013，**当时**）：`compressor.h`、`compressor.cpp`、`test_compressor.cpp`、两处 CMake
  接线与 `test_header_c_compat.c` 的 DG-013 段从未编译、从未运行；该边界已被第十三轮 **348 checks / 0 failures**
  和头文件兼容 PASSED 更新。即使如此，压缩器仍**没有听感/真机证据**，未接入播放链路，不能声称“无爆音/无抽吸”
  已被听感验证。
- 压缩器**没有**限幅器、true peak 检测、lookahead、侧链、并行混合或自动 release；makeup 为正时输出可超
  0 dBFS（计数不削波），这些都属于 DG-014 或以后的迭代，不在 DG-013 的交付范围内。
- 第十七轮 DG-014 的 limiter **只保证采样峰值**：`process` 对每个输出样本施加 `gain <= ceiling/|x|`，但没有
  过采样或重建，因此**不能**保证样本间（true）峰值，也没有做过任何 true-peak 测量。它未接入播放链路、未新增
  JNI/Kotlin 调用；`src/limiter.cpp` 已进静态库，第十七轮主代理 Gradle 三 ABI 构建已实际通过，但本会话仍未
  验证，且 true-peak 验收不能据此成立。
  该组件的 sample-peak 测试已由主代理在第十七轮实际运行（**2113 checks / 0 failures**），但 true-peak
  过采样测量与听感仍未实现/未运行，**不得**把 2113/0 移用为 true-peak 证据，也不得声称“true peak 已实测通过”。
- 新增的 `src/compressor.cpp` 已加入 `dreamguard_dsp_core` 静态库，而 Android 侧
  `app/src/main/cpp/CMakeLists.txt` 以子目录方式引入 dsp-core，因此**下一次 Gradle/NDK 构建会把该翻译单元
  编进 `libdreamguard_dsp.so`**（无 JNI 符号、无 Kotlin 调用，行为不变，但构建面扩大了）。第二十轮主代理
  `assembleDebug` 成功，**编译兼容性已实际验证**；运行期仍未接入播放链路，行为未验证。
- 第十五轮证据边界（参考对照，主代理执行、本会话只读核对）：16 个 WAV 全部由**同一台模拟器上的同一二进制**生成后
  `adb pull`，报告记录了 CSV 与每例 WAV 的 SHA256，因此“两个表计量的是同一批字节”有据可查；ffmpeg 是构建目录内的
  **9.0.2 便携副本**，不是系统安装，也没有被复制到 PATH。
  - 能证明的：在这 16 个合成信号上，`dg_lufs.integrated_lufs` 与 ffmpeg `ebur128` 的差在 0.1 LU 内（14/16），
    且无任何用例出现“我方有值而参考无值”的伪造迹象。
  - **不能**证明的：整体对照**未通过**（2 项定义差异 `FAIL`，exit 1）；0.049 LU 与 ffmpeg 打印取整同量级，
    不能当作实现精度的证据；未做 0.001 LU 级复核、未跑 `--libebur128`、未在真机、未测 5.1/多声道、
    未测超 4 s 长节目或 < 8 kHz 采样率。**不得**把本轮描述成“LUFS 已通过标准认证”。
- 关于 −70.0：它是 ffmpeg `ebur128` 在“无任何块通过 −70 LUFS 绝对门”时的哨兵（源码里字段初值即 `ABS_THRES`），
  本轮**只**用它解释现象，**不用它**替代标准条文；第十五轮这两项曾保持 `FAIL` / 待人工裁定，第十八轮按四条
  判据改判 **NO-DATA**（单列、不计入通过），**不是**数值通过，也没有修改判定规则。
- 第二十轮证据边界（主代理执行、本会话只改文档）：Gradle `testDebugUnitTest --rerun-tasks assembleDebug`
  成功且 **68 tests / 0 failures / 0 skipped**；API 35 **x86_64 模拟器**上完成“选 40 s 44.1 kHz 单声道
  PCM16 WAV → UI 文件名/格式/播放中 → MediaSession `PLAYING` → −6 dB 开关 `checked=true` → 暂停/恢复 →
  无 FATAL”。**不能**由此证明：输出波形/电平、听感、延迟/underrun、iQOO Z9T 真机、内置轨回切与错误路径；
  模拟器无音频输出。当前 −6 dB 是 Kotlin 整数增益级，**不是** C++ 压缩器/限幅器，也未接入 DSP Core。
  本轮代码一行未改；未提交、未推送。
- 第二十一轮证据边界（主代理执行、本会话只改文档）：iQOO Z9T（`V2352A` / `vivo` / Android 16 API 36）上安装同批
  Debug APK（0.1.0 / versionCode 1 / SHA256 `3ABD75F1…F4C97D2`）并启动进程 pid 22780；用户用系统选择器播放
  40 s / 44.1 kHz / 单声道 PCM16 WAV，对“声音、−6 dB、暂停恢复、爆音卡顿”**合并**答复“正常”，锁屏约 15 s 持续，
  播放其他媒体后暂停，关闭本地音轨后 DSP 声景正常出声。**不能**由此证明：输出波形/电平/真实峰值、延迟与 underrun、
  多格式与多采样率、蓝牙/有线/外放路由、进程重建/系统回收、通知栏与锁屏媒体键、30 分钟长稳、来电打断；
  也没有 nativeloader 成功日志（只有实际出声作为间接证据）。C++ PCM DSP 仍未接入，−6 dB 是 Kotlin 整数增益。
  本轮未提交、未推送，代码一行未改，旧的真机基础回归记录保留。
- 第二十三轮证据边界（主代理执行、本会话只改文档）：Debug 构建与 JVM/模拟器 native 测试已实际通过——
  `--offline :app:testDebugUnitTest :app:assembleDebug` 成功、**80 JVM tests / 0 failures / 0 skipped**、
  Debug NDK 三 ABI 编译通过；API 35 x86_64 模拟器 `dg_pcm16_processor` **206 checks / 0 failures**、
  `dg_compressor` **348 checks / 0 failures**；首轮 native 类型错误已修。**不能**由此证明：APK 在模拟器/真机
  的播放、真实 JNI 调用、真实音频输出/波形/听感、延迟与 underrun、多格式/多采样率、进程重建、长稳、true peak。
  JVM 的 JNI 合约测试与 `FakeCompressor` mock **不等于实测 JNI**。第二十/二十一轮的 `68/0/0` 与真机“正常”
  对应**旧 Kotlin −6 dB 链路**，不能移用。>2 声道、缺库、非 direct、process 失败时的位精确回退只由 JVM
  fake 后端覆盖；C++ bypass 由 206/0 覆盖。DG-022/023 仍 `IN_PROGRESS`，DG-020/DG-013 保持 `DONE`、
  不标 `VERIFIED`。**第二十三轮代码已随 `4b7ace3` 推送；第二十四轮日志更新未提交**。
- 第二十四轮证据边界（主代理执行、本次只改文档/状态）：代码提交 `4b7ace3` 已推送；APK 在 API 35 x86_64
  模拟器安装启动，系统选择器选中合成 40 s / 44.1 kHz / mono PCM16 WAV，UI 播放中并显示“本地压缩就绪”，
  开关 `checked=true`；MediaSession `PLAYING`、进程存活、`AndroidRuntime:E` 无 `FATAL EXCEPTION` /
  `UnsatisfiedLinkError`。**不能**由此证明：逐块 `nativeProcess` 调用/JNI 返回值正确、实际音频输出电平、
  听感、延迟/underrun、资源泄漏、真机新版压缩；用户要求延后真机与 30 分钟长稳。DG-022/023 仍 `IN_PROGRESS`，
  DG-020/DG-013 保持 `DONE`、不标 `VERIFIED`；第二十四轮日志更新未提交。
