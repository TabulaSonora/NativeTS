#pragma once

#include "playback_source.hpp"

#include "tabulasonora/note_renderer.hpp"
#include "tabulasonora/sequence_player.hpp"
#include "tabulasonora/render_options.hpp"
#include "tabulasonora/tone_generator.hpp"

#include <filesystem>
#include <vector>

namespace ts::player {

/// A rendered song held in memory, with a transport over it.
///
/// The whole song is rendered before playback starts, which makes seeking free and exact and costs
/// nothing at play time. `StreamingSource` is the alternative, and the usual one.
class PlaybackBuffer final : public PlaybackSource {
public:
    PlaybackBuffer(std::vector<float> left, std::vector<float> right, int sample_rate)
        : left_(std::move(left)), right_(std::move(right)), sample_rate_(sample_rate)
    {
    }

    [[nodiscard]] int sample_rate() const noexcept override { return sample_rate_; }

    [[nodiscard]] std::int64_t length() const noexcept override
    {
        return static_cast<std::int64_t>(left_.size());
    }

    [[nodiscard]] std::int64_t position() const noexcept override { return position_; }

    void set_position(std::int64_t frame) override;

    std::size_t read(std::span<float> interleaved, float gain) override;

private:
    std::vector<float> left_;
    std::vector<float> right_;
    int sample_rate_;
    std::int64_t position_ = 0;
};

/// A song synthesised as it plays, through the engine's block-based voice loop.
///
/// Playback starts at once -- there is nothing to wait for -- and a song of any length costs the
/// same few kilobytes of buffers. What it gives up against `PlaybackBuffer` is exactness of
/// seeking: a seek has to rebuild the engine's state by replaying the file's controllers up to the
/// target, which drops whatever tail was ringing.
///
/// The engine and the player are held by value and in that order, because the player keeps a
/// reference to the engine and the engine keeps one to the note renderer, which the caller owns.
class StreamingSource final : public PlaybackSource {
public:
    /// Creates a source over a file.
    ///
    /// Anything `options` points at -- a channel mask above all -- must outlive this.
    StreamingSource(NoteRenderer& notes,
                    const ToneGeneratorOptions& options,
                    const std::filesystem::path& midi,
                    double tail_seconds);

    [[nodiscard]] int sample_rate() const noexcept override { return ToneGenerator::sample_rate; }

    [[nodiscard]] std::int64_t length() const noexcept override { return length_; }

    [[nodiscard]] std::int64_t position() const noexcept override { return player_.position(); }

    void set_position(std::int64_t frame) override;

    std::size_t read(std::span<float> interleaved, float gain) override;

    void capture(EngineSnapshot& into) const override;

    /// The engine being driven.
    ///
    /// Only the render thread may touch it: everything a UI needs comes through `capture`.
    [[nodiscard]] const ToneGenerator& generator() const noexcept { return generator_; }

private:
    ToneGenerator generator_;
    SequencePlayer player_;
    std::int64_t length_ = 0;
    std::vector<float> left_;
    std::vector<float> right_;
    const ChannelMask* channels_ = nullptr;
    /// Parts the file addresses, computed once so the mixer can list only those.
    std::vector<int> addressed_;
};

} // namespace ts::player
