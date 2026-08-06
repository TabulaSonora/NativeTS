#include "tabulasonora/control_ramp.hpp"
#include "tabulasonora/part.hpp"
#include "tabulasonora/tva_chain.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <cstdint>

using namespace ts;

// The part-volume anti-zipper ramp, against numbers read out of the engine itself.
//
// The oracle trace is `scdec volramp 19 96 110 8 220 4 0`: a held note at CC#7 127, stepped to 0,
// with `voice_ctrl_ramp_b`'s own gain buffer (`DAT_181a1cbb0`, located by searching memory for the
// float that tracks CC#7 rather than by trusting a label) read every eight samples. What it shows
// is the whole specification of this class, and each number below is quoted from it.

namespace {

/// The rate word the engine hard-codes for this path, and the mask it selects.
constexpr int rate = ControlRamp::volume_rate_word;
constexpr unsigned no_hold = 0;

/// Full scale, in the units the ramp's target is set in.
[[nodiscard]] int full_scale_target()
{
    return ControlRamp::target_of(TvaChain::part_volume_word(127, 127, 127));
}

} // namespace

TEST_CASE("the part-volume ramp closes a fixed fraction of the error per update", "[control_ramp]")
{
    // The oracle's gain falls by a *constant ratio* of 0.90403 per 32-sample call. A constant ratio
    // is what a step rescaled from the live error produces; a fixed decrement would fall linearly.
    // 0.904046 is (1 - 204/8192)^4 -- four updates per call, one every eight samples.
    constexpr double per_call = 0.90403;

    ControlRamp ramp;
    ramp.seed(full_scale_target(), rate, no_hold);
    for (int i = 0; i < 64; ++i) {
        (void)ramp.step();
    }
    ramp.retarget(0, rate, no_hold);

    double previous = 0.0;
    for (int call = 0; call < 12; ++call) {
        double first = 0.0;
        for (int i = 0; i < 32; ++i) {
            const double gain = ramp.step();
            if (i == 0) {
                first = gain;
            }
        }
        if (previous > 0.0) {
            // The tolerance is the gain word's own quantisation: it is 14-bit, so a ratio taken
            // between neighbouring readings cannot be tighter than about 1e-4.
            CHECK(first / previous == Catch::Approx(per_call).margin(2e-4));
        }
        previous = first;
    }
}

TEST_CASE("the part-volume ramp is eight samples to an update", "[control_ramp]")
{
    // Held flat within a sub-chunk. This is the fact the tables get wrong on their own -- the rate
    // word's divider bits select mask zero, which read literally would be one update a sample and a
    // glide eight times too fast. The oracle pins it twice over: the gain buffer holds one value
    // per call, and at rest it creeps by exactly 1/16384 every 64 samples, which is eight pushes of
    // `minimum_step` rather than sixty-four.
    ControlRamp ramp;
    ramp.seed(full_scale_target(), rate, no_hold);
    ramp.retarget(0, rate, no_hold);

    std::array<double, 24> gains{};
    for (double& gain : gains) {
        gain = ramp.step();
    }

    // Three changes across twenty-four samples, and every run between them eight long.
    int changes = 0;
    for (std::size_t i = 1; i < gains.size(); ++i) {
        if (gains[i] != gains[i - 1]) {
            CHECK(i % ControlRamp::samples_per_update == 7);
            ++changes;
        }
    }
    CHECK(changes == 3);
}

TEST_CASE("the part-volume ramp reaches its target and stays there", "[control_ramp]")
{
    // A proportional step alone stalls: once the error shifts down to zero the move is zero. The
    // `minimum_step` floor is what carries it the last of the way, so a fade actually reaches
    // silence rather than parking a hair above it.
    ControlRamp ramp;
    ramp.seed(full_scale_target(), rate, no_hold);
    ramp.retarget(0, rate, no_hold);

    // 45 ms is the settle the 9.9 ms time constant implies; give it double and it must be there.
    for (int i = 0; i < 32000 / 10; ++i) {
        (void)ramp.step();
    }
    CHECK(ramp.current() == 0);
    CHECK(ramp.step() == ControlRamp::floor_gain);
}

