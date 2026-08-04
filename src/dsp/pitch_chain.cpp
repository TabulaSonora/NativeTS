#include "tabulasonora/pitch_chain.hpp"

#include "dsp/fixed.hpp"

#include <algorithm>
#include <cmath>

namespace ts {
namespace {

/// The key-follow scale lookup, giving a clean 0% to 100% ladder in steps of 10%.
constexpr std::array<int, 11> key_follow_scale{
    0, 3276, 6553, 9830, 13107, 16383, 19660, 22937, 26214, 29491, 32767};

} // namespace

// ---------------------------------------------------------------------------------------------
// PitchEnvelopeRunner
// ---------------------------------------------------------------------------------------------

PitchEnvelopeRunner::PitchEnvelopeRunner(const PitchEnvelope& envelope, bool ignore_note_off)
    : release_(envelope.release),
      release_rate_(PitchChain::segment_rate_word(envelope.release_ms)),
      ignore_note_off_(ignore_note_off),
      segment_start_(envelope.start),
      level_(envelope.start)
{
    targets_.assign(envelope.targets.begin(), envelope.targets.end());

    rates_.reserve(envelope.times.size());
    for (double milliseconds : envelope.times) {
        rates_.push_back(PitchChain::segment_rate_word(milliseconds));
    }
}

PitchEnvelopeRunner PitchEnvelopeRunner::constant(double level)
{
    PitchEnvelopeRunner runner;
    runner.segment_start_ = level;
    runner.level_ = level;
    runner.segment_ = std::numeric_limits<int>::max();
    runner.ignore_note_off_ = true;
    return runner;
}

void PitchEnvelopeRunner::set_release_damper(int damper) noexcept
{
    release_damper_ = std::clamp(damper, 0, 0x3F);
}

int PitchEnvelopeRunner::scaled_release_rate() const noexcept
{
    if (release_damper_ == 0) {
        return release_rate_;
    }

    // 33279 * 65535 is about 2.18e9, past INT32_MAX, so this wraps by design.
    return fx::wmul(0xFFFF - (release_damper_ << 9), release_rate_) >> 16;
}

double PitchEnvelopeRunner::tick(bool released) noexcept
{
    double target = 0.0;
    int rate = 0;

    if (released && !released_ && !ignore_note_off_) {
        released_ = true;
        segment_start_ = level_;
        segment_ = -1;
        phase_ = 0;
        target = release_;
        rate = scaled_release_rate();
    } else if (segment_ < 0) {
        target = release_;
        rate = scaled_release_rate();
    } else if (static_cast<std::size_t>(segment_) < rates_.size()) {
        target = targets_[static_cast<std::size_t>(segment_)];
        rate = rates_[static_cast<std::size_t>(segment_)];
    } else {
        return level_;
    }

    phase_ += rate;
    if (phase_ >= 0xFFFF) {
        // Reaches 0xffff, not exceeds it, and the next segment starts with a fresh phase rather
        // than carrying the remainder -- which is why this has to be stepped, not evaluated.
        level_ = target;
        segment_start_ = target;
        phase_ = 0;
        if (segment_ >= 0) {
            ++segment_;
        }
    } else {
        level_ = segment_start_ + ((target - segment_start_) * (phase_ / 65536.0));
    }

    return level_;
}

// ---------------------------------------------------------------------------------------------
// PitchChain
// ---------------------------------------------------------------------------------------------

PitchChain::PitchChain(const TableSet& tables, const EnvelopeMachine& envelope, EngineNoise* noise)
    : envelope_(&envelope),
      noise_(noise),
      portamento_step_(tables.portamento_step()),
      pitch_bias_(tables.pitch_bias()),
      depth_velocity_sensitivity_(tables.pitch_depth_vs()),
      rate_key_follow0_(tables.kf_pitch_rate0()),
      rate_key_follow1_(tables.kf_pitch_rate1()),
      key_follow_(tables.kf_pitch()),
      rate_curve_(tables.rate_curve())
{
    if (noise_ == nullptr) {
        noise_ = &owned_noise_;
    }

    const auto pitch_env = tables.pitch_env();
    for (std::size_t i = 0; i < depth_slope_.size(); ++i) {
        depth_slope_[i] = fx::read_i16le(pitch_env.data() + (i * 2));
    }
}

PitchChain::KeyFollow
PitchChain::key_follow_key(const PartialParameters& partial, int note, int key_center) noexcept
{
    const auto raw = partial.raw();
    const int transpose = raw[0x10] - 0x40;
    const int amount = raw[0x13] - 0x40;

    if (amount == 10 || amount < 0) {
        return KeyFollow{note + transpose, 0};
    }
    if (amount == 0) {
        return KeyFollow{key_center + transpose, 0};
    }

    const int distance = note - key_center;
    const int product =
        key_follow_scale[static_cast<std::size_t>(std::min(amount, 10))] * (std::abs(distance) * 2);
    const int high = product >> 16;
    const int low = product & 0xFFFF;

    int weight = 0;
    int key_step = 0;
    if (low < 65000) {
        const int scaled = (low * 999) / 65000;
        if (distance >= 0) {
            weight = scaled;
            key_step = high;
        } else {
            weight = 1000 - scaled;
            key_step = ~high;
        }
    } else {
        weight = 0;
        key_step = distance >= 0 ? high + 1 : ~high;
    }

    return KeyFollow{key_center + key_step + transpose, weight};
}

int PitchChain::base_pitch_milli_semitones(const PartialParameters& partial,
                                           int note,
                                           int key_center) const noexcept
{
    const auto raw = partial.raw();
    const KeyFollow follow = key_follow_key(partial, note, key_center);
    const int key = std::clamp(follow.key, 0, 0x7F);

    const int row = std::clamp((raw[0x13] - 0x40) >> 2, 0, 7);
    return (key * 1000) + follow.weight + key_follow_[static_cast<std::size_t>((row * 0x80) + key)]
           + ((raw[0x11] - 0x40) * 10);
}

int PitchChain::drum_pitch_milli_semitones(const PartialParameters& partial,
                                           int coarse_pitch) noexcept
{
    const KeyFollow follow = key_follow_key(partial, coarse_pitch, drum_key_centre);
    const int key = std::clamp(follow.key, 0, 0x7F);
    return (key * 1000) + follow.weight + ((partial.raw()[0x11] - 0x40) * 10);
}

int PitchChain::portamento_step(int time) const noexcept
{
    return portamento_step_[static_cast<std::size_t>(std::clamp(time, 0, 0x7F))];
}

int PitchChain::portamento_offset(int from_key, int target_pitch) noexcept
{
    return (std::clamp(from_key, 0, 0x7F) * 1000) - target_pitch;
}

double PitchChain::bend_offset_milli_semitones(int bend, double semitone_range) noexcept
{
    return (bend - 8192) / 8192.0 * semitone_range * 1000.0;
}

std::optional<PitchEnvelope>
PitchChain::envelope_offsets(const PartialParameters& partial, int key, int velocity) const noexcept
{
    const auto raw = partial.raw();
    const int depth = raw[0x18] | (raw[0x19] << 8);
    if (depth == 0) {
        return std::nullopt;
    }

    int sensitivity = raw[0x2B] - 0x40;
    int scaled_depth = 0;
    if (sensitivity == 0) {
        scaled_depth = depth;
    } else {
        int v = std::clamp(velocity, 0, 127);
        if (sensitivity < 0) {
            sensitivity = -sensitivity;
            v = (-v) & 0x7F;
        }

        // The slope reaches +-32767, v 127 and depth 0xffff, so the product runs to about 2.7e11
        // and the engine keeps only the low word. Widening this to 64 bits changes the depth.
        const int inner =
            fx::wadd(fx::wmul(depth_slope_[static_cast<std::size_t>(sensitivity)], v),
                     depth_velocity_sensitivity_[static_cast<std::size_t>(sensitivity)]);
        scaled_depth = fx::wadd(fx::wmul(inner, depth), 0x8000) >> 16;
    }

    const auto delta = [&](int bias) {
        if (bias == 0) {
            return 0;
        }
        const int d =
            (pitch_bias_[static_cast<std::size_t>(std::min(64, std::abs(bias)))] * scaled_depth)
            >> 7;
        return bias < 0 ? -d : d;
    };

    std::array<int, 5> biases{};
    for (std::size_t i = 0; i < biases.size(); ++i) {
        biases[i] = fx::i8(raw[0x1B + i]) - 0x40;
    }

    const int main_rate = envelope_->rate_scale(
        (rate_key_follow0_[static_cast<std::size_t>((raw[0x27] * 0x80) + (key & 0x7F))] - 0x80)
            & 0xFF,
        raw[0x29]);
    const int release_rate = envelope_->rate_scale(
        (rate_key_follow1_[static_cast<std::size_t>((raw[0x27] * 0x80) + (key & 0x7F))] - 0x80)
            & 0xFF,
        raw[0x2A]);
    const int velocity_scale = envelope_->level_scale(std::clamp(velocity, 0, 127), raw[0x2C]);

    const auto time = [&](int rate_byte, int rate_multiplier) -> double {
        const int r = rate_byte & 0x7F;
        const int ticks = rate_curve_[static_cast<std::size_t>(r)];
        if (r == 0 || ticks < EnvelopeMachine::minimum_segment_ticks) {
            return 0.0;
        }
        // Both multipliers reach 0xffff, so the product overflows and the mask takes the low word.
        return fx::wmul((fx::wmul(rate_multiplier, ticks) >> 8) & 0xFFFF, velocity_scale) >> 8;
    };

    PitchEnvelope envelope;
    envelope.start = delta(biases[0]);
    // The fourth target is always zero: the envelope returns to the base pitch.
    envelope.targets = {delta(biases[1]), delta(biases[2]), delta(biases[3]), 0};
    envelope.release = delta(biases[4]);
    for (std::size_t i = 0; i < envelope.times.size(); ++i) {
        envelope.times[i] = time(raw[0x20 + i], main_rate);
    }
    envelope.release_ms = time(raw[0x24], release_rate);
    return envelope;
}

int PitchChain::segment_rate_word(double milliseconds) noexcept
{
    return milliseconds < 11 ? 0xFFFF : std::min(0xFFFF, 0xA0000 / static_cast<int>(milliseconds));
}

std::optional<std::vector<double>> PitchChain::envelope_ticks(const PartialParameters& partial,
                                                              int key,
                                                              int velocity,
                                                              double hold_seconds,
                                                              int tick_count) const
{
    std::optional<PitchEnvelopeRunner> runner = create_envelope_runner(partial, key, velocity);
    if (!runner) {
        return std::nullopt;
    }

    std::vector<double> output(static_cast<std::size_t>(std::max(0, tick_count)));
    const auto note_off_tick = static_cast<int>(hold_seconds * 100);

    for (std::size_t i = 0; i < output.size(); ++i) {
        output[i] = runner->tick(static_cast<int>(i) >= note_off_tick);
    }

    return output;
}

int PitchChain::start_jitter_milli_semitones(int depth, std::uint16_t draw) noexcept
{
    // Bit 14 picks the sign, and the magnitude slice is asymmetric: 7 bits positive, 8 negative.
    // The rounding term is added before the shift, not after -- parenthesised because that is the
    // precedence C# gives it too, and the two must not drift.
    //
    // The sign test is spelled as the bit it tests rather than as the C# original's
    // `(short)(draw << 1) >= 0`, and that is not a tidy-up. The two are the same predicate --
    // shifting left by one puts bit 14 into a signed 16-bit sign position -- but **MSVC
    // miscompiles the cast form under optimisation**. At `/O2` it takes the positive branch for
    // every draw below 0x8000, which is bit *15*, so half the negative range is unreachable and
    // the positive range doubles: swept over all 65536 draws at depth 10 the reachable range came
    // out [-100, +100] where it should be [-50, +50]. The same binary built at `/Od` is correct,
    // as is GCC at any level, and `fx::i16` evaluated into a variable one line away is correct
    // even in the miscompiled build -- it is only wrong when it feeds this branch directly.
    // Renders therefore differed by platform on any patch with a non-zero pitch-envelope jitter
    // byte. Testing the bit says the same thing with nothing left to optimise wrongly.
    if ((draw & 0x4000) == 0) {
        return (((((draw & 0x7FFF) >> 7) * depth) + 0x80) >> 8) * 10;
    }
    return ((((static_cast<std::uint16_t>(draw * -2) >> 8) * depth) + 0x80) >> 8) * -10;
}

std::optional<PitchEnvelopeRunner>
PitchChain::create_envelope_runner(const PartialParameters& partial, int key, int velocity) const
{
    const auto raw = partial.raw();

    // **This byte is almost certainly wrong, and correcting it is blocked on a second bug.**
    //
    // `partial_compute_pitch @ 18005fc20` takes the coarse tune from block +0x11 -- which this port
    // agrees with -- and then tests **+0x12** for the jitter draw, not +0x1a. The ROM says the same
    // thing where no render can: +0x12 is non-zero on 223 of the 4,726 partial blocks with nineteen
    // distinct depths, +0x1a on nineteen blocks with two. Nothing recorded why +0x1a was chosen.
    //
    // Switching it makes the authoritative song gate *worse*, which is why it still says +0x1a.
    // `robyn_show_me_love` goes from inside the default 0.01 peak bound to 0.036 out and `rainy`
    // breaches its row, while their level, spectrum and envelope all stay passing. That pattern --
    // one fragile sample-level metric moving on two songs, nothing else -- points at the *draw
    // order*: jitter on 223 partials instead of 19 means many more voices pulling from the one
    // shared generator, so any disagreement about which voice draws when now shows. Fixing the byte
    // needs that settled first, or it trades a known-wrong constant for wrong-sounding songs.
    const int jitter_depth = raw[0x1A];

    // One draw per partial voice, and only when the byte is non-zero -- the engine skips the draw
    // entirely otherwise, so an unaffected patch must not consume from the shared generator.
    const int jitter =
        jitter_depth != 0 ? start_jitter_milli_semitones(jitter_depth, noise_->next()) : 0;
    const bool one_shot = (raw[0x00] & 0x80) != 0;

    std::optional<PitchEnvelope> envelope = envelope_offsets(partial, key, velocity);
    if (!envelope) {
        return jitter == 0 ? std::nullopt : std::optional{PitchEnvelopeRunner::constant(jitter)};
    }

    if (jitter != 0) {
        envelope->start += jitter;
    }

    const bool flat = envelope->start == 0 && envelope->release == 0
                      && std::all_of(envelope->targets.begin(), envelope->targets.end(), [](int t) {
                             return t == 0;
                         });

    return flat ? std::nullopt : std::optional{PitchEnvelopeRunner{*envelope, one_shot}};
}

double PitchChain::ratio(const PartialParameters& /*partial*/,
                         const WaveDescriptor& descriptor,
                         double pitch_milli_semitones) noexcept
{
    const double native = descriptor.native_milli_semitones();
    return std::pow(2.0, (pitch_milli_semitones - native) / 12000.0);
}

double PitchChain::clamp(double milli_semitones) noexcept
{
    return std::clamp(milli_semitones, 0.0, static_cast<double>(max_pitch_milli_semitones));
}

} // namespace ts
