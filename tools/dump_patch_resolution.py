#!/usr/bin/env python3
"""Resolve every (map, bank, program) slot and dump what it sounds.

A *differential* oracle for the patch directory, written directly against the reverse-engineering
notes rather than translated from the C++ so that agreement between the two is evidence.

The chain under test is three levels deep and carries three distinct tone spaces, which is the part
most likely to be got wrong:

    LUT1[map] -> l1                     (0xff means the map is undefined)
    LUT2[l1 * 0x80 + bank] -> l2        (0xff means the bank is undefined)
    LUT3[l2 * 0x80 + program] -> word   (0xffff means unassigned)

The word must be read UNSIGNED. Bit 15 marks a tone reachable only through an alternate-articulation
entry, not a missing one; reading it signed and bailing on the sign makes real patches disappear.
Words at or above 0x6000 index the alternate-articulation table, whose *primary* reference is what
an ordinary poly note sounds -- the secondary is a conditional articulation only mono/solo reaches.

A bank select picks a variation, and when the selected variation has no entry for a program a Sound
Canvas sounds the capital tone at bank 0 rather than falling silent. That fallback is reproduced.

Usage:
    python3 tools/dump_patch_resolution.py <SCCore.dll> <output.json> [--manifest assets/manifest.json]

Roland-derived output: generate locally, do not redistribute.
"""

import argparse
import hashlib
import json
import pathlib
import sys

TONE_STRIDE = 0x100
TONE_NAME_LENGTH = 12
ALTERNATE_STRIDE = 0x18
UNASSIGNED = 0xFFFF
INDIRECT_ONLY_FLAG = 0x8000
ALTERNATE_SPACE_START = 0x6000
MELODIC_SPACE_END = 0x4000
MAPS = {"Sc55": 1, "Sc88": 2, "Sc88Pro": 3, "Sc8820": 4}

# The two lookup banks that redirect into ANOTHER map rather than resolving in the current one --
# the SC-88 and SC-55 compatibility banks, which any map can reach. They are not the ordinary
# "unassigned bank falls back to bank 0" case, and treating them as such is wrong: from Sc55 bank
# 0x40 sounds the SC-88's tone, and from Sc88 bank 0x41 sounds the SC-55's.
#
# Taken from the module, not from the C++ implementation. `scdec <dll> map 0 <msb> 0` plays program
# 0 at bank MSB 0, 0x40 and 0x41 and reports the wave the live engine selected:
#
#     bank 0    -> 8008:656065
#     bank 0x40 -> 8004:877920
#     bank 0x41 -> 8005:223392
#
# Three distinct waves. The bank-0 fallback would have made all three identical.
# `program_resolve_tone` @180069200 tests the lookup bank before any of the three levels run, and
# these two send it through an indirection table indexed by program. That table is a *separate
# program map*: three 0x80-byte planes supplying the map, bank and program the lookup then uses, per
# program, rather than one fixed redirect for the whole bank.
#
# This used to be modelled as "bank 0x40 means map Sc88 bank 0, keeping the program", which is what
# the shipped data mostly amounts to -- but not everywhere, and the planes are what actually decide.
# Reading them keeps this resolver independent of the engine while still being driven by the DLL.
INDIRECT_BANK_TABLES = {0x40: "bank64", 0x41: "bank65"}
INDIRECT_PLANE = 0x80


def read_tables(image, manifest):
    """Slice the tables this resolver needs straight out of the DLL."""
    entries = {t["name"]: t for t in manifest["cached_tables"]}
    out = {}
    for key, name in (
        ("lut1", "lut1_2e30.bin"),
        ("lut2", "lut2_28b0.bin"),
        ("lut3", "lut3_32b0.bin"),
        ("tone", "tone_a.bin"),
        ("layered", "layered_1896690.bin"),
        ("bank64", "tone_indirect_bank64_a8bf0.bin"),
        ("bank65", "tone_indirect_bank65_a9800.bin"),
    ):
        entry = entries[name]
        offset = int(entry["file_offset"], 16)
        out[key] = image[offset : offset + entry["size"]]
    return out


def u16(data, index):
    return data[index * 2] | (data[index * 2 + 1] << 8)


def tone_name(tone_table, number):
    base = number * TONE_STRIDE
    raw = tone_table[base : base + TONE_NAME_LENGTH]
    return raw.decode("latin-1").rstrip(" \0")


