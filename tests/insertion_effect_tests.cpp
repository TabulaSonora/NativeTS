#include "tabulasonora/insertion_effect.hpp"

#include "tabulasonora/note_renderer.hpp"
#include "tabulasonora/rom_image.hpp"
#include "tabulasonora/tone_generator.hpp"
#include "test_data.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <numbers>
#include <vector>

using namespace ts;

namespace {

[[nodiscard]] RomImage open_rom()
{
    return RomImage::open(testdata::require_sccore().string(), RomVerification::quick);
}

[[nodiscard]] const EfxRecord* find(const std::vector<EfxRecord>& directory, const char* name)
{
    for (const EfxRecord& record : directory) {
        if (record.name == name) {
            return &record;
        }
    }
    return nullptr;
}

/// A block of sine, loud enough to exercise a clipper.
[[nodiscard]] std::vector<float> sine(std::size_t samples, double amplitude)
{
    std::vector<float> wave(samples);
    for (std::size_t n = 0; n < samples; ++n) {
        wave[n] = static_cast<float>(
            amplitude * std::sin(2.0 * std::numbers::pi * 440.0 * static_cast<double>(n) / 32000.0));
    }
    return wave;
}

[[nodiscard]] double rms(std::span<const float> wave)
{
    double energy = 0.0;
    for (const float sample : wave) {
        energy += static_cast<double>(sample) * static_cast<double>(sample);
    }
    return std::sqrt(energy / static_cast<double>(wave.size()));
}

/// Builds a GS DT1 with the checksum the engine verifies.
[[nodiscard]] std::vector<std::uint8_t> dt1(std::initializer_list<int> address_and_data)
{
    std::vector<std::uint8_t> message{0xF0, 0x41, 0x10, 0x42, 0x12};
    int sum = 0;
    for (int byte : address_and_data) {
        message.push_back(static_cast<std::uint8_t>(byte));
        sum += byte;
    }
    message.push_back(static_cast<std::uint8_t>((0x80 - (sum & 0x7F)) & 0x7F));
    message.push_back(0xF7);
    return message;
}

} // namespace

TEST_CASE("the EFX directory is the DLL's own", "[efx][sccore]")
{
    const RomImage rom = open_rom();
    InsertionEffect efx{rom};
    const std::vector<EfxRecord>& directory = efx.directory();

    REQUIRE(directory.size() == 66);

    // The engine names its own effects; the first record is Thru and the second the blank
    // "no effect assigned" state.
    CHECK(directory[0].name == "Thru");
    CHECK(directory[0].type_key == 0x0000);
    CHECK(directory[0].dispatch == 0);
    CHECK(directory[1].name.empty());
    CHECK(directory[1].type_key == 0xFFFF);

    // The dispatch mapping is a scramble of the type order and must match the record, not the
    // position: these are the examples FINDINGS pins.
    const EfxRecord* spectrum = find(directory, "Spectrum");
    REQUIRE(spectrum != nullptr);
    CHECK(spectrum->type_key == 0x0101);
    CHECK(spectrum->dispatch == 6);
    const EfxRecord* humanizer = find(directory, "Humanizer");
    REQUIRE(humanizer != nullptr);
    CHECK(humanizer->dispatch == 46);

    // Overdrive's defaults, straight from its `param_defaults` block: drive 48, amp simulator on,
    // amp type 1, level 0x60, and the block's reverb send at the GS default 40.
    const EfxRecord* overdrive = find(directory, "Overdrive");
    REQUIRE(overdrive != nullptr);
    CHECK(overdrive->type_key == 0x0110);
    CHECK(overdrive->dispatch == 3);
    CHECK(overdrive->defaults[0] == 0x30);
    CHECK(overdrive->defaults[1] == 0x01);
    CHECK(overdrive->defaults[2] == 0x01);
    CHECK(overdrive->defaults[0x13] == 0x60);
    CHECK(overdrive->defaults[0x14] == 0x28);

    // Distortion shares Overdrive's apply handler but not its preset.
    const EfxRecord* distortion = find(directory, "Distortion");
    REQUIRE(distortion != nullptr);
    CHECK(distortion->dispatch == 4);
    CHECK(distortion->defaults[0] == 0x4C);
}

