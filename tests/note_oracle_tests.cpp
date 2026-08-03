// The single-note gate against `SCCore.dll` itself, driven through its own exported API.
//
// The song gate next door asks whether a whole file comes out right. This asks the question one
// level down and answers it 180 times: does *this program*, on *this key*, at *this velocity*, on
// *this tone map* sound like the module? A song render averages every patch it touches into eight
// numbers, so a tone that resolves to the wrong wave can hide behind sixteen that resolve to the
// right one. Here nothing hides -- each case is one program on one key, and a failure names it.
//
// That makes this the diagnostic instrument for the song gate's open leads rather than a second
// opinion on them. `roland_sc88_y03` is 6.5 dB light at 63 Hz by the same amount at every tone map;
// whether that is a patch rendering wrong or a note never arriving is a question about single
// notes, and this is where it can be asked.
//
// What it said the first time it ran: level agrees to a median of 0.09 dB, the octave bands the
// note actually reaches to a median of 0.17 dB, and **27 of the 36 programs need no allowance at
// all**. The nine that do are in `known_deviations` below, and they are not a spread -- they are
// two named causes and one outlier.
//
// **This replaces the C# fixture as the authority, and does not replace the test.** The
// `[render][sccore][gate]` digest next door still compares against the archived engine bit for bit,
// which is a stronger check of *drift* than any tolerance can be -- but it is a check against a
// reimplementation that predates several fixes this port has since made, and 23 of its cases are
// already superseded for that reason. Where the two disagree, this one wins.

#include "render_metrics.hpp"
#include "test_data.hpp"

#include "tabulasonora/note_renderer.hpp"
#include "tabulasonora/rom_image.hpp"
#include "tabulasonora/tone_generator.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <string>
#include <vector>

using namespace ts;
using Catch::Matchers::WithinAbs;

namespace fs = std::filesystem;

