# DreamGuard — Development Roadmap

**版本：** 1.1 Draft

**当前阶段：** IN_PROGRESS（第三方媒体音量控制优先）

**首发平台：** Android  
**目标设备：** iQOO Z9T  
**开源协议：** MIT

## 1. 开发管理规则

任务状态：TODO（待开始）、IN_PROGRESS（进行中）、BLOCKED（受阻）、DONE（已完成）、VERIFIED（已通过测试）、DEFERRED（延期）。DONE 与 VERIFIED 不得混用。

每项任务必须有任务编号、目标、交付物、验收标准和测试证据。所有未执行任务保持 TODO，不得根据计划自动标记为完成。

本计划各任务的目标由标题和任务内容定义，交付物是所列实现或测试记录；测试证据须在实际执行后记录在开发日志中，执行前为“待收集”。未单独给出验收条件的任务，以交付物全部完成且相应功能测试通过为基本验收条件。任务状态须在核对当前工程与证据后单独登记，不能仅凭本计划判定。

## 当前优先目标：第三方媒体音量控制（P0）

产品目标是在用户使用第三方媒体应用时，让 DreamGuard 能控制系统媒体音量，并逐步实现睡眠定时与渐弱。DG-024 已通过 Debug 构建、92 项单元测试和模拟器基础验证。第二十七轮在 iQOO Z9T、哔哩哔哩及蓝牙输出上通过短时调节与共存检查。DG-024、DG-025 和 DG-026 仍为 `IN_PROGRESS`。DG-025 已通过 Debug 构建、JVM 测试和 API 35 模拟器短测，iQOO Z9T 真机复测待补。

