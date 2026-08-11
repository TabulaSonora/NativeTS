#include "tabulasonora/output_filter.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <span>
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

    SECTION("driving it a frame at a time is the same filter")
    {
        // `process` is `push`, `at`, `advance` in that order, and the three exist so that a rate
        // other than the engine's can be served. At 1:1 they have to come to the same thing, or
        // the conversion path and the render path are two different filters.
        OutputFilter whole;
        OutputFilter parts;
        whole.set_host_rate(OutputFilter::engine_rate);
        parts.set_host_rate(OutputFilter::engine_rate);

        const std::vector<float> input{0.25F, -0.5F, 0.75F, 1.0F, -0.125F, 0.0F, 0.5F};
        for (const float sample : input) {
            const auto expected = whole.process(sample, -sample);

            parts.push(sample, -sample);
            const auto actual = parts.at();
            CHECK(parts.advance() == 1);

            CHECK_THAT(actual.first, WithinAbs(expected.first, 1e-12));
            CHECK_THAT(actual.second, WithinAbs(expected.second, 1e-12));
        }
    }
}

// The same filter asked to actually convert, which is what a plugin needs of it: the engine runs at
// 32 kHz and the host asks for its own rate.
TEST_CASE("the output filter converts between rates", "[dsp]")
{
    // One output frame at a time, pulling input only when the phase says to. This is the loop a
    // host-rate render has to run, and the shape the filter's `advance` is written for.
    const auto convert = [](int host_rate, std::span<const float> input, std::size_t frames) {
        OutputFilter filter;
        filter.set_host_rate(host_rate);
        filter.reset();

        std::vector<float> output;
        output.reserve(frames);
        std::size_t taken = 0;

        // Primed with one frame, so `at` has something either side of the phase from the start.
        filter.push(input[taken], input[taken]);
        ++taken;

        for (std::size_t i = 0; i < frames; ++i) {
            output.push_back(filter.at().first);
            for (int wanted = filter.advance(); wanted > 0; --wanted) {
                filter.push(taken < input.size() ? input[taken] : 0.0F,
                            taken < input.size() ? input[taken] : 0.0F);
                ++taken;
            }
        }
        return std::pair{output, taken};
    };

    SECTION("it consumes input at the ratio, whatever the host asks for")
    {
        std::vector<float> ramp(4096);
        for (std::size_t i = 0; i < ramp.size(); ++i) {
            ramp[i] = static_cast<float>(i) / static_cast<float>(ramp.size());
        }

        // A host rate above the engine's takes less than one input frame per output frame, and the
        // count has to follow the ratio rather than the block size.
        for (const int rate : {32000, 44100, 48000, 88200, 96000}) {
            const std::size_t frames = 1000;
            const auto [output, taken] = convert(rate, ramp, frames);

            const double ratio = static_cast<double>(OutputFilter::engine_rate) / rate;
            const auto expected = static_cast<double>(frames) * ratio;

            INFO(rate << " Hz");
            CHECK(output.size() == frames);
            // Within a frame of the ideal: the phase carries the remainder, and priming took one.
            CHECK(std::abs(static_cast<double>(taken) - expected) <= 2.0);
        }
    }

    SECTION("a ramp comes out a ramp, with no step at any block boundary")
    {
        // The point of the exercise. A steady input must produce a steady output; a dropped or
        // repeated input frame shows up as a kink, which under a held note is heard as a click.
        std::vector<float> ramp(4096);
        for (std::size_t i = 0; i < ramp.size(); ++i) {
            ramp[i] = static_cast<float>(i) / static_cast<float>(ramp.size());
        }

        const auto [output, taken] = convert(44100, ramp, 2000);
        static_cast<void>(taken);

        // The rise per output frame is ratio / 4096; every step must match it.
        //
        // Measured past the first few hundred frames, because the allpass starts from a cleared
        // state and settles into the ramp exponentially -- that opening bend is the filter working,
        // not a fault, and it is over long before anything musical happens.
        const double step = (1.0 / 4096.0) * (32000.0 / 44100.0);
        for (std::size_t i = 400; i + 1 < output.size(); ++i) {
            INFO("frame " << i);
            CHECK_THAT(static_cast<double>(output[i] - output[i - 1]), WithinAbs(step, 1e-7));
        }
    }
}