def lut3_raw(tables, program, map_index, bank):
    lut1, lut2, lut3 = tables["lut1"], tables["lut2"], tables["lut3"]
    if map_index < 0 or map_index >= len(lut1):
        return None
    level1 = lut1[map_index]
    if level1 == 0xFF:
        return None

    index2 = level1 * 0x80 + bank
    if index2 >= len(lut2) or lut2[index2] == 0xFF:
        return None

    index3 = lut2[index2] * 0x80 + program
    if index3 * 2 + 1 >= len(lut3):
        return None
    return u16(lut3, index3)


def lut3_resolved(tables, program, map_index, bank):
    # A compatibility bank redirects through its own program map, whatever map the part is currently
    # on. Checked before the ordinary lookup because the current map's own entry for bank 0x40/0x41
    # is not what sounds.
    table_key = INDIRECT_BANK_TABLES.get(bank)
    if table_key is not None:
        table = tables[table_key]
        if program < 0 or program >= INDIRECT_PLANE or len(table) < 3 * INDIRECT_PLANE:
            return None
        substituted_bank = table[INDIRECT_PLANE + program]
        # The shipped planes substitute bank 0, so this resolves in one step. Guard against data
        # that says otherwise rather than against anything observed.
        if substituted_bank in INDIRECT_BANK_TABLES:
            return None
        return lut3_resolved(tables,
                             table[2 * INDIRECT_PLANE + program],
                             table[program],
                             substituted_bank)

    raw = lut3_raw(tables, program, map_index, bank)
    if bank != 0 and (raw is None or raw == UNASSIGNED):
        raw = lut3_raw(tables, program, map_index, 0)
    return raw


def dereference(tables, map_index, bank, program):
    raw = lut3_raw(tables, program, map_index, bank)
    if raw is None or raw == UNASSIGNED:
        return -1
    tone = raw & 0x7FFF
    return tone if tone < MELODIC_SPACE_END else -1


def program_to_tone(tables, program, map_index, bank):
    raw = lut3_resolved(tables, program, map_index, bank)
    if raw is None or raw == UNASSIGNED or raw >= INDIRECT_ONLY_FLAG:
        return -1
    if raw < ALTERNATE_SPACE_START:
        return raw

    index = raw - ALTERNATE_SPACE_START
    layered = tables["layered"]
    if index < 0 or index >= len(layered) // ALTERNATE_STRIDE:
        return -1
    record = layered[index * ALTERNATE_STRIDE : (index + 1) * ALTERNATE_STRIDE]
    return dereference(tables, record[0x10], record[0x11], record[0x12])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("dll", type=pathlib.Path)
    parser.add_argument("output", type=pathlib.Path)
    parser.add_argument("--manifest", type=pathlib.Path, default="assets/manifest.json")
    arguments = parser.parse_args()

    manifest = json.loads(arguments.manifest.read_text(encoding="utf-8"))
    image = arguments.dll.read_bytes()

    expected = manifest["dll"]
    if len(image) != expected["size"]:
        sys.exit(f"{arguments.dll} is {len(image)} bytes; the pinned build is {expected['size']}.")
    digest = hashlib.sha256(image).hexdigest()
    if digest != expected["sha256"]:
        sys.exit(f"{arguments.dll} has SHA-256 {digest}; expected {expected['sha256']}.")

    tables = read_tables(image, manifest)

    maps = {}
    for name, index in MAPS.items():
        slots = []
        banks_with_native = set()
        for bank in range(128):
            for program in range(128):
                tone = program_to_tone(tables, program, index, bank)
                raw = lut3_raw(tables, program, index, bank)
                native = raw is not None and raw != UNASSIGNED and raw < INDIRECT_ONLY_FLAG
                if native:
                    banks_with_native.add(bank)
                if tone >= 0:
                    slots.append(
                        {
                            "bank": bank,
                            "program": program,
                            "tone": tone,
                            "name": tone_name(tables["tone"], tone),
                        }
                    )
        maps[name] = {
            "sounding": len(slots),
            "banksWithNative": len(banks_with_native),
            "slots": slots,
        }
        print(f"{name:8} {len(slots):6} sounding slots, {len(banks_with_native):3} banks")

    arguments.output.parent.mkdir(parents=True, exist_ok=True)
    arguments.output.write_text(
        json.dumps(
            {
                "_note": (
                    "Patch resolution derived from a licensed SCCore.dll. Roland-derived: "
                    "generate locally, do not redistribute."
                ),
                "dllSha256": digest,
                "maps": maps,
            },
            indent=1,
        ),
        encoding="utf-8",
    )
    print(f"Wrote {arguments.output}")


if __name__ == "__main__":
    main()
