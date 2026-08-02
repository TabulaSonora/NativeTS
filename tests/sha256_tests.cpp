#include "rom/sha256.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <vector>

using namespace ts;

namespace {

[[nodiscard]] std::string hash_of(const std::string& text)
{
    Sha256 hash;
    hash.update(reinterpret_cast<const std::uint8_t*>(text.data()), text.size());
    return hash.finish_hex();
}

} // namespace

// The published FIPS 180-4 vectors. This hash exists only to recognise the pinned SCCore.dll, so
// what matters is that it is the real SHA-256 and not something that merely looks like one.

TEST_CASE("sha256 matches the published vectors", "[sha256]")
{
    CHECK(hash_of("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(hash_of("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(hash_of("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")
          == "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
}

TEST_CASE("sha256 spans block boundaries correctly", "[sha256]")
{
    // The padding path branches at 55, 56 and 64 bytes, and update() has a separate branch for
    // filling a partial buffer. These lengths walk every one of them.
    CHECK(hash_of(std::string(55, 'a'))
          == "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318");
    CHECK(hash_of(std::string(56, 'a'))
          == "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a");
    CHECK(hash_of(std::string(64, 'a'))
          == "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb");
    CHECK(hash_of(std::string(1000, 'a'))
          == "41edece42d63e8d9bf515a9ba6932e1c20cbc9f5a5d134645adb5db1b9737ea3");
}

TEST_CASE("sha256 is insensitive to how the input is chunked", "[sha256]")
{
    // The engine hashes a 27 MB file in one-megabyte reads, so a digest that depended on chunk
    // boundaries would fail only on the real DLL and only sometimes.
    const std::vector<std::uint8_t> data(4096, 0x5A);

    Sha256 whole;
    whole.update(data.data(), data.size());

    Sha256 chunked;
    for (std::size_t offset = 0; offset < data.size(); offset += 7) {
        chunked.update(data.data() + offset, std::min<std::size_t>(7, data.size() - offset));
    }

    CHECK(whole.finish_hex() == chunked.finish_hex());
}
