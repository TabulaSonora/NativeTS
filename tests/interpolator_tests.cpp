#include "tabulasonora/interpolator.hpp"

#include "test_data.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <vector>

using namespace ts;
using Catch::Matchers::WithinAbs;

TEST_CASE("the zero-phase row is a mild lowpass, not a passthrough", "[interp][sccore]")
{
    // The most timbre-defining fact about this engine. Substituting linear interpolation -- which
    // would be [0, 1, 0, 0] here -- makes everything measurably brighter, and it is the kind of
    // change that sounds like an improvement while being wrong.
    const TableSet tables = TableSet::from_cache_directory(testdata::require_tables());
    const auto coefficients = tables.interp_coef();

    CHECK_THAT(coefficients[0], WithinAbs(0.174, 0.001));
    CHECK_THAT(coefficients[1], WithinAbs(0.653, 0.001));
    CHECK_THAT(coefficients[2], WithinAbs(0.173, 0.001));
    CHECK_THAT(coefficients[3], WithinAbs(0.0, 0.001));
}

TEST_CASE("a constant signal resamples to itself", "[interp][sccore]")
{
    // Every phase row sums to one, so a flat input must come out flat at any fractional position.
    // This catches an off-by-one in the tap window that a smooth signal would hide.
    //
    // "One" is the measured 1.0 to 1.00001 of the actual table, not an exact 1.0 -- row 0's fourth
    // tap is literally 1e-5. The tolerance is that measurement, not a number picked to pass.
    const TableSet tables = TableSet::from_cache_directory(testdata::require_tables());
    const Interpolator interpolator{tables};

    const std::vector<float> flat(64, 0.25F);

    for (int step = 0; step < 64; ++step) {
        const double position = 8.0 + static_cast<double>(step) / 64.0;
        INFO("position " << position);
        CHECK_THAT(interpolator.sample(flat, position), WithinAbs(0.25, 0.25 * 1.1e-5));
    }
}

TEST_CASE("the read index is clamped to leave room for all four taps", "[interp][sccore]")
{
    // The window reaches from i-1 to i+2. Without the clamp this reads out of bounds at both ends,
    // which is a crash in a debug build and silent garbage in a release one.
    const TableSet tables = TableSet::from_cache_directory(testdata::require_tables());
    const Interpolator interpolator{tables};

    const std::vector<float> ramp{0.0F, 1.0F, 2.0F, 3.0F, 4.0F, 5.0F, 6.0F, 7.0F};

    CHECK_NOTHROW(interpolator.sample(ramp, -10.0));
    CHECK_NOTHROW(interpolator.sample(ramp, 0.0));
    CHECK_NOTHROW(interpolator.sample(ramp, 1000.0));

    // A position below the first usable index reads the same window as that index.
    CHECK(interpolator.sample(ramp, -5.0) == interpolator.sample(ramp, 1.0));
}

TEST_CASE("the kernel is darker than linear interpolation", "[interp][sccore]")
{
    // At a quarter of the sample rate the engine kernel passes 0.638 where linear passes 0.707.
    // Measured here rather than asserted from the table, because it is the audible consequence.
    const TableSet tables = TableSet::from_cache_directory(testdata::require_tables());
    const Interpolator interpolator{tables};

    // A sine at fs/4, sampled long enough to measure amplitude away from the clamped edges.
    std::vector<float> wave(256);
    for (std::size_t i = 0; i < wave.size(); ++i) {
        wave[i] = static_cast<float>(std::sin(std::numbers::pi / 2.0 * static_cast<double>(i)));
    }

    double peak = 0.0;
    for (int i = 0; i < 200; ++i) {
        const double position = 16.0 + static_cast<double>(i) * 0.25;
        peak = std::max(peak, std::abs(static_cast<double>(interpolator.sample(wave, position))));
    }

    CHECK(peak < 0.71);
    CHECK_THAT(peak, WithinAbs(0.638, 0.02));
}

TEST_CASE("the ring sampler wraps a power-of-two buffer", "[interp][sccore]")
{
    const TableSet tables = TableSet::from_cache_directory(testdata::require_tables());
    const Interpolator interpolator{tables};

    const std::vector<float> ring(16, 0.5F);

    // Indices past the end wrap rather than reading out of bounds, and a flat ring stays flat
    // across the wrap.
    constexpr double tolerance = 0.5 * 1.1e-5; // the table's own row-sum error, see above
    CHECK_THAT(interpolator.sample_ring(ring, 0, 0.0), WithinAbs(0.5, tolerance));
    CHECK_THAT(interpolator.sample_ring(ring, 15, 0.5), WithinAbs(0.5, tolerance));
    CHECK_THAT(interpolator.sample_ring(ring, 1'000'000, 0.25), WithinAbs(0.5, tolerance));
}

TEST_CASE("the pan law is the table, not a computed curve", "[pan][sccore]")
{
    // Recovered by sweeping the controller through the engine and reading per-channel RMS. Centre
    // is 75/127, which is neither the 0.707 of a constant-power law nor the 0.5 of a linear one --
    // substituting either is an audible change to every patch's stereo image.
    const TableSet tables = TableSet::from_cache_directory(testdata::require_tables());
    const PanLaw pan{tables};

    const auto [centre_left, centre_right] = pan.gains(PanLaw::centre);
    CHECK_THAT(centre_left, WithinAbs(75.0 / 127.0, 1e-9));
    CHECK_THAT(centre_left, WithinAbs(centre_right, 1e-12));
    CHECK_THAT(centre_left, WithinAbs(0.5906, 0.0001));

    // Pan 0 is fully left: the right channel is silent, not merely attenuated.
    const auto [hard_left_l, hard_left_r] = pan.gains(0);
    CHECK(hard_left_r == 0.0);
    CHECK(hard_left_l > 0.9);

    // Out-of-range values clamp rather than reading off the end of the table.
    CHECK(pan.gains(-5) == pan.gains(0));
    CHECK(pan.gains(999) == pan.gains(127));
}

TEST_CASE("the pan law is monotone across the sweep", "[pan][sccore]")
{
    const TableSet tables = TableSet::from_cache_directory(testdata::require_tables());
    const PanLaw pan{tables};

    double previous_left = 2.0;
    for (int p = 0; p <= 127; ++p) {
        const auto [left, right] = pan.gains(p);
        INFO("pan " << p << " -> " << left << ", " << right);
        CHECK(left <= previous_left);
        CHECK(left >= 0.0);
        CHECK(right >= 0.0);
        previous_left = left;
    }
}
