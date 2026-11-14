# DG-014/015 verification

Build with CMake Release and `DREAMGUARD_DSP_BUILD_TOOLS=ON`. Run `dg_dsp_bench`
or `dg_dsp_bench --repeats 1000`; stdout is a single CSV, metadata is stderr.
The tool fails on setup/process errors, nonfinite checksum, or observed process
allocations. Partial setup is released by RAII.

The matrix is 5 rates (8000, 44100, 48000, 96000, 192000), 6 blocks
(64, 128, 256, 512, 1024, 2048), mono/stereo, and 6 separate paths:
compressor, sample limiter, conservative true-peak guard, true-peak meter,
the actual PCM16 compressor/guard/conversion chain, and its bounded observation
snapshot. That gives 360 rows. The snapshot path times the native C query;
it does not include JNI, Kotlin publication or UI costs.
Each path has 32 warmup blocks and 200 measured calls by default, using 32
consecutive blocks of deterministic tone/burst/DC input, then repeating them.

Timing uses steady_clock at normal priority. Median is the mean of the middle
two samples, p95 is nearest-rank, max includes scheduling outliers. xRT is block
audio duration divided by median processing time, so larger is faster.
cpu_us_per_call is process CPU time across the measured loop divided by calls;
it includes clock/bookkeeping overhead, and is not Android app battery usage.
Latency is algorithmic frames/ms; the meter's 8 frames are measurement delay.
RSS and HWM come from /proc/self/status, refer to the entire benchmark process
with all handles/buffers alive, and are not per-component heap sizes. They are
-1 on unsupported platforms. cpp_alloc_calls intercepts scalar/array C++ new,
including nothrow, on the benchmark thread during processing. Calibration must
succeed before measurement. This covers current core allocation sites; it does
not measure malloc inside libc, aligned new, JVM/JNI or background allocations.

## Reproduce on Android x86_64 (PowerShell)

Start an x86_64 emulator, then run from any directory:

```powershell
& E:\DreamGuard\dsp-core\tools\bench\run_android.ps1 `
  -Sdk E:\AgentImeTest\sdk `
  -OutputDirectory E:\DreamGuard\dsp-core\build\my-verification
```

SDK/NDK/CMake must already be installed. Versions and serial can be supplied as
parameters. The script builds all nine native tests, runs fresh executables,
exports 120 limiter fixtures, runs the full benchmark, and records logs and
environment. Android temporary files are retained; no device/app data is deleted.
Fixtures are under `dsp-core/build/dg-stage/fixtures`; subsequent runs replace
the same named fixtures without nesting directories.

Then compare with an independently installed FFmpeg:

```powershell
python dsp-core/tools/bench/verify_true_peak.py `
  --ffmpeg C:/tools/ffmpeg.exe `
  --fixtures dsp-core/build/dg-stage/fixtures `
  --report dsp-core/build/my-verification/reference.csv
```

The comparison uses libswresample at 16x, 128-input-sample filter, 256 extra zero
frames to cover very short files, float64 output, and a fixed +0.05 dB ceiling
tolerance. It checks all output channels including filter tails and records
fixture SHA256. A finite test matrix is not an analog or broadcast certificate.
The production guard is deliberately conservative: its positive coefficient
bound protects both 65-tap and 257-tap 4x reconstructions, with 64-frame context.
This trades extra attenuation of HF/noise for bounded reconstruction during
gain changes. Independent comparison originally rejected the shorter guard
(19/120 failures); both the failed and corrected reports are retained.

Checked-in results and exact environment: [stage evidence](../../../docs/evidence/dg014-015-2026-11-12/README.md).
Emulator timings are regression data and do not establish phone performance.
