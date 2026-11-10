# LUFS reference comparison (DG-012)

`dg_lufs` is an in-house implementation of ITU-R BS.1770-4 / EBU R128 loudness.
An implementation agreeing with itself proves nothing, so this directory holds
the machinery that measures **byte-identical PCM** with an independent meter and
reports the deviations.

Two moving parts:

| file | role |
| --- | --- |
| `dg_lufs_compare.cpp` | synthesises a fixed set of PCM signals, measures them with `dg_lufs`, optionally writes 32-bit float WAVs and a CSV of our readings |
| `compare_with_ffmpeg.ps1` | runs FFmpeg's `ebur128` filter over those WAVs, pairs the two readings, writes a markdown report, exits non-zero when anything is out of tolerance |

The script accepts the material two ways, and the second one exists because a host
C++ compiler is not always available:

- **native mode** (default): it runs `dg_lufs_compare --wav-dir --csv` itself and
  compares what that produced;
- **pre-generated mode** (`-Csv <ours.csv> [-WavDir <dir>]`): the CSV and the WAVs
  were produced somewhere else - for example by the Android binary on an emulator,
  then `adb pull`ed - and are read **in place**. This mode never writes to, and never
  deletes, the inputs.

Either way the reference numbers come from running FFmpeg over the actual WAV
bytes. The verdict uses FFmpeg's `lavfi.r128.I` metadata (three printed decimals);
the one-decimal `I:` summary is parsed only as a consistency witness. An FFmpeg
failure, a missing metadata value, or a metadata/summary disagreement is a failure,
never a pass.

The tolerance is **0.1 LU** — the meter accuracy EBU Tech 3341 works to. Nothing
in here ever widens it: a failing case is a failing case.

## Prerequisites

- CMake ≥ 3.22 and a C++20 host compiler — **native mode only**. Pre-generated
  mode needs neither; it needs an `ours.csv` and the matching WAVs.
- FFmpeg with **both** the `ebur128` and `ametadata` filters:

  ```powershell
  ffmpeg -hide_banner -filters | Select-String 'ebur128|ametadata'
  ```

  FFmpeg is an independent implementation of the same standard, which is exactly
  why it is the reference here. **Installing it is the operator's decision.**
  These tools never install packages, never touch filesystem ACLs, and never run
  anything that needs elevation. If FFmpeg is absent, the comparison is simply
  *not executed* and DG-012 stays `IN_PROGRESS / PROVISIONAL`.

  The script looks for FFmpeg at `-Ffmpeg <path>`, then on `PATH`, then in a bundled
  copy under `dsp-core/build/reference-deps/**/ffmpeg*.exe`. A copy that lives inside
  the build tree is used in place and is not a system installation.
- Optional: libebur128 (`ebur128.h` + the library) for the second, in-process
  reference path.

## 1. Build the tool

```powershell
cmake -S dsp-core -B dsp-core/build/host `
      -DDREAMGUARD_DSP_BUILD_TESTS=ON `
      -DDREAMGUARD_DSP_BUILD_TOOLS=ON
cmake --build dsp-core/build/host --config Release
```

`DREAMGUARD_DSP_BUILD_TOOLS` defaults to on for a top-level checkout and off when
`dsp-core` is a subdirectory of the Android build, so the app build is unaffected.

## 2. Self-check (does not need FFmpeg)

```powershell
ctest --test-dir dsp-core/build/host -R dg_lufs_compare --output-on-failure
# or directly:
& dsp-core/build/host/tools/lufs_compare/Release/dg_lufs_compare.exe --selfcheck
```

The self-check asserts properties of the *harness* and the analyser's internal
consistency, and prints `N checks, M failures`:

- whole-buffer and chunked (`1, 13, 997, 4800, 4799, 19200, 7, 65536` frames)
  processing are bit-identical;
- every steady sine reads its analytic K-weighted value within 1e-3 LU;
- a mono stream reads exactly `10*log10(2)` LU below the stereo stream generated
  from the same samples;
- digital silence and a 300 ms stream report `integrated_ready == 0` and `-inf`;
- the level-step signal reproduces the counts `tests/test_lufs.cpp` pins
  (`blocks == 67`, `blocks_after_absolute_gate >= 60`, `blocks_used == 40`), and its
  quiet tail reads the analytic value of a mono −50 dBFS sine through `momentary`
  (this last check exists because the first device run caught the generator
  putting the tail at **−70 dBFS** - 20 dB too low - which left `blocks_used` at 40
  and only showed up as a missing block count; see §4);
