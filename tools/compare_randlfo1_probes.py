#!/usr/bin/env python3
"""Measure the shared-LFO1 probes against the reference DLL.

`partial_alloc_node @ 1800029e0` claims **one** LFO1 node per note and refcounts it by partial
count, where LFO2 gets one node per partial. That is only observable when LFO1 is one of the three
random shapes, since only then does it draw from the shared generator on a phase wrap -- one draw
for the note rather than one per partial.

**Nothing else in the suite can see it.** Twelve tones have a random LFO1 and eight of those have
two partials, and not one is reachable from any of the note sweep's 185 melodic cases. The song
gate cannot serve either: its measures are built for songs, and on a 3.6-second one-note file driven
by a random process its balance and envelope windows swing for reasons that have nothing to do with
the LFO. So this is a tool rather than a gate -- run it when you touch the LFO nodes, the generator,
or the order voices draw from it.

What it compares is the whole render's octave bands, its level and its peak. On a noise-driven tone
the bands are the statistics of the random process and the peak is a single sample of it, so the
bands are the measure with something to say and the peak is the one most able to move on luck.
Expect the bands to answer and the peak to wander.

The probes are `testdata/lfo/randlfo1_*.mid`, six one-note files that live in the spec repository
(they are ours, not Roland's) and are mirrored into `testdata/` here. Reach `Stream` and `Bubble` at
bank 4 and bank 5 of program 122, which every map defines including the SC-55's.

Usage:
    python3 tools/compare_randlfo1_probes.py <SCCore.dll> \\
        [--scdec <path to scdec.exe>] [--runner "<launcher>"] [--work <dir>] [--keep]

Roland-derived audio: rendered locally into a temporary directory, never committed.
"""

import argparse
import math
import os
import pathlib
import shlex
import shutil
import struct
import subprocess
import sys
import tempfile
import wave

BANDS = (63, 125, 250, 500, 1000, 2000, 4000, 8000)
TAIL = 1.8
PROBES = [("Stream", "stream", key) for key in (48, 60, 72)] + \
         [("Bubble", "bubble", key) for key in (48, 60, 72)]
MAPS = (1, 4)


def read_wav(path):
    with wave.open(str(path), "rb") as handle:
        frames, channels, rate = handle.getnframes(), handle.getnchannels(), handle.getframerate()
        raw = handle.readframes(frames)
    values = struct.unpack(f"<{len(raw) // 2}h", raw)
    left = [v / 32768.0 for v in values[0::channels]]
    right = [v / 32768.0 for v in values[1::channels]] if channels > 1 else left
    return [(left[i] + right[i]) * 0.5 for i in range(min(len(left), len(right)))], rate


