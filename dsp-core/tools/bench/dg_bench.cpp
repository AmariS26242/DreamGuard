#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <new>
#include <vector>

#include "dreamguard/compressor.h"
#include "dreamguard/limiter.h"
#include "dreamguard/pcm16_processor.h"
#include "dreamguard/true_peak.h"

// Counts current core allocation sites (new/new[]), not libc/JNI allocations.
namespace audit {
thread_local bool enabled = false;
thread_local uint64_t calls = 0;
void* allocate(std::size_t size) {
    if (enabled) ++calls;
    if (void* p = std::malloc(size ? size : 1)) return p;
    throw std::bad_alloc();
}
}  // namespace audit
void* operator new(std::size_t s) { return audit::allocate(s); }
void* operator new[](std::size_t s) { return audit::allocate(s); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void* operator new(std::size_t s, const std::nothrow_t&) noexcept {
    try {
        return audit::allocate(s);
    } catch (...) {
        return nullptr;
    }
}
void* operator new[](std::size_t s, const std::nothrow_t&) noexcept {
    try {
        return audit::allocate(s);
    } catch (...) {
        return nullptr;
    }
}
void operator delete(void* p, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { std::free(p); }

namespace {
using Clock = std::chrono::steady_clock;
constexpr double pi = 3.14159265358979323846;
volatile double checksum = 0;
struct Resources {
    dg_compressor* c = nullptr;
    dg_limiter* l = nullptr;
    dg_limiter* g = nullptr;
    dg_true_peak* m = nullptr;
    dg_pcm16_processor* p = nullptr;
    ~Resources() {
        if (c) dg_compressor_destroy(c);
        if (l) dg_limiter_destroy(l);
        if (g) dg_limiter_destroy(g);
        if (m) dg_true_peak_destroy(m);
        if (p) dg_pcm16_processor_destroy(p);
    }
};
bool setup(Resources& r, int rate, int ch) {
    dg_compressor_config c{};
    c.sample_rate = rate;
    c.channel_count = ch;
    c.threshold_db = -18;
    c.ratio = 2;
    c.knee_db = 6;
    c.attack_ms = 20;
    c.release_ms = 250;
    c.makeup_gain_db = -6.0206;
    c.link_stereo = 1;
    dg_limiter_config l{};
    l.sample_rate = rate;
    l.channel_count = ch;
    l.ceiling_dbfs = -1;
    l.lookahead_ms = 2;
    l.release_ms = 100;
    l.link_stereo = 1;
    if (dg_compressor_create(&c, &r.c) != DG_OK || dg_limiter_create(&l, &r.l) != DG_OK)
        return false;
    l.true_peak_detection = 1;
    if (dg_limiter_create(&l, &r.g) != DG_OK) return false;
    dg_true_peak_config m{rate, ch};
    if (dg_true_peak_create(&m, &r.m) != DG_OK) return false;
    dg_pcm16_processor_config p{};
    p.sample_rate = rate;
    p.channel_count = ch;
    p.threshold_db = c.threshold_db;
    p.ratio = c.ratio;
    p.knee_db = c.knee_db;
    p.attack_ms = c.attack_ms;
    p.release_ms = c.release_ms;
    p.makeup_gain_db = c.makeup_gain_db;
    p.link_stereo = 1;
    p.limiter_enabled = 1;
    return dg_pcm16_processor_create(&p, &r.p) == DG_OK;
}
void memory_kib(long& rss, long& hwm) {
    rss = hwm = -1;
#if defined(__linux__)
    if (FILE* f = std::fopen("/proc/self/status", "r")) {
        char line[256];
        long v;
        while (std::fgets(line, sizeof(line), f)) {
            if (std::sscanf(line, "VmRSS: %ld kB", &v) == 1) rss = v;
            if (std::sscanf(line, "VmHWM: %ld kB", &v) == 1) hwm = v;
        }
        std::fclose(f);
    }
#endif
}
bool bench(int rate, uint32_t block, int ch, int repeats) {
    Resources r;
    if (!setup(r, rate, ch)) return false;
    const auto samples = static_cast<std::size_t>(block) * ch;
    // Rotate through 32 consecutive blocks. Mixed tone, HF burst and DC step.
    std::vector<float> src(samples * 32), out(samples);
    std::vector<int16_t> pcm(src.size()), pcm_out(samples);
    for (std::size_t n = 0; n < src.size() / ch; ++n)
        for (int c = 0; c < ch; ++c) {
            const double t = static_cast<double>(n) / rate;
            const double x = 0.3 * std::sin(2 * pi * 997 * t + c * 0.2) +
                             (n % 511 < 32 ? 0.55 * std::sin(2 * pi * 0.41 * n) : 0) +
                             (n % 2048 < 100 ? 0.08 : -0.08);
            src[n * ch + c] = static_cast<float>(x);
            pcm[n * ch + c] = static_cast<int16_t>(x * 32767);
        }
    const char* names[] = {"compressor", "sample_limiter", "true_peak_guard", "true_peak_meter",
                           "pcm16_chain"};
    const uint32_t latency[] = {0, dg_limiter_latency_frames(r.l), dg_limiter_latency_frames(r.g),
                                DG_TRUE_PEAK_FILTER_LATENCY,
                                dg_pcm16_processor_latency_frames(r.p)};
    for (int chain = 0; chain < 5; ++chain) {
        auto process = [&](int i) {
            const auto offset = static_cast<std::size_t>(i % 32) * samples;
            dg_pcm_view in{src.data() + offset, block, ch};
            dg_pcm_buffer dst{out.data(), block, ch};
            switch (chain) {
                case 0:
                    return dg_compressor_process(r.c, in, dst);
                case 1:
                    return dg_limiter_process(r.l, in, dst);
                case 2:
                    return dg_limiter_process(r.g, in, dst);
                case 3:
                    return dg_true_peak_process(r.m, in);
                default: {
                    dg_pcm16_processor_values v{};
                    const auto bytes = static_cast<uint32_t>(samples * 2);
                    const auto status = dg_pcm16_processor_process(r.p, pcm.data() + offset, bytes,
                                                                   pcm_out.data(), bytes, 0, &v);
                    return status != DG_OK || v.bytes_written != bytes ? DG_ERR_INVALID_ARGUMENT
                                                                       : DG_OK;
                }
            }
        };
        for (int i = 0; i < 32; ++i)
            if (process(i) != DG_OK) return false;
        std::vector<double> times(repeats);
        audit::calls = 0;
        const auto cpu_start = std::clock();
        for (int i = 0; i < repeats; ++i) {
            audit::enabled = true;
            const auto start = Clock::now();
            const auto status = process(i);
            const auto end = Clock::now();
            audit::enabled = false;
            if (status != DG_OK) return false;
            times[i] = std::chrono::duration<double, std::micro>(end - start).count();
        }
        const double cpu_us = 1e6 * (std::clock() - cpu_start) / CLOCKS_PER_SEC / repeats;
        std::sort(times.begin(), times.end());
        const double median = (times[(repeats - 1) / 2] + times[repeats / 2]) * 0.5;
        const double p95 = times[static_cast<std::size_t>(std::ceil(0.95 * repeats)) - 1];
        long rss, hwm;
        memory_kib(rss, hwm);
        checksum = checksum + out[0] + pcm_out[0];
        std::printf("%s,%d,%u,%d,%d,%.3f,%.3f,%.3f,%.3f,%.3f,%u,%.6f,%ld,%ld,%llu\n", names[chain],
                    rate, block, ch, repeats, median, p95, times.back(), cpu_us,
                    (1e6 * block / rate) / std::max(median, 1e-9), latency[chain],
                    1e3 * latency[chain] / rate, rss, hwm,
                    static_cast<unsigned long long>(audit::calls));
        if (audit::calls != 0 || !std::isfinite(checksum)) return false;
    }
    return true;
}
}  // namespace
int main(int argc, char** argv) {
    int repeats = 200;
    if (argc == 3 && std::strcmp(argv[1], "--repeats") == 0) {
        char* end = nullptr;
        const long value = std::strtol(argv[2], &end, 10);
        if (*end || value < 32 || value > 100000) return 2;
        repeats = static_cast<int>(value);
    } else if (argc != 1) {
        std::fprintf(stderr, "Usage: dg_dsp_bench [--repeats 32..100000]\n");
        return 2;
    }
    audit::calls = 0;
    audit::enabled = true;
    void* probe = ::operator new(37);
    audit::enabled = false;
    ::operator delete(probe);
    if (audit::calls != 1) return 3;
    std::fprintf(stderr,
                 "steady_clock; 32 warmup blocks; normal priority; calibrated new/new[] audit\n");
    std::puts(
        "chain,rate,block,channels,repeats,median_us,p95_us,max_us,cpu_us_per_call,xrt,latency_"
        "frames,latency_ms,rss_kib,hwm_kib,cpp_alloc_calls");
    for (int rate : {8000, 44100, 48000, 96000, 192000})
        for (uint32_t block : {64U, 128U, 256U, 512U, 1024U, 2048U})
            for (int ch : {1, 2})
                if (!bench(rate, block, ch, repeats)) {
                    std::fprintf(stderr, "benchmark failed: %d/%u/%d\n", rate, block, ch);
                    return 3;
                }
    return 0;
}
