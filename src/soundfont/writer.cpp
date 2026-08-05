#include "tabulasonora/soundfont_writer.hpp"

#include <span>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <stdexcept>

namespace ts::sf2 {
namespace {

/// A growable little-endian byte buffer. RIFF is little-endian throughout and the sizes are only
/// known after the fact, so chunks are built in memory and back-patched.
class Buffer {
public:
    void u8(unsigned value) { bytes_.push_back(static_cast<std::uint8_t>(value)); }

    void u16(unsigned value)
    {
        u8(value & 0xFF);
        u8((value >> 8) & 0xFF);
    }

    void u32(std::uint32_t value)
    {
        u16(value & 0xFFFF);
        u16((value >> 16) & 0xFFFF);
    }

    void tag(const char* four) { bytes_.insert(bytes_.end(), four, four + 4); }

    /// Writes a fixed-width name field, zero-padded and never unterminated.
    ///
    /// `take` is the slice of the name this field carries: SF2 names are 20 bytes, and the `xdta`
    /// mirror carries characters 20 to 39 of the same name in its own 20.
    void name(const std::string& value, std::size_t width, std::size_t from = 0)
    {
        for (std::size_t i = 0; i < width; ++i) {
            const std::size_t index = from + i;
            const bool last = i + 1 == width;
            u8(!last && index < value.size() ? static_cast<unsigned char>(value[index]) : 0);
        }
    }

    void raw(std::span<const std::uint8_t> data)
    {
        bytes_.insert(bytes_.end(), data.begin(), data.end());
    }

    [[nodiscard]] std::size_t size() const noexcept { return bytes_.size(); }
    [[nodiscard]] const std::vector<std::uint8_t>& data() const noexcept { return bytes_; }

    /// Pads to an even length, as every RIFF chunk must be.
    void pad()
    {
        if ((bytes_.size() & 1) != 0) {
            u8(0);
        }
    }

private:
    std::vector<std::uint8_t> bytes_;
};

/// Emits a chunk header and its payload.
void chunk(Buffer& into, const char* id, const Buffer& payload)
{
    into.tag(id);
    into.u32(static_cast<std::uint32_t>(payload.size()));
    into.raw(payload.data());
    into.pad();
}

/// Converts a normalised float to 16-bit, with the clamp the pool may need.
[[nodiscard]] std::int16_t to_pcm16(float value) noexcept
{
    const float scaled = value * 32767.0F;
    return static_cast<std::int16_t>(std::clamp(scaled, -32768.0F, 32767.0F));
}

/// The nine pdta sub-chunks, in both their base and extended forms.
///
/// They are built together because every index in one is a position in another, and the extended
/// chunk must have exactly the same record count as its base or the reader ignores it outright.
struct Pdta {
    Buffer phdr, pbag, pmod, pgen, inst, ibag, imod, igen, shdr;
    Buffer xphdr, xpbag, xpmod, xpgen, xinst, xibag, ximod, xigen, xshdr;

