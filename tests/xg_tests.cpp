#include "tabulasonora/control_decode.hpp"

#include "tabulasonora/drum_kit_table.hpp"
#include "tabulasonora/note_renderer.hpp"
#include "tabulasonora/rom_image.hpp"
#include "tabulasonora/patch_directory.hpp"
#include "tabulasonora/table_set.hpp"
#include "tabulasonora/tone_generator.hpp"

#include "test_data.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <span>
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

// What a mixer shows for an XG part.
//
// Both UIs used to name programs against one fixed tone map and decide drums by comparing the
// channel number to a configured drum channel. Neither holds under XG: System On moves every part
// onto the XG map, the bank pair is inverted, and bank MSB 127 makes a drum part of any channel.
// The engine now answers all three per part, which is what these pin.
TEST_CASE("the engine reports per-part map, bank and drum state for a display", "[xg][sccore]")
{
    const RomImage rom =
        RomImage::open(testdata::require_sccore().string(), RomVerification::quick);
    NoteRenderer notes{rom};

    ToneGeneratorOptions options;
    options.ports = 1;
    options.map = ToneMap::sc8820; // Deliberately not XG, so the override is what is being seen.
    ToneGenerator generator{notes, options};

    const auto xg = [&](std::vector<std::uint8_t> bytes) { generator.send_sysex(bytes); };

    CHECK_FALSE(generator.xg_mode());
    CHECK(generator.part_tone_map(0) == ToneMap::sc8820);

    xg({0xF0, 0x43, 0x10, 0x4C, 0x00, 0x00, 0x7E, 0x00, 0xF7});
    CHECK(generator.xg_mode());

    // Every part moves to the XG map, whatever the host configured.
    CHECK(generator.part_tone_map(0) == ToneMap::xg);
    CHECK(generator.part_tone_map(9) == ToneMap::xg);

    // The bank LSB is the variation and lands in the lookup bank; the MSB does not.
    generator.send_channel(0xB0, 32, 40);
    CHECK(generator.part_lookup_bank(0) == 40);

    // Bank MSB 64 substitutes the SFX voice column, which is bank 125 -- where program 90 is
    // Submarine and not the Polysynth it is at bank 0.
    generator.send_channel(0xB0, 0, 64);
    CHECK(generator.part_lookup_bank(0) == 0x7D);

    // Drums from bank select alone, on a channel that is not the drum channel.
    CHECK_FALSE(generator.part_is_drum(2));
    generator.send_channel(0xB2, 0, 127);
    generator.send_channel(0xC2, 25, 0);
    CHECK(generator.part_is_drum(2));

    const int kit = generator.part_drum_kit(2);
    REQUIRE(kit >= 0);
    CHECK(notes.drums().kit_name(kit) == "analog kit");

    // And a kit name is a name, not an index -- what the mixer used to show.
    CHECK_FALSE(notes.drums().kit_name(kit).empty());
}

TEST_CASE("XG drum kits are named from their records", "[xg][sccore]")
{
    const RomImage rom =
        RomImage::open(testdata::require_sccore().string(), RomVerification::quick);
    DrumKitTable drums{rom};

    // Row 4 is XG's. The names come out of the kit records rather than a table in this repo.
    const auto name_of = [&](int program) {
        const std::optional<int> kit = drums.kit_for_program(program, 4);
        REQUIRE(kit);
        return drums.kit_name(*kit);
    };
    CHECK(name_of(0) == "standard kit");
    CHECK(name_of(25) == "analog kit");
    CHECK(name_of(48) == "classic kit");
    CHECK(name_of(120) == "SFX 1 kit");

    // The same program on a GS row is a different kit entirely, which is the whole reason the row
    // has to follow the mode.
    const std::optional<int> gs = drums.kit_for_program(25, 0);
    REQUIRE(gs);
    CHECK(drums.kit_name(*gs) == "TR-808");

    CHECK(drums.kit_name(-1).empty());
    CHECK(drums.kit_name(100000).empty());
}

// Starting in XG mode, as a map setting rather than a separate flag.
//
// A file that was authored for an XG module often never sends System On, because the module is
// already in XG at power-on. `CaveStory-MoonSong_XG.mid` is one: no `F0 43` anywhere, but bank LSB
// 18 and 19 on four channels, which under GS is a tone-map selector and means nothing.
TEST_CASE("ToneMap::xg starts the engine in XG mode", "[xg][sccore]")
{
    const RomImage rom =
        RomImage::open(testdata::require_sccore().string(), RomVerification::quick);
    NoteRenderer notes{rom};

    ToneGeneratorOptions options;
    options.ports = 1;
    options.map = ToneMap::xg;
    ToneGenerator generator{notes, options};

    CHECK(generator.xg_mode());
    CHECK(generator.part_tone_map(0) == ToneMap::xg);

    // The bank pair reads XG's way from the first message, with no System On sent.
    generator.send_channel(0xB0, 32, 18);
    CHECK(generator.part_lookup_bank(0) == 18);

    // A reset returns to the configured mode rather than dropping to GS: the host's choice is not
    // something the music did.
    generator.reset();
    CHECK(generator.xg_mode());

    // And a Roland message still leaves XG parsing, because a file may legitimately switch.
    generator.send_sysex(std::vector<std::uint8_t>{0xF0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x00, 0x7F,
                                                   0x00, 0x41, 0xF7});
    CHECK_FALSE(generator.xg_mode());
}

