#include "tabulasonora/note_renderer.hpp"
#include "tabulasonora/rom_image.hpp"
#include "tabulasonora/tone_generator.hpp"

#include "test_data.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <memory>
#include <type_traits>
#include <vector>

using namespace ts;
namespace fs = std::filesystem;

namespace {

// The shape a host has to work with, asserted rather than assumed. A ROM image exists only as the
// return of a factory, so a host whose lifetime starts before the DLL is chosen has nothing to
// construct a member from and must defer -- which is the whole reason the documented pattern holds
// the chain behind pointers.
static_assert(!std::is_default_constructible_v<RomImage>);
static_assert(!std::is_copy_constructible_v<RomImage>);
static_assert(std::is_move_constructible_v<RomImage>);

static_assert(!std::is_default_constructible_v<NoteRenderer>);
static_assert(!std::is_copy_constructible_v<NoteRenderer>);
static_assert(std::is_move_constructible_v<NoteRenderer>);

static_assert(!std::is_default_constructible_v<ToneGenerator>);
static_assert(!std::is_copy_constructible_v<ToneGenerator>);
static_assert(std::is_move_constructible_v<ToneGenerator>);

/// One block of the engine, and the peak in it.
[[nodiscard]] float render_peak(ToneGenerator& engine, int samples)
{
    std::vector<float> left(static_cast<std::size_t>(samples));
    std::vector<float> right(static_cast<std::size_t>(samples));
    engine.render(left, right);

    float peak = 0.0f;
    for (std::size_t i = 0; i < left.size(); ++i) {
        peak = std::max({peak, std::abs(left[i]), std::abs(right[i])});
    }
    return peak;
}

} // namespace

TEST_CASE("a host can own the chain through unique_ptr members", "[ownership][sccore]")
{
    // The pattern Getting started documents, compiled and run: a host that holds the three layers
    // as members, fills them on startup and releases them in reverse on shutdown. The claim under
    // test is that `make_unique<const RomImage>` over the factory's return is well formed and that
    // the layers above stay valid over it -- both silent to lose, since a host that got it wrong
    // would still compile and would still render until something moved.
    const fs::path dll = testdata::require_sccore();

    std::unique_ptr<const RomImage> rom;
    std::unique_ptr<NoteRenderer> notes;
    std::unique_ptr<ToneGenerator> engine;

    rom = std::make_unique<const RomImage>(RomImage::open(dll.string(), RomVerification::quick));
    notes = std::make_unique<NoteRenderer>(*rom);
    engine = std::make_unique<ToneGenerator>(*notes);

    engine->send_channel(0xC0, 48, 0);
    engine->send_channel(0x90, 60, 100);
    CHECK(render_peak(*engine, ToneGenerator::sample_rate / 10) > 0.0f);
    CHECK(engine->note_count() == 1);

    // Shutdown is reverse order, and it has to leave the host able to start again -- which is what
    // a plugin does when it is pointed at a different DLL.
    engine.reset();
    notes.reset();
    rom.reset();

    rom = std::make_unique<const RomImage>(RomImage::open(dll.string(), RomVerification::quick));
    notes = std::make_unique<NoteRenderer>(*rom);
    engine = std::make_unique<ToneGenerator>(*notes);

    engine->send_channel(0xC0, 48, 0);
    engine->send_channel(0x90, 60, 100);
    CHECK(render_peak(*engine, ToneGenerator::sample_rate / 10) > 0.0f);
}

TEST_CASE("moving the owner leaves the borrowed addresses alone", "[ownership][sccore]")
{
    // Why the pointer rather than the value. A NoteRenderer captures the address of the image it
    // was built over, so the guarantee a host needs is that moving itself does not move what its
    // layers captured. Holding the image behind a unique_ptr is what provides it.
    const fs::path dll = testdata::require_sccore();

    auto rom = std::make_unique<const RomImage>(
        RomImage::open(dll.string(), RomVerification::quick));
    const RomImage* const before = rom.get();

    auto moved = std::move(rom);
    CHECK(moved.get() == before);

    // Still the same image, still readable through the address the layer above would have kept.
    NoteRenderer notes{*moved};
    CHECK(notes.directory().tone_count() > 0);
    CHECK(moved->length() == moved->manifest().dll().size);
}
