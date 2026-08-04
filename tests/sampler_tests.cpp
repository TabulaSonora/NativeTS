#include "tabulasonora/sampler.hpp"

#include "dsp/wave_codec.hpp"
#include "test_data.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <nlohmann/json.hpp>

#include <array>
#include <cmath>
#include <fstream>
#include <utility>
#include <vector>

using namespace ts;
using Catch::Matchers::WithinAbs;
namespace fs = std::filesystem;

namespace {

/// Everything a sampler test needs, built once per test case.
///
/// Declaration order is load-bearing: members are initialised in the order they are declared, and
/// `sampler` holds references to `wave_rom` and `interpolator`, which therefore have to come first.
class Fixture {
public:
    static Fixture make()
    {
        RomImage rom = RomImage::open(testdata::require_sccore().string(), RomVerification::quick);
        TableSet tables = TableSet::from_rom(rom);
        return Fixture{std::move(rom), std::move(tables)};
    }

    [[nodiscard]] const RomImage& rom() const noexcept { return rom_; }

    [[nodiscard]] const TableSet& tables() const noexcept { return tables_; }

    [[nodiscard]] const WaveRom& wave_rom() const noexcept { return wave_rom_; }

    [[nodiscard]] Sampler& sampler() noexcept { return sampler_; }

private:
    Fixture(RomImage&& rom, TableSet&& tables)
        : rom_(std::move(rom)),
          tables_(std::move(tables)),
          wave_rom_(rom_),
          interpolator_(tables_),
          sampler_(wave_rom_, interpolator_)
    {
    }

    RomImage rom_;
    TableSet tables_;
    WaveRom wave_rom_;
    Interpolator interpolator_;
    Sampler sampler_;
};

/// Reads a descriptor out of the wave-descriptor table.
[[nodiscard]] WaveDescriptor descriptor_at(const TableSet& tables, int wave)
{
    const auto table = tables.wavedesc();
    const auto offset = static_cast<std::size_t>(wave) * WaveDescriptor::stride;
    REQUIRE(offset + WaveDescriptor::stride <= table.size());
    return WaveDescriptor::parse(table.subspan(offset, WaveDescriptor::stride));
}

} // namespace

TEST_CASE("a descriptor unpacks its 20-bit position fields", "[sampler]")
{
    // Needs no DLL. The three positions are 20-bit, most-significant nibble first, and the flag
    // bits decide the sampler variant: bit 0 bidirectional, bit 2 reverse, bit 1 unused.
    std::array<std::uint8_t, WaveDescriptor::stride> record{};
    record[0x00] = 0x93; // region, high bit masked off
    record[0x01] = 0x0A;
    record[0x02] = 0xBC;
    record[0x03] = 0xDE; // loop  = 0xABCDE
    record[0x04] = 0x00;
    record[0x05] = 0x04; // fine tune = 1024
    record[0x06] = 60;   // root key
    record[0x07] = 0x01;
    record[0x08] = 0x23;
    record[0x09] = 0x45; // end   = 0x12345
    record[0x0A] = 0x05; // flags: reverse + ping-pong
    record[0x0B] = 0x0F;
    record[0x0C] = 0xED;
    record[0x0D] = 0xCB; // start = 0xFEDCB
    record[0x0E] = 0x00;
    record[0x0F] = 0x04; // second fine tune = 1024, the neutral 80% of the ROM carries

    const WaveDescriptor descriptor = WaveDescriptor::parse(record);

    CHECK(descriptor.region == 0x13);
    CHECK(descriptor.bank() == 1);
    CHECK(descriptor.loop == 0xABCDE);
    CHECK(descriptor.end == 0x12345);
    CHECK(descriptor.start == 0xFEDCB);
    CHECK(descriptor.root_key == 60);
    CHECK(descriptor.fine_tune == 1024);
    CHECK(descriptor.reverse());
    CHECK(descriptor.ping_pong());

    CHECK(descriptor.second_fine_tune == 1024);

    // Fine tune 1024 is exactly the root key; the offset is (1024 - fineTune) / 1000.
    CHECK_THAT(descriptor.native_pitch(), WithinAbs(60.0, 1e-12));

    // The second fine tune tunes off that result, not off the root. The module computes it into
    // `voice+0x200` and `voices_control_update` copies that over `voice+0x1fc` on every control
    // tick of a sounding voice, so it is what the exponent ends up taken against.
    record[0x0E] = 0xC0;
    record[0x0F] = 0x02; // 704, the value the alto sax's top zone carries
    const WaveDescriptor detuned = WaveDescriptor::parse(record);
    CHECK(detuned.second_fine_tune == 704);
    CHECK_THAT(detuned.native_milli_semitones(), WithinAbs(60320.0, 1e-9));
    CHECK_THAT(detuned.native_pitch(), WithinAbs(60.320, 1e-12));
}

