#!/usr/bin/env python3
"""Sweep the envelope, pitch and LFO fixed-point paths and dump the results.

A *differential* oracle for Phase 4, written from the reverse-engineering notes rather than
translated from the C++ so that agreement between the two is evidence.

Everything here is deliberately integer arithmetic with explicit 16-bit truncation, because that is
what the engine's control path is. Python's integers do not wrap, so every place the engine's do is
made explicit with a mask -- and those places are exactly the ones a C++ port is most likely to get
wrong, since there the wrap happens silently or is undefined behaviour:

  * segment_milliseconds multiplies two 0xffff-scale words twice over
  * the pitch envelope's depth scale reaches about 2.7e11
  * the pitch release damper multiplies to about 2.18e9
  * the LFO waveforms are 16-bit throughout, and quadrants 1 and 3 rely on the truncation
  * shift8 truncates to 16 bits *between* its two shifts

Usage:
    python3 tools/dump_modulation.py <SCCore.dll> <output.json> [--manifest assets/manifest.json]

Roland-derived output: generate locally, do not redistribute.
"""

import argparse
import hashlib
import json
import pathlib
import struct
import sys

TONE_STRIDE = 0x100
TONE_HEADER = 0x24
PARTIAL_STRIDE = 0x6E
MINIMUM_SEGMENT_TICKS = 9
UNITY = 0x100


def s32(value):
    """Truncate to a signed 32-bit accumulator, which is what the engine's registers are."""
    value &= 0xFFFFFFFF
    return value - 0x100000000 if value >= 0x80000000 else value


def s16(value):
    value &= 0xFFFF
    return value - 0x10000 if value >= 0x8000 else value


def s8(value):
    value &= 0xFF
    return value - 0x100 if value >= 0x80 else value


def shift8(value):
    """The engine's (short)(v << 2) >> 8 idiom; the truncation between the shifts is the point."""
    return s16(s32(value << 2)) >> 8


