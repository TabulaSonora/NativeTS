#!/usr/bin/env python3
"""Record what the REFERENCE DLL renders for whole songs.

The end-to-end counterpart to `dump_note_renders_oracle.py`, and the replacement for
`dump_song_renders.py` / the `stream_renders.json` references, both of which recorded the archived
C# port. Everything this engine builds has to compose correctly for a song to agree: MIDI parsing,
patch resolution, all four envelopes, both LFOs, the pitch ramp, the filter, the drum path with its
kits and choke groups, the three send effects, and the final quantisation.

**Tolerance, not digests, and for a sharper reason than the single notes.** A song runs the chorus
and reverb, whose LFOs the reference starts at a phase this port cannot yet derive, so two renders
that agree on every note still diverge sample by sample -- whole-song correlation against the oracle
sits near 0.18 even when the level and every octave band agree to a tenth of a dB. Sample identity
is therefore not merely unreachable, it is the wrong question. What is recorded is what
`docs/COMPARING_RENDERS.md` argues actually distinguishes a good render from a bad one: duration,
peak, RMS, per-octave level, and a coarse RMS envelope that catches a note going missing or arriving
late without being sensitive to phase.

The oracle audio is kept alongside so a failure can be measured with `compare_envelope.py` rather
than only counted. Roland-derived: generated locally, gitignored.

Usage:
    python3 tools/dump_song_renders_oracle.py <SCCore.dll> fixtures/song_renders_oracle.json \\
        [--scdec <path>] [--audio-dir fixtures/oracle/songs] [--tail 1.5]

Needs the harness from the sibling specv2 checkout, and wine on a non-Windows host.
"""

import argparse
import hashlib
import json
import math
import pathlib
import struct
import subprocess
import sys
import wave

# Every map for the two canyon variants -- the pair is what proves format 0 and format 1 parse the
# same -- plus one pass over the rest of the corpus. The heavier files are here because they are
# the ones that have historically caught things: drum-dense, effect-dense, and bend-dense.
SONGS = [
    ("canyon.mid", 1), ("canyon.mid", 2), ("canyon.mid", 3), ("canyon.mid", 4),
    ("canyon-format1.mid", 1), ("canyon-format1.mid", 4),
    ("sc50nn.mid", 1),
    ("transcendental.mid", 1),
    ("bad_apple_feat_nomico_s__msgs.mid", 1),
    ("test_poly_bend.mid", 1),
    ("panwet.mid", 1),
    ("th07_19_user_gm.mid", 1),
    ("onestop.mid", 4),
]


def read_wav(path):
    with wave.open(str(path), "rb") as handle:
        frames = handle.getnframes()
        channels = handle.getnchannels()
        raw = handle.readframes(frames)
    values = struct.unpack(f"<{len(raw) // 2}h", raw)
    left = values[0::channels]
    right = values[1::channels] if channels > 1 else left
    return left, right, frames


