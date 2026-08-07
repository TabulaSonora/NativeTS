#pragma once

#include "tabulasonora/effect_presets.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace ts {

/// A stereo send effect fed from one mono bus.
class Effect {
public:
    virtual ~Effect() = default;

    /// Processes a block.
    virtual void
    process(std::span<const float> input, std::span<float> left, std::span<float> right) = 0;

    /// Clears all internal state.
    virtual void reset() = 0;
};

/// The engine's DC blocker: one design, instantiated once on each effect input.
///
/// A 20 Hz one-pole highpass. The coefficient 0.99804 is the only float literal between 0.85
/// and 1.0 anywhere in the binary, which is how it was established that no second design exists. It
/// is needed because the sample codec's predictor is a pure integrator that drifts on every loop
/// pass.
class DcBlocker {
public:
    static constexpr double input_coefficient = 0.99804;
    static constexpr double state_coefficient = -0.003919;

    void reset() noexcept { state_ = 0.0; }

    [[nodiscard]] double process(double input) noexcept
    {
        const double output = (input * input_coefficient) + state_;
        state_ += output * state_coefficient;
        return output;
    }

private:
    double state_ = 0.0;
};

/// The GS reverb: a Schroeder/Dattorro tank transcribed from the engine.
///
/// Input conditioner, then four series allpass diffusers, then two parallel tanks each with a
/// damping lowpass and two nested allpasses, then eight output taps. All eight GS types share this
/// topology — Delay and PanDelay simply zero the diffusers and collapse to a single long tap.
///
/// The write cursor **decrements**, so every tap offset is added to it.
class Reverb final : public Effect {
public:
    /// Size of the shared delay ring.
    static constexpr int ring_size = 65536;

    explicit Reverb(const ReverbPreset& preset) : preset_(preset) {}

    /// Creates a reverb for a GS type 0–7, or the default when none is given.
    [[nodiscard]] static Reverb for_type(std::optional<int> type);

    /// The send-bus gain for CC#91. Linear in the controller, unlike the volume law.
    [[nodiscard]] static double send_gain(double controller) noexcept
    {
        return ReverbPresets::send_at_full_scale * (controller / 127.0);
    }

    void reset() override;

    void
    process(std::span<const float> input, std::span<float> left, std::span<float> right) override;

    /// Processes a block with the other two networks feeding into it; either span may be empty.
    ///
    /// The reverb is last in the module's chain for a reason: `fx_reverb_process` opens by summing
    /// **three** buffers, not one -- its own send bus, the chorus's send-to-reverb scratch, and the
    /// delay's. Both cross-feeds are produced by networks that already ran this block, so this is a
    /// within-block route and not a one-block-late approximation of one.
    void process(std::span<const float> input,
                 std::span<const float> from_chorus,
                 std::span<const float> from_delay,
                 std::span<float> left,
                 std::span<float> right);

private:
    static constexpr int ring_mask = ring_size - 1;
    static constexpr int input_tap = 0x1000;

    void run_tank(const TankTaps& taps,
                  const ReverbTank& tank,
                  const AllpassStage& first,
                  const AllpassStage& second,
                  double carry,
                  double feedback,
                  double& state) noexcept;

    ReverbPreset preset_;
    std::vector<double> ring_ = std::vector<double>(ring_size, 0.0);

    double dc_state_ = 0.0;
    double damp_ = 0.0;
    double tank_state_a_ = 0.0;
    double tank_state_b_ = 0.0;
    int write_cursor_ = 0;
};

/// The GS chorus: a modulated delay line with two anti-phase taps.
///
/// A 24-bit signed sawtooth drives a triangle from its magnitude; the two taps read 180° apart,
/// which is what makes the output stereo from a mono send. The engine has a companion right-hand
/// stage, but it is gated off for every GS type, so the left stage alone is correct.
class Chorus final : public Effect {
public:
    /// Size of the delay ring.
    static constexpr int ring_size = 65536;

    explicit Chorus(const ChorusPreset& preset) : preset_(preset) {}

    /// Creates a chorus for a GS type 0–7, or the default when none is given.
    [[nodiscard]] static Chorus for_type(std::optional<int> type);

    /// The send-bus gain for CC#93.
    [[nodiscard]] static double send_gain(double controller) noexcept
    {
        return ChorusPresets::send_at_full_scale * (controller / 127.0);
    }

    void reset() override;

    void
    process(std::span<const float> input, std::span<float> left, std::span<float> right) override;

