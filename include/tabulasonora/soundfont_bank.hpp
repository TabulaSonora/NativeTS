#pragma once

#include "tabulasonora/drum_kit_table.hpp"
#include "tabulasonora/patch_directory.hpp"
#include "tabulasonora/soundfont_samples.hpp"
#include "tabulasonora/soundfont_writer.hpp"
#include "tabulasonora/tva_chain.hpp"

#include <string>

namespace ts::sf2 {

/// Knobs the bank layout exposes. The defaults are what the export is meant to produce.
struct BankOptions {
    std::string name = "Sound Canvas";
    std::string software;
    std::string comment;
    /// The wave ROM's native rate. Everything is stored at it; nothing is resampled.
    int sample_rate = 32000;
};

/// A built bank and the counts worth reporting about it.
struct BankBuild {
    Bank bank;
    int melodic_presets = 0;
    int drum_presets = 0;
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
                                   const BankOptions& options = {});

} // namespace ts::sf2
