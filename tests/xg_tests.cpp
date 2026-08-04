#include "tabulasonora/control_decode.hpp"

#include "tabulasonora/patch_directory.hpp"
#include "tabulasonora/table_set.hpp"
#include "tabulasonora/tone_generator.hpp"

#include "test_data.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

using namespace ts;

namespace {

/// `F0 43 10 4C <aH> <aM> <aL> <data...> F7`.
std::vector<std::uint8_t> xg(int high, int mid, int low, std::vector<std::uint8_t> data = {})
{
    std::vector<std::uint8_t> bytes{0xF0, 0x43, 0x10, 0x4C, static_cast<std::uint8_t>(high),
                                    static_cast<std::uint8_t>(mid),
                                    static_cast<std::uint8_t>(low)};
    bytes.insert(bytes.end(), data.begin(), data.end());
    bytes.push_back(0xF7);
    return bytes;
}

} // namespace

TEST_CASE("XG System On and All Parameter Reset are recognised", "[xg]")
{
    CHECK(decode_xg_sysex(xg(0x00, 0x00, 0x7E, {0x00})).kind == XgMessage::system_on);
    CHECK(decode_xg_sysex(xg(0x00, 0x00, 0x7F, {0x00})).kind == XgMessage::all_parameter_reset);
    CHECK(decode_xg_sysex(xg(0x00, 0x00, 0x04, {0x64})).kind == XgMessage::system_parameter);
}

TEST_CASE("a frame that is not XG decodes to nothing", "[xg]")
{
    // Roland.
    CHECK(decode_xg_sysex({{0xF0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x00, 0x7F, 0x00, 0x41, 0xF7}})
              .kind
          == XgMessage::none);

    // A bulk dump, device `0n`, which this engine does not accept.
    std::vector<std::uint8_t> dump = xg(0x08, 0x00, 0x0B, {0x64});
    dump[2] = 0x00;
    CHECK(decode_xg_sysex(dump).kind == XgMessage::none);

    // Another Yamaha model on a `1n` device. The module does not check the model byte once its XG
    // parser is armed and will act on this; requiring 4C is a deliberate departure.
    std::vector<std::uint8_t> tg300b = xg(0x30, 0x00, 0x00, {0x00});
    tg300b[3] = 0x27;
    CHECK(decode_xg_sysex(tg300b).kind == XgMessage::none);

    // Truncated, and missing its terminator.
    CHECK(decode_xg_sysex({{0xF0, 0x43, 0x10, 0x4C}}).kind == XgMessage::none);
}

TEST_CASE("XG Multi Part addresses the part its number names", "[xg]")
{
    // No remap: this engine indexes parts by MIDI channel, which is what XG counts. Part 9 is
    // channel 10, the drum channel, and must not come back as part 0 the way the module's own
    // GS-block-ordered array would have it.
    CHECK(decode_xg_sysex(xg(0x08, 0x00, 0x0B, {0x64})).part == 0);
    CHECK(decode_xg_sysex(xg(0x08, 0x09, 0x0B, {0x64})).part == 9);
    CHECK(decode_xg_sysex(xg(0x08, 0x0F, 0x0B, {0x64})).part == 15);

    // Past one port, and at the top of what XG can address. Range checking is the caller's.
    CHECK(decode_xg_sysex(xg(0x08, 0x20, 0x0B, {0x64})).part == 32);
    CHECK(decode_xg_sysex(xg(0x08, 0x3F, 0x0B, {0x64})).part == 63);
}

TEST_CASE("XG Multi Part parameters decode to the shared targets", "[xg]")
{
    const auto value_of = [](int parameter, int value) {
        return decode_xg_multi_part(decode_xg_sysex(
            xg(0x08, 0x03, parameter, {static_cast<std::uint8_t>(value)})));
    };

    const std::optional<ControlUpdate> volume = value_of(0x0B, 0x64);
    REQUIRE(volume);
    CHECK(volume->target == ControlTarget::volume);
    CHECK(volume->channel == 3);
    CHECK(volume->value == 0x64);

    // Reverb and chorus are the other way round from the order a reader expects: 0x12 is chorus.
    const std::optional<ControlUpdate> chorus = value_of(0x12, 0x40);
    REQUIRE(chorus);
    CHECK(chorus->target == ControlTarget::chorus_send);
    const std::optional<ControlUpdate> reverb = value_of(0x13, 0x40);
    REQUIRE(reverb);
    CHECK(reverb->target == ControlTarget::reverb_send);

    // Pan zero is random, folded to one exactly as CC#10's is.
    const std::optional<ControlUpdate> pan = value_of(0x0E, 0x00);
    REQUIRE(pan);
    CHECK(pan->target == ControlTarget::pan);
    CHECK(pan->value == 1);

    // Bank, program, part mode and the rest do something rather than store something, so they are
    // deliberately not in this vocabulary.
    CHECK_FALSE(value_of(0x01, 0x7F));
    CHECK_FALSE(value_of(0x03, 0x20));
    CHECK_FALSE(value_of(0x07, 0x01));
}