def octave_bands(mono, rate):
    size = 1
    while size < min(len(mono), 1 << 16):
        size <<= 1
    if size < 256:
        return []
    window = [0.5 - 0.5 * math.cos(2 * math.pi * i / size) for i in range(size)]
    # Take the window from a quarter in: the opening of a song is often silence.
    start = min(len(mono) - size, len(mono) // 4)
    real = [mono[start + i] * window[i] for i in range(size)]
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
                ur, ui = real[i + k], imag[i + k]
                vr = real[i + k + length // 2] * cr - imag[i + k + length // 2] * ci
                vi = real[i + k + length // 2] * ci + imag[i + k + length // 2] * cr
                real[i + k], imag[i + k] = ur + vr, ui + vi
                real[i + k + length // 2], imag[i + k + length // 2] = ur - vr, ui - vi
                cr, ci = cr * wr - ci * wi, cr * wi + ci * wr
        length <<= 1
    power = [real[k] * real[k] + imag[k] * imag[k] for k in range(size // 2 + 1)]
    bands = []
    for centre in (63, 125, 250, 500, 1000, 2000, 4000, 8000):
        low, high = centre / math.sqrt(2), centre * math.sqrt(2)
        total = sum(power[k] for k in range(len(power)) if low <= k * rate / size < high)
        bands.append(round(10 * math.log10(total) if total > 0 else -999.0, 3))
    return bands


def envelope(mono, rate, windows=64):
    """A coarse RMS envelope: catches a missing or late note without seeing phase at all."""
    if not mono:
        return []
    span = max(1, len(mono) // windows)
    out = []
    for w in range(windows):
        chunk = mono[w * span:(w + 1) * span]
        if not chunk:
            break
        energy = sum(v * v for v in chunk) / len(chunk)
        out.append(round(10 * math.log10(energy) if energy > 0 else -999.0, 2))
    return out


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("dll", type=pathlib.Path)
    parser.add_argument("output", type=pathlib.Path)
    parser.add_argument("--scdec", type=pathlib.Path,
                        default=pathlib.Path("../specv2/tools/decoder/bin/Release/net10.0/"
                                             "win-x64/publish/scdec.exe"))
    parser.add_argument("--audio-dir", type=pathlib.Path,
                        default=pathlib.Path("fixtures/oracle/songs"))
    parser.add_argument("--testdata", type=pathlib.Path, default=pathlib.Path("testdata"))
    parser.add_argument("--tail", type=float, default=1.5)
    parser.add_argument("--rate", type=int, default=32000)
    arguments = parser.parse_args()

    if not arguments.scdec.exists():
        sys.exit(f"harness not found at {arguments.scdec}")

    arguments.audio_dir.mkdir(parents=True, exist_ok=True)
    runner = [] if sys.platform == "win32" else ["wine"]
    cases = []

    for midi, tone_map in SONGS:
        source = arguments.testdata / midi
        if not source.exists():
            print(f"  skipping {midi}: not in {arguments.testdata}")
            continue
        stem = pathlib.Path(midi).stem.replace(" ", "_")
        audio = arguments.audio_dir / f"{stem}-map{tone_map}.wav"

        print(f"rendering {midi} map {tone_map} through the oracle ...", flush=True)
        command = runner + [str(arguments.scdec), str(arguments.dll), "smf",
                            str(source.resolve()), str(audio.resolve()),
                            str(tone_map), str(arguments.tail)]
        # The pared-down environment is for wine, which is noisy and picks up the host's PATH.
        # Windows runs the harness directly and needs its own environment intact -- handing it a
        # POSIX PATH and nothing else fails before the DLL is even opened.
        environment = None if sys.platform == "win32" else {"WINEDEBUG": "-all",
                                                            "PATH": "/usr/bin:/bin"}
        result = subprocess.run(command, capture_output=True, text=True, env=environment)
        if result.returncode != 0 or not audio.exists():
            # One bad file must not cost the whole sweep. The reference itself faults on some
            # inputs -- th07_19_user_gm.mid takes it down with an access violation -- and that is
            # worth recording as a property of the oracle rather than hiding by aborting.
            print(f"  ! oracle failed on {midi} map {tone_map} (exit {result.returncode}); "
                  f"recording as unavailable")
            cases.append({"midi": midi, "map": tone_map, "oracleFailed": True,
                          "exitCode": result.returncode})
            continue

        left, right, frames = read_wav(audio)
        mono = [(left[i] + right[i]) * 0.5 / 32768.0 for i in range(frames)]
        peak = max((abs(v) for v in left + right), default=0) / 32768.0
        energy = sum(v * v for v in mono) / max(1, len(mono))

        cases.append({
            "midi": midi,
            "map": tone_map,
            "audio": str(audio),
            "frames": frames,
            "seconds": round(frames / arguments.rate, 4),
            "peak": peak,
            "rms": math.sqrt(energy),
            "bands": octave_bands(mono, arguments.rate),
            "envelope": envelope(mono, arguments.rate),
            "sha256": hashlib.sha256(audio.read_bytes()).hexdigest(),
        })

    document = {
        "_note": ("Whole-song renders from the REFERENCE DLL, not from any reimplementation. "
                  "Roland-derived: generate locally, do not redistribute. Tolerance gates: the "
                  "reference's effect LFOs start at a phase this port cannot yet derive, so two "
                  "renders can agree on every note and still correlate near 0.18 sample by sample. "
                  "Compare frames, peak, rms, the octave bands and the coarse envelope; the sha256 "
                  "identifies the oracle audio, it is not a target."),
        "generator": "tools/dump_song_renders_oracle.py",
        "dllSha256": hashlib.sha256(arguments.dll.read_bytes()).hexdigest(),
        "sampleRate": arguments.rate,
        "tailSeconds": arguments.tail,
        "cases": cases,
    }
    arguments.output.parent.mkdir(parents=True, exist_ok=True)
    arguments.output.write_text(json.dumps(document, indent=1))
    print(f"wrote {len(cases)} songs to {arguments.output}")


if __name__ == "__main__":
    main()
