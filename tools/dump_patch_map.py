#!/usr/bin/env python3
"""Export the whole patch directory -- melodic maps, drum kits and tone attributes -- as JSON.

This is the *reference export*, not an oracle. `dump_patch_resolution.py` is deliberately a second,
independent implementation of the three-level lookup so that agreement between it and the C++ is
evidence; this tool has the opposite job, which is to state what the directory contains in a shape
other programs can consume. Keep them separate: making the oracle serve both roles would destroy
what makes its agreement mean anything.

What it emits, and why in this shape:

    tones        every defined melodic tone, keyed by tone number -- name, level, partial count,
                 and the two attributes the name suffixes advertise (see \\ref tone-names)
    melodic      per tone map, the slots a bank select can actually reach, as (bank, program)
                 -> tone number. Names are NOT repeated here; look them up in `tones`.
    alternates   the alternate-articulation table -- the `Name:` / `Name:L` legato pairs, whose
                 second member no program change reaches
    drums        per tone map, program -> kit index
    kits         every reachable kit, with its 128 key slots

Only *natively assigned* melodic slots are listed. A Sound Canvas sounds the bank-0 capital tone
when the selected variation has no entry for a program, so the reachable set is far larger than the
assigned one and expanding it would bury the actual data under its own fallback. The rule is
declared in `melodicResolution` instead. Same for the two compatibility banks, which redirect into
another map wholesale rather than holding entries of their own.

Usage:
    python3 tools/dump_patch_map.py <SCCore.dll> <output.json> [--manifest assets/manifest.json]

Roland-derived output: generate locally, do not redistribute.
"""

import argparse
import hashlib
import json
import pathlib
import sys

SCHEMA = "tabulasonora.patch-map/1"

TONE_STRIDE = 0x100
TONE_NAME_LENGTH = 12
TONE_HEADER_SIZE = 0x24
PARTIAL_STRIDE = 0x6E
PARTIAL_SLOTS = 2

# Partial block bytes that the tone-name suffixes correspond to. Both are established in
# docs/spec/tone-names.md; `+0x00 == 0xff` is what the engine tests to fire a key-off layer, and
# `+0x6b` is set on the `w` tones and on nothing else but the stereo strings.
PARTIAL_KEY_OFF = 0x00
PARTIAL_KEY_OFF_VALUE = 0xFF
PARTIAL_WIDE = 0x6B
PARTIAL_MULTISAMPLE = 0x02
NO_MULTISAMPLE = 0xFFFF

ALTERNATE_STRIDE = 0x18
ALTERNATE_NAME_LENGTH = 12
UNASSIGNED = 0xFFFF
INDIRECT_ONLY_FLAG = 0x8000
ALTERNATE_SPACE_START = 0x6000
DRUM_SPACE_START = 0x4000

# The module's own selector values, not labels invented here -- they match ts::ToneMap.
MAPS = {"sc55": 1, "sc88": 2, "sc88pro": 3, "sc8820": 4, "xg": 0x77}

# ts::DrumKitTable::row_for_map. A drum program change resolves through its own pair of lookups,
# and which row it reads is a property of the tone map rather than of the bank.
DRUM_ROWS = {"sc8820": 0, "sc88pro": 1, "sc88": 2, "sc55": 3, "xg": 4}
DRUM_MAP_ROW_COUNT = 6
DRUM_KIT_STRIDE = 0x50C
DRUM_KEY_COUNT = 128
DRUM_PLANES = {
    "tone": 0x000,  # u16 per key
    "level": 0x100,
    "pitch": 0x180,
    "group": 0x200,
    "pan": 0x280,
    "reverb": 0x300,
    "chorus": 0x380,
    "delay": 0x400,
    "receive": 0x480,
}
DRUM_NAME_PLANE = 0x500
DRUM_NAME_LENGTH = 12

# ts::DrumKitTable::kit_index_offset. Alone among the drum regions this one is not in the manifest.
KIT_INDEX_OFFSET = 0x19F21B0
KIT_INDEX_COUNT = 256

