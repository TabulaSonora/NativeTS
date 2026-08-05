#include "tabulasonora/control_matrix.hpp"
#include "tabulasonora/lfo_engine.hpp"
#include "tabulasonora/part.hpp"
#include "tabulasonora/patch_directory.hpp"
#include "tabulasonora/pitch_chain.hpp"
#include "tabulasonora/pitch_ramp.hpp"
#include "tabulasonora/tva_chain.hpp"
#include "tabulasonora/tvf_chain.hpp"

#include "dsp/fixed.hpp"
#include "test_data.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <set>

using namespace ts;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
namespace fs = std::filesystem;

namespace {

/// The control-rate objects, built once per test case.
///
/// Declaration order is load-bearing: the chains hold references to the tables and the machine.
class Fixture {
public:
    static Fixture make()
    {
        return Fixture{TableSet::from_cache_directory(testdata::require_tables())};
    }

    [[nodiscard]] const TableSet& tables() const noexcept { return tables_; }

    [[nodiscard]] const EnvelopeMachine& machine() const noexcept { return machine_; }

    [[nodiscard]] const TvaChain& tva() const noexcept { return tva_; }

    [[nodiscard]] const TvfChain& tvf() const noexcept { return tvf_; }

    [[nodiscard]] const PitchChain& pitch() const noexcept { return pitch_; }

    [[nodiscard]] const LfoEngine& lfo() const noexcept { return lfo_; }

    [[nodiscard]] const PatchDirectory& directory() const noexcept { return directory_; }

private:
    explicit Fixture(TableSet&& tables)
        : tables_(std::move(tables)),
          machine_(tables_),
          tva_(tables_, machine_),
          tvf_(tables_, machine_),
          pitch_(tables_, machine_),
          lfo_(tables_),
          directory_(tables_)
    {
    }

    TableSet tables_;
    EnvelopeMachine machine_;
    TvaChain tva_;
    TvfChain tvf_;
    PitchChain pitch_;
    LfoEngine lfo_;
    PatchDirectory directory_;
};

} // namespace

TEST_CASE("the fixed-point control path matches an independent implementation",
          "[dsp][sccore][gate]")
{
    // The Phase 4 gate. Every sweep here lands on an expression that overflows 32 bits or truncates
    // to 16 on purpose -- the places a port goes wrong silently. The fixture comes from
    // tools/dump_modulation.py, which spells each wrap out with an explicit mask because Python's
    // integers do not wrap on their own, so it cannot accidentally agree with a C++ bug.
    const fs::path fixture_path = testdata::repository_root() / "fixtures" / "modulation.json";
    if (!fs::exists(fixture_path)) {
        SKIP("No modulation fixture. Generate it with:\n"
             "  python3 tools/dump_modulation.py <SCCore.dll> fixtures/modulation.json");
    }

    Fixture fixture = Fixture::make();

    std::ifstream stream{fixture_path};
    REQUIRE(stream);
    const nlohmann::json document = nlohmann::json::parse(stream);

    SECTION("rate_scale")
    {
        const auto& expected = document.at("rateScale");
        std::size_t at = 0;
        for (int base = 0; base < 256; base += 3) {
            for (int modifier = 0; modifier < 128; modifier += 5) {
                INFO("base " << base << " modifier " << modifier);
                REQUIRE(fixture.machine().rate_scale(base, modifier) == expected[at].get<int>());
                ++at;
            }
        }
        CHECK(at == expected.size());
        CHECK(at == 2236);
    }

    SECTION("level_scale")
    {
        const auto& expected = document.at("levelScale");
        std::size_t at = 0;
        for (int level = 0; level < 128; level += 3) {
            for (int modifier = 0; modifier < 128; modifier += 5) {
                INFO("level " << level << " modifier " << modifier);
                REQUIRE(fixture.machine().level_scale(level, modifier) == expected[at].get<int>());
                ++at;
            }
        }
        CHECK(at == expected.size());
    }

    SECTION("segment_milliseconds")
    {
        // Two 0xffff-scale products in a row. This is the expression most likely to be silently
        // wrong, because widening it to 64 bits "to be safe" produces plausible timings.
        const auto& expected = document.at("segmentMilliseconds");
        std::size_t at = 0;
        for (int rate = 0; rate < 256; rate += 7) {
            for (int multiplier : {0x100, 0x1000, 0x8000, 0xFFFF}) {
                for (int velocity : {0x100, 0x4000, 0xFFFF}) {
                    INFO("rate " << rate << " multiplier " << multiplier << " velocity "
                                 << velocity);
                    REQUIRE(fixture.machine().segment_milliseconds(rate, multiplier, velocity)
                            == expected[at].get<double>());
                    ++at;
                }
            }
        }
        CHECK(at == expected.size());
    }

    SECTION("shift8")
    {
        const auto& expected = document.at("shift8");
        std::size_t at = 0;
        for (int v = -70000; v < 70000; v += 137) {
            INFO("value " << v);
            REQUIRE(fx::shift8(v) == expected[at].get<int>());
            ++at;
        }
        CHECK(at == expected.size());
    }

    SECTION("lfo waveforms")
    {
        // Quadrants 1 and 3 of waveforms 6 and 7 depend on the 16-bit truncation of p*2.
        const auto& expected = document.at("lfoWaveform");
        std::size_t at = 0;
        for (int wave : {0, 4, 5, 6, 7, 8, 12, 20, 31}) {
            for (int phase = 0; phase < 0x10000; phase += 313) {
                INFO("wave " << wave << " phase " << phase);
                REQUIRE(fixture.lfo().waveform(phase, wave) == expected[at].get<int>());
                ++at;
            }
        }
        CHECK(at == expected.size());
    }

    SECTION("pitch envelopes across the whole tone table")
    {
        // Every present partial at three velocities: start, four targets, release, four segment
        // times and the release time. The depth scale that feeds all of them reaches about 2.7e11
        // and the engine keeps only the low word, so a 64-bit "fix" shows up here immediately.
        const auto& expected = document.at("pitchEnvelopes");
        REQUIRE(expected.size() > 10000);

        std::size_t checked = 0;
        std::size_t with_envelope = 0;

        for (const auto& entry : expected) {
            const int tone_number = entry.at("tone").get<int>();
            const int slot = entry.at("slot").get<int>();
            const int velocity = entry.at("velocity").get<int>();

            const PartialParameters partial =
                fixture.directory().partial_by_slot(tone_number, slot);
            REQUIRE(partial.is_present());

            const std::optional<PitchEnvelope> envelope =
                fixture.pitch().envelope_offsets(partial, 60, velocity);

            INFO("tone " << tone_number << " slot " << slot << " velocity " << velocity);

            if (entry.at("envelope").is_null()) {
                REQUIRE_FALSE(envelope.has_value());
                ++checked;
                continue;
            }

            REQUIRE(envelope.has_value());
            const auto& reference = entry.at("envelope");

            REQUIRE(envelope->start == reference.at("start").get<int>());
            REQUIRE(envelope->release == reference.at("release").get<int>());
            for (std::size_t i = 0; i < envelope->targets.size(); ++i) {
                REQUIRE(envelope->targets[i] == reference.at("targets")[i].get<int>());
            }
            for (std::size_t i = 0; i < envelope->times.size(); ++i) {
                REQUIRE(envelope->times[i] == reference.at("times")[i].get<double>());
            }
            REQUIRE(envelope->release_ms == reference.at("releaseMs").get<double>());

            ++with_envelope;
            ++checked;
        }

        CHECK(checked == expected.size());
        // Guard against passing vacuously: most partials must actually have an envelope.
        CHECK(with_envelope > 1000);
    }
}

