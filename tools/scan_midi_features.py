#!/usr/bin/env python3
"""Tag every file in a MIDI archive with the engine feature areas it exercises.

`scan_midi_archive.py` answers one narrow question -- which files drive the GS control matrix.
This answers the broader one: for each feature area the engine implements (or should), which
files in a large archive actually use it, so a handful can be copied out as local test material.
Picking by feature beats picking by ear: a corpus chosen for how it sounds exercises the sampler
and little else.

Two steps:

  * `scan` parses every file properly -- tracks, tempo map, running status -- and emits one JSON
    row per file tagging the feature areas it touches, with enough detail (which keys, which
    SysEx parameters, how long a hold) to rank exemplars later.
  * `report` aggregates the rows, documents each feature area with counts, and prints the best
    few exemplar files per area -- preferring small files that use the feature heavily, since a
    focused two-minute file makes a better regression asset than a ten-minute medley that
    touches the feature once.

Feature areas tagged:

  ports        FF 21 numbers, FF 09 device names, FF 04 instrument names; multiport by each
               scheme, and files carrying more than one scheme (which test the preference order:
               FF 21 over FF 09 over FF 04).
  drums        Long holds (>2 s) on rhythm-channel keys whose kit records mark Rx Note Off ON --
               the keys a fixed ring timer would cut short (snare roll 25, applause 88, the SFX
               kits); whether rhythm notes receive note-offs at all; NRPN drum overrides
               (18/1A/1C-1F); drum-setup SysEx parameters, especially 07/08, the per-key
               Rx Note Off / Rx Note On toggles.
  sysex        Resets (GM, GM2, GS, XG); GS master, part, effects (reverb/chorus/delay/EQ/EFX),
               scale tuning, display messages; parts mapped to rhythm via `40 1x 15`.
  channel      Bank selects, poly and channel pressure, portamento, sostenuto/soft pedals,
               send controllers, mono mode, RPN pitch-bend range beyond the +/-2 default.
  timing       SMPTE division and format 2, which the reader currently refuses -- flagged so the
               day either is supported there is material to test with.

Usage:
    python3 tools/scan_midi_features.py scan <archive dir> <features.jsonl> [workers]
    python3 tools/scan_midi_features.py report <features.jsonl> [picks per area]

Neither the archive nor the files this picks are anyone's to redistribute, and `testdata/` is
gitignored. What is worth keeping in the repository is the method.
"""

import concurrent.futures
import json
import os
import pathlib
import struct
import sys
from collections import defaultdict

# Rhythm-channel keys marked Rx Note Off ON on a looping wave somewhere in the kit records --
# the keys that genuinely sustain while held and release on note-off. Read from the ROM by the
# drum survey (kits reachable through the program map, all velocities); the standard-kit members
# are 25 (snare roll) and 88 (applause), and the rest are largely the SFX kits.
RX_SUSTAIN_KEYS = frozenset({
    25, 28, 29, 36, 41, 42, 52, 54, 56, 57, 58, 60, 61, 62, 63, 64, 65, 66, 67, 68,
    70, 71, 72, 73, 78, 79, 80, 81, 82, 83, 84, 85, 86, 87, 88, 89, 90, 91, 92, 94,
    96, 97, 98, 103, 104, 105, 107, 108,
})

# A hold longer than this on one of those keys is what a fixed ring window would truncate.
LONG_HOLD_SECONDS = 2.0

# NRPN MSBs of the per-key drum overrides (coarse pitch, level, pan, reverb, chorus, delay).
DRUM_NRPN_MSBS = frozenset({0x18, 0x1A, 0x1C, 0x1D, 0x1E, 0x1F})


# ---------------------------------------------------------------------------------------------
# One file
# ---------------------------------------------------------------------------------------------

def _varlen(data, at):
    value = 0
    while True:
        byte = data[at]
        at += 1
        value = (value << 7) | (byte & 0x7F)
        if not byte & 0x80:
            return value, at