# The banks that redirect into another map instead of resolving in the current one. The shipped
# indirection tables make both keep the program and land on bank 0, which is why they can be stated
# as a rule here rather than expanded slot by slot.
INDIRECT_BANKS = {0x40: "sc88", 0x41: "sc55"}


def u16(data, offset):
    return data[offset] | (data[offset + 1] << 8)


def ascii_name(raw):
    return raw.decode("latin-1").rstrip(" \0")


class Directory:
    """The tables the patch directory is built from, sliced out of one verified DLL image."""

    def __init__(self, image, manifest):
        cached = {t["name"]: t for t in manifest["cached_tables"]}
        for key, name in (
            ("lut1", "lut1_2e30.bin"),
            ("lut2", "lut2_28b0.bin"),
            ("lut3", "lut3_32b0.bin"),
            ("tone", "tone_a.bin"),
            ("layered", "layered_1896690.bin"),
        ):
            entry = cached[name]
            offset = int(entry["file_offset"], 16)
            setattr(self, key, image[offset : offset + entry["size"]])

        # The drum regions carry 'va' rather than 'file_offset', but those values are already file
        # offsets -- ts::TableManifest reads them the same way.
        regions = {r["name"]: r for r in manifest["live_regions"]}

        def region(name):
            entry = regions[name]
            return int(entry.get("file_offset", entry.get("va")), 16)

        self.image = image
        self.kit_base = region("drum_kit_records")
        prog_map = region("drum_prog_map")
        self.drum_prog_map = image[prog_map : prog_map + DRUM_MAP_ROW_COUNT * 0x80]
        self.kit_index = [
            u16(image, KIT_INDEX_OFFSET + i * 2) for i in range(KIT_INDEX_COUNT)
        ]
        self.tone_count = len(self.tone) // TONE_STRIDE

    # -- melodic ---------------------------------------------------------------------------------

    def lut3_raw(self, program, map_index, bank):
        """The third level's word, unsigned. None when a level is undefined."""
        if not 0 <= map_index < len(self.lut1):
            return None
        level1 = self.lut1[map_index]
        if level1 == 0xFF:
            return None

        index2 = level1 * 0x80 + bank
        if index2 >= len(self.lut2) or self.lut2[index2] == 0xFF:
            return None

        index3 = self.lut2[index2] * 0x80 + program
        if index3 * 2 + 1 >= len(self.lut3):
            return None
        return u16(self.lut3, index3 * 2)

    def tone_record(self, number):
        base = number * TONE_STRIDE
        return self.tone[base : base + TONE_STRIDE]

    def tone_name(self, number):
        return ascii_name(self.tone_record(number)[:TONE_NAME_LENGTH])

    def partials(self, number):
        record = self.tone_record(number)
        blocks = []
        for slot in range(PARTIAL_SLOTS):
            start = TONE_HEADER_SIZE + slot * PARTIAL_STRIDE
            block = record[start : start + PARTIAL_STRIDE]
            if u16(block, PARTIAL_MULTISAMPLE) != NO_MULTISAMPLE:
                blocks.append(block)
        return blocks

    def alternate(self, index):
        record = self.layered[index * ALTERNATE_STRIDE : (index + 1) * ALTERNATE_STRIDE]
        if len(record) < ALTERNATE_STRIDE:
            return None
        return {
            "name": ascii_name(record[:ALTERNATE_NAME_LENGTH]),
            "threshold": record[0x0E],
            "primary": {"map": record[0x10], "bank": record[0x11], "program": record[0x12]},
            "alternate": {"map": record[0x14], "bank": record[0x15], "program": record[0x16]},
        }

    def dereference(self, reference):
        """Resolve an alternate entry's (map, bank, program) triple to a melodic tone number."""
        raw = self.lut3_raw(reference["program"], reference["map"], reference["bank"])
        if raw is None or raw == UNASSIGNED:
            return None
        tone = raw & 0x7FFF
        return tone if tone < DRUM_SPACE_START else None

    # -- drums -----------------------------------------------------------------------------------

    def kit_for_program(self, program, row):
        level2 = self.drum_prog_map[row * 0x80 + (program & 0x7F)]
        return None if level2 == 0xFF else self.kit_index[level2]

    def kit_record(self, kit):
        base = self.kit_base + kit * DRUM_KIT_STRIDE
        return self.image[base : base + DRUM_KIT_STRIDE]


