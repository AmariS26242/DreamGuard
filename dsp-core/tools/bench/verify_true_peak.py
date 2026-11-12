"""Measure exported limiter fixtures with an independent FFmpeg 16x resampler.

No third-party Python packages. Inputs are the f32 files exported by
DG_TRUE_PEAK_EXPORT=<directory> dg_limiter_tests, including zero-padded tails.
"""
import argparse
import array
import csv
import hashlib
import math
from pathlib import Path
import re
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ffmpeg", required=True, type=Path)
    parser.add_argument("--fixtures", required=True, type=Path)
    parser.add_argument("--report", required=True, type=Path)
    args = parser.parse_args()
    files = sorted(args.fixtures.glob("tp_*.f32"))
    if len(files) != 120:
        parser.error(f"expected 120 fixtures, found {len(files)}")
    version = subprocess.run([str(args.ffmpeg), "-version"], check=True,
                             capture_output=True, text=True).stdout.splitlines()[0]
    rows = []
    for file in files:
        match = re.fullmatch(r"tp_(\d+)_([12])_([01])_([0-5])", file.stem)
        if match is None:
            parser.error(f"unexpected fixture name: {file}")
        rate, channels, linked, shape = map(int, match.groups())
        ceiling = -60.0 if shape == 5 else -2.0
        result = subprocess.run([
            str(args.ffmpeg), "-nostdin", "-v", "error", "-f", "f32le",
            "-ar", str(rate), "-ac", str(channels), "-i", str(file),
            "-af", f"apad=pad_len=256,aresample={rate * 16}:filter_size=128:phase_shift=10:cutoff=0.99",
            "-f", "f64le", "-acodec", "pcm_f64le", "pipe:1",
        ], check=True, capture_output=True)
        output = array.array("d")
        output.frombytes(result.stdout)
        if sys.byteorder != "little":
            output.byteswap()
        if not output or not all(math.isfinite(x) for x in output):
            raise RuntimeError(f"empty/nonfinite reference output: {file}")
        peak = max(abs(x) for x in output)
        dbfs = 20 * math.log10(max(peak, 1e-12))
        rows.append(dict(fixture=file.name, rate=rate, channels=channels, linked=linked,
                         shape=shape, ceiling_dbfs=ceiling, reference_peak_dbfs=dbfs,
                         margin_db=ceiling-dbfs, passed=dbfs <= ceiling+0.05,
                         sha256=hashlib.sha256(file.read_bytes()).hexdigest()))
    args.report.parent.mkdir(parents=True, exist_ok=True)
    with args.report.open("w", newline="", encoding="utf-8") as report:
        writer = csv.DictWriter(report, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)
    args.report.with_suffix(".txt").write_text(
        version + "\n256-frame zero tail, 16x libswresample, filter_size=128, phase_shift=10, cutoff=0.99\n"
        "Acceptance: independent reconstruction <= configured ceiling + 0.05 dB.\n"
        "This finite fixture comparison is not a broadcast certification.\n", encoding="utf-8")
    failures = sum(not row["passed"] for row in rows)
    print(f"{len(rows)-failures} PASS / {failures} FAIL; "
          f"minimum headroom {min(row['margin_db'] for row in rows):.6f} dB")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
