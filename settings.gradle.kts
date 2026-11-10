pluginManagement {
    repositories {
        google {
            content {
                includeGroupByRegex("com\\.android.*")
                includeGroupByRegex("com\\.google.*")
                includeGroupByRegex("androidx.*")
            }
        }
        mavenCentral()
        gradlePluginPortal()
    }
}

dependencyResolutionManagement {
    repositoriesMode.set(RepositoriesMode.FAIL_ON_PROJECT_REPOS)
    repositories {
        google()
        mavenCentral()
    }
}

rootProject.name = "DreamGuard"

// :app is the Android application (Compose UI, Media3 playback, JNI bridge).
include(":app")

// :dsp-core is the portable C++20 DSP core: the soundscape generator plus the
// DG-010 C lifecycle API and DG-011 PCM helpers. It has no Android dependency,
// so the same sources build on the host (`./gradlew :dsp-core:dspCoreTest`) and
// for Android through app/src/main/cpp/CMakeLists.txt.
include(":dsp-core")