def tone_entries(directory):
    """Every defined tone, with the two attributes the name suffixes advertise."""
    tones = {}
    for number in range(directory.tone_count):
        name = directory.tone_name(number)
        # Unused records read back short or empty; ts::Tone::is_defined applies the same rule.
        if len(name) < 2:
            continue
        blocks = directory.partials(number)
        entry = {
            "name": name,
            "level": directory.tone_record(number)[0x0C],
            "partials": len(blocks),
        }
        if any(b[PARTIAL_KEY_OFF] == PARTIAL_KEY_OFF_VALUE for b in blocks):
            entry["keyOff"] = True
        if any(b[PARTIAL_WIDE] for b in blocks):
            entry["wide"] = True
        tones[number] = entry
    return tones


def melodic_slots(directory, map_index):
    """The (bank, program) slots this map assigns natively, in bank-then-program order."""
    slots = []
    for bank in range(128):
        if bank in INDIRECT_BANKS:
            continue
        for program in range(128):
            raw = directory.lut3_raw(program, map_index, bank)
            # Bit 15 marks a tone reachable only through an alternate entry. It is not a missing
            # slot, but it is not one a program change selects either, so it is not listed.
            if raw is None or raw == UNASSIGNED or raw >= INDIRECT_ONLY_FLAG:
                continue

            slot = {"bank": bank, "program": program}
            if raw < ALTERNATE_SPACE_START:
                slot["tone"] = raw
                if raw >= DRUM_SPACE_START:
                    slot["space"] = "drum"
            else:
                index = raw - ALTERNATE_SPACE_START
                entry = directory.alternate(index)
                if entry is None:
                    continue
                tone = directory.dereference(entry["primary"])
                if tone is None:
                    continue
                slot["tone"] = tone
                slot["alternate"] = index
            slots.append(slot)
    return slots


def alternate_entries(directory):
    """The legato pairs. Deduplicated: the shipped table repeats its 25 records twice."""
    seen = {}
    entries = []
    count = len(directory.layered) // ALTERNATE_STRIDE
    for index in range(count):
        record = directory.alternate(index)
        if record is None or not record["name"]:
            continue
        primary = directory.dereference(record["primary"])
        alternate = directory.dereference(record["alternate"])
        if primary is None:
            continue
        key = (record["name"], primary, alternate)
        if key in seen:
            seen[key]["indices"].append(index)
            continue
        entry = {
            "indices": [index],
            "name": record["name"],
            "threshold": record["threshold"],
            "primaryTone": primary,
            "legatoTone": alternate,
            "reference": {
                "bank": record["primary"]["bank"],
                "program": record["primary"]["program"],
            },
        }
        seen[key] = entry
        entries.append(entry)
    return entries