def _parse_tracks(data):
    """Yields per-track event lists of (tick, kind, payload...) without merging."""
    head = data.find(b"MThd")
    if head < 0 or len(data) < head + 14:
        return None
    fmt, ntrk, division = struct.unpack(">HHH", data[head + 8 : head + 14])
    tracks = []
    at = head + 14
    for _ in range(ntrk):
        if at + 8 > len(data) or data[at : at + 4] != b"MTrk":
            break
        length = struct.unpack(">I", data[at + 4 : at + 8])[0]
        track = data[at + 8 : at + 8 + length]
        at += 8 + length
        events = []
        i = 0
        tick = 0
        status = 0
        try:
            while i < len(track):
                delta, i = _varlen(track, i)
                tick += delta
                byte = track[i]
                if byte & 0x80:
                    i += 1
                    if byte < 0xF0:
                        status = byte
                else:
                    byte = status
                    if byte == 0:
                        break
                if byte == 0xFF:
                    meta = track[i]
                    i += 1
                    length, i = _varlen(track, i)
                    events.append((tick, "meta", meta, bytes(track[i : i + length])))
                    i += length
                elif byte in (0xF0, 0xF7):
                    length, i = _varlen(track, i)
                    if byte == 0xF0:
                        events.append((tick, "sysex", bytes(track[i : i + length])))
                    i += length
                else:
                    kind = byte & 0xF0
                    if kind in (0xC0, 0xD0):
                        events.append((tick, "channel", byte, track[i], 0))
                        i += 1
                    else:
                        events.append((tick, "channel", byte, track[i], track[i + 1]))
                        i += 2
        except IndexError:
            pass  # a truncated track keeps what it yielded
        tracks.append(events)
    return fmt, division, tracks


def _tick_clock(tempi, division):
    """Returns tick -> seconds over a sorted (tick, tempo) map."""
    def seconds_at(tick):
        seconds = 0.0
        last_tick = 0
        tempo = 500000
        for at, next_tempo in tempi:
            if at >= tick:
                break
            seconds += (at - last_tick) * tempo / 1e6 / division
            last_tick = at
            tempo = next_tempo
        return seconds + (tick - last_tick) * tempo / 1e6 / division
    return seconds_at


def describe(path):
    """The feature areas one file touches, or None where it is not a usable SMF.

    Never raises: one malformed file in a hundred thousand must not take the whole pool down,
    and an archive this size has every kind of malformed.
    """
    try:
        return _describe(path)
    except Exception:
        return None


