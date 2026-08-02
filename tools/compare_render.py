#!/usr/bin/env python3
"""Compare two raw interleaved float32 stereo renders.

The A/B harness for every render-level gate. Reports the maximum absolute difference and where it
falls, which is what a bit-exactness claim rests on, and then -- only if that is non-zero --
correlation and level, which is how the upstream project judges a residual it cannot eliminate.

A bad-looking number is a lead, not a verdict. See the verification notes: one passage there sits at
0.72 envelope correlation with a spectrum matching the DLL within 0.5 dB, and nothing is wrong
with it.

Usage:
    python3 tools/compare_render.py <a.f32> <b.f32> [--tolerance 0.0]
"""

import argparse
import math
import pathlib
import struct
import sys


def read_f32(path):
    raw = path.read_bytes()
    if len(raw) % 4 != 0:
        sys.exit(f"{path} is {len(raw)} bytes, not a whole number of float32 samples.")
    return list(struct.unpack("<%df" % (len(raw) // 4), raw))


def correlation(a, b):
    n = min(len(a), len(b))
    if n == 0:
        return float("nan")
    mean_a = sum(a[:n]) / n
    mean_b = sum(b[:n]) / n
    num = sum((a[i] - mean_a) * (b[i] - mean_b) for i in range(n))
    den_a = math.sqrt(sum((a[i] - mean_a) ** 2 for i in range(n)))
    den_b = math.sqrt(sum((b[i] - mean_b) ** 2 for i in range(n)))
    return num / (den_a * den_b) if den_a > 0 and den_b > 0 else float("nan")


def rms(values):
    return math.sqrt(sum(v * v for v in values) / len(values)) if values else 0.0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("a", type=pathlib.Path)
    parser.add_argument("b", type=pathlib.Path)
    parser.add_argument(
        "--tolerance",
        type=float,
        default=0.0,
        help="maximum absolute difference to accept; the default demands exactness",
    )
    arguments = parser.parse_args()

    a = read_f32(arguments.a)
    b = read_f32(arguments.b)

    print(f"{arguments.a.name:28} {len(a) // 2:9,} frames")
    print(f"{arguments.b.name:28} {len(b) // 2:9,} frames")

    if len(a) != len(b):
        print(f"\nLENGTH MISMATCH: {len(a)} against {len(b)} samples")
        return 1

    if not a:
        print("\nBoth files are empty -- this comparison proves nothing.")
        return 1

    worst = 0.0
    worst_at = -1
    differing = 0
    for i in range(len(a)):
        difference = abs(a[i] - b[i])
        if difference > 0:
            differing += 1
        if difference > worst:
            worst = difference
            worst_at = i

    peak = max(max(abs(v) for v in a), max(abs(v) for v in b))
    print(f"\npeak amplitude              {peak:.9f}")
    print(f"samples differing           {differing:,} of {len(a):,}")
    print(f"max abs difference          {worst:.9e}")

    if worst_at >= 0:
        print(f"  first worst at sample     {worst_at} (frame {worst_at // 2}, "
              f"{'L' if worst_at % 2 == 0 else 'R'})")
        print(f"    a = {a[worst_at]:+.9f}")
        print(f"    b = {b[worst_at]:+.9f}")

    if worst == 0.0:
        print("\nIDENTICAL: every sample matches bit for bit.")
        return 0

    # Only worth computing once there is a residual to judge.
    print(f"\ncorrelation                 {correlation(a, b):.9f}")
    ra, rb = rms(a), rms(b)
    print(f"rms a / rms b               {ra:.9f} / {rb:.9f}")
    if rb > 0:
        print(f"level difference            {20 * math.log10(ra / rb):+.4f} dB")
    if peak > 0:
        print(f"max difference vs peak      {worst / peak:.3e}")

    if worst <= arguments.tolerance:
        print(f"\nWITHIN TOLERANCE ({arguments.tolerance:g}).")
        return 0

    print(f"\nDIFFERS beyond tolerance ({arguments.tolerance:g}).")
    return 1


if __name__ == "__main__":
    sys.exit(main())
