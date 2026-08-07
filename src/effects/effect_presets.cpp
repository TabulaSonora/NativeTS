#include "tabulasonora/effect_presets.hpp"

#include "tabulasonora/effect_programmer.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <utility>

namespace ts {
namespace {

using nlohmann::json;

[[nodiscard]] const json& require(const json& object, const char* name)
{
    const auto found = object.find(name);
    if (found == object.end()) {
        throw std::runtime_error(std::string{"presets.json is missing '"} + name + "'.");
    }
    return *found;
}

[[nodiscard]] AllpassStage read_allpass(const json& node)
{
    return AllpassStage{
        .write_tap = require(node, "writeTap").get<int>(),
        .read_tap = require(node, "readTap").get<int>(),
        .coef_a = require(node, "coefA").get<double>(),
        .coef_b = require(node, "coefB").get<double>(),
    };
}

/// Flattens a tank's name-keyed tap object into fixed fields. See `TankTaps`.
[[nodiscard]] ReverbTank read_tank(const json& node)
{
    const json& taps = require(node, "taps");
    return ReverbTank{
        .taps =
            TankTaps{
                .tap10 = require(taps, "tap10").get<int>(),
                .tap14 = require(taps, "tap14").get<int>(),
                .tap18 = require(taps, "tap18").get<int>(),
                .tap1c = require(taps, "tap1C").get<int>(),
                .tap20 = require(taps, "tap20").get<int>(),
                .tap24 = require(taps, "tap24").get<int>(),
                .tap28 = require(taps, "tap28").get<int>(),
                .tap2c = require(taps, "tap2C").get<int>(),
            },
        .coef_a = require(node, "coefA").get<double>(),
        .coef_b = require(node, "coefB").get<double>(),
    };
}

[[nodiscard]] ReverbPreset read_reverb_preset(const json& node)
{
    ReverbPreset preset;

    const json& diffusers = require(node, "diffusers");
    if (diffusers.size() != preset.diffusers.size()) {
        throw std::runtime_error("A reverb preset needs exactly four diffusers.");
    }
    for (std::size_t i = 0; i < preset.diffusers.size(); ++i) {
        preset.diffusers[i] = read_allpass(diffusers[i]);
    }

    preset.tank_a = read_tank(require(node, "tankA"));
    preset.tank_b = read_tank(require(node, "tankB"));

    const json& nested = require(node, "tankAllpasses");
    preset.tank_allpasses = TankAllpasses{
        .a0 = read_allpass(require(nested, "A0")),
        .a1 = read_allpass(require(nested, "A1")),
        .b0 = read_allpass(require(nested, "B0")),
        .b1 = read_allpass(require(nested, "B1")),
    };

    preset.injection_tap = require(node, "injectionTap").get<int>();
    preset.damp_feedback = require(node, "dampFeedback").get<double>();
    preset.damp_input = require(node, "dampInput").get<double>();
    preset.gain_input = require(node, "gainInput").get<double>();
    preset.gain_injection = require(node, "gainInjection").get<double>();
    preset.gain_feedback = require(node, "gainFeedback").get<double>();
    preset.gain_output = require(node, "gainOutput").get<double>();
    return preset;
}

[[nodiscard]] ChorusPreset read_chorus_preset(const json& node)
{
    return ChorusPreset{
        .lfo_increment = require(node, "lfoIncrement").get<int>(),
        .lpf_a = require(node, "lpfA").get<double>(),
        .lpf_b = require(node, "lpfB").get<double>(),
        .tap1_depth = require(node, "tap1Depth").get<int>(),
        .tap1_base = require(node, "tap1Base").get<int>(),
        .tap2_depth = require(node, "tap2Depth").get<int>(),
        .tap2_base = require(node, "tap2Base").get<int>(),
        .feedback = require(node, "feedback").get<double>(),
        .gain_write = require(node, "gainWrite").get<double>(),
        .gain_tap = require(node, "gainTap").get<double>(),
        // Optional: the two cross-feeds are zero in every stored macro, so a preset file written
        // before they were decoded is still a complete description of one.
        .gain_to_reverb = node.value("gainToReverb", 0.0),
        .gain_to_delay = node.value("gainToDelay", 0.0),
    };
}

/// Reads presets from `TABULASONORA_PRESETS` when it names a readable file.
[[nodiscard]] std::optional<std::string> presets_from_environment()
{
    const char* path = std::getenv(std::string{EffectPresets::path_variable}.c_str());
    if (path == nullptr || *path == '\0') {
        return std::nullopt;
    }

    std::ifstream stream{path};
    if (!stream) {
        return std::nullopt;
    }
    return std::string{std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
}

std::mutex& gate()
{
    static std::mutex instance;
    return instance;
}

/// The presets in force, or empty until one is supplied or computed.
///
/// A pointer rather than a value so "not set yet" is distinguishable from a default-constructed
/// set. `defaults` hands out a reference into whichever set is current, so a set that has ever been
/// current is never destroyed -- `retired` keeps it alive. That is a bounded cost: nothing replaces
/// the presets in normal use, and a host that pins its own does so once at start-up.
std::shared_ptr<const EffectPresets>& current()
{
    static std::shared_ptr<const EffectPresets> instance;
    return instance;
}

std::vector<std::shared_ptr<const EffectPresets>>& retired()
{
    static std::vector<std::shared_ptr<const EffectPresets>> instance;
    return instance;
}

/// Installs a set, keeping any previous one alive for references already handed out.
void install(std::shared_ptr<const EffectPresets> presets)
{
    if (current()) {
        retired().push_back(current());
    }
    current() = std::move(presets);
}

/// Resolves the overrides a host may have pinned, in precedence order.
[[nodiscard]] std::shared_ptr<const EffectPresets> locate()
{
    if (const std::optional<std::string> external = presets_from_environment()) {
        return std::make_shared<const EffectPresets>(EffectPresets::parse(*external));
    }
    return nullptr;
}

} // namespace

EffectPresets EffectPresets::from_parts(ReverbPresets reverb,
                                        ChorusPresets chorus,
                                        DelayPresets delay,
                                        EqPresets eq)
{
    EffectPresets presets;
    presets.reverb_ = std::move(reverb);
    presets.chorus_ = std::move(chorus);
    presets.delay_ = std::move(delay);
    presets.eq_ = eq;

    // A default-constructed band is unity, which is indistinguishable from a real flat setting, so
    // presence is tracked rather than inferred: only a set that came from a DLL has EQ.
    presets.has_eq_ = eq.low[0][0].a1 != 0.0 || eq.high[0][0].a1 != 0.0;
    return presets;
}

const EqBand& EqPresets::low_band(int frequency, int gain) const noexcept
{
    return low[static_cast<std::size_t>(std::clamp(frequency, 0, 1))]
              [static_cast<std::size_t>(std::clamp(gain - gain_base, 0, gain_count - 1))];
}

const EqBand& EqPresets::high_band(int frequency, int gain) const noexcept
{
    return high[static_cast<std::size_t>(std::clamp(frequency, 0, 1))]
               [static_cast<std::size_t>(std::clamp(gain - gain_base, 0, gain_count - 1))];
}

void EffectPresets::use(EffectPresets presets)
{
    const std::lock_guard<std::mutex> lock{gate()};
    install(std::make_shared<const EffectPresets>(std::move(presets)));
}

void EffectPresets::ensure_from(const RomImage& rom)
{
    const std::lock_guard<std::mutex> lock{gate()};
    if (current()) {
        return;
    }
    if (std::shared_ptr<const EffectPresets> pinned = locate()) {
        install(std::move(pinned));
        return;
    }
    install(std::make_shared<const EffectPresets>(EffectProgrammer::compute(rom)));
}

bool EffectPresets::available()
{
    const std::lock_guard<std::mutex> lock{gate()};
    if (!current()) {
        if (std::shared_ptr<const EffectPresets> pinned = locate()) {
            install(std::move(pinned));
        }
    }
    return current() != nullptr;
}

EffectPresets EffectPresets::parse(std::string_view text)
{
    const json root = json::parse(text);
    EffectPresets presets;

    const json& reverb = require(root, "reverb");
    presets.reverb_.type_names = require(reverb, "typeNames").get<std::vector<std::string>>();
    presets.reverb_.defaults = read_reverb_preset(require(reverb, "default"));
    for (const json& node : require(reverb, "types")) {
        presets.reverb_.types.push_back(read_reverb_preset(node));
    }

    const json& chorus = require(root, "chorus");
    presets.chorus_.type_names = require(chorus, "typeNames").get<std::vector<std::string>>();
    presets.chorus_.defaults = read_chorus_preset(require(chorus, "default"));
    for (const json& node : require(chorus, "types")) {
        presets.chorus_.types.push_back(read_chorus_preset(node));
    }

    const json& delay = require(root, "delay");
    presets.delay_.type_names = require(delay, "typeNames").get<std::vector<std::string>>();
    presets.delay_.time_milliseconds =
        require(delay, "timeMilliseconds").get<std::vector<double>>();
    presets.delay_.ratio_percent = require(delay, "ratioPercent").get<std::vector<double>>();
    for (const json& row : require(delay, "rawPresets")) {
        std::array<int, 10> parameters{};
        if (row.size() != parameters.size()) {
            throw std::runtime_error("A delay preset row needs exactly ten parameters.");
        }
        for (std::size_t i = 0; i < parameters.size(); ++i) {
            parameters[i] = row[i].get<int>();
        }
        presets.delay_.raw_presets.push_back(parameters);
    }

    return presets;
}

const EffectPresets& EffectPresets::defaults()
{
    const std::lock_guard<std::mutex> lock{gate()};
    if (!current()) {
        if (std::shared_ptr<const EffectPresets> pinned = locate()) {
            install(std::move(pinned));
        }
    }
    if (!current()) {
        throw std::runtime_error(
            "Effect presets are not set. They are computed from SCCore.dll when a NoteRenderer "
            "opens it, so build the renderer before the first effect render -- or call "
            "EffectPresets::ensure_from(rom), point TABULASONORA_PRESETS at a baked file, or call "
            "EffectPresets::use().");
    }

    // Safe to hand out: `install` retires rather than destroys, so this reference outlives any
    // later replacement.
    return *current();
}

} // namespace ts
