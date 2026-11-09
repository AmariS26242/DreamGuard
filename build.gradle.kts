// Top-level build file. Plugin versions are declared in gradle/libs.versions.toml
// and applied per-module so every module resolves an identical, pinned toolchain.
plugins {
    alias(libs.plugins.android.application) apply false
    alias(libs.plugins.kotlin.android) apply false
    alias(libs.plugins.kotlin.compose) apply false
}