TEST_CASE("bit 1 of the flag byte takes no part in the dispatch", "[sampler]")
{
    // The old rule "flags & 2 means one-shot" was right by accident: all 649 such waves have an
    // empty loop region, which is what actually makes them one-shots.
    std::array<std::uint8_t, WaveDescriptor::stride> record{};
    record[0x0A] = 0x02;

    const WaveDescriptor descriptor = WaveDescriptor::parse(record);
    CHECK_FALSE(descriptor.reverse());
    CHECK_FALSE(descriptor.ping_pong());
}

TEST_CASE("the predictor stream matches an independent decoder", "[sampler][sccore][gate]")
{
    // The Phase 2 gate. The fixture is produced by tools/dump_predictors.py, which implements the
    // documented formula directly in Python rather than translating this code -- so agreement is
    // evidence, not a tautology. It covers every distinct wave in the descriptor table.
    const fs::path fixture_path = testdata::repository_root() / "fixtures" / "predictors.json";
    if (!fs::exists(fixture_path)) {
        SKIP("No predictor fixture. Generate it with:\n"
             "  python3 tools/dump_predictors.py <SCCore.dll> fixtures/predictors.json");
    }

    Fixture fixture = Fixture::make();

    std::ifstream stream{fixture_path};
    REQUIRE(stream);
    const nlohmann::json document = nlohmann::json::parse(stream);

    REQUIRE(document.at("dllSha256").get<std::string>() == fixture.rom().manifest().dll().sha256);

    const auto& cases = document.at("cases");
    REQUIRE(cases.size() > 100);

    std::size_t checked = 0;
    std::int64_t samples = 0;

    for (const auto& entry : cases) {
        const WaveDescriptor descriptor{
            .region = entry.at("region").get<int>(),
            .loop = entry.at("loop").get<int>(),
            .end = 0,
            .start = entry.at("start").get<int>(),
            .root_key = 60,
            .fine_tune = 1024,
            .flags = 0,
        };

        INFO("wave " << entry.at("wave").get<int>() << " region " << descriptor.region << " loop "
                     << descriptor.loop << " start " << descriptor.start);

        const auto streams =
            fixture.wave_rom().read_streams(descriptor.region, descriptor.loop, descriptor.start);
        REQUIRE(streams.has_value());
        REQUIRE(streams->sample_count == entry.at("sampleCount").get<int>());

        std::vector<std::int32_t> predictors(static_cast<std::size_t>(streams->sample_count) + 1);
        codec::decode_predictors(streams->delta, streams->scale, predictors,
                                 streams->scale_phase);

        // A hash of the whole stream is the real assertion; the literal prefix and suffix are what
        // make a failure diagnosable rather than just red.
        CHECK(testdata::sha256_of_le32(predictors)
              == entry.at("predictorSha256").get<std::string>());

        const auto& first = entry.at("first16");
        for (std::size_t i = 0; i < first.size(); ++i) {
            REQUIRE(predictors[i] == first[i].get<std::int32_t>());
        }
        const auto& last = entry.at("last16");
        for (std::size_t i = 0; i < last.size(); ++i) {
            REQUIRE(predictors[predictors.size() - last.size() + i] == last[i].get<std::int32_t>());
        }

        ++checked;
        samples += streams->sample_count;
    }

    // Guard against passing vacuously: an empty fixture would agree with anything.
    CHECK(checked == cases.size());
    CHECK(samples > 1'000'000);
}