TEST_CASE("the state-variable filter is stable and orders its three lines", "[dsp]")
{
    // Needs no DLL. The order of the three lines and the grouping inside the middle one are both
    // load-bearing; a reassociated version stays close for a few samples and then diverges.
    StateVariableFilter filter;

    // A lowpass at a gentle cutoff must settle on a DC input rather than ring or blow up.
    double last = 0.0;
    for (int i = 0; i < 4000; ++i) {
        last = filter.process(1.0, 0.1, 1.0, FilterTap::low_pass);
        REQUIRE(std::isfinite(last));
    }
    CHECK_THAT(last, WithinAbs(1.0, 0.02));

    filter.reset();
    CHECK(filter.low() == 0.0);
    CHECK(filter.band() == 0.0);

    // Bypass returns the input untouched and does not consult the integrators.
    CHECK(filter.process(0.375, 0.5, 1.0, FilterTap::bypass) == 0.375);
}

TEST_CASE("a segment envelope is a pure function of position", "[dsp][sccore]")
{
    // This is what lets the offline renderer and the block loop share one envelope: evaluating it
    // at a sample must not depend on having evaluated any earlier sample.
    Fixture fixture = Fixture::make();

    const std::array<double, 4> targets{1.0, 0.5, 0.25, 0.125};
    const std::array<double, 4> spans{1000.0, 1000.0, 1000.0, 1000.0};
    const std::array<bool, 4> linear{true, true, true, true};

    SegmentEnvelope forward{fixture.machine(), targets, spans, linear, 0.0, 500.0, true, 0.0, 320};
    SegmentEnvelope backward{fixture.machine(), targets, spans, linear, 0.0, 500.0, true, 0.0, 320};

    // Walk one forwards and the other backwards; they must agree at every point.
    for (std::int64_t n = 0; n < 4000; ++n) {
        REQUIRE(forward.value_at(n) == backward.value_at(3999 - n + n - (3999 - n)));
    }
    for (std::int64_t n = 3999; n >= 0; --n) {
        REQUIRE(forward.value_at(n) == backward.value_at(n));
    }
}

TEST_CASE("note-off is deferred to the following control tick", "[dsp]")
{
    // Measured on the DLL: a note-off at 1010 ms, exactly a tick, released at 1020 ms, while one at
    // 1008 ms released at 1010 ms. The deferral spans one full tick and is never zero.
    constexpr std::int64_t tick = 320;

    CHECK(SegmentEnvelope::defer_to_control_tick(0, tick) == 320);
    CHECK(SegmentEnvelope::defer_to_control_tick(1, tick) == 320);
    CHECK(SegmentEnvelope::defer_to_control_tick(319, tick) == 320);

    // Landing exactly on a boundary still waits a full tick: that tick's update has already run.
    CHECK(SegmentEnvelope::defer_to_control_tick(320, tick) == 640);
    CHECK(SegmentEnvelope::defer_to_control_tick(321, tick) == 640);

    CHECK(SegmentEnvelope::defer_to_control_tick(-5, tick) == 320);
}

TEST_CASE("the part volume scale is one at the reference", "[dsp]")
{
    // Needs no DLL. The intermediate exceeds 32 bits and is widened; the result is squared.
    CHECK_THAT(TvaChain::part_volume_scale(127, 127, 127), WithinAbs(1.0, 1e-12));

    // Volume and expression enter symmetrically, so swapping them cannot change the result.
    CHECK_THAT(TvaChain::part_volume_scale(100, 60, 127),
               WithinAbs(TvaChain::part_volume_scale(60, 100, 127), 1e-12));

    // Silence at either input, and monotone in between.
    CHECK(TvaChain::part_volume_scale(0, 127, 127) == 0.0);
    CHECK(TvaChain::part_volume_scale(127, 0, 127) == 0.0);

    double previous = -1.0;
    for (int v = 0; v <= 127; ++v) {
        const double value = TvaChain::part_volume_scale(v, 127, 127);
        INFO("volume " << v << " -> " << value);
        REQUIRE(value >= previous);
        previous = value;
    }
}

TEST_CASE("the amplitude curve floors at the table, not at zero", "[dsp][sccore]")
{
    // g_amp_curve_hi[0] is 4 rather than 0, so level zero is 4.6e-05 and not silence. A level that
    // has decayed past the floor must therefore be forced to true silence by the caller rather than
    // clamped into the table.
    Fixture fixture = Fixture::make();

    CHECK_THAT(fixture.tva().amp_of(0), WithinAbs(4.6e-05, 1e-6));
    CHECK(fixture.tva().amp_of(0) > 0.0);

    // Monotone, and the top of the range reaches unity.
    double previous = -1.0;
    for (int level = 0; level <= 0xFFFF; level += 97) {
        const double value = fixture.tva().amp_of(level);
        REQUIRE(value >= previous);
        previous = value;
    }
    CHECK_THAT(fixture.tva().amp_of(0xFFFF), WithinAbs(1.0, 0.001));

    // Out-of-range clamps rather than reading past the table.
    CHECK(fixture.tva().amp_of(-1) == fixture.tva().amp_of(0));
    CHECK(fixture.tva().amp_of(0x10000) == fixture.tva().amp_of(0xFFFF));
}

TEST_CASE("the neutral resonance byte gives exactly unity damping", "[dsp][sccore]")
{
    // Reciprocal-Q: smaller is more resonant, and 0x40 is exactly 1.0.
    Fixture fixture = Fixture::make();

    CHECK_THAT(fixture.tvf().damping_coefficient(0, 0x40, 1), WithinAbs(1.0, 1e-12));
    CHECK(fixture.tvf().damping_coefficient(0, 0x20, 1) < 1.0);
    CHECK(fixture.tvf().damping_coefficient(0, 0x60, 1) > 1.0);
}

TEST_CASE("the f-coefficient ceiling is the filter's stability bound", "[dsp][sccore]")
{
    // g_svf_f_ceil is indexed by the raw damping value's top bits, and what it holds is
    // Chamberlin's own bound, sqrt(q^2 + 4) − q: the largest f the loop stays stable at for that q.
    // The engine clamps f to it in voice_ctrl_ramp_d, which is why the table is loaded at all.
    Fixture fixture = Fixture::make();
    const auto ceiling = fixture.tables().svf_f_ceil();

    REQUIRE(ceiling.size() == 1024);
    for (std::size_t index = 32; index <= 1016; ++index) {
        const double q = static_cast<double>(index) * 256.0 / 131072.0;
        INFO("index " << index << " q " << q);
        REQUIRE_THAT(static_cast<double>(ceiling[index]),
                     WithinAbs(std::sqrt((q * q) + 4.0) - q, 0.002));
    }

    // Monotone decreasing: more resonance, less headroom.
    for (std::size_t index = 1; index < ceiling.size(); ++index) {
        REQUIRE(ceiling[index] <= ceiling[index - 1]);
    }

    // The clamp itself, driven past the point the cutoff units can actually reach: f saturates at
    // 2.0 and the most resonant byte holds it to 0.832.
    const TvfChain& tvf = fixture.tvf();
    CHECK(tvf.frequency_coefficient(0x3FFFF) > 1.9);
    CHECK_THAT(tvf.coefficients(0x3FFFF, 0x7F, 1).frequency,
               WithinAbs(static_cast<double>(ceiling[0x7F * 8]), 1e-12));
    CHECK_THAT(tvf.coefficients(0x3FFFF, 0x7F, 1).damping,
               WithinAbs(tvf.damping_coefficient(0x3FFFF, 0x7F, 1), 1e-12));
}