TEST_CASE("a seeded ramp does not glide", "[control_ramp]")
{
    // `tvf_env_prep` writes the same value into the ramp's source and target slots, so a note
    // struck part-way through a fade starts at the level the fade has reached instead of sweeping
    // up to it from wherever the recycled voice left off.
    ControlRamp ramp;
    ramp.seed(full_scale_target(), rate, no_hold);
    const double first = ramp.step();
    for (int i = 0; i < 200; ++i) {
        CHECK(std::abs(ramp.step() - first) < 1e-4);
    }
}

TEST_CASE("the volume word matches the engine's own integer law", "[control_ramp]")
{
    // Everything at 127 is 32762, not a round 32768 -- so full volume rests a whisker under
    // unity, at 0.99982. The oracle's own reading of its gain buffer at CC#7 127 agrees.
    CHECK(TvaChain::part_volume_word(127, 127, 127) == 32762);
    CHECK(TvaChain::part_volume_word(0, 127, 127) == 0);

    // Volume and expression enter symmetrically, and the result is squared: halving either drops
    // the gain by a factor of four.
    const double full = TvaChain::part_volume_word(127, 127, 127);
    CHECK(TvaChain::part_volume_word(64, 127, 127) / full == Catch::Approx(0.2540).margin(2e-3));
    CHECK(TvaChain::part_volume_word(127, 64, 127) == TvaChain::part_volume_word(64, 127, 127));

    // The oracle's own reading at CC#7 64, from `scdec volscan`: 1.000549 -> 0.253662.
    CHECK(0.253662 / 1.000549 == Catch::Approx(0.2540).margin(2e-3));
}

TEST_CASE("the matrix ramp reproduces the engine's coefficient walk", "[control_ramp]")
{
    // `scdec fxmatrix sx:40,01,33 0 127` drives the GS reverb level and reads the engine's own
    // coefficient a block at a time. These are its first eight values, and the port reproduces them
    // exactly -- 123 blocks compared, no mismatch, settling on 32512 rather than stalling short.
    constexpr std::array<int, 8> oracle{5594, 10229, 14069, 17248, 19877, 22055, 23860, 25354};

    MatrixRamp ramp;
    std::array<double, 32> gains{};

    // Seeded at zero by taking the first block at level zero, which is where the capture starts.
    ramp.fill(0, gains);
    REQUIRE(ramp.current() == 0);

    for (const int expected : oracle) {
        ramp.fill(127, gains);
        CHECK(ramp.current() == expected);
    }
}

TEST_CASE("the matrix ramp arrives instead of stalling short", "[control_ramp]")
{
    // There is no minimum step here. Arrival comes from the shift: both branches shift the
    // *negative* of the error, and an arithmetic shift right rounds toward negative infinity, so
    // the magnitude rounds up and cannot truncate to zero. Taking `(target - current) * rate >>
    // shift` in both directions instead parks 85 short of a full-scale target.
    MatrixRamp ramp;
    std::array<double, 32> gains{};
    ramp.fill(0, gains);

    for (int block = 0; block < 200; ++block) {
        ramp.fill(127, gains);
    }
    CHECK(ramp.current() == MatrixRamp::target_of(127));

    // And back down, which exercises the other branch.
    for (int block = 0; block < 200; ++block) {
        ramp.fill(0, gains);
    }
    CHECK(ramp.current() == 0);
}

TEST_CASE("a level at unity is exactly unity", "[control_ramp]")
{
    // 0x40 is the power-on level and has to decode to 1.0 on the nose, because the mixer skips the
    // multiply entirely in that state and any drift would show up as a silent level change.
    MatrixRamp ramp;
    std::array<double, 32> gains{};
    ramp.fill(0x40, gains);
    CHECK(ramp.current() == MatrixRamp::target_of(0x40));
    for (const double gain : gains) {
        CHECK(gain == 1.0);
    }
}

