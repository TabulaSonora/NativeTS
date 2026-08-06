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

**Each case is rendered in its own process, and that is not a performance oversight.** The harness
has a batch mode that renders the whole sweep at once with a GS reset between cases, and it was used
here until the reset turned out not to isolate them: the reference's LFO nodes live in a pool whose
random registers survive being freed, so a tone with a random LFO opens on whatever the previous
case left behind. Measured, three cases in the sweep sat up to 5.8 cents from their own isolated
render -- and two of them were being read as failures of this engine when the engine was right. The
cases are independent, so the processes run concurrently and the cost is mostly recovered.

Usage:
    python3 tools/dump_note_renders_oracle.py <SCCore.dll> fixtures/note_renders_oracle.json \\
        [--scdec <path to scdec.exe>] [--audio-dir fixtures/oracle/notes] [--tail 1.8] \\
        [--jobs 6] [--runner "<launcher>"] [--batch]

Needs the harness from the sibling specv2 checkout, and wine on a non-Windows host:
    (in specv2/tools/decoder) dotnet publish -c Release -r win-x64 --self-contained
"""

import argparse
import concurrent.futures
import hashlib
import json
import math
import os
import pathlib
import shlex
import shutil
import struct
import subprocess
import sys

# The sweep. Programs across the GM map so every family is represented, each on a few keys and
# velocities, and every tone map -- the map is what selects the tone table, so a case that is right
# on one can be wrong on another.
#
# `32` sits at the end rather than in its place. It was added after the rest, to answer a specific
# question -- `roland_sc88_y03` plays `Acoustic Bs.` 324 times inside the 63 Hz band, and it was the
# one thing carrying that file's bass which no case here covered -- and the velocity and map a case
# gets is a rotation over this list's *index*. Inserting it in order would have re-rolled the
# velocity and tone map of every program after it, changing 100 cases to add 5.
PROGRAMS = [0, 1, 4, 6, 11, 16, 19, 24, 26, 30, 33, 35, 38, 40, 42, 46, 48, 52,
            56, 57, 61, 64, 66, 71, 73, 75, 78, 80, 87, 91, 95, 99, 104, 114, 118, 122,
            32]
KEYS = [36, 48, 60, 72, 84]
VELOCITIES = [40, 100, 127]
MAPS = [1, 2, 3, 4]
HOLD_SECONDS = 1.0

# The drum half, on channel 10. A program change there selects a kit rather than a tone, through a
# different pair of lookups, into a different table, with the key supplying a coarse pitch instead
# of transposing the sample -- so none of it is exercised by any number of melodic cases. The nine
# programs are the classic GS kits, and they are the nine that every one of the four maps defines;
# each resolves to a *distinct* kit record on each map, so 36 different kits are covered.
DRUM_CHANNEL = 9
DRUM_PROGRAMS = [0, 8, 16, 24, 25, 32, 40, 48, 56]

# Six keys that sound in all 36 kits, chosen for the mechanisms behind them rather than for the
# names: the bass drum for the low band this sweep could not previously see, the snare and hats for
# noise-dominated tones at opposite ends of the spectrum, the crash for the longest decay in the
# kit, and the low tom because it is the one of the six whose kit coarse pitch is far from neutral.
DRUM_KEYS = [36, 38, 42, 45, 46, 49]

# Shorter than a melodic note because a drum hit is over long before a second is up, and 32 RMS
# windows across 2.1 s resolve a transient about a third better than across 2.8 s. The note-off
# still lands where a sequencer would put it; most kit keys ignore it.
DRUM_HOLD_SECONDS = 0.3


def build_cases():
    """A deterministic spread; the same list every run so fixtures diff cleanly.

    Drums come after the melodic half rather than interleaved with it, so that adding them left
    every melodic case's index -- and so every `caseNNNN.f32` on disk -- where it already was.
    """
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
                "channel": 0,
            })
    for i, program in enumerate(DRUM_PROGRAMS):
        for j, key in enumerate(DRUM_KEYS):
            cases.append({
                "program": program,
                "note": key,
                "velocity": VELOCITIES[(i + j) % len(VELOCITIES)],
                "hold": DRUM_HOLD_SECONDS,
                "map": MAPS[(i + j) % len(MAPS)],
                "channel": DRUM_CHANNEL,
            })
    return cases


def power_spectrum(mono):
    """The windowed power spectrum, computed once and read by everything below it."""
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

    return [real[k] * real[k] + imag[k] * imag[k] for k in range(size // 2 + 1)], size


def octave_bands(power, size, rate):
    """Per-octave level in dB, the measure a timbre difference actually shows up in.

    Eight bands, starting at 63 Hz -- which spans 44.5 to 89.1 Hz, and so is where the sweep's
    lowest key at 65.4 Hz and the bass drum both put their fundamental. It was left out while this
    began at 125 Hz, on the reasoning that most single notes have nothing down there. Most have
    nothing down there; the two that do are the two that matter for the song gate's open low-end
    lead, and a band nobody measures is a band anything can happen in.
    """
    bands = []
    for centre in (63, 125, 250, 500, 1000, 2000, 4000, 8000):
        low = centre / math.sqrt(2)
        high = centre * math.sqrt(2)
        total = sum(power[k] for k in range(len(power))
                    if low <= k * rate / size < high)
        bands.append(round(10 * math.log10(total) if total > 0 else -999.0, 3))
    return bands


def fundamental(power, size, rate, target):
    """The strongest partial within a semitone and a half of where the note should sound, in Hz.

    Nothing else this file records can see a tuning error. Level, spectrum and envelope are all
    blind to a couple of cents -- it is far inside an octave band, moves no RMS and changes no
    envelope -- so an engine can be sharp on every patch and pass every one of them, which is
    exactly what happened. This is the measurement that would have caught it.

    Parabolic interpolation on the log magnitude, which is what makes a 0.5 Hz bin resolve to
    about a twentieth of that. A search window of +-3% is wide enough for any patch's own detune
    and narrow enough that it cannot lock onto a neighbouring harmonic.
    """
    if not power:
        return 0.0
    low = max(1, int(target * 0.97 * size / rate))
    high = min(len(power) - 2, int(target * 1.03 * size / rate))
    if high <= low:
        return 0.0
    peak = max(range(low, high + 1), key=lambda k: power[k])
    if power[peak] <= 0.0:
        return 0.0
    a, b, c = (math.log(power[k] + 1e-30) for k in (peak - 1, peak, peak + 1))
    divisor = a - 2 * b + c
    offset = 0.5 * (a - c) / divisor if divisor != 0 else 0.0
    return round((peak + offset) * rate / size, 4)


def rms_envelope(mono, windows=32):
    """A coarse RMS envelope, which for a single note is the measure that matters most.

    Peak and RMS say how loud the note is and the bands say what colour it is; neither can tell a
    correct attack from one that arrives late, or a release that decays at the wrong rate. Over a
    2.8 s note thirty-two windows are about 88 ms each, which resolves the attack and the release
    without being sensitive to phase.
    """
    out = []
    if not mono:
        return out
    span = max(1, len(mono) // windows)
    for w in range(windows):
        begin = w * span
        if begin >= len(mono):
            break
        end = min(begin + span, len(mono))
        energy = sum(v * v for v in mono[begin:end]) / (end - begin)
        out.append(round(10 * math.log10(energy) if energy > 0 else -999.0, 3))
    return out


def measure(path, rate, target):
    """Every metric for one case. `target` is where the fundamental should be, or None for a case
    that has no pitch to find -- a drum key selects a kit entry, not a transposition, so asking
    where its fundamental landed would only measure which noise peak won."""
    raw = path.read_bytes()
    frames = len(raw) // 8
    samples = struct.unpack(f"<{frames * 2}f", raw[:frames * 8])
    left = samples[0::2]
    right = samples[1::2]

    peak = max((abs(v) for v in samples), default=0.0)
    energy = sum(v * v for v in samples)
    rms = math.sqrt(energy / len(samples)) if samples else 0.0
    mono = [(left[i] + right[i]) * 0.5 for i in range(frames)]

    power, size = power_spectrum(mono)

    measured = {
        "frames": frames,
        "peak": peak,
        "rms": rms,
        "sha256": hashlib.sha256(raw).hexdigest(),
        "first8": [round(v, 9) for v in samples[:8]],
        "bands": octave_bands(power, size, rate),
        "envelope": rms_envelope(mono),
    }
    if target is not None:
        measured["pitchHz"] = fundamental(power, size, rate, target)
    return measured


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
    # How to launch a Windows binary when the host is not Windows. Plain `wine` is the default and
    # is what a Linux box wants; CrossOver's launcher takes the bottle as its own argument, so the
    # whole command line is given here rather than guessed:
    #
    #   --runner "/Applications/CrossOver.app/Contents/SharedSupport/CrossOver/bin/cxstart
    #             --bottle tabulasonora"
    parser.add_argument("--runner", default=None,
                        help="command that launches a Windows .exe; defaults to 'wine' off Windows")
    parser.add_argument("--jobs", type=int, default=6,
                        help="how many cases to render at once, each its own process")
    parser.add_argument("--batch", action="store_true",
                        help="render the whole sweep in one process: faster, and WRONG for any "
                             "tone with a random LFO -- see the note above")
    arguments = parser.parse_args()

    if not arguments.scdec.exists():
        sys.exit(f"harness not found at {arguments.scdec}\n"
                 "  build it: (in specv2/tools/decoder) "
                 "dotnet publish -c Release -r win-x64 --self-contained")

    cases = build_cases()
    arguments.audio_dir.mkdir(parents=True, exist_ok=True)

    if arguments.runner is not None:
        runner = shlex.split(arguments.runner)
    else:
        runner = [] if sys.platform == "win32" else ["wine"]

    # The pared-down environment is for wine, which is noisy and picks up the host's PATH. Windows
    # runs the harness directly and needs its own environment intact; a custom runner keeps it for
    # the same reason, since CrossOver's launcher resolves its bottle through HOME.
    if sys.platform == "win32":
        environment = None
    elif arguments.runner is not None:
        environment = {**os.environ, "WINEDEBUG": "-all"}
    else:
        environment = {"WINEDEBUG": "-all", "PATH": "/usr/bin:/bin"}

    def case_line(case):
        return (f"{case['program']} {case['note']} {case['velocity']} "
                f"{case['hold']} {case['map']} {case['channel']}\n")

    if arguments.batch:
        # **Order-dependent, and only here because it is fast.** `notebatch` resets between cases,
        # but a GS reset frees the LFO nodes without zeroing them -- their random registers survive
        # into the next case, so any tone with a random LFO records a value that depends on what
        # preceded it. Measured: three cases in the sweep differ from their own isolated render by
        # up to 5.8 cents. Use for a quick look, never for a fixture anything is judged against.
        case_file = arguments.audio_dir.parent / "notes_cases.txt"
        case_file.write_text("".join(case_line(c) for c in cases))
        command = runner + [str(arguments.scdec), str(arguments.dll), "notebatch",
                            str(case_file.resolve()), str(arguments.audio_dir.resolve()),
                            str(arguments.tail)]
        print(f"rendering {len(cases)} notes in ONE process (order-dependent) ...", flush=True)
        result = subprocess.run(command, capture_output=True, text=True, env=environment)
        if result.returncode != 0:
            sys.exit(f"harness failed ({result.returncode}):\n{result.stderr[-2000:]}")
    else:
        # One process per case, because that is the only thing that actually isolates them. The
        # cases are independent, so they run concurrently; each needs its own output directory
        # because `notebatch` always names what it writes `case0000.f32`.
        def render(index_case):
            index, case = index_case
            work = arguments.audio_dir / f".work{index:04d}"
            if work.exists():
                shutil.rmtree(work)
            work.mkdir(parents=True)
            case_file = work / "case.txt"
            case_file.write_text(case_line(case))
            command = runner + [str(arguments.scdec), str(arguments.dll), "notebatch",
                                str(case_file.resolve()), str(work.resolve()),
                                str(arguments.tail)]
            result = subprocess.run(command, capture_output=True, text=True, env=environment)
            produced = work / "case0000.f32"
            if result.returncode != 0 or not produced.exists():
                return index, f"exit {result.returncode}: {result.stderr[-400:]}"
            shutil.move(str(produced), str(arguments.audio_dir / f"case{index:04d}.f32"))
            shutil.rmtree(work, ignore_errors=True)
            return index, None

        print(f"rendering {len(cases)} notes, one process each, {arguments.jobs} at a time ...",
              flush=True)
        done = 0
        with concurrent.futures.ThreadPoolExecutor(max_workers=arguments.jobs) as pool:
            for index, error in pool.map(render, enumerate(cases)):
                if error is not None:
                    sys.exit(f"harness failed on case {index}: {error}")
                done += 1
                if done % 20 == 0 or done == len(cases):
                    print(f"  {done}/{len(cases)}", flush=True)

    for index, case in enumerate(cases):
        audio = arguments.audio_dir / f"case{index:04d}.f32"
        if not audio.exists():
            sys.exit(f"harness did not produce {audio}")
        target = None if case["channel"] == DRUM_CHANNEL \
            else 440.0 * 2 ** ((case["note"] - 69) / 12)
        case.update(measure(audio, arguments.rate, target))
        case["audio"] = str(audio.relative_to(arguments.output.parent)) \
            if arguments.output.parent in audio.parents else str(audio)

    document = {
        "_note": ("Single-note renders from the REFERENCE DLL, not from any reimplementation. "
                  "Roland-derived: generate locally, do not redistribute. These are tolerance "
                  "gates -- this port is not bit-exact with the DLL and does not aim to be, so the "
                  "sha256 identifies the oracle audio rather than anything the port must match. "
                  "Compare frames exactly, and peak, rms, the octave bands, the envelope and "
                  "pitchHz within tolerance. Cases on channel 9 are drum kits, and carry no "
                  "pitchHz: a drum key selects a kit entry rather than a transposition."),
        "generator": "tools/dump_note_renders_oracle.py",
        "dllSha256": hashlib.sha256(arguments.dll.read_bytes()).hexdigest(),
        "sampleRate": arguments.rate,
        "tailSeconds": arguments.tail,
        "cases": cases,
    }
    arguments.output.parent.mkdir(parents=True, exist_ok=True)
    arguments.output.write_text(json.dumps(document, indent=1))

    sounding = sum(1 for c in cases if c["peak"] > 0.0)
    drums = sum(1 for c in cases if c["channel"] == DRUM_CHANNEL)
    print(f"wrote {len(cases)} cases ({len(cases) - drums} melodic, {drums} drum, "
          f"{sounding} sounding) to {arguments.output}")
    print(f"oracle audio in {arguments.audio_dir} "
          f"({sum(c['frames'] for c in cases):,} frames total)")


if __name__ == "__main__":
    main()
