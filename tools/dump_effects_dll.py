#!/usr/bin/env python3
"""Capture all 26 send-effect impulse responses from the module itself.

This replaces a fixture that was taken from the retired C# port. Pinning one reimplementation
against another only proves they agree, and they did -- on a delay that was 60 ms late, quantised
its tap gains over 127 where the module uses 128, spaced its echoes one sample short, scaled its
left and right taps through the front panel's rounded percent table, and had no input DC blocker.
Every one of those survived a bit-exact gate for as long as the gate faced the wrong way.

The responses come from `scdec`'s `revir` / `choir` / `dlyir` modes, which call the module's own
`fx_reverb_process`, `fx_chorus_stage_l` and `fx_chorus_stage_r` with a controlled impulse. Driving
the networks with a *note* instead would fold in the voice path, which differs enough between
engines (~11%, and it varies with the patch) to hide or invent a network difference.

Two things the capture has to control for, both handled by scdec:

  * The networks' gain ramps sit at zero while nothing is feeding their bus, so an impulse into an
    idle engine is silent. A note is driven first and allowed to decay.
  * The chorus sweep LFO free-runs. Its phase is pinned to zero, where a reimplementation's
    accumulator starts, or the comparison is meaningless.

Output is raw interleaved float32 -- the same form the C++ side writes -- because 16-bit renders
put a floor at -90 dB that a decaying tail falls through, which is its own way of inventing a
result. Compare with a tolerance around 1e-5 rather than bit-exactly: the module is float
throughout and this engine is double, and that alone is worth ~3e-5 on a unit impulse.

Roland-derived output: generate locally, do not redistribute.

Usage:
    python3 tools/dump_effects_dll.py [--scdec PATH] [--dll PATH] [--out fixtures/effects_dll]
"""

import argparse
import pathlib
import shutil
import subprocess
import sys

# The harness lives in the sibling spec checkout. Its directory has been called both `spec` and
# `specv2`; try each rather than pinning one, so a checkout named either way works without the flag.
SCDEC_SUFFIX = "tools/decoder/bin/Release/net10.0/win-x64/publish/scdec.exe"


def default_scdec() -> pathlib.Path:
    """First sibling harness that exists, else the `spec` spelling for the error message."""
    candidates = [pathlib.Path("..") / name / SCDEC_SUFFIX for name in ("spec", "specv2")]
    return next((c for c in candidates if c.exists()), candidates[0])



NETWORKS = (
    [("reverb", "revir", t) for t in range(8)]
    + [("chorus", "choir", t) for t in range(8)]
    + [("delay", "dlyir", t) for t in range(10)]
)

# Long enough that every network has spoken. Delay type 2's centre tap is 32000 samples, so a
# shorter window would capture silence and agree with anything.
SAMPLES = 48000


def to_wine_path(path: pathlib.Path) -> str:
    return "Z:" + str(path.resolve()).replace("/", "\\")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--scdec",
        type=pathlib.Path,
        default=default_scdec(),
    )
    parser.add_argument("--dll", type=pathlib.Path, default=pathlib.Path("SCCore.dll"))
    parser.add_argument("--out", type=pathlib.Path, default=pathlib.Path("fixtures/effects_dll"))
    arguments = parser.parse_args()

    if not arguments.scdec.exists():
        sys.exit(
            f"No scdec at {arguments.scdec}. Build it self-contained or it will not start under "
            "wine:\n  dotnet publish -c Release -r win-x64 --self-contained true"
        )
    if not arguments.dll.exists():
        sys.exit(f"No SCCore.dll at {arguments.dll}.")
    if shutil.which("wine") is None:
        sys.exit("wine is not installed; scdec is a Windows binary.")

    arguments.out.mkdir(parents=True, exist_ok=True)
    for kind, mode, type_number in NETWORKS:
        destination = arguments.out / f"{kind}{type_number}.f32"
        result = subprocess.run(
            [
                "wine",
                str(arguments.scdec),
                to_wine_path(arguments.dll),
                mode,
                to_wine_path(destination),
                str(SAMPLES),
                str(type_number),
            ],
            capture_output=True,
            text=True,
        )
        if not destination.exists() or destination.stat().st_size != SAMPLES * 2 * 4:
            sys.exit(f"{kind} {type_number} produced no capture.\n{result.stdout}{result.stderr}")
        print(f"{kind} {type_number} -> {destination}")

    print(f"\nwrote {len(NETWORKS)} responses to {arguments.out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
