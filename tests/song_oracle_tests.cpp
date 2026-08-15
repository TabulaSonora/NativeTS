// The whole-song gate against `SCCore.dll` itself, driven through its own exported API.
//
// This is the tier the verification article calls *authoritative*: agreement with the black box
// rather than with another reimplementation. The fixtures come from `tools/dump_song_renders_oracle.py`,
// which renders the corpus through the reference DLL with the spec repository's `scdec` harness.
//
// **Tolerances, not a digest, and for a reason that is a property of the problem.** A song runs the
// chorus and reverb, whose LFOs the reference starts at a phase this port cannot yet derive, so two
// renders that agree on every note still diverge sample by sample -- whole-song correlation sits
// near 0.18 while every octave band agrees to a tenth of a dB. Sample identity is therefore not
// merely out of reach, it is the wrong question. What is compared is what `COMPARING_RENDERS.md`
// argues actually separates a good render from a bad one: the length exactly, then level, spectrum
// and a coarse envelope that catches a note going missing or arriving late without seeing phase.

#include "render_metrics.hpp"
#include "test_data.hpp"

#include "tabulasonora/note_renderer.hpp"
#include "tabulasonora/rom_image.hpp"
#include "tabulasonora/sequence_player.hpp"
#include "tabulasonora/tone_generator.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace ts;
using Catch::Matchers::WithinAbs;

namespace fs = std::filesystem;