TEST_CASE("a part's chorus and delay sends cross full scale in 127 ticks", "[control_ramp]")
{
    // These two are not per-voice. They are single taps in the 33-bus send matrix -- chorus at
    // DAT_181a6f310 tap 1, delay at DAT_181a6e8c0 tap 1 -- one coefficient a part, and the engine
    // slews them there. Measured with `scdec mattrace 93 1 1 0 127`: 64 steps of 16/1024, one every
    // 640 samples, 1255 ms. Which is one controller unit a control tick, the same effective rate as
    // the per-voice reverb send, just taken in double steps at half the cadence.
    Part part;
    REQUIRE(part.chorus_send_level() == 0.0);

    part.chorus_send = 127;
    part.delay_send = 127;

    int ticks = 0;
    while (part.chorus_send_level() < 127.0 && ticks < 1000) {
        part.slew_sends(Part::send_slew_per_tick);
        ++ticks;
    }
    CHECK(ticks == 127);
    CHECK(part.delay_send_level() == 127.0);

    // 127 ticks is 1.27 s at the 100 Hz control rate.
    CHECK(ticks / 100.0 == Catch::Approx(1.27).margin(0.01));

    // And back down at the same rate, so a send being turned off keeps feeding the bus on its way.
    part.chorus_send = 0;
    for (int i = 0; i < 63; ++i) {
        part.slew_sends(Part::send_slew_per_tick);
    }
    CHECK(part.chorus_send_level() == Catch::Approx(64.0).margin(1e-9));
}

// The filter's frequency-coefficient ramp, against the engine's own trace.
//
// The oracle is `scdec svfslew 81 6 60 100 20 110 100 32 40 xg`: program 81 on XG bank LSB 6 -- the
// OB saw two channels of `MM6_-_MrX2010XG.mid` play -- held while CC#74 steps from 20 to 110, with
// `g_svf_f_coef` read every 32 samples. The same mode reads the ramp's own slot in
// `g_voice_ramp_cutoff`, which is where the numbers below come from rather than being inferred from
// the decoded trace.
TEST_CASE("the coefficient ramp walks the engine's own line", "[dsp]")
{
    // Read off the live ramp mid-glide: current 62976, target 181070, rate 204, step 4194.
    constexpr int start = 12648;
    constexpr int target = 181070;

    SECTION("the step is fixed when the target is set, and it is the engine's")
    {
        CoefficientRamp ramp;
        ramp.seed(start);
        ramp.retarget(target);

        const int before = ramp.current();
        for (int i = 0; i < CoefficientRamp::samples_per_update; ++i) {
            (void)ramp.step();
        }
        CHECK(ramp.current() - before == 4194);

        // And it stays fixed rather than rescaling from the shrinking distance, which is the whole
        // difference between this ramp and `ControlRamp`. A second update moves exactly as far.
        const int after_one = ramp.current();
        for (int i = 0; i < CoefficientRamp::samples_per_update; ++i) {
            (void)ramp.step();
        }
        CHECK(ramp.current() - after_one == 4194);
    }

    SECTION("the decoded coefficient advances by the engine's own increment")
    {
        CoefficientRamp ramp;
        ramp.seed(start);
        ramp.retarget(target);

        // The trace advances 0.127991 every 32 samples -- four updates -- in even steps.
        double previous = ramp.value();
        for (int chunk = 0; chunk < 8; ++chunk) {
            double value = 0.0;
            for (int n = 0; n < 32; ++n) {
                value = ramp.step();
            }
            INFO("chunk " << chunk);
            CHECK(value - previous == Catch::Approx(0.127991).margin(1e-05));
            previous = value;
        }
    }

    SECTION("it arrives on the target and stops there")
    {
        CoefficientRamp ramp;
        ramp.seed(start);
        ramp.retarget(target);

        // 168422 to close at 4194 an update is 41 updates, 328 samples.
        for (int n = 0; n < 328; ++n) {
            (void)ramp.step();
        }
        CHECK(ramp.current() == target);
        CHECK_FALSE(ramp.is_active());

        // And holds, rather than walking past it the way a fixed step would.
        for (int n = 0; n < 320; ++n) {
            (void)ramp.step();
        }
        CHECK(ramp.current() == target);
    }

    SECTION("only the climb needs a minimum step")
    {
        // Descending, the arithmetic shift of a negative product already rounds away from zero, so
        // a tiny distance still moves. Climbing, it truncates to nothing and would stall, which is
        // why the engine forces one and not the other.
        CoefficientRamp climbing;
        climbing.seed(1000);
        climbing.retarget(1001);
        for (int n = 0; n < CoefficientRamp::samples_per_update; ++n) {
            (void)climbing.step();
        }
        CHECK(climbing.current() == 1001);

        CoefficientRamp falling;
        falling.seed(1001);
        falling.retarget(1000);
        for (int n = 0; n < CoefficientRamp::samples_per_update; ++n) {
            (void)falling.step();
        }
        CHECK(falling.current() == 1000);
    }

    SECTION("a zero accumulator decodes to the engine's floor, not to silence")
    {
        CHECK(CoefficientRamp::decode(0) == CoefficientRamp::floor_value);
        CHECK(CoefficientRamp::decode(start) == Catch::Approx(0.096497).margin(1e-06));
        CHECK(CoefficientRamp::decode(target) == Catch::Approx(1.381409).margin(1e-06));
    }
}

