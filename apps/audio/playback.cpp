#include "playback.hpp"

#include <miniaudio.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <utility>

namespace ts::player {
namespace {

/// Lowercases a name for the substring match `--device` does.
[[nodiscard]] std::string folded(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return text;
}

/// A context, opened and closed around one enumeration or one device.
class Context {
public:
    Context()
    {
        if (ma_context_init(nullptr, 0, nullptr, &context_) != MA_SUCCESS) {
            throw std::runtime_error("Cannot initialise the audio backend.");
        }
    }

    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;
    Context(Context&&) = delete;
    Context& operator=(Context&&) = delete;

    ~Context() { ma_context_uninit(&context_); }

    [[nodiscard]] ma_context& get() noexcept { return context_; }

    /// The backend's playback devices, in the backend's own order.
    [[nodiscard]] std::pair<ma_device_info*, ma_uint32> devices()
    {
        ma_device_info* playback = nullptr;
        ma_uint32 count = 0;
        ma_device_info* capture = nullptr;
        ma_uint32 capture_count = 0;

        if (ma_context_get_devices(&context_, &playback, &count, &capture, &capture_count)
            != MA_SUCCESS) {
            throw std::runtime_error("Cannot enumerate output devices.");
        }
        return {playback, count};
    }

private:
    ma_context context_{};
};

} // namespace

std::vector<DeviceInfo> output_devices()
{
    Context context;
    const auto [playback, count] = context.devices();

    std::vector<DeviceInfo> devices;
    devices.reserve(count);
    for (ma_uint32 i = 0; i < count; ++i) {
        devices.push_back(DeviceInfo{playback[i].name, playback[i].isDefault != 0});
    }
    return devices;
}

struct Playback::Impl {
    std::unique_ptr<PlaybackSource> source;
    FrameRing ring;
    DeviceOptions options;

    Context context;
    ma_device device{};
    bool device_open = false;

    std::string device_name;
    std::string backend_name;
    int device_rate = 0;

    std::atomic<float> gain{1.0F};
    std::atomic<bool> quit{false};
    std::atomic<bool> finished{false};
    std::atomic<bool> paused{false};
    std::atomic<std::int64_t> audible{0};

    std::mutex commands;
    std::optional<std::int64_t> pending_seek;

    mutable std::mutex snapshot_lock;
    EngineSnapshot snapshot;

    std::thread renderer;

    Impl(std::unique_ptr<PlaybackSource> owned, const DeviceOptions& device_options)
        : source(std::move(owned)),
          // Sized from the requested lead, and never smaller than a few device periods: the render
          // thread is scheduled like any other, and the ring has to cover its worst nap rather than
          // its average.
          ring(2
               * static_cast<std::size_t>(std::max<std::int64_t>(
                   static_cast<std::int64_t>(device_options.period_frames) * 4,
                   static_cast<std::int64_t>(device_options.latency_ms) * source->sample_rate()
                       / 1000))),
          options(device_options)
    {
        gain.store(options.gain, std::memory_order_relaxed);
    }

