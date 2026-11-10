# DreamGuard 开发日志

按 `docs/ROADMAP.md` §12 的字段记录：日期、Commit、修改模块、完成任务编号、测试设备、测试结果、
未解决问题及下一步任务。状态判定与证据见 `docs/STATUS.md`。

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