    /// Processes a block, also writing the two routes out of the chorus.
    ///
    /// `to_reverb` and `to_delay` receive the **mono sum of the two taps**, taken before the return
    /// level and scaled by their own send gains -- so the chorus can be loud in the mix and feed
    /// nothing onward, or the reverse. Either span may be empty. See `ChorusPreset::gain_to_reverb`
    /// for why both gains are zero unless a stream sends `40 01 3F` / `40 01 40`.
    void process(std::span<const float> input,
                 std::span<float> left,
                 std::span<float> right,
                 std::span<float> to_reverb,
                 std::span<float> to_delay);

    /// Places the LFO accumulator, which otherwise free-runs from zero.
    ///
    /// The module's chorus LFO is a 24-bit accumulator advanced by `rate << 6` every sample, and
    /// **nothing resets it** — not a GS reset, not a macro change. So its phase when a song starts
    /// is a function of how long that engine has been running, and two engines that agree on every
    /// note still place their wet differently unless they agree on this too.
    ///
    /// Not cosmetic. Rendering `panwet.mid` through the module at three warm-up lengths — changing
    /// nothing but this accumulator — moves the worst window of the stereo balance measure by up to
    /// **0.50**, which is larger than the gap that measure reports between this engine and the
    /// module. A comparison that does not align the phase cannot see wet *placement* at all.
    void set_phase(int phase) noexcept { phase_ = phase & (phase_sign - 1); }

    /// Where the accumulator stands.
    [[nodiscard]] int lfo_phase() const noexcept { return phase_; }

private:
    static constexpr int ring_mask = ring_size - 1;
    static constexpr int phase_sign = 1 << 24;
    static constexpr int phase_half = 1 << 23;

    [[nodiscard]] double read_tap(int depth, int base_delay, int triangle) const noexcept;

    ChorusPreset preset_;
    std::vector<double> ring_ = std::vector<double>(ring_size, 0.0);

    int phase_ = 0;
    double dc_state_ = 0.0;
    double lowpass_ = 0.0;
    double feedback_ = 0.0;
    int write_cursor_ = 0;
};

/// Compiled coefficients for one GS delay type.
struct DelayParameters {
    int centre_samples = 1;
    int left_samples = 1;
    int right_samples = 1;
    double centre_gain = 0.0;
    double left_gain = 0.0;
    double right_gain = 0.0;
    double feedback = 0.0;
    int pre_low_pass = 0;

    /// The delay's own route into the reverb, `40 01 5A` -- the third cross-feed, and the only one
    /// any stored preset turns on: type 8 carries 36, the rest zero. Quantised over 128.
    ///
    /// The module scales the *pre-return-level* tap sum by this, so the delay can be inaudible in
    /// the mix and still be reverberated. Nothing here carries the return level: it is the mixer's
    /// ramp, exactly as the chorus's and the reverb's are.
    double send_to_reverb = 0.0;
};

/// The GS system delay: three taps off one feedback line, behind a fixed input pre-delay.
class SystemDelay final : public Effect {
public:
    /// Size of the delay ring.
    static constexpr int ring_size = 1 << 17;

    explicit SystemDelay(const DelayParameters& parameters);

    /// Creates a delay for a GS type 0–9.
    [[nodiscard]] static SystemDelay for_type(int type);

    /// The send-bus gain for a part's delay send, 0–127.
    ///
    /// There is no Control Change for this: the delay send is reachable only over SysEx.
    [[nodiscard]] static double send_gain(double send) noexcept
    {
        return DelayPresets::send_at_full_scale * (send / 127.0);
    }

    /// Converts a raw time value, 1–115, to milliseconds.
    [[nodiscard]] static double time_milliseconds(int raw);

    /// Converts a raw ratio value, 1–120, to a percentage.
    [[nodiscard]] static double ratio_percent(int raw);

    /// Compiles ten raw GS parameters into DSP coefficients.
    [[nodiscard]] static DelayParameters compile(std::span<const int> raw, int sample_rate = 32000);

    void reset() override;

    void
    process(std::span<const float> input, std::span<float> left, std::span<float> right) override;

    /// Processes a block, also writing the delay's route into the reverb.
    ///
    /// `to_reverb` receives `left_gain·l + right_gain·r + centre_gain·centre` -- the centre tap
    /// counted **once**, not once per side as summing the two outputs would -- taken before the
    /// return level and scaled by `send_to_reverb`. May be empty.
    void process(std::span<const float> input,
                 std::span<float> left,
                 std::span<float> right,
                 std::span<float> to_reverb);

private:
    static constexpr int ring_mask = ring_size - 1;

    DelayParameters parameters_;
    std::vector<double> ring_ = std::vector<double>(ring_size, 0.0);

    double pre_low_pass_coefficient_ = 0.0;
    double dc_state_ = 0.0;
    double feedback_hold_ = 0.0;
    double lowpass_ = 0.0;
    int write_cursor_ = 0;
};

} // namespace ts
