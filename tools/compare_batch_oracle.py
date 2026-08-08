#!/usr/bin/env python3
"""Quantify how far `--batch` moves the reference's own note renders.

`dump_note_renders_oracle.py --batch` renders the whole sweep in one DLL instance with a GS reset
between cases. A reset does not isolate them -- the PRNG, the free-running chorus LFO accumulator
and the effect tanks all survive it -- so each case opens on whatever the previous one left behind.
This measures the size of that, by comparing a batched fixture against the committed
one-process-per-case one.

The point is not to fix the batched numbers. It is to have the magnitude on record, so that a batch
disagreement is never again read as a defect in this port: the two fixtures below come from the same
DLL and the same sweep, and every difference is contamination alone.

Usage:
    python3 tools/compare_batch_oracle.py fixtures/note_renders_oracle.json \\
        fixtures/note_renders_oracle_batch.json [--json <report path>]
"""

import argparse
import json
import math
import pathlib
import sys

# The gate's own tolerances, so "beyond tolerance" here means what it means to the test.
PEAK_TOLERANCE = 0.01
RMS_DB_TOLERANCE = 1.0
BAND_DB_TOLERANCE = 3.0
CENTS_TOLERANCE = 5.0


def key(case):
    # `channel` and `hold` belong in the key: the sweep carries one (program, note, velocity, map)
    # twice -- prog 0 note 36 vel 40 map 1, once melodic on channel 0 and once as a drum on channel
    # 9 -- and a narrower key silently drops one of them from the comparison.
    return (case["program"], case["note"], case["velocity"], case["map"],
            case["channel"], case["hold"])


def cents(a, b):
    """Pitch difference in cents, or None where either side reported no pitch."""
    if not a or not b or a <= 0 or b <= 0:
        return None
    return 1200.0 * math.log2(a / b)


def db(a, b):
    """Level difference in dB, or None where either side is silent."""
    if not a or not b or a <= 0 or b <= 0:
        return None
    return 20.0 * math.log10(a / b)


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("isolated", type=pathlib.Path, help="one-process-per-case fixture")
    parser.add_argument("batched", type=pathlib.Path, help="fixture generated with --batch")
    parser.add_argument("--json", type=pathlib.Path, default=None,
                        help="also write the full per-case report here")
    arguments = parser.parse_args()

    isolated = json.loads(arguments.isolated.read_text())
    batched = json.loads(arguments.batched.read_text())

    # Same DLL, or the comparison measures two variables at once.
    if isolated.get("dllSha256") != batched.get("dllSha256"):
        print("REFUSING: the two fixtures were generated from different DLLs.", file=sys.stderr)
        print(f"  isolated {isolated.get('dllSha256')}", file=sys.stderr)
        print(f"  batched  {batched.get('dllSha256')}", file=sys.stderr)
        return 2

    left = {key(c): c for c in isolated["cases"]}
    right = {key(c): c for c in batched["cases"]}
    shared = sorted(set(left) & set(right))

    if missing := (set(left) ^ set(right)):
        print(f"note: {len(missing)} case(s) present on only one side, skipped")

    rows = []
    for k in shared:
        a, b = left[k], right[k]
        rows.append({
            "case": {"program": k[0], "note": k[1], "velocity": k[2], "map": k[3],
                     "channel": k[4], "hold": k[5]},
            "index": isolated["cases"].index(a),
            "identical": a.get("sha256") == b.get("sha256"),
            "peakDelta": abs(a["peak"] - b["peak"]),
            "rmsDb": db(b["rms"], a["rms"]),
            "cents": cents(b.get("pitchHz"), a.get("pitchHz")),
            "worstBandDb": max(
                (abs(x - y) for x, y in zip(a["bands"], b["bands"])), default=0.0),
        })

    identical = sum(1 for r in rows if r["identical"])
    over_peak = [r for r in rows if r["peakDelta"] > PEAK_TOLERANCE]
    over_rms = [r for r in rows if r["rmsDb"] is not None and abs(r["rmsDb"]) > RMS_DB_TOLERANCE]
    over_band = [r for r in rows if r["worstBandDb"] > BAND_DB_TOLERANCE]
    over_cents = [r for r in rows if r["cents"] is not None and abs(r["cents"]) > CENTS_TOLERANCE]

    def name(row):
        c = row["case"]
        return f"prog {c['program']:3d} note {c['note']:3d} vel {c['velocity']:3d} map {c['map']}"

    print(f"Same DLL ({isolated['dllSha256'][:16]}...), {len(shared)} cases compared.\n")
    print(f"  bit-identical to the isolated render : {identical} of {len(rows)}")
    print(f"  peak beyond {PEAK_TOLERANCE}                     : {len(over_peak)}")
    print(f"  rms beyond {RMS_DB_TOLERANCE} dB                    : {len(over_rms)}")
    print(f"  any octave band beyond {BAND_DB_TOLERANCE} dB        : {len(over_band)}")
    print(f"  pitch beyond {CENTS_TOLERANCE} cents               : {len(over_cents)}")

    if over_cents:
        print("\nWorst pitch deviations (batched vs isolated):")
        for row in sorted(over_cents, key=lambda r: -abs(r["cents"]))[:10]:
            print(f"  {abs(row['cents']):8.2f} cents   {name(row)}")

    if over_band:
        print("\nWorst band deviations:")
        for row in sorted(over_band, key=lambda r: -r["worstBandDb"])[:10]:
            # A band holding literal silence on one side reads as a huge dB gap. That is a real
            # behavioural difference but a meaningless magnitude, so it is worth seeing flagged.
            note = "   (one side silent in that band)" if row["worstBandDb"] > 100 else ""
            print(f"  {row['worstBandDb']:8.2f} dB      {name(row)}{note}")

    if arguments.json:
        arguments.json.write_text(json.dumps({
            "dllSha256": isolated["dllSha256"],
            "compared": len(shared),
            "identical": identical,
            "beyondTolerance": {
                "peak": len(over_peak), "rms": len(over_rms),
                "band": len(over_band), "cents": len(over_cents),
            },
            "rows": rows,
        }, indent=1))
        print(f"\nWrote {arguments.json}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
