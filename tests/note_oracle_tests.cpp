// The single-note gate against `SCCore.dll` itself, driven through its own exported API.
//
// The song gate next door asks whether a whole file comes out right. This asks the question one
// level down and answers it 239 times: does *this program*, on *this key*, at *this velocity*, on
// *this tone map* sound like the module? A song render averages every patch it touches into eight
// numbers, so a tone that resolves to the wrong wave can hide behind sixteen that resolve to the
// right one. Here nothing hides -- each case is one program on one key, and a failure names it.
//
// That makes this the diagnostic instrument for the song gate's open leads rather than a second
// opinion on them. `roland_sc88_y03` is 6.5 dB light at 63 Hz by the same amount at every tone map;
// whether that is a patch rendering wrong or a note never arriving is a question about single
// notes, and this is where it can be asked.
//
// **It could not be asked here until the sweep was widened, and saying it could was wrong.** Two
// gaps, both in exactly the place that lead lives: the bands began at 125 Hz, so nothing below
// 88 Hz was measured at all, and every case was melodic, so the drum kits -- which resolve through
// their own tables and hold the only sound in a GS arrangement with real energy under 90 Hz -- were
// not reached by any number of cases. The sweep now carries a 63 Hz band and 54 drum cases: nine
// kit programs, each of which every tone map defines and each of which resolves to a *different*
// kit on each map, across six keys.
//
// It can now be asked, and the answer is that no sound in the sweep is quiet enough to account for
// it. `Acoustic Bs.` -- which the file plays 324 times inside that band, and which was added to the
// sweep for this -- comes out within 0.2 dB at every key. The worst kick is 3.2 dB light and the
// median bass-carrying case is under one. See \ref widening-the-note-sweep.
//
// What it said the first time it ran: level agrees to a median of 0.09 dB, the octave bands the
// note actually reaches to a median of 0.17 dB, and 27 of the 36 programs needed no allowance at
// all. **Thirty-two of them do now**, and tuning agrees to a median of 0.09 cents -- the nine rows
// it opened with were largely one defect, and closing it emptied five of them. See
// `known_deviations` below and \ref the-exponential-is-the-modules.
//
// **This replaces the C# fixture as the authority, and does not replace the test.** The
// `[render][sccore][gate]` digest next door still compares against the archived engine bit for bit,
// which is a stronger check of *drift* than any tolerance can be -- but it is a check against a
// reimplementation that predates several fixes this port has since made, and 23 of its cases are
// already superseded for that reason. Where the two disagree, this one wins.

#include "render_metrics.hpp"
#include "test_data.hpp"

#include "tabulasonora/drum_kit_table.hpp"
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

/// The bands the generator takes, and the song gate's own list -- the same eight, so a level read
/// here and a level read there are the same measurement of the same thing.
///
/// This began at 125 Hz, on the reasoning that most single notes have nothing below it. That was
/// true and it was the wrong band to leave out. 63 Hz spans 44.5 to 89.1 Hz, which is where this
/// sweep's lowest key at 65.4 Hz puts its fundamental and where a bass drum puts most of its
/// energy -- so the one band the song gate has an open lead in was the one band this gate could not
/// see, and the claim that this was the instrument for that lead was not yet true.
constexpr std::array<int, 8> band_centres{63, 125, 250, 500, 1000, 2000, 4000, 8000};

/// Channel 10, where a program change selects a drum kit rather than a tone.
constexpr int drum_channel = 9;

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
        const double l = static_cast<double>(left[i]);
        const double r = static_cast<double>(right[i]);
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

/// The same idea applied to the envelope: how far below a case's own loudest window a window has to
/// be before its level stops describing the note.
///
/// The module does not decay to zero. Left alone it settles onto a floor -- `808 Snare 1` holds a
/// flat -90.5 dB for the last twenty-seven of its thirty-two windows, 64 dB under its own attack --
/// and this engine's voices do reach exactly zero. Comparing there measures the module's dither
/// against this port's silence, and reads as 908 dB of disagreement about a snare drum that both
/// engines finished playing a second and a half earlier.
///
/// Set deep enough to leave real tails alone: the crash cymbals stop 35 dB under their own attack,
/// which is well inside this and still fails, correctly.
constexpr double envelope_floor_range = 60.0;

