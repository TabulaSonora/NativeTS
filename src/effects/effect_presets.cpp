#include "tabulasonora/effect_presets.hpp"

#include "rom/embedded_assets.hpp"

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <fstream>
#include <iterator>
#include <optional>
#include <stdexcept>

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

} // namespace

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
    // The compiled-in set is what makes the engine work out of the box; the environment variable is
    // for regenerating from a different DLL and checking the two agree.
    static const EffectPresets instance = [] {
        if (const std::optional<std::string> external = presets_from_environment()) {
            return parse(*external);
        }
        return parse(assets::presets_json());
    }();
    return instance;
}

} // namespace ts
