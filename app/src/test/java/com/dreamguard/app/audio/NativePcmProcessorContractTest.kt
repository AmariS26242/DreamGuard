package com.dreamguard.app.audio

import java.io.File
import java.lang.reflect.Modifier
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertTrue
import org.junit.Assume.assumeTrue
import org.junit.Test

/**
 * Guards the JNI naming contract for the DG-022 PCM bridge between
 * [NativePcmProcessor] and `jni_bridge.cpp`.
 *
 * Same failure mode as [JniContractTest]: an `internal` native method gets the
 * Kotlin module name appended to its JVM name (`nativeCreate$dreamguard_debug`),
 * which compiles and loads but cannot be resolved by the exported C symbol. The
 * PCM surface is a separate class with its own symbol prefix, so it needs its
 * own guard; a wrong name here would silently downgrade every local file to the
 * byte-exact fallback on a device.
 *
 * These tests run on the JVM and inspect the compiled method names plus the C++
 * file, so they catch a rename or visibility change on either side without a
 * device.
 */
class NativePcmProcessorContractTest {

    @Test
    fun `native methods are declared with the names the bridge exports`() {
        assertEquals(EXPECTED_NATIVE_METHODS, declaredNativeMethods())
    }

    @Test
    fun `native method names carry no Kotlin module suffix in any build type`() {
        val declared = declaredNativeMethods()
        assertTrue("no native method is declared on NativePcmProcessor", declared.isNotEmpty())
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

    @Test
    fun `pcm symbols use their own class prefix`() {
        val bridge = jniBridgeFile()
        assumeTrue(
            "jni_bridge.cpp not found from ${File("").absoluteFile}",
            bridge != null,
        )
        val text = bridge!!.readText()
        // The two handle types are not interchangeable, so the PCM surface must
        // never be exported under the soundscape engine's class name.
        assertTrue(text.contains("Java_com_dreamguard_app_audio_NativePcmProcessor_nativeCreate"))
        assertFalse(
            text.contains("Java_com_dreamguard_app_audio_NativeAudioEngine_nativeProcess"),
        )
    }

    @Test
    fun `proguard rules keep NativePcmProcessor native methods unminified`() {
        val rules = proguardRulesFile()
        assertNotNull(
            "proguard-rules.pro not found from ${File("").absoluteFile}",
            rules,
        )
        val normalized = rules!!.readText().replace(Regex("\\s+"), " ")
        assertTrue(
            "proguard-rules.pro must preserve NativePcmProcessor JNI entry points",
            normalized.contains(
                "-keepclasseswithmembernames class com.dreamguard.app.audio.NativePcmProcessor { native <methods>; }",
            ),
        )
    }

    private fun declaredNativeMethods(): List<String> =
        NativePcmProcessor::class.java.declaredMethods
            .filter { Modifier.isNative(it.modifiers) }
            .map { it.name }
            .sorted()

    private fun jniBridgeFile(): File? =
        listOf("src/main/cpp/jni_bridge.cpp", "app/src/main/cpp/jni_bridge.cpp")
            .map(::File)
            .firstOrNull { it.isFile }

    private fun proguardRulesFile(): File? =
        listOf("proguard-rules.pro", "app/proguard-rules.pro")
            .map(::File)
            .firstOrNull { it.isFile }

    private companion object {
        private const val SYMBOL_PREFIX = "Java_com_dreamguard_app_audio_NativePcmProcessor_"
        private val SYMBOL = Regex(SYMBOL_PREFIX + "(\\w+)")

        private val EXPECTED_NATIVE_METHODS = listOf(
            "nativeCreate",
            "nativeDestroy",
            "nativeDrain",
            "nativeProcess",
            "nativeReset",
            "nativeVersion",
        ).sorted()
    }
}
