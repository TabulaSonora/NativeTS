#include "tabulasonora/frame_ring.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <thread>
#include <vector>

namespace {

/// Fills a block with a frame counter, so the consumer can prove it saw every frame in order.
void stamp(std::vector<float>& block, std::int64_t first)
{
    for (std::size_t i = 0; i < block.size() / 2; ++i) {
        block[i * 2] = static_cast<float>(first + static_cast<std::int64_t>(i));
        block[(i * 2) + 1] = -block[i * 2];
    }
}

} // namespace

TEST_CASE("a ring rounds its capacity up to a power of two", "[ring]")
{
    CHECK(ts::FrameRing{100}.capacity() == 128);
    CHECK(ts::FrameRing{128}.capacity() == 128);
    CHECK(ts::FrameRing{129}.capacity() == 256);

    // The smallest ring is still a ring, not a division by zero in the mask.
    CHECK(ts::FrameRing{1}.capacity() == 2);
}

TEST_CASE("what is written comes back in order", "[ring]")
{
    ts::FrameRing ring{16};
    REQUIRE(ring.queued() == 0);
    REQUIRE(ring.writable() == 16);

    std::vector<float> block(8 * 2);
    stamp(block, 0);
    ring.write(block);

    CHECK(ring.queued() == 8);
    CHECK(ring.writable() == 8);

    std::vector<float> out(4 * 2, 99.0F);
    CHECK(ring.read(out) == 4);
    for (int i = 0; i < 4; ++i) {
        CHECK(out[static_cast<std::size_t>(i) * 2] == static_cast<float>(i));
        CHECK(out[(static_cast<std::size_t>(i) * 2) + 1] == -static_cast<float>(i));
    }

    CHECK(ring.queued() == 4);
}

TEST_CASE("a short read is zero-filled and counted as an underrun", "[ring]")
{
    ts::FrameRing ring{16};

    std::vector<float> block(2 * 2);
    stamp(block, 7);
    ring.write(block);

    std::vector<float> out(6 * 2, 99.0F);
    CHECK(ring.read(out) == 2);

    // The two frames that existed, then silence -- never the 99s the caller left behind, or the
    // device would play whatever was in that buffer last.
    CHECK(out[0] == 7.0F);
    CHECK(out[2] == 8.0F);
    for (std::size_t i = 4; i < out.size(); ++i) {
        CHECK(out[i] == 0.0F);
    }

    CHECK(ring.underruns() == 1);
}

TEST_CASE("expected starvation is not an underrun", "[ring]")
{
    ts::FrameRing ring{16};
    ring.set_starvation_expected(true);

    std::vector<float> out(4 * 2, 0.0F);
    CHECK(ring.read(out) == 0);
    CHECK(ring.underruns() == 0);

    ring.set_starvation_expected(false);
    CHECK(ring.read(out) == 0);
    CHECK(ring.underruns() == 1);
}

TEST_CASE("a flush drops what is queued, and only the consumer performs it", "[ring]")
{
    ts::FrameRing ring{16};

    std::vector<float> block(8 * 2);
    stamp(block, 0);
    ring.write(block);
    REQUIRE(ring.queued() == 8);

    ring.request_flush();

    // Nothing has happened yet: the producer cannot rewind the write cursor without racing the
    // consumer's reads, so the queued frames are still there until the consumer next runs.
    CHECK(ring.flush_pending());
    CHECK(ring.queued() == 8);

    std::vector<float> out(4 * 2, 99.0F);
    CHECK(ring.read(out) == 0);
    CHECK_FALSE(ring.flush_pending());
    CHECK(ring.queued() == 0);

    // And the producer may write again, from the position the flush left.
    stamp(block, 100);
    ring.write(block);
    CHECK(ring.read(out) == 4);
    CHECK(out[0] == 100.0F);
}

TEST_CASE("the peak is of the block the consumer just handed over", "[ring]")
{
    ts::FrameRing ring{16};

    std::vector<float> block(4 * 2, 0.0F);
    block[0] = -0.5F; // left
    block[3] = 0.25F; // right
    ring.write(block);

    std::vector<float> out(4 * 2, 0.0F);
    REQUIRE(ring.read(out) == 4);

    const auto [left, right] = ring.peak();
    CHECK(left == 0.5F);
    CHECK(right == 0.25F);
}

TEST_CASE("a producer and a consumer on two threads lose nothing", "[ring]")
{
    // Deliberately small relative to the traffic, so the ring wraps thousands of times and both
    // sides spend real time waiting on the other. A ring large enough never to fill would prove
    // nothing about the counters.
    ts::FrameRing ring{64};
    constexpr std::size_t chunk = 25;
    constexpr std::int64_t total = 200000; // a whole number of chunks

    std::atomic<bool> consuming{true};
    std::atomic<std::int64_t> received{0};
    std::atomic<bool> in_order{true};

    std::thread consumer{[&] {
        std::vector<float> out(chunk * 2, 0.0F);
        std::int64_t expected = 0;

        while (consuming.load(std::memory_order_relaxed) || ring.queued() > 0) {
            const std::size_t got = ring.read(out);
            for (std::size_t i = 0; i < got; ++i) {
                if (out[i * 2] != static_cast<float>(expected)
                    || out[(i * 2) + 1] != -static_cast<float>(expected)) {
                    in_order.store(false, std::memory_order_relaxed);
                }
                ++expected;
            }
            received.store(expected, std::memory_order_relaxed);
        }
    }};

    std::vector<float> block(chunk * 2);
    for (std::int64_t at = 0; at < total; at += static_cast<std::int64_t>(chunk)) {
        stamp(block, at);
        while (ring.writable() < chunk) {
            std::this_thread::yield();
        }
        ring.write(block);
    }

    while (received.load(std::memory_order_relaxed) < total) {
        std::this_thread::yield();
    }
    consuming.store(false, std::memory_order_relaxed);
    consumer.join();

    static_assert(total % static_cast<std::int64_t>(chunk) == 0);

    CHECK(in_order.load());
    CHECK(received.load() == total);

    // Non-vacuity: the consumer really did run dry sometimes, which is what makes the ordering
    // check above worth anything. A test where the ring stayed full would never exercise the wrap.
    CHECK(ring.underruns() > 0);
}