- the WAV container header and file size are exact.

**This is not agreement evidence.** Keep that distinction when you write the
status down.

## 3. Compare against FFmpeg

### 3a. Native mode (a host compiler is available)

```powershell
pwsh -File dsp-core/tools/lufs_compare/compare_with_ffmpeg.ps1 `
     -Report dsp-core/build/lufs-compare/reference-report-metadata.md
# explicit paths, wider tolerance is NOT a DG-012 criterion:
pwsh -File dsp-core/tools/lufs_compare/compare_with_ffmpeg.ps1 `
     -Tool dsp-core/build/host/tools/lufs_compare/Release/dg_lufs_compare.exe `
     -Ffmpeg 'C:\ffmpeg\bin\ffmpeg.exe' `
     -Report dsp-core/build/lufs-compare/reference-report-metadata.md
```

It writes into `dsp-core/build/lufs-compare/`: the `-Report` file (for example
`reference-report-metadata.md`; metadata, command lines, per-case
`ours / ffmpeg / delta / PASS|FAIL` table, worst deviation, verdict), its `.csv`
sidecar (the same table, machine readable) and `ours.csv` (our readings, kept as
input evidence). To preserve an existing report, pass
`-Report <new path>`; the report path and its `.csv` sidecar are both derived from
that argument.

### 3b. Pre-generated mode (no host compiler: Android binary + emulator)

The Windows host has no C++ compiler, but the NDK does. Cross-compile the tool, run
it **on the device**, pull both artefacts back, then compare locally — the reference
side still runs on the host, over the very WAV bytes the device measured:

```powershell
# 1. cross-compile (paths: set your own NDK root)
& "$ndk\x86_64-linux-android26-clang++.cmd" -std=c++20 -O2 -fPIE -pie `
    -static-libstdc++ -I dsp-core\include `
    -o dsp-core\build\android-test\dg_lufs_compare `
    dsp-core\tools\lufs_compare\dg_lufs_compare.cpp `
    dsp-core\src\lufs.cpp dsp-core\src\pcm.cpp

# 2. run on the emulator, material lands in /data/local/tmp/lufs-compare
adb push dsp-core\build\android-test\dg_lufs_compare /data/local/tmp/
adb shell "chmod 755 /data/local/tmp/dg_lufs_compare"
adb shell "mkdir -p /data/local/tmp/lufs-compare"
adb shell "/data/local/tmp/dg_lufs_compare --selfcheck"
adb shell "cd /data/local/tmp && /data/local/tmp/dg_lufs_compare --wav-dir lufs-compare --csv lufs-compare/ours.csv"

# 3. pull the CSV and the 16 WAVs (nothing is deleted afterwards)
adb pull /data/local/tmp/lufs-compare dsp-core\build\lufs-compare-android

# 4. compare on the host
pwsh -File dsp-core\tools\lufs_compare\compare_with_ffmpeg.ps1 `
     -Csv dsp-core\build\lufs-compare-android\ours.csv `
     -WavDir dsp-core\build\lufs-compare-android `
     -Report dsp-core\build\lufs-compare-android\reference-report-metadata.md
