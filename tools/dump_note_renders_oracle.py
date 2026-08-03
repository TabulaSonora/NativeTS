#!/usr/bin/env python3
"""Record what the REFERENCE DLL renders for a spread of single notes.

This replaces `dump_note_renders.py`, which recorded what the archived C# port rendered. That was a
sound oracle while the C# engine was the reference; it is not one now. It predates several fixes
this engine has since made -- the unaligned-loop decode and the pitch ramp among them -- so its
digests encode behaviour both engines have moved past, and a case where the two disagree says
nothing about which is right.

**These fixtures are tolerance gates, not digest gates, and that is not a compromise.** This port is
not bit-exact with the DLL and does not aim to be: the two differ by float rounding, by an
uninitialised ring the reference reads on a recycled voice, and by effect LFO phase. A sha256 over
the oracle's samples could only ever fail. What is recorded instead is what a difference has to
respect -- length, peak, RMS, and per-octave level -- which is exactly the standard
`docs/COMPARING_RENDERS.md` argues for and tight enough that a real regression cannot hide under it.

The audio itself is written alongside as raw interleaved float32, so a failure can be listened to
and measured rather than only counted. It is Roland-derived, so like the tables it is generated
locally and gitignored.

Usage:
    python3 tools/dump_note_renders_oracle.py <SCCore.dll> fixtures/note_renders_oracle.json \\
        [--scdec <path to scdec.exe>] [--audio-dir fixtures/oracle/notes] [--tail 1.8]

Needs the harness from the sibling specv2 checkout, and wine on a non-Windows host:
    (in specv2/tools/decoder) dotnet publish -c Release -r win-x64 --self-contained
"""

import argparse
import hashlib
import json
import math
import pathlib
import shutil
import struct
import subprocess
import sys

# The sweep. Programs across the GM map so every family is represented, each on a few keys and
# velocities, and every tone map -- the map is what selects the tone table, so a case that is right
# on one can be wrong on another.
PROGRAMS = [0, 1, 4, 6, 11, 16, 19, 24, 26, 30, 33, 35, 38, 40, 42, 46, 48, 52,
            56, 57, 61, 64, 66, 71, 73, 75, 78, 80, 87, 91, 95, 99, 104, 114, 118, 122]
KEYS = [36, 48, 60, 72, 84]
VELOCITIES = [40, 100, 127]
MAPS = [1, 2, 3, 4]
HOLD_SECONDS = 1.0


def build_cases():
    """A deterministic spread; the same list every run so fixtures diff cleanly."""
    cases = []
    for i, program in enumerate(PROGRAMS):
        for j, key in enumerate(KEYS):
            velocity = VELOCITIES[(i + j) % len(VELOCITIES)]
            cases.append({
                "program": program,
                "note": key,
                "velocity": velocity,
                "hold": HOLD_SECONDS,
                "map": MAPS[(i + j) % len(MAPS)],
            })
    return cases