namespace {

/// The same conversion `wav::write` performs, because the oracle's metrics are taken from a 16-bit
/// WAV and comparing a float render against them means quantising it the same way first.
[[nodiscard]] std::int16_t to_pcm16(float sample) noexcept
{
    const double scaled = std::clamp(static_cast<double>(sample) * 32767.0, -32768.0, 32767.0);
    return static_cast<std::int16_t>(scaled);
}

struct Metrics {
    std::size_t frames = 0;
    double peak = 0.0;
    double rms = 0.0;
    std::vector<double> bands;
    std::vector<double> envelope;
    std::vector<double> balance;
};

/// The bands the generator takes. Eight, down to 63 Hz: a song has a bass line and a single note
/// mostly does not, which is why the note gate's list starts one octave higher.
constexpr std::array<int, 8> band_centres{63, 125, 250, 500, 1000, 2000, 4000, 8000};

/// A quarter of the way in rather than at the start, because the opening of a song is often silence
/// and a spectrum of silence compares equal to any other silence.
constexpr double band_window_start = 0.25;

[[nodiscard]] Metrics measure(const std::vector<std::int16_t>& left,
                              const std::vector<std::int16_t>& right,
                              int rate)
{
    Metrics metrics;
    metrics.frames = left.size();

    std::vector<double> mono(left.size());
    std::vector<double> left_signal(left.size());
    std::vector<double> right_signal(left.size());
    double energy = 0.0;
    int peak = 0;
    for (std::size_t i = 0; i < left.size(); ++i) {
        peak = std::max({peak, std::abs(static_cast<int>(left[i])),
                         std::abs(static_cast<int>(right[i]))});
        left_signal[i] = static_cast<double>(left[i]) / 32768.0;
        right_signal[i] = static_cast<double>(right[i]) / 32768.0;
        mono[i] = (left_signal[i] + right_signal[i]) * 0.5;
        energy += mono[i] * mono[i];
    }

    metrics.peak = static_cast<double>(peak) / 32768.0;
    metrics.rms = std::sqrt(energy / static_cast<double>(std::max<std::size_t>(1, mono.size())));
    metrics.bands = testmetrics::octave_bands(mono, rate, band_centres, band_window_start);
    metrics.envelope = testmetrics::rms_envelope(mono, /*windows=*/64);
    metrics.balance = testmetrics::balance_envelope(left_signal, right_signal, /*windows=*/64);
    return metrics;
}

/// How far one song is currently allowed to sit from the module, and why.
///
/// A ratchet, not a target. Widening the corpus from one file to eighteen turned a single 1.94 dB
/// figure into a list of concrete defects; recording each one is what keeps them visible and stops
/// them growing. Every bound is the measured deviation plus a little headroom, so any of them
/// getting worse fails the gate, and closing one should be followed by tightening its row until it
/// reaches the defaults.
///
/// A row here is a debt, not a dispensation. The songs with no row are held to the defaults, which
/// is where every row should end up.
struct Deviation {
    double rms_db = 1.0;
    /// Absolute, on a 0-1 scale. The loosest of the four by nature: the peak of a whole song is a
    /// single sample, so one voice stolen a moment earlier moves it while nothing else budges.
    double peak = 0.01;
    double band_db = 3.0;
    double envelope_db = 6.0;
    /// Worst window's stereo balance, as the right channel's share. 0.06 is the corpus's own worst
    /// once the eleven songs with rows below are set aside -- canyon at map 2, which is where the
    /// residual sits on every map. Twenty of the thirty-one cases are held to it.
    ///
    /// The one measure here that is not taken on the mono sum, and so the only one that can see the
    /// stereo image at all -- see `testmetrics::balance_envelope` for why the other four cannot.
    double balance = 0.06;
};

struct KnownDeviation {
    const char* song;
    Deviation allowed;
    const char* cause;
};

/// The songs that do not yet match, with what is known about why.
///
/// **Every row here is a lead rather than a diagnosis**, and three of them were mis-attributed in a
/// way worth recording. `shangai`, `macross2` and `ff5_1_16_harvest` all *assign* the matrix's CC1
/// or CC2 sources, so while those sources were unimplemented it looked obvious that this was why
/// they deviated. Implementing the sources -- correctly, and verified against the module -- moved
/// none of the three by a hundredth of a dB, and reading the files says why:
///
///  - `macross2` assigns both to pitch at depth **0x40**, which is the neutral value. The routes
///    are switched off by the file itself.
///  - `shangai` assigns both to amplitude at full depth and points them at CC#2, which it never
///    sends. Inert on the module too.
///  - `ff5_1_16_harvest` genuinely drives its route -- CC1 pointed at CC#11 and moved 4,335 times
///    -- but only on channel 1, where a cutoff sweep cannot reach the 63 and 125 Hz bands that are
///    what deviates.
///
/// Assigning a route is not driving it, driving it is not driving it *audibly*, and a scan that
/// records the first is evidence of neither. `tools/scan_midi_archive.py` now reports whether the
/// assigned controller is ever sent, so a corpus cannot be picked on inert routes again.
///
/// The sharpest lead is `roland_sc88_y03`: about 6.5 dB light at 63 Hz and 5 dB at 125 Hz, by the
/// same amount at every tone map, so it is not patch resolution -- some bass is not arriving.
/// `roland_suplex` has one envelope window 14 dB out while its spectrum stays close, which is the
/// signature of a passage that plays differently rather than a timbre that is wrong.
/// **Every bound in this table was re-measured on 2026-08-14, against a re-harvested oracle, and
/// the previous ones discarded.** They have to be read as one event rather than as twenty
/// regressions and corrections, because one thing changed and it was not this engine.
///
/// The harvest they had been fitted against was taken on 2026-08-07. The day after, the spec
/// project fixed how `scdec` hands events to the DLL: real `deltaFrames` with a flush before
/// enqueueing, and event times **rounded rather than truncated**. Re-harvesting on the current
/// harness changed **34 of the 36 song renders**, by up to 0.150 of peak, 0.29 dB of RMS and 4.4 dB
/// in one band of `macross2` -- comfortably past the tolerances below. The two it did not change say
/// why: `panwet.mid` is a one-note probe and `pchoral3.mid` renders as silence, and a single event
/// has nowhere to be misplaced to. The same re-harvest left all 239 note-oracle renders byte for
/// byte identical, which is worth knowing on its own: **a note gate cannot detect harness drift**,
/// so `fixture_manifest.json` and not this file is what carries that check.
///
/// **The corrected harness moved the reference toward this engine, not away.** Comparing each
/// song's peak against both harvests -- this engine's own render is the same in both comparisons,
/// so the difference isolates the harness -- **18 of 26 measurable cases moved closer, 6 moved
/// away, 2 did not move.** The worst offenders improved most: `macross2` from 0.0945 to 0.0147,
/// `roland_allstars` 0.0469 to 0.0086, `roland_deadend` 0.0490 to 0.0102. That is the evidence for
/// treating the new harvest as the better reference rather than merely the newer one.
///
/// So the table is **much** tighter. `roland_suplex`'s peak goes 0.24 to 0.021, `roland_sc88_y05`'s
/// 0.2 to 0.025, `roland_deadend`'s 0.1 to 0.011; `shangai`'s band column drops from 20.5 to the
/// default and `macross2`'s from 11.0. Four songs -- `bad_apple`, `test_poly_bend`,
/// `ff5_1_16_harvest` and `dreaming_i_was_dreaming` -- now sit inside every default and have no row
/// at all. Six songs that had none now need one, and three of those are the leads below.
///
/// **Two notes from removed rows, kept because the rows were carrying them.**
/// `ff5_1_16_harvest` is the corpus's only real test of the **per-key drum chorus plane**: its drum
/// channel selects bank LSB 1, the SC-55 map, and opens CC#93 to 127 while leaving CC#94 at 0, over
/// a kit whose per-key chorus depths are non-zero by default. Wiring reverb alone broke that row and
/// wiring all three fixed it, so it is the case to watch when `DrumKey::chorus` changes, row or no
/// row. And `dreaming_i_was_dreaming` **clicks on note boundaries where the module does not** --
/// heard, not measured. It is inside every bound here and still audibly wrong, which makes it a gap
/// in the gate as much as a lead for the engine: a click is a step discontinuity, broadband and over
/// in a sample or two, and none of these five measures can see one. The octave bands average over a
/// window, the RMS envelope is 64 windows across a whole song, and a peak is a single sample.
///
/// Every bound here is `TS_STRICT_SONGS=1` plus 2%, which is the tightest the ratchet can be while
/// still being deterministic. None of it is a diagnosis -- see the header: a row is a debt, not a
/// dispensation.
constexpr std::array<KnownDeviation, 21> known_deviations{{
    // **The chorus return, in decibels.** This is a one-note wet probe, and it is one of the two
    // renders the re-harvest did not touch, so both figures here are current and neither is a
    // harness artifact. It reads 9.25 dB light at 63 Hz and 4.49 dB at 125 Hz -- the two bands a
    // wet probe's return dominates -- and a chorus return short by 2.95x is 20*log10(1/2.95) =
    // **9.4 dB**. The band row and the chorus deficit are the same defect, agreeing to a fifth of a
    // decibel. Its balance is the same story seen sideways: this port pulls toward centre through
    // the body and tail while the module holds the voice's side, though it agrees with the module
    // **exactly** on the attack window, 0.7380 against 0.7380. Both close when the return does.
    {"panwet.mid", {1.0, 0.01, 9.44, 6.0, 0.155}, "the chorus return deficit, in dB and in balance"},

    // **XG, and one of the six the corrected harness moved *away* from this engine** -- 0.025 to
    // 0.036 of peak. The other is `th07_19_user_gm` below, at 0.047 to 0.103, and they are the
    // corpus's only two XG files. Eighteen GS songs moved closer on the same change. A more
    // accurate reference making both XG files worse and almost every GS file better is not a
    // tolerance question; it says the XG path places its events differently, and that is the row to
    // read alongside `th07` rather than either alone.
    {"MAKORO.MID", {1.0, 0.037, 3.0, 6.0}, "XG; moved away when the harness got more accurate"},

    {"shangai.mid", {1.0, 0.01, 3.0, 6.0, 0.128},
     "balance alone now; the CC1/CC2 routes it assigns are inert"},
    {"macross2.mid", {1.0, 0.015, 3.0, 6.0, 0.070},
     "was the corpus's worst band row at 11.0 dB; the harness held that error, not this engine"},

    // Still the corpus's worst stereo deviation, and still carrying a 63 Hz band, but less than
    // half what it was: 0.294 to 0.127. A song can be light in the low end and misplaced across the
    // image for one reason or for two, and nothing measured here yet says which.
    {"bigben.mid", {1.28, 0.01, 3.0, 6.0, 0.130}, "lead; worst stereo balance, and clicks"},
    {"it_must_have_been_love.mid", {1.0, 0.040, 3.0, 6.0}, "lead; peak alone"},
    {"rainy.mid", {1.43, 0.023, 3.0, 6.0, 0.091}, "lead"},
    {"onestop.mid", {1.0, 0.019, 3.0, 6.0}, "peak only, and only since drums ring their full length"},

    // **The bass arrived, and this row is tightened as the previous note asked.** Four of its five
    // columns are back to the defaults, from 5.0/0.33/9.8/7.0: the song was never light in the bass
    // at all, it was missing the +7 dB 200 Hz shelf it asks for in `40 02 01 47`, because parts
    // reset with their EQ switch off. See `Part::eq_enabled`. Measured after the fix, against the
    // module: rms 0.23 dB, peak 3.05e-05, worst envelope window 2.18 dB.
    //
    // Balance is the one column that had to move the other way, 0.0638 to 0.0896, and the reason is
    // known rather than guessed. The module takes a part's **reverb send after the EQ**: with
    // `40 4x 20` on, a +12 dB low shelf lifts the pure reverb tail by 4.89 dB, and with the switch
    // off the same shelf moves the tail by 0.00 dB. This engine filters only the dry bus and takes
    // its sends ahead of the filter, so its wet signal is unshaped and sits differently across the
    // image. That is a separate defect from the default, it is not fixed here, and this bound is
    // `TS_STRICT_SONGS=1` plus 2% like every other. Tighten it when the sends move behind the EQ.
    {"roland_sc88_y03.mid", {1.0, 0.01, 3.0, 6.0, 0.092},
     "sends are taken ahead of the EQ; the module takes them behind it"},
    // Its envelope column was 14.5 dB and its peak 0.24 -- the loosest two figures this table has
    // ever carried, and both were the harness. The envelope window is back to the default and the
    // peak is an eleventh of what it was. "One passage plays differently" was a reading of a
    // reference that placed that passage's events differently.
    {"roland_suplex.mid", {1.0, 0.021, 3.0, 6.0, 0.072}, "balance, and a much reduced peak"},
    {"roland_sc88_y05.mid", {1.0, 0.025, 3.0, 6.0}, "lead; peak alone, from 0.2"},
    {"roland_sc55_demo13.mid", {1.0, 0.019, 3.0, 6.0}, "lead; peak alone"},
    {"roland_sc55_demo03.mid", {1.35, 0.070, 3.0, 6.0, 0.089},
     "lead; one of the six that moved away, 0.040 to 0.068 of peak"},
    // The band row opened at 3.046 dB in the 2 kHz octave when the LFO nodes started taking their
    // seed from the generator instead of a discard, and the re-harvest widened it further to 3.56.
    // Still the only row where a change measured correct against the module moved this table the
    // wrong way. Tighten it back to the default when the 2 kHz gap is understood.
    {"roland_allstars.mid", {1.0, 0.01, 3.64, 6.0}, "2 kHz band; peak closed on the re-harvest"},
    {"roland_deadend.mid", {1.0, 0.011, 3.0, 6.0}, "lead; peak alone, from 0.1"},

    // **Six songs that needed no row before the re-harvest and need one now.** Their renders did not
    // change; the reference did, and on these six it moved away rather than toward. They are new
    // debts, not new excuses, and three of them are worth reading rather than counting.
    //
    // `canyon` is the sharpest. It is the file the bit-exact stream gate replays, which is a
    // *self*-baseline -- it reports that the render moved, never whether it should have -- so this
    // engine has been running 4.28 dB hot at 125 Hz and 5.09 dB at 8 kHz under the most-exercised
    // file in the suite, where by construction that gate could not see it. Both variants carry it
    // identically, which is expected: format 0 and format 1 of the same music.
    {"canyon.mid", {1.0, 0.046, 5.20, 6.0}, "hot at 125 Hz and 8 kHz; invisible to the stream gate"},
    {"canyon-format1.mid", {1.0, 0.046, 5.20, 6.0}, "the same music as `canyon.mid`, format 1"},

    // The corpus's second XG file, and the largest single deviation in this table. Its peak
    // **doubled away** from the module when the harness stopped truncating event times -- 0.047 to
    // 0.103 -- while eighteen GS songs improved. Read it with `MAKORO.MID` above.
    {"th07_19_user_gm.mid", {1.0, 0.105, 3.0, 6.0}, "XG; the largest deviation here, and it grew"},

    {"MIDI-Corona-Baby Baby.mid", {1.24, 0.022, 3.0, 6.0},
     "the portamento-ceiling file; rms and peak"},
    {"darkness3.mid", {1.0, 0.019, 3.0, 6.0}, "the bulk-dump file; peak alone"},
    {"rockarn12.mid", {1.0, 0.011, 3.0, 6.0}, "peak alone, and only just"},
}};

[[nodiscard]] Deviation deviation_for(const std::string& song)
{
    // `TS_STRICT_SONGS=1` holds every song to the defaults, which is how each row's current
    // deviation is measured when it is due to be tightened. Not a test mode -- a ruler.
    if (std::getenv("TS_STRICT_SONGS") == nullptr) {
        for (const KnownDeviation& entry : known_deviations) {
            if (song == entry.song) {
                return entry.allowed;
            }
        }
    }
    return Deviation{};
}

} // namespace

