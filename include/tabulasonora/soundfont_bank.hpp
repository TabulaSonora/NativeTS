#pragma once

#include "tabulasonora/drum_kit_table.hpp"
#include "tabulasonora/patch_directory.hpp"
#include "tabulasonora/soundfont_samples.hpp"
#include "tabulasonora/soundfont_writer.hpp"
#include "tabulasonora/pitch_chain.hpp"
#include "tabulasonora/tva_chain.hpp"
#include "tabulasonora/tvf_chain.hpp"

#include <string>

namespace ts::sf2 {

/// Knobs the bank layout exposes. The defaults are what the export is meant to produce.
struct BankOptions {
    std::string name = "Sound Canvas";
    std::string software;
    std::string comment;
    /// The wave ROM's native rate. Everything is stored at it; nothing is resampled.
    int sample_rate = 32000;
    /// How long a note the envelope fit is scored over, in seconds.
    double fit_hold_seconds = 1.0;
};

/// A built bank and the counts worth reporting about it.
struct BankBuild {
    Bank bank;
    int melodic_presets = 0;
    int drum_presets = 0;

    /// How the amplitude envelopes came out, as fractions of each partial's own peak.
    ///
    /// `overflowed_zones` counts the zones whose engine envelope moved in more than two segments,
    /// which is exactly the population SF2's DAHDSR cannot hold. It is reported rather than fixed
    /// because there is nothing in the format to fix it with.
    double worst_fit = 0.0;
    double fit_error_sum = 0.0;
    int fitted_zones = 0;
    int overflowed_zones = 0;

    /// How the one modulation envelope was spent.
    ///
    /// `shared_mod_envelopes` is the population where the filter and the pitch envelope both wanted
    /// it and the filter won -- the collision the format cannot resolve.
    int filter_envelopes = 0;
    int pitch_envelopes = 0;
    int shared_mod_envelopes = 0;
    double filter_fit_sum = 0.0;
    double pitch_fit_sum = 0.0;

    [[nodiscard]] double mean_fit_error() const noexcept
    {
        return fitted_zones > 0 ? fit_error_sum / fitted_zones : 0.0;
    }
};

/// Lays out the whole sound set as an SF2 bank.
///
/// The layout is ROM-aligned: a melodic tone *N* lands at bank word `(N >> 7) << 8`, program
/// `N & 0x7f`, and a drum kit *K* at `0x80 | ((K >> 7) << 8)`, program `K & 0x7f`. That makes a
/// preset number a stable name for a tone across all five vintage maps, so the `.sflist.json`
/// files can point at it without knowing how the bank was built, and a regenerated bank keeps its
/// numbering when unrelated tones change.
///
/// The bank word packs a *pair*: the reader takes the low seven bits as the bank MSB, the high byte
/// as the LSB, and bit 7 as the percussion flag. That is a spessasynth convention rather than
/// SF2 — the specification knows only banks 0–127 plus 128 for percussion — so a conforming reader
/// sees every page collapsed onto bank 0.
///
/// `levels` supplies the amplitude chain the static attenuation is taken from.
[[nodiscard]] BankBuild build_bank(const PatchDirectory& directory,
                                   const DrumKitTable& kits,
                                   const SampleSet& set,
                                   const TvaChain& levels,
                                   const TvfChain& filters,
                                   const PitchChain& pitches,
                                   const BankOptions& options = {});

} // namespace ts::sf2
