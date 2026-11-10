package com.dreamguard.app.audio

/**
 * Production [AudioEngine]: renders float PCM on a dedicated thread through
 * [android.media.AudioTrack] and pulls samples from the C++20 DSP core via JNI.
 *
 * Threading contract:
 *  - exactly one render thread at a time, and it owns the [RenderOutput] lifetime;
 *  - [stop] never blocks on the render thread longer than [STOP_JOIN_TIMEOUT_MS], and
 *    never performs AudioTrack control calls on the caller's thread;
 *  - the native handle is destroyed only after the render thread has returned, so
 *    a wedged render thread can never observe freed native memory;
 *  - at most one wake-up thread is in flight per output, so a permanently wedged HAL
 *    cannot make successive [stop] calls create an unbounded number of threads.
 *
 * JNI contract: the `nativeXxx` declarations are `private` on purpose. Kotlin mangles
 * `internal` function names with the module name, which breaks the symbols exported by
 * `jni_bridge.cpp`. `JniContractTest` fails the build if that regresses.
 *
 * Library name, JNI symbols, and the CMake target all use `dreamguard_dsp`.
 */
class NativeAudioEngine internal constructor(
    dspOverride: DspCommands?,
    private val outputs: RenderOutputFactory,
) : AudioEngine {

    constructor() : this(null, AndroidRenderOutputFactory())

    private val lock = Any()

    /**
     * Serializes control calls on the [RenderOutput] (pause/flush/stop) made by the
     * wake-up thread against `release()` made by the render thread. Never held while
     * taking [lock]: every acquisition is outermost.
     */
    private val outputLock = Any()

    private val dsp: DspCommands = dspOverride ?: JniDspCommands()

    @Volatile
    private var currentState: EngineState = EngineState.IDLE

    @Volatile
    private var running = false

    @Volatile
    private var released = false

    @Volatile
    private var failure: String? = null

    @Volatile
    private var failureKindValue: EngineFailureKind = EngineFailureKind.NONE

    /** Set when [release] found a render thread that had not returned yet. */
    private var destroyOnThreadExit = false

    /**
     * Guards [wakingOutput]. Kept separate from [outputLock] on purpose: the wake-up
     * thread holds [outputLock] for the whole (possibly wedged) HAL call, so a
     * repeated [stop] must be able to detect it without waiting for that call.
     */
    private val wakeLock = Any()

    /**
     * Output that a wake-up thread is already servicing. A HAL that never returns
     * from `pause()`/`stop()` leaves that thread blocked forever, so the flag is
     * cleared by the thread's `finally` block, not by a timeout. Holding the identity
     * of the *current* output (not a boolean) lets a genuinely new session be woken
     * even while the old wake-up thread is wedged.
     */
    private var wakingOutput: RenderOutput? = null

    override var onStateChanged: (() -> Unit)? = null

    override val lastFailure: String?
        get() = failure

    override val failureKind: EngineFailureKind
        get() = failureKindValue

    private var handle: Long = 0L
    @Volatile
    private var output: RenderOutput? = null
    private var renderThread: Thread? = null

    private var desiredGain = AudioSettings.DEFAULT_GAIN
    private var desiredCutoffHz = AudioSettings.DEFAULT_TONE_CUTOFF_HZ
    private var desiredNoiseMix = AudioSettings.DEFAULT_NOISE_MIX
    private var desiredDroneHz = AudioSettings.DEFAULT_DRONE_HZ

    override val isAvailable: Boolean
        get() = dsp.isLoaded && !released

    override val state: EngineState
        get() = currentState

    override fun start(): Boolean {
        val started = synchronized(lock) { startLocked() }
        notifyStateChanged()
        return started
    }

    private fun startLocked(): Boolean {
        if (released) return false
        if (!dsp.isLoaded) return failLocked(MSG_NO_LIBRARY, EngineFailureKind.PERMANENT)
        if (running && currentState == EngineState.RUNNING) return true

        val previous = renderThread
        if (previous != null && previous.isAlive) {
            // Never share one native handle between two render threads. The caller can
            // retry once the previous stop has actually finished. This is a transient
            // condition, not a failure, so the state is not moved to ERROR and no
            // failure kind is reported: the controller marks the start retryable instead.
            failure = MSG_SHUTTING_DOWN
            failureKindValue = EngineFailureKind.NONE
            running = false
            if (currentState == EngineState.RUNNING) currentState = EngineState.STOPPED
            return false
        }
        renderThread = null

        if (handle == 0L) {
            handle = runCatching { dsp.create(SAMPLE_RATE, CHANNELS) }.getOrDefault(0L)
            if (handle == 0L) return failLocked(MSG_CREATE, EngineFailureKind.PERMANENT)
        }

        val newOutput = runCatching { outputs.open(SAMPLE_RATE, CHANNELS) }.getOrNull()
            ?: return failLocked(MSG_OUTPUT, EngineFailureKind.TRANSIENT)

        try {
            applyNativeParametersLocked()
            newOutput.play()
        } catch (t: Throwable) {
            runCatching { newOutput.release() }
            return failLocked(
                "Audio output failed to start: ${t.describe()}",
                EngineFailureKind.TRANSIENT,
            )
        }

        output = newOutput
        failure = null
        failureKindValue = EngineFailureKind.NONE
        running = true
        currentState = EngineState.RUNNING

        val thread = Thread({ renderLoop(handle, newOutput) }, RENDER_THREAD_NAME).apply {
            priority = Thread.NORM_PRIORITY + 1
        }
        renderThread = thread
        thread.start()
        return true
    }

    override fun stop() {
        val snapshot = synchronized(lock) {
            if (released) return
            if (!running && renderThread == null) {
                if (currentState == EngineState.RUNNING) currentState = EngineState.STOPPED
                return
            }
            running = false
            renderThread to output
        }
        val thread = snapshot.first
        val currentOutput = snapshot.second
        val self = thread != null && thread === Thread.currentThread()

        if (thread != null && !self && thread.isAlive) {
            // AudioTrack.pause()/flush()/stop() are HAL round-trips: on a wedged or
            // Bluetooth output they block exactly like the write() we are trying to
            // interrupt. They must therefore never run on the caller's thread.
            wakeRenderThread(currentOutput)
            runCatching { thread.join(STOP_JOIN_TIMEOUT_MS) }
        }

        val stillExiting = thread != null && !self && thread.isAlive
        if (stillExiting) {
            // The render thread is wedged inside the output or a JNI call. It finishes
            // the teardown itself; touching the handle here would be a use-after-free.
            // Playback is already stopped, so report that immediately.
            synchronized(lock) {
                if (currentState == EngineState.RUNNING) currentState = EngineState.STOPPED
            }
            notifyStateChanged()
            return
        }

        synchronized(lock) {
            if (released) return
            if (self) return
            finishTeardownLocked(resetHandle = true)
        }
        notifyStateChanged()
    }

    /**
     * Makes a render thread parked in `write(WRITE_BLOCKING)` return, without blocking
     * the caller on the audio HAL. The render thread keeps ownership of the output and
     * is the only one that releases it; [outputLock] only guarantees that these three
     * calls never overlap with that release.
     */
    private fun wakeRenderThread(out: RenderOutput?) {
        if (out == null) return
        synchronized(wakeLock) {
            // A wake-up thread is already stuck on this output. Spawning another one
            // would only add a second thread that blocks in the same HAL call; the
            // existing thread will wake the render loop when the HAL returns.
            if (wakingOutput === out) return
            wakingOutput = out
        }
        val waker = Thread({
            try {
                synchronized(outputLock) {
                    // A newer session may already own the output, or it may be released.
                    if (output !== out) return@synchronized
                    runCatching {
                        out.pause()
                        out.flush()
                        out.stop()
                    }
                }
            } finally {
                synchronized(wakeLock) {
                    if (wakingOutput === out) wakingOutput = null
                }
            }
        }, STOP_THREAD_NAME)
        waker.isDaemon = true
        waker.priority = Thread.NORM_PRIORITY + 1
        waker.start()
    }

    override fun release() {
        if (released) return
        stop()
        var postponed = false
        synchronized(lock) {
            if (released) return
            released = true
            running = false
            val thread = renderThread
            if (thread != null && thread !== Thread.currentThread() && thread.isAlive) {
                destroyOnThreadExit = true
                postponed = true
            } else {
                destroyHandleLocked()
                output = null
                renderThread = null
                currentState = EngineState.RELEASED
            }
        }
        if (!postponed) notifyStateChanged()
    }

    override fun setGain(gain: Float) {
        if (!gain.isFinite()) return
        val safe = gain.coerceIn(AudioSettings.MIN_GAIN, AudioSettings.MAX_GAIN)
        desiredGain = safe
        withHandle { dsp.setGain(it, safe) }
    }

    override fun setToneCutoff(cutoffHz: Float) {
        if (!cutoffHz.isFinite()) return
        val safe = cutoffHz.coerceIn(
            AudioSettings.MIN_TONE_CUTOFF_HZ,
            AudioSettings.MAX_TONE_CUTOFF_HZ,
        )
        desiredCutoffHz = safe
        withHandle { dsp.setToneCutoff(it, safe) }
    }

    override fun setNoiseMix(mix: Float) {
        if (!mix.isFinite()) return
        val safe = mix.coerceIn(AudioSettings.MIN_NOISE_MIX, AudioSettings.MAX_NOISE_MIX)
        desiredNoiseMix = safe
        withHandle { dsp.setNoiseMix(it, safe) }
    }

    override fun setDroneHz(hz: Float) {
        if (!hz.isFinite()) return
        val safe = hz.coerceIn(AudioSettings.MIN_DRONE_HZ, AudioSettings.MAX_DRONE_HZ)
        desiredDroneHz = safe
        withHandle { dsp.setDroneHz(it, safe) }
    }

    /** DSP core version reported by the native library, for the UI footer. */
    fun versionLabel(): String = if (dsp.isLoaded) {
        runCatching { dsp.version() }.getOrDefault("unknown")
    } else {
        "unavailable"
    }

    private fun renderLoop(handle: Long, out: RenderOutput) {
        val buffer = FloatArray(FRAMES_PER_BLOCK)
        var renderFailure: String? = null
        var renderFailureKind = EngineFailureKind.TRANSIENT
        try {
            outputs.prepareRenderThread()
            while (running) {
                val frames = dsp.render(handle, buffer, FRAMES_PER_BLOCK)
                if (frames <= 0) {
                    if (running) {
                        renderFailure = "Native renderer returned $frames frames."
                        // The native renderer has no valid state left to retry from.
                        renderFailureKind = EngineFailureKind.PERMANENT
                    }
                    break
                }
                var offset = 0
                var zeroWrites = 0
                while (offset < frames && running) {
                    val written = out.write(buffer, offset, frames - offset)
                    if (written < 0) {
                        if (running) {
                            renderFailure = "Audio output rejected the block ($written)."
                        }
                        running = false
                        break
                    }
                    if (written == 0) {
                        // A blocking write that makes no progress must not spin this
                        // thread at 100% CPU forever. Back off a little and fail after a
                        // bounded number of consecutive stalls; a later user-triggered
                        // start re-opens the output.
                        zeroWrites++
                        if (!running) break
                        if (zeroWrites >= MAX_CONSECUTIVE_ZERO_WRITES) {
                            renderFailure = "Audio output made no progress after " +
                                "$zeroWrites zero-frame writes."
                            running = false
                            break
                        }
                        Thread.sleep(ZERO_WRITE_BACKOFF_MS)
                        continue
                    }
                    zeroWrites = 0
                    offset += written
                }
            }
        } catch (t: Throwable) {
            if (running) renderFailure = "Audio render thread failed: ${t.describe()}"
            running = false
        } finally {
            finishRenderThread(out, renderFailure, renderFailureKind)
        }
    }

    private fun finishRenderThread(
        out: RenderOutput,
        renderFailure: String?,
        renderFailureKind: EngineFailureKind,
    ) {
        // outputLock, then lock: never the other way round. This keeps release() from
        // overlapping the wake-up thread's pause()/flush()/stop() on the same track.
        synchronized(outputLock) {
            runCatching {
                out.pause()
                out.stop()
                out.release()
            }
            synchronized(lock) {
                if (output === out) output = null
                if (renderThread === Thread.currentThread()) renderThread = null

                if (destroyOnThreadExit) {
                    destroyOnThreadExit = false
                    destroyHandleLocked()
                    running = false
                    currentState = EngineState.RELEASED
                } else if (renderFailure != null) {
                    failure = renderFailure
                    failureKindValue = renderFailureKind
                    running = false
                    // The handle survives an error so the next start() can reuse it, but
                    // its filter state must not leak into the next session.
                    if (handle != 0L) runCatching { dsp.reset(handle) }
                    currentState = EngineState.ERROR
                } else {
                    if (handle != 0L) runCatching { dsp.reset(handle) }
                    if (currentState == EngineState.RUNNING) currentState = EngineState.STOPPED
                }
            }
        }
        notifyStateChanged()
    }

    /**
     * Releases everything that the render thread no longer uses. Callers must have
     * established that the render thread has returned (or never started).
     */
    private fun finishTeardownLocked(resetHandle: Boolean) {
        renderThread = null
        val out = output
        output = null
        out?.let { runCatching { it.release() } }
        if (resetHandle && handle != 0L) runCatching { dsp.reset(handle) }
        if (currentState == EngineState.RUNNING) currentState = EngineState.STOPPED
    }

    private fun destroyHandleLocked() {
        if (handle != 0L) {
            val doomed = handle
            handle = 0L
            runCatching { dsp.destroy(doomed) }
        }
    }

    private fun applyNativeParametersLocked() {
        if (handle == 0L) return
        runCatching {
            dsp.setGain(handle, desiredGain)
            dsp.setToneCutoff(handle, desiredCutoffHz)
            dsp.setNoiseMix(handle, desiredNoiseMix)
            dsp.setDroneHz(handle, desiredDroneHz)
        }
    }

    /**
     * Runs [block] with the current handle while holding [lock], so it can never
     * overlap with [destroyHandleLocked].
     */
    private inline fun withHandle(block: (Long) -> Unit) {
        if (!dsp.isLoaded) return
        synchronized(lock) {
            val current = handle
            if (current == 0L || released) return
            runCatching { block(current) }
        }
    }

    private fun failLocked(message: String, kind: EngineFailureKind): Boolean {
        failure = message
        failureKindValue = kind
        running = false
        currentState = EngineState.ERROR
        return false
    }

    private fun notifyStateChanged() {
        runCatching { onStateChanged?.invoke() }
    }

    private fun Throwable.describe(): String = message ?: this::class.java.simpleName

    /**
     * Adapter from [DspCommands] to the JNI symbols declared on this class.
     *
     * The native declarations below must stay `private`: Kotlin appends `$moduleName`
     * to `internal` function names on the JVM (`nativeCreate$dreamguard_debug`), which
     * silently breaks the symbol lookup in `jni_bridge.cpp`. `JniContractTest` guards
     * both sides of that contract.
     */
    private inner class JniDspCommands : DspCommands {
        override val isLoaded: Boolean
            get() = isNativeLibraryLoaded

        override fun create(sampleRate: Int, channels: Int): Long =
            nativeCreate(sampleRate, channels)

        override fun destroy(handle: Long) = nativeDestroy(handle)

        override fun reset(handle: Long) = nativeReset(handle)

        override fun render(handle: Long, buffer: FloatArray, frames: Int): Int =
            nativeRender(handle, buffer, frames)

        override fun setGain(handle: Long, gain: Float) = nativeSetGain(handle, gain)

        override fun setToneCutoff(handle: Long, cutoffHz: Float) =
            nativeSetToneCutoff(handle, cutoffHz)

        override fun setNoiseMix(handle: Long, mix: Float) = nativeSetNoiseMix(handle, mix)

        override fun setDroneHz(handle: Long, hz: Float) = nativeSetDroneHz(handle, hz)

        override fun version(): String = nativeVersion()
    }

    private external fun nativeCreate(sampleRate: Int, channels: Int): Long
    private external fun nativeDestroy(handle: Long)
    private external fun nativeReset(handle: Long)
    private external fun nativeRender(handle: Long, buffer: FloatArray, frames: Int): Int
    private external fun nativeSetGain(handle: Long, gain: Float)
    private external fun nativeSetToneCutoff(handle: Long, cutoffHz: Float)
    private external fun nativeSetNoiseMix(handle: Long, mix: Float)
    private external fun nativeSetDroneHz(handle: Long, hz: Float)
    private external fun nativeVersion(): String

    companion object {
        const val LIBRARY_NAME = "dreamguard_dsp"
        const val SAMPLE_RATE = 48_000
        const val CHANNELS = 1
        private const val FRAMES_PER_BLOCK = 1024
        private const val RENDER_THREAD_NAME = "dreamguard-audio"
        private const val STOP_THREAD_NAME = "dreamguard-stop"

        /**
         * A blocking `write` that keeps returning 0 means the output cannot make
         * progress. Back off briefly and give up after this many consecutive stalls
         * instead of spinning the render thread forever.
         */
        private const val MAX_CONSECUTIVE_ZERO_WRITES = 20
        private const val ZERO_WRITE_BACKOFF_MS = 5L

        /**
         * Upper bound for a single stop/release call on the caller's thread. A render
         * thread that is stuck in the audio HAL longer than this keeps ownership of the
         * native handle and finishes teardown when it returns.
         */
        const val STOP_JOIN_TIMEOUT_MS = 250L

        private const val MSG_NO_LIBRARY = "Native DSP library is unavailable for this device ABI."
        private const val MSG_SHUTTING_DOWN = "The previous audio session is still shutting down."
        private const val MSG_CREATE = "Native renderer could not be created."
        private const val MSG_OUTPUT = "Audio output could not be opened."

        val isNativeLibraryLoaded: Boolean by lazy {
            runCatching { System.loadLibrary(LIBRARY_NAME) }.isSuccess
        }
    }
}
