#!/usr/bin/env python3
"""Find MIDI files worth putting in the oracle corpus, in a large archive.

The problem this solves: a fidelity gate is only as good as what its corpus exercises, and picking
songs by ear picks songs that sound nice rather than songs that drive the engine hard. Run over a
128,000-file archive, this found 288 files that genuinely drive the GS control matrix -- between
them assigning all eleven destinations from all six sources -- and ten of those cover every route
any of them use.

Three steps, because the cheap one is cheap enough to run over everything:

  * `scan` walks the archive looking for the byte pattern of a Roland GS DT1 message and reads the
    three address bytes after it. No MIDI parsing, so it is I/O bound rather than CPU bound.
  * `describe` parses the shortlist properly and reports what each file would actually drive:
    programs, controllers, aftertouch, ports, duration, and which matrix routes it assigns. A byte
    pattern cannot tell a SysEx from note data that happens to look like one, and cannot see
    channel messages at all; this can.
  * `choose` runs a greedy set cover over those features and prints the smallest set of files that
    exercises everything the shortlist does.

Usage:
    python3 tools/scan_midi_archive.py scan <archive dir> <scan.jsonl> [workers]
    python3 tools/scan_midi_archive.py describe <scan.jsonl> <described.jsonl>
    python3 tools/scan_midi_archive.py choose <described.jsonl> [count]

Neither the archive nor the files this picks are anyone's to redistribute, and `testdata/` is
gitignored. What is worth keeping in the repository is the method.
"""

import concurrent.futures
import json
import os
import pathlib
import re
import struct
import sys
from collections import Counter, defaultdict

DESTINATIONS = ["pitch", "tvf_cutoff", "amplitude", "lfo1_rate", "lfo1_pitch", "lfo1_tvf",
                "lfo1_tva", "lfo2_rate", "lfo2_pitch", "lfo2_tvf", "lfo2_tva"]
SOURCES = ["modulation", "bend", "chan_press", "poly_press", "cc1", "cc2"]


# ---------------------------------------------------------------------------------------------
# Pass one: byte scan
# ---------------------------------------------------------------------------------------------

def scan(path):
    """The address bytes of every GS DT1 in one file, or None if it carries none."""
    try:
        data = pathlib.Path(path).read_bytes()
    except OSError:
        return None
    if not data.startswith(b"MThd"):
        return None

    addresses = set()
    at = data.find(b"\x42\x12")
    while at >= 0:
        # A GS DT1 is `F0 41 <device> 42 12 <a1 a2 a3> ...`. The model and command bytes are the
        # distinctive pair; the manufacturer byte two back confirms it is Roland.
        if at >= 2 and data[at - 2] == 0x41 and at + 5 <= len(data):
            addresses.add(data[at + 2:at + 5].hex())
        at = data.find(b"\x42\x12", at + 1)

    if not addresses:
        return None

    return {
        "path": str(path),
        "size": len(data),
        "tracks": data.count(b"MTrk"),
        "ports": sorted({data[i + 3] for i in range(len(data) - 3)
                         if data[i] == 0xFF and data[i + 1] == 0x21 and data[i + 2] == 0x01}),
        "addresses": sorted(addresses),
    }


# ---------------------------------------------------------------------------------------------
# Pass two: parse
# ---------------------------------------------------------------------------------------------

def varlen(data, at):
    value = 0
    while True:
        byte = data[at]
        at += 1
        value = (value << 7) | (byte & 0x7F)
        if not byte & 0x80:
            return value, at


