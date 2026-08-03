#!/usr/bin/env python3
"""Recover the insertion-effect (EFX) directory from SCCore.dll.

The 65 GS insertion effects are not anonymous in the file: the engine carries a directory that
names every one of them, keyed by the GS type number the SysEx block selects, and pointing at the
DSP algorithm and the parameter-apply handler that serve it. This reads that directory.

The record is 0x28 bytes at `g_fx_type_to_algo_map`, and the field Ghidra's symbol points at is not
the first one -- the symbol lands on the type key, 12 bytes into the record, so a naive dump reads
each effect's name against the *previous* effect's type. That off-by-one record is what made the
dispatch mapping look like a scramble with no names attached to it:

    +0x00  char  name[12]      display name, space padded
    +0x0C  u16   type          GS type key, (MSB << 8) | LSB -- what `40 03 00` selects
    +0x0E  u16   dispatch      index into `g_fx_algo_dispatch`, the per-algorithm DSP processor
    +0x10  u64   param_apply   per-effect handler mapping the 20 GS parameters to registers
    +0x18  u64   param_defaults  returns a block whose +0x0C holds the 0x1C-byte default parameters
    +0x20  u64   common        one shared handler, identical in all 66 records

There are 66 records: the 65 types the manual lists (00: Thru through 64: PH/AutoWah) plus a
`0xFFFF` record with a blank name and a null apply handler, which is the "no effect assigned"
state. Record 66 is not a record -- reading it returns noise, which is how the count is pinned.

The dispatch indices are a scramble of the type order and must be read from here rather than
inferred: Spectrum is type `01 01` but algorithm 6, Humanizer is type `01 03` but algorithm 46.
Algorithm 66 exists in the dispatch table and no record selects it.

Roland-derived output: generate locally, do not redistribute.

Usage:
    python3 tools/dump_efx_table.py [--dll SCCore.dll] [--json out.json]
"""

import argparse
import json
import os
import pathlib
import struct
import sys

# SOUND Canvas VA 1.1.6, the build every offset in this repository is pinned to.
IMAGE_BASE = 0x180000000
HEADER_SIZE = 0x1000

# `g_fx_type_to_algo_map`, at the start of the record rather than at the type key.
TYPE_MAP_VA = 0x181895660
TYPE_MAP_STRIDE = 0x28
TYPE_MAP_COUNT = 66

# `g_fx_algo_dispatch`: the function-pointer table of per-algorithm DSP processors.
DISPATCH_VA = 0x181895190
DISPATCH_COUNT = 67

# The record used for "no effect assigned"; it is a real record, not a terminator.
NO_EFFECT_KEY = 0xFFFF


def find_dll(explicit):
    """Resolves the DLL the way every front end in this repository resolves it."""
    candidates = [explicit, os.environ.get("TS_SCCORE_DLL"), "SCCore.dll"]
    for candidate in candidates:
        if candidate and pathlib.Path(candidate).is_file():
            return pathlib.Path(candidate)
    sys.exit("no SCCore.dll: pass --dll, set TS_SCCORE_DLL, or put one in the working directory")


def read_at(image, va, size):
    offset = va - IMAGE_BASE - HEADER_SIZE
    if offset < 0 or offset + size > len(image):
        sys.exit(f"address {va:#x} is outside this file -- is it the 1.1.6 build?")
    return image[offset : offset + size]


def read_directory(image):
    """Reads the 66 records, in file order."""
    entries = []
    for index in range(TYPE_MAP_COUNT):
        record = read_at(image, TYPE_MAP_VA + index * TYPE_MAP_STRIDE, TYPE_MAP_STRIDE)
        name = record[0:12].decode("latin1").rstrip()
        key, dispatch = struct.unpack("<HH", record[12:16])
        apply_fn, defaults_fn, common_fn = struct.unpack("<QQQ", record[16:40])
        entries.append(
            {
                "name": name,
                "type_msb": key >> 8,
                "type_lsb": key & 0xFF,
                "dispatch": dispatch,
                "param_apply": apply_fn,
                "param_defaults": defaults_fn,
                "common": common_fn,
                "assigned": key != NO_EFFECT_KEY,
            }
        )
    return entries


def check(entries, dispatch):
    """Fails loudly rather than emitting a table that only looks right.

    A wrong base address still produces 66 plausible-looking rows, so the shape is checked instead:
    the names have to be printable, the dispatch indices have to be in range and distinct, and the
    shared handler has to be shared.
    """
    seen = set()
    common = entries[0]["common"]
    for entry in entries:
        if not all(0x20 <= byte < 0x7F for byte in entry["name"].encode("latin1")):
            sys.exit(f"record {entry['dispatch']} has a name that is not text -- wrong base address")
        if not 0 <= entry["dispatch"] < len(dispatch):
            sys.exit(f"'{entry['name']}' dispatches to {entry['dispatch']}, outside the table")
        if entry["dispatch"] in seen:
            sys.exit(f"dispatch {entry['dispatch']} is claimed twice -- wrong stride")
        seen.add(entry["dispatch"])
        if entry["common"] != common:
            sys.exit("the shared handler is not shared -- wrong record layout")

    unreachable = sorted(set(range(len(dispatch))) - seen)
    if unreachable != [66]:
        sys.exit(f"expected algorithm 66 alone to be unreachable, got {unreachable}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dll", help="path to SCCore.dll")
    parser.add_argument("--json", help="write the table here instead of printing it")
    args = parser.parse_args()

    image = find_dll(args.dll).read_bytes()
    entries = read_directory(image)
    dispatch = list(struct.unpack(f"<{DISPATCH_COUNT}Q", read_at(image, DISPATCH_VA, DISPATCH_COUNT * 8)))
    check(entries, dispatch)

    for entry in entries:
        entry["algorithm"] = dispatch[entry["dispatch"]]

    if args.json:
        pathlib.Path(args.json).write_text(json.dumps(entries, indent=2) + "\n")
        return

    print("type   dispatch  algorithm  apply     name")
    for entry in sorted(entries, key=lambda e: (not e["assigned"], e["type_msb"], e["type_lsb"])):
        key = "--   " if entry["assigned"] is False else f"{entry['type_msb']:02X} {entry['type_lsb']:02X}"
        # The unassigned record's apply handler is null, which is the point of it.
        apply_fn = f"{entry['param_apply'] - IMAGE_BASE:6x}" if entry["param_apply"] else "  none"
        name = entry["name"] or "(no effect)"
        print(
            f"{key}     {entry['dispatch']:3d}    {entry['algorithm'] - IMAGE_BASE:7x}"
            f"    {apply_fn}    {name}"
        )
    print(f"\n{sum(e['assigned'] for e in entries)} assigned types, "
          f"{DISPATCH_COUNT} algorithms, 1 unreachable (66)")


if __name__ == "__main__":
    main()