namespace {

/// The bands the generator takes. Seven, starting at 125 Hz rather than the song gate's 63: a
/// single note has one fundamental, and below 125 Hz there is nothing to compare for most of them.
constexpr std::array<int, 7> band_centres{125, 250, 500, 1000, 2000, 4000, 8000};

struct Metrics {
    std::size_t frames = 0;
    double peak = 0.0;
    double rms = 0.0;
    std::vector<double> bands;
    std::vector<double> envelope;
};

/// Measured on the float samples directly, with no quantisation anywhere.
///
/// The song gate rounds to 16-bit first because its oracle metrics are taken from a WAV. This
/// oracle is raw interleaved float32 -- `notebatch` writes exactly what `TG_Process` produced -- so
/// rounding here would only add a difference that is not in either engine.
[[nodiscard]] Metrics measure(const std::vector<float>& left,
                              const std::vector<float>& right,
                              int rate)
{
    Metrics metrics;
    metrics.frames = left.size();

    std::vector<double> mono(left.size());
    double energy = 0.0;
    double peak = 0.0;
    for (std::size_t i = 0; i < left.size(); ++i) {
        const double l = left[i];
        const double r = right[i];
        peak = std::max({peak, std::abs(l), std::abs(r)});
        energy += l * l + r * r;
        mono[i] = (l + r) * 0.5;
    }

    metrics.peak = peak;
    // Over both channels, which is what the generator divides by: 2 samples per frame.
    metrics.rms = std::sqrt(energy / static_cast<double>(std::max<std::size_t>(1, left.size() * 2)));
    metrics.bands = testmetrics::octave_bands(mono, rate, band_centres, /*start_fraction=*/0.0);
    metrics.envelope = testmetrics::rms_envelope(mono, /*windows=*/32);
    return metrics;
}

/// How far below a note's own loudest band a band has to be before its level stops being a
/// statement about the instrument.
///
/// This threshold is the whole reason the gate says anything useful, and it was chosen from the
/// measurement rather than picked. Sorting all 1254 comparable bands by how far they sit below
/// their case's loudest one splits them cleanly in two: above this line the median disagreement
/// with the module is **0.17 dB** and the 95th percentile 1.8 dB; below it, disagreements reach
/// 31 dB. A band 60 dB down carries the window's leakage and the engine's own noise floor, not the
/// note -- comparing it to the module measures which engine has the quieter arithmetic.
constexpr double signal_band_range = 40.0;

/// What the bands below that line are held to instead.
///
/// Not nothing, because "we do not compare this" is how an artefact grows until it is audible.
/// The comparison that still means something there is against the note's *own* level: whatever is
/// in those bands must stay far enough under the note for no one to hear it. Worst measured is
/// 29.7 dB on `Syn.Bass 1`, and this engine's floor is the higher of the two -- by up to 31 dB in
/// the emptiest bands, which is the one broad finding this gate turned up.
constexpr double noise_floor_headroom = 25.0;

/// How far one program is currently allowed to sit from the module, and why.
///
/// A ratchet, not a target -- the same contract the song gate's table carries. Every bound is the
/// measured deviation plus a little headroom, so any of them getting worse fails the gate, and
/// closing one should be followed by tightening its row until it reaches the defaults.
struct Deviation {
    double rms_db = 1.0;
    double peak = 0.01;
    double band_db = 3.0;
    double envelope_db = 6.0;
};

struct Case {
    int program;
    int note;
    int velocity;
    int map;
};

struct KnownDeviation {
    int program;
    Deviation allowed;
    const char* cause;
};

/// The programs that do not yet match, with what is known about why.
///
/// **Keyed by program rather than by case, because that is where the defects actually live.** Every
/// one of these misses on several of its five keys and on more than one tone map, and a row per
/// case would be twenty-odd bounds all restating one fact about one patch. Reading it back the
/// other way is the point of the table: *Whistle is wrong*, not *these four renders are wrong*.
///
/// Twenty-seven of the thirty-six programs in the sweep need no row at all.
///
/// They fall into two groups, and the split is informative:
///
///  - **Patches that deviate in *time* while their spectrum and level agree.** `Bass & Lead` is the
///    clearest, and the only one where the cause is legible from the numbers alone: the module's
///    envelope dips 8 dB partway through the note and recovers, which is tremolo, and this engine's
///    dip lands in a different window -- 12 dB apart at the worst one while the spectrum stays
///    inside 2.6 dB and the level inside 0.5 dB. Nothing about the *tone* is wrong. That points at
///    the same unresolved LFO starting phase the song gate names for the effect LFOs, met one level
///    down where it is much easier to look at. `Nylon Gt.` and `Syn.Bass 1` deviate the same way
///    but 20 dB into the decay, where a release rate would do it too; `Vibraphone` and `Tenor Sax`
///    miss on the peak alone and pass everything else, which is weaker evidence still. Only the
///    first is diagnosed. The rest are grouped by *shape*, which is a hypothesis, not a finding.
///  - **Patches with a noise component**: `Whistle`, `Synth Drum`, `Seashore`, `Atmosphere`. The
///    obvious explanation is that the shared pseudo-random source is at a different point when the
///    note starts, and **that was measured and is not it** -- returning the generator to its seed
///    for every case, which is what `render_case` now does, moves these four by hundredths of a dB
///    and makes `Whistle` slightly *worse*. Whatever the cause is, it is not the generator's phase
///    between cases. `Whistle` is the outlier of the four by a wide margin: 21 dB in a band the
///    note genuinely reaches and 2 dB of overall level. It is the sharpest lead this gate produced.
///
/// `TS_STRICT_NOTES=1` holds every program to the defaults, which is how a row's current deviation
/// is read off when it is due to be tightened. Not a test mode -- a ruler.
constexpr std::array<KnownDeviation, 9> known_deviations{{
    {11, {1.0, 0.06, 3.0, 6.0}, "Vibraphone; peak alone -- level, spectrum and envelope all pass"},
    {24, {1.6, 0.01, 7.0, 11.5}, "Nylon Gt.; envelope, 20 dB into the decay tail"},
    {38, {1.3, 0.01, 3.0, 11.5}, "Syn.Bass 1; envelope in the tail, and the sweep's highest floor"},
    {66, {1.0, 0.04, 3.0, 6.0}, "Tenor Sax; peak alone -- level, spectrum and envelope all pass"},
    {78, {2.5, 0.13, 23.0, 6.0}, "Whistle; lead -- 21 dB in a band the note reaches"},
    {87, {1.0, 0.02, 3.0, 13.5}, "Bass & Lead; envelope dip in the wrong window, spectrum inside 2.6 dB"},
    {99, {1.6, 0.01, 4.5, 9.0}, "Atmosphere; noise layer"},
    {118, {1.0, 0.045, 7.0, 6.0}, "Synth Drum; noise layer"},
    {122, {1.0, 0.01, 6.0, 6.0}, "Seashore; noise is the whole patch"},
}};

[[nodiscard]] Deviation deviation_for(const Case& which)
{
    if (std::getenv("TS_STRICT_NOTES") == nullptr) {
        for (const KnownDeviation& entry : known_deviations) {
            if (entry.program == which.program) {
                return entry.allowed;
            }
        }
    }
    return Deviation{};
}

/// Renders one note the way `scdec notebatch` rendered it, because a comparison against a render
/// made a different way is not a comparison.
///
/// Every step here mirrors the harness: the same warm-up before anything is sent, the same six
/// controllers, the same 320-sample chunks with the note-off landing on a control tick. Through
/// `ToneGenerator` rather than `NoteRenderer::render_note`, for the same reason -- the oracle audio
/// came out of the DLL's whole pipeline, and the standalone note renderer is a different signal
/// path with no part processing and no output stage. Comparing against it would measure the gap
/// between two architectures and call it a defect.
[[nodiscard]] Metrics render_case(NoteRenderer& notes,
                                  const Case& which,
                                  double hold,
                                  double tail,
                                  int rate)
{
    // The pseudo-random source lives in the renderer, which outlives the generator, so a fresh
    // `ToneGenerator` does *not* return it to its seed -- and the sweep shares one renderer. Without
    // this, every case's random content would depend on the 179 that could run before it, which is
    // the one thing the harness says it is avoiding: "a full reset between cases: a fixture case
    // must not depend on what preceded it."
    notes.noise().reset();

    ToneGeneratorOptions options;
    // One port and the hardware's 64 voices: the harness drives `TG_ShortMidiIn`, which can only
    // reach port A, and a single note never approaches the voice limit anyway.
    options.ports = 1;
    options.map = static_cast<ToneMap>(which.map);
    ToneGenerator generator{notes, options};

    // The harness renders 8 x 512 frames after the reset and throws them away. Nothing sounds yet,
    // so this is not warm-up in the audible sense -- it is the effect state the DLL settles into
    // before the note arrives, and mirroring it costs nothing.
    std::vector<float> discard_left(512);
    std::vector<float> discard_right(512);
    for (int i = 0; i < 8; ++i) {
        generator.render(discard_left, discard_right);
    }

    generator.send_channel(0xB0, 0, 0);    // bank select MSB
    generator.send_channel(0xB0, 32, 0);   // bank select LSB
    generator.send_channel(0xB0, 7, 127);  // part volume
    generator.send_channel(0xB0, 10, 64);  // pan centre
    generator.send_channel(0xB0, 91, 0);   // reverb send off
    generator.send_channel(0xB0, 93, 0);   // chorus send off
    generator.send_channel(0xC0, which.program, 0);

    const auto total = static_cast<std::size_t>((hold + tail) * rate);
    const auto off_at = static_cast<std::size_t>(hold * rate);

    std::vector<float> left(total);
    std::vector<float> right(total);

    generator.send_channel(0x90, which.note, which.velocity);
    std::size_t position = 0;
    bool released = false;
    while (position < total) {
        if (!released && position >= off_at) {
            generator.send_channel(0x80, which.note, 0);
            released = true;
        }
        const std::size_t count =
            std::min<std::size_t>(NoteRenderer::control_block, total - position);
        generator.render(std::span{left}.subspan(position, count),
                         std::span{right}.subspan(position, count));
        position += count;
    }

    return measure(left, right, rate);
}

} // namespace

