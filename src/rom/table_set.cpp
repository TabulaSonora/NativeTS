#include "tabulasonora/table_set.hpp"

#include <bit>
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <utility>

namespace ts {
namespace {

/// The table data is little-endian regardless of the host.
///
/// Only the wholesale reinterpretation below actually cares -- every individual 16-bit field read
/// elsewhere in the engine is written as an explicit byte pair and is endian-neutral. Refusing up
/// front is still better than silently byte-swapping an entire curve.
void require_little_endian()
{
    static_assert(std::endian::native == std::endian::little
                      || std::endian::native == std::endian::big,
                  "Mixed-endian hosts are not supported.");
    if constexpr (std::endian::native != std::endian::little) {
        throw std::runtime_error(
            "SCCore.dll table data is little-endian; this engine requires a little-endian host.");
    }
}

[[nodiscard]] std::vector<std::uint8_t> read_whole_file(const std::filesystem::path& path)
{
    std::ifstream stream{path, std::ios::binary};
    if (!stream) {
        throw std::runtime_error("Cannot read '" + path.string() + "'.");
    }
    return std::vector<std::uint8_t>{std::istreambuf_iterator<char>{stream},
                                     std::istreambuf_iterator<char>{}};
}

} // namespace

template<typename T>
std::vector<T> TableSet::reinterpret_as(std::span<const std::uint8_t> bytes)
{
    std::vector<T> result(bytes.size() / sizeof(T));
    if (!result.empty()) {
        std::memcpy(result.data(), bytes.data(), result.size() * sizeof(T));
    }
    return result;
}

TableSet::TableSet(std::unordered_map<std::string, std::vector<std::uint8_t>> raw,
                   const TableManifest& manifest)
    : raw_(std::move(raw)), manifest_(&manifest)
{
    bind();
}

TableSet::TableSet(TableSet&&) noexcept = default;
TableSet& TableSet::operator=(TableSet&&) noexcept = default;
TableSet::~TableSet() = default;

TableSet TableSet::from_rom(const RomImage& rom)
{
    require_little_endian();

    std::unordered_map<std::string, std::vector<std::uint8_t>> raw;
    for (const TableEntry& entry : rom.manifest().cached_tables()) {
        raw.emplace(entry.name, rom.read(entry));
    }
    return TableSet{std::move(raw), rom.manifest()};
}

TableSet TableSet::from_cache_directory(const std::filesystem::path& directory,
                                        const TableManifest* manifest)
{
    require_little_endian();
    const TableManifest& map = manifest != nullptr ? *manifest : TableManifest::defaults();

    std::unordered_map<std::string, std::vector<std::uint8_t>> raw;
    for (const TableEntry& entry : map.cached_tables()) {
        const std::filesystem::path path = directory / entry.name;
        if (!std::filesystem::exists(path)) {
            throw std::runtime_error("Table cache '" + entry.name + "' not found in '"
                                     + directory.string() + "'. Extract it from SCCore.dll first.");
        }

        std::vector<std::uint8_t> bytes = read_whole_file(path);
        if (bytes.size() != static_cast<std::size_t>(entry.size)) {
            throw std::runtime_error("Table cache '" + entry.name + "' is "
                                     + std::to_string(bytes.size()) + " bytes; expected "
                                     + std::to_string(entry.size)
                                     + ". Re-extract: the manifest is ahead of this cache.");
        }
        raw.emplace(entry.name, std::move(bytes));
    }
    return TableSet{std::move(raw), map};
}

std::span<const std::uint8_t> TableSet::raw(std::string_view name) const
{
    const auto found = raw_.find(std::string{name});
    if (found == raw_.end()) {
        throw std::out_of_range("No table named '" + std::string{name} + "' was loaded.");
    }
    return found->second;
}

/// Binds every accessor to its table.
///
/// Generated from the upstream C# constructor so the name-to-element-type mapping cannot drift:
/// getting one of these widths wrong reads a real table at the wrong stride, which produces
/// plausible numbers and wrong sound.
void TableSet::bind()
{
    amp_curve_hi_ = reinterpret_as<std::uint16_t>(raw(names::amp_curve_hi));
    amp_curve_lo_ = reinterpret_as<std::uint16_t>(raw(names::amp_curve_lo));
    tvf_env_depth_ = raw(names::tvf_env_depth);
    filter_type_coef_ = reinterpret_as<std::uint32_t>(raw(names::filter_type_coef));
    level_curve_ = reinterpret_as<std::uint16_t>(raw(names::level_curve));
    pitch_bias_ = raw(names::pitch_bias);
    pitch_depth_vs_ = reinterpret_as<std::uint16_t>(raw(names::pitch_depth_vs));
    pitch_env_ = raw(names::pitch_env);
    env_rate_out_ = reinterpret_as<std::uint16_t>(raw(names::env_rate_out));
    reso_curve_ = reinterpret_as<std::int16_t>(raw(names::reso_curve));
    env_scale_curve_ = raw(names::env_scale_curve);
    rate_curve_ = reinterpret_as<std::uint16_t>(raw(names::rate_curve));
    vel_curve_ = raw(names::vel_curve);
    vel_sens_ = raw(names::vel_sens);
    vel_xfade_ = raw(names::vel_xfade);
    env_shape_ = reinterpret_as<std::uint16_t>(raw(names::env_shape));
    env_start_phase_ = reinterpret_as<std::uint16_t>(raw(names::env_start_phase));
    interp_coef_ = reinterpret_as<float>(raw(names::interp_coef));
    kf_pitch_ = reinterpret_as<std::int16_t>(raw(names::kf_pitch));
    kf_pitch_rate0_ = raw(names::kf_pitch_rate0);
    kf_pitch_rate1_ = raw(names::kf_pitch_rate1);
    kf_tva_level_ = raw(names::kf_tva_level);
    kf_tva_level2_ = raw(names::kf_tva_level2);
    kf_tva_rate0_ = raw(names::kf_tva_rate0);
    kf_tva_rate1_ = raw(names::kf_tva_rate1);
    kf_tvf_env_ = reinterpret_as<std::int16_t>(raw(names::kf_tvf_env));
    kf_tvf_rate_ = raw(names::kf_tvf_rate);
    layered_ = raw(names::layered);
    lfo_cents_ = reinterpret_as<std::uint16_t>(raw(names::lfo_cents));
    lfo_delay_ = reinterpret_as<std::uint16_t>(raw(names::lfo_delay));
    lfo_rate_ = reinterpret_as<std::uint16_t>(raw(names::lfo_rate));
    portamento_step_ = reinterpret_as<std::uint16_t>(raw(names::portamento_step));
    lfo_wave_ = raw(names::lfo_wave);
    lfo_wave_bank_ = reinterpret_as<std::int16_t>(raw(names::lfo_wave_bank));
    lfo_wave_map_ = raw(names::lfo_wave_map);
    dir_lut1_ = raw(names::dir_lut1);
    dir_lut2_ = raw(names::dir_lut2);
    dir_lut3_ = reinterpret_as<std::uint16_t>(raw(names::dir_lut3));
    multisample_ = raw(names::multisample);
    pan_ = raw(names::pan);
    ramp_divider_ = raw(names::ramp_divider);
    ramp_exp_ = reinterpret_as<std::int32_t>(raw(names::ramp_exp));
    ramp_flagword_ = raw(names::ramp_flagword);
    tone_ = raw(names::tone);
    tvf_cutoff_ceil_ = reinterpret_as<std::uint16_t>(raw(names::tvf_cutoff_ceil));
    tvf_q_lp_ = reinterpret_as<std::uint16_t>(raw(names::tvf_q_lp));
    tvf_q_t6_ = reinterpret_as<std::uint16_t>(raw(names::tvf_q_t6));
    tvf_cutoff_warp_ = reinterpret_as<std::uint16_t>(raw(names::tvf_cutoff_warp));
    wavedesc_ = raw(names::wavedesc);
}

} // namespace ts
