#!/usr/bin/env python3
"""Parse a Standard MIDI File and dump the events and notes it yields.

A *differential* oracle for the MIDI layer, written from the SMF specification and the
reverse-engineering notes rather than translated from the C++.

Two details here are easy to get wrong in a port and are the reason this exists:

  * Event positions are quantised onto the 32-sample render grid with round-half-to-**even**,
    because that is what .NET's Math.Round does and the reference build uses it. C's round() goes
    half-away-from-zero and moves events by a whole block on every exact tie.
  * Same-position events must keep their original tick order, so the final sort has to be stable.
    An unstable sort can put a note-on ahead of the program change that selects its patch.

The note list also exercises the sustain pedal rules: a note-off arriving with the damper down is
parked rather than acted on, and a re-strike of that note supersedes the parked entry rather than
leaving it to be closed by the pedal's lift.

Usage:
    python3 tools/dump_sequence.py <song.mid> <output.json>
"""

import argparse
import json
import pathlib
import sys

BLOCK_GRID = 32
DEFAULT_TEMPO = 500_000
SAMPLE_RATE = 32000
CHANNELS = 16

DEFAULT_VOLUME = 100
DEFAULT_EXPRESSION = 127
DEFAULT_PAN = 64
DEFAULT_REVERB_SEND = 40
DEFAULT_CHORUS_SEND = 0


def quantise(samples):
    """Round to nearest with ties to even, then floor onto the block grid."""
    # Python's round() is already round-half-to-even, which is what .NET's Math.Round does.
    return int(round(samples)) // BLOCK_GRID * BLOCK_GRID


def read_variable_length(data, position):
    value = 0
    while True:
        byte = data[position]
        position += 1
        value = (value << 7) | (byte & 0x7F)
        if not byte & 0x80:
            return position, value


def parse(data):
    if len(data) < 14 or data[:4] != b"MThd":
        sys.exit("Not a Standard MIDI File: missing MThd.")

    track_count = int.from_bytes(data[10:12], "big")
    division = int.from_bytes(data[12:14], "big")
    if division & 0x8000:
        sys.exit("SMPTE division is not supported.")

    ticks_per_quarter = division
    position = 14
    merged = []
    order = 0

    for track in range(track_count):
        if data[position : position + 4] != b"MTrk":
            sys.exit(f"Track {track} is missing its MTrk header.")
        length = int.from_bytes(data[position + 4 : position + 8], "big")
        position += 8
        end = min(position + length, len(data))

        tick = 0
        status = 0
        while position < end:
            position, delta = read_variable_length(data, position)
            tick += delta
            if position >= end:
                break

            message = data[position]
            if message & 0x80:
                status = message
                position += 1
            else:
                message = status

            if message == 0xFF:
                meta_type = data[position]
                position += 1
                position, meta_length = read_variable_length(data, position)
                if meta_type == 0x51 and meta_length >= 3:
                    tempo = (data[position] << 16) | (data[position + 1] << 8) | data[position + 2]
                    merged.append((tick, order, "tempo", tempo, 0, 0, None))
                    order += 1
                position += meta_length
            elif message in (0xF0, 0xF7):
                position, sysex_length = read_variable_length(data, position)
                if message == 0xF0:
                    payload = bytes([0xF0]) + data[position : position + sysex_length]
                    merged.append((tick, order, "sysex", 0, 0, 0, list(payload)))
                    order += 1
                position += sysex_length
            else:
                kind = message & 0xF0
                if kind in (0xC0, 0xD0):
                    merged.append((tick, order, "channel", message, data[position], 0, None))
                    order += 1
                    position += 1
                else:
                    merged.append(
                        (tick, order, "channel", message, data[position], data[position + 1], None)
                    )
                    order += 1
                    position += 2

        position = end

    merged.sort(key=lambda e: (e[0], e[1]))

    events = []
    tempo_now = DEFAULT_TEMPO
    last_tick = 0
    seconds = 0.0
    for tick, _order, kind, status, data1, data2, payload in merged:
        seconds += (tick - last_tick) * (tempo_now / 1e6 / ticks_per_quarter)
        last_tick = tick
        if kind == "tempo":
            tempo_now = status
        elif kind == "sysex":
            events.append(
                {"position": quantise(seconds * SAMPLE_RATE), "kind": "sysex", "sysex": payload}
            )
        else:
            events.append(
                {
                    "position": quantise(seconds * SAMPLE_RATE),
                    "kind": "channel",
                    "status": status,
                    "data1": data1,
                    "data2": data2,
                }
            )

    # Stable: same-position events keep their tick order.
    events.sort(key=lambda e: e["position"])
    return events


class Timeline:
    def __init__(self):
        self.points = []

    def add(self, position, value):
        self.points.append((position, value))

    def value_at(self, position, fallback):
        result = fallback
        for at, value in self.points:
            if at > position:
                break
            result = value
        return result


