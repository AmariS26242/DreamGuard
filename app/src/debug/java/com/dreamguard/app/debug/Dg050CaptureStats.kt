package com.dreamguard.app.debug

import kotlin.math.abs
import kotlin.math.sqrt

/**
 * Aggregate-only PCM statistics for DG-050.
 *
 * The caller's block is consumed in [add] and never retained. Frames are derived
 * from the configured channel count instead of assuming a particular read size.
 * Interval values reset once per Logcat report; totals span the whole run.
 */
internal class Dg050CaptureStats(private val channelCount: Int) {

    data class Interval(
        val framesTotal: Long,
        val nonzeroTotal: Long,
        val peakTotal: Int,
        val nonzeroInterval: Long,
        val peakInterval: Int,
        val rmsInterval: Double,
    )

    data class Totals(
        val frames: Long,
        val nonzero: Long,
        val peak: Int,
    )

    private var samplesTotal = 0L
    private var nonzeroTotal = 0L
    private var peakTotal = 0

    private var samplesInterval = 0L
    private var nonzeroInterval = 0L
    private var peakInterval = 0
    private var sumSquaresInterval = 0.0

    fun add(buffer: ShortArray, count: Int) {
        for (index in 0 until count) {
            val sample = buffer[index].toInt()
            val magnitude = if (sample == Short.MIN_VALUE.toInt()) 32768 else abs(sample)
            if (sample != 0) {
                nonzeroTotal++
                nonzeroInterval++
            }
            if (magnitude > peakTotal) peakTotal = magnitude
            if (magnitude > peakInterval) peakInterval = magnitude
            val squared = sample.toDouble() * sample.toDouble()
            sumSquaresInterval += squared
        }
        samplesTotal += count.toLong()
        samplesInterval += count.toLong()
    }

    fun takeInterval(): Interval {
        val rms = if (samplesInterval == 0L) {
            0.0
        } else {
            sqrt(sumSquaresInterval / samplesInterval.toDouble())
        }
        val snapshot = Interval(
            framesTotal = samplesTotal / channelCount,
            nonzeroTotal = nonzeroTotal,
            peakTotal = peakTotal,
            nonzeroInterval = nonzeroInterval,
            peakInterval = peakInterval,
            rmsInterval = rms,
        )
        samplesInterval = 0L
        nonzeroInterval = 0L
        peakInterval = 0
        sumSquaresInterval = 0.0
        return snapshot
    }

    fun totals(): Totals = Totals(
        frames = samplesTotal / channelCount,
        nonzero = nonzeroTotal,
        peak = peakTotal,
    )
}
