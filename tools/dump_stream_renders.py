#!/usr/bin/env python3
"""Record this engine's own block-loop renders, as a baseline to notice drift against.

**Read this before trusting the gate it feeds.** These references used to come from the archived C#
engine, and while that engine was the closest thing to the hardware available, the stream gate was a
real independent check: two implementations agreeing sample by sample to within one LSB.

It is not that any more. Every one of the gate's cases touches a wave whose data starts partway into
a scale block, and this engine now decodes those from their exact start where the C# one rounded
down -- a correction towards the DLL that the archived checkout cannot be re-run to follow. A fixture
generated from it would fail, and would be right to.

So this script generates from *this* engine, and the gate it feeds is a **regression baseline**: it
catches a change nobody meant to make, and it cannot catch a mistake both sides share. The check that
does have an outside opinion is `dump_song_renders_oracle.py`, which drives the reference DLL through
its own exported API -- start there when the question is whether the engine is right rather than
whether it moved.

The WAVs are large and Roland-derived; they live in fixtures/, which is gitignored.

Usage:
    python3 tools/dump_stream_renders.py <SCCore.dll> <output-dir> <song.mid> [<song.mid> ...]
        [--cli <path to tabula-sonora>]
"""

import argparse
import json
import pathlib
import re
import struct
import subprocess
import sys

MAPS = [1, 4]

# Where a build of this repository usually leaves the CLI. Several, because the presets differ by
# platform and generator -- Ninja puts it under the preset directory, Visual Studio adds a
# configuration below that, and a Windows build appends .exe.
CLI_CANDIDATES = [
    "build/release/apps/cli/tabula-sonora",
    "build/release/apps/cli/tabula-sonora.exe",
    "build/release-vs/apps/cli/RelWithDebInfo/tabula-sonora.exe",
    "build/debug/apps/cli/tabula-sonora",
    "build/debug-vs/apps/cli/Debug/tabula-sonora.exe",
]


def find_cli(explicit):
    if explicit is not None:
        if not explicit.exists():
            sys.exit(f"No CLI at {explicit}.")
        return explicit.resolve()

    root = pathlib.Path(__file__).resolve().parent.parent
    for candidate in CLI_CANDIDATES:
        path = root / candidate
        if path.exists():
            return path
    sys.exit("No built CLI found. Build one (cmake --build --preset release) or pass --cli.")


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("dll", type=pathlib.Path)
    parser.add_argument("output", type=pathlib.Path)
    parser.add_argument("midi", type=pathlib.Path, nargs="+")
    parser.add_argument("--cli", type=pathlib.Path, default=None,
                        help="the tabula-sonora binary to render with")
    arguments = parser.parse_args()

    dll = arguments.dll.resolve()
    cli = find_cli(arguments.cli)
    print(f"rendering with {cli}")

    arguments.output.mkdir(parents=True, exist_ok=True)
    output_dir = arguments.output.resolve()
    cases = []

    for midi in arguments.midi:
        midi = midi.resolve()
        for tone_map in MAPS:
            name = f"{midi.stem}-map{tone_map}-stream.wav"
            out_path = output_dir / name

            # `--stream` is the hardware's 64-voice limit, which is what the gate is about: the
            # default grows the pool instead, and a baseline taken with unbounded polyphony would
            # not notice a change in the stealing at all.
            result = subprocess.run(
                [str(cli), "render", str(midi), str(out_path),
                 "--dll", str(dll), "--map", str(tone_map), "--stream"],
                check=True, capture_output=True, text=True,
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
        "_note": ("Block-loop renders from THIS engine, kept as a regression baseline rather than "
                  "as an independent check -- see the module docstring, and the oracle gate for a "
                  "comparison that has an outside opinion. Roland-derived: generate locally, do "
                  "not redistribute."),
        "generator": "tools/dump_stream_renders.py",
        "cases": cases,
    }, indent=1), encoding="utf-8")
    print(f"Wrote {len(cases)} references to {arguments.output}")


if __name__ == "__main__":
    main()