TEST_CASE("the f-coefficient ceiling never engages in the reachable range", "[dsp][sccore]")
{
    // The cutoff ceiling already holds f below the stability bound for every resonance byte, filter
    // type and cutoff the engine can produce, so this stage is inert -- it is implemented because
    // it is a real stage, not because it changes a render. A failure here means something upstream
    // now drives the filter past the point the engine considers safe, which is worth knowing about.
    Fixture fixture = Fixture::make();
    const TvfChain& tvf = fixture.tvf();

    for (int resonance = 4; resonance <= 0x7F; ++resonance) {
        for (int type : {0, 1, 2, 4, 5, 6}) {
            for (int cutoff = 0; cutoff <= 0x7FFF; cutoff += 16) {
                const int units = tvf.cutoff_units(cutoff, resonance);
                INFO("resonance " << resonance << " type " << type << " cutoff " << cutoff);
                REQUIRE(tvf.coefficients(units, resonance, type).frequency
                        == tvf.frequency_coefficient(units));
            }
        }
    }
}

TEST_CASE("the noise generator reproduces the engine's reset sequence", "[dsp]")
{
    // Needs no DLL. The sequence is deterministic from engine reset; what polyphony changes is only
    // the order voices consume draws. A generator that drifted here would move every random pan and
    // every pitch jitter in the song.
    EngineNoise noise;

    const std::uint16_t first = noise.next();
    const std::uint16_t second = noise.next();
    const std::uint16_t third = noise.next();

    noise.reset();
    CHECK(noise.next() == first);
    CHECK(noise.next() == second);
    CHECK(noise.next() == third);

    // The draws must not collapse to a constant or a short cycle.
    noise.reset();
    std::set<std::uint16_t> seen;
    for (int i = 0; i < 1000; ++i) {
        seen.insert(noise.next());
    }
    CHECK(seen.size() > 900);

    // next_pan takes the top seven bits, so it stays on CC#10's scale.
    noise.reset();
    for (int i = 0; i < 500; ++i) {
        const int pan = noise.next_pan();
        REQUIRE(pan >= 0);
        REQUIRE(pan <= 127);
    }
}

TEST_CASE("the pitch envelope is stepped, not evaluated", "[dsp][sccore]")
{
    // A segment completes when the 16-bit phase reaches 0xffff -- not exceeds it -- and the next
    // starts with a fresh phase rather than carrying the remainder. The trajectory therefore
    // depends on the accumulator's history, which is why it cannot be a pure function of time.
    PitchEnvelope envelope;
    envelope.start = -1000;
    envelope.targets = {500, 0, 0, 0};
    envelope.release = 0;
    envelope.times = {100.0, 100.0, 0.0, 0.0};
    envelope.release_ms = 50.0;

    PitchEnvelopeRunner runner{envelope};
    CHECK_THAT(runner.level(), WithinAbs(-1000.0, 1e-12));

    double previous = runner.level();
    for (int i = 0; i < 10; ++i) {
        const double now = runner.tick(false);
        REQUIRE(std::isfinite(now));
        previous = now;
    }
    CHECK(previous > -1000.0);

    // A constant runner holds its level regardless of note-off: that is the jitter-only case.
    PitchEnvelopeRunner held = PitchEnvelopeRunner::constant(42.0);
    CHECK_THAT(held.tick(false), WithinAbs(42.0, 1e-12));
    CHECK_THAT(held.tick(true), WithinAbs(42.0, 1e-12));
}

TEST_CASE("start jitter's reachable range is narrower than documented", "[dsp]")
{
    // The upstream remark says the magnitude slice is 7 bits positive against 8 negative, "so the
    // range is about [-10*d, +5*d]" -- for depth 10, [-100, +50].
    //
    // Swept over all 65536 draws it is not. The same bit that picks the sign, bit 14, also
    // constrains which draws can reach the negative branch, so the wider negative slice is never
    // fully used. Measured: depth 10 gives [-50, +50] and depth 5 gives [-30, +20].
    //
    // The asymmetry is real -- depth 5 is visibly lopsided -- it is just half the size the comment
    // claims. Pinned here rather than restated, because the range is what a listener would notice
    // if it ever moved.
    const auto sweep = [](int depth) {
        int lowest = 0;
        int highest = 0;
        for (std::uint32_t draw = 0; draw <= 0xFFFF; ++draw) {
            const int jitter =
                PitchChain::start_jitter_milli_semitones(depth, static_cast<std::uint16_t>(draw));
            highest = std::max(highest, jitter);
            lowest = std::min(lowest, jitter);
        }
        return std::pair{lowest, highest};
    };

    const auto [low10, high10] = sweep(10);
    CHECK(low10 == -50);
    CHECK(high10 == 50);

    const auto [low5, high5] = sweep(5);
    CHECK(low5 == -30);
    CHECK(high5 == 20);

    // A zero depth draws nothing and contributes nothing.
    CHECK(PitchChain::start_jitter_milli_semitones(0, 0x1234) == 0);

    // Which bit picks the sign, said directly. The sweep above is what caught MSVC compiling the
    // sign test as bit 15 under optimisation, but it could only report that the range had doubled;
    // these four draws name the predicate, so the next time it moves the failure says which bit.
    // Each pair differs in bit 14 alone, and 0x4000 and 0x7f80 are the two the miscompile sent
    // down the positive branch.
    CHECK(PitchChain::start_jitter_milli_semitones(10, 0x3FFF) == 50);  // bit 14 clear: positive
    CHECK(PitchChain::start_jitter_milli_semitones(10, 0x4000) == -50); // bit 14 set: negative
    CHECK(PitchChain::start_jitter_milli_semitones(10, 0x7F80) == 0);
    CHECK(PitchChain::start_jitter_milli_semitones(10, 0x0080) == 0);

    // Bit 15 does not pick it, which is the thing that was silently untrue on one toolchain: these
    // two differ in bit 15 alone and land on the same branch, so they agree.
    CHECK(PitchChain::start_jitter_milli_semitones(10, 0x4000)
          == PitchChain::start_jitter_milli_semitones(10, 0xC000));
}

TEST_CASE("the pitch accumulator clamps at 127 semitones", "[dsp]")
{
    // 127 * 1000 milli-semitones, which is what fixes the unit. Jetplane's first partial has a base
    // of 24000 with an envelope starting at -24000, landing exactly on the floor.
    CHECK(PitchChain::clamp(-1.0) == 0.0);
    CHECK(PitchChain::clamp(0.0) == 0.0);
    CHECK(PitchChain::clamp(1e9) == PitchChain::max_pitch_milli_semitones);
    // 0x1f018 is exactly 127000; the hex spelling is what the engine uses.
    CHECK(PitchChain::max_pitch_milli_semitones == 127 * 1000);
}

