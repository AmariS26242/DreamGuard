# DreamGuard

DreamGuard 是一个 Android-first 的睡眠声景应用骨架：Jetpack Compose 界面、Media3 环境音轨、以及一个用 **C++20 + NDK/CMake** 实现、通过 **JNI** 驱动的实时 DSP 声景引擎。所有声音都在本机生成或来自 APK 内置循环，应用不申请网络、麦克风或存储权限。

> 当前版本是 **工程骨架 / 可运行原型**，不是已经过真机长时间睡眠验证的产品。未验证项见[验证状态](#验证状态)。

## 功能

- Compose + Material 3 深色睡眠界面，单屏操作。
- 播放/暂停由原生 DSP 生成的声景：双正弦低频 drone、粉红噪声、低通音色控制、软限幅、2 秒淡入。
- 可调音量、音色亮度（低通截止频率）、噪声音量、低频频率。
- Media3 ExoPlayer 播放 APK 内置的可循环环境音轨，可与原生声景叠加；环境音关闭时该轨道静音，以维持统一媒体会话。
- 播放由 MediaSessionService 管理，提供媒体通知和后台播放；点击通知/锁屏卡片返回应用。音频焦点中断及耳机拔出时暂停声景，但保留会话，中断结束后可继续。
- DSP 渲染线程异步失败时：若只有 DSP 层在播，暂停媒体会话以免留下无声的前台服务；若环境音同时在播，保留环境音并在界面显示 DSP 错误。失败按 `TRANSIENT` / `PERMANENT` 分类；**临时输出错误不会清除已保存的 DSP/环境音选择**，只有用户主动关闭层或成功重启才更新状态。
- 开始被上一段渲染线程的关闭延迟时，服务做最多 6 次 × 250 ms 的有界重试，界面显示“正在等待上一段音频结束…”；重试耗尽后暂停并给出“上一段音频仍在关闭，请稍后重试”的可见提示，用户再点播放即显式重试，**不会自动播放**。
- 媒体播放器内部错误与用户暂停分开处理：错误暂停不会触发 `stopSelf()`，错误文案保留在界面上，直到成功重试或用户主动关闭。
- 纯 Kotlin 的 `PlaybackController` 状态机与失败/恢复策略不依赖 Android，可直接在 JVM 上测试。
- 58 个 JVM 单元测试覆盖参数清洗、状态迁移、错误路径、监听通知、渲染线程停止/销毁时序、失败分类、有界重试、零写退避、两层选择状态与 JNI 命名契约。
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
│       │   │   ├── CMakeLists.txt          # 目标/库名 dreamguard_dsp
│       │   │   ├── dsp_core.h
│       │   │   ├── dsp_core.cpp            # C++20 DSP
│       │   │   └── jni_bridge.cpp          # JNI 符号
│       │   ├── java/com/dreamguard/app/
│       │   │   ├── MainActivity.kt
│       │   │   ├── audio/
│       │   │   │   ├── AudioEngine.kt
│       │   │   │   ├── AudioPlatform.kt      # AudioTrack / JNI 适配层
│       │   │   │   ├── AudioSettings.kt
│       │   │   │   ├── NativeAudioEngine.kt
│       │   │   │   ├── PlaybackController.kt
│       │   │   │   ├── PlaybackFailurePolicy.kt
│       │   │   │   ├── PlaybackLayers.kt
│       │   │   │   ├── PlaybackRecoveryPolicy.kt
│       │   │   │   └── PlaybackService.kt
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
│           ├── NativeAudioEngineTest.kt
│           ├── PlaybackControllerTest.kt
│           ├── PlaybackFailurePolicyTest.kt
│           ├── PlaybackLayersTest.kt
│           └── PlaybackRecoveryPolicyTest.kt
├── gradle/libs.versions.toml
├── gradle/wrapper/
├── build.gradle.kts
├── settings.gradle.kts
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
       │         └─ JNI → libdreamguard_dsp.so → SoundscapeRenderer
  └─ ExoPlayer（内置 dream_loop.wav；环境音关闭时静音）

界面绑定服务；服务负责释放播放器、媒体会话和原生引擎。
```

### DSP 设计

- 双正弦 drone：基础频率 `f` 与 `1.5f`，默认 110 Hz。
- 粉红噪声：Paul Kellet 算法 + xorshift32，确定性种子，便于复现。
- 音色：RBJ biquad 低通，默认 1.2 kHz，范围 80 Hz–12 kHz。
- 呼吸感：0.05 Hz LFO，20 秒一个周期。
- 淡入：2 秒线性淡入，避免起播爆音。
- 软限幅：`tanh(1.1x) / tanh(1.1)`，保证输出绝对值小于 1。
- 实时安全：`render()` 内无分配；参数用 `std::atomic` 从 UI 线程传入，滤波器系数只在音频线程块边界重算。

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

### 本次实际运行

- 环境：Windows、JDK 17.0.21（Microsoft build）、Gradle 8.11.1、Android SDK 35 / Build-Tools 35.0.0、NDK 27.0.12077973、CMake 3.22.1。
- **2026-11-10 主代理在本机执行**（非用户执行）：`.\gradlew.bat testDebugUnitTest assembleDebug assembleRelease`（单条命令，未指定 `--no-daemon`）；**58 项测试通过、0 跳过**，Debug 与 Release 两种构建均成功。上述结果对应当前最新代码。
- 更早记录：2026-11-10 同样由主代理在本机执行 `.\gradlew.bat testDebugUnitTest assembleDebug assembleRelease`，当时通过的是本轮改动**之前**的 **37 个用例**；再往前 `main` @ `4954671` 上 `clean testDebugUnitTest` 通过（10 个用例）；`assembleDebug` 通过并生成三 ABI APK；GitHub Actions [42613031269](https://github.com/AmariS26242/DreamGuard/actions/runs/42613031269) 对应旧代码通过。
- JVM 单元测试在本地 JVM 执行，不涉及真机或模拟器。2026-11-09 在 V2352A（Android 16）上完成基础安装、原生播放和听感检查。

> 构建与单测已覆盖最新代码；真机仅完成下述基础回归，其余见[明确未验证](#明确未验证)。

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
- [ ] 未验证 x86_64 模拟器运行。
- [ ] 本轮 `assembleRelease` 构建已成功，但签名与混淆（当前 release 未开启 R8）仍未验证。
- [x] GitHub Actions 已触发并通过；后续代码变更仍需关注 Actions 结果。
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
