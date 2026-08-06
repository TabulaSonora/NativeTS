#include "tabulasonora/output_filter.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <vector>

using namespace ts;
using Catch::Matchers::WithinAbs;

// The module's output stage, against the state `scdec outfilt` reads back off the running engine.
TEST_CASE("the output filter is a half-sample allpass into a linear interpolator", "[dsp]")
{
    SECTION("the coefficient is a constant of the filter, not of the conversion")
    {
        // Read back at 22.05, 32, 44.1 and 48 kHz: the coefficient is 0.33333334 every time and
        // only the ratio moves.
        CHECK_THAT(OutputFilter::allpass_coefficient, WithinAbs(0.33333334, 1e-7));

        OutputFilter filter;
        filter.set_host_rate(32000);
        CHECK_THAT(filter.ratio(), WithinAbs(1.0, 1e-12));
        filter.set_host_rate(44100);
        CHECK_THAT(filter.ratio(), WithinAbs(0.7256236, 1e-6));
        filter.set_host_rate(48000);
        CHECK_THAT(filter.ratio(), WithinAbs(0.6666667, 1e-6));
        filter.set_host_rate(22050);
        CHECK_THAT(filter.ratio(), WithinAbs(1.4512472, 1e-6));
    }

    SECTION("at the engine's own rate it is exactly one sample of delay")
    {
        // The phase accumulator sits at zero, so the interpolation weight is zero and the output is
        // the previous input -- the allpass midpoint is computed and then weighted out entirely.
        // This is the whole of the filter's contribution to a render at 32 kHz, and it is why
        // engaging it moves a note-on by one sample and nothing else.
        OutputFilter filter;
        filter.set_host_rate(OutputFilter::engine_rate);
        filter.reset();

        const std::vector<float> input{0.25F, -0.5F, 0.75F, 1.0F, -0.125F, 0.0F, 0.5F};
        std::vector<float> output;
        for (const float sample : input) {
            output.push_back(filter.process(sample, sample).first);
        }

        // First out is the seeded zero; every one after is the input a sample earlier.
        CHECK_THAT(output.front(), WithinAbs(0.0, 1e-6));
        for (std::size_t i = 1; i < input.size(); ++i) {
            INFO("sample " << i);
            CHECK_THAT(output[i], WithinAbs(static_cast<double>(input[i - 1]), 1e-6));
        }
    }

    SECTION("both channels run independently")
    {
        OutputFilter filter;
        filter.set_host_rate(OutputFilter::engine_rate);
        filter.reset();

        (void)filter.process(1.0F, -1.0F);
        const auto [left, right] = filter.process(0.0F, 0.0F);
        CHECK_THAT(left, WithinAbs(1.0, 1e-6));
        CHECK_THAT(right, WithinAbs(-1.0, 1e-6));
    }
}