```

`-WavDir` defaults to the CSV's directory. An existing report is never replaced
silently: choose a new `-Report` path (as above, preserving the round-15 report),
or add `-Force` only when you deliberately mean to overwrite it.
In pre-generated mode the script is read-only with respect to its inputs: it does
not rewrite the CSV, does not move the WAVs, and `-CleanWav` is refused (WAVs are
kept by default in both modes; `-CleanWav` only deletes files the script generated
itself in native mode).

The report records the SHA256 of the CSV and of every WAV, so the evidence says
which bytes were measured by each meter.

### Reference precision and the metadata path

The script runs this filtergraph for every case:

```text
-af "ebur128=metadata=1:peak=true,ametadata=mode=print:file=<temp>.txt"
```

`metadata=1` forces 100 ms framing and makes `ebur128` publish `lavfi.r128.I`,
`lavfi.r128.M`, `lavfi.r128.S`, `lavfi.r128.LRA` and the peak keys on every frame.
`ametadata` writes those keys to a unique temporary file; the script reads the last
`lavfi.r128.I` and deletes the file. The file name is relative because a Windows
absolute path inside a filtergraph would need fragile escaping, so FFmpeg is run
with its working directory set to the temp directory.

Why not just parse `I: ... LUFS`? That summary is printed with `%5.1f`, i.e. one
decimal, so the 0.1 LU tolerance would equal one quantisation step. The metadata
uses `%.3f`. This is **not** 0.001 LU measurement accuracy: FFmpeg accumulates the
integrated value in 0.01 LU histogram bins (`HIST_GRAIN=100`), so the third decimal
is formatting resolution. The summary is still parsed, but only as a cross-check;
a metadata/summary difference above half a summary step is a `FAIL`, so a parsing
mistake cannot silently produce a verdict.

**Execution status:** the caller has executed this metadata path twice, and both
attempts stopped at the FFmpeg probe with exit 3 even though the same absolute
binary answered `-version` with 9.0.2 by hand. The first probe piped native output
into `Select-Object -First 1`; early pipeline teardown left a non-zero
`$LASTEXITCODE`. The replacement probe captured the full stdout+stderr and the real
exit code, but its version regex still used line anchors; on Windows `Out-String`
joins lines with CRLF, and in .NET multiline mode `$` does not match before the
`\r`, so the version line was missed even at exit 0. The version check now searches
the full text for `ffmpeg version` without `^`/`$` anchors, so line endings and
stderr prefixes cannot hide it. The probe still exits 3 on any non-zero exit, a
missing `ffmpeg version` line, or a missing filter. **The corrected version has not been
re-run**, so there is still no high-precision report or CSV. The 14 PASS / 2 FAIL
run below used the older summary-only command; it remains historical evidence and
must not be presented as a high-precision result. Re-run §3 to produce the new
report at a new `-Report` path.

### Verdict rules (both modes)

Exit codes: `0` every comparable case within tolerance (and at least one `PASS`), `1`
something did not pass or nothing was comparable, `2` a required input is missing,
the arguments contradict each other (e.g. `-WavDir` without `-Csv`, `-CleanWav` with
`-Csv`), or the report already exists without `-Force`, `3` FFmpeg is missing or does
not run. Non-zero means the comparison did
**not** pass.

- `PASS` / `FAIL` are numeric only: `|ours − ffmpeg metadata I| ≤ tolerance` decides,
  and the tolerance is not adjustable per case. The metadata and the one-decimal
  summary must also agree within 0.0501 LU; otherwise the case is `FAIL`.
- A numeric `ours` value is not enough to pass a `-70.000` reference. If that
  value is the initialised sentinel and every `lavfi.r128.M` block is finite and
  below the absolute gate, the reference has no measured integrated value, so the
  case is `FAIL` rather than a numeric comparison.
- Cases whose integrated loudness is **undefined by construction** — digital silence
  and a stream shorter than one 400 ms block — are marked `kind = undefined`.
  FFmpeg prints `-70.000` for them because `integrated_loudness` is initialised to
  `ABS_THRES` and the metadata is written unconditionally. The script requires all
  of the following before it records `NO-DATA`:
  1. our side reports no value;
  2. metadata and summary agree;
  3. metadata `I` is the `-70.000` sentinel;
  4. **every** 400 ms `lavfi.r128.M` block is strictly below -70.000 LUFS.
  If any `M` is at or above -70, the sentinel is ambiguous and the case is `FAIL`.
  `NO-DATA` is reported separately with a cause (`silence`, `sub-400ms`, or
  `no-gating-block`) and is **never counted as passing**. It is a definitional
  outcome, not numeric agreement.
- `NO-DATA` alone cannot make the exit code zero: at least one numeric case must
  pass and no case may fail.

Paste the report's summary into `docs/STATUS.md` when it passes; quote it as the
acceptance evidence for DG-012.

## 4. Compare against libebur128 instead (or in addition)

```powershell
cmake -S dsp-core -B dsp-core/build/ebur128 `
      -DDREAMGUARD_DSP_BUILD_TOOLS=ON `
      -DDREAMGUARD_LUFS_COMPARE_EBUR128=ON
