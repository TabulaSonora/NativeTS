#!/usr/bin/env python3
"""Record the reference engine's impulse response for all 26 send-effect networks.

Like the render oracle, this does not reimplement anything: the reference build is the oracle, and
what is being checked is that the port's networks are the same networks. It shells out to the C#
`dump-effect` and stores a digest of each response plus the peak and a few literal samples.

The peak is not decoration. Twelve of these comparisons were once green upstream while testing
nothing at all -- the fixture windows were shorter than the delays, so both sides were silent and
agreed perfectly. Recording the peak is what makes a silent network a failure rather than a pass.

Needs the sibling C# checkout built:
    dotnet build -c Release          (in ../DotNetAdministravit)

Usage:
    python3 tools/dump_effects.py <output.json> [--csharp ../DotNetAdministravit]

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

# Long enough that every network has actually spoken by the end of the window. The delay presets
# reach well past a second, so they get twice as long.
NETWORKS = (
    [("reverb", t, 48000) for t in range(8)]
    + [("chorus", t, 48000) for t in range(8)]
    + [("delay", t, 96000) for t in range(10)]
)


def dump(csharp_root, kind, type_number, samples, out_path):
    subprocess.run(
        [
            "dotnet", "run", "-c", "Release", "--no-build",
            "--project", "src/TabulaSonora.Tools", "--",
            "dump-effect", kind, str(type_number), str(samples), str(out_path),
        ],
        cwd=csharp_root,
        check=True,
        capture_output=True,
    )
    return out_path.read_bytes()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=pathlib.Path)
    parser.add_argument("--csharp", type=pathlib.Path, default=pathlib.Path("../DotNetAdministravit"))
    arguments = parser.parse_args()

    csharp_root = arguments.csharp.resolve()
    if not (csharp_root / "src" / "TabulaSonora.Tools").is_dir():
        sys.exit(f"No C# checkout at {csharp_root}.")

    cases = []
    with tempfile.TemporaryDirectory() as scratch:
        out_path = pathlib.Path(scratch) / "effect.f32"

        for kind, type_number, samples in NETWORKS:
            raw = dump(csharp_root, kind, type_number, samples, out_path)
            values = struct.unpack("<%df" % (len(raw) // 4), raw)
            peak = max((abs(v) for v in values), default=0.0)

            if peak == 0.0:
                sys.exit(
                    f"{kind} {type_number} produced silence over {samples} samples. "
                    "Widen the window rather than recording a fixture that proves nothing."
                )

            cases.append(
                {
                    "kind": kind,
                    "type": type_number,
                    "samples": samples,
                    "sha256": hashlib.sha256(raw).hexdigest(),
                    "peak": peak,
                    # First non-zero sample and its index: where the network starts speaking, which
                    # is the single most diagnosable number when a digest moves.
                    "firstSoundingIndex": next(
                        (i for i, v in enumerate(values) if v != 0.0), -1
                    ),
                    "last8": list(values[-8:]),
                }
            )
            print(f"  {kind:7} {type_number}  peak {peak:.9f}  first sound at "
                  f"{cases[-1]['firstSoundingIndex']}")

    arguments.output.parent.mkdir(parents=True, exist_ok=True)
    arguments.output.write_text(
        json.dumps(
            {
                "_note": (
                    "Reference effect impulse responses from the C# engine, driven by coefficients "
                    "harvested from a licensed SCCore.dll. Roland-derived: generate locally, do not "
                    "redistribute."
                ),
                "cases": cases,
            },
            indent=1,
        ),
        encoding="utf-8",
    )
    print(f"Wrote {len(cases)} networks to {arguments.output}")


if __name__ == "__main__":
    main()