TEST_CASE("the part modify offsets are neutral at 0x40", "[dsp]")
{
    // Every one of these is a controller centred on 0x40, and a default-constructed set has to be
    // indistinguishable from having no part at all -- that property is what lets the chains take a
    // modifier argument everywhere without the offline renderer having to supply one.
    const PartModifiers neutral;
    CHECK(neutral.is_neutral());
    CHECK(neutral.attack_bias() == 0);
    CHECK(neutral.decay_bias() == 0);
    CHECK(neutral.release_bias() == 0);
    CHECK(neutral.cutoff_offset() == 0);

    // The envelope offsets move the rate-curve index two entries per controller step, and the
    // cutoff offset moves the 15-bit cutoff sum a whole 0x100.
    PartModifiers moved;
    moved.env_attack = 0x50;
    moved.env_decay = 0x30;
    moved.env_release = 0x7F;
    moved.tvf_cutoff = 0x30;
    CHECK_FALSE(moved.is_neutral());
    CHECK(moved.attack_bias() == 32);
    CHECK(moved.decay_bias() == -32);
    CHECK(moved.release_bias() == 126);
    CHECK(moved.cutoff_offset() == -0x1000);
}

TEST_CASE("velocity sense has three edges that are easy to get wrong", "[dsp][sccore]")
{
    Part part;

    // Neutral is the identity, and specifically not a multiply by 0x40 followed by a shift: that
    // would be the identity too, right up until truncation ate a count.
    for (int velocity : {1, 27, 64, 100, 127}) {
        INFO("velocity " << velocity);
        CHECK(part.effective_velocity(velocity) == velocity);
    }

    // A depth of zero collapses everything to 1, not to silence. A part set this way still sounds.
    part.velocity_depth = 0;
    CHECK(part.effective_velocity(127) == 1);
    CHECK(part.effective_velocity(1) == 1);

    // Depth scales by depth/0x40 with a shift, so half depth is half velocity.
    part.velocity_depth = 0x20;
    CHECK(part.effective_velocity(100) == 50);
    CHECK(part.effective_velocity(127) == 63);

    // Offset is two counts per step, and it is added after the depth scaling rather than before.
    part.velocity_depth = 0x40;
    part.velocity_offset = 0x50;
    CHECK(part.effective_velocity(64) == 64 + 32);
    part.velocity_offset = 0x30;
    CHECK(part.effective_velocity(64) == 64 - 32);

    // Both rails. The low one is 1 and not 0 -- an offset that undershoots still sounds, at the
    // floor -- and the high one is 0x7f.
    part.velocity_offset = 0x00;
    CHECK(part.effective_velocity(1) == 1);
    part.velocity_offset = 0x7F;
    CHECK(part.effective_velocity(127) == 0x7F);
}