/// Below this peak the module is not making a sound, and there is nothing to compare it against.
///
/// Exactly one case is under it, and it is not a defect: key 36 of the SFX kit at the SC-55 map is
/// an *undefined* kit entry -- tone 0xFFFF -- so this engine plays nothing, and the module answers
/// with a peak of 0.00003, which is its own noise floor rather than a note. The rest of the sweep
/// starts at 0.004, a factor of 136 away, so the line is drawn across an empty gap.
///
/// The case is kept rather than dropped from the sweep, and it is checked rather than skipped: what
/// it asserts is that this engine is silent too. An undefined kit key that started sounding would
/// be a real defect, and this is the only case in the sweep that could catch it.
constexpr double inaudible_peak = 1e-4;

/// How far one program or kit key is currently allowed to sit from the module, and why.
///
/// A ratchet, not a target -- the same contract the song gate's table carries. Every bound is the
/// measured deviation plus a little headroom, so any of them getting worse fails the gate, and
/// closing one should be followed by tightening its row until it reaches the defaults.
struct Deviation {
    double rms_db = 1.0;
    double peak = 0.01;
    double band_db = 3.0;
    double envelope_db = 6.0;
    /// Tuning, in cents.
    ///
    /// This was 9.5 and documented as a debt: the median case sat 2.2 cents sharp of the module and
    /// only 25 of them were inside one cent. The cause is found -- see \ref the-engine-plays-sharp
    /// -- and the median is now 0.09 cents with 119 of 155 inside one, so the bound is what a
    /// measurement can actually resolve rather than what the engine was managing.
    ///
    /// What is left is the measurement rather than the engine: a 0.5 Hz bin is 13 cents wide at the
    /// sweep's lowest key, so the estimator is least able to speak exactly where the notes are
    /// lowest, and every case still outside two cents is on a 65 Hz key. Five leaves that room and
    /// nothing else.
    double pitch_cents = 5.0;
};

struct Case {
    int program;
    int note;
    int velocity;
    int map;
    int channel;

    [[nodiscard]] bool drums() const noexcept { return channel == drum_channel; }
};

struct KnownDeviation {
    int program;
    Deviation allowed;
    const char* cause;
};

