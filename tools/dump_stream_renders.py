#!/usr/bin/env python3
"""Record the reference engine's real-time block-loop renders.

Unlike the offline gate, this one cannot be a digest. The block loop does not reproduce the
reference bit for bit and is not expected to: see the note in the test for what the residual is and
why chasing it further would mean replicating an accident rather than a behaviour.

So the reference audio is kept whole and the test compares sample by sample against a stated
tolerance. The WAVs are large and Roland-derived; they live in fixtures/, which is gitignored.

Needs the sibling C# checkout built:
    dotnet build -c Release          (in ../DotNetAdministravit)

Usage:
    python3 tools/dump_stream_renders.py <SCCore.dll> <output-dir> <song.mid> [<song.mid> ...]
"""

import argparse
import json
import pathlib
import re
import struct
import subprocess
import sys

MAPS = [1, 4]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("dll", type=pathlib.Path)
    parser.add_argument("output", type=pathlib.Path)
    parser.add_argument("midi", type=pathlib.Path, nargs="+")
    parser.add_argument("--csharp", type=pathlib.Path, default=pathlib.Path("../DotNetAdministravit"))
    arguments = parser.parse_args()

    dll = arguments.dll.resolve()
    csharp_root = arguments.csharp.resolve()
    if not (csharp_root / "src" / "TabulaSonora.Tools").is_dir():
        sys.exit(f"No C# checkout at {csharp_root}.")

    arguments.output.mkdir(parents=True, exist_ok=True)
    output_dir = arguments.output.resolve()
    cases = []

    for midi in arguments.midi:
        midi = midi.resolve()
        for tone_map in MAPS:
            name = f"{midi.stem}-map{tone_map}-stream.wav"
            out_path = output_dir / name

            result = subprocess.run(
                ["dotnet", "run", "-c", "Release", "--no-build",
                 "--project", "src/TabulaSonora.Tools", "--",
                 "render", str(dll), str(midi), str(out_path),
                 "--map", str(tone_map), "--stream"],
                cwd=csharp_root, check=True, capture_output=True, text=True,
            )

            raw = out_path.read_bytes()[44:]
            samples = struct.unpack("<%dh" % (len(raw) // 2), raw)
            peak = max((abs(v) for v in samples), default=0) / 32767.0
            notes = int(re.search(r"(\d+) notes", result.stdout).group(1))
            if peak == 0.0:
                sys.exit(f"{name} rendered silence.")

            cases.append({"midi": midi.name, "map": tone_map, "reference": name,
                          "frames": len(samples) // 2, "notes": notes, "peak": peak})
            print(f"  {name:44} {notes:5} notes  peak {peak:.6f}")

    (output_dir / "stream_renders.json").write_text(json.dumps({
        "_note": ("Reference block-loop renders from the C# engine. Roland-derived: generate "
                  "locally, do not redistribute."),
        "cases": cases,
    }, indent=1), encoding="utf-8")
    print(f"Wrote {len(cases)} references to {arguments.output}")


if __name__ == "__main__":
    main()