TEST_CASE("the part modify offsets reach the chains that consume them", "[dsp][sccore]")
{
    const Fixture fixture = Fixture::make();

    // A real partial, copied so the opt-in bit can be flipped. Building one from zeroes does not
    // work and should not: several of the block's bytes are table indices whose zero is out of
    // range, because a stored zero never occurs there. Starting from data the engine ships keeps
    // the laws under test honest.
    const PartialParameters source = fixture.directory().partial_by_slot(0, 0);
    REQUIRE(source.is_present());

    std::array<std::uint8_t, PartialParameters::stride> block{};
    std::copy(source.raw().begin(), source.raw().end(), block.begin());
    const PartialParameters partial{block.data()};

    SECTION("the resonance byte is floored at 4 and capped at 0x7f")
    {
        // `tvf_cutoff_compute` @`180083f00` clamps the resonance byte at voice+0xee up to 4 before
        // the filter runs -- `if (*pbVar1 < 4) *pbVar1 = 4` -- and stage A caps it at 0x7f.
        //
        // Both rails are reachable and the engine sits on them. Reading its own byte through
        // `scdec svfmel 38 0 48 127 64 <cc71>`, CC#71 walks it 127 -> 112 -> 48 -> 4 across
        // 0/32/64/96, and 96, 120 and 127 all read exactly 4 with the filter's q pinned at
        // 0.0625. So this is not a defensive clamp on data that never occurs; it is where the
        // module parks whenever the resonance control is driven far enough.
        int lowest = 0x7F;
        int highest = 0;
        int previous = 0x7F + 1;

        for (int part_resonance = 0; part_resonance <= 0x7F; ++part_resonance) {
            const int byte = TvfChain::resonance_byte(partial, part_resonance);

            INFO("part resonance " << part_resonance);
            CHECK(byte >= 4);
            CHECK(byte <= 0x7F);

            // Monotone: more of the control never means more of the byte.
            CHECK(byte <= previous);
            previous = byte;

            lowest = std::min(lowest, byte);
            highest = std::max(highest, byte);
        }

        // The clamps are live rather than vacuous -- the sweep reaches both.
        CHECK(lowest == 4);
        CHECK(highest == 0x7F);
    }

    SECTION("CC#71 walks the resonance byte the way the engine's own does")
    {
        // Read straight off the DLL with `scdec svfmel 38 0 48 127 64 <cc71> gs`, which prints
        // voice+0xee and the filter's q for every sounding voice. Program 38 sounds two partials
        // whose own resonance bytes are 48 and 18, so one table pins the law at two different
        // starting points -- including where each one hits the rails, which is at a different
        // controller value for each.
        //
        // The law is `(0x80 - sibling - cc71) * 2 + partial`, with the user-tone sibling neutral at
        // 0x40 on any standard file. Both terms subtract, which is why the walk descends.
        struct Point {
            int cc71;
            int from48;
            int from18;
        };
        constexpr std::array<Point, 9> measured{{
            {0, 127, 127},
            {16, 127, 114},
            {32, 112, 82},
            {48, 80, 50},
            {64, 48, 18},
            {80, 16, 4},
            {96, 4, 4},
            {112, 4, 4},
            {127, 4, 4},
        }};

        for (const Point& point : measured) {
            INFO("CC#71 " << point.cc71);
            CHECK(TvfChain::resonance_byte_of(48, point.cc71) == point.from48);
            CHECK(TvfChain::resonance_byte_of(18, point.cc71) == point.from18);
        }

        // Neutral is neutral: the whole point of the default is that a file which never sends the
        // controller renders exactly as it did before this control existed.
        CHECK(TvfChain::resonance_byte_of(partial.resonance(), PartModifiers::neutral)
              == TvfChain::resonance_byte(partial));

        // And q is the byte over 64 -- 0.750000 at 48, 1.984375 at 127, 0.062500 at the floor, all
        // three read off the DLL's own coefficient slot in the same sweep.
        const TvfChain& tvf = fixture.tvf();
        const int units = tvf.cutoff_units(0x4000, 0x40);
        CHECK_THAT(tvf.damping_coefficient(units, 48, 0), WithinAbs(0.750000, 1e-09));
        CHECK_THAT(tvf.damping_coefficient(units, 127, 0), WithinAbs(1.984375, 1e-09));
        CHECK_THAT(tvf.damping_coefficient(units, 4, 0), WithinAbs(0.062500, 1e-09));
    }

    SECTION("the cutoff offset is not in the base, because the live path adds it")
    {
        // `base_cutoff` is a property of the tone. The part's cutoff offset reaches the filter from
        // `PartialVoice::control`, which adds it to the per-block sum, and it must not also be
        // latched here -- doing both applied CC#74 twice and moved the cutoff 0x200 a controller
        // step where the module moves 0x100.
        const int neutral = fixture.tvf().create_envelope(partial, 100, 60).base_cutoff;

        PartModifiers up;
        up.tvf_cutoff = 0x44;
        CHECK(fixture.tvf().create_envelope(partial, 100, 60, 32000, up).base_cutoff == neutral);

        PartModifiers down;
        down.tvf_cutoff = 0x3C;
        CHECK(fixture.tvf().create_envelope(partial, 100, 60, 32000, down).base_cutoff == neutral);

        // And the offset itself is still a whole 0x100 a step, which is what the live path adds.
        CHECK(up.cutoff_offset() == 4 * 0x100);
        CHECK(down.cutoff_offset() == -4 * 0x100);
    }

    SECTION("the filter envelope opts in to the envelope offsets and the amplitude one does not")
    {
        PartModifiers slow;
        slow.env_attack = 0x70;
        slow.env_decay = 0x70;
        slow.env_release = 0x70;

        // Bit 4 of block byte 0x0e clear: the filter envelope ignores them outright. Only that
        // bit is touched -- the rest of the byte is the partial's own and is not this test's to
        // invent.
        block[0x0E] = static_cast<std::uint8_t>(block[0x0E] & ~0x10);
        CHECK_FALSE(TvfChain::responds_to_env_modifiers(partial));
        const auto opted_out = fixture.tvf().create_envelope(partial, 100, 60, 32000, slow);
        const auto none = fixture.tvf().create_envelope(partial, 100, 60);
        CHECK(opted_out.offsets.release_samples() == none.offsets.release_samples());

        // Bit 4 set: the same offsets now move it.
        block[0x0E] = static_cast<std::uint8_t>(block[0x0E] | 0x10);
        CHECK(TvfChain::responds_to_env_modifiers(partial));
        const auto opted_in = fixture.tvf().create_envelope(partial, 100, 60, 32000, slow);
        CHECK(opted_in.offsets.release_samples() != none.offsets.release_samples());
    }

    SECTION("the vibrato offsets reach LFO1 and leave LFO2 alone")
    {
        const auto [base1, base2] = fixture.lfo().configure(0, partial);

        // Depth biases the cents-table index two entries a step, which is the same thing as a
        // partial whose stored depth index is that much higher. Saying it that way tests the law
        // without restating the table.
        PartModifiers deeper;
        deeper.vibrato_depth = 0x44;
        const auto [wide, ignored_a] = fixture.lfo().configure(0, partial, deeper);

        std::array<std::uint8_t, PartialParameters::stride> shifted = block;
        shifted[0x15] = static_cast<std::uint8_t>(block[0x15] + (4 * 2));
        REQUIRE(fx::i8(shifted[0x15]) > 0);
        const auto [reference, ignored_b] =
            fixture.lfo().configure(0, PartialParameters{shifted.data()});
        CHECK(wide.pitch_depth == reference.pitch_depth);
        CHECK(wide.pitch_depth != base1.pitch_depth);

        // Rate and delay index their own tables. Driving each from one rail to the other has to
        // move them whatever the tone's stored index is, because the two ends of those tables
        // differ; that is enough to prove the wire is live without pinning a tone's contents.
        PartModifiers slowest;
        slowest.vibrato_rate = 0x00;
        slowest.vibrato_delay = 0x00;
        PartModifiers fastest;
        fastest.vibrato_rate = 0x7F;
        fastest.vibrato_delay = 0x7F;
        const auto [low, ignored_c] = fixture.lfo().configure(0, partial, slowest);
        const auto [high, ignored_d] = fixture.lfo().configure(0, partial, fastest);
        CHECK(low.increment != high.increment);
        CHECK(low.delay_rate != high.delay_rate);

        // LFO2's rate and delay are raw increments with no index to bias, so none of this touches
        // it.
        for (const PartModifiers& modifiers : {deeper, slowest, fastest}) {
            const auto [ignored_e, lfo2] = fixture.lfo().configure(0, partial, modifiers);
            CHECK(lfo2.increment == base2.increment);
            CHECK(lfo2.delay_rate == base2.delay_rate);
            CHECK(lfo2.pitch_depth == base2.pitch_depth);
        }
    }
}

TEST_CASE("the control matrix's linear apply routes each destination to itself", "[dsp][sccore]")
{
    using Source = ControlMatrix::Source;
    using Destination = ControlMatrix::Destination;

    // Reads one named output of the modulation, so a test can name a destination twice -- once as
    // the address it writes and once as the field it expects -- and fail if the two disagree.
    const auto field = [](const ControlMatrix::Modulation& m, Destination destination) {
        switch (destination) {
        case Destination::pitch:
            return m.pitch;
        case Destination::tvf_cutoff:
            return m.tvf_cutoff;
        case Destination::amplitude:
            return m.amplitude;
        case Destination::lfo1_rate:
            return m.lfo1_rate;
        case Destination::lfo1_pitch:
            return m.lfo1_pitch;
        case Destination::lfo1_tvf:
            return m.lfo1_tvf;
        case Destination::lfo1_tva:
            return m.lfo1_tva;
        case Destination::lfo2_rate:
            return m.lfo2_rate;
        case Destination::lfo2_pitch:
            return m.lfo2_pitch;
        case Destination::lfo2_tvf:
            return m.lfo2_tvf;
        case Destination::lfo2_tva:
            return m.lfo2_tva;
        }
        return 0;
    };

    constexpr std::array<Destination, ControlMatrix::destination_count> all{
        Destination::pitch,
        Destination::tvf_cutoff,
        Destination::amplitude,
        Destination::lfo1_rate,
        Destination::lfo1_pitch,
        Destination::lfo1_tvf,
        Destination::lfo1_tva,
        Destination::lfo2_rate,
        Destination::lfo2_pitch,
        Destination::lfo2_tvf,
        Destination::lfo2_tva,
    };

    SECTION("a source assigned nothing contributes nothing")
    {
        ControlMatrix matrix;
        // Power-on has one non-zero route, so clear it before claiming silence.
        matrix.at(Source::modulation, Destination::lfo1_pitch) = 0;
        const ControlMatrix::Modulation m = matrix.applied_linear(Source::modulation, 127);
        for (const Destination destination : all) {
            INFO("destination " << static_cast<int>(destination));
            CHECK(field(m, destination) == 0);
        }
    }

    SECTION("a controller at rest contributes nothing whatever is assigned")
    {
        ControlMatrix matrix;
        for (const Destination destination : all) {
            matrix.at(Source::cc1, destination) = 0x7F;
        }
        const ControlMatrix::Modulation m = matrix.applied_linear(Source::cc1, 0);
        for (const Destination destination : all) {
            CHECK(field(m, destination) == 0);
        }
    }

    SECTION("moving one destination moves exactly that one output")
    {
        // The engine permutes twice between the address and the output -- once writing the block
        // and once reading it -- and each LFO group comes back reversed. This is the test that a
        // mistake in either permutation cannot survive: assign one route, and the field that moves
        // has to be the field the address names.
        for (const Destination assigned : all) {
            ControlMatrix matrix;
            for (const Destination destination : all) {
                matrix.at(Source::cc2, destination) =
                    destination == Destination::lfo1_rate || destination == Destination::lfo2_rate
                            || destination == Destination::pitch
                            || destination == Destination::tvf_cutoff
                            || destination == Destination::amplitude
                        ? ControlMatrix::neutral
                        : 0;
            }
            matrix.at(Source::cc2, assigned) = 0x7F;

            const ControlMatrix::Modulation m = matrix.applied_linear(Source::cc2, 100);
            for (const Destination destination : all) {
                INFO("assigned " << static_cast<int>(assigned) << ", looked at "
                                 << static_cast<int>(destination));
                if (destination == assigned) {
                    CHECK(field(m, destination) != 0);
                } else {
                    CHECK(field(m, destination) == 0);
                }
            }
        }
    }

    SECTION("the three scalings are what the published ranges imply")
    {
        ControlMatrix matrix;
        for (const Destination destination : all) {
            matrix.at(Source::cc1, destination) = 0x7F;
        }
        const ControlMatrix::Modulation m = matrix.applied_linear(Source::cc1, 64);

        // Pitch takes the product whole; cutoff and the rates take it halved. Both are measured
        // from 0x40, so a full-scale depth is 0x3f away from centre.
        CHECK(m.pitch == 0x3F * 64);
        CHECK(m.tvf_cutoff == (0x3F * 64) >> 1);
        CHECK(m.lfo1_rate == (0x3F * 64) >> 1);

        // The LFO depths are amounts, not offsets: they are quartered and measured from zero, so a
        // full-scale depth is the whole 0x7f.
        CHECK(m.lfo1_pitch == (0x7F * 64) >> 2);
        CHECK(m.lfo2_tva == (0x7F * 64) >> 2);
    }

    SECTION("a negative depth mirrors a positive one exactly")
    {
        // The halving is a magnitude shift with the sign reapplied, not an arithmetic shift: an
        // arithmetic shift of a negative rounds toward minus infinity, which would make the two
        // sides of centre disagree by a count.
        ControlMatrix positive;
        ControlMatrix negative;
        positive.at(Source::cc1, Destination::tvf_cutoff) = ControlMatrix::neutral + 0x1F;
        negative.at(Source::cc1, Destination::tvf_cutoff) = ControlMatrix::neutral - 0x1F;
        CHECK(positive.applied_linear(Source::cc1, 33).tvf_cutoff
              == -negative.applied_linear(Source::cc1, 33).tvf_cutoff);
    }
}

