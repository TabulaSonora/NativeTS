#include "tabulasonora/soundfont_writer.hpp"

#include <algorithm>
#include <cmath>

#ifdef TS_HAVE_FLAC
#include <FLAC/stream_encoder.h>
#endif

#ifdef TS_HAVE_VORBIS
#include <vorbis/vorbisenc.h>
#include <cstring>
#include <random>
#endif

namespace ts::sf2 {

bool codec_available(Codec codec) noexcept
{
    switch (codec) {
    case Codec::pcm:
        return true;
    case Codec::flac:
#ifdef TS_HAVE_FLAC
        return true;
#else
        return false;
#endif
    case Codec::vorbis:
#ifdef TS_HAVE_VORBIS
        return true;
#else
        return false;
#endif
    }
    return false;
}

#ifdef TS_HAVE_FLAC

namespace {

// Lives inside the FLAC branch because the FLAC encoder is its only caller: at namespace scope it
// would be an unused static whenever the build is configured without libFLAC.
[[nodiscard]] std::int16_t to_pcm16(float value) noexcept
{
    return static_cast<std::int16_t>(
        std::lround(std::clamp(value * 32767.0F, -32768.0F, 32767.0F)));
}

FLAC__StreamEncoderWriteStatus flac_write(const FLAC__StreamEncoder*,
                                          const FLAC__byte buffer[],
                                          std::size_t bytes,
                                          std::uint32_t,
                                          std::uint32_t,
                                          void* client)
{
    auto* out = static_cast<std::vector<std::uint8_t>*>(client);
    out->insert(out->end(), buffer, buffer + bytes);
    return FLAC__STREAM_ENCODER_WRITE_STATUS_OK;
}

} // namespace

std::vector<std::uint8_t> encode_flac(std::span<const float> samples, int sample_rate)
{
    std::vector<std::uint8_t> out;
    FLAC__StreamEncoder* encoder = FLAC__stream_encoder_new();
    if (encoder == nullptr) {
        return out;
    }

    FLAC__stream_encoder_set_channels(encoder, 1);
    FLAC__stream_encoder_set_bits_per_sample(encoder, 16);
    FLAC__stream_encoder_set_sample_rate(encoder, static_cast<unsigned>(sample_rate));
    FLAC__stream_encoder_set_compression_level(encoder, 8);
    FLAC__stream_encoder_set_total_samples_estimate(encoder, samples.size());

    if (FLAC__stream_encoder_init_stream(encoder, flac_write, nullptr, nullptr, nullptr, &out)
        != FLAC__STREAM_ENCODER_INIT_STATUS_OK) {
        FLAC__stream_encoder_delete(encoder);
        return {};
    }

    // The encoder takes one int32 per sample per channel, sign-extended from the sample width.
    std::vector<FLAC__int32> block;
    constexpr std::size_t chunk = 4096;
    block.reserve(chunk);

    for (std::size_t offset = 0; offset < samples.size(); offset += chunk) {
        const std::size_t count = std::min(chunk, samples.size() - offset);
        block.clear();
        for (std::size_t i = 0; i < count; ++i) {
            block.push_back(to_pcm16(samples[offset + i]));
        }
        if (!FLAC__stream_encoder_process_interleaved(encoder, block.data(),
                                                      static_cast<unsigned>(count))) {
            FLAC__stream_encoder_delete(encoder);
            return {};
        }
    }

    FLAC__stream_encoder_finish(encoder);
    FLAC__stream_encoder_delete(encoder);
    return out;
}

#else

std::vector<std::uint8_t> encode_flac(std::span<const float>, int) { return {}; }

#endif

#ifdef TS_HAVE_VORBIS

std::vector<std::uint8_t> encode_vorbis(std::span<const float> samples,
                                        int sample_rate,
                                        float quality)
{
    std::vector<std::uint8_t> out;

    vorbis_info info;
    vorbis_info_init(&info);
    if (vorbis_encode_init_vbr(&info, 1, sample_rate, quality) != 0) {
        vorbis_info_clear(&info);
        return out;
    }

    vorbis_comment comment;
    vorbis_comment_init(&comment);

    vorbis_dsp_state dsp;
    vorbis_block block;
    vorbis_analysis_init(&dsp, &info);
    vorbis_block_init(&dsp, &block);

    ogg_stream_state stream;
    // A fixed serial number keeps the output reproducible; nothing multiplexes these streams, so
    // the value only has to be consistent within one of them.
    ogg_stream_init(&stream, 0x5343);

    {
        ogg_packet header;
        ogg_packet header_comment;
        ogg_packet header_code;
        vorbis_analysis_headerout(&dsp, &comment, &header, &header_comment, &header_code);
        ogg_stream_packetin(&stream, &header);
        ogg_stream_packetin(&stream, &header_comment);
        ogg_stream_packetin(&stream, &header_code);

        // The headers must end on their own page, or a decoder that seeks the first audio page
        // finds header data in it.
        ogg_page page;
        while (ogg_stream_flush(&stream, &page) != 0) {
            out.insert(out.end(), page.header, page.header + page.header_len);
            out.insert(out.end(), page.body, page.body + page.body_len);
        }
    }

    const auto drain = [&](bool flush) {
        while (vorbis_analysis_blockout(&dsp, &block) == 1) {
            vorbis_analysis(&block, nullptr);
            vorbis_bitrate_addblock(&block);
            ogg_packet packet;
            while (vorbis_bitrate_flushpacket(&dsp, &packet) != 0) {
                ogg_stream_packetin(&stream, &packet);
                ogg_page page;
                while ((flush ? ogg_stream_flush(&stream, &page) : ogg_stream_pageout(&stream, &page))
                       != 0) {
                    out.insert(out.end(), page.header, page.header + page.header_len);
                    out.insert(out.end(), page.body, page.body + page.body_len);
                }
            }
        }
    };

    constexpr long chunk = 1024;
    for (std::size_t offset = 0; offset < samples.size(); offset += chunk) {
        const auto count = static_cast<long>(std::min<std::size_t>(chunk, samples.size() - offset));
        float** buffer = vorbis_analysis_buffer(&dsp, static_cast<int>(count));
        for (long i = 0; i < count; ++i) {
            buffer[0][i] = samples[offset + static_cast<std::size_t>(i)];
        }
        vorbis_analysis_wrote(&dsp, static_cast<int>(count));
        drain(false);
    }

    vorbis_analysis_wrote(&dsp, 0);
    drain(true);

    ogg_stream_clear(&stream);
    vorbis_block_clear(&block);
    vorbis_dsp_clear(&dsp);
    vorbis_comment_clear(&comment);
    vorbis_info_clear(&info);
    return out;
}

#else

std::vector<std::uint8_t> encode_vorbis(std::span<const float>, int, float) { return {}; }

#endif

} // namespace ts::sf2