def parse(path):
    """What one file would actually drive, by parsing it."""
    data = pathlib.Path(path).read_bytes()
    if not data.startswith(b"MThd") or len(data) < 14:
        return None
    _, _, division = struct.unpack(">HHH", data[8:14])
    if division & 0x8000:
        return None  # SMPTE timing, not worth special-casing to pick a corpus

    facts = {
        "ticks": 0, "notes": 0, "poly_at": 0, "chan_at": 0, "bend": 0,
        "programs": set(), "ccs": Counter(), "matrix": defaultdict(set),
        "gs_reset": False, "ports": set(), "drum_setup": False, "efx": False, "eq": False,
        "tempos": [],
    }

    at = 14
    while at + 8 <= len(data):
        chunk, length = data[at:at + 4], struct.unpack(">I", data[at + 4:at + 8])[0]
        body = data[at + 8:at + 8 + length]
        at += 8 + length
        if chunk != b"MTrk":
            continue

        pos, now, status = 0, 0, 0
        while pos < len(body):
            try:
                delta, pos = varlen(body, pos)
            except IndexError:
                break
            now += delta
            if pos >= len(body):
                break
            byte = body[pos]
            if byte & 0x80:
                status = byte
                pos += 1

            if status == 0xFF:
                if pos >= len(body):
                    break
                kind = body[pos]
                pos += 1
                size, pos = varlen(body, pos)
                payload = body[pos:pos + size]
                pos += size
                if kind == 0x51 and len(payload) == 3:
                    facts["tempos"].append(
                        (now, (payload[0] << 16) | (payload[1] << 8) | payload[2]))
                elif kind == 0x21 and len(payload) == 1:
                    facts["ports"].add(payload[0])
                elif kind == 0x2F:
                    break
            elif status in (0xF0, 0xF7):
                size, pos = varlen(body, pos)
                payload = body[pos:pos + size]
                pos += size
                if len(payload) >= 8 and payload[0] == 0x41 and payload[2:4] == b"\x42\x12":
                    a1, a2, a3, value = payload[4], payload[5], payload[6], payload[7]
                    if (a1, a2, a3) == (0x40, 0x00, 0x7F):
                        facts["gs_reset"] = True
                    elif a1 == 0x40 and (a2 & 0xF0) == 0x20:
                        source, destination = a3 >> 4, a3 & 0x0F
                        if source < len(SOURCES) and destination < len(DESTINATIONS):
                            facts["matrix"][(source, destination)].add(value)
                    elif a1 == 0x40 and (a2 & 0xF0) == 0x40 and a3 == 0x20:
                        facts["eq"] = True
                    elif a1 in (0x41, 0x51):
                        facts["drum_setup"] = True
                    elif a1 == 0x40 and a2 == 0x03:
                        facts["efx"] = True
            else:
                high = status & 0xF0
                if high in (0x80, 0x90, 0xA0, 0xB0, 0xE0):
                    if pos + 2 > len(body):
                        break
                    d1, d2 = body[pos], body[pos + 1]
                    pos += 2
                    if high == 0x90 and d2 > 0:
                        facts["notes"] += 1
                    elif high == 0xA0:
                        facts["poly_at"] += 1
                    elif high == 0xB0:
                        facts["ccs"][d1] += 1
                    elif high == 0xE0:
                        facts["bend"] += 1
                elif high in (0xC0, 0xD0):
                    if pos + 1 > len(body):
                        break
                    if high == 0xC0:
                        facts["programs"].add(body[pos])
                    else:
                        facts["chan_at"] += 1
                    pos += 1
                else:
                    pos += 1
            facts["ticks"] = max(facts["ticks"], now)

    # Duration from the tempo map, which is what makes a length comparable between files.
    tempos = sorted(facts["tempos"]) or [(0, 500000)]
    seconds = 0.0
    last_tick, last_tempo = 0, tempos[0][1]
    for tick, tempo in tempos:
        seconds += (tick - last_tick) * last_tempo / 1e6 / division
        last_tick, last_tempo = tick, tempo
    seconds += (facts["ticks"] - last_tick) * last_tempo / 1e6 / division

    return {
        "path": str(path),
        "seconds": round(seconds, 1),
        "notes": facts["notes"],
        "programs": sorted(facts["programs"]),
        "ports": sorted(facts["ports"]),
        "poly_at": facts["poly_at"],
        "chan_at": facts["chan_at"],
        "bend": facts["bend"],
        "cc1": facts["ccs"].get(1, 0),
        "sound_controllers": sum(facts["ccs"].get(n, 0) for n in range(70, 80)),
        "gs_reset": facts["gs_reset"],
        "drum_setup": facts["drum_setup"],
        "eq": facts["eq"],
        "efx": facts["efx"],
        "matrix": {f"{SOURCES[s]}->{DESTINATIONS[d]}": sorted(v)
                   for (s, d), v in sorted(facts["matrix"].items())},
    }


# ---------------------------------------------------------------------------------------------
# Selection
# ---------------------------------------------------------------------------------------------