TEST_CASE("the matrix's two apply laws agree on which destination is which", "[dsp][sccore]")
{
    using Source = ControlMatrix::Source;
    using Destination = ControlMatrix::Destination;

    // Bend's law is not a variant of the others -- it multiplies by a per-destination constant
    // where the linear one shifts. What the two must agree on is the *classification*: which
    // destinations are measured from centre and which are plain amounts. They were read out of two
    // separate functions, so agreement is evidence rather than tautology.
    ControlMatrix matrix;

    // A centred destination reads zero at centre under both laws, whatever the amount.
    CHECK(matrix.applied_linear(Source::cc1, 127).tvf_cutoff == 0);
    CHECK(matrix.applied_bipolar(Source::bend, 8000).tvf_cutoff == 0);

    // A unipolar depth reads zero at zero under both, and the power-on matrix has them at zero.
    CHECK(matrix.applied_linear(Source::cc1, 127).lfo2_tva == 0);
    CHECK(matrix.applied_bipolar(Source::bend, 8000).lfo2_tva == 0);

    // Off centre, both produce something, and both mirror about centre.
    matrix.at(Source::bend, Destination::tvf_cutoff) = ControlMatrix::neutral + 0x20;
    const int up = matrix.applied_bipolar(Source::bend, 8000).tvf_cutoff;
    matrix.at(Source::bend, Destination::tvf_cutoff) = ControlMatrix::neutral - 0x20;
    const int down = matrix.applied_bipolar(Source::bend, 8000).tvf_cutoff;
    CHECK(up != 0);
    CHECK(up == -down);

    // The two signs multiply: an inverted assignment driven the other way comes back positive.
    // That is the whole point of a negative depth, and it is easy to lose to an abs().
    CHECK(matrix.applied_bipolar(Source::bend, -8000).tvf_cutoff == up);

    // A unipolar depth takes its sign from the amount alone -- the depth has no side to be on.
    matrix.at(Source::bend, Destination::lfo1_pitch) = 0x40;
    const int rising = matrix.applied_bipolar(Source::bend, 8000).lfo1_pitch;
    const int falling = matrix.applied_bipolar(Source::bend, -8000).lfo1_pitch;
    CHECK(rising > 0);
    CHECK(falling == -rising);

    // Pitch is the destination that takes the product whole, so it must exceed a halved one at the
    // same depth -- the same ordering the linear law has. Its depth is passed in rather than read
    // from the matrix, because bend's pitch cell lives in `Part::bend_range`.
    const ControlMatrix::Modulation m =
        matrix.applied_bipolar(Source::bend, 8000, ControlMatrix::neutral + 0x20);
    CHECK(m.pitch > std::abs(m.tvf_cutoff));
    CHECK(matrix.applied_bipolar(Source::bend, 8000).pitch == 0);
}

TEST_CASE("the pitch ramp reproduces the engine's measured glide", "[pitch][ramp][sccore]")
{
    // Ground truth, read live out of the DLL for drum tone 1946 (TR-808 open hat, kit 11 key 46,
    // velocity 100): its pitch ramp is armed from 238484 to 229376 with a step of -4553, and across
    // the first 32-sample chunk the sampler's read position advances 33.693481 samples, leaving a
    // phase of 45448/65536. That phase is what selects interpolator row 88 instead of row 0 for the
    // rest of the note, and it is worth about 1.1 dB.
    const fs::path path = testdata::require_sccore();
    const RomImage rom = RomImage::open(path.string(), RomVerification::quick);
    const TableSet tables = TableSet::from_rom(rom);
    const PitchRamp reference{tables.ramp_exp()};

    // The endpoints decode to the increments the DLL reports at each end of the glide.
    CHECK(reference.increment_of(238484) == 96199);
    CHECK(reference.increment_of(229376) == 65536);

    // 0x38000 is unity and an octave is 16384 units, so a ratio round-trips through the domain.
    CHECK(PitchRamp::domain_of(1.0) == PitchRamp::unity);
    CHECK(PitchRamp::domain_of(2.0) == PitchRamp::unity + PitchRamp::units_per_octave);

    // Walk the ramp the way the engine does -- one step per 8-sample slot, four slots to a chunk --
    // and the accumulated phase has to land on the measured 45448 exactly.
    PitchRamp ramp{tables.ramp_exp()};
    const double from = std::exp2((238484.0 - PitchRamp::unity) / PitchRamp::units_per_octave);
    const double to = 1.0;
    ramp.arm(from, to);
    REQUIRE(ramp.is_active());

    std::int64_t advance = 0;
    for (int slot = 0; slot < 4; ++slot) {
        advance += static_cast<std::int64_t>(ramp.next_slot()) * PitchRamp::samples_per_slot;
    }

    CHECK(advance / 65536 == 33);
    CHECK(advance % 65536 == 45448);

    // And it settles: once the target is reached the ramp stops moving and reports unity.
    CHECK_FALSE(ramp.is_active());
    CHECK(ramp.next_slot() == 65536);
}