TEST_CASE("XG blocks this engine does not act on are still classified", "[xg]")
{
    CHECK(decode_xg_sysex(xg(0x02, 0x01, 0x00, {0x01, 0x00})).kind == XgMessage::effect1);
    CHECK(decode_xg_sysex(xg(0x30, 0x24, 0x02, {0x40})).kind == XgMessage::drum_setup);
    CHECK(decode_xg_sysex(xg(0x33, 0x24, 0x02, {0x40})).kind == XgMessage::drum_setup);

    // The A/D part, which the module also ignores.
    CHECK(decode_xg_sysex(xg(0x0A, 0x00, 0x00, {0x00})).kind == XgMessage::none);
}

// The table-backed half of XG: that the map selector reaches the XG layout at all, and that the
// bank the module substitutes for bank MSB 64 is the one carrying the SFX voices.
//
// `MAKORO.MID` is the case that motivated this. Its channel 6 selects bank MSB 64, LSB 0, program
// 90. Read the LSB alone and program 90 is Polysynth; take the MSB's substitution and it is
// Submarine -- the same preset number on a different column of the same map. Getting this wrong is
// silent, since both resolve to a real instrument and only the wrong one plays.
TEST_CASE("XG bank MSB 64 reaches the SFX voice column", "[xg][tables]")
{
    const TableSet tables = TableSet::from_cache_directory(testdata::require_tables());
    const PatchDirectory directory{tables};

    const auto name_of = [&](int bank, int program) {
        const std::vector<int> tones = directory.program_tones(program, ToneMap::xg, bank);
        REQUIRE_FALSE(tones.empty());
        std::vector<ToneZone> zones;
        const std::optional<std::string> name = directory.tone_zones(tones.front(), zones);
        REQUIRE(name);
        std::string trimmed = *name;
        while (!trimmed.empty() && trimmed.back() == ' ') {
            trimmed.pop_back();
        }
        return trimmed;
    };

    // Bank LSB 0 is the plain XG voice; 125 is the column bank MSB 64 substitutes.
    CHECK(name_of(0, 90) == "Polysynth");
    CHECK(name_of(0x7D, 90) == "Submarine");

    // And the map really is XG's rather than a vintage's: the SC-8820 map has no such column.
    const std::vector<int> native = directory.program_tones(90, ToneMap::sc8820, 0x7D);
    CHECK(native != directory.program_tones(90, ToneMap::xg, 0x7D));
}

// Out-of-range XG parts are ignored, not folded.
//
// This is the property that separates the engine from the module, and it fails silently: the module
// has no bounds check and writes past its part array, and the obvious "safe" repair -- masking the
// index into range -- would apply part 32's settings to part 0. Both are wrong and only one of them
// crashes, so the quiet one needs a test.
//
// The probe is volume zero. Aimed at a part that exists, it silences the note; aimed at one that
// does not, the note must still sound at full level.
TEST_CASE("an XG part above the configured count is dropped, not wrapped", "[xg][sccore]")
{
    const RomImage rom =
        RomImage::open(testdata::require_sccore().string(), RomVerification::quick);
    NoteRenderer notes{rom};

    const auto peak_with_volume_aimed_at = [&](int part) {
        ToneGeneratorOptions options;
        options.ports = 2; // Thirty-two parts, so 0x20 is one past the end.
        ToneGenerator generator{notes, options};

        generator.send_sysex(std::vector<std::uint8_t>{0xF0, 0x43, 0x10, 0x4C, 0x00, 0x00, 0x7E,
                                                       0x00, 0xF7});
        generator.send_sysex(std::vector<std::uint8_t>{
            0xF0, 0x43, 0x10, 0x4C, 0x08, static_cast<std::uint8_t>(part), 0x0B, 0x00, 0xF7});
        generator.send_channel(0x90, 60, 100);

        double peak = 0.0;
        std::vector<float> left(ToneGenerator::block_size);
        std::vector<float> right(ToneGenerator::block_size);
        for (int block = 0; block < 200; ++block) {
            generator.render(left, right);
            for (std::size_t i = 0; i < left.size(); ++i) {
                peak = std::max(peak, static_cast<double>(std::abs(left[i])));
            }
        }
        return peak;
    };

    // Part 0 exists: the volume lands and the note is silent.
    CHECK(peak_with_volume_aimed_at(0x00) == 0.0);

    // Part 32 does not, at two ports. Folding it would mask to part 0 and silence the note too.
    CHECK(peak_with_volume_aimed_at(0x20) > 0.0);
}