class Tables:
    def __init__(self, image, manifest):
        entries = {t["name"]: t for t in manifest["cached_tables"]}

        def slice_of(name):
            entry = entries[name]
            offset = int(entry["file_offset"], 16)
            return image[offset : offset + entry["size"]]

        def u16s(name):
            raw = slice_of(name)
            return list(struct.unpack("<%dH" % (len(raw) // 2), raw[: len(raw) // 2 * 2]))

        def s16s(name):
            raw = slice_of(name)
            return list(struct.unpack("<%dh" % (len(raw) // 2), raw[: len(raw) // 2 * 2]))

        self.rate_curve = u16s("curve_segrate_2900.bin")
        self.rate_out = u16s("curve_rateout_3060.bin")
        self.scale_curve = slice_of("curve_scale_28e8.bin")
        self.env_shape = u16s("env_shape_7a90.bin")
        self.tone = slice_of("tone_a.bin")
        self.lfo_wave = slice_of("lfo_wave_1740.bin")
        self.lfo_rate = u16s("lfo_rate_2790.bin")
        self.lfo_cents = u16s("lfo_cents_2690.bin")
        self.lfo_delay = u16s("lfo_delay_a2590.bin")
        self.lfo_wave_map = slice_of("lfo_wavemap_87ae0.bin")
        self.lfo_wave_bank = s16s("lfo_wavebank_a17f0.bin")
        self.pitch_env = slice_of("curve_pitchenv_2578.bin")
        self.pitch_bias = slice_of("curve_pitchbias_2890.bin")
        self.pitch_depth_vs = u16s("curve_pitchdepthvs_28d0.bin")
        self.kf_pitch_rate0 = slice_of("kf_pitchrate0_01f20.bin")
        self.kf_pitchrate1 = slice_of("kf_pitchrate1_01aa0.bin")
        self.kf_pitch = s16s("kf_pitch_01b20.bin")

        self.depth_slope = [
            s16(self.pitch_env[i * 2] | (self.pitch_env[i * 2 + 1] << 8)) for i in range(0x80)
        ]


def rate_scale(tables, base_rate, modifier):
    u = modifier - 0x40
    if u == 0:
        return UNITY

    if u < 0:
        signed = s8(base_rate)
        u = -u
        base_rate = (-signed) & 0xFF
        if base_rate == 0:
            base_rate = 0xFF

    centred = s16(base_rate - 0x80)
    if centred == 0:
        return UNITY

    scale = tables.scale_curve[u]
    if centred < 0:
        index = (0x80 - ((centred * scale * -2) >> 8)) & 0x1FF
    else:
        index = (((centred * scale * 2) >> 8) + 0x80) & 0x1FF
    return tables.rate_out[index]


def level_scale(tables, level, modifier):
    from_level = 0x40 - level
    if from_level == 0:
        return UNITY
    from_modifier = modifier - 0x40
    if from_modifier == 0:
        return UNITY

    def curve(at):
        return tables.scale_curve[at & 0xFF]

    if from_level < 0:
        if from_modifier >= 0:
            return tables.rate_out[(0x80 - shift8(-(curve(from_modifier * 2) * from_level))) & 0x1FF]
        product = -(curve(-from_modifier * 2) * from_level)
    else:
        if from_modifier < 0:
            return tables.rate_out[(0x80 - shift8(curve(-from_modifier * 2) * from_level)) & 0x1FF]
        product = curve(from_modifier * 2) * from_level

    return tables.rate_out[(shift8(product) + 0x80) & 0x1FF]


def segment_milliseconds(tables, rate_byte, rate_multiplier, velocity_multiplier=UNITY, bias=0):
    index = (rate_byte & 0x7F) + bias
    if index < 0:
        return 0
    ticks = tables.rate_curve[min(index, 0x7F)]
    if ticks < MINIMUM_SEGMENT_TICKS:
        return 0
    # Both products exceed 32 bits: each factor reaches 0xffff.
    scaled = min(0xFFFF, s32(rate_multiplier * ticks) >> 8)
    return s32(velocity_multiplier * scaled) >> 8


def lfo_waveform(tables, phase, waveform):
    p = phase & 0xFFFF
    if waveform == 0:
        value = tables.lfo_wave[(p >> 8) & 0x7F]
        return value * -0x80 if p > 0x8000 else value * 0x80
    if waveform == 4:
        return s16(((((p - 0x10000) if p >= 0x8000 else p) >> 15) & 2) + 0x7FFF)
    if waveform == 5:
        return s16(p - 0x8000)
    if waveform == 6:
        quadrant = p >> 14
        if quadrant == 1:
            return s16(~(p * 2))
        if quadrant == 3:
            return s16((p * 2) + 1)
        return s16(((-p) if quadrant == 2 else p) * 2)
    if waveform == 7:
        quadrant = p >> 14
        if quadrant == 1:
            return s16(~(p * 2) + 0x8001)
        if quadrant == 3:
            return s16((p * 2) - 0x7FFE)
        return s16((((-p) if quadrant == 2 else p) * 2) - 0x7FFF)
    if 8 <= waveform < 0x20:
        row = (waveform - 8) * 0x81
        index = p >> 9
        fraction = (p >> 1) & 0xFF
        a = tables.lfo_wave_bank[row + index]
        b = tables.lfo_wave_bank[row + index + 1]
        return s16((((b - a) * fraction) >> 8) + a)
    return 0


def pitch_envelope(tables, block, key, velocity):
    """The fully decoded pitch envelope, or None when it is disabled.

    Every number here passes through at least one expression that overflows 32 bits."""
    depth = block[0x18] | (block[0x19] << 8)
    if depth == 0:
        return None

    sensitivity = block[0x2B] - 0x40
    if sensitivity == 0:
        scaled_depth = depth
    else:
        v = max(0, min(127, velocity))
        if sensitivity < 0:
            sensitivity = -sensitivity
            v = (-v) & 0x7F
        # Reaches about 2.7e11; the engine keeps only the low word.
        inner = s32(s32(tables.depth_slope[sensitivity] * v) + tables.pitch_depth_vs[sensitivity])
        scaled_depth = s32(s32(inner * depth) + 0x8000) >> 16

    def delta(bias):
        if bias == 0:
            return 0
        d = (tables.pitch_bias[min(64, abs(bias))] * scaled_depth) >> 7
        return -d if bias < 0 else d

    biases = [s8(block[0x1B + i]) - 0x40 for i in range(5)]

    main_rate = rate_scale(
        tables, (tables.kf_pitch_rate0[block[0x27] * 0x80 + (key & 0x7F)] - 0x80) & 0xFF, block[0x29])
    release_rate = rate_scale(
        tables, (tables.kf_pitchrate1[block[0x27] * 0x80 + (key & 0x7F)] - 0x80) & 0xFF, block[0x2A])
    velocity_scale = level_scale(tables, max(0, min(127, velocity)), block[0x2C])

    def time(rate_byte, multiplier):
        r = rate_byte & 0x7F
        ticks = tables.rate_curve[r]
        if r == 0 or ticks < MINIMUM_SEGMENT_TICKS:
            return 0
        # Both multipliers reach 0xffff, so the product overflows and the mask takes the low word.
        return s32(((s32(multiplier * ticks) >> 8) & 0xFFFF) * velocity_scale) >> 8

    return {
        "start": delta(biases[0]),
        "targets": [delta(biases[1]), delta(biases[2]), delta(biases[3]), 0],
        "release": delta(biases[4]),
        "times": [time(block[0x20 + i], main_rate) for i in range(4)],
        "releaseMs": time(block[0x24], release_rate),
    }


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
        sys.exit(f"{arguments.dll} is {len(image)} bytes; expected {expected['size']}.")
    digest = hashlib.sha256(image).hexdigest()
    if digest != expected["sha256"]:
        sys.exit(f"{arguments.dll} has SHA-256 {digest}; expected {expected['sha256']}.")

    tables = Tables(image, manifest)

    # Whole-domain sweeps of the shared fixed-point primitives. These are where the wrapping lives.
    rate_scale_values = [
        rate_scale(tables, base, modifier)
        for base in range(0, 256, 3)
        for modifier in range(0, 128, 5)
    ]
    level_scale_values = [
        level_scale(tables, level, modifier)
        for level in range(0, 128, 3)
        for modifier in range(0, 128, 5)
    ]
    segment_ms_values = [
        segment_milliseconds(tables, rate, multiplier, velocity)
        for rate in range(0, 256, 7)
        for multiplier in (0x100, 0x1000, 0x8000, 0xFFFF)
        for velocity in (0x100, 0x4000, 0xFFFF)
    ]
    shift8_values = [shift8(v) for v in range(-70000, 70000, 137)]
    waveform_values = [
        lfo_waveform(tables, phase, wave)
        for wave in (0, 4, 5, 6, 7, 8, 12, 20, 31)
        for phase in range(0, 0x10000, 313)
    ]

    # Per-partial decodes across the whole tone table.
    partials = []
    tone_count = len(tables.tone) // TONE_STRIDE
    for number in range(tone_count):
        base = number * TONE_STRIDE
        name = tables.tone[base : base + 12].decode("latin-1").rstrip(" \0")
        if len(name) < 2:
            continue
        for slot in range(2):
            p = base + TONE_HEADER + slot * PARTIAL_STRIDE
            block = tables.tone[p : p + PARTIAL_STRIDE]
            if (block[0x02] | (block[0x03] << 8)) == 0xFFFF:
                continue
            for velocity in (1, 64, 127):
                partials.append(
                    {
                        "tone": number,
                        "slot": slot,
                        "velocity": velocity,
                        "envelope": pitch_envelope(tables, block, 60, velocity),
                    }
                )

    document = {
        "_note": (
            "Modulation sweeps derived from a licensed SCCore.dll. Roland-derived: generate "
            "locally, do not redistribute."
        ),
        "dllSha256": digest,
        "rateScale": rate_scale_values,
        "levelScale": level_scale_values,
        "segmentMilliseconds": segment_ms_values,
        "shift8": shift8_values,
        "lfoWaveform": waveform_values,
        "pitchEnvelopes": partials,
    }

    arguments.output.parent.mkdir(parents=True, exist_ok=True)
    arguments.output.write_text(json.dumps(document, indent=1), encoding="utf-8")

    print(f"rateScale           {len(rate_scale_values):7}")
    print(f"levelScale          {len(level_scale_values):7}")
    print(f"segmentMilliseconds {len(segment_ms_values):7}")
    print(f"shift8              {len(shift8_values):7}")
    print(f"lfoWaveform         {len(waveform_values):7}")
    print(f"pitchEnvelopes      {len(partials):7}")
    print(f"Wrote {arguments.output}")


if __name__ == "__main__":
    main()
