# dsp-core — portable DreamGuard DSP core

C++20 soundscape generator with its **DG-010 lifecycle C API**, the
**stateless Float32 PCM helpers** from DG-011, the **streaming meter plus a
PROVISIONAL BS.1770-4-style loudness analyser** from DG-012, the **DG-013
feed-forward compressor**, the **DG-014 sample-peak lookahead limiter**, and the
**DG-022 PCM16 bridge** that lets Android local playback reach the compressor over
JNI. No Android, no JNI, no platform headers: the same sources build for the host
(tests) and for Android (as a subdirectory of `app/src/main/cpp/CMakeLists.txt`).

> **DG-012 is `IN_PROGRESS`, not verified.** The meter and LUFS sources are now
> compiled and tested on Android x86_64 (API 35 emulator): `dg_meter` 207 checks /
> 0 failures, `dg_lufs` 238 / 0, extended C-header smoke passed, and Debug/Release
> build all three ABIs. What is still missing: no run on a real device, no
> ASan/LSan job, arm64-v8a/armeabi-v7a have only been built, and the LUFS output
> has **never been compared against a reference implementation** in a run that
> passes — the harness for that comparison exists (`tools/lufs_compare/`); its
> `--selfcheck` has now run twice (round 13: 113 checks / 1 failure, a bug in the
> tool's signal generator; fixed, round 14: **115 checks / 0 failures**) and the
> reference comparison itself ran for the first time in round 15:
> **14 PASS / 2 FAIL / 0 SKIP, worst |delta| 0.049 LU, overall FAIL (exit 1)** —
> report at `build/lufs-compare-android/reference-report.md`. The two failures are
> `kind=undefined` cases (2 s digital silence, 0.3 s stream): we report
> `integrated = -inf` with `integrated_ready = 0`, FFmpeg's `ebur128` prints
> `I: -70.0 LUFS`, which is its *sentinel* for "no 400 ms block passed the −70 LUFS
> absolute gate" (its `integrated_loudness` field starts at `ABS_THRES = -70`), not a
> measurement. Both are left **not passing**, pending a human ruling; the rule was not
> relaxed. A green self-check proves the tool and the analyser agree with each other,
> not with the standard. FFmpeg 9.0.2 sits inside `build/reference-deps/` (not a system
> install) and `compare_with_ffmpeg.ps1` takes a pre-generated `-Csv`/`-WavDir` pair,
> which is how round 15 ran on a host without a C++ compiler. Note the reference only
> prints one decimal (`%5.1f`), so every delta carries ±0.05 LU of rounding and
> 0.049 LU cannot distinguish "close" from "the same".
> Treat `dreamguard/lufs.h` as provisional.
>
> **DG-013 is `DONE` but not `VERIFIED`.** `compressor.h/.cpp` and
> `tests/test_compressor.cpp` were written in round 10. The first real compile
> (round 11, NDK x86_64) failed on an unterminated comment in `compressor.h`;
> that was fixed, the rerun reached 334 checks / 2 failures (both wrong *test*
> expectations: the peak meters report the peak of the whole last block, attack
> included, and a float sample cannot sit exactly on the threshold), those were
> rewritten against analytic values, and the third run came back
> **348 checks / 0 failures** with the extended C-header check PASSING and all
> three ABIs building. Round 23 changed the output descriptor type to `dg_pcm_buffer`
> (it was a read-only `dg_pcm_view` with a hidden `const_cast`) and re-ran the suite
> on the same x86_64 emulator: **348 checks / 0 failures** again. That covers the
> signal-level acceptance only: there is no listening test, no reference compressor
> comparison, no real-device run, and although the component is now wired into the
> PCM16 bridge, no device playback has exercised it — do not describe it as a
> finished or audibly verified compressor.
>
> **DG-014 is `IN_PROGRESS`; its sample-peak tests have run, true peak has not.**
> `limiter.h`, `limiter.cpp`, `tests/test_limiter.cpp` and the CMake/CTest wiring
> were written in round 17. The caller then ran the suite on an NDK x86_64 API 35
> emulator: **2113 checks / 0 failures**, and
> `testDebugUnitTest assembleDebug assembleRelease` succeeded for all three ABIs
> with the JVM tests `up-to-date`. That is evidence for the sample-peak path only.
> The component bounds **sample peak only**: it has no oversampling and no
> reconstruction, so it does not detect or bound inter-sample/true peak. A
> true-peak acceptance claim needs an oversampled measurement that has not been
> performed. It is not wired into playback.

## Layout

```
dsp-core/
├── include/dreamguard/dsp_core.h   public C99 API (opaque handle, error codes)
├── include/dreamguard/pcm.h        public C99 API for Float32 PCM helpers
├── include/dreamguard/meter.h      public C99 API for the streaming meter (DG-012)
├── include/dreamguard/lufs.h       public C99 API for LUFS (DG-012, PROVISIONAL)
├── include/dreamguard/compressor.h public C99 API for the compressor (DG-013)
├── include/dreamguard/limiter.h    public C99 API for the limiter (DG-014, sample peak)
├── include/dreamguard/pcm16_processor.h public C99 PCM16 <-> compressor bridge (DG-022)
├── src/dg_engine.cpp               lifecycle implementation (no global state)
├── src/compressor.cpp              threshold / ratio / soft knee / attack / release
├── src/pcm16_processor.cpp         PCM16 <-> float conversion, chunking, bit-exact bypass
├── src/limiter.cpp                 lookahead delay + sliding-minimum sample-peak limiter
├── src/lufs.cpp                    K-weighting, gating, momentary/short/integrated
├── src/meter.cpp                   streaming RMS / sample peak / rolling window
├── src/pcm.cpp                     stateless bypass / gain / channel mapping
├── src/soundscape.{h,cpp}          the generator moved from app/src/main/cpp
├── tests/test_dg_lifecycle.cpp     lifecycle / validation / concurrency tests
├── tests/test_compressor.cpp       curve / ballistics / linking / click-free tests
├── tests/test_limiter.cpp          delay / sample-peak bound / flush / linking tests
├── tests/test_lufs.cpp             K-weighting / windows / gating / chunking tests
├── tests/test_meter.cpp            window / peak / clip / chunking tests
├── tests/test_pcm.cpp              PCM bit-exactness / mapping / validation tests
├── tests/test_pcm16_processor.cpp  PCM16 conversion / bypass / chunking / lifecycle tests (DG-022)
├── tests/test_header_c_compat.c    plain-C consumer of all public headers
├── tools/lufs_compare/             DG-012 reference-comparison harness (host only)
│   ├── dg_lufs_compare.cpp         synthetic PCM + our readings + CSV/WAV output
│   ├── compare_with_ffmpeg.ps1     pairs the readings with ffmpeg's ebur128 filter
│   └── README.md                   how to run it, tolerance, what counts as evidence
├── CMakeLists.txt                  static library `dreamguard_dsp_core`
└── build.gradle.kts                Gradle `:dsp-core` module entry points
```

## Scope boundary

- `dreamguard/dsp_core.h` is the soundscape generator and its lifetime API. It is
  **not** a PCM processor: no input stream, no resampler, no format conversion.
- `dreamguard/pcm.h` is a small set of stateless buffer transforms (bit-exact
  copy, gain, mono/stereo mapping, silence test). It is **not** an audio capture
  implementation — no AudioRecord, OpenSL, AAudio or microphone path — and it is
  not wired into the playback pipeline yet.
- Neither header claims third-party audio capture. DG-011 is `DONE` but **not**
  `VERIFIED`: the helpers are not wired into the playback pipeline, have not
  processed real audio, and no leak/race checking has been run. See
  `docs/STATUS.md` for the exact evidence.
- `dreamguard/meter.h` is a streaming measurement object (RMS, **sample** peak,
  rolling window, peak hold, clip and non-finite counters). It reads caller
  buffers only; it has no device, no capture path and no third-party volume
  control. Its peak is **not** a true peak (no oversampling).
- `dreamguard/lufs.h` is a **PROVISIONAL** BS.1770-4-style integrated loudness
  analyser. It reproduces the published 48 kHz K-weighting table to 1e-6 and is
  anchored to a 1 kHz calibration with a known ≈0.024 LU offset, but it has
  **not** been compared with libebur128, ffmpeg or any commercial meter, so it
  must not be advertised as standards-compliant. It is not wired into playback
  either. The comparison harness that produces that evidence lives in
  `tools/lufs_compare/`: its self-check reached 115/0 in round 14 and the first
  real comparison (round 15, summary-only path) was 14 PASS / 2 FAIL / 0 SKIP,
  overall FAIL. Round 16 rewrote the script to use three-decimal
  `lavfi.r128.I` metadata and a NO-DATA classification, but that path has **not
  been executed**.
- `dreamguard/compressor.h` is a **feed-forward** dynamic range compressor:
  threshold, ratio, symmetric soft knee, attack, release, optional stereo
  linking and a makeup gain, with the gain reduction itself smoothed so the gain
  cannot jump. It has **no lookahead**, so it adds no latency and cannot catch a
  transient before it happens; it has **no limiter and no true-peak detector**,
  so a positive makeup gain can and does overshoot 0 dBFS (those samples are
  counted, not clipped). That is DG-014's job, not this component's. It is now
  called by the DG-022 PCM16 bridge, but no device playback has exercised it.
- `dreamguard/limiter.h` is a **sample-peak** lookahead limiter: it delays the
  signal, looks ahead, and guarantees `|y[n]| <= 10^(ceiling_dbfs/20)` for the
  finite samples it processes. It has **no oversampling and no reconstruction**,
  so it cannot bound the true (inter-sample) peak. `process`/`flush`/`snapshot`
  do not allocate; it is not wired into playback. Its sample-peak suite ran on
  x86_64 only (**2113 checks / 0 failures**); true peak has not been measured.

## API summary

| Function | Purpose |
| --- | --- |
| `dg_create(const dg_config*, dg_engine**)` | validates `sample_rate ∈ [8000, 192000]` and `channel_count ∈ [1, 2]`, allocates, returns the handle |
| `dg_destroy(dg_engine*)` | releases the handle exactly once |
| `dg_reset(dg_engine*)` | clears phases, filter memory, noise seed and fade ramp; parameters survive |
| `dg_render(...)`, `dg_set_*` | expose the existing generator to the JNI bridge (kept so the Android ABI is unchanged) |
| `dg_version()`, `dg_channel_count()` | version string and channel count of a live handle |
| `dg_pcm_bypass`, `dg_pcm_apply_gain`, `dg_pcm_map_channels`, `dg_pcm_is_silent` | stateless Float32 PCM helpers (DG-011) |
| `dg_meter_create/destroy/reset/reset_peak_hold` | meter lifetime, zeroing and peak-hold reset (DG-012) |
| `dg_meter_process(view)`, `dg_meter_snapshot(values)` | stream frames, then read the current block/window/total/peak values |
| `dg_meter_linear_to_dbfs(float)` | pure `20*log10(x)` helper (0 → `-INFINITY`) |
| `dg_lufs_create/destroy/reset`, `dg_lufs_process`, `dg_lufs_snapshot` | PROVISIONAL loudness analyser (DG-012) |
| `dg_lufs_k_weighting_for_rate(rate, out)` | the K-weighting biquad coefficients the analyser uses |
| `dg_compressor_create/destroy/reset`, `dg_compressor_process`, `dg_compressor_snapshot` | feed-forward compressor (DG-013, 348/0 on x86_64 emulator) |
| `dg_compressor_static_gain_db(config, level_db, out)` | pure query for the static knee/ratio curve, in dB of gain reduction |
| `dg_limiter_create/destroy/reset`, `dg_limiter_process`, `dg_limiter_flush`, `dg_limiter_snapshot` | sample-peak lookahead limiter (DG-014, 2113/0 on x86_64 emulator; true peak not measured) |
| `dg_limiter_latency_frames(limiter)` | delay introduced by the lookahead, in frames (0 for NULL) |
| `dg_pcm16_processor_create/destroy/reset`, `dg_pcm16_processor_process`, `dg_pcm16_processor_latency_frames` | PCM16 <-> float <-> DG-013 compressor bridge used by Android local playback (DG-022; 27 groups, **206/0** on x86_64 emulator; **not** device-playback verified) |

### PCM buffer aliasing (DG-011)

`dg_pcm_*` takes plain descriptors, so it has to state what happens when input
and output memory overlap:

- identical pointer, same shape: defined in-place operation for `dg_pcm_bypass`
  (a validated no-op), `dg_pcm_apply_gain` (one read and one write per sample)
  and `dg_pcm_map_channels` with equal channel counts;
- identical pointer with a channel conversion (mono↔stereo): rejected with
  `DG_ERR_INVALID_ARGUMENT` — an in-place up-mix would overwrite input it has not
  read yet;
- any partially overlapping distinct buffers: rejected with
  `DG_ERR_INVALID_ARGUMENT` and **nothing is written** (handing overlap to
  `memcpy` is undefined behaviour).

Sample indices are widened to `std::size_t` before multiplying, so
`frame * 2` cannot wrap in 32-bit arithmetic on a 64-bit host.

### Meter and loudness (DG-012, phase 1)

`dg_meter` is stateful: create it with a sample rate (8000–192000), 1–2 channels
and a window length (`window_ms` 1–1000, 0 = 400 ms default), then call
`dg_meter_process` with successive interleaved blocks and `dg_meter_snapshot`
whenever you need numbers.

- `block_*` describe the **last non-empty** `process` call; a zero-frame call is a
  no-op and does not erase them.
- `window_*` cover the last `round(fs * window_ms / 1000)` frames (at least 1),
  stored as `|x|` in a ring; the ring is `window_frames * channels * 4` bytes
  (153,600 B for 48 kHz / 400 ms stereo, 1,536,000 B at the 192 kHz / 1000 ms
  limit).
- `total_*` accumulate from `create`/`reset`. `peak_hold` is the largest `|x|`
  since `create`/`reset_peak_hold`.
- `clip_samples` counts finite samples with `|x| > 1.0` — exactly 1.0 does not
  count. NaN/±Inf are counted in `non_finite_samples` and contribute 0 to energy,
  peak and window.
- Peaks are **sample** peaks; no oversampling, so this is not a true-peak meter.
- One handle must be driven by one caller at a time; there is no lock.

`dg_lufs` implements the BS.1770-4 *shape*: K-weighting (analog prototype mapped
with the bilinear transform, so any supported rate works; at 48 kHz the
published table is reproduced to 1e-6), 400 ms blocks at 75 % overlap, a −70
LUFS absolute gate and a relative gate at "gated mean − 10 LU", with momentary
(400 ms), short-term (3 s, `-INFINITY` until 3 s of history exists) and
integrated values. Silent or empty input yields `-INFINITY` and
`integrated_ready == 0`. Channel weight is 1.0, so an equal-amplitude mono signal
reads 10·log10(2) ≈ 3.0103 LU lower than stereo. The block store holds 36,000
blocks (1 hour at 10 blocks/s) and then drops the oldest, counting
`blocks_dropped`.

A stereo 1 kHz sine of peak amplitude A reads `20*log10(A) - 0.024` LUFS: the
K-weighting gain at 1 kHz is ≈ +0.667 dB, which does not exactly cancel the
−0.691 offset. That 0.024 LU bias is documented rather than hidden; the test
tolerance is 0.1 LU. Everything above is **provisional** until the output is
compared with a reference meter.

### Compressor (DG-013, DONE — not verified; wired into the PCM16 bridge, not device-tested)

`dg_compressor` is deliberately the *minimum* compressor, and its signal path is
short enough to verify by hand:

1. the detector is the **instantaneous sample magnitude** of each frame (the
   maximum over the channels when `link_stereo` is 1). No hold, no decay, no
   oversampling — it is not a true-peak detector;
2. that level is converted to dBFS with a floor of `1e-12`, so silence reads
   −240 dBFS instead of `-INFINITY`;
3. the static curve, with `x = level_db - threshold_db` and `k = knee_db`:
   `0` for `x <= -k/2`, `(1/ratio - 1) * x` for `x >= k/2`, and
   `(1/ratio - 1) * (x + k/2)^2 / (2k)` in between. `knee_db = 0` is a hard knee,
   the knee is always centred on the threshold, and `ratio = 1` is a pass-through;
4. the **gain reduction** is a one-pole follower of that target with separate
   attack and release time constants
   (`coef = 1 - exp(-1 / (ms/1000 * fs))`). Smoothing the gain rather than the
   level is what makes a click impossible: the gain moves by at most
   `(target - current) * coef` per frame, whatever the input does;
5. the frame is multiplied by `10^((reduction + makeup_gain_db) / 20)`.

`dg_compressor_process` takes a read-only `dg_pcm_view` for the input and a writable
`dg_pcm_buffer` for the output, the same convention as `dg_limiter_process` and the
`dg_pcm_*` helpers. The exact in-place case passes the same pointer in both
descriptors; partial overlap is rejected before anything is written.

Consequences worth stating out loud: a steady 1 kHz sine settles **near** the
curve's peak-based value but not exactly on it, because a per-sample detector
sees the sine's instantaneous magnitude rather than its peak. The tests therefore
assert exact values only for constant-magnitude signals and use a ±0.5 dB bound
for a sine. `clip_samples` counts finite output samples with `|y| > 1.0`; there is
no limiter behind this component. One handle, one thread, no locks, no allocation
in `process`.

Two measurement facts that the first real run (round 11) made explicit, and that
the tests now assert instead of assuming:

- `input_peak_db` / `output_peak_db` describe **the whole last block**, so a single
  block started from 0 dB of reduction reports the *attack* peak, not the settled
  one. The settled peak needs a second block through the same handle; the first
  block's peak is `level_db + target * (1 - exp(-1/(attack*fs)))`, i.e. exactly one
  attack step, which is itself a useful check of the ballistics.
- A float sample cannot be exactly at the threshold: the closest float to
  `10^(-20/20)` is `0.100000001490116`, which is 1.3e-7 dB *above* −20 dBFS. A hard
  knee therefore produces a −1e-7 dB reduction there, and that is the correct
  answer. The exactly-at-threshold case is only checkable in double, through
  `dg_compressor_static_gain_db`, where it returns exactly `0.0`. Stream-level
  "no compression" is asserted at a level safely below the threshold, bitwise.

### Limiter (DG-014, IN_PROGRESS — sample peak tested, true peak not measured)

`dg_limiter` is the minimum safety limiter, and its guarantee is deliberately
narrow:

1. per frame and channel, `req = min(1, ceiling / |x|)` (linked stereo uses the
   louder channel for both). Non-finite input is replaced by 0 and counted;
2. a monotonic deque holds the minimum `req` over the last `latency + 1` frames.
   The gain drops to that minimum immediately (instant attack) or releases toward
   it with `1 - exp(-1 / (release_ms/1000 * fs))`, and is clamped to it every
   frame;
3. the output is `y[n] = x[n - latency] * gain[n]`. Because
   `gain[n] <= req[n - latency]`, every finite **sample** of the output is at or
   below the linear ceiling. This is a sample-peak bound, not a true-peak bound;
4. the first `latency` output frames are zeros; the last `latency` input frames
   stay in the delay line until `dg_limiter_flush`, which emits exactly that many
   frames and then resets the envelope/delay/window (counters survive).

`latency = ceil(lookahead_ms * rate / 1000)`, so 20 ms at 192 kHz is 3840
frames. The instant attack is safe for the bound but is a step in the gain, so a
hard transient can click; the smoothed DG-013 compressor remains the click-free
path. `process`, `flush` and `snapshot` do not allocate or lock. In-place
processing is allowed; any other overlap is rejected.

**What has and has not happened:** the caller ran the suite on an NDK x86_64 API 35
emulator (**2113 checks / 0 failures**) and the Gradle build/test tasks succeeded
for three ABIs. There has been **no true-peak (oversampled) measurement**, no
listening test, no real device, and no playback integration. The test file covers
parameter validation, NULL/overlap handling, exact delay and pass-through, an
impulse at the latency index, sample-peak bounds across ceilings/rates/
lookaheads/channels, linked vs unlinked stereo, chunking invariance, a
full-window increasing-gain case (the sliding-minimum deque case that is easy to
get wrong), flush/reset, in-place processing, NaN/Inf, the maximum 20 ms
lookahead, silence and a final-frame impulse. Those assertions passed in that one
x86_64 emulator configuration; do not generalize the count to other ABIs or call
the true-peak requirement satisfied.

### Reference comparison harness (DG-012)

`tools/lufs_compare/` closes exactly that gap, and it is deliberately kept out of
the library so it can never be mistaken for shipped code:

- `dg_lufs_compare` synthesises sixteen deterministic signals (mono and stereo
  1 kHz at −20/−23/−6/−60 dBFS, 100 Hz and 10 kHz for the K-weighting shape, an
  asymmetric stereo pair, 8/44.1/48/96/192 kHz, deterministic xorshift noise, a
  7 s −20 dBFS → silence → −50 dBFS step that exercises the relative gate, 2 s of
  digital silence, and a 0.3 s stream), measures each with `dg_lufs`, and can
  write every signal as a 32-bit float WAV plus a CSV of its readings.
- `compare_with_ffmpeg.ps1` runs
  `ffmpeg -af ebur128=metadata=1:peak=true,ametadata=mode=print:file=<temp>` over
  those exact WAV bytes, reads the last `lavfi.r128.I` (0.01 LU histogram
  resolution), cross-checks the one-decimal summary line, and writes
  `dsp-core/build/lufs-compare/reference-report.md` with the per-case deviation.
  The acceptance tolerance is **0.1 LU** (EBU Tech 3341); the script exits
  non-zero rather than widening it, and a missing ffmpeg exits `3` with the exact
  commands to run by hand instead of silently "passing".
  It takes the material two ways: **native** (it runs the tool itself) or
  **pre-generated** (`-Csv ours.csv [-WavDir dir]`, for a CSV and WAVs produced
  elsewhere — e.g. by the Android binary on an emulator and `adb pull`ed — with the
  inputs treated as read-only and never deleted). Silence and the 0.3 s stream are
  marked `kind = undefined`: the high-precision path reports `NO-DATA` only when
  our side has no value, the metadata and summary agree, `lavfi.r128.I` is the
  `-70.000` sentinel and **every** `lavfi.r128.M` block is strictly below −70;
  `NO-DATA` is never counted as a pass, and any other mismatch is `FAIL`. The
  report also records the SHA256 of the CSV and of every WAV, so the evidence says
  which bytes each meter measured.
- `--selfcheck` (CTest `dg_lufs_compare`) needs nothing but the built binary and
  asserts harness/analyser internal consistency: chunking invariance, the
  analytic K-weighted value of every steady sine, mono-vs-stereo exactly
  `10*log10(2)`, `integrated_ready == 0` for silence and for a sub-400 ms
  stream, the `blocks == 67 / blocks_after_absolute_gate >= 60 / blocks_used == 40`
  gate bookkeeping plus the quiet tail's analytic `momentary`, and the WAV
  container layout. It is **not** agreement evidence.

Status: the `--selfcheck` path **has been executed twice** (NDK x86_64, API 35
emulator). Round 13: **113 checks / 1 failure** — the level-step generator put the
quiet section at −70 dBFS instead of −50 dBFS. That was a bug in this tool, not in
`dg_lufs`; it was fixed (absolute −50 dBFS, plus a new check that the quiet tail's
`momentary` matches its analytic value) and round 14 reran it:
**115 checks / 0 failures** (the two extra checks are the new ones, which is what
proves the fixed binary ran).

The reference comparison ran for the first time in round 15 (pre-generated mode, the
caller's host, FFmpeg 9.0.2): **14 PASS / 2 FAIL / 0 SKIP, worst |delta| 0.049 LU,
overall FAIL, exit 1**, report in `build/lufs-compare-android/reference-report.md`.
The 14 comparable cases span 8 / 44.1 / 48 / 96 / 192 kHz, mono and stereo,
−6 … −60 dBFS, 100 Hz / 1 kHz / 10 kHz, asymmetric stereo, deterministic noise and a
level step; deltas run from −0.049 (10 kHz) to +0.030 (level step). The two failures
are the undefined-by-definition cases: 2 s of digital silence and a 0.3 s stream (no
complete 400 ms block). We report `integrated = -inf` + `integrated_ready = 0`;
FFmpeg prints `I: -70.0 LUFS`, which is its sentinel for "nothing passed the absolute
gate" (`ABS_THRES` initialises the field), so the two sides agree on the fact but
express it differently. Per the verdict rules both stay **not passing**, awaiting a
human ruling — the tolerance was not widened.

Resolution limit: FFmpeg prints the summary with `%5.1f`, i.e. 0.1 LU, so every delta
in the table above carries ±0.05 LU of rounding and the 0.1 LU tolerance *is* one
quantisation step. That is not enough to resolve the documented ≈0.024 LU 1 kHz bias
or the 100 Hz / 10 kHz figures. Round 16 therefore replaced the reference reading
with `ebur128=metadata=1` + `ametadata=mode=print` and the three-decimal
`lavfi.r128.I` (0.01 LU histogram resolution), never widening the tolerance.

The first high-precision retry did not produce a report: it exited 3 at the ffmpeg
probe with `ffmpeg ... does not run`, although the same absolute path answered
`-version` with 9.0.2 by hand. The probe piped native output into
`Select-Object -First 1`; early pipeline teardown left a non-zero `$LASTEXITCODE`,
and the version line was not validated. The probe now captures the full
stdout+stderr, stores the real exit code first, requires the `ffmpeg version` line,
and still fails on any non-zero probe or missing filter. That fix has **not been
re-run**, so there is no `reference-report-metadata.md` and no high-precision
verdict yet.

Never run: `--libebur128` (no library on the host), the native host-compiler mode (no
Windows C++ compiler), arm64-v8a / armeabi-v7a (built only), the high-precision
comparison end-to-end, and any real device. DG-012 therefore stays
`IN_PROGRESS / PROVISIONAL`: the round-15 error table exists, but the comparison as
a whole failed and the reference is too coarse to settle it. See
`tools/lufs_compare/README.md` for the commands.

Error codes (`dg_status`): `DG_OK`, `DG_ERR_INVALID_ARGUMENT`,
`DG_ERR_SAMPLE_RATE_OUT_OF_RANGE`, `DG_ERR_CHANNEL_COUNT_OUT_OF_RANGE`,
`DG_ERR_OUT_OF_MEMORY`, `DG_ERR_NULL_HANDLE`.

## Lifetime contract (deliberately small)

The handle is an opaque pointer that is valid from `dg_create` to the matching
`dg_destroy`. `NULL` is reported as `DG_ERR_NULL_HANDLE`. Using a handle after
`dg_destroy`, or one that never came from `dg_create`, is **undefined behaviour
and is not detected**: there is no registry, no generation tag and no mutex, so
the audio path stays free of locks, reference counts and syscalls. That is a
deliberate trade — a registry can never distinguish "same address, new engine"
(reused-address ABA), and 64-bit generation IDs do not fit a 32-bit
`armeabi-v7a` handle.

Per-handle threading: `dg_render` and `dg_reset` must not overlap (one audio
thread, which is what the Kotlin engine does); setters store atomics and may run
concurrently with rendering, taking effect at the next block; `dg_destroy` must
be externally synchronized with every other call on that handle. The full text is
at the top of `include/dreamguard/dsp_core.h`.

## Building and testing

Directly with CMake (any host with a C++20 compiler):

```sh
cmake -S dsp-core -B dsp-core/build/native \
      -DCMAKE_BUILD_TYPE=Debug -DDREAMGUARD_DSP_BUILD_TESTS=ON
cmake --build dsp-core/build/native --config Debug
ctest --test-dir dsp-core/build/native --output-on-failure -C Debug
```

Through Gradle (same commands, one entry point next to the Kotlin tests):

```sh
./gradlew :dsp-core:dspCoreTest     # configure + build + ctest
./gradlew :dsp-core:dspCoreConfigure
./gradlew :dsp-core:dspCoreBuild
```

`cmake` and `ctest` must be on `PATH`; override them with
`-PdspCmake=...` / `-PdspCtest=...` when they are not.

Registered CTest tests, with the last measured results (maintainer's machine,
Windows host for the build, NDK 27.0.12077973 cross-compiled x86_64 binaries run
on an Android API 35 emulator because the host has no native C++ compiler):

- `dg_lifecycle` — `tests/test_dg_lifecycle.cpp` — 17 groups, 1328 checks / 0 failures
- `dg_pcm` — `tests/test_pcm.cpp` — 24 groups, 139 checks / 0 failures
- `dg_header_c_compat` — `tests/test_header_c_compat.c` (compiled as C, linked as C++) — passed,
  including the DG-012 meter/LUFS smoke calls (rebuilt and run in round 8). The file also contains a
  DG-022 PCM16 create/process/destroy block; it was not separately compiled or run in the round-23
  report, so treat that block as unverified.
- `dg_meter` — `tests/test_meter.cpp` — 10 groups — **207 checks / 0 failures** on x86_64 (NDK, API 35 emulator)
- `dg_lufs` — `tests/test_lufs.cpp` — 12 groups — **238 checks / 0 failures** on x86_64 after the round-7
  gating fix (the first run's 227 checks / 2 failures were wrong test expectations: a mono sine reads
  10*log10(2) LU below its peak level, which the gating case had omitted)
- `dg_lufs_compare` — `tools/lufs_compare/dg_lufs_compare.cpp --selfcheck` — **115 checks / 0 failures**
  on x86_64 (round 14, NDK, API 35 emulator). Round 13 had failed with 113/1: the level-step case's
  quiet section was generated at −70 dBFS instead of the documented −50 dBFS (a *tool* bug — the
  generator multiplied by the case's −20 dBFS level, so passing 10^(-50/20) as a scale double-counted
  the 20 dB). Fixed to an absolute −50 dBFS plus a new check that the quiet tail's `momentary` equals
  its analytic value; the count going 113 → 115 is what proves the fixed binary ran. It does not need
  ffmpeg, and it is self-consistency evidence, not agreement with a reference meter. Round 15 ran the
  actual reference comparison (pre-generated mode, FFmpeg 9.0.2): **14 PASS / 2 FAIL / 0 SKIP, worst
  0.049 LU, overall FAIL, exit 1** — see the DG-012 status block above for what the two undefined cases
  mean.
- `dg_compressor` — `tests/test_compressor.cpp` — 16 groups — **348 checks / 0 failures** on x86_64
  (round 13, NDK, API 35 emulator; the round-12 334/2 run had two wrong test expectations, rewritten
  against analytic values). **Re-run in round 23 after the output descriptor type changed to
  `dg_pcm_buffer`: 348 / 0 again.** Signal-level acceptance only — no listening test.
- `dg_pcm16_processor` — `tests/test_pcm16_processor.cpp` — **27 groups** — **206 checks / 0 failures** on
  x86_64 (NDK, API 35 emulator, round 23). It covers byte-exact bypass including a partial frame, bypass
  leaving the compressor envelope untouched, every int16 value at unity, -6.02 dB makeup, saturation, the
  static curve, reset, stereo link/unlink and a stereo partial frame, chunk invariance, a larger output
  capacity, argument/aliasing rejection, and zero latency. This is still **not** device playback evidence:
  the APK was not installed or run, so no real audio passed through the bridge.

The lifecycle and C-header results predate the last PCM change and were not re-run
after it. In round 23 the `dg_pcm16_processor` suite (206/0) and `dg_compressor`
(348/0) ran on x86_64, while the Debug build compiled and linked all three ABIs;
arm64-v8a and armeabi-v7a have never *run* the tests, no test was run on a device,
and no run has been compared against a reference loudness meter (no ffmpeg/
ffprobe was available). There is no ASan/LSan job.

## What the tests do and do not prove

They assert: the `dg_create` validation matrix (both boundary values, validation
order, failing create clears its out-parameter), that alive handles are distinct
and independent, that every `NULL` entry point returns `DG_ERR_NULL_HANDLE`,
`dg_render` argument validation and output shape, that `dg_reset` makes the next
block bit-identical to a fresh engine's first block, that setters clamp and
survive NaN/Infinity, that setters racing a render thread stay clean, that four
engines created in parallel produce byte-identical first blocks (no shared
state), and that 200 create/render/reset/destroy cycles stay deterministic. For
PCM: bit-exact bypass including NaN payloads and signed zeros, mono/stereo
mapping, silence detection, rejection of out-of-contract gains and shapes, the
aliasing rules above (identical pointer accepted, partial overlap and in-place
channel conversion rejected with the buffer unchanged), and a 65 536-frame
stereo round trip. For the meter: the create validation matrix, NULL handles,
DC and sine known-signal levels (analytic RMS `0.353553` for a 0.5-peak sine),
per-channel independence, window fill/roll-off, clip and non-finite counters,
`reset`/`reset_peak_hold` semantics, and **byte-identical results for any chunking
of the same stream**. For LUFS: the 48 kHz coefficient table against BS.1770-4
(1e-6), coefficient stability at 8k–192k, the 1 kHz calibration within 0.1 LU,
mono versus stereo (exactly 10·log10(2)), window availability, silence,
gating both ways round (a −50 dBFS section must be gated out; a −9 dB section must
be kept, because the relative gate is the absolute-gated mean minus 10 LU), NaN
handling, and chunking invariance. For the compressor (once it has been run):
the create/parameter validation matrix and its boundaries, the static curve in
closed form (hard knee, the quadratic soft knee, continuity at the knee edges,
ratio 1), steady-state gain reduction on a constant-magnitude signal, the
attack/release one-pole law at whole time constants, channel linking versus
independence, reset, chunking invariance, the per-sample gain-step bound that
rules out clicks, and the clip/non-finite counters.

They do **not** use or test undefined behaviour: no double destroy, no stale or
foreign handles, no destroy racing a render. They are also **not** a leak
detector and **not** a race detector — there is no AddressSanitizer/
LeakSanitizer job here, and the concurrency cases are smoke tests. Do not read a
green CTest run as "no resource leak verified". And for DG-012 specifically: a
green run would prove internal consistency and the published calibration point,
**not** agreement with a reference loudness meter, true-peak accuracy or any
particular streaming platform's normalisation. For DG-013 specifically: a green
run would prove the curve, the ballistics law and click-freedom **as defined
here**, not that the compressor sounds good, matches a commercial unit, or handles
true peaks above 0 dBFS (it does not; that is DG-014).