def _describe(path):
    try:
        data = pathlib.Path(path).read_bytes()
    except OSError:
        return None
    if len(data) > 8 << 20 or b"MThd" not in data[:1024]:
        return None
    parsed = _parse_tracks(data)
    if parsed is None:
        return None
    fmt, division, tracks = parsed
    if not tracks:
        return None

    row = {"path": str(path), "size": len(data), "format": fmt}
    features = {}

    if division & 0x8000:
        features["smpte_division"] = True
        row["features"] = features
        return row  # ticks are frames; the second pass below would misread everything
    if fmt == 2:
        features["format2"] = True

    # Timing first, so drum holds can be measured in seconds.
    tempi = sorted(
        (tick, (payload[0] << 16) | (payload[1] << 8) | payload[2])
        for events in tracks
        for tick, kind, *rest in events
        if kind == "meta" and rest[0] == 0x51 and len(rest[1]) >= 3
        for payload in (rest[1],)
    )
    clock = _tick_clock(tempi, division)

    port_numbers = set()
    device_names = set()
    instrument_names = set()
    rhythm_channels = {9, 25, 41, 57}  # channel 9 of each possible port
    melodic_ch10 = set()
    resets = set()
    drum_setup_params = set()
    drum_nrpns = set()
    gs_families = set()
    controllers = set()
    banks = set()
    last_tick = 0

    # Pass one, SysEx only: rhythm-part mapping has to be known before notes are judged.
    for events in tracks:
        for tick, kind, *rest in events:
            if kind != "sysex":
                continue
            payload = rest[0]
            if payload[:4] == b"\x7e\x7f\x09\x01":
                resets.add("gm")
            elif payload[:4] == b"\x7e\x7f\x09\x03":
                resets.add("gm2")
            elif len(payload) >= 8 and payload[0] == 0x43 and payload[2] == 0x4C:
                if payload[3:6] == b"\x00\x00\x7e":
                    resets.add("xg")
                gs_families.add("xg_native")
            if len(payload) < 8 or payload[0] != 0x41 or payload[2] != 0x42 or payload[3] != 0x12:
                continue
            a1, a2, a3 = payload[4], payload[5], payload[6]
            if (a1, a2, a3) == (0x40, 0x00, 0x7F):
                resets.add("gs")
            elif a1 == 0x40 and a2 == 0x00:
                gs_families.add("master")
            elif a1 == 0x40 and a2 == 0x01:
                gs_families.add("effects")
            elif a1 == 0x40 and a2 == 0x02:
                gs_families.add("eq")
            elif a1 == 0x40 and a2 == 0x03:
                gs_families.add("efx")
            elif a1 == 0x40 and 0x10 <= a2 <= 0x1F:
                block = a2 & 0x0F
                channel = 9 if block == 0 else (block - 1 if block <= 9 else block)
                if a3 == 0x15 and len(payload) > 7:
                    if payload[7] > 0:
                        rhythm_channels.add(channel)
                        if channel != 9:
                            gs_families.add("extra_rhythm_part")
                    elif channel == 9:
                        melodic_ch10.add(channel)
                        gs_families.add("melodic_ch10")
                elif 0x40 <= a3 <= 0x4B:
                    gs_families.add("scale_tuning")
                else:
                    gs_families.add("part")
            elif a1 == 0x41:
                # Drum setup: 41 <map+key group> -- parameter is a2's low nibble family; the GS
                # address is 41 mp kk with p the parameter, so a2 carries map<<4 | parameter.
                drum_setup_params.add(a2 & 0x0F)
            elif a1 == 0x10:
                gs_families.add("display")

    # Pass two, channel traffic: holds, note-offs, NRPNs, controllers.
    open_drum_notes = {}
    long_holds = {}
    drum_note_ons = 0
    drum_note_offs = 0
    nrpn_msb = defaultdict(int)
    for events in tracks:
        for event in events:
            tick = event[0]
            last_tick = max(last_tick, tick)
            kind = event[1]
            if kind == "meta":
                meta, payload = event[2], event[3]
                if meta == 0x21 and payload:
                    port_numbers.add(payload[0])
                elif meta == 0x09 and payload:
                    device_names.add(bytes(payload))
                elif meta == 0x04 and payload:
                    instrument_names.add(bytes(payload))
                continue
            if kind != "channel":
                continue
            status, d1, d2 = event[2], event[3], event[4]
            channel = status & 0x0F
            message = status & 0xF0
            drums = channel in rhythm_channels and channel not in melodic_ch10
            if message == 0x90 and d2 > 0:
                if drums:
                    drum_note_ons += 1
                    open_drum_notes[(channel, d1)] = tick
            elif message == 0x80 or (message == 0x90 and d2 == 0):
                if drums:
                    drum_note_offs += 1
                    start = open_drum_notes.pop((channel, d1), None)
                    if start is not None and d1 in RX_SUSTAIN_KEYS:
                        held = clock(tick) - clock(start)
                        if held > LONG_HOLD_SECONDS:
                            long_holds[d1] = max(long_holds.get(d1, 0.0), held)
            elif message == 0xA0:
                controllers.add("poly_pressure")
            elif message == 0xD0:
                controllers.add("channel_pressure")
            elif message == 0xB0:
                if d1 == 0:
                    banks.add(d2)
                elif d1 == 99:
                    nrpn_msb[channel] = d2
                elif d1 in (100, 101):
                    nrpn_msb[channel] = -1  # an RPN select ends the NRPN's reign
                elif d1 == 6 and drums and nrpn_msb.get(channel) in DRUM_NRPN_MSBS:
                    drum_nrpns.add(nrpn_msb[channel])
                elif d1 in (5, 65, 84):
                    controllers.add("portamento")
                elif d1 == 66:
                    controllers.add("sostenuto")
                elif d1 == 67:
                    controllers.add("soft_pedal")
                elif d1 in (91, 93, 94):
                    controllers.add("sends")
                elif d1 == 126:
                    controllers.add("mono_mode")

    if len(port_numbers) > 1:
        features["multiport_ff21"] = sorted(port_numbers)
    if len(device_names) > 1:
        features["multiport_ff09"] = len(device_names)
    if len(instrument_names) > 1:
        features["names_ff04"] = len(instrument_names)
        if not port_numbers and not device_names:
            features["multiport_ff04_only"] = len(instrument_names)
    if port_numbers and (device_names or len(instrument_names) > 1):
        features["port_scheme_conflict"] = True
    if long_holds:
        features["drum_long_holds"] = {str(k): round(v, 2) for k, v in sorted(long_holds.items())}
    if drum_note_ons:
        features["drum_note_off_ratio"] = round(drum_note_offs / drum_note_ons, 2)
    if drum_setup_params:
        features["drum_setup_sysex"] = sorted(drum_setup_params)
        if {7, 8} & drum_setup_params:
            features["drum_rx_toggles"] = sorted({7, 8} & drum_setup_params)
    if drum_nrpns:
        features["drum_nrpn"] = sorted(drum_nrpns)
    if resets:
        features["resets"] = sorted(resets)
    if gs_families:
        features["gs_sysex"] = sorted(gs_families)
    if controllers:
        features["controllers"] = sorted(controllers)
    if len(banks - {0}) > 0:
        features["variation_banks"] = sorted(banks - {0})

    row["seconds"] = round(clock(last_tick), 1)
    row["features"] = features
    return row