TEST_CASE("every matrix destination rails at its published range", "[dsp]")
{
    using Destination = ControlMatrix::Destination;

    // The scale is what fixes each destination's unit, so the check is that a full-scale sum comes
    // out as the figure the GS documentation names -- not as whatever the constants happen to give.
    // Written as the published quantity beside the raw result, because the raw result alone would
    // be a restatement of the table rather than a test of it.
    struct Case {
        Destination destination;
        int rail;
        const char* published;
    };

    const std::array<Case, 11> cases{{
        {Destination::pitch, 24000, "24 semitones, in milli-semitones"},
        {Destination::tvf_cutoff, 24576, "9600 cents, at 2.56 units a cent"},
        {Destination::amplitude, 32512, "100 %, as a fraction of 0x7f00"},
        {Destination::lfo1_rate, 6553, "10 Hz, as a per-tick phase increment"},
        {Destination::lfo1_pitch, 6000, "600 cents"},
        {Destination::lfo1_tvf, 6144, "2400 cents"},
        {Destination::lfo1_tva, 32512, "100 %"},
        {Destination::lfo2_rate, 6553, "10 Hz"},
        {Destination::lfo2_pitch, 6000, "600 cents"},
        {Destination::lfo2_tvf, 6144, "2400 cents"},
        {Destination::lfo2_tva, 32512, "100 %"},
    }};

    for (const Case& entry : cases) {
        const ControlMatrix::Law law = ControlMatrix::law(entry.destination);
        INFO(entry.published);
        CHECK(ControlMatrix::scaled(entry.destination, law.clamp) == entry.rail);
        CHECK(ControlMatrix::scaled(entry.destination, -law.clamp) == -entry.rail);

        // Past the clamp nothing more happens, and the clamp is symmetric.
        CHECK(ControlMatrix::scaled(entry.destination, law.clamp * 4) == entry.rail);
        CHECK(ControlMatrix::scaled(entry.destination, 0) == 0);

        // The engine's intermediate is an unsigned short, and the clamp is what keeps it from
        // wrapping. If a law ever changed so that it could, this is where it would show.
        CHECK((law.clamp << law.shift) <= 0xFFFF);
    }
}

TEST_CASE("the pitch destination is stored clamped to plus or minus 24 semitones", "[dsp]")
{
    using Destination = ControlMatrix::Destination;
    using Source = ControlMatrix::Source;

    ControlMatrix matrix;

    // 0x28-0x58 is the pitch destination's whole range, and a byte outside it is clamped rather
    // than taken. Storing it raw is what this engine used to do, and the module disagreed audibly.
    matrix.store(Source::modulation, Destination::pitch, 0x7F);
    CHECK(matrix.at(Source::modulation, Destination::pitch) == 0x58);
    matrix.store(Source::modulation, Destination::pitch, 0x00);
    CHECK(matrix.at(Source::modulation, Destination::pitch) == 0x28);
    matrix.store(Source::modulation, Destination::pitch, 0x50);
    CHECK(matrix.at(Source::modulation, Destination::pitch) == 0x50);

    // Nothing else is clamped: the other ten destinations take the byte as it arrives.
    matrix.store(Source::modulation, Destination::amplitude, 0x7F);
    CHECK(matrix.at(Source::modulation, Destination::amplitude) == 0x7F);
    matrix.store(Source::modulation, Destination::lfo1_tva, 0x00);
    CHECK(matrix.at(Source::modulation, Destination::lfo1_tva) == 0x00);

    // What the clamp buys, which is the part that was wrong: with the depth at its maximum the
    // pitch rises *evenly across the whole controller* and reaches the rail exactly at 127. Stored
    // raw, the same sweep railed at 49 and then sat still -- a ramp that stops a third of the way
    // up, which is what the module was asked about and did not do.
    matrix.store(Source::modulation, Destination::pitch, 0x7F);
    const int at_full = ControlMatrix::scaled(
        Destination::pitch, matrix.applied_linear(Source::modulation, 127).pitch);
    const int at_half = ControlMatrix::scaled(
        Destination::pitch, matrix.applied_linear(Source::modulation, 64).pitch);
    const int just_under = ControlMatrix::scaled(
        Destination::pitch, matrix.applied_linear(Source::modulation, 126).pitch);

    CHECK(at_full == 24000);
    CHECK(just_under < at_full);
    CHECK_THAT(static_cast<double>(at_half),
               WithinRel(static_cast<double>(at_full) * 64.0 / 127.0, 0.01));
}

TEST_CASE("a matrix depth drives an LFO a patch gives no depth of its own", "[dsp]")
{
    const Fixture fixture = Fixture::make();

    // A plain triangle at a middling rate with every depth at zero: on its own this LFO is silent
    // at all three destinations, which is what makes anything measured here the matrix's doing.
    LfoConfig config;
    config.waveform = 5;
    config.increment = 3000;
    config.delay_rate = 0xFFFF;
    config.fade_rate = 0xFFFF;

    LfoRunner runner = fixture.lfo().create_runner(config);
    for (int tick = 0; tick < 8; ++tick) {
        runner.tick();
    }
    REQUIRE(runner.is_applied());

    CHECK(runner.value(LfoDestination::pitch) == 0.0);
    CHECK(runner.value(LfoDestination::tvf) == 0.0);
    CHECK(runner.value(LfoDestination::tva) == 0.0);

    // With a depth assigned, all three sound -- and the sign follows the depth, since the waveform
    // is the same one in each case.
    const double pitch = runner.value(LfoDestination::pitch, 6000);
    const double tvf = runner.value(LfoDestination::tvf, 6144);
    const double tva = runner.value(LfoDestination::tva, 0x7F00);
    CHECK(pitch != 0.0);
    CHECK(tvf != 0.0);
    CHECK(tva != 0.0);
    CHECK(runner.value(LfoDestination::pitch, -6000) == -pitch);

    // The depth is clamped to the destination's own limit, which is also the matrix's full scale --
    // so a rail is reachable but never exceeded.
    CHECK(runner.value(LfoDestination::pitch, 12000) == pitch);
}

TEST_CASE("the matrix's LFO rate moves the increment, and can stop the LFO", "[dsp]")
{
    const Fixture fixture = Fixture::make();

    LfoConfig config;
    config.waveform = 5;
    config.increment = 1000;
    config.delay_rate = 0xFFFF;
    config.fade_rate = 0xFFFF;

    // Two runners over the same configuration, one of them handed a rate offset every tick: the
    // faster one gets further round the cycle, which is the whole of what a rate destination does.
    const auto phase_after = [&](int rate_offset, int ticks) {
        LfoRunner runner = fixture.lfo().create_runner(config);
        double travelled = 0.0;
        for (int tick = 0; tick < ticks; ++tick) {
            runner.tick(rate_offset);
            travelled += std::abs(runner.value(LfoDestination::pitch, 6000));
        }
        return travelled;
    };

    CHECK(phase_after(2000, 4) != phase_after(0, 4));

    // An offset that drives the increment to nothing stops the LFO outright rather than freezing
    // its phase: the engine skips the entire update, so no depth is applied either.
    LfoRunner stalled = fixture.lfo().create_runner(config);
    for (int tick = 0; tick < 8; ++tick) {
        stalled.tick(-1000);
    }
    CHECK_FALSE(stalled.is_applied());
    CHECK(stalled.value(LfoDestination::pitch, 6000) == 0.0);

    // And it is only the total that matters, so the same LFO revives when the offset lifts.
    stalled.tick(0);
    CHECK(stalled.is_applied());
}



