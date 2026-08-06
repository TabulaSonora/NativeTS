#pragma once

/// The numbers that go into an SF2 modulator source word, under the names the specification gives
/// them.
///
/// Both halves of the export reach for these: `modulators.cpp` builds the bank-wide `DMOD` set and
/// `bank_builder.cpp` builds the per-instrument global zones that replace entries in it. A zone
/// modulator only replaces a default when the two source words match exactly, so the two files
/// have to agree on every curve and controller number they share -- which is easier to see when
/// they are reading the same names rather than the same literals.

namespace ts::sf2 {

/// The four curve shapes an SF2 source can apply. Convex, 2, is the one this export never asks for.
inline constexpr int curve_linear = 0;
inline constexpr int curve_concave = 1;
inline constexpr int curve_switch = 3;

inline constexpr int cc_modulation = 1;
inline constexpr int cc_volume = 7;
inline constexpr int cc_balance = 8;
inline constexpr int cc_pan = 10;
inline constexpr int cc_expression = 11;
/// CC#64, the damper pedal -- "sustain" in the MIDI tables, and the pedal the engine reads
/// continuously on the piano tones and quantises everywhere else.
inline constexpr int cc_damper = 64;
inline constexpr int cc_soft = 67;
inline constexpr int cc_resonance = 71;
inline constexpr int cc_release = 72;
inline constexpr int cc_attack = 73;
inline constexpr int cc_brightness = 74;
inline constexpr int cc_decay = 75;
inline constexpr int cc_vibrato_rate = 76;
inline constexpr int cc_vibrato_depth = 77;
inline constexpr int cc_vibrato_delay = 78;
inline constexpr int cc_reverb = 91;
inline constexpr int cc_chorus = 93;

/// The named sources, which are indexed separately from the controllers and selected by clearing
/// the source word's CC bit.
inline constexpr int velocity_source = 2;
inline constexpr int channel_pressure_source = 13;
inline constexpr int pitch_wheel_source = 14;
inline constexpr int pitch_wheel_range_source = 16;

} // namespace ts::sf2