cmake --build dsp-core/build/ebur128 --config Release
& dsp-core/build/ebur128/tools/lufs_compare/Release/dg_lufs_compare.exe --libebur128 --tolerance 0.1
```

If CMake cannot find the library, point it with
`-DEBUR128_INCLUDE_DIR=<dir> -DEBUR128_LIBRARY=<file>`; configure fails loudly
rather than building a binary that silently lacks the reference path.

## The signal set

Deterministic by construction: xorshift32 with a fixed seed for the noise,
integer-cycle tone frequencies, no clock, no threads, no fast-math. Same build →
same bytes.

| case | rate | ch | length | what it exercises |
| --- | --- | --- | --- | --- |
| `stereo_1k_-20dB_48k` | 48000 | 2 | 4 s | Tech 3341 calibration point |
| `mono_1k_-20dB_48k` | 48000 | 1 | 4 s | channel sum: must be 3.0103 LU lower |
| `stereo_1k_-23dB_48k` | 48000 | 2 | 4 s | nominal level |
| `stereo_1k_-6dB_48k` | 48000 | 2 | 4 s | hot, below clipping |
| `stereo_1k_-60dB_48k` | 48000 | 2 | 4 s | just above the −70 LUFS absolute gate |
| `stereo_100Hz_-20dB_48k` | 48000 | 2 | 4 s | K-weighting high-pass region |
| `stereo_10kHz_-20dB_48k` | 48000 | 2 | 4 s | K-weighting high-shelf region |
| `stereo_asym_L-6_R-12dB_48k` | 48000 | 2 | 4 s | asymmetric stereo (no analytic value) |
| `stereo_1k_-20dB_44k1` | 44100 | 2 | 4 s | derived coefficients, 44.1 kHz |
| `stereo_1k_-20dB_96k` | 96000 | 2 | 4 s | derived coefficients, 96 kHz |
| `stereo_1k_-20dB_192k` | 192000 | 2 | 4 s | top of the supported range |
| `mono_1k_-20dB_8k` | 8000 | 1 | 4 s | bottom of the supported range |
| `stereo_noise_-20dB_48k` | 48000 | 2 | 5 s | whole K curve at once |
| `mono_levelstep_4s_1s_2s_48k` | 48000 | 1 | 7 s | −20 dBFS, silence, **−50 dBFS absolute** (same construction as `test_lufs.cpp`'s gating test): relative gate |
| `stereo_silence_2s_48k` | 48000 | 2 | 2 s | silence: no integrated value |
| `stereo_1k_-20dB_0p3s_48k` | 48000 | 2 | 0.3 s | shorter than one 400 ms block |

Each case is described the same way in the CSV: sample rate, channel count,
length, signal kind, frequency, peak in dBFS, and whether an integrated value is
defined at all.

## Run history (executed, not assumed)

- 2026-11-10, first run ever: built with the NDK for x86_64 and executed on an
  API 35 emulator by the caller, `--selfcheck` → **113 checks / 1 failure**:
  `mono_levelstep_4s_1s_2s_48k: expected >= 60 blocks above the absolute gate`.
- Root cause: a **signal-generator bug in this tool**, not a defect of `dg_lufs`.
  `fill_sine` multiplies by the case's `peak` (−20 dBFS = 0.1), so passing
  `10^(-50/20)` as a *scale* put the quiet section at −70 dBFS rather than
  −50 dBFS, 20 dB lower than `tests/test_lufs.cpp`'s gating test, which uses
  `std::pow(10, -50/20)` as an **absolute** amplitude. At −70 dBFS the whole tail
  sits below the −70 LUFS absolute gate, so it vanished from
  `blocks_after_absolute_gate` (the run's own `blocks_used == 40` still passed,
  which is what pointed at the tail's level rather than at the gate logic).
- Fix: the quiet section is now generated from an absolute `kLevelStepQuietDbfs`
  (−50 dBFS), and the self-check additionally asserts that the `momentary` value of
  the quiet tail equals the analytic value of a mono −50 dBFS 1 kHz sine, so a
  level error of this kind fails loudly instead of hiding behind a gate count.
- 2026-11-10, same setup after the fix: `--selfcheck` → **115 checks / 0 failures**
  (the two extra checks are exactly the new finite/momentary assertions on the quiet
  tail; the count changing 113 → 115 is what proves the new binary ran). The
  self-check is now green on x86_64 Android.
- 2026-11-10, **first real reference comparison** (pre-generated mode, caller's host,
  FFmpeg 9.0.2 `ebur128=peak=true`, 16 WAVs produced on an API 35 x86_64 emulator):
  **14 PASS / 2 FAIL / 0 SKIP, worst |delta| 0.049 LU, overall `FAIL`, exit code 1.**
  Report: `dsp-core/build/lufs-compare-android/reference-report.md` (and a `.csv`
  sidecar). Per-case deltas: 1 kHz at −20/−23/−6/−60 dBFS → +0.007 / +0.007 / +0.007 /
  +0.007, mono 1 kHz −0.004, 44.1 kHz +0.009, 48 kHz −0.007…+0.007, 96 kHz −0.011,
  192 kHz −0.020, 100 Hz −0.025, asymmetric stereo −0.030, noise −0.023, level step
  +0.030, 10 kHz −0.049.
- The two `FAIL`s are the `kind=undefined` cases (`stereo_silence_2s_48k`,
  `stereo_1k_-20dB_0p3s_48k`): `dg_lufs` reports `integrated_ready = 0` and
  `integrated = -inf`, while FFmpeg prints **`I: -70.0 LUFS`**. The verdict rule
  (`undefined` + one side has a number ⇒ `FAIL`, needs a human decision) fired as
  designed; **both are left as not-passing**, the rule was not relaxed.
- What FFmpeg's −70.0 is (verified in FFmpeg's source, not guessed): `f_ebur128.c`
  has `#define ABS_THRES -70` and `init()` sets
  `ebur128->integrated_loudness = ABS_THRES;`. `filter_frame()` only overwrites that
  field when a 400 ms block has `loudness_400 >= ABS_THRES` **and** `nb_integrated > 0`.
  Digital silence gives `power_400 = 1e-12` → `-120.7 LUFS`, never gated; a 0.3 s
  stream never fills the 400 ms window (`i400.filled` stays 0), so neither case ever
  enters the gate and the field keeps its initial value. `uninit()` then prints it
  with `I: %5.1f LUFS`. So `-70.0` is a **sentinel meaning "no block passed the
  absolute gate"**, not a measurement of the program at −70 LUFS — and it correlates
  with our `-inf` + `ready == 0`. It is *not* a standard citation and *not* a pass.
  Note the sentinel is **ambiguous by construction**: a program that is genuinely
  measured at −70.0 LUFS would be gated out and print the same number, so the string
  cannot distinguish "nothing passed the gate" from "everything sat exactly at the
  gate". The script now adds the `lavfi.r128.M` witness described above; the
  `kind=undefined` classification alone is only a necessary condition.
