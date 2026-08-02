#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace ts {

/// A streaming SHA-256, used only to verify that the supplied `SCCore.dll` is the pinned build.
///
/// Vendored rather than taken from a crypto library: this is the engine's one hashing need, it is
/// an integrity check rather than a security boundary, and a 27 MB file hashed once at start-up is
/// not worth an OpenSSL dependency in a library meant to be embedded.
class Sha256 {
public:
    /// Length of a digest in bytes.
    static constexpr std::size_t digest_size = 32;

    Sha256() = default;

    /// Feeds bytes into the hash.
    void update(const std::uint8_t* data, std::size_t length) noexcept;

    /// Finishes the hash and returns the digest. The object must not be reused afterwards.
    [[nodiscard]] std::array<std::uint8_t, digest_size> finish() noexcept;

    /// Finishes the hash and returns it as lower-case hex, which is the form the manifest records.
    [[nodiscard]] std::string finish_hex() noexcept;

private:
    void compress(const std::uint8_t* block) noexcept;

    std::array<std::uint32_t, 8> state_{0x6a09e667u,
                                        0xbb67ae85u,
                                        0x3c6ef372u,
                                        0xa54ff53au,
                                        0x510e527fu,
                                        0x9b05688cu,
                                        0x1f83d9abu,
                                        0x5be0cd19u};
    std::array<std::uint8_t, 64> buffer_{};
    std::uint64_t length_ = 0;
    std::size_t pending_ = 0;
};

} // namespace ts
