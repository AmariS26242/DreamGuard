plugins {
    base
}

// The native DSP core is a plain CMake project. Gradle drives it so the repo has
// one repeatable entry point for the native lifecycle tests, next to
// `:app:testDebugUnitTest` for the Kotlin side:
//
//   ./gradlew :dsp-core:dspCoreTest
//
// CMake and CTest are expected on PATH (the Android SDK ships both). Override
// the binaries when they are not: -PdspCmake=C:/path/to/cmake.exe
// -PdspCtest=C:/path/to/ctest.exe
//
// `:app` does not depend on this project. The Android build compiles the same
// sources through app/src/main/cpp/CMakeLists.txt (add_subdirectory), which
// keeps a single implementation and avoids packaging the core twice.

val dspCoreDir = layout.projectDirectory.asFile
val nativeBuildDir = layout.buildDirectory.dir("native").get().asFile
val cmakeExecutable = providers.gradleProperty("dspCmake").getOrElse("cmake")
val ctestExecutable = providers.gradleProperty("dspCtest").getOrElse("ctest")

val dspCoreConfigure = tasks.register<Exec>("dspCoreConfigure") {
    group = "verification"
    description = "Configures the host build of the portable DSP core with its tests."
    workingDir = dspCoreDir
    outputs.dir(nativeBuildDir)
    commandLine(
        cmakeExecutable,
        "-S", dspCoreDir.absolutePath,
        "-B", nativeBuildDir.absolutePath,
        "-DCMAKE_BUILD_TYPE=Debug",
        "-DDREAMGUARD_DSP_BUILD_TESTS=ON",
    )
}

val dspCoreBuild = tasks.register<Exec>("dspCoreBuild") {
    group = "verification"
    description = "Compiles the portable DSP core and its lifecycle tests for the host."
    dependsOn(dspCoreConfigure)
    workingDir = dspCoreDir
    commandLine(
        cmakeExecutable,
        "--build", nativeBuildDir.absolutePath,
        "--config", "Debug",
    )
}

tasks.register<Exec>("dspCoreTest") {
    group = "verification"
    description = "Runs the portable DSP core lifecycle tests through CTest."
    dependsOn(dspCoreBuild)
    workingDir = dspCoreDir
    commandLine(
        ctestExecutable,
        "--test-dir", nativeBuildDir.absolutePath,
        "--output-on-failure",
        "-C", "Debug",
    )
}