TEST_CASE("the random LFO shapes redraw on the phase wrapping", "[dsp]")
{
    // The three random shapes are the reason the runner holds waveform state at all: what they
    // depend on is the phase having *wrapped*, which a function of the phase cannot see.
    const Fixture fixture = Fixture::make();

    // An increment that divides 0x10000 exactly, so the wrap lands on a tick this test can name:
    // 0x2000 wraps on every eighth. It has to stay under the 0x28f6 ceiling `tick` applies, or the
    // clamp picks the period instead and the arithmetic here is quietly wrong.
    LfoConfig config;
    config.increment = 0x2000;
    static_assert(0x2000 <= 0x28F6);
    config.delay_rate = 0xFFFF;
    config.fade_rate = 0xFFFF;
    config.pitch_depth = 0;

    SECTION("sample and hold steps only on the wrap")
    {
        config.waveform = 1;
        LfoRunner runner = fixture.lfo().create_runner(config);

        std::vector<double> values;
        for (int tick = 0; tick < 40; ++tick) {
            runner.tick();
            // A patch depth of zero: everything seen here is the matrix's, which is the case a
            // silent-random-shape engine got wrong in both directions at once.
            values.push_back(runner.value(LfoDestination::pitch, 6000));
        }

        // The value is flat except where it steps, and it steps only on the wrap. Found rather
        // than assumed, so the test reports the period it saw instead of failing on an assumption
        // about which tick the first one lands on.
        std::vector<std::size_t> steps;
        for (std::size_t i = 1; i < values.size(); ++i) {
            if (values[i] != values[i - 1]) {
                steps.push_back(i);
            }
        }

        INFO("stepped at " << steps.size() << " ticks");
        REQUIRE(steps.size() >= 3);
        for (std::size_t i = 1; i < steps.size(); ++i) {
            CHECK(steps[i] - steps[i - 1] == 8);
        }

        // And it does move: a shape that returned zero throughout would pass every check above.
        CHECK(std::set<double>(values.begin(), values.end()).size() > 2);
    }

    SECTION("the slewed shapes walk toward the draw instead of stepping to it")
    {
        config.waveform = 2;
        LfoRunner runner = fixture.lfo().create_runner(config);

        std::vector<double> values;
        for (int tick = 0; tick < 24; ++tick) {
            runner.tick();
            values.push_back(runner.value(LfoDestination::pitch, 6000));
        }

        // The walk is 0x50 a tick against a full-scale draw, so consecutive ticks differ by a
        // bounded step where sample-and-hold would jump the whole way at once.
        const double ceiling = (static_cast<double>(LfoEngine::slew_step) * 6000.0 * 2.0) / 65536.0;
        for (std::size_t i = 1; i < values.size(); ++i) {
            INFO("tick " << i << ": " << values[i - 1] << " -> " << values[i]);
            CHECK(std::abs(values[i] - values[i - 1]) <= ceiling + 1.0);
        }
        CHECK(std::set<double>(values.begin(), values.end()).size() > 1);
    }

    SECTION("shapes 2 and 3 are the same shape")
    {
        // Not an assumption: the engine has two switch cases for them and the two are identical
        // instruction for instruction, same step and same rails. Pinned so that a later reading of
        // one of them cannot quietly diverge from the other.
        EngineNoise left;
        EngineNoise right;
        LfoEngine engine_two{fixture.tables(), &left};
        LfoEngine engine_three{fixture.tables(), &right};

        config.waveform = 2;
        LfoRunner two = engine_two.create_runner(config);
        config.waveform = 3;
        LfoRunner three = engine_three.create_runner(config);

        for (int tick = 0; tick < 32; ++tick) {
            two.tick();
            three.tick();
            INFO("tick " << tick);
            REQUIRE(two.value(LfoDestination::pitch, 6000)
                    == three.value(LfoDestination::pitch, 6000));
        }
    }

    SECTION("the shapes share the engine's one generator")
    {
        // The module has a single generator and the LFO shapes, the pitch start jitter and the
        // random pan all draw from it, so two voices sounding together do not repeat each other.
        EngineNoise shared;
        LfoEngine engine{fixture.tables(), &shared};
        config.waveform = 1;

        LfoRunner first = engine.create_runner(config);
        LfoRunner second = engine.create_runner(config);

        std::vector<double> a;
        std::vector<double> b;
        for (int tick = 0; tick < 16; ++tick) {
            first.tick();
            second.tick();
            a.push_back(first.value(LfoDestination::pitch, 6000));
            b.push_back(second.value(LfoDestination::pitch, 6000));
        }
        CHECK(a != b);
    }

    SECTION("waveform() cannot evaluate them and says so by returning zero")
    {
        for (const int shape : {1, 2, 3}) {
            INFO("shape " << shape);
            CHECK(LfoEngine::is_random(shape));
            for (const int phase : {0, 0x1234, 0x8000, 0xFFFF}) {
                CHECK(fixture.lfo().waveform(phase, shape) == 0);
            }
        }
        CHECK_FALSE(LfoEngine::is_random(0));
        CHECK_FALSE(LfoEngine::is_random(4));
    }
}



TEST_CASE("an LFO's delay holds back the patch's depth, not a controller's", "[dsp]")
{
    const Fixture fixture = Fixture::make();

    // A saw, so a tick that reports nothing really is nothing rather than a zero crossing.
    LfoConfig config;
    config.waveform = 5;
    config.increment = 0x1000;
    config.delay_rate = 0x0400;   // 0xffff / 0x400, so the delay completes on the 64th tick
    config.fade_rate = 0xFFFF;
    config.pitch_depth = 6000;

    const auto peak_over = [](LfoRunner& runner, int ticks, int matrix_depth) {
        double seen = 0.0;
        for (int tick = 0; tick < ticks; ++tick) {
            runner.tick();
            seen = std::max(seen, std::abs(runner.value(LfoDestination::pitch, matrix_depth)));
        }
        return seen;
    };

    // The patch's own depth is scaled by the fade-in, and the fade does not start until the delay
    // has run out -- so for the first sixty-odd ticks the patch contributes nothing.
    LfoRunner patch_only = fixture.lfo().create_runner(config);
    CHECK(peak_over(patch_only, 8, 0) == 0.0);

    // A controller's depth is summed *past* the fade, so it sounds from the first tick. This is the
    // distinction the engine draws and this one did not: suppressing the whole update during the
    // delay silences the controller too, which the module was measured not to do.
    LfoRunner controlled = fixture.lfo().create_runner(config);
    CHECK(peak_over(controlled, 8, 6000) > 1000.0);

    // And once the delay elapses the patch's own depth arrives, so the delay is still a delay.
    CHECK(peak_over(patch_only, 70, 0) > 1000.0);
}