/// A drum row, keyed by the kit *and the key within it*.
///
/// The melodic table above argues against a row per case, and is right to: one tone covers all five
/// of a program's keys, so five rows would restate one fact five times. Drums are the other way
/// round. Key 36 of the TR-808 kit is a kick and key 42 of the same kit is a hi-hat -- different
/// tones, different waves, different defects -- and a row keyed by the kit alone would lend one the
/// other's bound. So the key is part of the key.
struct KnownDrumDeviation {
    int program;
    int note;
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
/// Thirty-two of the thirty-six programs in the sweep need no row at all.
///
/// **This table used to be twice as long, and what emptied it was one line of the engine.** Nine
/// melodic rows became four when the playback rate started going through the module's own
/// exponential table rather than around it -- see \ref the-engine-plays-sharp. The two families
/// those rows described were the same defect seen from either end:
///
///  - **Patches that deviated in *time* while their spectrum and level agreed**, which was beating
///    between two detuned partials at the wrong rate. `Bass & Lead` was the measured case: the
///    module's partials 1.7 Hz apart against this engine's 2.4 Hz, because both of ours sat sharp
///    by 3.5 and 4.6 cents and a detune error is a beat-rate error. Its partials now agree with the
///    module's to a tenth of a cent and its envelope is inside the default; what is left is its
///    peak. `Syn.Bass 1` and `Nylon Gt.` were the same story and are gone from the table entirely.
///  - **Patches read as out of tune**: `Violin`, `Harp`, `Tenor Sax` at 23 cents, `Atmosphere`.
///    All four are now inside a cent, and `Whistle` -- the row this file called the sharpest lead
///    it produced, 21 dB in a band the note reaches -- passes every default it was excused from.
///
/// What remains is genuinely something else: two peaks, one band, and two unpitched noise patches
/// whose pitch bound is not a check at all.
///
/// `TS_STRICT_NOTES=1` holds every program to the defaults, which is how a row's current deviation
/// is read off when it is due to be tightened. Not a test mode -- a ruler.
constexpr std::array<KnownDeviation, 6> known_deviations{{
    {11, {1.0, 0.06, 3.0, 6.0}, "Vibraphone; peak alone -- level, spectrum and envelope all pass"},
    {24, {1.0, 0.01, 3.6, 6.0}, "Nylon Gt.; one band, 37 dB under the note's loudest"},
    // Not a tuning error. `Square Wave` is two partials 137 units of the pitch word apart -- ten
    // cents -- and both of ours match the module's to one unit. What differs is which of the pair
    // each estimator calls the fundamental, and picking the other one is worth exactly the detune.
    // The bound is that detune plus headroom, so a real move still fails it.
    {80, {1.0, 0.01, 3.0, 6.0, 10.5}, "Square Wave; the estimator picks the other partial of the "
                                      "pair, which is the ten cents between them"},
    {87, {1.0, 0.02}, "Bass & Lead; peak alone -- the beat rate it missed is now the module's"},
    // Unpitched. The estimator finds the loudest peak near the note in each render and there is no
    // fundamental there to find, so what these two compare is one noise peak against another. The
    // bound says "not checked" out loud rather than skipping them where nobody would notice.
    //
    // Their band bounds widened when the 63 Hz band was added, and only there -- 10 dB on
    // `Synth Drum`, 12 on `Seashore`, on the two cases where the module puts real energy under
    // 90 Hz. Two unpitched noise patches disagreeing in the bottom octave is the same finding as
    // the rest of their rows, one octave lower than it could previously be seen.
    //
    // **`Seashore` closed and its row is now nearly the default.** It carried `block[0x13] = 0x42`,
    // two tenths of key follow, and this engine was pivoting that follow on the partial's key centre
    // where the module pivots on middle C -- worth `(64 - 60) * 100 * (10 - 2)` = 3200
    // milli-semitones, three and a bit semitones, on every one of its five cases. With the pivot
    // right all five agree to **0.12 dB in every band**, 0.1 dB of level and 0.0014 of peak. This
    // was one of the four "patches with a noise component" the article listed as unexplained, and
    // the pseudo-random source it was blamed on was never the cause.
    //
    // `Synth Drum` moved with it -- five tenths of follow, so half the error -- but has not closed:
    // its worst above-floor band is 6.6 dB at 1 kHz on note 36 and 5.6 at 2 kHz on note 72. What is
    // left there is its own defect, no longer this one.
    {118, {1.0, 0.045, 7.0, 6.0, 110.0}, "Synth Drum; unpitched -- the pitch bound is not a check"},
    {122, {1.0, 0.005, 1.0, 6.0, 41.0}, "Seashore; unpitched -- the pitch bound is not a check"},
}};

/// The kit keys that do not yet match, kept apart from the melodic table rather than folded in.
///
/// A separate table because a drum program is a different thing that happens to be numbered the
/// same way: program 0 on channel 10 is the Standard kit, and it has nothing whatever to do with
/// the Piano row above it. One table keyed by program alone would silently lend one the other's
/// bound, and the mistake would look exactly like a passing test.
///
/// **Thirty-six of the fifty-four drum cases need no row**, and forty of them agree on level to
/// under half a decibel. What the eighteen that remain say is four things, not eighteen:
///
///  - **Crash cymbals are about 2.5 dB light**, six cases across six kits and three distinct tones.
///    They used to stop dead at 1.84 s as well, and that part is fixed: it was a ring timer this
///    port invented, not the decay -- `scdec tvatrace` reads the module's own segment targets and
///    durations out of the voice, and this engine builds them exactly. See
///    \ref the-drum-ring-was-invented. Roughly a third of the level that remains is a DC offset the
///    module's render carries and this one does not; see \ref the-module-has-dc.
///  - **Hi-hats, where the module has a low-frequency floor this engine does not.** The module's
///    `TR-808 CHH` reads -46 dB at 63 Hz against a -34.8 dB loudest band; ours reads -147, which is
///    silence. Below 4 kHz the hat carries nothing either way, but its whole spectrum is only 26 dB
///    wide, so \ref signal_band_range cannot tell that floor apart from the note. That is a real
///    limit of the relative-band rule for a broadband source, and the reason these bounds are the
///    ugliest in the file. `Close HiHat2` separately runs 2.3 dB light in every band at a peak that
///    matches to 0.3%, which is the decay-rate defect above.
///  - **The Orchestra kit's timpani, which is wrong in a way none of the others are.** Three keys,
///    all of them: the attack arrives 4.7 dB low and half the module's peak, and the tail then runs
///    *12 dB long*. Every other kit key in the sweep is either right or slightly light; this one is
///    quiet and then loud. It is also the sweep's clearest test of the kit coarse-pitch plane --
///    the Orchestra kit tunes the timpani per key, so `pitch` is 42, 45 and 46 against a neutral 60,
///    while every other case here is at or near neutral.
///  - **Two SFX kit entries.** `Pick Scrape` is 4 dB loud and 38 dB out in a band; `Gt.CutNoise` is
///    10 dB quiet with the spectrum tilted the wrong way. Both are single tones in one kit.
///
/// The remaining three rows are small and unattributed: a snare's peak, and two snares a hair over
/// the band default.
constexpr std::array<KnownDrumDeviation, 19> known_drum_deviations{{
    // Crash cymbals. What is left here is level, and only level: the envelope markers of 945 that
    // two of these carried are gone, because the cause was not the decay. See
    // \ref the-drum-ring-was-invented.
    {0, 49, {2.6}, "Crash Cym.1; 2.5 dB light, and see \\ref the-module-has-dc"},
    {8, 49, {2.5}, "Crash Cym.1, Room kit"},
    {16, 49, {2.5}, "Crash Cym.1, Power kit"},
    {24, 49, {1.35, 0.045, 3.0, 7.5}, "GS Crash"},
    {32, 49, {2.6}, "Crash Cym.1, Jazz kit"},
    {40, 49, {2.6, 0.02}, "Brush Crash"},

    // Closed hi-hats.
    {0, 42, {1.0, 0.01, 8.5}, "Close HiHat2; 2.3 dB light in every band at a matching peak"},
    {8, 42, {2.3, 0.01, 26.0}, "Room Chh; 5 dB light above 500 Hz, and no floor below it"},
    {16, 42, {1.0, 0.01, 11.9}, "Close HiHat, Power kit"},
    {25, 42, {1.0, 0.01, 101.5}, "TR-808 CHH; the module's floor against this engine's silence"},
    {40, 42, {1.0, 0.01, 4.0}, "Close HiHat, Brush kit"},

    // The Orchestra kit's timpani, on the three keys it is tuned across. The only three rows the
    // note-off fix moved the wrong way, and only in the spectrum -- their level improved. It rings
    // longer now, which is right, and what it rings is still wrong.
    {48, 42, {1.6, 0.06, 7.7, 9.2}, "Timpani at key 42; quiet attack, long tail"},
    {48, 45, {1.4, 0.16, 8.2, 11.6}, "Timpani at key 45"},
    {48, 46, {1.35, 0.23, 9.0, 13.2}, "Timpani at key 46"},

    // The SFX kit. `Pick Scrape` used to be here at 38 dB and is not any more: it is one of the 52
    // keys in that kit which *do* answer a note-off, and it was being held for the ring instead.
    {56, 49, {10.5, 0.08, 17.5, 11.0}, "Gt.CutNoise; 10 dB quiet, spectrum tilted the wrong way"},

    // Surfaced when the gate began rendering with the module's own timing -- 128 samples of event
    // staging and its output stage, which this engine otherwise does not carry. Aligning the two
    // renders is what made this visible; it was under the line before only because the hit sat 130
    // samples earlier in the window than the module's does. 3.2 dB loud at 125 Hz on a band 18.6 dB
    // below the kick's own loudest, and unattributed beyond that.
    {24, 36, {1.0, 0.01, 3.3}, "Elec. BD; 3.2 dB loud at 125 Hz, seen once the timing lined up"},

    // Small, and so far unattributed.
    {24, 38, {1.0, 0.015}, "Elec. Snare; peak alone"},
    {25, 38, {1.0, 0.01, 3.5, 11.0}, "808 Snare 1"},
    {32, 38, {1.0, 0.01, 3.5}, "Jazz Snare 1"},
}};

[[nodiscard]] Deviation deviation_for(const Case& which)
{
    if (std::getenv("TS_STRICT_NOTES") != nullptr) {
        return Deviation{};
    }
    if (which.drums()) {
        for (const KnownDrumDeviation& entry : known_drum_deviations) {
            if (entry.program == which.program && entry.note == which.note) {
                return entry.allowed;
            }
        }
        return Deviation{};
    }
    for (const KnownDeviation& entry : known_deviations) {
        if (entry.program == which.program) {
            return entry.allowed;
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

    // The module's timing, because the module is what this is being compared against and it has no
    // switch for any of it. It always stages a message through its rings before a part sees it, and
    // it always runs its output stage even at its own rate. Rendering without those and then
    // explaining the difference by hand is how two separate findings got misattributed; the point
    // of a gate against the oracle is that it does not need explaining.
    options.event_delay_blocks = 4;
    options.bypass_output_filter = false;

    // And the module's resampler. `extended_interpolation` defaults on because it corrects a defect
    // the module has against the hardware it models; leaving it on here would compare the module
    // against a deliberate departure from the module.
    options.extended_interpolation = std::getenv("TS_EXTENDED") != nullptr;
    ToneGenerator generator{notes, options};

    // The harness renders 8 x 512 frames after the reset and throws them away. Nothing sounds yet,
    // so this is not warm-up in the audible sense -- it is the effect state the DLL settles into
    // before the note arrives, and mirroring it costs nothing.
    std::vector<float> discard_left(512);
    std::vector<float> discard_right(512);
    for (int i = 0; i < 8; ++i) {
        generator.render(discard_left, discard_right);
    }

    // Channel 10 for a drum case, where the same program change means a kit. Nothing else about the
    // case changes: the part is already a rhythm part after a GS reset, so the only thing the
    // channel does here is route.
    const int channel = which.channel;
    generator.send_channel(0xB0 | channel, 0, 0);    // bank select MSB
    generator.send_channel(0xB0 | channel, 32, 0);   // bank select LSB
    generator.send_channel(0xB0 | channel, 7, 127);  // part volume
    generator.send_channel(0xB0 | channel, 10, 64);  // pan centre
    generator.send_channel(0xB0 | channel, 91, 0);   // reverb send off
    generator.send_channel(0xB0 | channel, 93, 0);   // chorus send off
    generator.send_channel(0xC0 | channel, which.program, 0);

    const auto total = static_cast<std::size_t>((hold + tail) * rate);
    const auto off_at = static_cast<std::size_t>(hold * rate);

    std::vector<float> left(total);
    std::vector<float> right(total);

    generator.send_channel(0x90 | channel, which.note, which.velocity);
    std::size_t position = 0;
    bool released = false;
    while (position < total) {
        if (!released && position >= off_at) {
            generator.send_channel(0x80 | channel, which.note, 0);
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

    // A drum key names a kit entry, not a transposition, so there is no frequency to look for and
    // `fundamental` is asked for none. The fixture omits `pitchHz` on those cases for the same
    // reason, and the two absences have to agree or the comparison below would be against nothing.
    const double target_hz =
        which.drums() ? 0.0 : 440.0 * std::pow(2.0, (which.note - 69) / 12.0);
    return measure(left, right, rate, target_hz);
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
    // sit there looking like a known defect forever. Checked per table, because a melodic row is
    // not answered by a drum case that shares its number.
    const auto covers = [&cases](int program, int note, bool drums) {
        return std::any_of(cases.begin(), cases.end(), [&](const nlohmann::json& entry) {
            return entry.at("program").get<int>() == program
                   && (note < 0 || entry.at("note").get<int>() == note)
                   && (entry.value("channel", 0) == drum_channel) == drums;
        });
    };
    for (const KnownDeviation& known : known_deviations) {
        INFO("deviation row for program " << known.program << " (" << known.cause << ")");
        CHECK(covers(known.program, /*note=*/-1, /*drums=*/false));
    }
    for (const KnownDrumDeviation& known : known_drum_deviations) {
        INFO("deviation row for drum kit " << known.program << " key " << known.note << " ("
                                           << known.cause << ")");
        CHECK(covers(known.program, known.note, /*drums=*/true));
    }

    std::size_t compared = 0;
    std::size_t sounding = 0;
    std::size_t pitched = 0;
    std::size_t drums = 0;
    std::size_t signal_bands = 0;
    std::size_t floor_bands = 0;

    for (const auto& entry : cases) {
        const Case which{entry.at("program").get<int>(),  entry.at("note").get<int>(),
                         entry.at("velocity").get<int>(), entry.at("map").get<int>(),
                         entry.value("channel", 0)};
        const double hold = entry.at("hold").get<double>();
        if (which.drums()) {
            ++drums;
        }

        INFO((which.drums() ? "drum kit " : "program ")
             << which.program << " note " << which.note << " velocity " << which.velocity << " map "
             << which.map);

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

        // A case the module does not sound is a different question, and asking the loud one of it
        // gives nonsense: dividing by a silent reference's level is an infinity, and comparing its
        // octave bands compares two noise floors. See \ref inaudible_peak -- the one case in the
        // sweep this covers is an undefined kit key, and what it should assert is silence.
        const bool audible = expected_peak >= inaudible_peak;
        if (!audible) {
            INFO("the module does not sound here: peak " << expected_peak << " vs ours "
                                                         << ours.peak);
            CHECK(ours.peak < inaudible_peak);
        } else {
            INFO("peak " << ours.peak << " vs " << expected_peak << " ("
                         << (ours.peak - expected_peak) << ")");
            CHECK_THAT(ours.peak, WithinAbs(expected_peak, allowed.peak));

            const double expected_rms = entry.at("rms").get<double>();
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

        for (std::size_t band = 0; band < ours.bands.size() && audible; ++band) {
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
        //
        // Drum cases are skipped by the fixture rather than here -- it records no `pitchHz` for
        // them at all, because a drum key selects a kit entry rather than a transposition and there
        // is no frequency the note is supposed to come out at.
        // The lock test applies to *both* estimates, not only the module's. A tone with no real
        // fundamental hands each estimator a different peak out of a dense cluster, and the gap
        // between those two peaks is not a pitch error: `Synth Drum` at note 48 carries four peaks
        // within a decibel of each other between 113 and 125 Hz, the module's estimator settles 44
        // cents from nominal, and ours picks a different one entirely. Rendering that note either
        // side of a four-cent change moves its RMS by 0.001 dB and no spectral peak by more than
        // 0.1 dB, so there is nothing there for a pitch check to find.
        //
        // This cannot hide a real pitch error. Moving a note by anything like the amount that
        // trips this guard relocates its whole harmonic series, which the per-octave band
        // comparison below sees; that check runs on every case regardless.
        const double expected_pitch = entry.value("pitchHz", 0.0);
        const double nominal = 440.0 * std::pow(2.0, (which.note - 69) / 12.0);
        constexpr double lock_cents = 50.0;
        if (expected_pitch > 0.0 && ours.pitch_hz > 0.0
            && std::abs(1200.0 * std::log2(expected_pitch / nominal)) < lock_cents
            && std::abs(1200.0 * std::log2(ours.pitch_hz / nominal)) < lock_cents) {
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
        const double loudest_window =
            *std::max_element(expected_envelope.begin(), expected_envelope.end());
        double worst = 0.0;
        std::size_t worst_window = 0;
        for (std::size_t window = 0; window < ours.envelope.size() && audible; ++window) {
            // Absolute silence, and the module's own resting floor -- see \ref envelope_floor_range.
            if (expected_envelope[window] < -60.0
                || loudest_window - expected_envelope[window] > envelope_floor_range) {
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

        // The ruler. The sweep is too long to read through failures alone -- what a bound should be
        // is a question about the whole distribution, not about the cases that happen to miss.
        //
        // What it prints is this engine's own measurements, undifferenced. The fixture beside it
        // holds the module's, so anything worth asking -- a delta, a distribution, the shape of one
        // note's envelope against the module's -- can be asked afterwards without running again.
        if (std::getenv("TS_NOTE_REPORT") != nullptr && which.drums()) {
            // A drum case resolves through none of the machinery below it -- a different pair of
            // lookups, into the kit records, with the key's own coarse pitch standing in for the
            // transposition -- so what is worth printing is different too. Running the melodic
            // resolution on a kit program would print a tone that never sounds.
            const auto row = DrumKitTable::row_for_map(static_cast<ToneMap>(which.map));
            const auto kit = row ? notes.drums().kit_for_program(which.program, *row) : std::nullopt;
            const DrumKey key = notes.drums().key(which.note, kit.value_or(0));
            const auto tone = notes.directory().tone(key.tone);

            // kit / tone# / kit level / coarse pitch / mute group / pan.
            std::cout << "drum " << which.program << ' ' << which.note << ' ' << which.velocity
                      << ' ' << which.map << " kit " << kit.value_or(-1) << '/' << key.tone << '/'
                      << key.level << '/' << key.pitch << '/' << key.group << '/' << key.pan;

            // The amplitude envelope this engine builds, in the module's own terms: four targets on
            // a 0..0xffff scale and four segment durations in milliseconds. `scdec tvatrace` prints
            // the same eight numbers read straight out of the module's voice, so the two are
            // directly comparable -- which is the only way to tell a decay that is *rendered* wrong
            // from one that was *computed* wrong.
            if (tone) {
                // A drum sounds its tone at key 60; the kit's plane, not the note, gives the pitch.
                for (const ResolvedPartial& voice :
                     notes.directory().resolve(key.tone, /*note=*/60, which.velocity).partials) {
                    const PartialParameters& partial =
                        tone->partials()[static_cast<std::size_t>(voice.partial_index)];
                    const SegmentEnvelope built = notes.tva().create_envelope(
                        partial,
                        which.velocity,
                        /*key=*/60,
                        notes.directory().zone_level(partial.multisample(), 60,
                                                     partial.key_center()),
                        notes.directory().tone_level(key.tone),
                        rate,
                        0.0,
                        NoteRenderer::envelope_rate_key(key, 0));

                    // Divided back out by `TvaChain::amp_scale`, so these are the module's own
                    // 0..0xffff gain words rather than this engine's doubled ones.
                    std::cout << " env";
                    for (const double target : built.targets()) {
                        std::cout << ' '
                                  << static_cast<int>(
                                         std::lround(target / TvaChain::amp_scale * 65535.0));
                    }
                    std::int64_t previous = 0;
                    for (const std::int64_t end : built.segment_ends()) {
                        std::cout << '/' << ((end - previous) * 1000 / rate);
                        previous = end;
                    }
                    std::cout << '/' << (built.release_samples() * 1000 / rate);
                }
            }

            std::cout << " partials " << (tone ? tone->partials().size() : 0) << " rms " << ours.rms
                      << " peak " << ours.peak << " bands";
            for (const double band : ours.bands) {
                std::cout << ' ' << band;
            }
            std::cout << " env";
            for (const double window : ours.envelope) {
                std::cout << ' ' << window;
            }
            std::cout << " name " << (tone ? tone->name() : std::string{"?"}) << '\n';
        } else if (std::getenv("TS_NOTE_REPORT") != nullptr) {
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
                // LFO pitch depth / loop length / root key / fine tune / wave number.
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
                          << voice.descriptor.root_key << '/' << voice.descriptor.fine_tune << '/'
                          << voice.wave << std::setprecision(6);
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
    // Guard against passing vacuously: a sweep of silent notes would agree perfectly. One case is
    // legitimately silent on both sides -- see \ref inaudible_peak -- and is held to that instead.
    CHECK(sounding == compared - 1);

    // And guard `signal_band_range` against quietly swallowing the comparison. If a change ever
    // pushes most bands below the line, this gate would go on passing while checking almost
    // nothing. Measured at 1229 against the module and 660 held to the floor.
    INFO(signal_bands << " bands compared against the module, " << floor_bands
                      << " held to the noise floor");
    CHECK(signal_bands > floor_bands);
    CHECK(signal_bands > 1100);

    // And that the pitch check is not quietly skipping the melodic half. Measured at 182 of the
    // 185 melodic cases; the 54 drum cases have no pitch to compare and are not counted against it.
    INFO(pitched << " melodic cases had a fundamental to compare");
    CHECK(pitched > compared - drums - 10);

    // And that the drum half is actually there. It is the reason the sweep was widened, and a
    // fixture regenerated by an older copy of the generator would silently drop it.
    INFO(drums << " drum cases");
    CHECK(drums >= 50);
}