def power_spectrum(mono):
    size = 1
    while size < min(len(mono), 1 << 16):
        size <<= 1
    if size < 256 or size > len(mono):
        return [], 0
    window = [0.5 - 0.5 * math.cos(2 * math.pi * i / size) for i in range(size)]
    real = [mono[i] * window[i] for i in range(size)]
    imag = [0.0] * size
    j = 0
    for i in range(1, size):
        bit = size >> 1
        while j & bit:
            j ^= bit
            bit >>= 1
        j |= bit
        if i < j:
            real[i], real[j] = real[j], real[i]
    length = 2
    while length <= size:
        angle = -2 * math.pi / length
        wr, wi = math.cos(angle), math.sin(angle)
        for i in range(0, size, length):
            cr, ci = 1.0, 0.0
            for k in range(length // 2):
                a, b = i + k, i + k + length // 2
                tr = real[b] * cr - imag[b] * ci
                ti = real[b] * ci + imag[b] * cr
                real[b], imag[b] = real[a] - tr, imag[a] - ti
                real[a], imag[a] = real[a] + tr, imag[a] + ti
                cr, ci = cr * wr - ci * wi, cr * wi + ci * wr
        length <<= 1
    return [real[k] * real[k] + imag[k] * imag[k] for k in range(size // 2 + 1)], size


def measure(path):
    """Peak, RMS and the octave bands, over the **whole** render rather than a window into it."""
    mono, rate = read_wav(path)
    peak = max(abs(s) for s in mono)
    rms = math.sqrt(sum(s * s for s in mono) / len(mono))
    power, size = power_spectrum(mono)
    bands = []
    for centre in BANDS:
        low, high = centre / math.sqrt(2), centre * math.sqrt(2)
        klow = max(1, int(low * size / rate))
        khigh = min(len(power) - 1, int(high * size / rate))
        bands.append(10 * math.log10(sum(power[klow:khigh + 1]) + 1e-30))
    return peak, rms, bands


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("dll", type=pathlib.Path)
    parser.add_argument("--scdec", type=pathlib.Path,
                        default=pathlib.Path("../specv2/tools/decoder/bin/Release/net10.0/"
                                             "win-x64/publish/scdec.exe"))
    parser.add_argument("--runner", default=None,
                        help="launcher for the harness, e.g. a CrossOver cxstart invocation")
    parser.add_argument("--testdata", type=pathlib.Path, default=pathlib.Path("testdata/lfo"))
    parser.add_argument("--cli", type=pathlib.Path,
                        default=pathlib.Path("build/mac-release/apps/cli/tabula-sonora"))
    parser.add_argument("--work", type=pathlib.Path, default=None)
    parser.add_argument("--keep", action="store_true", help="leave the rendered audio behind")
    arguments = parser.parse_args()

    runner = shlex.split(arguments.runner) if arguments.runner else \
        ([] if sys.platform == "win32" else ["wine"])
    work = arguments.work or pathlib.Path(tempfile.mkdtemp(prefix="randlfo1-"))
    work.mkdir(parents=True, exist_ok=True)

    # The harness picks up the host's PATH under wine and gets noisy; a custom runner needs the
    # environment intact, since CrossOver resolves its bottle through HOME.
    if sys.platform == "win32":
        environment = None
    elif arguments.runner:
        environment = {**os.environ, "WINEDEBUG": "-all"}
    else:
        environment = {"WINEDEBUG": "-all", "PATH": "/usr/bin:/bin"}

    rows = []
    for label, stem, key in PROBES:
        midi = arguments.testdata / f"randlfo1_{stem}_k{key}.mid"
        if not midi.exists():
            print(f"  skipping {midi}: not present")
            continue
        for tone_map in MAPS:
            oracle = work / f"oracle_{stem}_k{key}_m{tone_map}.wav"
            ours = work / f"ours_{stem}_k{key}_m{tone_map}.wav"

            result = subprocess.run(
                runner + [str(arguments.scdec), str(arguments.dll), "smf",
                          str(midi.resolve()), str(oracle.resolve()), str(tone_map), str(TAIL)],
                capture_output=True, text=True, env=environment)
            if result.returncode != 0 or not oracle.exists():
                print(f"  ! oracle failed on {midi} map {tone_map} (exit {result.returncode})")
                continue

            # One port and the default 64 voices, which is the tier comparable to the DLL at all.
            subprocess.run([str(arguments.cli), "render", str(midi), str(ours),
                            "--map", str(tone_map), "--ports", "1", "--tail", str(TAIL),
                            "--dll", str(arguments.dll)], check=True, capture_output=True)

            peak_a, rms_a, bands_a = measure(ours)
            peak_b, rms_b, bands_b = measure(oracle)
            gaps = [abs(bands_a[i] - bands_b[i]) for i in range(len(BANDS))]
            rows.append(dict(label=label, key=key, map=tone_map,
                             peak=abs(peak_a - peak_b),
                             rms=abs(20 * math.log10((rms_a + 1e-30) / (rms_b + 1e-30))),
                             worst=max(gaps), mean=sum(gaps) / len(gaps)))

    if not rows:
        print("nothing measured")
        return

    print(f"\n{'tone':8s} {'key':>4s} {'map':>4s} {'peak':>9s} {'rms dB':>8s} "
          f"{'worst band':>11s} {'mean band':>10s}")
    for row in rows:
        print(f"{row['label']:8s} {row['key']:4d} {row['map']:4d} {row['peak']:9.6f} "
              f"{row['rms']:8.4f} {row['worst']:11.4f} {row['mean']:10.4f}")
    n = len(rows)
    print(f"{'MEAN':8s} {'':4s} {'':4s} {sum(r['peak'] for r in rows) / n:9.6f} "
          f"{sum(r['rms'] for r in rows) / n:8.4f} "
          f"{sum(r['worst'] for r in rows) / n:11.4f} "
          f"{sum(r['mean'] for r in rows) / n:10.4f}")
    print("\nFor reference, the reading that landed the shared node (mean of 12):")
    print("  per-partial LFO1   peak 0.006638   rms 0.1436   worst band 11.4770   mean band 2.4161")
    print("  shared LFO1        peak 0.007284   rms 0.1145   worst band 11.0379   mean band 2.2352")

    if arguments.keep or arguments.work:
        print(f"\naudio left in {work}")
    else:
        shutil.rmtree(work, ignore_errors=True)


main()
