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
import os
import pathlib
import shlex
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

    # The XG case. `th07_19_user_gm.mid` cannot serve as one: the harness has to filter its
    # out-of-range Multi Part writes to stop the reference faulting, so its oracle render is of a
    # file with some of its XG removed and it cannot tell a correct XG reading from no reading.
    # MAKORO.MID trips neither unchecked index, so the reference renders it whole -- System On, the
    # effect-type translation, a variation bank, the SFX voice bank on channel 6 and a drum kit
    # chosen by bank MSB 127 on channel 10.
    ("MAKORO.MID", 1),
    ("onestop.mid", 4),

    # Chosen by coverage rather than by taste: a scan of a 128,000-file archive found 288 files
    # that genuinely drive the control matrix, and these ten are the smallest set that assigns
    # every route any of them assigns -- all eleven destinations from all six sources -- while also
    # between them carrying polyphonic and channel aftertouch, two ports, the part EQ, drum setup,
    # insertion EFX selection and the sound controllers. Every one of them was checked to render
    # through the DLL before being added; the oracle faults on some inputs and a corpus entry that
    # cannot be rendered is worse than none.
    #
    # They are ordinary MIDI files off the internet and are **not** redistributed with this repo --
    # `testdata/` is gitignored. Anyone rebuilding this fixture supplies their own corpus; what is
    # worth keeping is the selection method, in `tools/scan_midi_archive.py`.
    ("it_must_have_been_love.mid", 4),
    ("bigben.mid", 4),
    ("rainy.mid", 4),
    ("macross2.mid", 4),
    ("robyn_show_me_love.mid", 4),
    ("shangai.mid", 4),
    ("dreaming_i_was_dreaming.mid", 4),
    ("ff5_1_16_harvest.mid", 4),
    ("pchoral3.mid", 4),
    ("rockarn12.mid", 4),

    # The GS patch bulk dump, `48 <a2> <a3>` -- 402 files in a 132,006-file archive carry it, and
    # until it was implemented every one of them rendered with all sixteen parts at their defaults.
    # `ntysoypr` and `darkness3` send the complete sequence (29 and 30 messages, `48 00 00` through
    # `48 1c 00` / `1d 00`); `wwtbam` sends two, a partial restore rather than a whole mixer.
    #
    # The synthetic exercisers beside them in `specv2/testdata/bulkdump` are deliberately **not**
    # here. They drive an unanchored continuation, a malformed payload, and a walk past the last
    # part -- and the module faults on all three, so there is no reference render to compare
    # against. That is the point of them: they are unit-test fixtures for behaviour this engine has
    # on purpose and the module does not.
    ("ntysoypr.mid", 4),
    ("darkness3.mid", 4),
    ("wwtbam.mid", 4),

    # Roland's own demonstration disks, at the map each was written for. These are the general
    # fidelity backbone rather than a feature hunt: they are densely and competently sequenced by
    # the people who built the module, so they exercise ordinary playing -- twenty programs at a
    # time, real bend and modulation, drums throughout -- in a way an internet file selected for
    # one SysEx address does not. `roland_sc88_y05` alone touches 43 programs.
    ("roland_allstars.mid", 1),
    ("roland_suplex.mid", 1),
    ("roland_deadend.mid", 1),
    ("roland_sc55_demo03.mid", 1),
    ("roland_sc55_demo13.mid", 1),
    ("roland_sc88_y03.mid", 2),
    ("roland_sc88_y05.mid", 2),
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


def balance(left, right, windows=64):
    """The right channel's share of each window: what every measure above cannot see.

    Peak, RMS, the bands and the envelope are all taken on the mono sum, so a voice that moves
    across the stereo field moves none of them -- the energy is still there, just somewhere else.
    This is the measure that sees where. 0 is hard left, 0.5 centred, 1 hard right; a silent window
    reads 0.5 and the gate skips it against the RMS envelope rather than believing it centred.
    """
    if not left:
        return []
    span = max(1, len(left) // windows)
    out = []
    for w in range(windows):
        chunk_l = left[w * span:(w + 1) * span]
        chunk_r = right[w * span:(w + 1) * span]
        if not chunk_l:
            break
        rms_l = math.sqrt(sum(v * v for v in chunk_l) / len(chunk_l))
        rms_r = math.sqrt(sum(v * v for v in chunk_r) / len(chunk_r))
        total = rms_l + rms_r
        out.append(round(rms_r / total, 4) if total > 0 else 0.5)
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
    # How to launch a Windows binary, when the host is not Windows. Plain `wine` is the default and
    # is what a Linux box wants. macOS has no system wine; CrossOver's launcher takes the bottle as
    # its own argument, so the whole command line is given here rather than guessed:
    #
    #   --runner "/Applications/CrossOver.app/Contents/SharedSupport/CrossOver/bin/cxstart
    #             --bottle tabulasonora"
    #
    # A render taken this way is not interchangeable with one from a real x86_64 host -- see the
    # note in the fixture about which oracle produced it -- so point `--output` and `--audio-dir`
    # somewhere of their own rather than overwriting the authoritative harvest.
    parser.add_argument("--runner", default=None,
                        help="command that launches a Windows .exe; defaults to 'wine' off Windows")
    arguments = parser.parse_args()

    if not arguments.scdec.exists():
        sys.exit(f"harness not found at {arguments.scdec}")

    arguments.audio_dir.mkdir(parents=True, exist_ok=True)
    if arguments.runner is not None:
        runner = shlex.split(arguments.runner)
    else:
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
        # POSIX PATH and nothing else fails before the DLL is even opened. A custom runner keeps
        # the host's environment for the same reason Windows does: CrossOver's launcher resolves
        # its bottle through `HOME` and its own support paths, and starves without them.
        if sys.platform == "win32" or arguments.runner is not None:
            environment = None if sys.platform == "win32" else {**os.environ, "WINEDEBUG": "-all"}
        else:
            environment = {"WINEDEBUG": "-all", "PATH": "/usr/bin:/bin"}
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

        # The chorus LFO accumulator's position when the song starts. The reference prints it on
        # every render, and it matters because nothing ever resets that accumulator -- not a GS
        # reset, not a macro change -- so its phase here is a function of how long the reference ran
        # before the song did. Two engines that agree on every note still place their wet
        # differently unless they agree on this, and the gate seeds it rather than comparing across
        # an arbitrary offset.
        #
        # A property of the *harvest*, not of the DLL: change the warm-up and it changes. So it is
        # recorded per case rather than assumed, the same way each case's audio is.
        phase = None
        for line in result.stdout.splitlines():
            if "lfoPhase at song start" in line:
                digits = "".join(c for c in line.split("=", 1)[1] if c in "-0123456789 ").split()
                if digits:
                    phase = int(digits[0])
                break

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
            "balance": balance(left, right),
            "sha256": hashlib.sha256(audio.read_bytes()).hexdigest(),
            **({"chorusLfoPhase": phase} if phase is not None else {}),
        })

    document = {
        "_note": ("Whole-song renders from the REFERENCE DLL, not from any reimplementation. "
                  "Roland-derived: generate locally, do not redistribute. Tolerance gates: the "
                  "reference's effect LFOs start at a phase this port cannot yet derive, so two "
                  "renders can agree on every note and still correlate near 0.18 sample by sample. "
                  "`chorusLfoPhase` is where that accumulator stood when each song began, so a gate "
                  "can place its own there and compare the wet rather than the offset. "
                  "Compare frames, peak, rms, the octave bands, the coarse envelope and the "
                  "per-window stereo balance; the sha256 identifies the oracle audio, it is not a "
                  "target."),
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
