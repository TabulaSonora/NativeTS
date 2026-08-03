#!/usr/bin/env python3
"""Dump the codec's predictor stream for a spread of real ROM waves.

This is a *differential* oracle, and its value comes from being an independent implementation of
the same documented formula rather than a translation of the C++ one. It is deliberately written
in plain Python against the specification in the reverse-engineering notes:

    predictor += (int8)delta[i] << (scale(i) + 10)          -- a wrapping 32-bit accumulator
    scale(i)   = nibble (i>>4)&1 of byte i>>5 of the scale stream

Two things it does NOT copy from the reference implementation in the spec repo, because the
hardware disagrees with it there and this engine follows the hardware:

  * the reference integrates in Python's arbitrary-precision integers; the engine's accumulator is
    32 bits wide and wraps, which traces of the DLL's own predictor field confirm.
  * the reference stops one sample short of the data end; the forward loop is inclusive of that
    index, so it is decoded here.

Usage:
    python3 tools/dump_predictors.py <SCCore.dll> <output.json> [--manifest assets/manifest.json]

Nothing Roland-derived is written: the output holds predictor values derived from the wave ROM, so
it is treated as ROM data and is gitignored like the tables.
"""

import argparse
import hashlib
import json
import pathlib
import sys


def load_manifest(path):
    with open(path, "r", encoding="utf-8") as handle:
        return json.load(handle)


def region_base(manifest, region):
    """File offset of a descriptor region byte's 1 MB region."""
    regions = {r["name"]: r for r in manifest["live_regions"]}
    bank = (region >> 4) & 1
    name = "wave_rom_bank_A" if bank == 0 else "wave_rom_bank_B"
    base = int(regions[name]["file_offset"], 16)
    return base + (region - 16 * bank) * 0x100000


def scale_at(scale, position):
    packed = scale[position >> 5]
    return packed & 0x0F if ((position >> 4) & 1) == 0 else (packed >> 4) & 0x0F


def wrap32(value):
    """Truncate to a signed 32-bit accumulator, which is what the engine's field is."""
    value &= 0xFFFFFFFF
    return value - 0x100000000 if value >= 0x80000000 else value


def decode_predictors(image, region_offset, loop, start, manifest):
    # The data begins at `loop` exactly. Two in five descriptors put it partway into a 32-sample
    # exponent block, which is legal: the codec stores no absolute value per block, only
    # differences, so a wave may start and end mid-block. What that costs is that the exponents
    # must be indexed by the ABSOLUTE sample position, hence the phase carried below. Rounding the
    # start down instead would begin integrating early, and with no leak in the predictor and no DC
    # blocker downstream, those extra deltas displace the wave for its whole length.
    count = start - loop
    if count <= 0 or count > 2_000_000:
        return None

    base = region_offset
    phase = loop & 0x1F
    delta = image[base + loop : base + loop + count + 1]
    scale_length = ((phase + count + 1) >> 5) + 4
    scale = image[base + (loop >> 5) : base + (loop >> 5) + scale_length]

    if len(delta) < count + 1 or len(scale) < scale_length:
        return None

    predictors = []
    predictor = 0
    for i in range(count + 1):
        signed = delta[i] - 256 if delta[i] >= 128 else delta[i]
        predictor = wrap32(predictor + wrap32(signed << (scale_at(scale, phase + i) + 10)))
        predictors.append(predictor)
    return predictors


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("dll", type=pathlib.Path, help="path to SCCore.dll")
    parser.add_argument("output", type=pathlib.Path, help="where to write the fixture")
    parser.add_argument("--manifest", type=pathlib.Path, default="assets/manifest.json")
    arguments = parser.parse_args()

    manifest = load_manifest(arguments.manifest)
    image = arguments.dll.read_bytes()

    expected = manifest["dll"]
    if len(image) != expected["size"]:
        sys.exit(f"{arguments.dll} is {len(image)} bytes; the pinned build is {expected['size']}.")
    digest = hashlib.sha256(image).hexdigest()
    if digest != expected["sha256"]:
        sys.exit(f"{arguments.dll} has SHA-256 {digest}; the pinned build is {expected['sha256']}.")

    # Read the descriptor table out of the DLL and sample across it, rather than hand-picking
    # waves: a fixed spread would be free to miss whatever the port gets wrong.
    tables = {t["name"]: t for t in manifest["cached_tables"]}
    descriptor_entry = tables["wavedesc_a.bin"]
    descriptor_offset = int(descriptor_entry["file_offset"], 16)
    descriptor_bytes = image[descriptor_offset : descriptor_offset + descriptor_entry["size"]]

    stride = 0x16
    cases = []
    seen = set()
    for index in range(0, len(descriptor_bytes) // stride):
        record = descriptor_bytes[index * stride : (index + 1) * stride]
        region = record[0x00] & 0x7F
        loop = ((record[0x01] & 0x0F) << 16) | (record[0x02] << 8) | record[0x03]
        start = ((record[0x0B] & 0x0F) << 16) | (record[0x0C] << 8) | record[0x0D]
        flags = record[0x0A]

        key = (region, loop, start)
        if key in seen:
            continue

        offset = region_base(manifest, region)
        predictors = decode_predictors(image, offset, loop, start, manifest)
        if predictors is None:
            continue
        seen.add(key)

        # The whole stream for a 100k-sample wave would be a huge fixture, so record a hash of the
        # full stream plus a literal prefix and suffix. The hash is what makes it a real check;
        # the literals are what make a failure diagnosable.
        stream = b"".join(int(p & 0xFFFFFFFF).to_bytes(4, "little") for p in predictors)
        cases.append(
            {
                "wave": index,
                "region": region,
                "loop": loop,
                "start": start,
                "flags": flags,
                "sampleCount": start - loop,
                "predictorSha256": hashlib.sha256(stream).hexdigest(),
                "first16": predictors[:16],
                "last16": predictors[-16:],
            }
        )

    arguments.output.parent.mkdir(parents=True, exist_ok=True)
    with open(arguments.output, "w", encoding="utf-8") as handle:
        json.dump(
            {
                "_note": (
                    "Predictor streams derived from a licensed SCCore.dll. Roland-derived: "
                    "generate locally, do not redistribute."
                ),
                "dllSha256": digest,
                "cases": cases,
            },
            handle,
            indent=1,
        )

    total = sum(case["sampleCount"] for case in cases)
    print(f"Wrote {len(cases)} distinct waves ({total:,} samples) to {arguments.output}")


if __name__ == "__main__":
    main()
