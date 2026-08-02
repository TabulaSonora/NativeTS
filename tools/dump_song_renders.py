#!/usr/bin/env python3
"""Record the reference engine's whole-song renders for a MIDI file.

The end-to-end oracle. Everything the port has built has to compose correctly for these digests to
hold: MIDI parsing, patch resolution, all four envelopes, both LFOs, the filter, the drum path with
its kits and choke groups, the three send effects, and the final 16-bit quantisation.

Digests rather than audio because a render is fifteen megabytes; the note count, duration and peak
are recorded alongside so a failure says roughly where it went wrong before the digest says that it
did.

Needs the sibling C# checkout built:
    dotnet build -c Release          (in ../DotNetAdministravit)

Usage:
    python3 tools/dump_song_renders.py <SCCore.dll> <song.mid> <output.json>

Roland-derived output: generate locally, do not redistribute.
"""

import argparse
import hashlib
import json
import pathlib
import re
import struct
import subprocess
import sys
import tempfile

# One per tone map, then the option combinations that change which code paths run at all.
VARIANTS = [
    {"map": 1, "flags": []},
    {"map": 2, "flags": []},
    {"map": 3, "flags": []},
    {"map": 4, "flags": []},
    {"map": 4, "flags": ["--no-reverb"]},
    {"map": 4, "flags": ["--no-chorus"]},
    {"map": 4, "flags": ["--no-delay"]},
    {"map": 4, "flags": ["--no-reverb", "--no-chorus", "--no-delay"]},
    {"map": 4, "flags": ["--volume", "0.5"]},
    {"map": 4, "flags": ["--end", "30"]},
]


def render(csharp_root, dll, midi, out_path, tone_map, flags):
    result = subprocess.run(
        ["dotnet", "run", "-c", "Release", "--no-build",
         "--project", "src/TabulaSonora.Tools", "--",
         "render", str(dll), str(midi), str(out_path), "--map", str(tone_map), *flags],
        cwd=csharp_root, check=True, capture_output=True, text=True,
    )
    return out_path.read_bytes(), result.stdout


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("dll", type=pathlib.Path)
    parser.add_argument("midi", type=pathlib.Path, nargs="+",
                        help="one or more MIDI files; each is rendered through every variant")
    parser.add_argument("output", type=pathlib.Path)
    parser.add_argument("--csharp", type=pathlib.Path, default=pathlib.Path("../DotNetAdministravit"))
    arguments = parser.parse_args()

    dll = arguments.dll.resolve()
    midis = [m.resolve() for m in arguments.midi]
    csharp_root = arguments.csharp.resolve()
    if not (csharp_root / "src" / "TabulaSonora.Tools").is_dir():
        sys.exit(f"No C# checkout at {csharp_root}.")

    cases = []
    with tempfile.TemporaryDirectory() as scratch:
        out_path = pathlib.Path(scratch) / "song.wav"

        for midi in midis:
          for variant in VARIANTS:
            raw, stdout = render(csharp_root, dll, midi, out_path, variant["map"], variant["flags"])

            # The 44-byte RIFF header is ours to write; what is being compared is the audio.
            audio = raw[44:]
            samples = struct.unpack("<%dh" % (len(audio) // 2), audio)
            peak = max((abs(v) for v in samples), default=0) / 32767.0
            notes = int(re.search(r"(\d+) notes", stdout).group(1))

            if peak == 0.0:
                sys.exit(f"{midi.name} map {variant['map']} {variant['flags']} rendered silence.")

            cases.append({
                "midi": midi.name,
                "map": variant["map"],
                "flags": variant["flags"],
                "frames": len(samples) // 2,
                "notes": notes,
                "peak": peak,
                "sha256": hashlib.sha256(raw).hexdigest(),
                "audioSha256": hashlib.sha256(audio).hexdigest(),
            })
            print(f"  {midi.name:22} map {variant['map']} "
                  f"{' '.join(variant['flags']) or '(default)':38} "
                  f"{notes:5} notes  peak {peak:.6f}")

    arguments.output.parent.mkdir(parents=True, exist_ok=True)
    arguments.output.write_text(json.dumps({
        "_note": ("Reference whole-song renders from the C# engine. Roland-derived: generate "
                  "locally, do not redistribute."),
        "sources": [m.name for m in midis],
        "dllSha256": hashlib.sha256(dll.read_bytes()).hexdigest(),
        "cases": cases,
    }, indent=1), encoding="utf-8")
    print(f"Wrote {len(cases)} renders to {arguments.output}")


if __name__ == "__main__":
    main()
