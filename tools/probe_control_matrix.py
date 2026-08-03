#!/usr/bin/env python3
"""Probe the GS control matrix one destination at a time against `SCCore.dll`.

Each of the eleven destinations gets a MIDI file that assigns it to the mod wheel, holds one note,
and steps the wheel through five amounts. Both engines render it and the comparison is a
per-destination feature track -- level, brightness or pitch.

Three details of the method are load-bearing, and the obvious version of each does not work.

  * **Differential.** Every file is rendered twice per engine, once with the destination assigned
    and once with that route switched off, and only the difference is reported. A patch's own
    envelope drifts across a ten-second note by more than most of these destinations move.
  * **A curve, not a point.** Stepping the source is what catches a wrong *scale*: a destination
    whose endpoints agree can still reach them along the wrong path, which is exactly how the
    pitch destination's missing 0x28-0x58 clamp was found.
  * **The wheel is never inert.** `40 2x 04` starts at 0x0a, so moving the wheel adds vibrato
    whatever else it is assigned to. That route leaks into every probe and has to be accounted for.

Usage:
    python3 tools/probe_control_matrix.py --scdec <scdec.exe> --cli <tabula-sonora>
        --dll <SCCore.dll> --work <scratch dir> [--only pitch] [--program 79]

Renders Roland-derived audio into the working directory: generate locally, do not redistribute.
"""

import argparse
import math
import pathlib
import struct
import subprocess
import sys
import wave

RATE = 32000

# Address low byte, name, the feature it should move, and the value that switches the route off.
# The first four destinations are bipolar and centre on 0x40; the six LFO depths are amounts and
# start at zero.
DESTINATIONS = [
    (0x00, "pitch", "pitch", 0x40),
    (0x01, "tvf_cutoff", "centroid", 0x40),
    (0x02, "amplitude", "level", 0x40),
    (0x03, "lfo1_rate", "pitch", 0x40),
    (0x04, "lfo1_pitch", "pitch", 0x00),
    (0x05, "lfo1_tvf", "centroid", 0x00),
    (0x06, "lfo1_tva", "level", 0x00),
    (0x07, "lfo2_rate", "pitch", 0x40),
    (0x08, "lfo2_pitch", "pitch", 0x00),
    (0x09, "lfo2_tvf", "centroid", 0x00),
    (0x0A, "lfo2_tva", "level", 0x00),
]

# A rate needs something already moving to speed up, so a second source supplies the depth:
# channel pressure drives the same LFO's pitch depth flat out while the wheel moves the rate.
COMPANION = {0x03: 0x24, 0x07: 0x28}

STEPS = [0, 32, 64, 96, 127]
STEP_SECONDS = 2.0
TRACK_HOP = 256
TRACK_SPAN = 2048


def set_track(hop, span):
    global TRACK_HOP, TRACK_SPAN
    TRACK_HOP, TRACK_SPAN = hop, span


def varlen(value):
    out = bytearray([value & 0x7F])
    value >>= 7
    while value:
        out.insert(0, (value & 0x7F) | 0x80)
        value >>= 7
    return bytes(out)


def dt1(address, data):
    """A GS DT1 SysEx with its checksum, as an SMF event payload."""
    body = list(address) + list(data)
    checksum = (128 - (sum(body) & 0x7F)) & 0x7F
    message = [0x41, 0x10, 0x42, 0x12] + body + [checksum, 0xF7]
    return bytes([0xF0]) + varlen(len(message)) + bytes(message)