def octave_bands(mono, rate):
    """Per-octave level in dB, the measure a timbre difference actually shows up in."""
    size = 1
    while size < min(len(mono), 1 << 16):
        size <<= 1
    if size < 256:
        return []

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
            imag[i], imag[j] = imag[j], imag[i]
    length = 2
    while length <= size:
        angle = -2 * math.pi / length
        wr, wi = math.cos(angle), math.sin(angle)
        for i in range(0, size, length):
            cr, ci = 1.0, 0.0
            for k in range(length // 2):
                ur, ui = real[i + k], imag[i + k]
                vr = real[i + k + length // 2] * cr - imag[i + k + length // 2] * ci
                vi = real[i + k + length // 2] * ci + imag[i + k + length // 2] * cr
                real[i + k], imag[i + k] = ur + vr, ui + vi
                real[i + k + length // 2], imag[i + k + length // 2] = ur - vr, ui - vi
                cr, ci = cr * wr - ci * wi, cr * wi + ci * wr
        length <<= 1

    power = [real[k] * real[k] + imag[k] * imag[k] for k in range(size // 2 + 1)]
    bands = []
    for centre in (125, 250, 500, 1000, 2000, 4000, 8000):
        low = centre / math.sqrt(2)
        high = centre * math.sqrt(2)
        total = sum(power[k] for k in range(len(power))
                    if low <= k * rate / size < high)
        bands.append(round(10 * math.log10(total) if total > 0 else -999.0, 3))
    return bands


def measure(path, rate):
    raw = path.read_bytes()
    frames = len(raw) // 8
    samples = struct.unpack(f"<{frames * 2}f", raw[:frames * 8])
    left = samples[0::2]
    right = samples[1::2]

    peak = max((abs(v) for v in samples), default=0.0)
    energy = sum(v * v for v in samples)
    rms = math.sqrt(energy / len(samples)) if samples else 0.0
    mono = [(left[i] + right[i]) * 0.5 for i in range(frames)]

    return {
        "frames": frames,
        "peak": peak,
        "rms": rms,
        "sha256": hashlib.sha256(raw).hexdigest(),
        "first8": [round(v, 9) for v in samples[:8]],
        "bands": octave_bands(mono, rate),
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("dll", type=pathlib.Path, help="path to SCCore.dll")
    parser.add_argument("output", type=pathlib.Path, help="where to write the fixture")
    parser.add_argument("--scdec", type=pathlib.Path,
                        default=pathlib.Path("../specv2/tools/decoder/bin/Release/net10.0/"
                                             "win-x64/publish/scdec.exe"))
    parser.add_argument("--audio-dir", type=pathlib.Path,
                        default=pathlib.Path("fixtures/oracle/notes"))
    parser.add_argument("--tail", type=float, default=1.8,
                        help="seconds rendered past the note-off; must match the test's")
    parser.add_argument("--rate", type=int, default=32000)
    arguments = parser.parse_args()

    if not arguments.scdec.exists():
        sys.exit(f"harness not found at {arguments.scdec}\n"
                 "  build it: (in specv2/tools/decoder) "
                 "dotnet publish -c Release -r win-x64 --self-contained")

    cases = build_cases()
    case_file = arguments.audio_dir.parent / "notes_cases.txt"
    arguments.audio_dir.mkdir(parents=True, exist_ok=True)
    case_file.write_text("".join(
        f"{c['program']} {c['note']} {c['velocity']} {c['hold']} {c['map']}\n" for c in cases))

    # One process for the whole sweep: wine start-up dominates otherwise.
    runner = [] if sys.platform == "win32" else ["wine"]
    command = runner + [str(arguments.scdec), str(arguments.dll), "notebatch",
                        str(case_file), str(arguments.audio_dir), str(arguments.tail)]
    print(f"rendering {len(cases)} notes through the oracle ...", flush=True)
    result = subprocess.run(command, capture_output=True, text=True,
                            env={"WINEDEBUG": "-all", "PATH": "/usr/bin:/bin"})
    if result.returncode != 0:
        sys.exit(f"harness failed ({result.returncode}):\n{result.stderr[-2000:]}")

    for index, case in enumerate(cases):
        audio = arguments.audio_dir / f"case{index:04d}.f32"
        if not audio.exists():
            sys.exit(f"harness did not produce {audio}")
        case.update(measure(audio, arguments.rate))
        case["audio"] = str(audio.relative_to(arguments.output.parent)) \
            if arguments.output.parent in audio.parents else str(audio)

    document = {
        "_note": ("Single-note renders from the REFERENCE DLL, not from any reimplementation. "
                  "Roland-derived: generate locally, do not redistribute. These are tolerance "
                  "gates -- this port is not bit-exact with the DLL and does not aim to be, so the "
                  "sha256 identifies the oracle audio rather than anything the port must match. "
                  "Compare frames exactly, and peak, rms and the octave bands within tolerance."),
        "generator": "tools/dump_note_renders_oracle.py",
        "dllSha256": hashlib.sha256(arguments.dll.read_bytes()).hexdigest(),
        "sampleRate": arguments.rate,
        "tailSeconds": arguments.tail,
        "cases": cases,
    }
    arguments.output.parent.mkdir(parents=True, exist_ok=True)
    arguments.output.write_text(json.dumps(document, indent=1))

    sounding = sum(1 for c in cases if c["peak"] > 0.0)
    print(f"wrote {len(cases)} cases ({sounding} sounding) to {arguments.output}")
    print(f"oracle audio in {arguments.audio_dir} "
          f"({sum(c['frames'] for c in cases):,} frames total)")


if __name__ == "__main__":
    main()
