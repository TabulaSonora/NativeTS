#include "tabulasonora/insertion_effect.hpp"

#include "tabulasonora/note_renderer.hpp"
#include "tabulasonora/rom_image.hpp"
#include "tabulasonora/send_effects.hpp"
#include "tabulasonora/tone_generator.hpp"
#include "test_data.hpp"

#include <catch2/catch_approx.hpp>
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

TEST_CASE("the EFX reverb builds a tail and its tap program", "[efx][sccore]")
{
    const RomImage rom = open_rom();
    InsertionEffect efx{rom};
    efx.select_type(0x01, 0x55);
    CHECK(efx.current().name == "Reverb");
    CHECK(efx.implemented());

    // The character program's taps: slot 0 is the pre-delay (curve + 0x6000), slot 1 the fixed
    // second read, and 2 onward the 32-tap network. All must be inside buffer B.
    const auto taps = efx.tap_program();
    REQUIRE(taps.size() == 34);
    CHECK(taps[0] > 0x6000);
    for (const std::int32_t tap : taps) {
        CHECK(tap >= 0);
        CHECK(tap < (1 << 17));
    }

    // A short burst, then silence: the tail has to outlive the input and decay rather than run
    // away — a wrong feedback tap shows up as one or the other.
    std::vector<float> burst = sine(2048, 0.25);
    std::vector<float> quiet(16384, 0.0F);
    std::vector<float> out_left(burst.size());
    std::vector<float> out_right(burst.size());
    efx.process(burst, burst, out_left, out_right);
    std::vector<float> tail_left(quiet.size());
    std::vector<float> tail_right(quiet.size());
    efx.process(quiet, quiet, tail_left, tail_right);

    const double early = rms(std::span<const float>{tail_left}.subspan(0, 4096));
    const double late = rms(std::span<const float>{tail_left}.subspan(12288));
    CHECK(early > 1e-5);
    CHECK(late < early);
    CHECK(late < 1.0);
}

TEST_CASE("the block's sends carry the engine's own ratios", "[efx][sccore]")
{
    const RomImage rom = open_rom();
    InsertionEffect efx{rom};
    efx.select_type(0x00, 0x00);

    // Linear in the byte, and zero at zero — the ramp targets are `byte << 7`.
    efx.set_parameter(0x17, 0);
    CHECK(efx.reverb_send() == 0.0);
    efx.set_parameter(0x17, 64);
    const double half = efx.reverb_send();
    efx.set_parameter(0x17, 127);
    const double full = efx.reverb_send();
    CHECK(half > 0.0);
    CHECK(full > half);
    CHECK(full / half == Catch::Approx(127.0 / 64.0).epsilon(0.001));

    // Measured against the live engine: with a level-transparent Thru in the path, an EFX send
    // byte puts 0.98 of a part send's wet on the reverb bus. `Reverb::send_gain` is the part
    // side, and the block's tap is the stereo sum rather than the pre-pan mono the part path
    // uses, so the comparison carries the pan-table centre sum.
    constexpr double centre_pan_sum = 2.0 * 75.0 / 127.0;
    const double against_part = full * centre_pan_sum / Reverb::send_gain(127);
    CHECK(against_part == Catch::Approx(0.98).epsilon(0.02));

    // The three sends are independent: writing one leaves the others alone.
    efx.set_parameter(0x18, 0);
    efx.set_parameter(0x19, 0);
    CHECK(efx.chorus_send() == 0.0);
    CHECK(efx.delay_send() == 0.0);
    CHECK(efx.reverb_send() == full);
}