def build(events):
    parts = [
        {name: Timeline() for name in
         ("volume", "expression", "pan", "bend", "bend_range", "damper", "modulation",
          "reverb_send", "chorus_send", "delay_send", "program", "bank")}
        for _ in range(CHANNELS)
    ]
    for part in parts:
        part["bend_range"].add(0, 2)

    notes = []
    open_notes = []
    sustained = []
    rpn = [[0x7F, 0x7F] for _ in range(CHANNELS)]
    nrpn = [[0x7F, 0x7F] for _ in range(CHANNELS)]
    is_nrpn = [False] * CHANNELS
    drum_pitch = [dict() for _ in range(CHANNELS)]
    last_position = 0

    def close(channel, note, off):
        for index, held in enumerate(open_notes):
            if held["channel"] == channel and held["note"] == note:
                open_notes.pop(index)
                part = parts[channel]
                notes.append(
                    {
                        "channel": channel,
                        "note": note,
                        "velocity": held["velocity"],
                        "on": held["on"],
                        "off": off,
                        "program": part["program"].value_at(held["on"], 0),
                        "bank": part["bank"].value_at(held["on"], 0),
                        "pan": part["pan"].value_at(held["on"], DEFAULT_PAN),
                        "volume": part["volume"].value_at(held["on"], DEFAULT_VOLUME),
                        "expression": part["expression"].value_at(held["on"], DEFAULT_EXPRESSION),
                        "reverbSend": part["reverb_send"].value_at(held["on"], DEFAULT_REVERB_SEND),
                        "chorusSend": part["chorus_send"].value_at(held["on"], DEFAULT_CHORUS_SEND),
                        "delaySend": part["delay_send"].value_at(held["on"], 0),
                        "drumPitch": held["drum_pitch"],
                    }
                )
                return

    for event in events:
        last_position = max(last_position, event["position"])
        if event["kind"] == "sysex":
            continue

        channel = event["status"] & 0x0F
        kind = event["status"] & 0xF0
        part = parts[channel]
        position = event["position"]

        if kind == 0x90 and event["data2"] > 0:
            close(channel, event["data1"], position)
            sustained[:] = [s for s in sustained
                            if not (s[0] == channel and s[1] == event["data1"])]
            open_notes.append(
                {
                    "channel": channel,
                    "note": event["data1"],
                    "on": position,
                    "velocity": event["data2"],
                    "drum_pitch": drum_pitch[channel].get(event["data1"], 0),
                }
            )
        elif kind in (0x80, 0x90):
            if part["damper"].value_at(position, 0) >= 0x40:
                sustained.append((channel, event["data1"], position))
            else:
                close(channel, event["data1"], position)
        elif kind == 0xC0:
            part["program"].add(position, event["data1"])
        elif kind == 0xE0:
            part["bend"].add(position, event["data1"] | (event["data2"] << 7))
        elif kind == 0xB0:
            controller, value = event["data1"], event["data2"]
            if controller == 1:
                part["modulation"].add(position, value)
            elif controller == 7:
                part["volume"].add(position, value)
            elif controller == 10:
                part["pan"].add(position, 1 if value == 0 else value)
            elif controller == 11:
                part["expression"].add(position, value)
            elif controller == 91:
                part["reverb_send"].add(position, value)
            elif controller == 93:
                part["chorus_send"].add(position, value)
            elif controller == 0:
                part["bank"].add(position, value)
            elif controller == 64:
                part["damper"].add(position, value)
                if value < 0x40:
                    releasing = [s for s in sustained if s[0] == channel]
                    sustained[:] = [s for s in sustained if s[0] != channel]
                    for _c, note, _req in releasing:
                        close(channel, note, position)
            elif controller == 101:
                rpn[channel][0] = value
                is_nrpn[channel] = False
            elif controller == 100:
                rpn[channel][1] = value
                is_nrpn[channel] = False
            elif controller == 99:
                nrpn[channel][0] = value
                is_nrpn[channel] = True
            elif controller == 98:
                nrpn[channel][1] = value
                is_nrpn[channel] = True
            elif controller == 6:
                if is_nrpn[channel]:
                    if nrpn[channel][0] == 0x18:
                        drum_pitch[channel][nrpn[channel][1]] = value - 0x40
                elif rpn[channel] == [0, 0]:
                    part["bend_range"].add(position, value)
            elif controller in (120, 123):
                for note in [o["note"] for o in open_notes if o["channel"] == channel]:
                    close(channel, note, position)
                sustained[:] = [s for s in sustained if s[0] != channel]
            elif controller == 121:
                part["expression"].add(position, 127)
                part["bend"].add(position, 8192)
                part["damper"].add(position, 0)
                part["modulation"].add(position, 0)

    for channel, note, requested in sustained:
        close(channel, note, requested)
    while open_notes:
        close(open_notes[0]["channel"], open_notes[0]["note"], last_position)

    return notes, last_position


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("midi", type=pathlib.Path)
    parser.add_argument("output", type=pathlib.Path)
    arguments = parser.parse_args()

    data = arguments.midi.read_bytes()
    events = parse(data)
    notes, last_position = build(events)

    arguments.output.parent.mkdir(parents=True, exist_ok=True)
    arguments.output.write_text(
        json.dumps(
            {
                "source": arguments.midi.name,
                "eventCount": len(events),
                "lastEventPosition": last_position,
                "events": events,
                "notes": notes,
            },
            indent=1,
        ),
        encoding="utf-8",
    )
    print(f"{len(events)} events, {len(notes)} notes, last event at {last_position}")
    print(f"Wrote {arguments.output}")


if __name__ == "__main__":
    main()