    std::size_t pbag_count = 0;
    std::size_t ibag_count = 0;
    std::size_t pgen_count = 0;
    std::size_t igen_count = 0;
    std::size_t pmod_count = 0;
    std::size_t imod_count = 0;
};

void write_generator(Buffer& into, const Generator& generator)
{
    into.u16(static_cast<std::uint16_t>(generator.oper));
    into.u16(static_cast<std::uint16_t>(generator.amount));
}

void write_modulator(Buffer& into, const Modulator& modulator)
{
    into.u16(modulator.source);
    into.u16(static_cast<std::uint16_t>(modulator.destination));
    into.u16(static_cast<std::uint16_t>(modulator.amount));
    into.u16(modulator.amount_source);
    into.u16(modulator.transform);
}

/// Writes one bag record, splitting each index across the base and extended chunks.
void write_bag(Buffer& base, Buffer& extended, std::size_t gen_index, std::size_t mod_index)
{
    base.u16(gen_index & 0xFFFF);
    base.u16(mod_index & 0xFFFF);
    extended.u16((gen_index >> 16) & 0xFFFF);
    extended.u16((mod_index >> 16) & 0xFFFF);
}

/// Appends a zone's generators and modulators, returning nothing — the caller records the indices
/// before calling, since a bag points at where its zone *starts*.
void append_zone(Pdta& pdta, const Zone& zone, bool preset)
{
    for (const Generator& generator : zone.generators) {
        write_generator(preset ? pdta.pgen : pdta.igen, generator);
    }
    for (const Modulator& modulator : zone.modulators) {
        write_modulator(preset ? pdta.pmod : pdta.imod, modulator);
    }
    (preset ? pdta.pgen_count : pdta.igen_count) += zone.generators.size();
    (preset ? pdta.pmod_count : pdta.imod_count) += zone.modulators.size();
}

} // namespace

WriteReport write(const std::filesystem::path& path, const Bank& bank)
{
    WriteReport report;
    Pdta pdta;

    // ── inst / ibag / igen / imod ────────────────────────────────────────────
    for (const Instrument& instrument : bank.instruments) {
        pdta.inst.name(instrument.name, 20);
        pdta.inst.u16(pdta.ibag_count & 0xFFFF);
        pdta.xinst.name(instrument.name, 20, 20);
        pdta.xinst.u16((pdta.ibag_count >> 16) & 0xFFFF);

        for (const Zone& zone : instrument.zones) {
            write_bag(pdta.ibag, pdta.xibag, pdta.igen_count, pdta.imod_count);
            ++pdta.ibag_count;
            append_zone(pdta, zone, /*preset=*/false);
        }
    }

    // The terminal record. Its bag index is one past the last real zone, which is what bounds the
    // final instrument -- and the reader reads `ibags[start + 1]` for a global zone, so the
    // terminal bag has to exist too.
    pdta.inst.name("EOI", 20);
    pdta.inst.u16(pdta.ibag_count & 0xFFFF);
    pdta.xinst.name("EOI", 20, 20);
    pdta.xinst.u16((pdta.ibag_count >> 16) & 0xFFFF);

    write_bag(pdta.ibag, pdta.xibag, pdta.igen_count, pdta.imod_count);
    ++pdta.ibag_count;

    // ── phdr / pbag / pgen / pmod ────────────────────────────────────────────
    for (const Preset& preset : bank.presets) {
        pdta.phdr.name(preset.name, 20);
        pdta.phdr.u16(preset.program);
        pdta.phdr.u16(preset.bank);
        pdta.phdr.u16(pdta.pbag_count & 0xFFFF);
        pdta.phdr.u32(0); // library
        pdta.phdr.u32(0); // genre
        pdta.phdr.u32(0); // morphology

        pdta.xphdr.name(preset.name, 20, 20);
        pdta.xphdr.u16(0); // program, unused in the mirror
        pdta.xphdr.u16(0); // bank, unused in the mirror
        pdta.xphdr.u16((pdta.pbag_count >> 16) & 0xFFFF);
        pdta.xphdr.u32(0);
        pdta.xphdr.u32(0);
        pdta.xphdr.u32(0);

        for (const Zone& zone : preset.zones) {
            write_bag(pdta.pbag, pdta.xpbag, pdta.pgen_count, pdta.pmod_count);
            ++pdta.pbag_count;
            append_zone(pdta, zone, /*preset=*/true);
        }
    }

    pdta.phdr.name("EOP", 20);
    pdta.phdr.u16(0);
    pdta.phdr.u16(0);
    pdta.phdr.u16(pdta.pbag_count & 0xFFFF);
    pdta.phdr.u32(0);
    pdta.phdr.u32(0);
    pdta.phdr.u32(0);

    pdta.xphdr.name("EOP", 20, 20);
    pdta.xphdr.u16(0);
    pdta.xphdr.u16(0);
    pdta.xphdr.u16((pdta.pbag_count >> 16) & 0xFFFF);
    pdta.xphdr.u32(0);
    pdta.xphdr.u32(0);
    pdta.xphdr.u32(0);

    write_bag(pdta.pbag, pdta.xpbag, pdta.pgen_count, pdta.pmod_count);
    ++pdta.pbag_count;

    // A terminal generator and modulator record closes each list, as the specification asks.
    write_generator(pdta.pgen, Generator{});
    write_generator(pdta.igen, Generator{});
    write_modulator(pdta.pmod, Modulator{});
    write_modulator(pdta.imod, Modulator{});
    // The generator and modulator mirrors carry nothing -- a generator record is an operator and
    // an amount, with no index to extend. They are emitted zero-filled at exactly the base record
    // count because the reader accepts a mirror only when the counts match, and a mirror that is
    // present but the wrong length is worse than one that is absent.
    for (std::size_t i = 0; i <= pdta.pgen_count; ++i) {
        write_generator(pdta.xpgen, Generator{});
    }
    for (std::size_t i = 0; i <= pdta.igen_count; ++i) {
        write_generator(pdta.xigen, Generator{});
    }
    for (std::size_t i = 0; i <= pdta.pmod_count; ++i) {
        write_modulator(pdta.xpmod, Modulator{});
    }
    for (std::size_t i = 0; i <= pdta.imod_count; ++i) {
        write_modulator(pdta.ximod, Modulator{});
    }

    // ── shdr ─────────────────────────────────────────────────────────────────
    for (const Sample& sample : bank.samples) {
        pdta.shdr.name(sample.name, 20);
        pdta.shdr.u32(sample.start);
        pdta.shdr.u32(sample.end);
        pdta.shdr.u32(sample.loop_start);
        pdta.shdr.u32(sample.loop_end);
        pdta.shdr.u32(sample.sample_rate);
        pdta.shdr.u8(sample.original_key);
        pdta.shdr.u8(static_cast<unsigned char>(sample.correction));
        pdta.shdr.u16(sample.link);
        pdta.shdr.u16(sample.type);

        // The mirror carries only the name's second half; every other field is read from the base.
        pdta.xshdr.name(sample.name, 20, 20);
        for (int i = 0; i < 26; ++i) {
            pdta.xshdr.u8(0);
        }
    }

    pdta.shdr.name("EOS", 20);
    for (int i = 0; i < 5; ++i) {
        pdta.shdr.u32(0);
    }
    pdta.shdr.u8(0);
    pdta.shdr.u8(0);
    pdta.shdr.u16(0);
    pdta.shdr.u16(0);

    pdta.xshdr.name("EOS", 20, 20);
    for (int i = 0; i < 26; ++i) {
        pdta.xshdr.u8(0);
    }

    // ── INFO ─────────────────────────────────────────────────────────────────
    Buffer info;
    info.tag("INFO");

    Buffer ifil;
    ifil.u16(2);
    ifil.u16(4); // 2.04, the version the xdta extension belongs to
    chunk(info, "ifil", ifil);

    Buffer isng;
    isng.name(bank.engine, ((bank.engine.size() + 2) / 2) * 2);
    chunk(info, "isng", isng);

    Buffer inam;
    inam.name(bank.name, ((bank.name.size() + 2) / 2) * 2);
    chunk(info, "INAM", inam);

    if (!bank.software.empty()) {
        Buffer isft;
        isft.name(bank.software, ((bank.software.size() + 2) / 2) * 2);
        chunk(info, "ISFT", isft);
    }

    if (!bank.comment.empty()) {
        Buffer icmt;
        icmt.name(bank.comment, ((bank.comment.size() + 2) / 2) * 2);
        chunk(info, "ICMT", icmt);
    }

    if (!bank.default_modulators.empty()) {
        Buffer dmod;
        // No terminal record here, unlike pmod and imod: the reader takes DMOD's modulator count
        // as the chunk size divided by ten, so a terminal would be read as one more modulator --
        // a live one, pointing at generator zero.
        for (const Modulator& modulator : bank.default_modulators) {
            write_modulator(dmod, modulator);
        }
        chunk(info, "DMOD", dmod);
    }

    // The xdta LIST lives inside INFO, not beside it -- the reader looks for it while walking the
    // INFO sub-chunks. Its members are named exactly like pdta's, and each must have the same
    // record count as its base or the reader silently ignores the whole mirror.
    {
        Buffer xdta;
        xdta.tag("xdta");
        chunk(xdta, "phdr", pdta.xphdr);
        chunk(xdta, "pbag", pdta.xpbag);
        chunk(xdta, "pmod", pdta.xpmod);
        chunk(xdta, "pgen", pdta.xpgen);
        chunk(xdta, "inst", pdta.xinst);
        chunk(xdta, "ibag", pdta.xibag);
        chunk(xdta, "imod", pdta.ximod);
        chunk(xdta, "igen", pdta.xigen);
        chunk(xdta, "shdr", pdta.xshdr);
        chunk(info, "LIST", xdta);
    }

    // ── sdta ─────────────────────────────────────────────────────────────────
    Buffer sdta;
    sdta.tag("sdta");
    {
        Buffer smpl;
        for (const float sample : bank.pool) {
            smpl.u16(static_cast<std::uint16_t>(to_pcm16(sample)));
        }
        report.pcm_bytes = static_cast<std::int64_t>(smpl.size());
        chunk(sdta, "smpl", smpl);
    }

    // ── pdta ─────────────────────────────────────────────────────────────────
    Buffer pdta_list;
    pdta_list.tag("pdta");
    chunk(pdta_list, "phdr", pdta.phdr);
    chunk(pdta_list, "pbag", pdta.pbag);
    chunk(pdta_list, "pmod", pdta.pmod);
    chunk(pdta_list, "pgen", pdta.pgen);
    chunk(pdta_list, "inst", pdta.inst);
    chunk(pdta_list, "ibag", pdta.ibag);
    chunk(pdta_list, "imod", pdta.imod);
    chunk(pdta_list, "igen", pdta.igen);
    chunk(pdta_list, "shdr", pdta.shdr);

    // ── RIFF ─────────────────────────────────────────────────────────────────
    Buffer body;
    body.tag("sfbk");
    chunk(body, "LIST", info);
    chunk(body, "LIST", sdta);
    chunk(body, "LIST", pdta_list);

    Buffer file;
    chunk(file, "RIFF", body);

    std::FILE* out = std::fopen(path.string().c_str(), "wb");
    if (out == nullptr) {
        throw std::runtime_error("Cannot open '" + path.string() + "' for writing.");
    }
    const std::size_t written =
        std::fwrite(file.data().data(), 1, file.data().size(), out);
    const bool ok = written == file.data().size();
    std::fclose(out);
    if (!ok) {
        throw std::runtime_error("Short write to '" + path.string() + "'.");
    }

    report.sample_count = bank.samples.size();
    report.instrument_count = bank.instruments.size();
    report.preset_count = bank.presets.size();
    report.ibag_count = pdta.ibag_count;
    report.pbag_count = pdta.pbag_count;
    report.igen_count = pdta.igen_count;
    report.pgen_count = pdta.pgen_count;
    report.file_bytes = static_cast<std::int64_t>(file.size());
    return report;
}

} // namespace ts::sf2