**能力边界：**第一版通过 Android 系统媒体音量控制，影响同一媒体音量通道的其他应用，不能承诺只调整某一个应用、消除所有突发响声或精细压缩动态范围。不同设备、固定音量和蓝牙输出的表现需要实测。第三方应用持续播放时，DreamGuard 不应因启动自身声景或申请音频焦点而无故打断它。参考：[Android 音频输出与音量控制](https://developer.android.com/media/platform/output)。

### DG-024：第三方媒体音量控制原型

- **状态** `IN_PROGRESS`。Debug 构建、92 项单元测试、模拟器基础读写及指定真机组合的短测通过。边界、不可调输出及更广兼容性仍待验证。
- **目标：**第三方媒体应用播放时，用户可在 DreamGuard 查看和调节系统媒体音量。
- **交付物：**系统媒体音量读取与调整、设备是否支持音量调整的检查、最小控制界面、失败及不支持状态提示；不启动 DreamGuard 音频也能使用控制，修改不应隐式申请会打断第三方播放的音频焦点。
- **第二十五轮已落地（第二十六轮构建与 JVM 测试通过，设备侧仅部分验证）：**`SystemMediaVolume.kt`（纯 Kotlin：范围/钳制、固定音量、单级输出、
  读失败、写异常、平台静默忽略写入、外部变化去重通知）、`AudioManagerMediaVolume.kt`
  （只操作 `STREAM_MUSIC`；无新增权限、无 `requestAudioFocus`；`Settings.System` 观察器）、
  `DreamGuardScreen.kt` 的“系统媒体音量”卡（滑块按平台最小/最大分级，固定/失败/不可调文案，
  `ON_START`/`ON_STOP` 注册与反注册）、`SystemMediaVolumeTest.kt`（12 个 JVM 测试）。
  界面明确说明影响所有媒体应用、不是单应用音量；音量卡不依赖播放服务，打开应用不会自动播放。
- **第二十六轮执行证据（主代理在本机/API 35 x86_64 模拟器执行；本会话只登记）：**
  - `.\gradlew.bat --no-daemon --offline :app:testDebugUnitTest :app:assembleDebug` → **BUILD SUCCESSFUL**，
    测试报告 **92 tests / 0 failures / 0 skipped**。
  - 模拟器安装启动；**未启动 DreamGuard 声景**，页面待机、系统媒体音量 **33%（5/15）**；用音量卡滑块调到
    **20%（3/15）**，`dumpsys audio` 记录
    `setStreamVolume STREAM_MUSIC index 3 oldIndex 5 from com.dreamguard.app`，页面显示 **20%**。
  - 该证据证明：Debug 构建与 JVM 测试通过；不播放 DreamGuard 时音量卡能读取并写入真实 `STREAM_MUSIC`
    通道，系统音频服务接受写入，界面与系统索引一致。
  - **不证明**：第三方播放器共存/不被打断、实际声音电平变化、外部音量变化同步（模拟器 adb 调节未生效）、
    边界/固定音量/蓝牙/真机、音频焦点行为；也不证明原生实时压缩或单应用音量。
- **验收标准：**在 iQOO Z9T 与至少一个指定第三方播放器实测，调节后系统媒体音量和实际声音变化一致；第三方播放器持续播放；界面正确反映音量、边界与不可调状态；记录同时影响其他媒体应用的行为。Debug 构建与相关单元测试通过。
- **真机证据** iQOO Z9T（Android 16）、哔哩哔哩 9.18.0 和蓝牙 A2DP。滑块调节 6/15 → 3/15，声音变轻且继续播放。音量键调节后系统为 4/15，页面为 27%。[第 27 轮开发日志](DEVLOG.md)
- **估算：**1–3 个开发日做出可测试原型；为粗略工作量估计，取决于设备与第三方播放器测试结果，并非交付承诺。

### DG-025：睡眠定时与渐弱控制

- **状态** `IN_PROGRESS`。已实现倒计时与渐弱。Debug 构建、137 项 JVM 测试和 API 35 模拟器短测已通过，iQOO Z9T 与哔哩哔哩短测待补。
- **目标：**允许用户主动设置倒计时，逐步降低系统媒体音量，并可取消。
- **交付物：**定时启动/取消、剩余时间、可配置渐弱与目标音量、停止或失效后的明确状态；记录音量变化及用户手动调整时的处理规则。
- **验收标准：**在目标手机和指定第三方播放器上测试定时、取消、锁屏、后台及用户手动调节音量；渐弱按用户设定执行且第三方播放不被意外暂停。进程结束与重建的行为有明确设计和测试记录，不默认宣称可恢复。
- **测试证据** 第 30 轮记录了 API 35 模拟器的系统音量分级、第三方播放器状态、取消、手动调节、短时锁屏和进程强停。见[开发日志](DEVLOG.md)。

### DG-026：第三方播放共存与最小回归

- **状态** `IN_PROGRESS`。已完成指定组合的短时回归。
- **目标：**确认 DreamGuard 控制媒体音量时与目标第三方播放器、锁屏及音频设备共存。
- **交付物：**可重复测试步骤及兼容性记录，覆盖播放器正常播放、DreamGuard 前后台切换、锁屏、音频焦点变化与可用输出设备。
- **验收标准：**至少覆盖 iQOO Z9T、一个指定第三方播放器和用户当前输出设备；记录音量变化、播放器是否中断、设备切换和异常，未覆盖项保持未验证。功能可用后进行真机短测。至少 30 分钟的连续播放等长测统一安排在项目功能全部实现之后。
- **真机证据** iQOO Z9T、哔哩哔哩和蓝牙 A2DP。用户确认打开 DreamGuard、调节音量、退到后台及短时锁屏期间均未中断播放。解锁返回后页面仍显示 27%，系统回读 4/15。设备切换待补测，30 分钟测试列入全功能实现后的长测。[开发日志](DEVLOG.md)

### DG-050：第三方音频捕获与替换可行性（提前验证）

- **状态** `IN_PROGRESS`。Debug 探针已完成 API 35 模拟器实验，iQOO Z9T 与哔哩哔哩兼容性待测。
- **目标：**验证在目标手机和指定第三方应用上，能否取得可用音频，并研究是否存在可行路由，使处理后声音替代原始输出。
- **交付物：**AudioPlaybackCapture 与 MediaProjection 授权原型、来源应用兼容性清单、捕获失败及授权撤销处理、原声/处理声叠加和延迟测量记录、可行性结论。
- **验收标准：**对至少一个允许捕获与一个拒绝或不可捕获的来源分别记录结果；实测原声是否仍输出、处理声是否叠加、延迟与路由；结果不足以实现替换时明确记录受限，不将捕获等同于控制。禁止在未经验证前承诺对所有第三方应用实时压缩或限制峰值。
- **测试证据** 第 31 轮捕获到 AOSP AudioPreview 的 PCM，DeskClock 的 `USAGE_ALARM` 在播放期间被筛掉。监听副本和原声同时走模拟器扬声器路由，系统撤销后探针完成清理。物理路由与声学延迟待测。见[开发日志](DEVLOG.md)。
- Android 播放捕获需要用户授权及录音权限，且来源应用可以禁止捕获；它取得的是音频副本。参考：[Android 播放捕获文档](https://developer.android.com/media/platform/av-capture)。

## 2. Phase 0：环境与仓库初始化

优先级：P0

### DG-001：GitHub 仓库

- 检查 AmariS26242/DreamGuard 是否存在；若不存在，创建公开仓库。
- 配置 MIT License、README、.gitignore；建立 docs 目录和 GitHub Actions。
- **验收：**仓库可访问，初始文件正确提交。

### DG-002：Android 开发环境

- 检查 Android Studio、JDK、Android SDK；安装 NDK 和 CMake。
- 验证 Gradle 构建和真机 USB 调试。
- **验收：**空白 Android 工程成功构建并安装。

### DG-003：工程骨架

- 创建 Gradle 多模块结构、Kotlin 主应用、Compose UI 和 C++ Native Library。
- 配置 JNI、CMake，建立测试目录。
- **验收：**Kotlin 能成功调用 Native 函数。
- 状态登记：结构已完成（`include(":app")` + `include(":dsp-core")`）；2026-11-10 第二轮已重新构建三 ABI 原生库并在模拟器上
  完成安装、启动与播放冒烟，但直接断言 JNI 调用的 instrumented 测试与真机回归未做。
  第三轮又修改了 `dsp-core`（新增 PCM 助手）与 `app/src/main/cpp/jni_bridge.cpp`（句柄改走 `intptr_t`）；
  第四轮由主代理在本机对这批代码执行 `clean testDebugUnitTest assembleDebug assembleRelease` 全部成功，
  JVM 58 tests / 0 failures（含 `JniContractTest`），因此构建证据覆盖到第三轮代码。
  第四轮对 PCM 别名契约的修正随后在第五轮由主代理复测：`testDebugUnitTest assembleDebug assembleRelease`
  → BUILD SUCCESSFUL，三个 ABI 原生库构建通过（JVM 任务 `up-to-date`，58 tests 报告仍为第四轮）。
  证据见 `docs/STATUS.md`。

## 3. Phase 1：DSP Core

优先级：P0

### DG-010：DSP 生命周期

- 实现 dg_create、dg_destroy、dg_reset、参数校验和错误码定义。
- **验收：**生命周期测试通过，无资源泄漏。
- 状态登记：`DONE`（不标 `VERIFIED`）。接口、实现与 native 测试已落地（`dsp-core/`）。第三轮按评审意见重做为
  “不透明指针句柄 + 库内零全局状态”：删除活句柄注册表（它既有 ABA 问题，又让音频路径每块加全局锁），
  错误码收敛为 6 个，销毁后使用句柄明确写为**未定义行为且不检测**；测试整体重写为 17 组。
  第四轮主代理实测：NDK x86_64 交叉编译后在 Android API 35 模拟器运行 **1328 checks / 0 failures**
  （17 组），Debug/Release 构建均成功。**泄漏检测仍未做**，故不满足 `VERIFIED`。证据见 `docs/STATUS.md`。

### DG-011：PCM 基础处理

- 支持 Float32 PCM、单声道、立体声、不同帧数缓冲区、静音输入和 Bypass。
- **验收：**输出格式正确，旁路模式保持输入输出一致。
- 状态登记：`DONE`（**不标 `VERIFIED`**）。2026-11-10 第三轮新增 `dsp-core/include/dreamguard/pcm.h` + `src/pcm.cpp`
  （无状态 `dg_pcm_bypass` / `dg_pcm_apply_gain` / `dg_pcm_map_channels` / `dg_pcm_is_silent`）与
  `tests/test_pcm.cpp`（注册为 CTest `dg_pcm`）。第四轮补了缓冲别名契约（同指针原地 / 重叠拒绝）与索引加宽，
  测试扩为 **24 组**；第五轮主代理对这批最新代码交叉编译并在 API 35 x86_64 模拟器执行
  `dg_pcm_tests` → **139 checks / 0 failures / PASSED**。该模块**未接入播放链路**、**未处理真实音频**，
  也**不是音频采集实现**，且泄漏检测与真机回归未做，故不标 `VERIFIED`。证据见 `docs/STATUS.md`。

### DG-012：基础响度分析

- 实现 RMS、Peak、LUFS 算法设计与实现、分析窗口和 Meter API。
- **验收：**与可信参考工具对比，并记录误差。
- 状态登记：**`DONE`（不标 `VERIFIED`，依据见本节末第十八轮）**。2026-11-10 第六轮写入
  `dsp-core/include/dreamguard/meter.h` + `src/meter.cpp`（流式 RMS/采样峰值/滚动窗口/peak-hold/clip 计数，
  `dg_meter*` API）与 `dsp-core/include/dreamguard/lufs.h` + `src/lufs.cpp`（BS.1770-4 式 K 权重、400 ms 块、
  75 % 重叠、−70 LUFS 绝对门 + 相对门、momentary/short-term/integrated），连同 `tests/test_meter.cpp`、
  `tests/test_lufs.cpp` 并注册为 CTest `dg_meter`、`dg_lufs`。第七轮主代理在 NDK x86_64 + API 35 模拟器首跑：
  `dg_meter` 207 checks / 0 failures，`dg_lufs` 227 / 2，两处失败已定位为 gating 用例漏算单声道 3.0103 LU
  （测试期望错误，非算法）并修正。第八轮修复后复测：`dg_lufs` **238 checks / 0 failures**
  （gating 诊断 `blocks=67 after_abs=61 used=40 integrated=-23.1696`，与解析预期差 0.159 LU，来源已解释）、
  `dg_meter` **207/0**、扩展后的 `dg_header_c_compat` **PASSED**；Gradle `testDebugUnitTest assembleDebug
  assembleRelease` 成功，Debug/Release 三 ABI 均构建通过，JVM `up-to-date`。
  仍未做：真机、泄漏检测（ASan/LSan）、宿主 `ctest`；arm64-v8a / armeabi-v7a 只编译未运行。
  第九轮补上了参考对照工具链：`dsp-core/tools/lufs_compare/`（`dg_lufs_compare` 16 例确定性合成信号 +
  32-bit float WAV 导出 + CSV，`compare_with_ffmpeg.ps1` 驱动 `ffmpeg -af ebur128` 并按 **0.1 LU** 容差判定，
  CTest `dg_lufs_compare` 跑 `--selfcheck`），并接入 `DREAMGUARD_DSP_BUILD_TOOLS`。
  但该工具链**只在静态审查层面成立**：本会话命令环境不可用，`--selfcheck` 与 ffmpeg 对照**零次执行**，
  `dsp-core/build/lufs-compare/reference-report.md` 尚不存在。
  验收要求的“与可信参考工具对比并记录误差”**仍不成立**（LUFS 头文件继续标为 PROVISIONAL，不声称标准兼容）。
  第十三轮：`dg_lufs_compare --selfcheck` **首次真实执行**（主代理 NDK x86_64 + API 35 模拟器）得
  **113 checks / 1 failure**：`mono_levelstep_4s_1s_2s_48k: expected >= 60 blocks above the absolute gate`。
  根因是**工具自身的信号生成缺陷**——`fill_sine` 乘的是用例的 −20 dBFS 电平，电平阶跃却把 `10^(−50/20)` 当相对比例
  传入，安静段实际为 **−70 dBFS**（比 `tests/test_lufs.cpp` 的绝对 −50 dBFS 低 20 dB），整段掉到 −70 LUFS 绝对门以下；
  已改为绝对 −50 dBFS 并新增“安静段 `momentary` 等于解析值”的检查以防复发。
  第十四轮：修正后**重跑 115 checks / 0 failures**（计数 113 → 115 正是新增的两条检查），工具自检全绿；
  但这是“实现与自身公式自洽”的同源自证，**不等于**与标准一致。
  同轮条件变化：FFmpeg **9.0.2** 已存在于 `dsp-core/build/reference-deps/imageio_ffmpeg/binaries/`
  （构建目录内，非系统安装），但本机**无 Windows C++ 编译器**，原生对照路线不可行，故
  `compare_with_ffmpeg.ps1` 新增 `-Csv`/`-WavDir` **预生成模式**（NDK 编译的 Android 工具在模拟器上生成
  CSV + WAV，`adb pull` 后由本机脚本对同一批字节调用 `ffmpeg -af ebur128`；输入只读、不删文件、拒绝
  `-CleanWav`），并把静音与 0.3 s 用例改为 `kind=undefined` 的**定义检查**（双方无值记 `SKIP`、**不计入通过**，
  单边有值记 `FAIL`），报告新增 CSV/每个 WAV 的 SHA256 与 `.csv` 副产物。
  **对照本身仍零执行**：`reference-report.md` 尚不存在，无任何 delta 数。
  第十五轮：**对照首次真实执行**（主代理在本机用预生成模式跑 FFmpeg 9.0.2 `ebur128=peak=true`，16 例全部有读数）：
  **14 PASS / 2 FAIL / 0 SKIP，最大绝对差 0.049 LU**（`stereo_10kHz_-20dB_48k`：我方 −16.649118 vs −16.6），
  其余 |delta| ≤ 0.030（1 kHz +0.007/−0.004、44.1k +0.009、96k −0.011、192k −0.020、100 Hz −0.025、
  非对称 −0.030、电平阶跃 +0.030），整体 verdict `FAIL`、脚本 exit 1，报告落在
  `dsp-core/build/lufs-compare-android/reference-report.md`（含 CSV/每例 WAV 的 SHA256）。
  两项 FAIL 是 `kind=undefined` 的**定义问题**：2 s 数字静音与 0.3 s（不足一 400 ms 块）我方
  `integrated_ready=0`/`-inf`，ffmpeg 打印 **−70.0**；按 FFmpeg 源码该值是绝对门哨兵（`f_ebur128.c` 的
  `#define ABS_THRES -70`、`init()` 里 `integrated_loudness = ABS_THRES`，仅当 400 ms 块 ≥ 门限才覆盖），
  **不是测量值**。两项**保持未通过 / 待人工裁定**，不改判定规则、不改实现、不调容差。
  同时登记精度事实：ffmpeg 汇总只用 `%5.1f`，分辨率 **0.1 LU**，0.049 与取整同量级，故这些偏差既不能证明实现差异
  也不能证明高度一致；0.001 LU 级复核（`ebur128=metadata=1` + `ffprobe ... lavfi.r128.I`，或装 libebur128 跑
  `--libebur128`）尚未做。
  因此第十五轮时仍只登记 `IN_PROGRESS`：误差表已有，但整体对照未通过（2 项待裁定）且参考精度不足。
  第十八轮：**高精度 metadata 路径真实执行成功**（主代理，FFmpeg 9.0.2，同一 CSV + 16 WAV）：
  14 个可比样本 **14 PASS / 0 FAIL / 0 SKIP，worst |delta| 0.010 LU**（`stereo_asym_L-6_R-12dB_48k`、
  `stereo_1k_-20dB_192k`）；2 例静音/0.3 s 判 **NO-DATA**（`I=-70.000` 为 `ABS_THRES` 哨兵，全部 M 块 < −70），
  **单列、不计入 PASS**；报告 `dsp-core/build/lufs-compare-android/reference-report-metadata.md`。
  有效分辨率约 0.01 LU（`HIST_GRAIN=100`），三位小数只是文本格式。据此验收“与可信参考工具对比并记录误差”
  达成，DG-012 记 **`DONE`**；真机、泄漏检测、`--libebur128` 未做，**不标 `VERIFIED`**，LUFS 仍标 `PROVISIONAL`。
  执行步骤见 `docs/STATUS.md` 复现命令第 8 节与 `dsp-core/tools/lufs_compare/README.md`。

### DG-013：动态压缩器

- 实现 Soft Knee、Threshold、Ratio、Attack、Release、立体声联动和增益平滑。
- **验收：**测试信号处理符合预期，无明显爆音或抽吸。
- 状态登记：**`DONE`（不标 `VERIFIED`，依据见本节末第十三轮）**。2026-11-10 第十轮写入
  `dsp-core/include/dreamguard/compressor.h`（C99 兼容 API + 参数区间 + 线程约定）、
  `dsp-core/src/compressor.cpp`（瞬时幅度检测 → dBFS → 对称软拐点静态曲线 → 对增益衰减做单极点
  attack/release 平滑 → makeup 增益）、`dsp-core/tests/test_compressor.cpp`（16 组：校验矩阵、静态曲线闭式值、
  ratio 1 直通、稳态、软拐点流、attack/release 定律、正弦界、立体声联动、reset、分块不变性、无爆音增益步长、
  makeup/clip、非有限输入、原地别名、采样率/声道冒烟），并接入静态库、CTest `dg_compressor` 与
  `test_header_c_compat.c` 的 DG-013 段。
  但本轮交付物**从未编译、从未运行**：本会话 `pwsh` 在启动阶段即失败
  （`Error: SetNamedSecurityInfoW failed (Win32 5): grantWrite(E:\DreamGuard)`），按用户要求未改 ACL、未做系统级安装。
  因此“无明显爆音或抽吸”**没有音频证据**，只有“增益平滑 ⇒ 逐帧增益变化 ≤ `(target−gr)·coef`”的设计论证
  与一组尚未执行的断言；监听测试、参考压缩器对比、真机、true peak 行为均未做，压缩器也**未接入播放链路**。
  主代理跑通 `ctest -R dg_compressor`（`docs/STATUS.md` 复现命令第 9 节）后才可改 `DONE`，`VERIFIED` 仍需听感/真实音频证据。
  第十一轮：主代理首次在 NDK x86_64 真实编译**失败**——`compressor.h:161` 报 `warning: '/*' within block comment`，
  随后 `dg_compressor_process` 声明被注释吞掉，多处 `use of undeclared identifier`。根因是第十轮合并该函数注释时漏写
  结尾 `*/`，已修复并复核四个文件的注释闭合（测试期望一字未改）；同时把 `compressor.cpp` 补进复现命令第 4 步的
  C 头兼容链接行（否则 `dg_compressor_*` 会 undefined reference）。
  第十二轮：修正后真实运行——NDK x86_64 + API 35 模拟器 **334 checks / 2 failures**。两处失败经分析都是**测试期望错**，
  不是实现缺陷：①`output_peak_db` 是“整块峰值，含 attack”，第一块最响的是第一个样本
  （`level_db + target·(1−e^(−1/(attack·fs)))` = −6.23677，与实测 −6.236768 吻合到 6 位小数）；②浮点样本无法恰好落在
  −20 dBFS（`float(0.1)` 比阈值高 1.3e-7 dB，硬拐点合法给出 ≈−9.7e-8 dB）。测试已改写为按解析值断言（第一块查攻击峰值、
  第二块查稳态峰值；阈值边界改用样本真实电平推导，并新增“−20.5 dBFS 低于阈值 → bitwise 0”与“double 精确 −20 dBFS →
  `== 0.0`”两条更硬的检查）。**修正后尚未重跑**，因此仍只登记 `IN_PROGRESS`。
  第十三轮：重跑 **348 checks / 0 failures**，`dg_header_c_compat` PASSED，Gradle Debug/Release 三 ABI 构建成功、
  JVM `up-to-date`。验收第 1 条“测试信号处理符合预期”达成 → 登记 **`DONE`**；验收第 2 条“无明显爆音或抽吸”只有
  无爆音断言与设计论证，**无听感/真机证据**，压缩器当时也未接入播放链路，故**不标 `VERIFIED`**，也不得描述成“音质已验收”。
  第二十二轮起压缩器经 `dg_pcm16_processor` 桥接入本地 PCM16 播放链路；第二十三轮已编译并跑通
  PCM16 桥 206/0 与压缩器 348/0；第二十四轮已安装运行 APK 并在 UI 看到“本地压缩就绪”，
  但**没有真实音频输出/听感证据**，不改变本任务 `DONE`（不标 `VERIFIED`）的状态。
  已知边界：无 lookahead、无限幅器、无 true peak、无侧链/并行混合/自动 release；makeup 为正时输出可超 0 dBFS
  （计入 `clip_samples`，属 DG-014 范围）。

### DG-014：峰值限制器

- **状态：DONE，不标 VERIFIED。** 保守 4× guard 已接入本地播放，独立 16× 参考对照 120/0。真机听感及长测待补。[第 36 轮证据](evidence/dg014-015-2026-11-12/README.md)

- 实现峰值检测、True Peak 检测策略、限制器、过采样验证和极端输入测试。
- **验收：**峰值目标得到实际测量验证。

### DG-015：性能测试

- **状态：DONE，不标 VERIFIED。** 300 项 x86_64 模拟器矩阵及复现脚本已保存，真机性能后测。[结果](evidence/dg014-015-2026-11-12/README.md)

- 测试多种采样率、不同缓冲区大小、CPU、延迟和内存。
- **验收：**保存可复现的测试结果。

## 4. Phase 2：Android 音频接入

优先级：P0

### DG-020：JNI Bridge

- 支持 Native Engine 创建、销毁、参数传递、PCM 缓冲区传输、错误处理与 Native Buffer 优化。
- **验收：**反复创建、处理、释放不发生崩溃。
- 状态登记：**`DONE`（不标 `VERIFIED`）**。第二十二轮新增独立 PCM surface：`NativePcmProcessor` 与
  `jni_bridge.cpp` 的 5 个 `nativeCreate/Process/Reset/Destroy/Version` 符号（不与 `NativeAudioEngine`
  的 9 个符号共享类前缀或句柄类型）；`NativePcmProcessorContractTest` 静态核对声明名、无 Kotlin 模块后缀与
  C++ 符号集合。PCM `process` 只接受 direct ByteBuffer，失败返回负错误码并由 Kotlin 位精确回退。
  **第二十三轮**已随 Debug 构建编译并链接进三个 ABI；`NativePcmProcessorContractTest` 在
  **80 JVM tests / 0 failures / 0 skipped** 中通过，但它验证的是声明名、无 Kotlin 模块后缀与符号集合，
  **不是 JNI 运行**；没有设备/模拟器播放调用 `nativeProcess` 的证据，JVM 合约与 mock 不能替代实测 JNI。
  **第二十四轮**模拟器播放显示“本地压缩就绪”，可**间接**说明 native 库加载与 `nativeCreate` 未报错，
  但 `nativeProcess`/`reset`/`destroy` 仍无设备侧证据。
  “反复创建、处理、释放不发生崩溃”仍只有第四轮 API 35 模拟器 **1328 checks / 0 failures**（旧桥 + 声景引擎）
  的旧证据，不能移用给新 surface，故不标 `VERIFIED`。

### DG-021：Media3 播放器

- 配置 Media3，创建 ExoPlayer，实现本地文件播放、暂停与恢复、播放进度和音频格式识别。
- **验收：**iQOO Z9T 本地音频正常播放。
- 状态登记：**`IN_PROGRESS`**。第十九轮写入系统文件选择器（`OpenDocument`，`audio/*`，持久化读授权）与
  本地文件播放/暂停/恢复（`PlaybackService.setLocalAudio/toggleLocalPlayback/closeLocalAudio`，
  复用同一 `ExoPlayer`，`REPEAT_MODE_OFF`，进程重建后校验授权恢复）；本地/内置 `dream_loop` 互斥切换，
  现有声景的层逻辑加本地守卫。**第二十轮主代理实测**：`testDebugUnitTest --rerun-tasks assembleDebug`
  成功（68/0/0）；API 35 x86_64 模拟器安装启动，系统选择器从 Downloads 选 40 s / 44.1 kHz / 单声道 PCM16 WAV，
  UI 显示文件名、`16-bit PCM · 44100 Hz · 1 ch`、播放中，MediaSession `PLAYING`；暂停/恢复通过，无 FATAL。
  第二十一轮主代理在 iQOO Z9T 真机（ADB `model` V2352A / `manufacturer` vivo / Android 16 API 36）安装同批
  Debug APK（0.1.0 / versionCode 1 / SHA256 `3ABD75F1…F4C97D2`，启动 pid 22780），用户用系统选择器播放
  40 s / 44.1 kHz / 单声道 PCM16 WAV，对声音、−6 dB 开关、暂停恢复、爆音卡顿**合并答复“正常”**；锁屏约 15 s
  声音持续，播放其他媒体后 DreamGuard 暂停，关闭本地音轨后 DSP 声景正常出声。日志：进程存活、MediaSession
  `PAUSED`、无 `FATAL EXCEPTION` / `UnsatisfiedLinkError`（无 nativeloader 成功行，以实际出声为间接证据）。
  **第二十三轮只构建了 Debug APK（80 JVM tests / 0 failures、Debug 三 ABI），未安装/未运行**。
  **第二十四轮已安装启动当前 Debug APK，系统选择器选 40 s / 44.1 kHz / mono PCM16 WAV，UI 播放中，
  MediaSession `PLAYING`，进程存活，`AndroidRuntime:E` 无 FATAL/UnsatisfiedLinkError**；仍无音频输出/波形/
  听感、真机、播放进度 UI。
  **交付物“播放进度”仍未实现**，多格式、多路由（蓝牙/有线/外放）、进程重建、30 分钟长稳均未测，
  故仍 `IN_PROGRESS`，不标 `DONE` / `VERIFIED`。

### DG-022：AudioProcessor

- 实现 configure、queueInput、getOutput、queueEndOfStream、isEnded、flush、reset 和 DSP Core 集成。
- **验收：**真实音频通过 DSP 后正常播放。
- 状态登记：**`IN_PROGRESS`**。第二十二轮把本地 PCM16 **真正接入 C++ 压缩器**（DG-022 功能构建）：
  `dsp-core` 新增 `dreamguard/pcm16_processor.h` + `src/pcm16_processor.cpp`（PCM16→float→DG-013 压缩器→PCM16；
  512 帧定长 scratch、无逐样本分配、bypass 用 `memmove` 位精确、残帧/奇数字节原样、饱和量化、无 lookahead）；
  压缩参数保守：threshold −18 dBFS、2:1、knee 6 dB、attack 20 ms、release 250 ms、stereo linked、
  makeup −6.0206 dB（保留旧固定衰减下限，故 UI 开关改名“本地压缩”）。`jni_bridge.cpp` 新增 5 个 PCM 符号，
  `LocalAudioProcessor` 仅当 native 返回字节数与输入完全相等时才采用输出；开关关闭、库缺失、create/process
  失败、非 direct buffer、>2 声道一律逐字节回退（奇尾字节保留）并置 `NativeState`/`nativeError`；
  `flush` reset 压缩器、format 变化重建句柄、`reset`/`close` 释放句柄；`PlaybackService.onDestroy`
  在 `player.release()` 后 close。测试新增 C++ `dg_pcm16_processor`（24 组 → 第二十三轮 27 组）、重写 JVM
  `LocalAudioProcessorTest`（17 → 18 个 fake 后端）、新增 `NativePcmProcessorContractTest`（4 个）。
  第二十/二十一轮的 `68/0/0`、模拟器/真机“正常”对应旧 Kotlin −6 dB 链路，不是本轮 native 压缩器证据。
  第二十三轮按 media3 1.5.1 官方源码修正了 **`onFlush()` 必须是 void**（原 `: Boolean` 会编译失败），并把
  native 句柄的创建/替换移到 `flush()`（格式生效点）：`configure()` 只记录 pending，configure 与 flush 之间
  旧格式 buffer 继续用旧句柄；同格式 flush 只 reset；失败后开关仍可关闭、格式提示始终明示直通原因；
  compressor 输出参数统一为 `dg_pcm_buffer`（首轮 native 类型错误已修）。**主代理实测**：
  `--offline :app:testDebugUnitTest :app:assembleDebug` **成功**、**80 JVM tests / 0 failures / 0 skipped**、
  Debug 三 ABI 编译通过；API 35 x86_64 模拟器 `dg_pcm16_processor_tests` **206 checks / 0 failures**、
  `dg_compressor_tests` **348 checks / 0 failures**。**边界**：第二十三轮未运行 APK；**第二十四轮已安装运行，
  打开处理开关 `checked=true` 并显示“本地压缩就绪”（`READY`）**，但未观察到 `ACTIVE`、无逐块日志/波形/录音，
  第 24 轮证据覆盖格式协商和 native 句柄就绪。
  **第 32 轮** 在 API 35 模拟器观察到 `ACTIVE`，独立探针实测 48 kHz 双声道 PCM16 播放输出随压缩开关变化。
  低、高电平输入的 RMS 分别降低约 6.1 dB 和 13.3 dB。文件播完后可从本地卡重播。
  状态保持 `IN_PROGRESS`，iQOO Z9T 真机听感和兼容性待测。见[开发日志](DEVLOG.md)。

### DG-023：Android 基础 UI

- 实现主界面、文件选择、播放控制、自然模式开关、Bypass、响度显示和错误提示。
- **验收：**完成完整本地播放闭环。
- 状态登记：**`IN_PROGRESS`**。第十九轮在 `DreamGuardScreen.kt` 加本地音频卡（选择/更换、播放/暂停、关闭、
  处理开关与格式提示、错误复用），本地模式禁用环境音按钮。**第二十轮主代理在 API 35 x86_64 模拟器实测**：
  选择 WAV 后显示文件名与格式、播放中，处理开关 `checked=true`，暂停显示已暂停、恢复后 `PLAYING`，无 FATAL。
  模拟器无音频输出、无听感、无截图比对；“关闭回内置轨”“错误路径”未实测；“自然模式/响度显示”仍未做
  （本轮开关是处理 bypass）。**第二十一轮 iQOO Z9T 真机**走通“选 40 s WAV → 播放 → −6 dB → 暂停/恢复 →
  关闭回 DSP 声景”，用户答复“正常”、无爆音卡顿；错误路径、进程重建、通知栏控制与截图/波形仍未做。
  **第二十三轮只构建 Debug APK，未安装/未运行**；**第二十四轮模拟器 UI 显示播放中、格式提示“本地压缩就绪”、
  开关 `checked=true`**，但无截图比对/真实听感/错误路径/进程重建/真机验证。
  验收主路径已覆盖但不完整，保持 `IN_PROGRESS`。

## 5. Phase 3：睡眠体验

优先级：P1

### DG-030：后台播放

- 实现 MediaSession、MediaSessionService、通知栏控制、锁屏播放、音频焦点和蓝牙控制。
- **验收：**完成对应功能，并通过设备上的后台、锁屏、焦点和蓝牙控制测试。

### DG-031：睡眠定时

- 实现定时停止、渐进式音量衰减、取消定时和定时状态恢复。
- **验收：**定时、取消、衰减及状态恢复均符合预期并有测试记录。

### DG-032：三种预设

- 实现 Natural、Balanced、Protection、参数保存和参数恢复。
- **验收：**各模式存在可测量的行为差异，并通过听感测试。

## 6. Phase 4：响度匹配与自然听感

优先级：P1

### DG-040：跨内容响度匹配

- 实现响度观察窗口、增益计算、增益上下限、平滑过渡和内容切换测试。
- **验收：**跨内容切换的响度和增益变化可测量，平滑过渡符合预期。

### DG-041：ASMR 专项测试

- 测试耳语、呼吸声、敲击声、摩擦声和高频触发音。
- **验收：**完成素材 A/B 对比并记录问题与参数调整依据。

### DG-042：环境音专项测试

- 测试雨声、海浪、风声、白噪声和助眠音乐。
- **验收：**完成 A/B 对比，记录听感问题与参数修改依据。

## 7. Phase 5：Android 兼容性

优先级：P2

### DG-050：外部应用音频捕获实验（P1，提前）

- 任务定义、验收与证据要求见上方“当前优先目标”的 DG-050；完成日期取决于目标应用和 Android 路由实验结果。

### DG-051：设备适配

- 测试 iQOO Z9T、不同 Android 版本、蓝牙耳机、有线耳机、扬声器和音频设备切换。
- **验收：**记录各设备与输出方式的测试结果及已知兼容性限制。

## 8. Phase 6：Windows

优先级：P2

### DG-060：Windows 工程

- 复用 DSP Core，配置 CMake 构建、Windows 测试程序和参数配置兼容。
- **验收：**Windows 构建及测试程序运行成功，参数配置兼容性有测试记录。

### DG-061：Windows 音频

- 实现 WASAPI Loopback、虚拟音频设备路由、实时处理、输出设备管理并防止音频反馈。
- **验收：**完成音频路由和实时处理测试，验证输出切换及反馈防护。

### DG-062：Windows UI

- 使用 Qt 6，实现音频状态监控、参数控制和预设管理。
- **验收：**状态监控、参数控制和预设管理均可操作并通过测试。

## 9. 版本里程碑

| 版本 | 目标 |
| --- | --- |
| 0.0.1 | 工程骨架与 Native 接口 |
| 0.0.2 | DSP 基础处理 |
| 0.0.3 | Media3 集成 |
| 0.1.0 | Android 技术 MVP |
| 0.2.0 | 完整睡眠体验 |
| 0.3.0 | 智能响度匹配 |
| 0.4.0 | Android 兼容性扩展 |
| 1.0.0 | Android 稳定版 |
| 2.0.0 | Windows 跨平台版本 |

版本号以实际验收结果决定，不以计划日期自动推进。

## 10. 发布门槛

### 0.1.0

- 可安装 APK、本地音频播放、DSP 实时处理、Bypass、基础响度监测、JNI 稳定性、基础测试记录。

### 0.2.0

- 增加后台播放、睡眠定时、渐进式衰减和三种预设，并完成相应短测。

### 1.0.0

- 稳定性测试、多类助眠素材听感测试、设备兼容性说明和完整用户文档。长时间运行测试按第 11 节的统一安排纳入最终验收。

## 11. 当前任务优先顺序

功能开发期间进行必要的自动测试和短时回归。项目功能全部实现后，再统一安排所有长测，包括 30 分钟连续播放和长时间锁屏、后台运行。

1. 完成 DG-024 系统媒体音量控制的实现与自动验证，确保第三方媒体可继续播放。
2. 功能构建完成后，在可安排真机时执行 DG-026：iQOO Z9T 与指定第三方播放器的实际音量、锁屏、切后台及音频焦点验证。
3. 在原型基础上完成 DG-025 睡眠定时与渐弱，并完成相应功能短测。
4. 提前执行 DG-050 外部音频捕获和路由可行性实验；在证据支持之前不承诺对第三方原始输出做实时压缩。
5. 根据实验结果决定自动响度保护的技术路线，继续 DG-014/015、DG-022 的真实音频处理验证。
6. 再推进其他睡眠体验、设备兼容性、发布准备和 Windows 阶段。各未执行任务仍为 TODO，已执行任务以 `docs/STATUS.md` 的证据为准。
7. 项目功能全部实现后，统一执行长测并记录结果，用于最终验收。

时间估算不是验收证明；不以日期自动改变任务状态。优先完成核心功能，避免复杂 UI 占用关键路径。

## 12. 开发日志

每次重要开发任务完成后记录：日期、Commit、修改模块、完成任务编号、测试设备、测试结果、未解决问题及下一步任务。

- 开发日志：`docs/DEVLOG.md`
- 任务状态台账（含验收证据、DONE 与 VERIFIED 的区别、未验证项）：`docs/STATUS.md`

当前状态：**项目整体 IN_PROGRESS**；第三方媒体音量控制为当前优先目标。所有未执行任务保持 TODO，不得根据计划自动标记为完成。
已触碰任务的实际状态以 `docs/STATUS.md` 为准：DG-001/DG-002 `DONE`（历史证据，非本次复核）；
DG-003 `DONE`（结构完成；第五轮主代理构建 Debug/Release + 三 ABI 通过，最新代码已覆盖）；
DG-010 `DONE` 不标 `VERIFIED`（实测 1328 checks / 0 failures，属未改动部分；泄漏检测从未做）；
DG-011 `DONE` 不标 `VERIFIED`（最新代码 24 组 / 139 checks 通过；未接入播放链路、未处理真实音频，泄漏检测与真机回归未做）；
DG-012 `DONE` 不标 `VERIFIED`（第八轮 x86_64 复测 `dg_lufs` 238/0、`dg_meter` 207/0、扩展 C 头兼容 PASSED，
Debug/Release 三 ABI 构建通过；第十五轮 summary-only 对照 16 例 **14 PASS / 2 FAIL / 0 SKIP**、最大绝对差
**0.049 LU**，两项 FAIL 为静音/0.3 s 的 −70.0 哨兵定义差异；第十六轮把脚本主判据改为三位小数
`lavfi.r128.I` + NO-DATA 分类；**第十八轮高精度 metadata 对照真实执行：14 个可比样本 14 PASS / 0 FAIL，
worst 0.010 LU，2 例 NO-DATA（−70.000 哨兵、全部 M 块 < −70），单列不计通过；有效分辨率约 0.01 LU
（HIST_GRAIN=100），三位小数只是格式**）。报告 `reference-report-metadata.md`；真机与泄漏检测、`--libebur128`
未做，LUFS 仍为 PROVISIONAL）。
DG-013 `DONE` 不标 `VERIFIED`（第十三轮 348 checks / 0 failures、C 头兼容 PASSED、三 ABI 构建成功；
**第二十三轮输出参数类型修正后复跑仍 348/0**；无听感/真机证据，虽已接入 PCM16 桥但没有设备播放验证）。
DG-014 `IN_PROGRESS`（`limiter.h` / `limiter.cpp` / `test_limiter.cpp` 与 CMake 接线已写入；主代理在 NDK x86_64
API 35 模拟器实测 **2113 checks / 0 failures**，Gradle `testDebugUnitTest assembleDebug assembleRelease` 成功、
三个 ABI 通过、JVM up-to-date；但只保证 sample peak，未做过采样 true-peak 测量、无真机/听感、未接播放链路，
验收仍未达成）。
DG-024 / DG-026 `IN_PROGRESS`。第 27 轮 iQOO Z9T + 哔哩哔哩 + 蓝牙短测通过。边界、固定输出、其他播放器和设备切换待补测。30 分钟长测排在项目功能全部实现之后。
DG-020 `DONE` 不标 `VERIFIED`（第二十二轮新增独立 PCM surface：`NativePcmProcessor` 5 个 JNI 符号与
`NativePcmProcessorContractTest`；**第二十三轮已编译进 Debug 三 ABI，契约测试在 80 JVM tests 中通过**；
第二十四轮模拟器“本地压缩就绪”间接说明库加载/`nativeCreate` 未报错，但无 `nativeProcess`/`reset`/`destroy`
设备证据，旧 9 符号证据仍为第四轮 1328/0）。
DG-021 / DG-022 / DG-023 `IN_PROGRESS`（第十九轮写入本地文件选择与播放/暂停/恢复、`LocalAudioProcessor`
（−6 dB、bypass 位精确、格式协商/flush/EOS）、`LocalAudioRenderersFactory` 注入、最小 UI 与 10 个 JVM 测试；
**第二十轮主代理真实执行**：`testDebugUnitTest --rerun-tasks assembleDebug` 成功、68 tests / 0 failures /
0 skipped；API 35 x86_64 模拟器安装启动，选 40 s 44.1k mono PCM16 WAV，文件名/格式/播放中，MediaSession
`PLAYING`，−6 dB 开关 `checked=true`，暂停/恢复通过，无 FATAL；但模拟器无音频输出、未听感、未真机、
  未录波；**第二十一轮主代理在 iQOO Z9T 真机（V2352A / vivo / Android 16 API 36）补了真机证据**：安装同批
  Debug APK（0.1.0 / versionCode 1 / SHA256 `3ABD75F1…F4C97D2`，pid 22780），用户用系统选择器播放
  40 s / 44.1 kHz / 单声道 PCM16 WAV，对声音、−6 dB、暂停恢复、爆音卡顿**合并答复“正常”**；锁屏约 15 s 持续、
  播其他媒体后暂停、关闭本地后 DSP 声景出声；日志无 `FATAL EXCEPTION` / `UnsatisfiedLinkError`
  （无 nativeloader 成功行）。**第二十二轮把 PCM16 真正接入 C++ 压缩器**（`pcm16_processor`、
  `NativePcmProcessor`、`LocalAudioProcessor` native 主路径 + 位精确回退、开关改名“本地压缩”）；
  **第二十三轮按 media3 1.5.1 修正 `onFlush()` 签名并改为 flush 时创建/替换句柄**，补强失败后开关与格式提示；
  **主代理实测**：`--offline :app:testDebugUnitTest :app:assembleDebug` 成功、**80 JVM tests / 0 failures /
  0 skipped**、Debug 三 ABI；API 35 x86_64 模拟器 `dg_pcm16_processor` **206/0**、`dg_compressor` **348/0**。
  **边界**：第二十三轮未运行 APK；**第二十四轮已安装启动当前 APK，系统选择器选 40 s/44.1kHz/mono PCM16 WAV，
  UI 播放中 + “本地压缩就绪”（READY）、开关 checked=true、MediaSession PLAYING、AndroidRuntime:E 无
  FATAL/UnsatisfiedLinkError**；但这只证明播放状态与处理器就绪，**不证明**逐块 nativeProcess、实际音频输出、
  听感、延迟/泄漏或真机通过；JVM mock ≠ 实测 JNI；第二十/二十一轮证据对应旧 Kotlin −6 dB 链路，
  不能移用给新 native 链路；仍缺播放进度 UI、多格式/多路由/进程重建/30 分钟长稳/波形/听感证据，
  三项维持 `IN_PROGRESS`，不标 `DONE` / `VERIFIED`）。