TEST_CASE("an unaligned wave rides its preamble sum as a constant", "[sampler][sccore]")
{
    // The engine's decoder zeroes its predictor at the 32-sample exponent-block boundary below
    // the data start, not at the data start, and integrates the preamble deltas in on the way.
    // The sum rides under every sample of the wave -- the "module has DC" of the verification
    // article, and the whole audible body of transcendental.mid's pitch-bent sine kick.
    //
    // `Crash Cym.1`'s wave is the case measured live: region 4, data start 370059 (eleven
    // preamble deltas), and subtracting a start-at-zero decode from the module's own predictor
    // trace leaves exactly -0.041015625 at correlation 1.0. The constant asserted here is the
    // module's, not this engine's own output re-pinned.
    Fixture fixture = Fixture::make();

    const WaveDescriptor crash{
        .region = 4, .loop = 370059, .end = 385475, .start = 399566,
        .root_key = 60, .fine_tune = 1024, .flags = 0,
    };
    const DecodedWave* wave = fixture.sampler().decode(crash);
    REQUIRE(wave != nullptr);
    CHECK(wave->seed == -5505024);
    CHECK_THAT(static_cast<double>(wave->seed) * codec::output_scale,
               WithinAbs(-0.041015625, 1e-12));

    // The seed is the preamble's integral and nothing else: re-integrating the wave's own steps
    // from it reproduces the decoded samples exactly.
    std::int32_t predictor = wave->seed;
    for (std::size_t i = 0; i < 64; ++i) {
        predictor = fx::wadd(predictor, wave->steps[i]);
        REQUIRE(wave->samples[i]
                == static_cast<float>(static_cast<double>(predictor) * codec::output_scale));
    }

    // An aligned wave has no preamble and no displacement.
    const auto aligned = fixture.wave_rom().read_streams(0, 0x1000, 0x2000);
    REQUIRE(aligned.has_value());
    CHECK(aligned->preamble_delta.empty());
}

TEST_CASE("looping is decided by a sustain region, not the loop flag", "[sampler][sccore]")
{
    // The descriptor's loop flag reads zero for piano, so trusting it makes held notes run out as
    // one-shots. What decides is whether end lands before the data end.
    Fixture fixture = Fixture::make();

    std::size_t looping = 0;
    std::size_t one_shot = 0;
    std::size_t ping_pong = 0;

    for (int wave = 0; wave < 512; ++wave) {
        const WaveDescriptor descriptor = descriptor_at(fixture.tables(), wave);
        const DecodedWave* decoded = fixture.sampler().decode(descriptor);
        if (decoded == nullptr) {
            continue;
        }

        switch (decoded->mode) {
        case SamplerMode::loop:
            ++looping;
            break;
        case SamplerMode::one_shot:
            ++one_shot;
            break;
        case SamplerMode::ping_pong:
            ++ping_pong;
            break;
        }
    }

    // All three variants must actually occur, or the classification is not being exercised.
    CHECK(looping > 0);
    CHECK(one_shot > 0);
    CHECK(ping_pong > 0);
}

TEST_CASE("the loop period is inclusive of the data end", "[sampler][sccore]")
{
    // Off by one here is inaudible on a long loop and detunes a single-cycle one by 27 cents.
    Fixture fixture = Fixture::make();

    for (int wave = 0; wave < 256; ++wave) {
        const DecodedWave* decoded =
            fixture.sampler().decode(descriptor_at(fixture.tables(), wave));
        if (decoded == nullptr || !decoded->is_looping()) {
            continue;
        }

        INFO("wave " << wave);
        REQUIRE(decoded->loop_period() == decoded->data_end - decoded->loop_start + 1);

        // The sample at the data end genuinely exists -- one past it is decoded so the loop can
        // close on it. Stopping one short substitutes the loop's first sample instead.
        REQUIRE(decoded->samples.size() == static_cast<std::size_t>(decoded->data_end) + 1);
        return;
    }
    FAIL("no looping wave found in the first 256 descriptors");
}

