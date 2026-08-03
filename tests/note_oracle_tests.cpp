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
#include <iomanip>
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
    double pitch_hz = 0.0;
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
                              int rate,
                              double target_hz)
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
    metrics.pitch_hz = testmetrics::fundamental(mono, rate, target_hz, /*start_fraction=*/0.0);
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
    /// Tuning, in cents. **This default is a debt, not a standard.** Two engines rendering the same
    /// note from the same wave should agree to a small fraction of a cent -- `Trombone` at the
    /// SC-88Pro map agrees with the module's own playback ratio to 0.001, so the machinery can --
    /// and the median here is +2.2 cents with only 25 of 177 cases inside one. It is set at what
    /// this engine currently manages so that nothing gets *worse* while the cause is found; see
    /// \ref the-engine-plays-sharp. Tighten it, do not live with it.
    ///
    /// Part of the spread is the measurement rather than the engine: a 0.5 Hz bin is 13 cents wide
    /// at the sweep's lowest key, so the estimator is least able to speak exactly where the notes
    /// are lowest. That is why `Tenor Sax`'s row is what it is.
    double pitch_cents = 9.5;
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
///  - **Patches that deviate in *time* while their spectrum and level agree**, which is **beating
///    between two detuned partials, at the wrong rate**. `Bass & Lead` was measured: the module's
///    two partials sit at 1043.8 and 1045.5 Hz, 1.7 Hz apart, and this engine's at 1045.9 and
///    1048.3 Hz, 2.4 Hz apart. Their envelopes then modulate at 1.75 and 2.33 Hz respectively --
///    the beat, not an LFO -- so the dips land in different windows and the envelope metric reads
///    12 dB while the spectrum stays inside 2.6 dB and the level inside 0.5 dB.
///
///    It looked like an LFO starting phase and is not. `Bass & Lead`'s LFO1 carries a TVA depth of
///    682 against a 0x7F00 full scale, which is 0.18 dB, and runs at 6 Hz -- too shallow to see and
///    far too fast to survive an 87 ms RMS window. Traced live with `scdec lfotrace`.
///
///    The beat is wrong because **the partials are**: both of this engine's sit sharp of the
///    module's, by 3.5 and 4.6 cents, and a detune error is a beat-rate error. See
///    `docs/articles/verification.md` for how far that generalises -- it is not confined to this
///    patch, and it is the largest thing this gate found.
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
constexpr std::array<KnownDeviation, 11> known_deviations{{
    {11, {1.0, 0.06, 3.0, 6.0}, "Vibraphone; peak alone -- level, spectrum and envelope all pass"},
    {24, {1.6, 0.01, 7.0, 11.5}, "Nylon Gt.; decays about 1.5x too fast, 20 dB into the tail"},
    {38, {1.3, 0.01, 3.0, 11.5}, "Syn.Bass 1; beat rate, and the sweep's highest noise floor"},
    {40, {1.0, 0.01, 3.0, 6.0, 10.5}, "Violin; tuning only"},
    {46, {1.0, 0.01, 3.0, 6.0, 12.0}, "Harp; tuning only"},
    {66, {1.0, 0.04, 3.0, 6.0, 24.5}, "Tenor Sax; 23 cents, and only on the 65 Hz key"},
    {78, {2.5, 0.13, 23.0, 6.0}, "Whistle; lead -- 21 dB in a band the note reaches"},
    {87, {1.0, 0.02, 3.0, 13.5}, "Bass & Lead; beats at 2.33 Hz where the module beats at 1.75"},
    {99, {1.6, 0.01, 4.5, 9.0, 11.0}, "Atmosphere; noise layer"},
    // Unpitched. The estimator finds the loudest peak near the note in each render and there is no
    // fundamental there to find, so what these two compare is one noise peak against another. The
    // bound says "not checked" out loud rather than skipping them where nobody would notice.
    {118, {1.0, 0.045, 7.0, 6.0, 110.0}, "Synth Drum; unpitched -- the pitch bound is not a check"},
    {122, {1.0, 0.01, 6.0, 6.0, 38.0}, "Seashore; unpitched -- the pitch bound is not a check"},
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
                                  int rate,
                                  const fs::path& audio_path = {})
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

    // Interleaved float32, byte for byte what `notebatch` wrote for the oracle, so the two renders
    // of a failing case can be measured against each other rather than only counted apart.
    if (!audio_path.empty()) {
        std::ofstream out{audio_path, std::ios::binary};
        for (std::size_t i = 0; i < left.size(); ++i) {
            const std::array<float, 2> frame{left[i], right[i]};
            out.write(reinterpret_cast<const char*>(frame.data()), sizeof(frame));
        }
    }

    return measure(left, right, rate, 440.0 * std::pow(2.0, (which.note - 69) / 12.0));
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
    std::size_t pitched = 0;
    std::size_t signal_bands = 0;
    std::size_t floor_bands = 0;

    for (const auto& entry : cases) {
        const Case which{entry.at("program").get<int>(), entry.at("note").get<int>(),
                         entry.at("velocity").get<int>(), entry.at("map").get<int>()};
        const double hold = entry.at("hold").get<double>();

        INFO("program " << which.program << " note " << which.note << " velocity "
                        << which.velocity << " map " << which.map);

        // `TS_NOTE_AUDIO=<dir>` writes this engine's render of every case beside the oracle's, under
        // the same `caseNNNN.f32` name. What a failure means is rarely legible from four numbers,
        // and the oracle audio is already on disk for exactly this reason.
        fs::path audio_path;
        if (const char* directory = std::getenv("TS_NOTE_AUDIO"); directory != nullptr) {
            fs::create_directories(directory);
            // Split on either separator by hand: the fixture is written by whichever host generated
            // it, so a Windows-generated path is read back under Linux where `\` is an ordinary
            // character and `fs::path::filename` would hand back the whole string.
            const auto stored = entry.at("audio").get<std::string>();
            const auto cut = stored.find_last_of("/\\");
            audio_path = fs::path{directory}
                         / (cut == std::string::npos ? stored : stored.substr(cut + 1));
        }

        const Metrics ours = render_case(notes, which, hold, tail, rate, audio_path);

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

        // Tuning. The one measure here that is not about level or colour, and the only one that can
        // see a wrong playback ratio at all -- see `Deviation::pitch_cents`.
        //
        // Skipped when the module's own fundamental lands more than a semitone from the note, which
        // means the estimator did not lock onto a fundamental rather than that the module is out of
        // tune: `Synth Drum` and `Seashore` have no pitch to find, and one `Choir Aahs` case reads
        // 73 cents flat. Three cases of 180.
        const double expected_pitch = entry.value("pitchHz", 0.0);
        const double nominal = 440.0 * std::pow(2.0, (which.note - 69) / 12.0);
        if (expected_pitch > 0.0 && ours.pitch_hz > 0.0
            && std::abs(1200.0 * std::log2(expected_pitch / nominal)) < 50.0) {
            const double cents = 1200.0 * std::log2(ours.pitch_hz / expected_pitch);
            INFO("pitch " << ours.pitch_hz << " Hz vs " << expected_pitch << " (" << cents
                          << " cents)");
            CHECK(std::abs(cents) < allowed.pitch_cents);
            ++pitched;
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
        //
        // What it prints is this engine's own measurements, undifferenced. The fixture beside it
        // holds the module's, so anything worth asking -- a delta, a distribution, the shape of one
        // note's envelope against the module's -- can be asked afterwards without running again.
        if (std::getenv("TS_NOTE_REPORT") != nullptr) {
            const ResolvedTone resolved = notes.directory().resolve_midi(
                which.program, which.note, which.velocity, static_cast<ToneMap>(which.map), 0);

            // The playback ratio each partial starts at, `2^((base_pitch - native)/12000)`, and its
            // two terms. `scdec postrace` reads the module's sampler read position per control
            // tick, so the module's ratio is *exact* -- an integer step in 16.16 -- and these are
            // directly comparable to it. Read one tick at a time: a longer baseline crosses a loop
            // wrap and the position jumps backwards.
            //
            // This is how the tuning question narrowed. `Trombone` at the SC-88Pro map comes out
            // 1.079415269 here against the module's 1.079414431 -- 0.001 cents, an exact match --
            // which rules out any error in the shared formula, the `1024` neutral included. Three
            // other patches traced the same way disagree by 4, 31 and 41 milli-semitones. Whatever
            // is wrong is a per-patch term, not a constant.
            const auto tone_number = notes.directory().lut3_resolved(
                which.program, static_cast<ToneMap>(which.map), 0);
            const auto tone = tone_number ? notes.directory().tone(*tone_number) : std::nullopt;

            std::cout << "case " << which.program << ' ' << which.note << ' ' << which.velocity
                      << ' ' << which.map << " ratio";
            for (const ResolvedPartial& voice : resolved.partials) {
                const double native = (voice.descriptor.root_key * 1000.0) + 1024.0
                                      - voice.descriptor.fine_tune;
                double base = 0.0;
                int coarse = 0x40;
                int jitter = 0;
                int sustain = 0;
                int vibrato = 0;
                if (tone && voice.partial_index < static_cast<int>(tone->partials().size())) {
                    const PartialParameters& partial =
                        tone->partials()[static_cast<std::size_t>(voice.partial_index)];
                    base = notes.pitch().base_pitch_milli_semitones(partial, which.note,
                                                                    partial.key_center());
                    coarse = partial.coarse_tune_raw();
                    jitter = partial.raw()[0x1A];
                    if (const auto envelope = notes.pitch().envelope_offsets(
                            partial, std::clamp(which.note, 0, 0x7F), which.velocity)) {
                        sustain = envelope->targets[3];
                    }
                    const auto [lfo1, lfo2] = notes.lfo().configure(*tone_number, partial);
                    vibrato = std::abs(lfo1.pitch_depth) + std::abs(lfo2.pitch_depth);
                }
                // ratio / base_pitch / native / coarse byte / jitter depth / envelope sustain /
                // LFO pitch depth / loop length / root key / fine tune.
                //
                // This is the field that localised the tuning error, by elimination. Across the
                // whole sweep the jitter depth and the envelope sustain are zero for every partial
                // and the coarse byte is neutral for 162 of 177 cases, so none of the three can be
                // the cause; the LFO depth and partial count show the residual is not the pitch
                // estimator being confused by vibrato or beating; and the loop length bears no
                // integer relation to it, so it is not a loop off by a sample. What is left is
                // `base_pitch` and `native`, and `scdec portatrace` reads the module's own pitch
                // out of `voice+0x6c`: it agrees with `base_pitch` to within 5 milli-semitones and
                // is exact in half the cases, while the whole error reaches 45. It is `native`.
                std::cout << ' ' << std::setprecision(10) << std::pow(2.0, (base - native) / 12000.0)
                          << '/' << base << '/' << native << '/' << coarse << '/' << jitter
                          << '/' << sustain << '/' << vibrato << '/'
                          << (voice.descriptor.start - voice.descriptor.end) << '/'
                          << voice.descriptor.root_key << '/' << voice.descriptor.fine_tune
                          << std::setprecision(6);
            }
            std::cout << " pitch " << std::setprecision(10) << ours.pitch_hz << std::setprecision(6)
                      << " partials " << resolved.partials.size() << " rms "
                      << ours.rms << " peak " << ours.peak << " bands";
            for (const double band : ours.bands) {
                std::cout << ' ' << band;
            }
            std::cout << " env";
            for (const double window : ours.envelope) {
                std::cout << ' ' << window;
            }
            std::cout << " name " << resolved.name << '\n';
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

    // And that the pitch check is not quietly skipping the sweep. Measured at 177 of 180.
    INFO(pitched << " cases had a fundamental to compare");
    CHECK(pitched > cases.size() - 10);
}