TEST_CASE("OD / OD2 runs both chains", "[efx][sccore]")
{
    const RomImage rom = open_rom();
    InsertionEffect efx{rom};
    efx.select_type(0x11, 0x03);
    CHECK(efx.current().name == "OD / OD2");
    CHECK(efx.implemented());

    const std::vector<float> input = sine(8192, 0.25);
    std::vector<float> left(input.size());
    std::vector<float> right(input.size());
    efx.process(input, input, left, right);
    const double base = rms(std::span<const float>{left}.subspan(4096));
    CHECK(base > 0.0);

    // The two chains have separate drive controls -- 0x04 for the first, 0x09 for the second --
    // and each has to reach the output on its own. A transcription that wires one chain twice, or
    // drops one, passes every other check in this file and fails here.
    const auto with = [&](int address, int value) {
        efx.select_type(0x11, 0x03);
        efx.set_parameter(address, value);
        std::vector<float> l(input.size());
        std::vector<float> r(input.size());
        efx.process(input, input, l, r);
        return rms(std::span<const float>{l}.subspan(4096));
    };
    const double chain_one = with(0x04, 0x7F);
    const double chain_two = with(0x09, 0x7F);
    CHECK(chain_one > 0.0);
    CHECK(chain_two > 0.0);
    CHECK(chain_one != base);
    CHECK(chain_two != base);
    CHECK(chain_one != chain_two);

    // Each chain also has its own level, and turning both down has to silence the block where
    // turning one down does not.
    efx.select_type(0x11, 0x03);
    efx.set_parameter(0x13, 0x00);
    efx.set_parameter(0x15, 0x00);
    std::vector<float> quiet_left(input.size());
    std::vector<float> quiet_right(input.size());
    efx.process(input, input, quiet_left, quiet_right);
    CHECK(rms(std::span<const float>{quiet_left}.subspan(4096)) < base);
}

TEST_CASE("Rotary modulates rather than merely passing", "[efx][sccore]")
{
    const RomImage rom = open_rom();
    InsertionEffect efx{rom};
    efx.select_type(0x01, 0x22);
    CHECK(efx.current().name == "Rotary");
    CHECK(efx.implemented());

    // Four seconds of steady tone. A rotor is an amplitude and delay sweep, so the test is that
    // the output is *not* steady -- a transcription that dropped the rotating tap would pass a
    // level check and fail this.
    const std::vector<float> input = sine(32000 * 4, 0.25);
    std::vector<float> left(input.size());
    std::vector<float> right(input.size());
    efx.process(input, input, left, right);

    const auto window = [&](std::size_t from) {
        return rms(std::span<const float>{left}.subspan(from, 3200));
    };
    double lowest = 1.0;
    double highest = 0.0;
    for (std::size_t at = 16000; at + 3200 < left.size(); at += 3200) {
        lowest = std::min(lowest, window(at));
        highest = std::max(highest, window(at));
    }
    CHECK(highest > 0.0);
    // The sweep has to be visible in the envelope but must not be gating the signal off.
    CHECK(highest / lowest > 1.02);
    CHECK(lowest / highest > 0.2);

    // Both rotors' speed switch is one parameter (0x0D), and moving it has to change the result.
    efx.select_type(0x01, 0x22);
    efx.set_parameter(0x0D, 0x7F);
    std::vector<float> fast_left(input.size());
    std::vector<float> fast_right(input.size());
    efx.process(input, input, fast_left, fast_right);
    CHECK(rms(std::span<const float>{fast_left}.subspan(16000))
          != rms(std::span<const float>{left}.subspan(16000)));
}

TEST_CASE("the stereo EQ shelves respond and its mid bands go through the bank loader",
          "[efx][sccore]")
{
    const RomImage rom = open_rom();
    InsertionEffect efx{rom};
    efx.select_type(0x01, 0x00);
    CHECK(efx.current().name == "Equalizer");
    CHECK(efx.implemented());

    const std::vector<float> input = sine(8192, 0.25);
    const auto render = [&](int address, int value) {
        efx.select_type(0x01, 0x00);
        if (address >= 0) {
            efx.set_parameter(address, value);
        }
        std::vector<float> l(input.size());
        std::vector<float> r(input.size());
        efx.process(input, input, l, r);
        return rms(std::span<const float>{l}.subspan(4096));
    };

    const double flat = render(-1, 0);
    CHECK(flat > 0.0);

    // Both shelf gains have to reach the filter. `04` is the low band's gain and `06` the high
    // band's. The two extremes are compared against each other rather than against flat: at this
    // type's default frequency settings the tables saturate, so one end of a band's range can
    // legitimately land on the same coefficients as the default and comparing to flat would
    // assert a table property rather than the wiring.
    CHECK(render(0x04, 0x7F) > render(0x04, 0x00));
    CHECK(render(0x06, 0x7F) > render(0x06, 0x00));
    CHECK(render(0x04, 0x7F) > flat);

    // The two mid bands go through the four-argument bank loader, which programs each band twice,
    // once per channel. Their gain byte is a window rather than a scale -- everything below 0x34
    // is the bottom of the table -- so the pair that has to differ is one inside the window
    // against one below it.
    CHECK(render(0x09, 0x60) != render(0x09, 0x00));
    CHECK(render(0x0C, 0x60) != render(0x0C, 0x00));
}