    void run();
    void publish();
};

namespace {

/// The audio callback: a copy out of the ring and nothing else.
void data_callback(ma_device* device, void* output, const void* input, ma_uint32 frames)
{
    (void)input;
    auto* ring = static_cast<FrameRing*>(device->pUserData);
    ring->read(std::span{static_cast<float*>(output), static_cast<std::size_t>(frames) * 2});
}

} // namespace

void Playback::Impl::publish()
{
    EngineSnapshot taken;
    source->capture(taken);

    const std::lock_guard<std::mutex> guard{snapshot_lock};
    snapshot = taken;
}

void Playback::Impl::run()
{
    const auto block_frames = static_cast<std::size_t>(options.period_frames);
    std::vector<float> block(block_frames * 2, 0.0F);
    int until_publish = 0;

    while (!quit.load(std::memory_order_relaxed)) {
        std::optional<std::int64_t> target;
        {
            const std::lock_guard<std::mutex> guard{commands};
            target = std::exchange(pending_seek, std::nullopt);
        }

        if (target) {
            // Drop what is queued so the new position is heard now rather than a ring's worth of
            // audio later. The consumer performs the drop; nothing may be written until it has.
            ring.set_starvation_expected(true);
            ring.request_flush();
            source->set_position(*target);
            finished.store(false, std::memory_order_release);
            audible.store(source->position(), std::memory_order_relaxed);
        }

        if (ring.flush_pending()) {
            std::this_thread::sleep_for(std::chrono::microseconds{500});
            continue;
        }

        if (ring.queued() >= ring.capacity() / 2) {
            ring.set_starvation_expected(false);
        }

        if (source->at_end() && ring.queued() == 0) {
            finished.store(true, std::memory_order_release);
            std::this_thread::sleep_for(std::chrono::milliseconds{10});
            continue;
        }

        if (ring.writable() < block_frames) {
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
            continue;
        }

        source->read(block, gain.load(std::memory_order_relaxed));
        ring.write(block);

        audible.store(source->position() - static_cast<std::int64_t>(ring.queued()),
                      std::memory_order_relaxed);

        // Roughly every 20 ms of audio, which is far finer than any display refresh and cheap
        // enough not to matter against a block of synthesis.
        if (--until_publish <= 0) {
            publish();
            until_publish = std::max<int>(1, source->sample_rate() / 50 / options.period_frames);
        }
    }
}

Playback::Playback(std::unique_ptr<PlaybackSource> source, const DeviceOptions& options)
    : impl_(std::make_unique<Impl>(std::move(source), options))
{
    ma_device_id id{};
    bool by_id = false;

    if (!impl_->options.name.empty()) {
        const auto [playback, count] = impl_->context.devices();
        const std::string& wanted = impl_->options.name;

        if (wanted.find_first_not_of("0123456789") == std::string::npos) {
            const auto index = static_cast<ma_uint32>(std::stoul(wanted));
            if (index >= count) {
                throw std::runtime_error("No output device with index " + wanted + "; there are "
                                         + std::to_string(count) + ".");
            }
            id = playback[index].id;
            impl_->device_name = playback[index].name;
            by_id = true;
        } else {
            const std::string needle = folded(wanted);
            for (ma_uint32 i = 0; i < count && !by_id; ++i) {
                if (folded(playback[i].name).find(needle) != std::string::npos) {
                    id = playback[i].id;
                    impl_->device_name = playback[i].name;
                    by_id = true;
                }
            }
            if (!by_id) {
                throw std::runtime_error("No output device matching '" + wanted + "'.");
            }
        }
    }

    ma_device_config config = ma_device_config_init(ma_device_type_playback);
    config.playback.format = ma_format_f32;
    config.playback.channels = 2;
    config.playback.pDeviceID = by_id ? &id : nullptr;

    // The engine's rate is a constant, not a setting. Asking the device for it lets miniaudio
    // convert to whatever the hardware actually runs at, which is one resampler rather than two.
    config.sampleRate = static_cast<ma_uint32>(impl_->source->sample_rate());
    config.periodSizeInFrames = static_cast<ma_uint32>(impl_->options.period_frames);
    config.dataCallback = data_callback;
    config.pUserData = &impl_->ring;

    if (ma_device_init(&impl_->context.get(), &config, &impl_->device) != MA_SUCCESS) {
        throw std::runtime_error("Cannot open the output device.");
    }
    impl_->device_open = true;

    impl_->backend_name = ma_get_backend_name(impl_->device.pContext->backend);
    impl_->device_rate = static_cast<int>(impl_->device.sampleRate);
    if (impl_->device_name.empty()) {
        impl_->device_name = impl_->device.playback.name;
    }

    impl_->publish();
    impl_->renderer = std::thread{[impl = impl_.get()] { impl->run(); }};
}

Playback::~Playback()
{
    impl_->quit.store(true, std::memory_order_relaxed);
    if (impl_->renderer.joinable()) {
        impl_->renderer.join();
    }
    if (impl_->device_open) {
        ma_device_uninit(&impl_->device);
    }
}

void Playback::start()
{
    // Fill the ring before the device starts, or the first blocks come out as counted underruns
    // through no fault of the renderer.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    while (impl_->ring.queued() < impl_->ring.capacity() / 2
           && !impl_->finished.load(std::memory_order_acquire)
           && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds{2});
    }

    if (ma_device_start(&impl_->device) != MA_SUCCESS) {
        throw std::runtime_error("Cannot start the output device.");
    }
}

const std::string& Playback::device_name() const noexcept
{
    return impl_->device_name;
}

const std::string& Playback::backend_name() const noexcept
{
    return impl_->backend_name;
}

int Playback::device_rate() const noexcept
{
    return impl_->device_rate;
}

int Playback::sample_rate() const noexcept
{
    return impl_->source->sample_rate();
}

std::int64_t Playback::length() const noexcept
{
    return impl_->source->length();
}

std::int64_t Playback::position() const noexcept
{
    return std::clamp(
        impl_->audible.load(std::memory_order_relaxed), std::int64_t{0}, impl_->source->length());
}

bool Playback::finished() const noexcept
{
    return impl_->finished.load(std::memory_order_acquire);
}

bool Playback::paused() const noexcept
{
    return impl_->paused.load(std::memory_order_relaxed);
}

void Playback::set_paused(bool paused)
{
    if (paused == impl_->paused.load(std::memory_order_relaxed)) {
        return;
    }

    impl_->paused.store(paused, std::memory_order_relaxed);

    // Stopping the device is what actually pauses: the ring keeps its contents, so resuming picks
    // up exactly where it left off rather than dropping the lead that was already rendered.
    if (paused) {
        ma_device_stop(&impl_->device);
    } else {
        ma_device_start(&impl_->device);
    }
}

bool Playback::looping() const noexcept
{
    return impl_->source->looping();
}

void Playback::set_looping(bool looping) noexcept
{
    // Safe from this thread by the source's own contract: the request is an atomic the render
    // thread applies at its next block.
    impl_->source->set_looping(looping);
}

void Playback::seek(std::int64_t frame)
{
    const std::lock_guard<std::mutex> guard{impl_->commands};
    impl_->pending_seek = std::clamp(frame, std::int64_t{0}, impl_->source->length());
}

void Playback::seek_by(double seconds)
{
    seek(position() + static_cast<std::int64_t>(seconds * sample_rate()));
}

void Playback::set_gain(float gain) noexcept
{
    impl_->gain.store(gain, std::memory_order_relaxed);
}

float Playback::gain() const noexcept
{
    return impl_->gain.load(std::memory_order_relaxed);
}

std::pair<float, float> Playback::peak() const noexcept
{
    return impl_->ring.peak();
}

std::int64_t Playback::underruns() const noexcept
{
    return impl_->ring.underruns();
}

EngineSnapshot Playback::snapshot() const
{
    const std::lock_guard<std::mutex> guard{impl_->snapshot_lock};
    return impl_->snapshot;
}

} // namespace ts::player