// The damping ramp, which shares `ControlRamp`'s law rather than its neighbour's.
//
// `scdec svfslew 81 6 60 100 64 64 100 32 14 xg 0` holds the cutoff and steps CC#71 from 100 to 0,
// so only the resonance moves. The trace decays geometrically -- successive increments in a
// constant ratio -- where the frequency ramp's are even, which is how the two laws tell themselves
// apart. The rate read off the live slot is 0x100.
TEST_CASE("the damping ramp approaches exponentially at the engine's rate", "[dsp]")
{
    SECTION("the rate predicts the trace's decay")
    {
        // Four updates to a 32-sample chunk, each closing 256/8192 of what is left.
        const double per_update = 1.0 - (static_cast<double>(DampingRamp::rate_word) / 8192.0);
        const double per_chunk = per_update * per_update * per_update * per_update;

        // The measured ratio between successive 32-sample increments is 0.880.
        CHECK(per_chunk == Catch::Approx(0.880).margin(0.002));
    }

    SECTION("successive increments shrink, rather than staying even")
    {
        // q at resonance byte 4 up to q at 127 -- 0.0625 to 1.984375, the range CC#71 spans.
        DampingRamp ramp;
        ramp.seed(DampingRamp::encode(0.0625));
        ramp.retarget(DampingRamp::encode(1.984375));

        std::array<double, 6> increments{};
        double previous = ramp.value();
        for (std::size_t i = 0; i < increments.size(); ++i) {
            double value = 0.0;
            for (int n = 0; n < 32; ++n) {
                value = ramp.step();
            }
            increments[i] = value - previous;
            previous = value;
        }

        for (std::size_t i = 1; i < increments.size(); ++i) {
            INFO("increment " << i);
            CHECK(increments[i] < increments[i - 1]);
            // And by the rate's own ratio, which is what makes this the engine's curve rather than
            // merely a decelerating one.
            CHECK(increments[i] / increments[i - 1] == Catch::Approx(0.880).margin(0.02));
        }
    }

    SECTION("it arrives, because the minimum step stops the approach stalling")
    {
        DampingRamp ramp;
        ramp.seed(DampingRamp::encode(0.0625));
        ramp.retarget(DampingRamp::encode(1.984375));

        // A purely proportional approach never lands; the 0x400 floor is what makes it terminate.
        for (int n = 0; n < 32000; ++n) {
            (void)ramp.step();
        }
        CHECK_FALSE(ramp.is_active());
        CHECK(ramp.value() == Catch::Approx(1.984375).margin(1e-04));
    }

    SECTION("the two coefficient ramps do not share a law")
    {
        // Same endpoints through both: the frequency ramp's increments are even and the damping
        // ramp's decay. Reading the engine's two functions as one would have been the easy mistake.
        CoefficientRamp linear;
        linear.seed(DampingRamp::encode(0.0625));
        linear.retarget(DampingRamp::encode(1.984375));

        DampingRamp exponential;
        exponential.seed(DampingRamp::encode(0.0625));
        exponential.retarget(DampingRamp::encode(1.984375));

        const auto advance = [](auto& ramp) {
            double value = 0.0;
            for (int n = 0; n < 32; ++n) {
                value = ramp.step();
            }
            return value;
        };

        // On the accumulator rather than the decoded value: the decode drops three bits, so two
        // exactly equal accumulator steps can still decode a single LSB apart.
        const int linear_start = linear.current();
        (void)advance(linear);
        const int linear_first = linear.current() - linear_start;
        (void)advance(linear);
        const int linear_second = linear.current() - linear_start - linear_first;
        CHECK(linear_second == linear_first);

        const double exp_first = advance(exponential) - 0.0625;
        const double exp_second = advance(exponential) - 0.0625 - exp_first;
        CHECK(exp_second < exp_first * 0.95);
    }
}
