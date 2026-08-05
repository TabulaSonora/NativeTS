#include "tabulasonora/control_ramp.hpp"
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