- **Resolution limit of that run**: the old script used the summary line, which
  prints one decimal (`%5.1f`), so every reference value carried ±0.05 LU of rounding
  and the 0.1 LU tolerance equalled exactly one quantisation step. The 0.049 LU worst
  case was therefore the same order as the rounding itself: it could not prove an
  implementation difference, and it could not prove agreement at 1e-3 LU either.
- 2026-11-10, **high-precision metadata rewrite**: `compare_with_ffmpeg.ps1` now
  judges `lavfi.r128.I` (three printed decimals, 0.01 LU histogram resolution),
  requires metadata/summary agreement, and classifies silence/sub-block material as
  `NO-DATA` only with the all-`M`-below-gate witness. The report and numbers above
  are from the older summary-only script and are not high-precision evidence.
- 2026-11-10, **first high-precision retry failed at the probe**: exit 3,
  `ffmpeg ... does not run`, even though the same absolute ffmpeg path answered
  `-version` successfully with 9.0.2. Root cause: the old probe piped the native
  command into `Select-Object -First 1`; stopping that pipeline early can tear the
  process down before normal exit and leave a non-zero `$LASTEXITCODE`, and the
  version line was not validated. The probe now captures the full stdout+stderr
  stream, stores the real exit code first, extracts `(?im)^\s*(ffmpeg version\b...)`,
  and still exits 3 on a non-zero probe or missing version line. `-filters` exit
  status is checked the same way; the reference check is never skipped. **This fix
  has not been re-run yet**, so there is still no `reference-report-metadata.md`.
- Still never executed: `--libebur128` (no libebur128 on the caller's host), the
  native host-compiler mode (no Windows C++ compiler), arm64/armeabi-v7a (only
  x86_64 emulator), any real-device run, and the new metadata path itself.

## Known, documented biases this comparison is meant to quantify

- The 1 kHz calibration point is expected to read ≈0.024 LU below the reference
  (the filter coefficients are derived rather than copied from the standard's
  published 48 kHz numbers). 0.024 is inside 0.1 LU, so it should pass — but the
  report shows the actual number instead of assuming it.
- Above and below 48 kHz the coefficients are first-order derivations, so
  deviations at 8 kHz / 44.1 kHz / 96 kHz / 192 kHz are expected to be larger
  than at 48 kHz. If any exceed 0.1 LU, the root cause is the coefficient
  derivation and that is where the fix belongs — not in the tolerance.

## What is deliberately out of scope

No true-peak, no LRA, no 3 s short-term integrated variant, no loudness range
histogram — see `docs/STATUS.md` for the DG-012 boundary list. This tool measures
integrated/momentary/short-term loudness only, because that is all `dg_lufs`
claims.