TEST_CASE("a whole song matches the reference DLL's own render", "[song][oracle][sccore][gate]")
{
    const fs::path index_path = testdata::repository_root() / "fixtures" / "song_renders_oracle.json";
    if (!fs::exists(index_path)) {
        SKIP("No oracle song fixtures. Generate them with:\n"
             "  python3 tools/dump_song_renders_oracle.py <SCCore.dll> "
             "fixtures/song_renders_oracle.json --scdec <path to scdec>");
    }

    const RomImage& rom = testdata::shared_rom();

    // Before anything is read out of it: prove this file is what the last regeneration wrote.
    // Fixtures are gitignored, so a stale one looks exactly like a current one.
    testdata::require_current_fixture(index_path);

    std::ifstream stream{index_path};
    REQUIRE(stream);
    const nlohmann::json document = nlohmann::json::parse(stream);
    REQUIRE(document.at("dllSha256").get<std::string>() == rom.manifest().dll().sha256);

    const int rate = document.at("sampleRate").get<int>();
    REQUIRE(rate == ToneGenerator::sample_rate);
    const double tail = document.at("tailSeconds").get<double>();

    // A row for a song that is no longer in the corpus is a bound nothing is held to, and it would
    // sit there looking like a known defect forever. Checked here so the table cannot rot.
    std::vector<std::string> corpus;
    for (const auto& entry : document.at("cases")) {
        corpus.push_back(entry.at("midi").get<std::string>());
    }
    for (const KnownDeviation& known : known_deviations) {
        INFO("deviation row for " << known.song << " (" << known.cause << ")");
        CHECK(std::find(corpus.begin(), corpus.end(), known.song) != corpus.end());
    }

    const auto& cases = document.at("cases");

    // Rendering the corpus is the whole cost of this gate -- it runs for minutes where the rest of
    // the suite together runs for about two seconds -- and the songs are independent of one
    // another, so they render across `TS_TEST_THREADS` workers here and are judged serially below.
    //
    // Nothing in this phase may assert. Catch2's macros are not thread-safe, and a `CHECK` firing
    // from a worker would race the reporter and attribute itself to whichever case the main thread
    // happened to be on. Each worker writes only to its own slot, which is why no lock appears.
    //
    // A `NoteRenderer` per song rather than per worker: it carries the noise source a render
    // mutates, so it cannot be shared, and building one is nothing beside rendering a song. Only
    // the ROM image is common, and that is immutable once open.
    struct Outcome {
        bool measured = false;
        bool unavailable = false;
        bool held_out = false;   ///< Skipped for length by `TS_FAST`, not missing.
        Metrics ours;
    };
    std::vector<Outcome> outcomes(cases.size());

    // Longest song first. The corpus is severely lopsided -- one file is 59% of its 3372 seconds --
    // so a worker that picks that one up last leaves every other worker idle while it finishes, and
    // the gate takes as long as it did serially. Starting it first bounds the wall clock at roughly
    // its own length instead. `frames` is what the fixture already records, and it does not have to
    // be exact: it only has to order the queue.
    std::vector<std::size_t> longest_first(cases.size());
    for (std::size_t index = 0; index < cases.size(); ++index) {
        longest_first[index] = index;
    }
    std::sort(longest_first.begin(), longest_first.end(),
              [&](std::size_t left, std::size_t right) {
                  return cases[left].value("frames", std::size_t{0})
                         > cases[right].value("frames", std::size_t{0});
              });

    testdata::parallel_for(cases.size(), [&](std::size_t slot) {
        const std::size_t index = longest_first[slot];
        const auto& entry = cases[index];

        // The oracle faults on some inputs -- an access violation inside the DLL, recorded rather
        // than hidden. Nothing here can be asserted about a render that does not exist.
        if (entry.value("oracleFailed", false)) {
            outcomes[index].unavailable = true;
            return;
        }

        // `TS_FAST` drops the long tail of the corpus, and the default is to render all of it.
        //
        // The corpus is severely lopsided: 6,924 seconds of audio across 34 songs, of which one
        // file is 1,993 -- `th07_19_user_gm.mid`, thirty-three minutes. The next longest is 314,
        // so a single threshold isolates it without naming it, and a threshold is the right shape
        // because the thing that costs wall clock is length rather than identity. Ten minutes sits
        // more than three times above everything else in the corpus and three times below that
        // one; nothing has to move when a song is added at either end.
        //
        // The default runs everything because this is the tier that settles correctness -- the two
        // cheaper ones can only report drift -- and a gate that quietly renders less than it claims
        // is worse than a slow one. `TS_FAST` exists for iterating on something else, and it says
        // so in the skip count.
        constexpr double fast_mode_seconds = 600.0;
        if (std::getenv("TS_FAST") != nullptr
            && entry.value("seconds", 0.0) > fast_mode_seconds) {
            outcomes[index].unavailable = true;
            outcomes[index].held_out = true;
            return;
        }

        const fs::path midi =
            testdata::repository_root() / "testdata" / entry.at("midi").get<std::string>();
        if (!fs::exists(midi)) {
            return;
        }

        // At the hardware's voice limit, which is the tier that is comparable to the DLL at all:
        // the reference has 64 voices and steals, so a render with more of them is measuring a
        // different instrument.
        //
        // **One port**, for the same reason. The harness drives the DLL through a single port, so
        // it folds a multi-port file's tracks onto sixteen channels; this engine's default of two
        // spreads them across thirty-two parts instead, which is a different arrangement of the
        // same notes and not a comparison at all. That default is right for playback and wrong
        // here. It went unnoticed while the corpus was one single-port file: the first multi-port
        // song added put the low band 10 dB out and the level 4 dB down, and setting this took the
        // worst band to 1.3 dB.
        NoteRenderer notes{rom};
        ToneGeneratorOptions options;
        options.ports = 1;
        // The module, not the correction to it. Everything in this file is measured against
        // `SCCore.dll`, and `extended_interpolation` is on by default precisely because it makes
        // this engine stop matching it -- see `ToneGeneratorOptions::extended_interpolation`. A
        // gate that left it on would be asserting the reference against a deliberate departure
        // from the reference.
        //
        // `TS_EXTENDED` turns it back on for a run, which is a measurement rather than a gate:
        // it answers how far the correction actually moves this corpus, and whether that distance
        // is inside the tolerances at all. Recorded in `docs/articles/verification.md`.
        options.extended_interpolation = std::getenv("TS_EXTENDED") != nullptr;

        // The module's own timing, for the same reason `extended_interpolation` is turned off
        // above: this file measures against `SCCore.dll`, so it wants the engine the module is,
        // not the engine that is convenient to write unit tests against.
        //
        // Both default off, and both belong on here. `ToneGeneratorOptions::event_delay_blocks`
        // says so outright -- "anything compared against the oracle wants these three on" -- and
        // the note gate has set them since it was written. This gate did not, so every song has
        // been compared against a reference whose events land 128 samples later than ours and
        // which carries an output stage we skipped. Measured directly on 2026-08-08: a note-on
        // sounds four 32-sample chunks earlier here than in the module, which is exactly the
        // staging `TG_ShortMidiIn` and `TG_Process` do before a message reaches a part.
        //
        // Not turned on by `TS_EXTENDED`-style opt-in, because unlike the interpolator these are
        // not a deliberate departure from the module -- they are the module.
        options.event_delay_blocks = 4;
        options.bypass_output_filter = false;
        options.map = static_cast<ToneMap>(entry.at("map").get<int>());
        ToneGenerator generator{notes, options};

        // Start the chorus LFO where the reference's was when this song began. Its accumulator is
        // never reset -- not by a GS reset, not by a macro change -- so its phase at the downbeat
        // is a function of how long that engine had been running first, and the harness prints the
        // value it reached. Comparing without this measures the offset rather than where the wet
        // is placed: rendering `panwet.mid` through the module at three warm-up lengths, changing
        // nothing else, moves its mean per-window balance error by a fifth.
        //
        // Absent on a fixture harvested before the generator recorded it, and then simply skipped:
        // an older fixture should still run rather than fail on a field it never had.
        if (const auto phase = entry.find("chorusLfoPhase"); phase != entry.end()) {
            generator.seed_chorus_phase(phase->get<int>());
        }

        SequencePlayer player = SequencePlayer::from_file(generator, midi);
        const RenderResult result = player.render_to_end(tail);

        std::vector<std::int16_t> left(result.left.size());
        std::vector<std::int16_t> right(result.right.size());
        std::transform(result.left.begin(), result.left.end(), left.begin(), to_pcm16);
        std::transform(result.right.begin(), result.right.end(), right.begin(), to_pcm16);

        outcomes[index].ours = measure(left, right, rate);
        outcomes[index].measured = true;
    });

    std::size_t compared = 0;
    std::size_t unavailable = 0;

    for (std::size_t index = 0; index < cases.size(); ++index) {
        const auto& entry = cases[index];
        const std::string name = entry.at("midi").get<std::string>();
        INFO("song " << name << " map " << entry.at("map").get<int>());

        if (outcomes[index].unavailable) {
            ++unavailable;
            continue;
        }
        if (!outcomes[index].measured) {
            continue;
        }

        const Metrics& ours = outcomes[index].ours;
        const auto expected_bands = entry.at("bands").get<std::vector<double>>();
        const auto expected_envelope = entry.at("envelope").get<std::vector<double>>();

        // Length first, because everything below is a distribution and comparing the distributions
        // of two renders of different lengths says nothing at all.
        //
        // Not exactly, though. The two sides round the same intent differently: the harness takes
        // `(int)((songSeconds + tail) * rate)` and this engine rounds its render up to the 32-sample
        // block grid it works on. On onestop that is 29 samples in 7.9 million, and a bound of one
        // control block keeps the check meaningful -- a tail that is actually missing is seconds
        // short, not milliseconds.
        const auto expected_frames = entry.at("frames").get<std::size_t>();
        const auto difference = static_cast<std::int64_t>(ours.frames)
                                - static_cast<std::int64_t>(expected_frames);
        INFO("length " << ours.frames << " vs " << expected_frames << " (" << difference << ")");
        CHECK(std::abs(difference) <= NoteRenderer::control_block);

        // Level. Measured on onestop at map 4: the peak agrees to 0.0009 of full scale and the RMS
        // to 0.05 dB. These bounds are an order of magnitude above that -- wide enough to survive
        // another machine's libm, narrow enough that a part sounding at the wrong volume, or an
        // effect send that stopped arriving, cannot pass.
        const Deviation allowed = deviation_for(name);

        const double peak_difference = ours.peak - entry.at("peak").get<double>();
        INFO("peak " << ours.peak << " vs " << entry.at("peak").get<double>() << " ("
                     << peak_difference << ")");
        CHECK_THAT(ours.peak, WithinAbs(entry.at("peak").get<double>(), allowed.peak));

        // A reference that is itself silent has no level to agree with, and the ratio would be a
        // division by zero -- `pchoral3` renders one LSB of peak on the module and nothing here,
        // which is two engines agreeing that a file makes no sound rather than a disagreement.
        // The bands already skip anything under -60 dB for the same reason; this is that guard,
        // applied to the level.
        const double reference_rms = entry.at("rms").get<double>();
        if (reference_rms > 1e-5) {
            const double rms_db = 20.0 * std::log10(ours.rms / reference_rms);
            INFO("rms " << rms_db << " dB");
            CHECK(std::abs(rms_db) < allowed.rms_db);
        }

        // Spectrum. Bands below -60 dB carry nothing worth comparing -- they are the noise floor
        // of a window that happened to land on a quiet passage.
        //
        // Measured on onestop at map 4, the worst band is the top one at 1.94 dB and every other is
        // inside half a dB. The top octave is where the residual lives, which is what you would
        // expect of an engine whose remaining gaps -- insertion EFX above all -- are bright.
        REQUIRE(ours.bands.size() == expected_bands.size());
        for (std::size_t band = 0; band < ours.bands.size(); ++band) {
            if (expected_bands[band] < -60.0) {
                continue;
            }
            INFO("band " << band_centres[band] << " Hz: " << ours.bands[band] << " vs "
                         << expected_bands[band]);
            CHECK(std::abs(ours.bands[band] - expected_bands[band]) < allowed.band_db);
        }

        // Shape over time. This is the one that catches a note that never sounds: it is deaf to
        // phase and to timbre, and loud about a passage that is not there. Worst window measured at
        // 1.72 dB, against a bound of six.
        REQUIRE(ours.envelope.size() == expected_envelope.size());
        double worst = 0.0;
        for (std::size_t window = 0; window < ours.envelope.size(); ++window) {
            if (expected_envelope[window] < -60.0) {
                continue;
            }
            worst = std::max(worst, std::abs(ours.envelope[window] - expected_envelope[window]));
        }
        INFO("worst envelope window: " << worst << " dB");
        CHECK(worst < allowed.envelope_db);

        // Where in the image, which nothing above can see: every other measure is taken on the mono
        // sum and a voice that moves across the field carries its energy with it. Skipped on the
        // same silence guard as the envelope -- a window with no signal has no balance, and the
        // generator writes 0.5 there rather than pretend otherwise.
        const auto expected_balance = entry.value("balance", std::vector<double>{});
        if (expected_balance.empty()) {
            WARN("fixture predates the stereo balance measure; regenerate "
                 "fixtures/song_renders_oracle.json to gate on it");
        } else {
            REQUIRE(ours.balance.size() == expected_balance.size());
            double worst_balance = 0.0;
            std::size_t worst_window = 0;
            for (std::size_t window = 0; window < ours.balance.size(); ++window) {
                if (expected_envelope[window] < -60.0) {
                    continue;
                }
                const double off = std::abs(ours.balance[window] - expected_balance[window]);
                if (off > worst_balance) {
                    worst_balance = off;
                    worst_window = window;
                }
            }
            INFO("worst balance window " << worst_window << ": " << ours.balance[worst_window]
                                         << " vs " << expected_balance[worst_window] << " ("
                                         << worst_balance << ")");
            CHECK(worst_balance < allowed.balance);
        }

        ++compared;
    }

    // A fast run has to say out loud that it rendered less than the gate claims to cover.
    const auto held_out = static_cast<std::size_t>(
        std::count_if(outcomes.begin(), outcomes.end(),
                      [](const Outcome& outcome) { return outcome.held_out; }));
    if (held_out != 0) {
        WARN("TS_FAST held out " << held_out << " song(s) over " << 600 << " s. This run is not "
             "the full gate; unset TS_FAST to render them.");
    }

    if (compared == 0) {
        SKIP("No oracle case had both its fixture and its MIDI file (" << unavailable
             << " the oracle could not render).");
    }
}