TEST_CASE("the Enhancer brightens rather than passing through", "[efx][sccore]")
{
    const RomImage rom = open_rom();
    InsertionEffect efx{rom};
    efx.select_type(0x01, 0x02);
    CHECK(efx.current().name == "Enhancer");
    CHECK(efx.implemented());

    // Verified against the module two ways that this test cannot repeat here: its coefficient file
    // and tap program match `scdec efxdump 01 02` word for word, all 384 and 34 of them, and its
    // impulse response is **bit-identical** to `scdec efxir 01 02` over 512 frames on both
    // channels. What is asserted here is that the transcription is wired in and does the thing the
    // name promises, which is what a regression would break first.
    const std::vector<float> input = sine(8192, 0.25);
    std::vector<float> left(input.size());
    std::vector<float> right(input.size());
    efx.process(input, input, left, right);

    // Not a pass-through: an Enhancer that did nothing would return its input unchanged.
    CHECK_FALSE(std::equal(input.begin(), input.end(), left.begin()));
    CHECK(rms(std::span<const float>{left}.subspan(4096)) > 0.0);

    // An impulse leaves a tail, which a bare gain would not.
    InsertionEffect tail{rom};
    tail.select_type(0x01, 0x02);
    std::vector<float> impulse(512, 0.0F);
    impulse[0] = 1.0F;
    std::vector<float> tl(impulse.size());
    std::vector<float> tr(impulse.size());
    tail.process(impulse, impulse, tl, tr);
    CHECK(rms(std::span<const float>{tl}.subspan(64)) > 0.0);
}

TEST_CASE("the Hexa Chorus reads one delay line six times", "[efx][sccore]")
{
    const RomImage rom = open_rom();
    InsertionEffect efx{rom};
    efx.select_type(0x01, 0x40);
    CHECK(efx.current().name == "Hexa Chorus");
    CHECK(efx.implemented());

    // Verified against the module the same two ways the Enhancer was: the whole coefficient file
    // and tap program match `scdec efxdump 01 40`, and the impulse response is **bit-identical**
    // to `scdec efxir 01 40` over 1024 frames on both channels.
    //
    // The level is the part that needed the register diff to find. The preset fill leaves 0x80 and
    // 0x1F0 to the type's own handler, and this type's level byte is `0x70`, which the shared curve
    // turns into 107 rather than the 127 an untranscribed type falls back to.
    // Address `0x16`, not `0x13`: the twenty parameters fold to indices 0 upward, so the level
    // byte the handler reads as `params[0x13]` is the last of them.
    const auto level_at = [&](int byte) {
        InsertionEffect one{rom};
        one.select_type(0x01, 0x40);
        one.set_parameter(0x16, byte);
        return one.coefficients()[0];
    };
    CHECK(level_at(0x7F) > level_at(0x70));
    CHECK(level_at(0x70) > level_at(0x40));

    // A modulated delay, not a gain: a lone impulse has to come back later and keep moving, so the
    // tail is neither silent nor a single echo.
    std::vector<float> impulse(4096, 0.0F);
    impulse[0] = 1.0F;
    std::vector<float> left(impulse.size());
    std::vector<float> right(impulse.size());
    efx.process(impulse, impulse, left, right);
    CHECK(rms(std::span<const float>{left}.subspan(512)) > 0.0);

    // Six voices at six phase offsets do not land on the two channels alike; a mono effect would.
    double difference = 0.0;
    for (std::size_t n = 512; n < left.size(); ++n) {
        difference += std::abs(static_cast<double>(left[n]) - static_cast<double>(right[n]));
    }
    CHECK(difference > 0.0);
}