def build(path, destination, depth, program, note):
    ticks_per_beat = 480
    # 120 bpm, so a beat is half a second and a tick is 1/960 s.
    ticks_per_second = ticks_per_beat * 2

    events = bytearray()

    def event(delta, payload):
        events.extend(varlen(delta))
        events.extend(payload)

    event(0, b"\xff\x51\x03" + struct.pack(">I", 500000)[1:])
    event(0, dt1([0x40, 0x00, 0x7F], [0x00]))
    event(ticks_per_second // 2, bytes([0xC0, program]))

    # Block 1 is channel 1: the address nibble is a block number, not a channel number.
    event(0, dt1([0x40, 0x21, destination], [depth]))
    companion = COMPANION.get(destination)
    if companion is not None:
        event(0, dt1([0x40, 0x21, companion], [0x7F]))
        event(0, bytes([0xD0, 0x7F]))

    event(0, bytes([0x90, note, 100]))
    for index, amount in enumerate(STEPS):
        delta = 0 if index == 0 else int(STEP_SECONDS * ticks_per_second)
        event(delta, bytes([0xB0, 0x01, amount]))
    event(int(STEP_SECONDS * ticks_per_second), bytes([0x80, note, 0]))
    event(0, b"\xff\x2f\x00")

    track = bytes(events)
    header = b"MThd" + struct.pack(">IHHH", 6, 0, 1, ticks_per_beat)
    path.write_bytes(header + b"MTrk" + struct.pack(">I", len(track)) + track)


def read_mono(path):
    with wave.open(str(path), "rb") as handle:
        frames = handle.getnframes()
        channels = handle.getnchannels()
        raw = handle.readframes(frames)
    values = struct.unpack(f"<{len(raw) // 2}h", raw)
    left = values[0::channels]
    right = values[1::channels] if channels > 1 else left
    return [(left[i] + right[i]) * 0.5 / 32768.0 for i in range(len(left))]


def fft(real, imag):
    size = len(real)
    j = 0
    for i in range(1, size):
        bit = size >> 1
        while j & bit:
            j ^= bit
            bit >>= 1
        j |= bit
        if i < j:
            real[i], real[j] = real[j], real[i]
            imag[i], imag[j] = imag[j], imag[i]
    length = 2
    while length <= size:
        angle = -2 * math.pi / length
        wr, wi = math.cos(angle), math.sin(angle)
        half = length // 2
        for i in range(0, size, length):
            cr, ci = 1.0, 0.0
            for k in range(half):
                ur, ui = real[i + k], imag[i + k]
                vr = real[i + k + half] * cr - imag[i + k + half] * ci
                vi = real[i + k + half] * ci + imag[i + k + half] * cr
                real[i + k], imag[i + k] = ur + vr, ui + vi
                real[i + k + half], imag[i + k + half] = ur - vr, ui - vi
                cr, ci = cr * wr - ci * wi, cr * wi + ci * wr
        length <<= 1
    return real, imag


def spectrum(chunk):
    size = 1
    while size * 2 <= len(chunk):
        size *= 2
    if size < 256:
        return []
    window = [0.5 - 0.5 * math.cos(2 * math.pi * i / size) for i in range(size)]
    real = [chunk[i] * window[i] for i in range(size)]
    imag = [0.0] * size
    real, imag = fft(real, imag)
    return [(real[k] * real[k] + imag[k] * imag[k], k * RATE / size) for k in range(size // 2)]


def centroid(chunk):
    power = spectrum(chunk)
    total = sum(p for p, _ in power)
    return sum(p * f for p, f in power) / total if total > 0 else 0.0


def level(chunk):
    return math.sqrt(sum(v * v for v in chunk) / len(chunk)) if chunk else 0.0


def spectral_pitch(chunk):
    """Pitch as the interpolated peak of the spectrum.

    Two cheaper estimators were tried and discarded. Zero crossings track the harmonics rather
    than the note on a sampled lead. Autocorrelation is indifferent to harmonics but jumps
    octaves, and it jumped to *different* octaves in the two engines, which turns a comparison
    into noise. A parabolic fit around the strongest bin is stable because it never has to choose
    a period at all -- it reports where the energy is.
    """
    size = 1
    while size * 2 <= len(chunk):
        size *= 2
    if size < 512:
        return 0.0

    window = [0.5 - 0.5 * math.cos(2 * math.pi * i / size) for i in range(size)]
    real = [chunk[i] * window[i] for i in range(size)]
    imag = [0.0] * size
    real, imag = fft(real, imag)
    power = [real[k] * real[k] + imag[k] * imag[k] for k in range(size // 2)]

    # From bin 2, so the window's own DC skirt cannot win.
    peak = max(range(2, len(power) - 1), key=lambda k: power[k])
    before, here, after = power[peak - 1], power[peak], power[peak + 1]
    offset = 0.0
    if before > 0.0 and here > 0.0 and after > 0.0:
        # Fitted in log magnitude, where a windowed peak is close to a parabola.
        a, b, c = math.log(before), math.log(here), math.log(after)
        curvature = a - (2.0 * b) + c
        if curvature != 0.0:
            offset = 0.5 * (a - c) / curvature
    return (peak + offset) * RATE / size


FEATURES = {"level": level, "centroid": centroid, "pitch": spectral_pitch}


def series_for(mono, feature, start, count):
    measure_one = FEATURES[feature]
    out = []
    position = start
    while position + TRACK_SPAN <= start + count:
        out.append(measure_one(mono[position:position + TRACK_SPAN]))
        position += TRACK_HOP
    return out


def wobble(series):
    """The rate of a series' strongest oscillation, in Hz, and its peak-to-peak size."""
    if len(series) < 32:
        return 0.0, 0.0
    size = 1
    while size * 2 <= len(series):
        size *= 2
    mean = sum(series[:size]) / size
    window = [0.5 - 0.5 * math.cos(2 * math.pi * i / size) for i in range(size)]
    real = [(series[i] - mean) * window[i] for i in range(size)]
    imag = [0.0] * size
    real, imag = fft(real, imag)
    power = [(real[k] * real[k] + imag[k] * imag[k], k) for k in range(1, size // 2)]
    peak = max(power, key=lambda entry: entry[0])[1] if power else 0
    return peak * (RATE / TRACK_HOP) / size, max(series) - min(series)


def measure(path, feature):
    """Per step: the feature's mean, and the rate and size of any wobble in it."""
    mono = read_mono(path)
    note_start = int(0.5 * RATE)
    rows = []
    for index in range(len(STEPS)):
        # Taken from inside the step, so the controller has landed and the previous step's
        # transient has passed.
        begin = note_start + int((index * STEP_SECONDS + 0.4) * RATE)
        count = int((STEP_SECONDS - 0.5) * RATE)
        if begin + count > len(mono):
            rows.append((0.0, 0.0, 0.0))
            continue
        series = series_for(mono, feature, begin, count)
        mean = sum(series) / len(series) if series else 0.0
        rate, size = wobble(series)
        rows.append((mean, rate, size))
    return rows


def run(command, label):
    result = subprocess.run(command, capture_output=True, text=True)
    if result.returncode != 0:
        raise RuntimeError(f"{label} failed ({result.returncode}): {result.stderr[-400:]}")


def render(arguments, midi, out, oracle):
    if oracle:
        run([str(arguments.scdec), str(arguments.dll), "smf", str(midi.resolve()),
             str(out.resolve()), str(arguments.map), "0.5"], "oracle")
    else:
        run([str(arguments.cli), "render", str(midi), str(out), "--map", str(arguments.map),
             "--tail", "0.5", "--dll", str(arguments.dll)], "cli")
    if not out.exists():
        raise RuntimeError("no output written")


def ratio_db(assigned, off):
    if assigned <= 0.0 or off <= 0.0:
        return float("nan")
    return 20.0 * math.log10(assigned / off)


def ratio_cents(assigned, off):
    if assigned <= 0.0 or off <= 0.0:
        return float("nan")
    return 1200.0 * math.log2(assigned / off)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--scdec", type=pathlib.Path, required=True)
    parser.add_argument("--cli", type=pathlib.Path, required=True)
    parser.add_argument("--dll", type=pathlib.Path, required=True)
    parser.add_argument("--work", type=pathlib.Path, required=True)
    parser.add_argument("--map", type=int, default=4)
    parser.add_argument("--program", type=int, default=79)
    parser.add_argument("--note", type=int, default=60)
    parser.add_argument("--depth", type=lambda v: int(v, 0), default=0x7F)
    parser.add_argument("--only", default=None)
    parser.add_argument("--hop", type=int, default=TRACK_HOP)
    parser.add_argument("--span", type=int, default=TRACK_SPAN)
    arguments = parser.parse_args()

    set_track(arguments.hop, arguments.span)
    arguments.work.mkdir(parents=True, exist_ok=True)

    for address, name, feature, off_value in DESTINATIONS:
        if arguments.only is not None and arguments.only != name:
            continue

        results = {}
        for tag, depth in (("on", arguments.depth), ("off", off_value)):
            midi = arguments.work / f"matrix-{name}-{tag}.mid"
            build(midi, address, depth, arguments.program, arguments.note)
            for engine, is_oracle in (("oracle", True), ("ours", False)):
                out = arguments.work / f"matrix-{name}-{tag}-{engine}.wav"
                try:
                    render(arguments, midi, out, is_oracle)
                except RuntimeError as error:
                    print(f"{name}/{tag}/{engine}: {error}")
                    return 1
                results[(tag, engine)] = measure(out, feature)

        print(f"\n=== {name}  (40 21 {address:02X}: 0x{arguments.depth:02X} vs 0x{off_value:02X}, "
              f"feature: {feature}) ===")
        print("  wheel        " + "".join(f"{value:>10}" for value in STEPS))

        unit = "dB" if feature == "level" else ("cents" if feature == "pitch" else "dB")
        convert = ratio_db if feature != "pitch" else ratio_cents
        for engine in ("oracle", "ours"):
            deltas = [convert(on[0], off[0])
                      for on, off in zip(results[("on", engine)], results[("off", engine)])]
            print(f"  {engine:<7} {unit:<5}" + "".join(f"{value:>10.1f}" for value in deltas))

        print("  -- wobble rate (Hz), assigned --")
        for engine in ("oracle", "ours"):
            print(f"  {engine:<13}" + "".join(f"{row[1]:>10.2f}"
                                              for row in results[("on", engine)]))
        print("  -- wobble size, assigned --")
        for engine in ("oracle", "ours"):
            print(f"  {engine:<13}" + "".join(f"{row[2]:>10.4g}"
                                              for row in results[("on", engine)]))

    return 0


if __name__ == "__main__":
    sys.exit(main())