TEST_CASE("a single note matches the reference DLL's own render", "[note][oracle][sccore][gate]")
{
    const fs::path index_path = testdata::repository_root() / "fixtures" / "note_renders_oracle.json";
    if (!fs::exists(index_path)) {
        SKIP("No oracle note fixtures. Generate them with:\n"
             "  python3 tools/dump_note_renders_oracle.py <SCCore.dll> "
             "fixtures/note_renders_oracle.json --scdec <path to scdec>");
    }

    const RomImage rom =
        RomImage::open(testdata::require_sccore().string(), RomVerification::quick);
    NoteRenderer notes{rom};

    std::ifstream stream{index_path};
    REQUIRE(stream);
    const nlohmann::json document = nlohmann::json::parse(stream);
    REQUIRE(document.at("dllSha256").get<std::string>() == rom.manifest().dll().sha256);

    const int rate = document.at("sampleRate").get<int>();
    REQUIRE(rate == ToneGenerator::sample_rate);
    const double tail = document.at("tailSeconds").get<double>();

    const auto& cases = document.at("cases");
    REQUIRE(cases.size() >= 100);

    // A row for a program the sweep no longer contains is a bound nothing is held to, and it would
    // sit there looking like a known defect forever.
    for (const KnownDeviation& known : known_deviations) {
        INFO("deviation row for program " << known.program << " (" << known.cause << ")");
        CHECK(std::any_of(cases.begin(), cases.end(), [&](const nlohmann::json& entry) {
            return entry.at("program").get<int>() == known.program;
        }));
    }

    std::size_t compared = 0;
    std::size_t sounding = 0;
    std::size_t signal_bands = 0;
    std::size_t floor_bands = 0;

    for (const auto& entry : cases) {
        const Case which{entry.at("program").get<int>(), entry.at("note").get<int>(),
                         entry.at("velocity").get<int>(), entry.at("map").get<int>()};
        const double hold = entry.at("hold").get<double>();

        INFO("program " << which.program << " note " << which.note << " velocity "
                        << which.velocity << " map " << which.map);

        const Metrics ours = render_case(notes, which, hold, tail, rate);

        // Length is exact here, unlike the song gate's: the test chooses how many frames to render
        // rather than a sequence deciding when it ends. What this asserts is that both sides
        // account for hold and tail the same way, which is worth one line to be sure of.
        CHECK(ours.frames == entry.at("frames").get<std::size_t>());

        const Deviation allowed = deviation_for(which);

        const double expected_peak = entry.at("peak").get<double>();
        INFO("peak " << ours.peak << " vs " << expected_peak << " (" << (ours.peak - expected_peak)
                     << ")");
        CHECK_THAT(ours.peak, WithinAbs(expected_peak, allowed.peak));

        // A reference that is silent has no level to agree with, and the ratio would divide by
        // zero. Every case in the current sweep sounds, so this is a guard rather than a filter.
        const double expected_rms = entry.at("rms").get<double>();
        if (expected_rms > 1e-6) {
            const double rms_db = 20.0 * std::log10(ours.rms / expected_rms);
            INFO("rms " << rms_db << " dB");
            CHECK(std::abs(rms_db) < allowed.rms_db);
            ++sounding;
        }

        // Spectrum, in two halves -- see `signal_band_range` for why the split exists and where the
        // line came from. A band below -60 dB is empty on any reading and is neither.
        const auto expected_bands = entry.at("bands").get<std::vector<double>>();
        REQUIRE(ours.bands.size() == expected_bands.size());
        const double loudest = *std::max_element(expected_bands.begin(), expected_bands.end());
        const double our_loudest = *std::max_element(ours.bands.begin(), ours.bands.end());

        for (std::size_t band = 0; band < ours.bands.size(); ++band) {
            if (expected_bands[band] < -60.0) {
                continue;
            }
            INFO("band " << band_centres[band] << " Hz: " << ours.bands[band] << " vs "
                         << expected_bands[band] << ", "
                         << (loudest - expected_bands[band]) << " dB below the note's loudest");

            if (loudest - expected_bands[band] > signal_band_range) {
                CHECK(our_loudest - ours.bands[band] > noise_floor_headroom);
                ++floor_bands;
                continue;
            }
            CHECK(std::abs(ours.bands[band] - expected_bands[band]) < allowed.band_db);
            ++signal_bands;
        }

        // Shape over time -- for one note, this is the envelope machine, and it is the measure most
        // likely to catch a real defect here. A wrong attack rate, a release that decays too fast,
        // a segment that never fires: none of them move the spectrum much and all of them move
        // this.
        const auto expected_envelope = entry.at("envelope").get<std::vector<double>>();
        REQUIRE(ours.envelope.size() == expected_envelope.size());
        double worst = 0.0;
        std::size_t worst_window = 0;
        for (std::size_t window = 0; window < ours.envelope.size(); ++window) {
            if (expected_envelope[window] < -60.0) {
                continue;
            }
            const double difference = std::abs(ours.envelope[window] - expected_envelope[window]);
            if (difference > worst) {
                worst = difference;
                worst_window = window;
            }
        }
        INFO("worst envelope window " << worst_window << ": " << worst << " dB");
        CHECK(worst < allowed.envelope_db);

        // The ruler. 180 cases is too many to read through failures alone -- what a bound should be
        // is a question about the whole distribution, not about the cases that happen to miss.
        if (std::getenv("TS_NOTE_REPORT") != nullptr) {
            std::cout << "case " << which.program << ' ' << which.note << ' ' << which.velocity
                      << ' ' << which.map << " rms "
                      << (expected_rms > 1e-6 ? 20.0 * std::log10(ours.rms / expected_rms) : 0.0)
                      << " peak " << (ours.peak - expected_peak) << " bands";
            for (std::size_t band = 0; band < ours.bands.size(); ++band) {
                std::cout << ' '
                          << (expected_bands[band] < -60.0
                                  ? 0.0
                                  : ours.bands[band] - expected_bands[band]);
            }
            std::cout << " env " << worst << ' ' << worst_window << ' '
                      << (ours.envelope[worst_window] - expected_envelope[worst_window]) << " name "
                      << notes.directory()
                             .resolve_midi(which.program, which.note, which.velocity,
                                           static_cast<ToneMap>(which.map), 0)
                             .name
                      << '\n';
        }

        ++compared;
    }

    CHECK(compared == cases.size());
    // Guard against passing vacuously: a sweep of silent notes would agree perfectly.
    CHECK(sounding == compared);

    // And guard `signal_band_range` against quietly swallowing the comparison. If a change ever
    // pushes most bands below the line, this gate would go on passing while checking almost
    // nothing. Measured at 837 against the module and 417 held to the floor.
    INFO(signal_bands << " bands compared against the module, " << floor_bands
                      << " held to the noise floor");
    CHECK(signal_bands > floor_bands);
    CHECK(signal_bands > 700);
}
