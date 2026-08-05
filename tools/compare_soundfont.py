#!/usr/bin/env python3
"""A/B one note between the engine and an exported SoundFont.

Renders the same note twice -- once through `tabula-sonora render-note`, once through
spessasynth playing the exported bank -- and reports how far apart they are.

READ THIS BEFORE READING A NUMBER FROM IT.

**A difference here is not an exporter bug, and a small difference is not success.** The two sides
do not share a synthesiser, and at least six things differ between them that have nothing to do
with how well the envelopes were fitted:

  * The reader applies the EMU attenuation quirk -- initialAttenuation times 0.4 -- to every voice.
  * Its interpolator is not this engine's 4-tap kernel.
  * Its LFOs are triangles; the engine's are wavetables, three of them random.
  * Its envelope shapes are SF2's, which is the whole subject of the export and not a defect.
  * The engine's render carries its send effects; the bank has none.
  * Neither side is aligned to the other in time, and this script does not try to align them.

So the honest use is **relative**: change one thing in the exporter, re-run, and see whether the
number moves the right way. The absolute value says almost nothing. Two renders that sound alike
can score badly here, and the reverse is likelier still.

The metrics are chosen to be somewhat robust to the above. Per-band energy over the held portion
compares timbre without caring about phase or a sample of latency; the envelope correlation compares
shape without caring about level. Sample-level difference is reported because it is cheap, not
because it means much.

Usage:
    python3 tools/compare_soundfont.py --dll SCCore.dll --bank out.sf2 \\
        --spessasynth ~/Source/Repos/spessasynth_core_c/spessasynth_core \\
        --program 0 --note 60 --velocity 100

The SoundFont side needs `tools/sf2_note_render.c` compiled against spessasynth; this script builds
it on demand into the scratch directory and caches it.
"""

import argparse
import math
import pathlib
import struct
import subprocess
import sys
import tempfile

RATE = 32000