TEST_CASE("Space D reads both halves of the line", "[efx][sccore]")
{
    const RomImage rom = open_rom();
    InsertionEffect efx{rom};
    efx.select_type(0x01, 0x43);
    CHECK(efx.current().name == "Space D");
    CHECK(efx.implemented());

    // Verified against the module as the other two were: the coefficient file and tap program match
    // `scdec efxdump 01 43`, and the impulse response is **bit-identical** to `scdec efxir 01 43`
    // over 1024 frames on both channels.
    //
    // Getting there needed `phase_fold` rather than `phase_wrap`. This algorithm has the fold
    // inlined, so its constants already carry the 1e-08 that `dsp_phase_wrap` would have added --
    // 1.48828e-08 against the Hexa Chorus's 4.8828e-09, and so on -- and nudging again put the
    // output about 1e-08 out. Nothing but a sample comparison would have shown it.
    std::vector<float> impulse(4096, 0.0F);
    impulse[0] = 1.0F;
    std::vector<float> left(impulse.size());
    std::vector<float> right(impulse.size());
    efx.process(impulse, impulse, left, right);

    // A modulated line, not a gain: the impulse comes back and keeps moving.
    CHECK(rms(std::span<const float>{left}.subspan(512)) > 0.0);

    // Two voices reading different halves of the buffer do not put the same signal on both
    // channels; the two output chains have their own weights.
    double difference = 0.0;
    for (std::size_t n = 512; n < left.size(); ++n) {
        difference += std::abs(static_cast<double>(left[n]) - static_cast<double>(right[n]));
    }
    CHECK(difference > 0.0);
}

TEST_CASE("every transcribed type's level reaches its registers", "[efx][sccore]")
{
    // **The check the per-type diffs could not make.** A register comparison taken at a type's
    // defaults cannot tell a handler that computes the right answer from one that returns a
    // constant, and the Enhancer sat on the untranscribed fallback's hard-coded `0x7F` for two
    // commits because its own default level is `0x7F` and `level_curve[127]` is 127. It agreed at
    // the default and nowhere else: at level `0x40` the module programs 53/128 where this wrote
    // 127/128.
    //
    // So this sweeps the level rather than reading it once, over every type that claims a
    // processor, which is what makes it catch the next one added without its level write.
    //
    // Swept against the module with `scdec efxdump <type> 16 40`: **Thru holds its level** -- its
    // handler writes the two registers on the select commit only -- and every other transcribed
    // type lands on the same 0.4140625, because they share one curve. Ten of ten agree.
    const RomImage rom = open_rom();
    InsertionEffect probe{rom};

    const auto level_of = [&rom](int msb, int lsb, int byte) {
        InsertionEffect efx{rom};
        efx.select_type(msb, lsb);
        efx.set_parameter(0x16, byte); // the twenty parameters fold to 0 upward; level is the last
        return efx.coefficients()[0];
    };

    int transcribed = 0;
    for (const EfxRecord& record : probe.directory()) {
        if (record.type_key == 0xFFFF) {
            continue;
        }
        InsertionEffect efx{rom};
        efx.select_type(record.type_key >> 8, record.type_key & 0xFF);
        if (!efx.implemented()) {
            continue;
        }
        ++transcribed;

        INFO(record.name << " (" << std::hex << record.type_key << ")");
        const float quiet = level_of(record.type_key >> 8, record.type_key & 0xFF, 0x40);
        const float loud = level_of(record.type_key >> 8, record.type_key & 0xFF, 0x7F);

        if (record.type_key == 0x0000) {
            // Thru, and it is the exception rather than an oversight.
            CHECK(quiet == loud);
            continue;
        }
        CHECK(quiet < loud);
        CHECK(quiet == Catch::Approx(0.4140625).epsilon(1e-6));
    }

    // A guard on the guard: if the tranche grows and this loop stops seeing the types, the
    // assertions above pass by never running.
    CHECK(transcribed >= 10);
}

