# DreamGuard

DreamGuard 是一个 Android-first 的睡眠声景应用骨架：Jetpack Compose 界面、Media3 环境音轨、以及一个用 **C++20 + NDK/CMake** 实现、通过 **JNI** 驱动的实时 DSP 声景引擎。所有声音都在本机生成或来自 APK 内置循环，应用不申请网络、麦克风或存储权限。

> 当前版本是 **工程骨架 / 可运行原型**，不是已经过真机长时间睡眠验证的产品。未验证项见[验证状态](#验证状态)。

## 功能

- Compose + Material 3 深色睡眠界面，单屏操作。
- 播放/暂停由原生 DSP 生成的声景：双正弦低频 drone、粉红噪声、低通音色控制、软限幅、2 秒淡入。
- 可调音量、音色亮度（低通截止频率）、噪声音量、低频频率。
- Media3 ExoPlayer 播放 APK 内置的可循环环境音轨，可与原生声景叠加。
- 纯 Kotlin 的 `PlaybackController` 状态机，不依赖 Android，可直接在 JVM 上测试。
- 7 个 JVM 单元测试覆盖参数清洗、状态迁移、错误路径与监听通知。
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
│       │   │   │   ├── AudioSettings.kt
│       │   │   │   ├── NativeAudioEngine.kt
│       │   │   │   ├── Media3AmbientPlayer.kt
│       │   │   │   └── PlaybackController.kt
│       │   │   └── ui/
│       │   │       ├── DreamGuardScreen.kt
│       │   │       └── theme/Theme.kt
│       │   └── res/
│       │       ├── raw/dream_loop.wav      # 3 秒可循环合成音轨
│       │       └── values/{strings,themes}.xml
│       └── test/java/com/dreamguard/app/audio/
│           ├── FakeAudioEngine.kt
│           ├── AudioSettingsTest.kt
│           └── PlaybackControllerTest.kt
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
```

Windows PowerShell：

```powershell
.\gradlew.bat testDebugUnitTest
.\gradlew.bat assembleDebug
```

> **路径含单引号时的已知环境问题**：如果 Windows 工作区路径包含 `'`（例如用户名 `A'maris`），Gradle 把超长测试 classpath 写入 Java `@argfile` 时，JDK 会把单引号当作引号解析，导致测试 worker 启动即报 `ClassNotFoundException: ...GradleWorkerMain`。可将工作区映射到无引号的短路径后构建（例如 `subst X: <workspace>`，再从 `X:\DreamGuard` 执行），这不是项目代码缺陷。

产物：

- 单元测试报告：`app/build/reports/tests/testDebugUnitTest/index.html`
- Debug APK：`app/build/outputs/apk/debug/app-debug.apk`

APK 默认打包 `arm64-v8a`、`armeabi-v7a`、`x86_64` 三个 ABI，因此模拟器也能加载原生库。

## 架构

```text
Compose UI
  └─ PlaybackController        (纯 Kotlin 状态机 + 参数清洗)
       ├─ AudioEngine         (接口，JVM 可替换为 FakeAudioEngine)
       │    └─ NativeAudioEngine
       │         ├─ AudioTrack（48 kHz / 单声道 / float）
       │         └─ JNI → libdreamguard_dsp.so → SoundscapeRenderer
       └─ Media3AmbientPlayer (ExoPlayer，内置 dream_loop.wav)

Lifecycle / 资源释放由 Compose DisposableEffect 负责。
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

| Kotlin（`NativeAudioEngine` 私有 external） | C++ JNI 符号 | 位置 |
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

## 测试

`app/src/test` 中的测试不加载 NDK，也不需要模拟器：

- `AudioSettingsTest`：默认值、越界钳制、NaN/±Infinity 回退。
- `PlaybackControllerTest`：启动参数下发、不可用引擎错误、参数钳制、toggle 状态、release 终止性、监听通知、启动失败错误。

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
- [ ] 按 Home 键 / 锁屏后观察是否继续播放（当前**没有**前台服务，见风险）。
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
- `.\gradlew.bat --no-daemon clean testDebugUnitTest`：`BUILD SUCCESSFUL`，JUnit 共 **10 个用例全部通过**（`AudioSettingsTest` 3/3、`PlaybackControllerTest` 7/7）。
- `.\gradlew.bat --no-daemon assembleDebug`：`BUILD SUCCESSFUL`，生成 `app/build/outputs/apk/debug/app-debug.apk`；C++20/CMake 为 arm64-v8a、armeabi-v7a、x86_64 三套 ABI 编译并打包。
- JVM 单元测试在本地 JVM 执行，不涉及真机或模拟器；原生库只验证了编译与打包，未在设备上实际加载。

### 明确未验证

- [ ] **未在 iQOO Z9T 真机上安装和运行**；上面的清单是操作步骤，不是已验证记录。
- [ ] 未验证实际音频输出、延迟、爆音、长时间播放稳定性。
- [ ] 未验证 Media3 循环音轨在真机上的播放与 AudioFocus 行为。
- [ ] 未验证锁屏/后台播放行为。
- [ ] 未验证 x86_64 模拟器运行。
- [ ] 未验证 release 构建、混淆、签名。
- [ ] 未在 GitHub 上实际触发 CI（工作流文件已写好，但本次没有 push，也没有创建仓库）。
- [ ] 未做 instrumented / UI 自动化测试。

## 风险与限制

- **没有前台服务**：播放依赖 Activity 进程；切后台或锁屏后系统可能限制或杀死进程。睡眠场景若需整夜播放，应增加 `MediaSessionService` + 前台服务与通知权限。
- **原生播放未接入 AudioFocus**：`NativeAudioEngine` 直接写 `AudioTrack`，不会自动避让来电/其他媒体；Media3 路径已启用 AudioFocus。二者同时播放时由用户手动控制。
- **参数不持久化**：音量/音色等仅在本次进程内有效。
- **三 ABI 体积**：Debug APK 包含三套原生库，体积较大；发布时可用 ABI split 或 App Bundle。
- **JNI 名称耦合**：包名、类名、库名、CMake 目标任一处改动都可能导致运行时链接失败。
- **线程/生命周期边界**：`stop()` 会等待渲染线程最多 500 ms；极端设备上仍可能出现短暂阻塞。
- **DSP 未经听感校准**：频率、噪声比例、限幅参数是工程默认值，未做睡眠场景的声压/听感评估。
- **构建工具链固定**：AGP/Gradle/Kotlin/NDK/CMake 版本强绑定；升级需整体回归。

## License

MIT，见 [LICENSE](LICENSE)。
