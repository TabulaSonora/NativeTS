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
/// **Three of these rows were widened once, and the reason has to be recorded or the ratchet means
/// nothing.** Teaching drums the kit's own `Rx.Note Off` bit, in place of the fixed ring timer this
/// port had invented, let long percussion ring for as long as the module rings it -- a crash cymbal
/// whose envelope the module runs for 4.7 seconds had been cut off at 1.8. That is a correction,
/// and it is verified against the module's own voice memory rather than inferred: see
/// \ref the-drum-ring-was-invented.
///
/// What it cost here is peaks. More drum voices overlap now, so `onestop`, `macross2` and `bigben`
/// each gained a little, and the rows below say how much. What it bought is level: **the RMS
/// deviation improved on eight of the nine songs it moved at all**, and `roland_allstars`'s band
/// error closed completely. Every row that improved was tightened in the same commit, so the table
/// is net tighter, not looser.
///
/// **The balance column was calibrated across the whole corpus once the fixture was made whole**
/// (2026-08-06). It had only ever been measured on the two songs whose rows carried it; every other
/// row inherited the 0.06 default it had never been checked against, and when the song fixture was
/// regenerated from 13 cases back to its full 31 the eleven rows below turned out to sit past it.
/// That is a column that was never calibrated rather than eleven regressions -- the fixture the
/// other four columns were measured against did not contain most of these songs.
///
/// Every bound here is `TS_STRICT_SONGS=1` plus 2%, which is the tightest the ratchet can be while
/// still being deterministic. **The column is net tighter, not looser**: `panwet` measured 0.185
/// against the 0.36 it was allowed and is halved here, and twenty of the corpus's thirty-one cases
/// need no row at all and are left on the default. Only `bad_apple` genuinely moved, 0.15 to 0.190.
///
/// Read the spread before reading any single row. Three songs sit together near 0.17-0.19 --
/// `panwet`, `bad_apple` and `test_poly_bend` -- and all three carry a reverb return and differ in
/// everything else, `test_poly_bend` reaching that figure on the GS default send alone with no
/// controllers and no chorus. Three songs at one figure by one shared route is one defect, not
/// three, and the row below says which route. `bigben` at 0.287 is alone at the top and is the
/// outlier to explain first; the seven Roland and control-matrix rows trail off from 0.146 to
/// 0.062 with no obvious join, and may be nothing more than the same lead at lower wet levels.
/// None of this is a diagnosis -- see the header: a row is a debt, not a dispensation.
constexpr std::array<KnownDeviation, 19> known_deviations{{
    // The two rows the stereo balance measure opened, and they are one lead rather than two. Both
    // sit where the send returns dominate and nowhere else: `panwet.mid` -- a one-note probe named
    // for exactly this -- agrees with the module **exactly** on its attack window, 0.7380 against
    // 0.7380, and only parts company through the body and tail, where this port pulls toward centre
    // while the module holds the voice's side. `bad_apple`'s worst two windows are its last two, at
    // -32 and -36 dB, which is its reverb tail and nothing else. The dry pan is not in question in
    // either; where the wet comes back is.
    {"panwet.mid", {1.0, 0.01, 3.0, 6.0, 0.189}, "wet return placement, not the dry pan"},
    {"bad_apple_feat_nomico_s__msgs.mid", {1.0, 0.01, 3.0, 6.0, 0.190},
     "wet return placement in the closing tail"},

    // The third song at very nearly the same figure, and it narrows the two above rather than
    // contradicting them. `test_poly_bend` sends no controllers at all -- one program change, two
    // SysEx, and bend -- so it runs at the GS defaults, which is reverb send 40 and chorus send 0.
    // A reverb return and no chorus return, deviating by 0.169 where `panwet` deviates by 0.185.
    // So the lead is the *reverb* return's placement specifically; chorus is not needed to produce
    // it. Nothing else about this file deviates at all, which is what makes it the cleanest of the
    // three to measure against.
    {"test_poly_bend.mid", {1.0, 0.01, 3.0, 6.0, 0.173}, "lead; reverb return placement"},

    // XG, and inside every default but the peak: measured against the module at -0.28 dB RMS,
    // 0.95 dB on the worst octave band and 1.20 dB on the worst envelope window, which is
    // better than most of this corpus. The peak is 0.051 out, in line with canyon at map 4
    // (0.054) and better than transcendental (0.106) -- see the note on the field for why a
    // whole song's peak is the loosest of the four. Nothing else here is owed.
    {"MAKORO.MID", {1.0, 0.06, 3.0, 6.0}, "XG; peak only, and a peak is a single sample"},

    {"shangai.mid", {1.6, 0.04, 20.5, 8.5, 0.151},
     "lead; CC1/CC2 pointed at a CC the file never sends"},
    {"macross2.mid", {2.3, 0.065, 11.0, 7.3, 0.085},
     "lead; CC1/CC2 routes assigned at neutral depth"},
    {"ff5_1_16_harvest.mid", {1.0, 0.01, 4.1, 6.0}, "lead; CC1 route is driven but not in the deviating bands"},

    // The corpus's worst stereo deviation by a clear margin -- 0.287 where the next is 0.190 -- and
    // it is not obviously the same lead as the three above. Worth noting that this is also the row
    // carrying the worst 63 Hz band: a song can be light in the low end and misplaced across the
    // image for one reason or for two, and nothing measured here yet says which.
    {"bigben.mid", {1.6, 0.03, 8.7, 6.0, 0.294},
     "lead; worst 63 Hz band, worst stereo balance, and clicks on note boundaries"},
    {"it_must_have_been_love.mid", {1.7, 0.025, 3.0, 6.0}, "lead"},
    {"rainy.mid", {1.3, 0.03, 3.3, 6.0, 0.080}, "lead"},

    // **Both of these click on note boundaries, and the module does not.** Heard rather than
    // measured, and that is the point worth recording: `dreaming_i_was_dreaming` is inside every
    // bound on this table and still audibly wrong, so its row is a debt the numbers here cannot
    // see. A click is a step discontinuity -- broadband, and over in a sample or two. None of the
    // five measures can catch that. The octave bands average across a window a quarter of the way
    // in, the RMS envelope is 64 windows over a whole song, and peak is a single sample that a
    // click can raise without anything else moving. `bigben` is the one where it shows at all: its
    // peak reads 0.791 against the module's 0.716, which is what a transient the module does not
    // have looks like from the outside.
    //
    // So this is a lead for the *engine* and a gap in the *gate* at once, and neither should wait
    // on the other. Somewhere a level or a phase is stepping where the module ramps -- a voice
    // starting without its envelope's first ramp applied, or a steal cutting one without release.
    // Whatever finds it will need a measure that sees sample-to-sample discontinuity, since by
    // construction nothing here will.
    {"dreaming_i_was_dreaming.mid", {1.0, 0.02, 3.0, 6.0}, "lead; clicks on note boundaries"},
    {"onestop.mid", {1.0, 0.015, 3.0, 6.0}, "peak only, and only since drums ring their full length"},

    {"roland_sc88_y03.mid", {5.0, 0.33, 9.8, 7.0}, "bass missing, same at every map"},
    {"roland_suplex.mid", {1.5, 0.24, 8.2, 14.5, 0.146}, "one passage plays differently"},
    {"roland_sc88_y05.mid", {1.2, 0.2, 3.0, 6.0, 0.064}, "lead"},
    {"roland_sc55_demo13.mid", {1.0, 0.06, 5.5, 6.0, 0.109}, "lead"},
    {"roland_sc55_demo03.mid", {1.4, 0.03, 3.0, 6.0, 0.087}, "lead"},
    // The band row reopened at 3.046 dB in the 2 kHz octave when the LFO nodes started taking their
    // seed from the generator instead of a discard. Held at 3.1 rather than the default 3.0 because
    // the change is measured correct against the module and this is the only row it moved the wrong
    // way -- by a twentieth of a dB, while `rainy`'s balance improved and two other songs' balance
    // rows closed outright. Tighten it back to the default when the 2 kHz gap is understood.
    {"roland_allstars.mid", {1.0, 0.02, 3.1, 6.0}, "peak, and 2 kHz by 0.05 dB since the LFO seed"},
    {"roland_deadend.mid", {1.0, 0.1, 3.0, 6.0}, "lead"},
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
        options.map = static_cast<ToneMap>(entry.at("map").get<int>());
        ToneGenerator generator{notes, options};
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

    if (compared == 0) {
        SKIP("No oracle case had both its fixture and its MIDI file (" << unavailable
             << " the oracle could not render).");
    }
}