TEST_CASE("the Enhancer's parameters reach its registers", "[efx][sccore]")
{
    const RomImage rom = open_rom();

    // `fx_param_apply_47340` transcribed. Swept against the module with `scdec efxdump 01 02
    // <addr> <value>` over **every one of the twenty parameter addresses at three values each** --
    // sixty comparisons, 0 of 384 registers differing in each.
    //
    // What is asserted here is that the parameters this type actually spends move something, which
    // is the part a stub would fail. The addresses are the ones the handler reads: `03` sens, `04`
    // mix, `13` the low band, `14` the high band, `16` the level.
    const auto registers_for = [&rom](int address, int value) {
        InsertionEffect efx{rom};
        efx.select_type(0x01, 0x02);
        efx.set_parameter(address, value);
        const auto coef = efx.coefficients();
        return std::vector<float>(coef.begin(), coef.end());
    };

    for (const int address : {0x03, 0x04, 0x13, 0x14, 0x16}) {
        INFO("parameter address " << std::hex << address);
        CHECK(registers_for(address, 0x10) != registers_for(address, 0x70));
    }

    // And the two bands land on their own registers rather than sharing one: a handler that wrote
    // the same triple twice would pass the check above.
    CHECK(registers_for(0x13, 0x20) != registers_for(0x14, 0x20));
}

TEST_CASE("the chorus types' parameters reach their registers", "[efx][sccore]")
{
    const RomImage rom = open_rom();

    // `fx_param_apply_4b980` and `fx_param_apply_580c0` transcribed. Swept against the module with
    // `scdec efxdump <type> <addr> <value>` over every address these handlers read: **Hexa Chorus
    // 50 of 50 exact** at five values including its latched ranges, **Space D 24 of 24**, and both
    // exact at their defaults on the tap program as well as the coefficient file.
    const auto registers_for = [&rom](int msb, int lsb, int address, int value) {
        InsertionEffect efx{rom};
        efx.select_type(msb, lsb);
        efx.set_parameter(address, value);
        const auto coef = efx.coefficients();
        return std::vector<float>(coef.begin(), coef.end());
    };

    // Rate, depth, their two spreads, the stereo spread, feedback, the two bands and the level.
    // The pairs are per address rather than uniform, because two of these parameters would not
    // move under one: the depth spread accepts only 0x2C-0x54 and latches anything else, and the
    // stereo spread only 0x00-0x14. Several of the shared curves are also flat across their bottom
    // few entries, so a low pair proves nothing.
    const std::vector<std::pair<int, std::pair<int, int>>> hexa{
        {0x03, {0x10, 0x60}}, {0x04, {0x10, 0x60}}, {0x05, {0x10, 0x60}}, {0x06, {0x02, 0x12}},
        {0x07, {0x2C, 0x54}}, {0x08, {0x02, 0x12}}, {0x12, {0x10, 0x60}}, {0x13, {0x10, 0x60}},
        {0x14, {0x10, 0x60}}, {0x16, {0x10, 0x60}},
    };
    for (const auto& [address, values] : hexa) {
        INFO("Hexa Chorus, parameter address " << std::hex << address);
        CHECK(registers_for(0x01, 0x40, address, values.first)
              != registers_for(0x01, 0x40, address, values.second));
    }
    for (const int address : {0x03, 0x04, 0x05, 0x06, 0x12, 0x13, 0x14, 0x16}) {
        INFO("Space D, parameter address " << std::hex << address);
        CHECK(registers_for(0x01, 0x43, address, 0x10) != registers_for(0x01, 0x43, address, 0x60));
    }

    // **The stereo spread starts latched at its default and must stay put until it is written.**
    // Firing it on the select moved twelve registers away from the module, and firing it when some
    // other parameter changed did the same.
    InsertionEffect fresh{rom};
    fresh.select_type(0x01, 0x40);
    const std::vector<float> at_select(fresh.coefficients().begin(), fresh.coefficients().end());
    fresh.set_parameter(0x03, 0x20); // a different parameter entirely
    const std::vector<float> after_other(fresh.coefficients().begin(), fresh.coefficients().end());
    CHECK(at_select[0x106] == after_other[0x106]);
    fresh.set_parameter(0x08, 0x05); // now the spread itself
    CHECK(fresh.coefficients()[0x106] != at_select[0x106]);
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