TEST_CASE("the decode cache returns the same wave for the same descriptor", "[sampler][sccore]")
{
    // Every voice playing a wave shares one copy, and the pointer has to stay valid as the cache
    // grows -- a rehash that moved a wave would dangle in a sounding voice.
    Fixture fixture = Fixture::make();

    const WaveDescriptor first = descriptor_at(fixture.tables(), 1);
    const DecodedWave* a = fixture.sampler().decode(first);

    for (int wave = 2; wave < 200; ++wave) {
        // Warm the cache with other waves so it rehashes underneath the pointer above.
        const DecodedWave* ignored =
            fixture.sampler().decode(descriptor_at(fixture.tables(), wave));
        static_cast<void>(ignored);
    }

    const DecodedWave* b = fixture.sampler().decode(first);
    CHECK(a == b);
}

TEST_CASE("a one-shot goes silent rather than holding its last sample", "[sampler][sccore]")
{
    Fixture fixture = Fixture::make();

    for (int wave = 0; wave < 512; ++wave) {
        const DecodedWave* decoded =
            fixture.sampler().decode(descriptor_at(fixture.tables(), wave));
        if (decoded == nullptr || decoded->mode != SamplerMode::one_shot
            || decoded->data_end < 64) {
            continue;
        }

        // Play well past the end; the tail must be true silence, not a held DC value. The codec's
        // predictor is a pure integrator, so a held last sample would be an audible DC step.
        const std::vector<float> output =
            fixture.sampler().play(*decoded, decoded->data_end + 64, 1.0);
        CHECK(output.back() == 0.0F);
        return;
    }
    SKIP("no suitable one-shot wave found");
}

TEST_CASE("a forward loop reads the samples the loop wraps to", "[sampler][sccore]")
{
    // The 4-tap window reaches two samples past the read index, so the loop buffer must carry the
    // wrapped samples rather than whatever follows the wave in the ROM.
    Fixture fixture = Fixture::make();

    for (int wave = 0; wave < 512; ++wave) {
        const DecodedWave* decoded =
            fixture.sampler().decode(descriptor_at(fixture.tables(), wave));
        if (decoded == nullptr || !decoded->is_looping()) {
            continue;
        }

        INFO("wave " << wave);
        REQUIRE(decoded->loop_buffer.size() >= static_cast<std::size_t>(decoded->data_end) + 4);

        // Playing several loop periods must not run out of buffer or produce silence.
        const int span = decoded->data_end + (decoded->loop_period() * 3);
        const std::vector<float> output = fixture.sampler().play(*decoded, span, 1.0);

        bool any_nonzero = false;
        for (float value : output) {
            REQUIRE(std::isfinite(value));
            any_nonzero = any_nonzero || value != 0.0F;
        }
        CHECK(any_nonzero);
        return;
    }
    FAIL("no looping wave found");
}

TEST_CASE("a reverse wave is turned round and plays as a one-shot", "[sampler][sccore]")
{
    Fixture fixture = Fixture::make();

    for (int wave = 0; wave < 4096; ++wave) {
        const WaveDescriptor descriptor = descriptor_at(fixture.tables(), wave);
        if (!descriptor.reverse()) {
            continue;
        }
        const DecodedWave* decoded = fixture.sampler().decode(descriptor);
        if (decoded == nullptr) {
            continue;
        }

        INFO("wave " << wave);
        CHECK(decoded->reversed);
        CHECK(decoded->mode == SamplerMode::one_shot);
        return;
    }
    SKIP("no reverse wave found in the descriptor table");
}
