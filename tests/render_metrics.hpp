#pragma once

// What a render is compared *by*, when it cannot be compared sample by sample.
//
// Both authoritative gates face the same problem: this port is not bit-exact with `SCCore.dll` and
// does not aim to be, so a digest could only ever fail. What is compared instead is what
// `docs/COMPARING_RENDERS.md` argues actually separates a good render from a bad one -- level,
// per-octave spectrum, and a coarse envelope that sees a note going missing without seeing phase.
//
// These live here rather than in either test because the *generators* compute the same numbers in
// Python, and two transcriptions of one FFT are two chances to transcribe it differently. The song
// and note gates disagree about where to take the window and which bands to take, and nothing else.

#include <span>
#include <vector>

namespace testmetrics {

/// In-place radix-2 FFT, transcribed from the generators.
void transform(std::vector<double>& real, std::vector<double>& imag);

/// Level in dB in octave bands around each of `centres`, from a Hann window of the largest power of
/// two that fits (capped at 65536) beginning `start_fraction` of the way in.
///
/// A song wants that fraction above zero, because its opening is often silence and the spectrum of
/// silence compares equal to any other silence. A single note wants zero, because its opening is
/// the attack and that is the part worth looking at.
///
/// Returns empty when there is not enough signal to transform.
[[nodiscard]] std::vector<double> octave_bands(const std::vector<double>& mono,
                                               int rate,
                                               std::span<const int> centres,
                                               double start_fraction);

/// A coarse RMS envelope in dB: catches a missing or late note without seeing phase at all.
[[nodiscard]] std::vector<double> rms_envelope(const std::vector<double>& mono, int windows);

} // namespace testmetrics