def features(entry):
    """What a file is worth having in the corpus for."""
    found = set(entry["matrix"])
    if entry["poly_at"] > 50:
        found.add("*poly_aftertouch")
    if entry["chan_at"] > 50:
        found.add("*channel_aftertouch")
    if len(entry["ports"]) > 1:
        found.add("*multiport")
    if entry["eq"]:
        found.add("*eq")
    if entry["drum_setup"]:
        found.add("*drum_setup")
    if entry["efx"]:
        found.add("*efx")
    if entry["sound_controllers"] > 20:
        found.add("*sound_controllers")
    if entry["cc1"] > 50:
        found.add("*mod_wheel")
    if len(entry["programs"]) >= 12:
        found.add("*many_programs")
    return found


def command_scan(argv):
    root, out = pathlib.Path(argv[0]), pathlib.Path(argv[1])
    workers = int(argv[2]) if len(argv) > 2 else max(1, (os.cpu_count() or 4) - 2)

    files = []
    for base, _, names in os.walk(root):
        if "__MACOSX" in base:
            continue
        for name in names:
            if name.lower().endswith((".mid", ".midi")) and not name.startswith("._"):
                files.append(os.path.join(base, name))
    print(f"{len(files)} files under {root}", flush=True)

    kept = 0
    with out.open("w", encoding="utf-8") as sink:
        with concurrent.futures.ProcessPoolExecutor(max_workers=workers) as pool:
            for index, result in enumerate(pool.map(scan, files, chunksize=256)):
                if index and index % 20000 == 0:
                    print(f"  {index}/{len(files)}  kept {kept}", flush=True)
                if result is not None:
                    kept += 1
                    json.dump(result, sink)
                    sink.write("\n")
    print(f"{kept} carry GS SysEx")


def command_describe(argv):
    rows = [json.loads(line) for line in open(argv[0], encoding="utf-8") if line.strip()]
    kept = []
    for row in rows:
        try:
            parsed = parse(row["path"])
        except Exception:
            continue
        if parsed:
            kept.append(parsed)

    with pathlib.Path(argv[1]).open("w", encoding="utf-8") as sink:
        for entry in kept:
            json.dump(entry, sink)
            sink.write("\n")
    print(f"described {len(kept)} files")

    coverage = Counter()
    for entry in kept:
        for key in features(entry):
            coverage[key] += 1
    for key, count in coverage.most_common():
        print(f"  {key:<30} {count}")


def command_choose(argv):
    rows = [json.loads(line) for line in open(argv[0], encoding="utf-8") if line.strip()]
    limit = int(argv[1]) if len(argv) > 1 else 12

    # Too short exercises nothing; too long costs the gate minutes of oracle render; and a file
    # with no GS reset starts from whatever state the module was left in.
    usable = [r for r in rows if 20 <= r["seconds"] <= 360 and r["notes"] >= 200 and r["gs_reset"]]
    universe = set()
    for entry in usable:
        universe |= features(entry)
    print(f"{len(usable)} usable files, {len(universe)} distinct features")

    chosen, covered = [], set()
    while len(chosen) < limit:
        best, gain = None, 0
        for entry in usable:
            if entry in chosen:
                continue
            score = len(features(entry) - covered)
            # Ties go to the shorter file: the gate pays for every second of oracle render.
            if score > gain or (score == gain and score > 0 and best
                                and entry["seconds"] < best["seconds"]):
                best, gain = entry, score
        if best is None or gain == 0:
            break
        chosen.append(best)
        covered |= features(best)

    print(f"{len(chosen)} files cover {len(covered)}/{len(universe)} features")
    for entry in chosen:
        stem = re.sub(r"[^a-z0-9]+", "_", pathlib.Path(entry["path"]).stem.lower()).strip("_")[:28]
        print(f"  {stem:<30} {entry['seconds']:>6.0f}s {entry['notes']:>6} notes "
              f"{len(entry['programs']):>3} programs")
        print(f"      {entry['path']}")
    missing = universe - covered
    if missing:
        print(f"not covered: {sorted(missing)}")


def main():
    commands = {"scan": command_scan, "describe": command_describe, "choose": command_choose}
    if len(sys.argv) < 2 or sys.argv[1] not in commands:
        sys.exit(__doc__)
    commands[sys.argv[1]](sys.argv[2:])


if __name__ == "__main__":
    main()