def kit_entries(directory, reachable):
    kits = []
    for kit in sorted(reachable):
        record = directory.kit_record(kit)
        keys = []
        for note in range(DRUM_KEY_COUNT):
            tone = u16(record, DRUM_PLANES["tone"] + note * 2)
            if tone == UNASSIGNED:
                continue
            receive = record[DRUM_PLANES["receive"] + note]
            keys.append(
                {
                    "key": note,
                    "tone": tone,
                    "level": record[DRUM_PLANES["level"] + note],
                    "pitch": record[DRUM_PLANES["pitch"] + note],
                    "group": record[DRUM_PLANES["group"] + note],
                    "pan": record[DRUM_PLANES["pan"] + note],
                    "reverb": record[DRUM_PLANES["reverb"] + note],
                    "chorus": record[DRUM_PLANES["chorus"] + note],
                    "delay": record[DRUM_PLANES["delay"] + note],
                    "rxNoteOff": bool(receive & 1),
                }
            )
        kits.append(
            {
                "index": kit,
                "name": ascii_name(record[DRUM_NAME_PLANE : DRUM_NAME_PLANE + DRUM_NAME_LENGTH]),
                "keys": keys,
            }
        )
    return kits


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("dll", type=pathlib.Path)
    parser.add_argument("output", type=pathlib.Path)
    parser.add_argument("--manifest", type=pathlib.Path, default="assets/manifest.json")
    parser.add_argument(
        "--compact", action="store_true", help="one line per JSON value instead of indented"
    )
    arguments = parser.parse_args()

    manifest = json.loads(arguments.manifest.read_text(encoding="utf-8"))
    image = arguments.dll.read_bytes()

    expected = manifest["dll"]
    if len(image) != expected["size"]:
        sys.exit(f"{arguments.dll} is {len(image)} bytes; the pinned build is {expected['size']}.")
    digest = hashlib.sha256(image).hexdigest()
    if digest != expected["sha256"]:
        sys.exit(f"{arguments.dll} has SHA-256 {digest}; expected {expected['sha256']}.")

    directory = Directory(image, manifest)

    tones = tone_entries(directory)
    melodic = {}
    for name, index in MAPS.items():
        melodic[name] = melodic_slots(directory, index)
        print(f"{name:8} {len(melodic[name]):6} melodic slots")

    drums = {}
    reachable_kits = set()
    for name, row in DRUM_ROWS.items():
        programs = []
        for program in range(128):
            kit = directory.kit_for_program(program, row)
            if kit is None:
                continue
            programs.append({"program": program, "kit": kit})
            reachable_kits.add(kit)
        drums[name] = {"row": row, "programs": programs}
        print(f"{name:8} {len(programs):6} drum programs on row {row}")

    kits = kit_entries(directory, reachable_kits)
    alternates = alternate_entries(directory)
    print(f"{len(tones)} tones, {len(kits)} kits, {len(alternates)} legato pairs")

    document = {
        "_note": (
            "Patch map derived from a licensed SCCore.dll. Roland-derived: generate locally, "
            "do not redistribute."
        ),
        "schema": SCHEMA,
        "dllSha256": digest,
        "melodicResolution": {
            "chain": "LUT1[map] -> LUT2[l1*0x80 + bank] -> LUT3[l2*0x80 + program] -> tone",
            "listed": "natively assigned slots only",
            "bankFallback": (
                "a bank with no entry for a program sounds that map's bank 0 tone, so the "
                "reachable set is larger than the listed one"
            ),
            "indirectBanks": {str(k): v for k, v in INDIRECT_BANKS.items()},
            "indirectBanksNote": (
                "these two banks redirect wholesale into the named map's bank 0, keeping the "
                "program; they are not expanded here"
            ),
            "alternateSlots": (
                "a slot with an 'alternate' index resolves through the alternate-articulation "
                "table; 'tone' is its primary, and the legato member is in 'alternates'"
            ),
        },
        "toneAttributes": {
            "keyOff": "a partial fires on note-off instead of releasing (the `.o` suffix)",
            "wide": "the stereo-spread flag at partial +0x6b (the `w` suffix)",
        },
        "tones": {str(number): entry for number, entry in sorted(tones.items())},
        "melodic": melodic,
        "alternates": alternates,
        "drums": drums,
        "kits": kits,
    }

    arguments.output.parent.mkdir(parents=True, exist_ok=True)
    arguments.output.write_text(
        json.dumps(document, indent=None if arguments.compact else 1),
        encoding="utf-8",
    )
    print(f"Wrote {arguments.output}")


if __name__ == "__main__":
    main()