# ---------------------------------------------------------------------------------------------
# Drivers
# ---------------------------------------------------------------------------------------------

SUFFIXES = {".mid", ".midi", ".kar", ".rmi"}


def run_scan(root, out_path, workers):
    paths = [
        os.path.join(directory, name)
        for directory, _, names in os.walk(root)
        for name in names
        if os.path.splitext(name)[1].lower() in SUFFIXES
    ]
    print(f"{len(paths)} files under {root}", file=sys.stderr)
    written = 0
    with open(out_path, "w", encoding="utf-8") as out:
        with concurrent.futures.ProcessPoolExecutor(max_workers=workers) as pool:
            for row in pool.map(describe, paths, chunksize=256):
                if row and row.get("features"):
                    out.write(json.dumps(row) + "\n")
                    written += 1
    print(f"{written} files with tagged features -> {out_path}", file=sys.stderr)


# What the report documents, in the order it documents it. The key is either a feature name or
# a callable over the features dict; the ranker orders candidate exemplars, best first.
AREAS = [
    ("multiport_ff21", "Multi-port by FF 21 MIDI Port metas",
     lambda f: -len(f["multiport_ff21"])),
    ("multiport_ff09", "Multi-port by FF 09 Device Name strings",
     lambda f: -f["multiport_ff09"]),
    ("multiport_ff04_only", "Multi-port by FF 04 Instrument Name only (no FF 21/FF 09)",
     lambda f: -f["multiport_ff04_only"]),
    ("port_scheme_conflict", "FF 21 alongside name metas: tests the scheme preference",
     lambda f: 0),
    # Ranked by the longest *plausible* hold: a hold past a minute is nearly always a broken
    # tempo map or a stuck note rather than a played roll, and a corpus this size has plenty of
    # both, so those files rank behind everything with a musically believable hold.
    ("drum_long_holds", "Rx Note Off drum keys held past 2 s (roll 25, applause 88, SFX)",
     lambda f: -max((v for v in f["drum_long_holds"].values() if v <= 60.0), default=0.0)),
    ("drum_rx_toggles", "Drum-setup SysEx toggling per-key Rx Note Off / Rx Note On",
     lambda f: -len(f.get("drum_setup_sysex", []))),
    ("drum_setup_sysex", "Any drum-setup SysEx (play key, level, group, pan, sends)",
     lambda f: -len(f["drum_setup_sysex"])),
    ("drum_nrpn", "Per-key drum NRPN overrides (pitch, level, pan, sends)",
     lambda f: -len(f["drum_nrpn"])),
    ("gs_sysex", "GS SysEx families (master, part, effects, EQ, EFX, scale tuning, display)",
     lambda f: -len(f["gs_sysex"])),
    ("resets", "System resets (GM, GM2, GS, XG)",
     lambda f: -len(f["resets"])),
    ("controllers", "Channel features (pressure, portamento, sostenuto, mono mode)",
     lambda f: -len(f["controllers"])),
    ("variation_banks", "GS variation bank selects",
     lambda f: -len(f["variation_banks"])),
    ("smpte_division", "SMPTE division (reader currently refuses these)",
     lambda f: 0),
    ("format2", "Format 2 files",
     lambda f: 0),
]


def run_report(rows_path, picks):
    rows = [json.loads(line) for line in open(rows_path, encoding="utf-8")]
    print(f"{len(rows)} files with features\n")
    for key, title, rank in AREAS:
        hits = [row for row in rows if key in row["features"]]
        if not hits:
            print(f"== {title}\n   none found\n")
            continue
        # Best exemplars: strongest use of the feature first, smallest file breaking ties, so
        # what gets copied out is focused rather than merely long.
        hits.sort(key=lambda row: (rank(row["features"]), row["size"]))
        print(f"== {title}  ({len(hits)} files)")
        for row in hits[:picks]:
            detail = row["features"].get(key)
            extra = "" if detail in (True, 0) else f"  {detail}"
            print(f"   {row['size']:>8}  {row.get('seconds', '?'):>7}s  {row['path']}{extra}")
        print()


def main():
    if len(sys.argv) >= 4 and sys.argv[1] == "scan":
        run_scan(sys.argv[2], sys.argv[3], int(sys.argv[4]) if len(sys.argv) > 4 else os.cpu_count())
    elif len(sys.argv) >= 3 and sys.argv[1] == "report":
        run_report(sys.argv[2], int(sys.argv[3]) if len(sys.argv) > 3 else 5)
    else:
        print(__doc__, file=sys.stderr)
        sys.exit(2)


if __name__ == "__main__":
    main()
