#include "rom/sha256.hpp"

#include <algorithm>
#include <cstring>

namespace ts {
namespace {

constexpr std::array<std::uint32_t, 64> k_round_constants = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u,
    0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu,
    0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu,
    0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
    0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u,
    0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u,
    0xc67178f2u};

[[nodiscard]] constexpr std::uint32_t rotr(std::uint32_t value, int count) noexcept
{
    return (value >> count) | (value << (32 - count));
}

[[nodiscard]] constexpr std::uint32_t big_endian_word(const std::uint8_t* data) noexcept
{
    return (static_cast<std::uint32_t>(data[0]) << 24) | (static_cast<std::uint32_t>(data[1]) << 16)
           | (static_cast<std::uint32_t>(data[2]) << 8) | static_cast<std::uint32_t>(data[3]);
}

} // namespace

void Sha256::compress(const std::uint8_t* block) noexcept
{
    std::array<std::uint32_t, 64> w{};
    for (std::size_t i = 0; i < 16; ++i) {
        w[i] = big_endian_word(block + i * 4);
    }
    for (std::size_t i = 16; i < 64; ++i) {
        const std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    std::uint32_t a = state_[0];
    std::uint32_t b = state_[1];
    std::uint32_t c = state_[2];
    std::uint32_t d = state_[3];
    std::uint32_t e = state_[4];
    std::uint32_t f = state_[5];
    std::uint32_t g = state_[6];
    std::uint32_t h = state_[7];

    for (std::size_t i = 0; i < 64; ++i) {
        const std::uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        const std::uint32_t ch = (e & f) ^ (~e & g);
        const std::uint32_t temp1 = h + s1 + ch + k_round_constants[i] + w[i];
        const std::uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        const std::uint32_t temp2 = s0 + maj;

        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }

    state_[0] += a;
    state_[1] += b;
    state_[2] += c;
    state_[3] += d;
    state_[4] += e;
    state_[5] += f;
    state_[6] += g;
    state_[7] += h;
}

void Sha256::update(const std::uint8_t* data, std::size_t length) noexcept
{
    length_ += length;

    if (pending_ > 0) {
        const std::size_t wanted = std::min(64 - pending_, length);
        std::memcpy(buffer_.data() + pending_, data, wanted);
        pending_ += wanted;
        data += wanted;
        length -= wanted;

        if (pending_ < 64) {
            return;
        }
        compress(buffer_.data());
        pending_ = 0;
    }

    while (length >= 64) {
        compress(data);
        data += 64;
        length -= 64;
    }

    if (length > 0) {
        std::memcpy(buffer_.data(), data, length);
        pending_ = length;
    }
}

std::array<std::uint8_t, Sha256::digest_size> Sha256::finish() noexcept
{
    const std::uint64_t bit_length = length_ * 8;

    // Pad with 0x80 then zeroes until eight bytes short of a block, then the big-endian bit count.
    buffer_[pending_++] = 0x80;
    if (pending_ > 56) {
        std::memset(buffer_.data() + pending_, 0, 64 - pending_);
        compress(buffer_.data());
        pending_ = 0;
    }
    std::memset(buffer_.data() + pending_, 0, 56 - pending_);

    for (std::size_t i = 0; i < 8; ++i) {
        buffer_[56 + i] = static_cast<std::uint8_t>(bit_length >> (56 - i * 8));
    }
    compress(buffer_.data());

    std::array<std::uint8_t, digest_size> digest{};
    for (std::size_t i = 0; i < 8; ++i) {
        digest[i * 4 + 0] = static_cast<std::uint8_t>(state_[i] >> 24);
        digest[i * 4 + 1] = static_cast<std::uint8_t>(state_[i] >> 16);
        digest[i * 4 + 2] = static_cast<std::uint8_t>(state_[i] >> 8);
        digest[i * 4 + 3] = static_cast<std::uint8_t>(state_[i]);
    }
    return digest;
}

std::string Sha256::finish_hex() noexcept
{
    constexpr char digits[] = "0123456789abcdef";
    const auto digest = finish();

    std::string hex;
    hex.resize(digest_size * 2);
    for (std::size_t i = 0; i < digest_size; ++i) {
        hex[i * 2 + 0] = digits[digest[i] >> 4];
        hex[i * 2 + 1] = digits[digest[i] & 0x0F];
    }
    return hex;
}

} // namespace ts
