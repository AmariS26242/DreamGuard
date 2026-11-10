/* Compile check: the public DSP headers must be includable from plain C.
 *
 * This translation unit is compiled as C by tests/CMakeLists.txt (no C++
 * keyword, no extern "C" block of its own). It also runs a minimal lifecycle and
 * a minimal PCM pass so the headers are verified to be linkable from a C caller,
 * not just parseable.
 */

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "dreamguard/compressor.h"
#include "dreamguard/dsp_core.h"
#include "dreamguard/limiter.h"
#include "dreamguard/lufs.h"
#include "dreamguard/meter.h"
#include "dreamguard/pcm.h"
#include "dreamguard/pcm16_processor.h"

int main(void) {
    dg_config config;
    config.sample_rate = 48000;
    config.channel_count = 1;

    dg_engine* engine = NULL;
    if (dg_create(&config, &engine) != DG_OK) {
        printf("dg_header_c_compat: dg_create failed\n");
        return 1;
    }
    if (engine == NULL) {
        printf("dg_header_c_compat: dg_create reported success with NULL handle\n");
        return 1;
    }
    if (dg_channel_count(engine) != 1) {
        printf("dg_header_c_compat: unexpected channel count\n");
        return 1;
    }

    float block[32];
    memset(block, 0, sizeof(block));
    uint32_t written = 0;
    if (dg_render(engine, block, 32, &written) != DG_OK || written != 32) {
        printf("dg_header_c_compat: dg_render failed\n");
        return 1;
    }
    if (dg_reset(engine) != DG_OK) {
        printf("dg_header_c_compat: dg_reset failed\n");
        return 1;
    }
    if (dg_destroy(engine) != DG_OK) {
        printf("dg_header_c_compat: dg_destroy failed\n");
        return 1;
    }

    /* The error path must be reachable from C too. */
    if (dg_reset(NULL) != DG_ERR_NULL_HANDLE) {
        printf("dg_header_c_compat: NULL handle was not rejected\n");
        return 1;
    }

    /* PCM header: a bit-exact bypass from C code. */
    {
        float input[2] = {0.25f, -0.5f};
        float output[2] = {0.0f, 0.0f};
        dg_pcm_view view;
        view.samples = input;
        view.frame_count = 1;
        view.channel_count = 2;
        dg_pcm_buffer buffer;
        buffer.samples = output;
        buffer.frame_count = 1;
        buffer.channel_count = 2;
        if (dg_pcm_bypass(view, buffer) != DG_OK) {
            printf("dg_header_c_compat: dg_pcm_bypass failed\n");
            return 1;
        }
        if (memcmp(input, output, sizeof(input)) != 0) {
            printf("dg_header_c_compat: dg_pcm_bypass was not bit exact\n");
            return 1;
        }
        int silent = 0;
        if (dg_pcm_is_silent(view, &silent) != DG_OK || silent != 0) {
            printf("dg_header_c_compat: dg_pcm_is_silent failed\n");
            return 1;
        }
    }

    /* DG-012 meter header: create, one block, snapshot, destroy from C. */
    {
        dg_meter_config meter_config;
        dg_meter* meter = NULL;
        meter_config.sample_rate = 48000;
        meter_config.channel_count = 1;
        meter_config.window_ms = 0;
        if (dg_meter_create(&meter_config, &meter) != DG_OK || meter == NULL) {
            printf("dg_header_c_compat: dg_meter_create failed\n");
            return 1;
        }
        {
            float samples[4] = {0.5f, -0.5f, 0.25f, -0.25f};
            dg_pcm_view view;
            dg_meter_values values;
            view.samples = samples;
            view.frame_count = 4;
            view.channel_count = 1;
            memset(&values, 0, sizeof(values));
            if (dg_meter_process(meter, view) != DG_OK) {
                printf("dg_header_c_compat: dg_meter_process failed\n");
                return 1;
            }
            if (dg_meter_snapshot(meter, &values) != DG_OK) {
                printf("dg_header_c_compat: dg_meter_snapshot failed\n");
                return 1;
            }
            if (values.total_frames != 4 || values.block_peak[0] != 0.5f) {
                printf("dg_header_c_compat: unexpected meter values\n");
                return 1;
            }
            if (dg_meter_linear_to_dbfs(1.0f) != 0.0f) {
                printf("dg_header_c_compat: dg_meter_linear_to_dbfs failed\n");
                return 1;
            }
        }
        if (dg_meter_destroy(meter) != DG_OK) {
            printf("dg_header_c_compat: dg_meter_destroy failed\n");
            return 1;
        }
    }

    /* DG-012 LUFS header: the coefficient struct and the analyser from C. */
    {
        dg_lufs_config lufs_config;
        dg_lufs* lufs = NULL;
        dg_lufs_k_weighting weighting;
        lufs_config.sample_rate = 48000;
        lufs_config.channel_count = 1;
        if (dg_lufs_k_weighting_for_rate(48000, &weighting) != DG_OK) {
            printf("dg_header_c_compat: dg_lufs_k_weighting_for_rate failed\n");
            return 1;
        }
        if (!(weighting.shelf.a2 > 0.0 && weighting.shelf.a2 < 1.0)) {
            printf("dg_header_c_compat: unstable K-weighting coefficients\n");
            return 1;
        }
        if (dg_lufs_create(&lufs_config, &lufs) != DG_OK || lufs == NULL) {
            printf("dg_header_c_compat: dg_lufs_create failed\n");
            return 1;
        }
        {
            float samples[4800];
            dg_pcm_view view;
            dg_lufs_values values;
            memset(samples, 0, sizeof(samples));
            memset(&values, 0, sizeof(values));
            view.samples = samples;
            view.frame_count = 4800;
            view.channel_count = 1;
            if (dg_lufs_process(lufs, view) != DG_OK) {
                printf("dg_header_c_compat: dg_lufs_process failed\n");
                return 1;
            }
            if (dg_lufs_snapshot(lufs, &values) != DG_OK) {
                printf("dg_header_c_compat: dg_lufs_snapshot failed\n");
                return 1;
            }
            if (values.hop_frames != 4800 || values.integrated_ready != 0) {
                printf("dg_header_c_compat: unexpected LUFS values\n");
                return 1;
            }
        }
        if (dg_lufs_destroy(lufs) != DG_OK) {
            printf("dg_header_c_compat: dg_lufs_destroy failed\n");
            return 1;
        }
    }

    /* DG-013 compressor header: the config struct, the pure curve query and a
     * whole process/snapshot cycle from C. */
    {
        dg_compressor_config compressor_config;
        dg_compressor* compressor = NULL;
        dg_compressor_values compressor_values;
        double curve_gain = 0.0;
        float compressor_samples[480];
        dg_pcm_view compressor_view;
        dg_pcm_buffer compressor_out;
        int i;

        compressor_config.sample_rate = 48000;
        compressor_config.channel_count = 1;
        compressor_config.threshold_db = -20.0;
        compressor_config.ratio = 4.0;
        compressor_config.knee_db = 0.0;
        compressor_config.attack_ms = 10.0;
        compressor_config.release_ms = 100.0;
        compressor_config.link_stereo = 1;
        compressor_config.makeup_gain_db = 0.0;

        /* -10 dBFS is 10 dB over the threshold at 4:1: -7.5 dB of reduction. */
        if (dg_compressor_static_gain_db(&compressor_config, -10.0, &curve_gain) !=
                DG_OK ||
            fabs(curve_gain + 7.5) > 1e-9) {
            printf("dg_header_c_compat: dg_compressor_static_gain_db failed\n");
            return 1;
        }

        if (dg_compressor_create(&compressor_config, &compressor) != DG_OK ||
            compressor == NULL) {
            printf("dg_header_c_compat: dg_compressor_create failed\n");
            return 1;
        }
        for (i = 0; i < 480; ++i) {
            compressor_samples[i] = (i % 2 == 0) ? 0.5f : -0.5f;
        }
        memset(&compressor_values, 0, sizeof(compressor_values));
        compressor_view.samples = compressor_samples;
        compressor_view.frame_count = 480;
        compressor_view.channel_count = 1;
        compressor_out.samples = compressor_samples;
        compressor_out.frame_count = 480;
        compressor_out.channel_count = 1;
        if (dg_compressor_process(compressor, compressor_view, compressor_out) !=
            DG_OK) {
            printf("dg_header_c_compat: dg_compressor_process failed\n");
            return 1;
        }
        if (dg_compressor_snapshot(compressor, &compressor_values) != DG_OK) {
            printf("dg_header_c_compat: dg_compressor_snapshot failed\n");
            return 1;
        }
        if (compressor_values.frames != 480 || compressor_values.linked != 1 ||
            compressor_values.clip_samples != 0 ||
            !(compressor_values.gain_reduction_db < 0.0)) {
            printf("dg_header_c_compat: unexpected compressor values\n");
            return 1;
        }
        if (dg_compressor_destroy(compressor) != DG_OK) {
            printf("dg_header_c_compat: dg_compressor_destroy failed\n");
            return 1;
        }
    }

    /* DG-014 limiter header: create, limit, snapshot, flush and destroy from C. */
    {
        dg_limiter_config limiter_config;
        dg_limiter* limiter = NULL;
        dg_limiter_values limiter_values;
        float limiter_samples[512];
        float limiter_tail[48];
        dg_pcm_view limiter_view;
        dg_pcm_buffer limiter_out;
        dg_pcm_buffer limiter_tail_out;
        uint32_t flushed = 0;
        double ceiling;
        int i;

        limiter_config.sample_rate = 48000;
        limiter_config.channel_count = 1;
        limiter_config.ceiling_dbfs = -6.0;
        limiter_config.lookahead_ms = 1.0;
        limiter_config.release_ms = 50.0;
        limiter_config.link_stereo = 1;
        ceiling = pow(10.0, -6.0 / 20.0);

        if (dg_limiter_create(&limiter_config, &limiter) != DG_OK || limiter == NULL) {
            printf("dg_header_c_compat: dg_limiter_create failed\n");
            return 1;
        }
        if (dg_limiter_latency_frames(limiter) != 48) {
            printf("dg_header_c_compat: unexpected limiter latency\n");
            return 1;
        }
        for (i = 0; i < 512; ++i) {
            limiter_samples[i] = 0.9f;
        }
        memset(&limiter_values, 0, sizeof(limiter_values));
        limiter_view.samples = limiter_samples;
        limiter_view.frame_count = 512;
        limiter_view.channel_count = 1;
        limiter_out.samples = limiter_samples;
        limiter_out.frame_count = 512;
        limiter_out.channel_count = 1;
        if (dg_limiter_process(limiter, limiter_view, limiter_out) != DG_OK) {
            printf("dg_header_c_compat: dg_limiter_process failed\n");
            return 1;
        }
        if (dg_limiter_snapshot(limiter, &limiter_values) != DG_OK) {
            printf("dg_header_c_compat: dg_limiter_snapshot failed\n");
            return 1;
        }
        if (limiter_values.frames != 512 || limiter_values.linked != 1 ||
            limiter_values.over_ceiling_samples != 0 ||
            !(limiter_values.limited_samples > 0) ||
            !(limiter_values.current_gain_db < 0.0)) {
            printf("dg_header_c_compat: unexpected limiter values\n");
            return 1;
        }
        for (i = 0; i < 512; ++i) {
            if (fabs((double)limiter_samples[i]) > ceiling + 1e-6) {
                printf("dg_header_c_compat: limiter exceeded the sample ceiling\n");
                return 1;
            }
        }
        limiter_tail_out.samples = limiter_tail;
        limiter_tail_out.frame_count = 48;
        limiter_tail_out.channel_count = 1;
        if (dg_limiter_flush(limiter, limiter_tail_out, &flushed) != DG_OK ||
            flushed != 48) {
            printf("dg_header_c_compat: dg_limiter_flush failed\n");
            return 1;
        }
        if (dg_limiter_destroy(limiter) != DG_OK) {
            printf("dg_header_c_compat: dg_limiter_destroy failed\n");
            return 1;
        }
    }

    /* DG-022 PCM16 bridge: create, bit-exact bypass, destroy from plain C. */
    {
        dg_pcm16_processor_config bridge_config;
        dg_pcm16_processor* bridge = NULL;
        uint8_t bridge_input[8] = {0x01, 0x00, 0x02, 0x00,
                                   0x03, 0x00, 0x04, 0x00};
        uint8_t bridge_output[8];
        dg_pcm16_processor_values bridge_values;

        bridge_config.sample_rate = 48000;
        bridge_config.channel_count = 1;
        bridge_config.threshold_db = -20.0;
        bridge_config.ratio = 1.0;
        bridge_config.knee_db = 0.0;
        bridge_config.attack_ms = 20.0;
        bridge_config.release_ms = 250.0;
        bridge_config.makeup_gain_db = 0.0;
        bridge_config.link_stereo = 1;

        memset(bridge_output, 0, sizeof(bridge_output));
        memset(&bridge_values, 0, sizeof(bridge_values));
        if (dg_pcm16_processor_create(&bridge_config, &bridge) != DG_OK ||
            bridge == NULL) {
            printf("dg_header_c_compat: dg_pcm16_processor_create failed\n");
            return 1;
        }
        if (dg_pcm16_processor_process(
                bridge, bridge_input, (uint32_t)sizeof(bridge_input), bridge_output,
                (uint32_t)sizeof(bridge_output), 1, &bridge_values) != DG_OK) {
            printf("dg_header_c_compat: dg_pcm16_processor_process failed\n");
            return 1;
        }
        if (memcmp(bridge_input, bridge_output, sizeof(bridge_input)) != 0 ||
            bridge_values.bytes_written != (uint32_t)sizeof(bridge_input) ||
            bridge_values.bypassed != 1 || bridge_values.frames != 4) {
            printf("dg_header_c_compat: unexpected PCM16 bridge values\n");
            return 1;
        }
        if (dg_pcm16_processor_latency_frames() != 0) {
            printf("dg_header_c_compat: unexpected PCM16 bridge latency\n");
            return 1;
        }
        if (dg_pcm16_processor_destroy(bridge) != DG_OK) {
            printf("dg_header_c_compat: dg_pcm16_processor_destroy failed\n");
            return 1;
        }
    }

    printf("dg_header_c_compat: PASSED\n");
    return 0;
}