TEST_CASE("the block powers on at Thru and passes signal", "[efx][sccore]")
{
    const RomImage rom = open_rom();
    InsertionEffect efx{rom};

    CHECK(efx.current().name == "Thru");
    CHECK(efx.implemented());
    // The GS default sends: reverb 40, the others silent.
    CHECK(efx.reverb_send() > 0.0);
    CHECK(efx.chorus_send() == 0.0);
    CHECK(efx.delay_send() == 0.0);

    const std::vector<float> input = sine(4096, 0.25);
    std::vector<float> out_left(input.size());
    std::vector<float> out_right(input.size());
    efx.process(input, input, out_left, out_right);

    // Thru is a delayed, gain-scaled pass — the exact gain is the preset's business, but silence
    // or an explosion would both mean the register machine mis-decoded it.
    const double gain = rms(std::span<const float>{out_left}.subspan(1024))
                        / rms(std::span<const float>{input}.subspan(1024));
    CHECK(gain > 0.25);
    CHECK(gain < 4.0);

    // Silence in, silence out — the anti-denormal seeds must not accumulate into signal.
    std::vector<float> quiet(4096, 0.0F);
    efx.process(quiet, quiet, out_left, out_right);
    CHECK(rms(std::span<const float>{out_left}.subspan(1024)) < 1e-4);
}

TEST_CASE("overdrive shapes and responds to drive", "[efx][sccore]")
{
    const RomImage rom = open_rom();
    InsertionEffect efx{rom};
    efx.select_type(0x01, 0x10);
    CHECK(efx.current().name == "Overdrive");
    CHECK(efx.implemented());

    const std::vector<float> input = sine(8192, 0.25);
    std::vector<float> left_low(input.size());
    std::vector<float> right_low(input.size());
    efx.process(input, input, left_low, right_low);
    const double low_drive = rms(std::span<const float>{left_low}.subspan(4096));
    CHECK(low_drive > 0.0);

    // Full drive against drive zero: the curve must reach the registers and move the output.
    efx.select_type(0x01, 0x10);
    efx.set_parameter(0x03, 0x7F);
    std::vector<float> left_high(input.size());
    std::vector<float> right_high(input.size());
    efx.process(input, input, left_high, right_high);

    efx.select_type(0x01, 0x10);
    efx.set_parameter(0x03, 0x00);
    std::vector<float> left_zero(input.size());
    std::vector<float> right_zero(input.size());
    efx.process(input, input, left_zero, right_zero);

    const double high_drive = rms(std::span<const float>{left_high}.subspan(4096));
    const double zero_drive = rms(std::span<const float>{left_zero}.subspan(4096));
    CHECK(high_drive > 0.0);
    CHECK(high_drive != zero_drive);
}

TEST_CASE("an untranscribed type passes through and says so", "[efx][sccore]")
{
    const RomImage rom = open_rom();
    InsertionEffect efx{rom};
    efx.select_type(0x01, 0x20); // Phaser, dispatch 5 — outside the tranche.
    CHECK(efx.current().name == "Phaser");
    CHECK_FALSE(efx.implemented());

    const std::vector<float> input = sine(1024, 0.25);
    std::vector<float> out_left(input.size());
    std::vector<float> out_right(input.size());
    efx.process(input, input, out_left, out_right);
    CHECK(std::equal(input.begin(), input.end(), out_left.begin()));

    // An unknown type key falls back to Thru, as the engine's map scan does.
    efx.select_type(0x00, 0x40);
    CHECK(efx.implemented());
}

TEST_CASE("EFX parts detour through the block with their sends nulled", "[efx][stream][sccore]")
{
    const RomImage rom = open_rom();
    NoteRenderer notes{rom};
    ToneGenerator generator{notes};

    // Overdrive on, channel 1 routed through it. Block 1 addresses channel 0.
    generator.send_sysex(dt1({0x40, 0x03, 0x00, 0x01, 0x10}));
    generator.send_sysex(dt1({0x40, 0x41, 0x22, 0x01}));

    generator.send_channel(0xC0, 27, 0); // clean guitar
    generator.send_channel(0x90, 64, 100);

    std::array<float, 32000> left{};
    std::array<float, 32000> right{};
    generator.render(left, right);

    double energy = 0.0;
    for (std::size_t i = 0; i < left.size(); ++i) {
        energy += static_cast<double>(left[i]) * static_cast<double>(left[i])
                  + static_cast<double>(right[i]) * static_cast<double>(right[i]);
    }
    CHECK(energy > 0.0);
}
