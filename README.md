# DreamGuard

DreamGuard 是一个 Android-first 的睡眠声景应用骨架：Jetpack Compose 界面、Media3 环境音轨、以及一个用 **C++20 + NDK/CMake** 实现、通过 **JNI** 驱动的实时 DSP 声景引擎。所有声音都在本机生成或来自 APK 内置循环，应用不申请网络、麦克风或存储权限。

> 当前优先目标：第三方媒体播放期间的系统媒体音量控制。第二十六轮主代理已构建（BUILD SUCCESSFUL）并通过 **92 tests / 0 failures / 0 skipped**，在 API 35 x86_64 模拟器验证未播放 DreamGuard 时 33%（5/15）→20%（3/15）的系统媒体音量读写（`dumpsys audio` 有记录）；但第三方播放器共存、外部同步、固定输出/蓝牙/真机未验证，状态仍为 `IN_PROGRESS`，更新**尚未提交**；详见[开发路线图](docs/ROADMAP.md)。

> 当前版本是 **工程骨架 / 可运行原型**，不是已经过真机长时间睡眠验证的产品。未验证项见[验证状态](#验证状态)。

## 功能

- Compose + Material 3 深色睡眠界面，单屏操作。
- 播放/暂停由原生 DSP 生成的声景：双正弦低频 drone、粉红噪声、低通音色控制、软限幅、2 秒淡入。
- 可调声景音量、音色亮度（低通截止频率）、噪声音量、低频频率。
- Media3 ExoPlayer 播放 APK 内置的可循环环境音轨，可与原生声景叠加；环境音关闭时该轨道静音，以维持统一媒体会话。也可通过系统文件选择器打开本机音频（`OpenDocument`，无需存储权限）；本地文件与内置轨道互斥，可播放/暂停/恢复/关闭，并可挂一个 16-bit PCM 处理级（见“架构”）。
- 播放由 MediaSessionService 管理，提供媒体通知和后台播放；点击通知/锁屏卡片返回应用。音频焦点中断及耳机拔出时暂停声景，但保留会话，中断结束后可继续。
- **系统媒体音量卡（DG-024，已构建/基础验证，未完成验收）**：直接读取和调节 Android `STREAM_MUSIC` 通道（`AudioManager`），**不需要播放 DreamGuard 声景、不申请音频焦点、不新增权限**。滑块按设备实际最小/最大索引分级；固定音量设备、单级输出、读取失败、写入被拒/忽略、外部音量键或其他应用造成的音量变化都有对应状态。文案明确该通道**影响所有媒体应用，不是只调 DreamGuard 或某一个播放器**，且不会自动启动/暂停第三方播放器。第二十六轮主代理实测 JVM **92/0**，并在 API 35 x86_64 模拟器未播放时把系统音量从 33%（5/15）调到 20%（3/15）；第三方播放器共存、外部同步、固定输出/蓝牙/真机仍未验证。
- DSP 渲染线程异步失败时：若只有 DSP 层在播，暂停媒体会话以免留下无声的前台服务；若环境音同时在播，保留环境音并在界面显示 DSP 错误。失败按 `TRANSIENT` / `PERMANENT` 分类；**临时输出错误不会清除已保存的 DSP/环境音选择**，只有用户主动关闭层或成功重启才更新状态。
- 开始被上一段渲染线程的关闭延迟时，服务做最多 6 次 × 250 ms 的有界重试，界面显示“正在等待上一段音频结束…”；重试耗尽后暂停并给出“上一段音频仍在关闭，请稍后重试”的可见提示，用户再点播放即显式重试，**不会自动播放**。
- 媒体播放器内部错误与用户暂停分开处理：错误暂停不会触发 `stopSelf()`，错误文案保留在界面上，直到成功重试或用户主动关闭。
- 纯 Kotlin 的 `PlaybackController` 状态机与失败/恢复策略不依赖 Android，可直接在 JVM 上测试。
- 58 个既有 JVM 单元测试覆盖参数清洗、状态迁移、错误路径、监听通知、渲染线程停止/销毁时序、失败分类、有界重试、零写退避、两层选择状态与 JNI 命名契约；第十九轮新增 10 个 `LocalAudioProcessorTest`（增益全值域、bypass 位精确、协商/拒绝、flush/EOS/reset），第二十轮主代理实测 68/0/0，第二十三轮实测 **80 tests / 0 failures / 0 skipped**。第二十五轮新增 **12 个** `SystemMediaVolumeTest`（范围与钳制、固定音量、单级输出、读/写失败、平台静默忽略写入、外部变化去重通知、失败恢复），**第二十六轮主代理实测：92 tests / 0 failures / 0 skipped**。
- GitHub Actions：JDK 17、Android SDK 35、NDK 27.0.12077973、CMake 3.22.1，执行 `testDebugUnitTest` 与 `assembleDebug`。

## 技术栈

| 层 | 选型 |
| --- | --- |
| UI | Kotlin 2.0.21、Jetpack Compose（BOM 2024.12.01）、Material 3 |
| 播放 | Android `AudioTrack`（float PCM）+ Media3 ExoPlayer 1.5.1 |
| 原生 | C++20、NDK 27.0.12077973、CMake 3.22.1 |
| 构建 | AGP 8.7.3、Gradle 8.11.1、JDK 17 |
| SDK | compileSdk / targetSdk 35，minSdk 26 |

## 目录结构

```text
DreamGuard/
├── .github/workflows/android-ci.yml
├── app/
│   ├── build.gradle.kts
│   ├── proguard-rules.pro
│   └── src/
│       ├── main/
│       │   ├── AndroidManifest.xml
│       │   ├── cpp/
│       │   │   ├── CMakeLists.txt          # 目标/库名 dreamguard_dsp，add_subdirectory(dsp-core)
│       │   │   ├── jni_bridge.cpp          # 9 个声景 + 5 个 PCM16 JNI 符号，只调用 C API
│       │   ├── java/com/dreamguard/app/
│       │   │   ├── MainActivity.kt
│       │   │   ├── audio/
│       │   │   │   ├── AudioEngine.kt
│       │   │   │   ├── AudioManagerMediaVolume.kt # DG-024 STREAM_MUSIC 读写 + 外部音量变化观察
│       │   │   │   ├── AudioPlatform.kt      # AudioTrack / JNI 适配层
│       │   │   │   ├── AudioSettings.kt
│       │   │   │   ├── LocalAudioProcessor.kt     # 16-bit PCM → native 压缩器；失败/关闭时逐字节 bypass
│       │   │   │   ├── LocalAudioRenderersFactory.kt
│       │   │   │   ├── NativeAudioEngine.kt
│       │   │   │   ├── NativePcmProcessor.kt      # DG-022 PCM16 JNI 封装
│       │   │   │   ├── PcmCompressor.kt           # 处理器后端接口 + JNI 实现
│       │   │   │   ├── PlaybackController.kt
│       │   │   │   ├── PlaybackFailurePolicy.kt
│       │   │   │   ├── PlaybackLayers.kt
│       │   │   │   ├── PlaybackRecoveryPolicy.kt
│       │   │   │   ├── PlaybackService.kt
│       │   │   │   └── SystemMediaVolume.kt       # DG-024 纯 Kotlin 系统音量规则
│       │   │   └── ui/
│       │   │       ├── DreamGuardScreen.kt
│       │   │       └── theme/Theme.kt
│       │   └── res/
│       │       ├── raw/dream_loop.wav      # 3 秒可循环合成音轨
│       │       └── values/{strings,themes}.xml
│       └── test/java/com/dreamguard/app/audio/
│           ├── AudioSettingsTest.kt
│           ├── FakeAudioEngine.kt
│           ├── JniContractTest.kt
│           ├── LocalAudioProcessorTest.kt
│           ├── NativeAudioEngineTest.kt
│           ├── NativePcmProcessorContractTest.kt
│           ├── PlaybackControllerTest.kt
│           ├── PlaybackFailurePolicyTest.kt
│           ├── PlaybackLayersTest.kt
│           ├── PlaybackRecoveryPolicyTest.kt
│           └── SystemMediaVolumeTest.kt        # DG-024（12 个测试；第二十六轮 92/0 已实测）
├── dsp-core/                                 # Gradle 模块 :dsp-core，可移植 C++20
│   ├── build.gradle.kts                      # dspCoreConfigure / dspCoreBuild / dspCoreTest
│   ├── CMakeLists.txt                        # 静态库 dreamguard_dsp_core
│   ├── include/dreamguard/dsp_core.h          # 公共 C99 生命周期 API（DG-010）
│   ├── include/dreamguard/pcm.h               # 公共 C99 Float32 PCM 助手（DG-011）
│   ├── include/dreamguard/meter.h             # 公共 C99 计量器 API（DG-012）
│   ├── include/dreamguard/lufs.h              # 公共 C99 LUFS API（DG-012，PROVISIONAL）
│   ├── include/dreamguard/compressor.h         # 前馈压缩器（DG-013）
│   ├── include/dreamguard/pcm16_processor.h    # PCM16↔float↔压缩器桥（DG-022）
│   ├── src/
│   │   ├── dg_engine.cpp                     # 生命周期实现（无全局状态、无锁）
│   │   ├── lufs.cpp                          # BS.1770-4 式 K 权重 + 门控（PROVISIONAL）
│   │   ├── meter.cpp                         # 流式 RMS / 采样峰值 / 滚动窗口
│   │   ├── pcm.cpp                           # 旁路/增益/声道映射/静音判定（无状态）
│   │   ├── pcm16_processor.cpp                # PCM16 转换 + 压缩器 + bit-exact bypass（DG-022）
│   │   └── soundscape.{h,cpp}                # 声景发生器
│   └── tests/
│       ├── test_dg_lifecycle.cpp              # 生命周期/校验/并发契约
│       ├── test_lufs.cpp                      # LUFS 系数/窗口/门控/分块一致性
│       ├── test_meter.cpp                     # 计量器窗口/峰值/clip/分块一致性
│       ├── test_pcm.cpp                       # PCM 逐位一致性/声道映射/参数校验
│       ├── test_pcm16_processor.cpp           # PCM16↔压缩器/bypass/生命周期/分块（DG-022）
│       └── test_header_c_compat.c             # 纯 C 消费者（四个头）
├── docs/
│   ├── ROADMAP.md
│   ├── STATUS.md                              # 任务状态台账（DONE 与 VERIFIED 分开记）
│   └── DEVLOG.md                              # 开发日志
├── gradle/libs.versions.toml
├── gradle/wrapper/
├── build.gradle.kts
├── settings.gradle.kts                        # include(":app") + include(":dsp-core")
├── gradle.properties
├── gradlew / gradlew.bat
├── .gitignore
├── LICENSE
└── README.md
```

## 构建与测试

前置条件：

- JDK 17（`JAVA_HOME` 指向 17）。
- Android SDK Platform 35、Build-Tools 35.0.0、Platform-Tools。
- NDK `27.0.12077973`、CMake `3.22.1`（Android Studio SDK Manager 安装）。
- 环境变量 `ANDROID_HOME` 或 `ANDROID_SDK_ROOT` 指向 SDK。

```bash
# 项目根目录：DreamGuard/
./gradlew testDebugUnitTest
./gradlew assembleDebug
./gradlew assembleRelease
```

Windows PowerShell：

```powershell
.\gradlew.bat testDebugUnitTest
.\gradlew.bat assembleDebug
.\gradlew.bat assembleRelease
```

> **路径含单引号时的已知环境问题**：如果 Windows 工作区路径包含 `'`（例如用户名 `A'maris`），Gradle 把超长测试 classpath 写入 Java `@argfile` 时，JDK 会把单引号当作引号解析，导致测试 worker 启动即报 `ClassNotFoundException: ...GradleWorkerMain`。可将工作区映射到无引号的短路径后构建（例如 `subst X: <workspace>`，再从 `X:\DreamGuard` 执行），这不是项目代码缺陷。

原生 DSP 核心测试（DG-010 生命周期 + DG-011 PCM 助手，宿主 CMake/CTest，不需要手机或模拟器）：

```powershell
# 一次性 configure + build + ctest
.\gradlew.bat :dsp-core:dspCoreTest

# 或直接用 CMake
cmake -S .\dsp-core -B .\dsp-core\build\native -DCMAKE_BUILD_TYPE=Debug -DDREAMGUARD_DSP_BUILD_TESTS=ON
cmake --build .\dsp-core\build\native --config Debug
ctest --test-dir .\dsp-core\build\native --output-on-failure -C Debug
```

`cmake` / `ctest` 需在 `PATH` 中；不在时用 `-PdspCmake=<path>` / `-PdspCtest=<path>` 覆盖。

> **结构迁移后先清理**：`dsp-core` 拆分改变了原生源码位置，`app/.cxx` 里可能残留指向旧 `dsp_core.cpp` 的 ninja 规则。
> 第一次构建前执行 `.\gradlew.bat clean`（或删除 `app/.cxx`、`app/build`）。

产物：

- 单元测试报告：`app/build/reports/tests/testDebugUnitTest/index.html`
- Debug APK：`app/build/outputs/apk/debug/app-debug.apk`

APK 默认打包 `arm64-v8a`、`armeabi-v7a`、`x86_64` 三个 ABI，因此模拟器也能加载原生库。

## 架构

```text
Compose UI → PlaybackService（媒体会话、前台服务、共享音频焦点）
  ├─ PlaybackController        (纯 Kotlin 状态机 + 参数清洗)
       ├─ AudioEngine         (接口，JVM 可替换为 FakeAudioEngine)
       │    └─ NativeAudioEngine
       │         ├─ AudioTrack（48 kHz / 单声道 / float）
       │         └─ JNI → libdreamguard_dsp.so → dsp-core（C API）→ SoundscapeRenderer
  └─ ExoPlayer（内置 dream_loop.wav，或经系统文件选择器打开的本地音频；
       本地音频可挂 LocalAudioProcessor：PCM16 → native 压缩器，关闭/失败时逐字节旁路）

界面绑定服务；服务负责释放播放器、媒体会话和原生引擎。
```

**系统媒体音量（第二十五轮实现、第二十六轮构建与模拟器基础验证，DG-024，未完成验收）**：`SystemVolumeController`（纯 Kotlin）通过 `MediaVolumeBackend`
读写系统 `STREAM_MUSIC` 通道；`AudioManagerVolumeBackend` 只调用公开音量 API（无新增权限、无
`requestAudioFocus`、flags=0 不弹系统音量面板），`SystemVolumeObserver` 观察公开的
`content://settings/system` 表以吸收硬件音量键、系统面板和其他应用造成的外部变化。音量卡在 `ON_START`
注册观察者并刷新、在 `ON_STOP` 反注册，Composition 晚于 `ON_START` 时补一次注册；整个过程不经过
DSP/音频线程，也不启动、暂停或抢占第三方播放器。该卡在播放服务尚未连接时也渲染，打开应用不会自动播放。
**注意**：`STREAM_MUSIC` 是共享媒体通道，调节必然同时影响其他媒体应用；本原型不做按应用分流、音频捕获
或动态压缩。第二十六轮主代理已构建并通过 **92/0** JVM 测试，在 API 35 x86_64 模拟器验证未播放时
33%（5/15）→20%（3/15）；第三方播放器共存、外部同步、固定输出/蓝牙/真机仍未验证。音量 API 的权限与固定音量说明见
[`AudioManager.setStreamVolume`](https://developer.android.com/reference/android/media/AudioManager#setStreamVolume(int,%20int,%20int))。

**本地音频（第十九至二十三轮）**：`PlaybackService` 复用同一个 `ExoPlayer` 播放系统文件选择器返回的 `content://` URI
（`OpenDocument` + 持久化读授权，进程重建后校验授权恢复）；本地模式与内置 `dream_loop` 互斥，
暂停保留媒体会话。`LocalAudioRenderersFactory` 通过 `DefaultAudioSink.Builder.setAudioProcessors`
把 `LocalAudioProcessor` 注入解码链路：仅接受 16-bit PCM；1–2 声道经 JNI 进入
`libdreamguard_dsp.so` 的 `dg_pcm16_processor`（PCM16→float→DG-013 压缩器→PCM16，默认 threshold −18 dBFS、
2:1、6 dB soft knee、20 ms attack、250 ms release、stereo linked、makeup −6.02 dB），>2 声道**不混音**、
原样直通。开关关闭、shared library 缺失、句柄创建失败、处理失败、非 direct buffer 时一律逐字节复制
（bit-exact，含奇数尾字节），并把状态显示在格式提示里；本地播放不会因 DSP 故障静音。
`configure` 只记录 pending 格式，`flush`（格式生效点）才创建/替换 native 句柄并重置压缩器，
同格式 flush 只 reset；`reset`/`close` 释放句柄（`close` 在 `player.release()` 后调用，并终止处理）。
第二十三轮主代理实测：`--offline :app:testDebugUnitTest :app:assembleDebug` 成功、**80 JVM tests / 0 failures /
0 skipped**、Debug 三 ABI 编译通过；API 35 x86_64 模拟器 `dg_pcm16_processor` **206/0**、compressor **348/0**。
**第二十四轮**同一模拟器安装当前 Debug APK，选 40 s/44.1 kHz/mono PCM16 WAV，UI 播放中并显示“本地压缩就绪”，
开关 `checked=true`，MediaSession `PLAYING`、进程存活、无 FATAL/UnsatisfiedLinkError。
但 JVM fake 后端与“就绪”状态**不等于**实测 JNI/音频输出；第二十轮模拟器 68/0/0 与第二十一轮 iQOO Z9T
真机“正常”对应**旧 Kotlin −6 dB 链路**，不能当作 native 压缩器已运行/已听感验证。
详见 `docs/STATUS.md` 的 DG-020/DG-022。

### DSP 设计

- 双正弦 drone：基础频率 `f` 与 `1.5f`，默认 110 Hz。
- 粉红噪声：Paul Kellet 算法 + xorshift32，确定性种子，便于复现。
- 音色：RBJ biquad 低通，默认 1.2 kHz，范围 80 Hz–12 kHz。
- 呼吸感：0.05 Hz LFO，20 秒一个周期。
- 淡入：2 秒线性淡入，避免起播爆音。
- 软限幅：`tanh(1.1x) / tanh(1.1)`，保证输出绝对值小于 1。
- 实时安全：`render()` 内无分配；参数用 `std::atomic` 从 UI 线程传入，滤波器系数只在音频线程块边界重算。

实现位置：声景发生器与**生命周期 C API**（`dg_create` / `dg_destroy` / `dg_reset`，DG-010）都在独立模块
`dsp-core/`，编译为静态库 `dreamguard_dsp_core`，不依赖 Android。`app` 侧只剩 `jni_bridge.cpp` 一层薄桥，
通过 `add_subdirectory` 复用同一份源码，因此宿主测试与 Android 构建测试的是同一实现。

> 边界：
> - `dreamguard/dsp_core.h` 是声景发生器 + 生命周期接口，**不是** PCM 处理器（没有输入流、重采样、格式转换）。
> - `dreamguard/pcm.h`（DG-011）是**无状态** Float32 缓冲助手：逐位旁路、增益、mono/stereo 映射、静音判定。
>   它**不是采集实现**（无 AudioRecord/OpenSL/AAudio/麦克风路径），目前也**未接入播放链路**，不改变现有音频行为。
>   缓冲别名是公开契约：同指针原地只允许样本数不变的操作（旁路、增益、等声道映射），声道转换与任何部分重叠
>   返回 `DG_ERR_INVALID_ARGUMENT` 且不写样本。
> - DG-011 的验收测试已在主代理机器上跑通**别名修正后的最新代码**：`dg_pcm_tests` → **139 checks / 0 failures**
>   （24 组，API 35 模拟器）。状态为 `DONE`，**不标 `VERIFIED`**：PCM 助手未接入播放链路、未处理真实音频，
>   泄漏检测与真机回归也未做。详见 `dsp-core/README.md` 与 `docs/STATUS.md`。

### JNI / CMake 命名契约

以下名称必须保持一致，改动时三处同步：

| Kotlin（`NativeAudioEngine` 的 `private external`，由 `JniDspCommands` 调用） | C++ JNI 符号 | 位置 |
| --- | --- | --- |
| `nativeCreate(int, int)` | `Java_com_dreamguard_app_audio_NativeAudioEngine_nativeCreate` | `jni_bridge.cpp` |
| `nativeDestroy(long)` | `Java_com_dreamguard_app_audio_NativeAudioEngine_nativeDestroy` | 同上 |
| `nativeReset(long)` | `Java_com_dreamguard_app_audio_NativeAudioEngine_nativeReset` | 同上 |
| `nativeRender(long, float[], int)` | `Java_com_dreamguard_app_audio_NativeAudioEngine_nativeRender` | 同上 |
| `nativeSetGain(long, float)` | `Java_com_dreamguard_app_audio_NativeAudioEngine_nativeSetGain` | 同上 |
| `nativeSetToneCutoff(long, float)` | `Java_com_dreamguard_app_audio_NativeAudioEngine_nativeSetToneCutoff` | 同上 |
| `nativeSetNoiseMix(long, float)` | `Java_com_dreamguard_app_audio_NativeAudioEngine_nativeSetNoiseMix` | 同上 |
| `nativeSetDroneHz(long, float)` | `Java_com_dreamguard_app_audio_NativeAudioEngine_nativeSetDroneHz` | 同上 |
| `nativeVersion()` | `Java_com_dreamguard_app_audio_NativeAudioEngine_nativeVersion` | 同上 |

- Kotlin 库名：`NativeAudioEngine.LIBRARY_NAME = "dreamguard_dsp"`
- CMake 目标 / 输出库：`dreamguard_dsp`
- CMake 路径：`app/src/main/cpp/CMakeLists.txt`
- 包名：`com.dreamguard.app`（JNI 符号前缀 `Java_com_dreamguard_app_...`）
- 桥接层：`jni_bridge.cpp` 只调用 `dsp-core` 的 C API（`dg_create`/`dg_destroy`/`dg_reset`/`dg_render`/`dg_set_*`/
  `dg_version`/`dg_channel_count`），不再直接持有 C++ 对象。**符号名与库名是硬契约，重构时不得改动**。
  创建失败仍返回 `0`，`render` 容量不足仍返回 `-1`，与 Kotlin 侧既有判断保持一致。

如果缩短包名、移动 `NativeAudioEngine` 或改库名，必须同步修改 `jni_bridge.cpp` 中的函数名与 `CMakeLists.txt`，否则运行时会抛 `UnsatisfiedLinkError`。

`nativeXxx` 必须保持 `private`：Kotlin 会给 `internal` 函数名追加模块后缀（Debug 为 `nativeCreate$dreamguard_debug`，Release 为 `nativeCreate$dreamguard_release`），与上面的 C++ 符号不再匹配。这种错误 `System.loadLibrary` 不会报错，只在第一次调用原生方法时抛 `UnsatisfiedLinkError`，而且两种构建类型的符号名还不一样。`JniContractTest` 在两个层面上守住这条契约：反射检查编译产物的方法名没有 `$` 后缀，并解析 `jni_bridge.cpp` 与本类声明的原生方法逐一比对。若将来开启 R8（当前 release 未开启 `isMinifyEnabled`），还需要为原生方法添加 keep 规则。

## 测试

`app/src/test` 中的测试不加载 NDK，也不需要模拟器：

- `AudioSettingsTest`：默认值、越界钳制、NaN/±Infinity 回退。
- `PlaybackControllerTest`：启动参数下发、不可用引擎错误、参数钳制、toggle 状态、release 终止性、监听通知、启动失败错误、异步失败/停止回调、失败重启后清除错误、每次失败只计数一次、延迟启动标记 `retryableStart`（不算失败、不计数）、`abandonDeferredStart` 只通知一次、成功/停止清除延迟标记、`failureKind` 透传。
- `NativeAudioEngineTest`：用可注入的 `DspCommands` / `RenderOutputFactory` 替身验证——停止在渲染线程阻塞时的耗时上限、停止时不在调用线程上调用 `AudioTrack` 的 pause/flush/stop、原生句柄在渲染线程退出前不被销毁、延迟销毁、渲染失败作为 `ERROR` 上报并带失败分类、失败重启后清除错误、上一会话未退出时拒绝重入且复用同一句柄/输出、连续零帧写入有界失败（不无限忙等）、HAL 卡住时重复 `stop()` 只创建一个唤醒线程、输出打开失败为 `TRANSIENT` 且重试复用句柄、release 幂等。
- `PlaybackLayersTest`：两层选择互相独立、`ambientVolume` 只跟随环境音选择、更新返回新值。
- `PlaybackFailurePolicyTest`：异步 DSP 失败时是否暂停媒体播放器的决策矩阵——仅 DSP 层要暂停、两层同时选中不暂停（保留环境音）、仅环境音与无选择都不受影响；以及只有显式用户暂停才结束服务、内部失败清理不结束服务。
- `PlaybackRecoveryPolicyTest`：延迟启动的重试矩阵——仅 DSP 层且 `playWhenReady` 时尝试启动、已在运行不重复启动、延迟启动在预算内重试、达到 6 次上限停止、真实失败不按延迟启动重试、用户取消（`playWhenReady=false`）或仅环境音时不重试。
- `JniContractTest`：反射检查原生方法名没有 Kotlin 模块后缀（Debug/Release 都不允许），并解析 `jni_bridge.cpp` 的导出符号与类声明的原生方法逐一比对，防止 JNI 命名回退。
- `SystemMediaVolumeTest`（DG-024，12 个）：系统媒体音量的读取范围与越界钳制、固定音量设备只读且不写、单级输出不可调、读失败降级、写入异常、平台静默忽略写入、外部变化只通知一次且空刷新不通知、成功/失败写入的发布行为、失败恢复。**第二十六轮主代理实测：92 tests / 0 failures / 0 skipped（含本类 12 个）**。

原生测试在 `dsp-core/tests`（CTest 注册，宿主执行，不需要 NDK 或手机）：

- `dg_lifecycle`：`dg_create` 校验矩阵与边界值（采样率 8000/44100/48000/192000 × 1/2 声道）、创建失败清零出参、
  并存句柄互不相同、所有 `NULL` 入口返回 `DG_ERR_NULL_HANDLE`、`dg_render` 参数校验与输出形状、
  `dg_reset` 后首块与新建引擎首块按位相同、reset 保留参数、setter 钳制与 NaN/Infinity 回退、
  “setter 与 render 并发”冒烟、“4 线程各自 create/render/destroy 且首块与参考引擎逐字节相同”、200 轮确定性。
- `dg_pcm`（24 组，最新代码 139 checks / 0 failures）：Float32 逐位旁路（含带 payload 的 NaN、signed zero、denormal）、gain 1.0 逐位等于旁路、
  越界 gain 被拒且不写输出、mono↔stereo 映射、静音判定、0 帧块、1/2/3/63/64/65/1024/4096 帧 × 1/2 声道旁路恒等；
  缓冲别名契约（同指针原地允许、部分重叠拒绝且缓冲不变、声道转换禁止别名）、65 536 帧立体声大块往返。
- `dg_header_c_compat`：以纯 C 编译并运行一次最小生命周期 + 一次 PCM 旁路 + 一次 meter 采样 + 一次 LUFS 采样，
  保证四个公共头在 C 下可用。
- `dg_meter`（10 组，**第七轮在 x86_64 模拟器首跑：207 checks / 0 failures**）：create 校验矩阵（7999/192001 Hz、0/3 声道、window −1/1001 ms）、
  NULL 句柄、拒绝不改句柄、DC 1.0 → 0 dBFS、±0 静音、1 kHz 峰值 0.5 正弦 RMS 0.353553、立体声声道独立、
  40 ms 窗口满窗/滑出、clip 与非有限计数、`denorm_min` 次正规不因下溢归零、任意分块与整块在流态字段上逐位一致
  （`block_*` 是“最近一次调用”的统计，故不参与该比较）、`reset`/`reset_peak_hold`、dBFS 边界。
- `dg_lufs`（12 组，**第七轮在 x86_64 模拟器首跑**）：48 kHz K 权重系数对照 BS.1770-4 公布表（1e-6）、8k–192k 稳定、
  1 kHz 正弦 integrated 与解析值一致（容差 0.1 LU，已登记 ≈0.024 LU 系统偏差）、momentary/short/integrated 关系、
  单声道比立体声低 10log10(2)、窗口未就绪状态、静音、门控两侧（4 s 0.1 + 1 s 静音 + 2 s −50 dBFS 需剔除 quiet 块；
  4 s 0.1 + 2 s −9 dB 需全部保留）、NaN 计数、分块一致性。
  首跑为 **227 checks / 2 failures**，两处失败经定位为 gating 用例漏算单声道 3.0103 LU（测试期望错误），
  已修正并新增一个反向回归。**修复后复测：238 checks / 0 failures**（12 组全过；gating 诊断
  `blocks=67 after_abs=61 used=40 integrated=-23.1696`）。

> 这些测试**不是**泄漏检测，也不检测竞态：仓库没有 ASan/LSan/TSan 作业，"无资源泄漏"至今未验证（见 `docs/STATUS.md`）。
> 它们也刻意**不测**契约外的行为（二次 destroy、销毁后使用、销毁与渲染并发）——那是未定义行为，库不检测。
> **DG-012 由主代理在本机验证**：x86_64 在 NDK + API 35 模拟器上 `dg_meter` 207/0、`dg_lufs` 238/0、
> 扩展后的 `dg_header_c_compat` PASSED；Gradle `assembleDebug assembleRelease` 让 Debug/Release 三 ABI
> 全部构建通过（arm ABI 只编译未运行）。参考工具对照已在第十五/十八轮补上（ffmpeg 9.0.2 `ebur128=metadata=1`，
> 14 个可比样本 PASS、最大显示差 0.010 LU，2 例 NO-DATA），但只在该参考约 0.01 LU 分辨率内成立；仍
> 未做：真机、ASan/LSan、宿主 `ctest`、`--libebur128` 独立复核。

## CI

`.github/workflows/android-ci.yml` 在 Ubuntu 上：

1. Temurin JDK 17；
2. 安装 `platforms;android-35`、`build-tools;35.0.0`、`ndk;27.0.12077973`、`cmake;3.22.1`；
3. `bash ./gradlew --no-daemon testDebugUnitTest`；
4. `bash ./gradlew --no-daemon assembleDebug`；
5. 上传 `app-debug.apk` 构件。

CI 使用 `bash ./gradlew`，因此不依赖 `gradlew` 的可执行位。CI 只构建 Debug，不做签名或发布。

## iQOO Z9T 真机联调检查清单

以下按 iQOO Z9T / OriginOS 的常见入口编写。不同系统版本菜单名称可能略有差异。

### 1. 电脑侧 Android Studio / SDK

- [ ] 安装 Android Studio（Ladybug 2024.2.1 或更新版本）。
- [ ] `Settings → Build, Execution, Deployment → Build Tools → Gradle → Gradle JDK` 选择 **JDK 17**（不要用 Android Studio 自带 JBR 21 以外的未知版本；本项目按 17 编译）。
- [ ] SDK Manager 勾选并安装：
  - [ ] Android 15（API 35）Platform
  - [ ] Android SDK Build-Tools 35.0.0
  - [ ] NDK (Side by side) **27.0.12077973**
  - [ ] CMake **3.22.1**
  - [ ] Android SDK Platform-Tools
- [ ] 确认 `adb` 可用：`adb version`。
- [ ] 确认 `ANDROID_HOME` / `ANDROID_SDK_ROOT` 指向 SDK 根目录（或在 `local.properties` 写 `sdk.dir`，该文件不入库）。
- [ ] 确认命令行构建：`.\gradlew.bat :app:assembleDebug` 成功。

### 2. 手机侧开发者选项

- [ ] `设置 → 关于手机 → 软件版本信息`，连续点击“软件版本号”约 7 次，开启开发者模式。
- [ ] `设置 → 系统管理 → 开发者选项`，打开 **USB 调试**。
- [ ] 同一页面打开 **USB 调试（安全设置）**。vivo/OriginOS 安装 ADB 推送的 APK 通常需要这一项；系统可能要求登录 vivo 账号或重启。
- [ ] 如存在 **“USB 安装监控 / 监控 ADB 安装应用”**，临时关闭，否则 `adb install` 可能被拦截。
- [ ] `默认 USB 配置` 选择 **文件传输 / MTP**，不要选“仅充电”。

### 3. USB 连接与 adb 识别

- [ ] 使用支持数据传输的 USB 线连接电脑；优先用主板后置 USB 口。
- [ ] Windows 如识别不到设备，安装 vivo 官方 USB 驱动，或在设备管理器更新 Android ADB Interface 驱动。
- [ ] 执行 `adb devices -l`：
  - 期望输出包含 `<序列号>  device`。
  - `unauthorized`：在手机弹窗点“允许”，勾选“始终允许”，必要时 `adb kill-server` 后重插。
  - `offline`：重新插拔、换线；仍失败则撤销 USB 调试授权后重新授权。
  - 列表为空：检查 USB 配置、驱动、开发者选项是否真的开启。
- [ ] 可选：`adb shell getprop ro.product.cpu.abi` 应返回 `arm64-v8a`（本项目已打包该 ABI）。

### 4. 安装与运行

```powershell
# 方式 A：Gradle 安装
.\gradlew.bat :app:installDebug

# 方式 B：手动安装
adb install -r app\build\outputs\apk\debug\app-debug.apk

# 启动
adb shell am start -n com.dreamguard.app/.MainActivity
```

- [ ] 首次启动不崩溃，界面显示 “DREAMGUARD / 沉入睡眠”。
- [ ] 状态文案显示 `运行中 · DSP v0.1.0`（说明 `libdreamguard_dsp.so` 已加载）。
- [ ] 点击中央圆形按钮，能听到低频声景；再次点击停止。
- [ ] 拖动音量、音色亮度、噪声音量、低频频率，声音应有可辨变化。
- [ ] 点击“播放环境音轨（Media3）”，内置循环应开始播放，再次点击停止。
- [ ] 按 Home 键 / 锁屏后观察是否继续播放，并检查媒体通知及暂停按钮。
- [ ] 查看日志：`adb logcat -s DreamGuard AndroidRuntime`，确认无 `UnsatisfiedLinkError`、`AudioTrack` 初始化失败。

### 5. 常见问题

| 现象 | 排查 |
| --- | --- |
| `UnsatisfiedLinkError: dlopen failed ... dreamguard_dsp` | 检查 APK 是否含对应 ABI 的 `.so`：`unzip -l app-debug.apk | findstr dreamguard`；清理后 `assembleDebug`；确认 JNI 符号/包名未改动。 |
| 点击播放无声音 | 手机媒体音量是否为 0；`adb logcat` 是否有 `AudioTrack` 错误；先试 Media3 音轨区分原生/系统问题。 |
| `adb install` 被拒绝 | 打开“USB 调试（安全设置）”、关闭安装监控、确认已允许 USB 调试。 |
| CMake 版本报错 | SDK Manager 安装 3.22.1；`app/build.gradle.kts` 的 `version = "3.22.1"` 不要写成其他版本。 |
| NDK 找不到 | 安装 27.0.12077973，或同步修改 `ndkVersion` 与 CI 的 `sdkmanager` 参数。 |

## 验证状态

### 2026-11-10 第二十六轮：DG-024 构建、JVM 92/0 与模拟器基础音量验证（主代理执行；更新未提交）

- 主代理执行 `.\gradlew.bat --no-daemon --offline :app:testDebugUnitTest :app:assembleDebug`：
  **BUILD SUCCESSFUL**，测试报告 **92 tests / 0 failures / 0 skipped**。
- API 35 x86_64 模拟器安装并启动；**未启动 DreamGuard 声景**，页面待机，系统媒体音量 **33%（5/15）**；
  用“系统媒体音量”卡滑块调到 **20%（3/15）**，`dumpsys audio` 记录
  `setStreamVolume STREAM_MUSIC index 3 oldIndex 5 from com.dreamguard.app`，页面显示 **20%**。
- **能证明**：Debug 构建与 JVM 测试通过；不播放 DreamGuard 时音量卡能读取并写入真实 `STREAM_MUSIC`
  通道，系统音频服务接受写入，界面与系统索引一致。
- **不能证明**：第三方播放器共存/不被打断、实际声音电平或听感、外部音量变化同步（模拟器 adb 调节未
  生效，未可靠验证）、音量边界与固定音量/单级输出 UI、蓝牙/固定输出/iQOO Z9T 真机、音频焦点行为；
  也不证明原生音频实时压缩或单应用音量。
- 回归边界：本轮**未启动 DreamGuard 声景**，也没有本地音频/DSP 播放冒烟；“现有播放不回归”目前只由
  **92/0** JVM 测试支持，不能声称已在设备上回归。
- 状态仍 `IN_PROGRESS`：路线图验收要求的第三方播放器、实际声音、边界/不可调状态、外部同步、真机未完成。
  **第二十五/二十六轮更新尚未提交、未推送**；用户要求本轮完成后暂停，提交与推送由主代理后续同步。

### 2026-11-10 第二十五轮：DG-024 系统媒体音量控制原型（代码写入；第二十六轮已补构建/测试）

- 新增 `app/src/main/java/com/dreamguard/app/audio/SystemMediaVolume.kt`（纯 Kotlin 范围/固定/失败/外部变化
  规则）、`AudioManagerMediaVolume.kt`（`STREAM_MUSIC` 读写 + `Settings.System` 观察）、
  `app/src/test/java/com/dreamguard/app/audio/SystemMediaVolumeTest.kt`（12 个 JVM 测试），并在
  `DreamGuardScreen.kt` 加“系统媒体音量”卡；原 DSP 增益卡改名“声景音量”。
- **本会话 `pwsh` 与 terminal 在启动阶段失败**：`Error: SetNamedSecurityInfoW failed (Win32 5):
  grantWrite(E:\DreamGuard)`；按用户要求未改 ACL、未申请 `danger-full-access`。因此**没有构建、没有运行
  JVM 测试、没有 APK、没有模拟器或真机结果**。12 个测试当时**尚未执行**，不得写成“通过”（第二十六轮已补执行，见上节）。
- 当时待主代理执行并回填：`.\gradlew.bat --no-daemon --offline :app:testDebugUnitTest` 与
  `.\gradlew.bat --no-daemon --offline :app:assembleDebug`（当时预期 92 个 JVM 测试）。**第二十六轮已执行：
  BUILD SUCCESSFUL、92 tests / 0 failures / 0 skipped，见上节。**
- 模拟器/本机验证建议：安装后**不按播放**，直接拖动“系统媒体音量”滑块；`adb shell dumpsys audio` 的 focus
  stack 在此之前不应出现 `com.dreamguard.app`；让另一媒体应用后台播放并确认调音量时它继续播放；再用音量键
  改变系统音量，确认界面百分比跟随。
- 回归边界：本轮未改 `PlaybackService` / `PlaybackController` / DSP / `LocalAudioProcessor`，改动是新增文件、
  在 UI 插入音量卡（服务未连接分支也放一张）、把原 DSP 增益卡标题“音量”改为“声景音量”；但“不回归”仍需
  主代理跑完整 JVM 测试与一次模拟器启动/播放冒烟后才能判定。
- 状态 `IN_PROGRESS`：验收要求的 iQOO Z9T + 指定第三方播放器实测按用户要求延后；不声称第三方音频捕获、
  按应用分流或动态压缩已实现。

### 2026-11-10 第二十四轮：API 35 x86_64 模拟器安装当前 Debug APK（播放状态 / 本地压缩就绪）

- 代码已提交并推送 `4b7ace322da31f06417f3dc09a591795ba14207f`（短 `4b7ace3`）；本次日志/状态更新未提交。
- 主代理在 API 35 x86_64 模拟器安装并启动当前 Debug APK；系统文件选择器选择合成 **40 s / 44.1 kHz /
  单声道 PCM16 WAV**；UI 显示播放中，格式提示“本地压缩就绪”，打开处理开关后 `checked=true`。
- `MediaSession` 为 `PLAYING`；进程存活；`AndroidRuntime:E` 日志未见 `FATAL EXCEPTION` / `UnsatisfiedLinkError`。
- **能证明**：应用可安装/启动/选择本地 PCM16 文件并进入播放状态；“本地压缩就绪”（`NativeState.READY`）
  说明 native 库加载与句柄创建路径未报错。
- **不能证明**：逐块 `nativeProcess` 调用/JNI 返回值正确、实际音频输出电平、听感、延迟/underrun、泄漏、
  真机新版压缩通过；未观察到 `ACTIVE`，也没有波形/录音/逐块日志。真机与 30 分钟长稳按用户要求延后。

### 2026-11-10 第二十三轮：DG-022 契约修正、Debug 构建与原生测试实测

- 对照 media3 **1.5.1** 官方源码复核 `BaseAudioProcessor`：`onFlush()` 返回 **void**（第二十二轮的
  `override fun onFlush(): Boolean` 会编译失败，已修正）；`flush()` 由基类清空待输出并应用 pending format。
- native 句柄生命周期改为 **configure 只记录 pending、flush 时创建/替换**：media3 允许 configure 之后、
  flush 之前继续排队旧格式 buffer，此时旧 buffer 仍使用旧句柄；同格式 flush 只 `reset` 压缩器。
- `nativeCapable` 纳入失败状态；新增 `layoutSupported`/`nativeLibraryAvailable`/`nativeFailure`，格式提示在
  开关关闭时也明示多声道/缺库/失败直通；已开启的开关始终可关闭。回退同时回卷输入 position；`close()` 终止处理。
- 首轮 native 交叉编译发现 `dg_compressor_process` 输出参数类型不一致（`dg_pcm_view` vs `dg_pcm_buffer`）：
  已把 compressor 输出统一为 `dg_pcm_buffer`、删除残留 `const_cast`，并同步 compressor/PCM16 测试与 C 头兼容。
- **实测（主代理）**：`.\gradlew.bat --no-daemon --offline :app:testDebugUnitTest :app:assembleDebug` **成功**，
  **80 JVM tests / 0 failures / 0 skipped**，Debug NDK 三个 ABI 编译通过；Android API 35 x86_64 模拟器
  `dg_pcm16_processor_tests` **206 checks / 0 failures**（27 组）、`dg_compressor_tests` **348 checks / 0 failures**。
- **边界**：第二十三轮未安装/未运行 APK，没有真实 JNI 调用、音频输出、波形/听感；JVM JNI 合约与 mock
  **不等于**实测 JNI；未做 true peak、长时间听感、多设备；用户明确长时间真机测试暂缓。
  第二十四轮已安装运行到“播放中 + 本地压缩就绪”，但仍不证明逐块 native 处理或真实音频输出。
  **第二十三轮代码已随 `4b7ace3` 推送；第二十四轮日志/状态更新未提交**。

### 2026-11-10 第二十二轮：DG-022 本地 PCM16 → native 压缩器（功能构建；第二十三轮已补构建/测试）

- 新增 `dsp-core/include/dreamguard/pcm16_processor.h` + `src/pcm16_processor.cpp`：PCM16→float→DG-013
  压缩器→PCM16，512 帧定长 scratch、处理路径无逐样本分配、bypass 用 `memmove` 位精确、残帧/奇字节原样、
  半远离零量化 + 饱和、无 lookahead；新增 24 组 C++ 测试（CTest `dg_pcm16_processor`）。
- 新增 `NativePcmProcessor` 5 个 JNI 符号与 `NativePcmProcessorContractTest`；`LocalAudioProcessor` 改为
  native 主路径：native 字节数完全匹配才采用，开关关闭/缺库/句柄失败/处理失败/多声道/非 direct 全部
  逐字节回退并显示状态；`flush` reset、format 变化重建句柄、`reset`/`close` 释放；UI 开关改名“本地压缩”。
- **本会话命令环境全部失败**（`SetNamedSecurityInfoW failed (Win32 5): grantWrite(E:\DreamGuard)`）：
  没有构建、没有 C++/JVM 测试、没有 APK、没有模拟器/真机；**不声称 native 压缩器已实际运行**，
  也不声称音质、响度、延迟或 true peak 已通过。构建与测试由主代理执行，结果需回填
  `docs/STATUS.md` / `docs/DEVLOG.md`；第二十一轮真机“正常”对应旧 Kotlin −6 dB 链路，不能移用。

### 2026-11-10 第二十一轮：iQOO Z9T 真机本地音频（用户听感确认）

- **由主代理安装并在真机操作，用户本人回答**（不是本会话执行）：iQOO Z9T（ADB `model` `V2352A`、
  `manufacturer` `vivo`、Android 16 / API 36）安装当前 Debug APK 成功——`versionName 0.1.0`、`versionCode 1`、
  SHA256 `3ABD75F122320CCE3A7BD1BD493FE6B0A867C1B8996296DD6E44621C8F4C97D2`，启动进程 pid **22780**；
  该 APK 与此前同批构建、**68 tests / 0 failures** 的 JVM 结果对应（本轮未重新构建）。
- 用户在 Downloads 用系统选择器打开主代理生成的 40 s / 44100 Hz / 单声道 PCM16 WAV；对“声音、−6 dB 开关变化、
  暂停恢复、爆音卡顿”的**合并询问**答复“正常”。另答复：锁屏约 15 秒声音持续；播放其他媒体后 DreamGuard 暂停；
  关闭本地音轨后原有 DSP 声景播放，调音量/音色与暂停正常、无爆音卡顿。
- 日志侧（主代理检查）：进程存活、MediaSession `PAUSED`；过滤 `AndroidRuntime:E` 与 `nativeloader` 未见
  `FATAL EXCEPTION` 或 `UnsatisfiedLinkError`；**未见 nativeloader 成功行**，以实际出声作间接证据。
- **边界**：不把“正常”拆成四项量化结论；C++ PCM DSP 未接入（−6 dB 是 Kotlin 整数增益）；
  未做 30 分钟、多路由（蓝牙/有线/外放）、多格式、进程重建、真实峰值/波形；`DG-021/022/023` 仍 `IN_PROGRESS`，
  不标 `DONE`/`VERIFIED`。详细台账见 `docs/STATUS.md` 的“第二十一轮真机验证”节。

### 2026-11-10 第六轮：DG-012 计量与 LUFS 代码（本会话未执行任何构建或测试）

- 新增 `dsp-core/include/dreamguard/meter.h`、`dsp-core/include/dreamguard/lufs.h` 与对应实现，以及
  `dsp-core/tests/test_meter.cpp`（10 组）、`dsp-core/tests/test_lufs.cpp`（当时 11 组，第七轮增到 12 组）。
  契约与设计限制见 `docs/STATUS.md` 的 DG-012 节。
- **本会话 `pwsh` 在启动阶段失败**：任何命令（包括 `pwd`）返回
  `Error: SetNamedSecurityInfoW failed (Win32 5): grantWrite(E:\DreamGuard)`，命令体从未执行。
  因此本轮**没有编译、没有运行 `ctest`、没有 Gradle 构建、没有模拟器/真机结果**，也没有与 libebur128、
  ffmpeg 等参考工具对比 LUFS 误差。
- 三 ABI 兼容只有**静态核对**：`app/build.gradle.kts` 的 `abiFilters` 为 `arm64-v8a` / `armeabi-v7a` / `x86_64`，
  新源文件并入同一个 `dreamguard_dsp_core` 目标；未做任何交叉编译。
- 状态：DG-012 = `IN_PROGRESS`（不标 `DONE`/`VERIFIED`）。LUFS 在头文件与文档中标为 **PROVISIONAL**，
  不声称标准兼容。

### 2026-11-10 第八轮：DG-012 修复后复测通过 + 三 ABI 构建（仅更新文档）

- **由主代理执行**（不是本会话）：修复后的 `dg_lufs_tests` 在 x86_64 / API 35 模拟器上
  **238 checks / 0 failures**；gating 诊断 `blocks=67 after_abs=61 used=40 integrated=-23.1696`。
  `−23.1696` 与解析预期 `−23.0103` 差 0.159 LU，来源是 3 个过渡块（约 0.17 LU）叠加 0.024 LU 校准偏差，
  与第 7 轮预估一致。
- 同轮：`dg_meter` **207/0**、扩展后的 `dg_header_c_compat` **PASSED**；
  `testDebugUnitTest assembleDebug assembleRelease` 成功，Debug/Release 三种 ABI 构建通过，JVM `up-to-date`。
- 仍未验证：真机、ASan/LSan、宿主 `ctest`、与 libebur128/ffmpeg 的误差对比（本机 PATH 无 ffmpeg/ffprobe）；
  arm64-v8a / armeabi-v7a 只编译未运行。DG-012 仍 `IN_PROGRESS`，LUFS 仍 `PROVISIONAL`。

### 2026-11-10 第七轮：DG-012 首跑（x86_64 模拟器）与 gating 修复

- **由主代理执行**（不是本会话）：NDK 交叉编译 x86_64，API 35 模拟器运行
  → `dg_meter` **207 checks / 0 failures**；`dg_lufs` **227 checks / 2 failures**。
- 两处失败都在 `dsp-core/tests/test_lufs.cpp` 的 gating 用例。根因是**测试期望错误**：单声道 0.1 峰值正弦
  应读 `20log10(0.1) − 10log10(2) = −23.01 LUFS`，原断言用了 −20。门限算法未改（相对门 = 过绝对门块均值 −10 LU，
  quiet 块低 68 倍被正确剔除，`blocks_used == 40`）。
- 修正：期望改为 `20log10(0.1) − 10log10(2)`，计数断言收紧为 `blocks == 67` / `blocks_used == 40`；
  新增 `test_relative_gate_keeps_near_blocks()`（−9 dB 段必须保留，57 块全过门）作为反向回归。
  该轮结束时**尚未重跑**；第八轮复测为 238/0。
- 本轮遗留（第八轮已补上复测与三 ABI 构建）：真机、ASan/LSan、与 libebur128/ffmpeg 的误差对比；
  状态仍为 `IN_PROGRESS`，LUFS 仍 `PROVISIONAL`。

### 2026-11-10 第五轮：主代理复测最新代码（仅更新记录，未改代码）

- **由主代理在本机执行**（不是本会话）：对别名修正后的最新代码跑 `testDebugUnitTest assembleDebug assembleRelease`
  → **BUILD SUCCESSFUL**，三个 ABI 的原生库均构建通过；JVM 测试为 `up-to-date`，未重跑
  （58 tests / 0 failures 仍是第四轮报告的结果）。
- native：NDK 27.0.12077973 交叉编译最新版 `dg_pcm_tests`，在 Android API 35 x86_64 模拟器执行
  → **139 checks / 0 failures / PASSED**，覆盖全部 **24 组**。
- `dg_lifecycle_tests`（1328 checks / 0 failures）与 `dg_header_c_compat` 属于本轮**未修改的部分**，
  沿用此前结果，本轮没有重跑。
- 状态变化：DG-011 → `DONE`（不标 `VERIFIED`）；DG-002 / DG-003 的证据补上第五轮构建；DG-010 不变。
- 仍未验证：泄漏检测（ASan/LSan）、真机回归、PCM 助手与真实音频链路的集成。

### 2026-11-10 第四轮：主代理实测第三轮代码 + PCM 缓冲别名修正

> 后续记录：第五轮（上一节）已复测别名修正后的代码，`dg_pcm` 最新结果为 **139 checks**；
> 本节末尾“尚未编译、尚未运行”与 110 checks 应视为当时的记录。

- **由主代理在本机执行**（不是本会话）：对第三轮代码跑 `clean testDebugUnitTest assembleDebug assembleRelease`
  全部成功，JVM **58 tests / 0 failures**（Gradle 从 build cache 恢复）、Debug 与 Release 均产出。
- native（NDK 27.0.12077973 交叉编译，Android API 35 x86_64 模拟器）：
  `dg_lifecycle_tests` → **1328 checks / 0 failures**（17 组）、`dg_pcm_tests` → **110 checks / 0 failures**、
  `dg_header_c_compat` → **通过**。
- 本轮修正：`dreamguard/pcm.h` 明确**缓冲别名契约**（同指针原地只允许样本数不变的操作；声道转换与任何部分重叠
  返回 `DG_ERR_INVALID_ARGUMENT` 且不写样本），`pcm.cpp` 增加 `in_place()` / `ranges_intersect()`、索引先加宽到
  `std::size_t`，`test_pcm.cpp` 17 → **24 组**，并删除 `settings.gradle.kts` 末尾多余空行。
- **本会话仍未执行任何命令**（`pwsh` 仍报 `Error: SetNamedSecurityInfoW failed (Win32 5): grantWrite(E:\DreamGuard)`），
  因此上述修正后的代码**尚未编译、尚未运行**；`dg_pcm_tests` 的断言数会高于 110。
- 仍未验证：泄漏检测（ASan/LSan）、真机回归、instrumented 断言。

### 2026-11-10 第三轮：DG-010 契约修正 + DG-011 PCM 助手（本会话未执行构建与测试）

- 改动：DG-010 去掉活句柄注册表与全局锁（原设计既有 ABA 问题，又让每个渲染块加锁），改为
  “不透明指针句柄 + 库内零全局状态”；错误码收敛为 6 个；销毁后使用同一句柄明确写为未定义行为、库不检测。
  新增 DG-011 无状态 Float32 PCM 助手（`include/dreamguard/pcm.h`、`src/pcm.cpp`、`tests/test_pcm.cpp`）；
  `jni_bridge.cpp` 的句柄改走 `intptr_t`，9 个 JNI 符号名与返回约定不变。
- **本轮没有执行任何命令**：`pwsh` 在启动阶段即失败
  （`Error: SetNamedSecurityInfoW failed (Win32 5): grantWrite(E:\DreamGuard)`）。没有 `clean`、没有
  `testDebugUnitTest`、没有 `assembleDebug`、没有 `ctest`、没有交叉编译、没有 APK。
- 因此 DG-010 与 DG-011 当时在 [docs/STATUS.md](docs/STATUS.md) 中均为 `IN_PROGRESS`；下方“第二轮”的
  58 tests 与 1524 checks 记录**只覆盖第二轮的代码**，不能当作本轮改动的证据。
  （第四轮已由主代理实测本轮代码，DG-010 升为 `DONE`；第五轮又实测别名修正后的代码，DG-011 也升为 `DONE`。）
- 复核顺序建议：`.\gradlew.bat clean` → `.\gradlew.bat testDebugUnitTest assembleDebug` →
  `.\gradlew.bat :dsp-core:dspCoreTest`（或缺宿主编译器时按 [docs/STATUS.md](docs/STATUS.md)
  的 NDK 交叉编译 + 模拟器路径跑 `dg_lifecycle_tests` / `dg_pcm_tests` / `dg_header_c_compat`）。

### 第二轮（2026-11-10）DG-003/DG-010 结构改动（实际构建并测试）

- 构建：`.\gradlew.bat --no-daemon --stacktrace clean testDebugUnitTest assembleDebug` → **BUILD SUCCESSFUL in 32s**；
  三个 ABI 的 `libdreamguard_dsp.so` 与 `app-debug.apk`（16 479 469 字节）均重新产出。
- JVM 单测：`--rerun-tasks` 强制重跑 → 7 个测试类 **58 tests / 0 failures / 0 skipped**（含 `JniContractTest` 的 9 符号比对）。
- native 生命周期测试：本机没有 MSVC/宿主 C++ 编译器，`:dsp-core:dspCoreTest` 的 cmake 配置即失败
  （`CMAKE_CXX_COMPILER not set`）；改用 NDK 27.0.12077973 交叉编译**同一份** `dsp-core` 源码，在
  模拟器 `emulator-5554`（AVD `ime35`，API 35，x86_64）上执行：
  `dg_lifecycle_tests` → **1524 checks / 0 failure(s) / PASSED**（16 组用例），`dg_header_c_compat` → **PASSED**。
- 端到端冒烟：`adb install` 成功 → `MainActivity` 启动 → `nativeloader: Load .../libdreamguard_dsp.so ... ok`；
  点按播放后 Media3 会话 `state=PLAYING(3)`、持有 `USAGE_MEDIA` 音频焦点、`PlaybackService` 前台服务启动，
  播放 10 秒以上位置持续推进，无崩溃。
- **仍未验证**：泄漏检测（ASan/LSan 未运行）、宿主 `ctest` 路径（缺 MSVC）、真机回归、instrumented 断言。
- 状态台账见 [docs/STATUS.md](docs/STATUS.md)；改动记录见 [docs/DEVLOG.md](docs/DEVLOG.md)。

### 结构改动之前的构建记录

- 环境：Windows、JDK 17.0.21（Microsoft build）、Gradle 8.11.1、Android SDK 35 / Build-Tools 35.0.0、NDK 27.0.12077973、CMake 3.22.1。
- **2026-11-10 主代理在本机执行**（非用户执行）：`.\gradlew.bat testDebugUnitTest assembleDebug assembleRelease`（单条命令，未指定 `--no-daemon`）；**58 项测试通过、0 跳过**，Debug 与 Release 两种构建均成功。该记录对应 `dsp-core` 结构改动**之前**的代码。
- 更早记录：2026-11-10 同样由主代理在本机执行 `.\gradlew.bat testDebugUnitTest assembleDebug assembleRelease`，当时通过的是本轮改动**之前**的 **37 个用例**；再往前 `main` @ `4954671` 上 `clean testDebugUnitTest` 通过（10 个用例）；`assembleDebug` 通过并生成三 ABI APK；GitHub Actions [42613031269](https://github.com/AmariS26242/DreamGuard/actions/runs/42613031269) 对应旧代码通过。
- JVM 单元测试在本地 JVM 执行，不涉及真机或模拟器。2026-11-09 在 V2352A（Android 16）上完成基础安装、原生播放和听感检查。

> 真机部分仍只有下述基础回归；其余见[明确未验证](#明确未验证)。

### 2026-11-10 真机基础回归（V2352A / Android 16）

当前新版 Debug APK 已安装在 V2352A（Android 16）上并能正常启动。本轮由用户实际操作确认：

- [x] **锁屏播放**：DSP 声景在锁屏状态下持续约 20 秒，用户确认无明显卡顿、爆音。
- [x] **进程被结束后重新打开**：用 `adb shell run-as com.dreamguard.app kill -9 <pid>` 模拟进程被结束；重新打开应用后先处于待机（不自动播放），重新启动 DSP 后可正常听到声音。
- [x] **其他媒体播放**：用户确认其他媒体播放时 DreamGuard 暂停（AudioFocus 中断路径）。
- [x] **环境音轨叠加**：用户确认环境音轨可与 DSP 声景叠加听见；关闭环境音后环境音停止，DSP 继续播放。
- [x] **结束状态**：结束时媒体暂停、DSP 音频线程退出；检查的应用进程日志无 `FATAL EXCEPTION`、`UnsatisfiedLinkError`。

> 本轮只覆盖「模拟进程结束并重新打开」的基础回归。**系统主动回收 / 媒体键恢复、来电打断、新版长时运行，以及真实 HAL 异常（`AudioTrack` 永久阻塞）等仍未验证。**

### 明确未验证

- [x] 已在 V2352A 真机上安装和运行，并完成基础声音检查；2026-11-10 又完成新一轮基础回归（锁屏约 20 秒、`kill -9` 后重新打开、其他媒体打断、环境音叠加、结束日志检查，见上节）。
- [x] 2026-11-10 主代理在本机执行最新代码的 58 项 JVM 单测（全部通过、0 跳过）及 `assembleDebug` / `assembleRelease`（两种构建均成功，命令为单条 `.\gradlew.bat testDebugUnitTest assembleDebug assembleRelease`）。
- [x] 上一版 30 分钟后台播放测试：按用户确认视作完成（2026-11-10）。实际监测依赖本机 USB/ADB 采样，约 29 分钟后连接断开；重新连接后确认同一进程仍在播放，故这次是采样观测中断，未被记录为完整 30 分钟连续采样。
- [ ] **本轮最新代码真机仅完成基础回归**：延迟启动重试、失败分类、错误保留、零写退避在真实 AudioTrack/HAL 上的行为，以及 Media3 长时循环、AudioFocus 中断后的恢复、耳机拔出、长时锁屏/后台播放仍未在真机上复验。
- [ ] 未验证系统主动回收（非 `kill -9`）后服务重建的恢复行为，也未验证媒体键/通知卡片恢复播放的表现。
- [ ] 未验证来电打断等真实音频中断场景。
- [ ] 未验证不同音频设备（蓝牙、有线耳机、外放）及实际听感；延迟、长时间播放稳定性和声音听感未评估。
- [x] 2026-11-10 第二十轮（本地音频）：`testDebugUnitTest --rerun-tasks assembleDebug` 成功，**68 tests / 0 failures / 0 skipped**；API 35 x86_64 模拟器安装启动，系统选择器从 Downloads 选 40 s / 44.1 kHz / 单声道 PCM16 WAV，显示文件名与 `16-bit PCM · 44100 Hz · 1 ch`、播放中，MediaSession `PLAYING`，−6 dB 开关 `checked=true`，暂停/恢复通过，无 FATAL。模拟器无音频输出，未听感、未录波、未在 iQOO Z9T 真机。
- [x] 2026-11-10 第二十一轮（真机）：iQOO Z9T（V2352A / vivo / Android 16 API 36）安装 Debug APK 0.1.0
  （versionCode 1，SHA256 `3ABD75F1…F4C97D2`）成功，进程 pid 22780；用户用系统选择器播放主代理生成的
  40 s / 44.1 kHz / 单声道 PCM16 WAV，对声音、−6 dB、暂停恢复、爆音卡顿**合并答复“正常”**；
  锁屏约 15 s 持续，播放其他媒体后 DreamGuard 暂停，关闭本地音轨后 DSP 声景出声。
  日志无 `FATAL EXCEPTION` / `UnsatisfiedLinkError`（无 nativeloader 成功行，以实际出声为间接证据）；未录音频/波形。
- [ ] 本地音频的完整验收仍未满足：**真机播放与听感已由用户在 iQOO Z9T 上确认（第二十一轮，“正常”为合并答复，对应旧 Kotlin −6 dB 链路）**；第二十二/二十三轮已把 PCM16 接入 C++ 压缩器并修正 media3 契约，第二十三轮构建与 native/JVM 测试通过（80 JVM tests / 0 failures、PCM16 206/0、compressor 348/0）；**第二十四轮已在 API 35 x86_64 模拟器安装当前 Debug APK、选中 40 s/44.1 kHz/mono PCM16 WAV，UI 播放中 + “本地压缩就绪”、开关 `checked=true`，MediaSession `PLAYING`、进程存活、无 FATAL/UnsatisfiedLinkError**。但这只证明应用播放状态与处理器就绪，**不证明**逐块 `nativeProcess`、实际音频输出电平/波形、听感、延迟/underrun、泄漏或真机通过；其他格式/采样率、错误路径、通知栏暂停/恢复、进程重建恢复均未实测；用户明确延后真机与 30 分钟长稳。
- [ ] 第二十三轮新增/修正的 C++ `dg_pcm16_processor`（27 组）与 JVM `LocalAudioProcessorTest`（18 个）已随构建执行
  （206/0、80 JVM tests/0）；`NativePcmProcessorContractTest`（4 个）也通过，但它与 `FakeCompressor` **不等于实测 JNI**；
  `onFlush()` 已按 media3 1.5.1 修正并编译通过；arm64/armeabi 只编译未运行、ASan/LSan 未做。
  开关“本地压缩”与 makeup −6.02 dB 是设计替代，不是听感验证。
- [x] 2026-11-10 第二十四轮（模拟器状态）：代码提交 `4b7ace3` 已推送；主代理在 API 35 x86_64 模拟器安装并启动
  当前 Debug APK，系统选择器选中合成 40 s / 44.1 kHz / 单声道 PCM16 WAV；UI 播放中，格式提示“本地压缩就绪”，
  打开处理开关后 `checked=true`；MediaSession `PLAYING`、进程存活、`AndroidRuntime:E` 无 FATAL/UnsatisfiedLinkError。
  该证据只覆盖应用播放状态与 native `READY`，不覆盖实际音频输出、听感、延迟/泄漏或真机。
- [x] x86_64 模拟器：2026-11-10 在 AVD `ime35`（API 35）上完成安装、启动、播放冒烟，并运行 native 生命周期测试；真机以外的听感/长稳仍未评估。
- [ ] **泄漏检测未做**：仓库没有 ASan/LSan 作业，DG-010 的「无资源泄漏」未验证。
- [ ] **DG-012 的测试只在 x86_64 上运行过**：`dg_meter` 207/0、`dg_lufs` 238/0、扩展 C 头兼容 PASSED；
  Debug/Release 三 ABI 均能构建，但 arm64-v8a 与 armeabi-v7a 没有运行过测试；
  ffmpeg 9.0.2 `ebur128=metadata=1` 对照已完成：14 个可比样本 14 PASS / 0 FAIL、worst 0.010 LU，
  2 例静音/0.3 s 为 NO-DATA（不计入通过）；该 PASS 只在该参考约 0.01 LU 分辨率内成立，未跑 `--libebur128`、
  未做标准认证，LUFS 仍 `PROVISIONAL`；peak 也只是采样峰值，不是 true peak。
- [ ] 本轮 `assembleRelease` 构建已成功，但签名与混淆（当前 release 未开启 R8）仍未验证。
- [x] GitHub Actions 已触发并通过；后续代码变更仍需关注 Actions 结果。
- [ ] **DG-024 系统媒体音量控制仅完成部分验证**：第二十六轮主代理执行构建 **BUILD SUCCESSFUL**、JVM
  **92 tests / 0 failures / 0 skipped**，并在 API 35 x86_64 模拟器完成“未播放时 33%（5/15）→20%（3/15）”
  的读写，`dumpsys audio` 有 `setStreamVolume STREAM_MUSIC index 3 oldIndex 5 from com.dreamguard.app` 记录；
  但第三方播放器共存/不被打断、实际声音电平、外部音量变化同步（模拟器 adb 调节未生效）、边界/固定音量/
  单级输出 UI、蓝牙/固定输出/真机均未验证；验收要求 iQOO Z9T + 指定第三方播放器实测，按用户要求延后。
- [ ] 未做 instrumented / UI 自动化测试。

## 风险与限制

- **后台稳定性仍需真机验证**：播放由前台媒体服务管理，但不同设备的电池管理、来电和长时间运行仍需回归。
- **共享 AudioFocus**：Media3 管理两层声音的焦点；焦点中断时暂停声景，而不是只降低其中一层的音量。只有用户主动暂停才会结束服务，焦点丢失和耳机拔出保留会话。
- **异步失败的联调边界**：DSP 渲染失败到「暂停媒体播放器」的联动在 Android 组件里完成，JVM 测试覆盖失败分类、`PlaybackFailurePolicy` 决策、控制器的失败计数/错误保留与 6×250 ms 有界重试；暂停本身、媒体通知刷新、前台服务状态与 Android 13+ 通知展示仍需真机验证。临时输出错误只暂停并保留错误文案与层选择，不会静默关闭 DSP。
- **两层选择会持久化**：`dsp` / `ambient` 选择写入 `SharedPreferences`，服务被系统重建后不会出现“显示正在播放但无声”；音色与音量参数仍只在本次进程内有效。失败路径**不再**调用 `withoutDsp()` / `persistLayers()`；只有用户主动关闭层才会清除选择，失败与重试都不改写用户选择。
- **HAL 永久卡死的边界**：若 `AudioTrack.write/pause/stop` 永久阻塞，本实现不跨线程强制 `release()`/`destroy()`（避免 UAF），代价是该 `dreamguard-stop` 唤醒线程可能永久阻塞，且旧渲染线程未退出前新的 `start()` 会被拒绝（返回 `STOPPED` + `NONE`，服务层做 6×250 ms 有界重试后给出可见提示）。每个输出最多只有一个唤醒线程，不会无限创建线程；真正卡死的进程只能由系统回收。
- **取消/重试语义**：用户暂停（`playWhenReady=false`）会取消待执行的重试并清除延迟启动标记；重试本身只在 `layers.dsp && playWhenReady` 时进行，不会因通知或媒体会话状态变化而自动开始播放。
- **三 ABI 体积**：Debug APK 包含三套原生库，体积较大；发布时可用 ABI split 或 App Bundle。
- **JNI 名称耦合**：包名、类名、库名、CMake 目标任一处改动都可能导致运行时链接失败；`nativeXxx` 一旦改成 `internal`（或开启 R8 而未加 keep 规则），Kotlin 会给方法名追加模块后缀，`loadLibrary` 仍成功但首个原生调用抛 `UnsatisfiedLinkError`。`JniContractTest` 覆盖这种回退。
- **线程/生命周期边界**：`stop()` 最多等待渲染线程 `250 ms`，并且不在调用线程上执行 `AudioTrack` 的 pause/flush/stop（交给每输出至多一个 `dreamguard-stop` 线程唤醒阻塞写）；若超时，原生句柄与 `AudioTrack` 交给渲染线程在退出时释放（`release()` 同样延迟销毁），避免提前释放导致 UAF。渲染线程连续 20 次零帧写入（每次退避 5 ms）后判为 `TRANSIENT` 失败，不会无限忙等。设备上真实停止延迟仍待验证。
- **通知权限**：应用不声明 `POST_NOTIFICATIONS`。Android 13+ 的媒体会话通知豁免该运行时权限；普通通知权限不是本应用的必需项，前台服务使用 `FOREGROUND_SERVICE_MEDIA_PLAYBACK`。
- **DSP 未经听感校准**：频率、噪声比例、限幅参数是工程默认值，未做睡眠场景的声压/听感评估。
- **构建工具链固定**：AGP/Gradle/Kotlin/NDK/CMake 版本强绑定；升级需整体回归。

## License

MIT，见 [LICENSE](LICENSE)。
