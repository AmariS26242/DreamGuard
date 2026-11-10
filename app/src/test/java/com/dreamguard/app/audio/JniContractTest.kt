package com.dreamguard.app.audio

import java.io.File
import java.lang.reflect.Modifier
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Assume.assumeTrue
import org.junit.Test

/**
 * Guards the JNI naming contract between [NativeAudioEngine] and `jni_bridge.cpp`.
 *
 * The regression this exists for: declaring the `nativeXxx` functions `internal` makes
 * Kotlin append the module name to the JVM name — `nativeCreate$dreamguard_debug` in
 * debug and `nativeCreate$dreamguard_release` in release. `System.loadLibrary` still
 * succeeds, so nothing fails until the first native call throws `UnsatisfiedLinkError`
 * on a device, and the symbol name differs per build type.
 *
 * These tests run on the JVM: they inspect the compiled method names and the C++ file,
 * so they catch a visibility (or a rename) change on either side without a device.
 */
class JniContractTest {

    @Test
    fun `native methods are declared with the names the bridge exports`() {
        assertEquals(EXPECTED_NATIVE_METHODS, declaredNativeMethods())
    }

    @Test
    fun `native method names carry no Kotlin module suffix in any build type`() {
        val declared = declaredNativeMethods()
        assertTrue("no native method is declared on NativeAudioEngine", declared.isNotEmpty())
        for (name in declared) {
            assertFalse(
                "JNI name mangled by Kotlin visibility; debug and release would differ: $name",
                name.contains('$'),
            )
        }
    }

    @Test
    fun `jni bridge exports a symbol for every declared native method`() {
        val bridge = jniBridgeFile()
        assumeTrue(
            "jni_bridge.cpp not found from ${File("").absoluteFile}",
            bridge != null,
        )
        val exported = SYMBOL.findAll(bridge!!.readText())
            .map { it.groupValues[1] }
            .toSortedSet()
            .toList()
        assertEquals(EXPECTED_NATIVE_METHODS, exported)
    }

    private fun declaredNativeMethods(): List<String> =
        NativeAudioEngine::class.java.declaredMethods
            .filter { Modifier.isNative(it.modifiers) }
            .map { it.name }
            .sorted()

    private fun jniBridgeFile(): File? =
        listOf("src/main/cpp/jni_bridge.cpp", "app/src/main/cpp/jni_bridge.cpp")
            .map(::File)
            .firstOrNull { it.isFile }

    private companion object {
        private const val SYMBOL_PREFIX = "Java_com_dreamguard_app_audio_NativeAudioEngine_"
        private val SYMBOL = Regex(SYMBOL_PREFIX + "(\\w+)")

        private val EXPECTED_NATIVE_METHODS = listOf(
            "nativeCreate",
            "nativeDestroy",
            "nativeRender",
            "nativeReset",
            "nativeSetDroneHz",
            "nativeSetGain",
            "nativeSetNoiseMix",
            "nativeSetToneCutoff",
            "nativeVersion",
        ).sorted()
    }
}
