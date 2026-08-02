#!/usr/bin/env python3
"""Record what the reference C# engine renders for a spread of single notes.

Unlike the other oracles in tools/, this one does not reimplement anything: at the render level the
reference implementation *is* the oracle, and what is being checked is that the port composes every
subsystem the same way it does. It shells out to the C# `render-note` and stores a digest of each
result plus a few literal samples.

Digests rather than whole renders because a single note is a megabyte of float; the literals are
what make a failure diagnosable rather than just red.

Needs the sibling C# checkout built:
    dotnet build -c Release          (in ../DotNetAdministravit)

Usage:
    python3 tools/dump_note_renders.py <SCCore.dll> <output.json> [--csharp ../DotNetAdministravit]

Roland-derived output: generate locally, do not redistribute.
"""

import argparse
import hashlib
import json
import pathlib
import struct
import subprocess
import sys
import tempfile

# Chosen to cover the shapes that behave differently rather than to be exhaustive: one-shot and
# looping waves, single- and multi-partial tones, every tone map, the velocity extremes that select
# different crossfade branches, and notes far enough apart to land in different multisample zones.
PROGRAMS = [0, 1, 4, 6, 11, 16, 19, 24, 30, 33, 40, 48, 52, 56, 61, 66, 73, 80, 88, 95, 104, 112, 120]
NOTES = [24, 36, 48, 60, 72, 84, 96]
VELOCITIES = [1, 20, 64, 100, 127]
MAPS = [1, 2, 3, 4]


def render(csharp_root, dll, program, note, velocity, hold, out_path, tone_map):
    subprocess.run(
        [
            "dotnet", "run", "-c", "Release", "--no-build",
            "--project", "src/TabulaSonora.Tools", "--",
            "render-note", str(dll), str(program), str(note), str(velocity), str(hold),
            str(out_path), str(tone_map),
        ],
        cwd=csharp_root,
        check=True,
        capture_output=True,
    )
    return out_path.read_bytes()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("dll", type=pathlib.Path)
    parser.add_argument("output", type=pathlib.Path)
    parser.add_argument("--csharp", type=pathlib.Path, default=pathlib.Path("../DotNetAdministravit"))
    arguments = parser.parse_args()

    dll = arguments.dll.resolve()
    csharp_root = arguments.csharp.resolve()
    if not (csharp_root / "src" / "TabulaSonora.Tools").is_dir():
        sys.exit(f"No C# checkout at {csharp_root}.")

    digest = hashlib.sha256(dll.read_bytes()).hexdigest()

    cases = []
    with tempfile.TemporaryDirectory() as scratch:
        out_path = pathlib.Path(scratch) / "note.f32"

        # A broad sweep at one note, then a narrow sweep across notes and velocities. Between them
        # every tone map, both partial counts and all the velocity branches are covered.
        plan = [(p, 60, 100, 0.6, m) for p in PROGRAMS for m in MAPS]
        plan += [(0, n, v, 0.4, 4) for n in NOTES for v in VELOCITIES]
        plan += [(48, n, 100, 1.0, 4) for n in NOTES]

        for index, (program, note, velocity, hold, tone_map) in enumerate(plan):
            raw = render(csharp_root, dll, program, note, velocity, hold, out_path, tone_map)
            samples = struct.unpack("<%df" % (len(raw) // 4), raw)

            cases.append(
                {
                    "program": program,
                    "note": note,
                    "velocity": velocity,
                    "hold": hold,
                    "map": tone_map,
                    "frames": len(samples) // 2,
                    "sha256": hashlib.sha256(raw).hexdigest(),
                    "peak": max((abs(v) for v in samples), default=0.0),
                    "first8": list(samples[:8]),
                }
            )
            if (index + 1) % 20 == 0:
                print(f"  {index + 1}/{len(plan)}")

    sounding = sum(1 for c in cases if c["peak"] > 0)
    arguments.output.parent.mkdir(parents=True, exist_ok=True)
    arguments.output.write_text(
        json.dumps(
            {
                "_note": (
                    "Reference renders from a licensed SCCore.dll via the C# engine. "
                    "Roland-derived: generate locally, do not redistribute."
                ),
                "dllSha256": digest,
                "cases": cases,
            },
            indent=1,
        ),
        encoding="utf-8",
    )
    print(f"Wrote {len(cases)} renders ({sounding} sounding) to {arguments.output}")


if __name__ == "__main__":
    main()