def read_stereo_f32(path):
    raw = pathlib.Path(path).read_bytes()
    values = struct.unpack("<%df" % (len(raw) // 4), raw)
    return values[0::2], values[1::2]


def mono(left, right):
    return [(a + b) * 0.5 for a, b in zip(left, right)]


def rms(values):
    return math.sqrt(sum(v * v for v in values) / len(values)) if values else 0.0


def envelope(values, window):
    """Block RMS, which is the shape without the waveform."""
    return [rms(values[i:i + window]) for i in range(0, len(values) - window, window)]


def correlation(a, b):
    n = min(len(a), len(b))
    if n < 2:
        return float("nan")
    mean_a, mean_b = sum(a[:n]) / n, sum(b[:n]) / n
    num = sum((a[i] - mean_a) * (b[i] - mean_b) for i in range(n))
    da = math.sqrt(sum((a[i] - mean_a) ** 2 for i in range(n)))
    db = math.sqrt(sum((b[i] - mean_b) ** 2 for i in range(n)))
    return num / (da * db) if da > 0 and db > 0 else float("nan")


def goertzel_bands(values, rate, edges):
    """Energy per band, via a coarse DFT over a Hann-windowed slice.

    Deliberately crude: this is a timbre comparison, not an analyser. Anything finer would invite
    reading more into the result than the harness can support.
    """
    n = min(len(values), 1 << 14)
    if n < 64:
        return [0.0] * (len(edges) - 1)
    windowed = [values[i] * (0.5 - 0.5 * math.cos(2 * math.pi * i / n)) for i in range(n)]

    bands = [0.0] * (len(edges) - 1)
    for band in range(len(edges) - 1):
        lo, hi = edges[band], edges[band + 1]
        # A handful of probe frequencies per band, geometrically spaced.
        probes = 6
        total = 0.0
        for p in range(probes):
            freq = lo * ((hi / lo) ** (p / max(1, probes - 1)))
            coeff = 2.0 * math.cos(2.0 * math.pi * freq / rate)
            s1 = s2 = 0.0
            for sample in windowed:
                s1, s2 = sample + coeff * s1 - s2, s1
            total += max(0.0, s1 * s1 + s2 * s2 - coeff * s1 * s2)
        bands[band] = total / probes
    return bands


def decibels(a, b):
    if a <= 0 and b <= 0:
        return 0.0
    if a <= 0 or b <= 0:
        return float("inf")
    return 10.0 * math.log10(a / b)


def build_sf2_renderer(spessasynth, scratch):
    source = pathlib.Path(__file__).with_name("sf2_note_render.c")
    binary = scratch / "sf2_note_render"
    if binary.exists() and binary.stat().st_mtime > source.stat().st_mtime:
        return binary
    include = pathlib.Path(spessasynth) / "include"
    lib = pathlib.Path(spessasynth) / "build"
    command = [
        "cc", "-O2", "-I", str(include), str(source),
        "-L", str(lib), "-lspessasynth", f"-Wl,-rpath,{lib}", "-lm", "-o", str(binary),
    ]
    subprocess.run(command, check=True)
    return binary


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--dll", required=True, help="SCCore.dll")
    parser.add_argument("--bank", required=True, help="the exported .sf2")
    parser.add_argument("--spessasynth", required=True,
                        help="path to the spessasynth_core tree (needs include/ and a built build/)")
    parser.add_argument("--cli", default="build/apps/cli/tabula-sonora")
    parser.add_argument("--sflist", help="an .sflist.json to address the bank through")
    parser.add_argument("--program", type=int, default=0)
    parser.add_argument("--note", type=int, default=60)
    parser.add_argument("--velocity", type=int, default=100)
    parser.add_argument("--hold", type=float, default=1.0)
    parser.add_argument("--map", default="sc8820")
    parser.add_argument("--bank-msb", type=int, default=0)
    parser.add_argument("--bank-lsb", type=int, default=0)
    parser.add_argument("--drum", action="store_true")
    parser.add_argument("--keep", help="directory to keep the two renders in")
    args = parser.parse_args()

    scratch = pathlib.Path(args.keep) if args.keep else pathlib.Path(tempfile.mkdtemp())
    scratch.mkdir(parents=True, exist_ok=True)

    engine_path = scratch / "engine.f32"
    bank_path = scratch / "bank.f32"

    subprocess.run([args.cli, "render-note", str(args.program), str(args.note),
                    str(args.velocity), str(args.hold), str(engine_path),
                    args.map, "--dll", args.dll], check=True)

    renderer = build_sf2_renderer(args.spessasynth, scratch)
    command = [str(renderer), args.bank, str(bank_path),
               "--program", str(args.program), "--note", str(args.note),
               "--velocity", str(args.velocity), "--hold", str(args.hold),
               "--bank-msb", str(args.bank_msb), "--bank-lsb", str(args.bank_lsb),
               "--rate", str(RATE)]
    if args.sflist:
        command += ["--sflist", args.sflist, "--base", str(pathlib.Path(args.bank).parent)]
    if args.drum:
        command.append("--drum")
    subprocess.run(command, check=True)

    engine = mono(*read_stereo_f32(engine_path))
    bank = mono(*read_stereo_f32(bank_path))
    n = min(len(engine), len(bank))
    engine, bank = engine[:n], bank[:n]

    if rms(engine) == 0.0 or rms(bank) == 0.0:
        print("one side is silent -- engine rms %.6f, bank rms %.6f" % (rms(engine), rms(bank)))
        print("that is a resolution failure, not a fidelity result; check the preset line above.")
        return 1

    print()
    print("level")
    print("  engine rms %.6f   bank rms %.6f   difference %+.2f dB"
          % (rms(engine), rms(bank), 20 * math.log10(rms(bank) / rms(engine))))

    # Normalise before comparing shape and timbre: an overall level error is a separate finding
    # from a wrong envelope, and mixing them makes both unreadable.
    scale = rms(engine) / rms(bank)
    scaled = [v * scale for v in bank]

    window = RATE // 100  # the engine's own control tick
    env_engine = envelope(engine, window)
    env_bank = envelope(scaled, window)
    print()
    print("envelope (block rms at the 100 Hz control tick, level removed)")
    print("  correlation %.4f over %d blocks" % (correlation(env_engine, env_bank),
                                                 min(len(env_engine), len(env_bank))))
    peak_engine = env_engine.index(max(env_engine)) if env_engine else 0
    peak_bank = env_bank.index(max(env_bank)) if env_bank else 0
    print("  peak block: engine %d, bank %d (%+d ticks)"
          % (peak_engine, peak_bank, peak_bank - peak_engine))

    edges = [80, 250, 800, 2500, 6000, 14000]
    hold_samples = min(len(engine), int(args.hold * RATE))
    bands_engine = goertzel_bands(engine[:hold_samples], RATE, edges)
    bands_bank = goertzel_bands(scaled[:hold_samples], RATE, edges)
    print()
    print("timbre (band energy over the held portion, level removed)")
    for i in range(len(edges) - 1):
        print("  %5d-%5d Hz  %+6.2f dB" % (edges[i], edges[i + 1],
                                           decibels(bands_bank[i], bands_engine[i])))

    print()
    print("sample-level difference (reported because it is cheap, not because it means much)")
    worst = max(abs(a - b) for a, b in zip(engine, scaled))
    print("  worst |diff| %.4f, rms |diff| %.4f"
          % (worst, rms([a - b for a, b in zip(engine, scaled)])))

    print()
    print("Renders kept in %s" % scratch)
    print("Reminder: this measures two different synthesisers. Use it to compare one exporter")
    print("revision against another, not to decide whether the export is good.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