// A program change with no bank select must not decide drum routing.
//
// This is the regression the Cave Story file found. Reading an unwritten bank MSB as zero made
// every program change look like an explicit "melodic", which turned channel 10 into a guitar on
// any file that sends program changes and no bank selects -- which is most of them.
TEST_CASE("XG routing follows the default until a bank select says otherwise", "[xg][sccore]")
{
    const RomImage rom =
        RomImage::open(testdata::require_sccore().string(), RomVerification::quick);
    NoteRenderer notes{rom};

    ToneGeneratorOptions options;
    options.ports = 1;
    options.map = ToneMap::xg;
    ToneGenerator generator{notes, options};

    // Channel 10, a program change and nothing else: still drums, and on the XG kit row.
    generator.send_channel(0xC9, 24, 0);
    CHECK(generator.part_is_drum(9));
    CHECK(notes.drums().kit_name(generator.part_drum_kit(9)) == "electro kit");

    // A melodic channel with no bank select stays melodic.
    CHECK_FALSE(generator.part_is_drum(0));

    // A melodic bank does NOT take the default drum part away. Only XG Part Mode or the GS
    // use-for-rhythm SysEx can do that; bank select choosing a sound is not a routing decision.
    generator.send_channel(0xB9, 0, 0);
    generator.send_channel(0xC9, 24, 0);
    CHECK(generator.part_is_drum(9));

    // XG Part Mode 0 does take it away, because that is what it is for.
    generator.send_sysex(std::vector<std::uint8_t>{0xF0, 0x43, 0x10, 0x4C, 0x08, 0x09, 0x07, 0x00,
                                                   0xF7});
    CHECK_FALSE(generator.part_is_drum(9));

    // And an explicit drum bank makes drums of a channel that is not channel 10, which a melodic
    // bank then does undo -- there it is returning the part to its own default, which is melodic.
    generator.send_channel(0xB3, 0, 127);
    generator.send_channel(0xC3, 25, 0);
    CHECK(generator.part_is_drum(3));
    CHECK(notes.drums().kit_name(generator.part_drum_kit(3)) == "analog kit");

    generator.send_channel(0xB3, 0, 0);
    generator.send_channel(0xC3, 25, 0);
    CHECK_FALSE(generator.part_is_drum(3));
}

// A kit per part under XG, and the module's shared arrangement under GS.
//
// The module gives a port's drum channel one kit slot and shares a second between every other drum
// part on that port. Under GS almost nothing reaches the second, so the limit is invisible and GS
// files are rendered against it -- it stays. XG makes it reachable: bank MSB 127 turns any channel
// into a drum part, so a file can have several and under the module's arrangement all but one would
// sound as whichever moved last. That divergence is deliberate and confined to XG.
TEST_CASE("XG gives each drum part its own kit; GS keeps the module's", "[xg][sccore]")
{
    const RomImage rom =
        RomImage::open(testdata::require_sccore().string(), RomVerification::quick);
    NoteRenderer notes{rom};

    SECTION("XG: two drum parts keep two kits")
    {
        ToneGeneratorOptions options;
        options.ports = 1;
        options.map = ToneMap::xg;
        ToneGenerator generator{notes, options};

        // Channel 10 on its default routing asks for Standard.
        generator.send_channel(0xC9, 0, 0);
        // Channel 11 takes the drum bank and asks for Analog.
        generator.send_channel(0xBA, 0, 127);
        generator.send_channel(0xCA, 25, 0);

        CHECK(notes.drums().kit_name(generator.part_drum_kit(9)) == "standard kit");
        CHECK(notes.drums().kit_name(generator.part_drum_kit(10)) == "analog kit");

        // And the port's kit still means the drum channel's, not part 0's.
        CHECK(generator.drum_kit() == generator.part_drum_kit(9));
    }

    SECTION("GS: the shared slot is preserved")
    {
        ToneGeneratorOptions options;
        options.ports = 1;
        ToneGenerator generator{notes, options};

        // GS use-for-rhythm on channel 11, MAP1 -- the module shares one kit with channel 10.
        const std::uint8_t rhythm[] = {0xF0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x1A,
                                       0x15, 0x01, 0x10, 0xF7};
        generator.send_sysex(std::span<const std::uint8_t>{rhythm, sizeof rhythm});
        generator.send_channel(0xC9, 0, 0);
        generator.send_channel(0xCA, 25, 0);

        REQUIRE(generator.part_is_drum(10));
        CHECK(generator.part_drum_kit(9) == generator.part_drum_kit(10));
    }
}
