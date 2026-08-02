#include "tabulasonora/wav_writer.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <vector>

namespace ts::wav {
namespace {

void put_u32(std::vector<std::uint8_t>& out, std::uint32_t value)
{
    out.push_back(static_cast<std::uint8_t>(value));
    out.push_back(static_cast<std::uint8_t>(value >> 8));
    out.push_back(static_cast<std::uint8_t>(value >> 16));
    out.push_back(static_cast<std::uint8_t>(value >> 24));
}

void put_u16(std::vector<std::uint8_t>& out, std::uint16_t value)
{
    out.push_back(static_cast<std::uint8_t>(value));
    out.push_back(static_cast<std::uint8_t>(value >> 8));
}

void put_tag(std::vector<std::uint8_t>& out, const char* tag)
{
    for (int i = 0; i < 4; ++i) {
        out.push_back(static_cast<std::uint8_t>(tag[i]));
    }
}

/// Converts one float sample to 16-bit PCM.
///
/// Two details, and both were wrong here first time round:
///
/// The conversion **truncates toward zero**, it does not round to nearest. That is what a C# cast
/// from `double` to `short` does, and it is what the reference build therefore does. Rounding
/// instead shifts roughly a quarter of all samples by one LSB -- inaudible on its own, and enough
/// to make a whole-file comparison useless.
///
/// The clamp is applied to the *scaled* value against the asymmetric int16 range, not to the input
/// against [-1, 1]. The two differ at the bottom of the range: a sample at -1.0 gives -32767 under
/// the second reading and -32767 under this one too, but a sample below -1.0 reaches -32768 here
/// and cannot there. Clamped rather than wrapped either way -- a wrapped sample is not a loud
/// sample, it is a click at the opposite polarity.
[[nodiscard]] std::int16_t to_pcm16(float sample) noexcept
{
    const double scaled = std::clamp(static_cast<double>(sample) * 32767.0, -32768.0, 32767.0);
    return static_cast<std::int16_t>(scaled);
}

} // namespace

void write(const std::filesystem::path& path,
           std::span<const float> left,
           std::span<const float> right,
           int sample_rate)
{
    if (left.size() != right.size()) {
        throw std::runtime_error("A stereo render needs both channels the same length.");
    }

    constexpr int channels = 2;
    constexpr int bits = 16;
    const auto frames = static_cast<std::uint32_t>(left.size());
    const std::uint32_t data_bytes = frames * channels * (bits / 8);

    std::vector<std::uint8_t> header;
    header.reserve(44);
    put_tag(header, "RIFF");
    put_u32(header, 36 + data_bytes);
    put_tag(header, "WAVE");
    put_tag(header, "fmt ");
    put_u32(header, 16);
    put_u16(header, 1); // PCM
    put_u16(header, channels);
    put_u32(header, static_cast<std::uint32_t>(sample_rate));
    put_u32(header, static_cast<std::uint32_t>(sample_rate) * channels * (bits / 8));
    put_u16(header, channels * (bits / 8));
    put_u16(header, bits);
    put_tag(header, "data");
    put_u32(header, data_bytes);

    std::ofstream stream{path, std::ios::binary};
    if (!stream) {
        throw std::runtime_error("Cannot write '" + path.string() + "'.");
    }
    stream.write(reinterpret_cast<const char*>(header.data()),
                 static_cast<std::streamsize>(header.size()));

    std::vector<std::uint8_t> block;
    block.reserve(left.size() * 4);
    for (std::size_t i = 0; i < left.size(); ++i) {
        put_u16(block, static_cast<std::uint16_t>(to_pcm16(left[i])));
        put_u16(block, static_cast<std::uint16_t>(to_pcm16(right[i])));
    }
    stream.write(reinterpret_cast<const char*>(block.data()),
                 static_cast<std::streamsize>(block.size()));

    if (!stream) {
        throw std::runtime_error("Short write to '" + path.string() + "'.");
    }
}

} // namespace ts::wav
