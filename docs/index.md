# Tabula Sonora {#mainpage}

A native C++20 implementation of the Roland Sound Canvas VA synth voice. It reads the wave ROM and
synth tables out of `SCCore.dll` **as a data file** — the DLL is never loaded as code — so the
engine is portable and has no Windows dependency at all.

MIDI file in, audio out, on one core.

```cpp
#include <tabulasonora/rom_image.hpp>
#include <tabulasonora/note_renderer.hpp>
#include <tabulasonora/sequence_renderer.hpp>

auto rom = ts::RomImage::open("SCCore.dll");
ts::NoteRenderer notes(rom);
ts::SequenceRenderer renderer(notes);

auto result = renderer.render_file("song.mid", {
    .map = ts::ToneMap::sc55,   // the vintage to resolve programs against
    .tail_seconds = 2.2,
});
```

`result.left` and `result.right` are the finished mix at ts::NoteRenderer::sample_rate.

## Where to start

| | |
|---|---|
| \subpage getting-started | Supplying the DLL, the first render, driving the engine live |
| \subpage architecture | How a note becomes sound, and where the clock domains sit |
| \subpage web | The engine compiled to WebAssembly, and the app around it |
| \subpage verification | What is proven, how, and against which oracle |
| \subpage spec | The normative specification this engine is built to |
| [API reference](annotated.html) | Every public type |

## The library, by area

The headers under `include/tabulasonora` divide the way the engine does. Everything is in
namespace `ts`.

| area | what it does | start at |
|---|---|---|
| ROM | Opens the DLL as data and slices the tables out of it | ts::RomImage, ts::TableManifest, ts::TableSet, ts::WaveRom |
| Patches | Turns a program change into a tone, and a note into a wave | ts::PatchDirectory, ts::Tone, ts::DrumKitTable, ts::WaveDescriptor |
| DSP | The per-voice signal path | ts::Sampler, ts::Interpolator, ts::StateVariableFilter, ts::PitchChain, ts::TvaChain, ts::TvfChain, ts::LfoEngine |
| Effects | The three send effects and their coefficient tables | ts::Reverb, ts::Chorus, ts::SystemDelay, ts::EffectPresets, ts::EffectProgrammer |
| MIDI | Reading a Standard MIDI File into something renderable | ts::smf::MidiEvent, ts::Sequence |
| Render | The offline path — every note rendered whole, then mixed | ts::SequenceRenderer, ts::NoteRenderer, ts::wav::write |
| Real time | The block loop, 32 samples at a time, with a voice limit that steals | ts::ToneGenerator, ts::Part, ts::VoicePool, ts::FrameRing |

## You must supply the DLL

This engine is inert on its own. Roland's wave ROM and tables are not redistributed here — see
`NOTICE.md`, which also covers what little is. You need a legally obtained `SCCore.dll` from a
Sound Canvas VA installation you have licensed, pinned to one exact build: the one shipped in
**SOUND Canvas VA 1.1.6**.

| field | value |
|---|---|
| SCVA release | 1.1.6 |
| size | 27,347,456 bytes |
| SHA-256 | `117e6aa147a96fbde5e10d2caf16c89965acc1e44235fd245992216cc620bdb1` |
| PE timestamp | 2019-10-30 |

A different build moves every table offset, so ts::RomImage refuses to open one.

The release number tells you which installer to look in and nothing more. The DLL has no version
resource, so 1.1.6 cannot be read from the file and is not verified; ts::DllIdentity records it as
provenance while the hash does the identifying.

Nothing Roland-derived is committed to this repository. `assets/manifest.json` is the offset *map*,
not the data. The effect coefficients are no exception: the reverb and chorus numbers, once thought
to exist only in the running engine's state, are encoded in the DLL, and ts::EffectProgrammer
decodes them from your own copy.

## Why it exists

Sound Canvas VA was withdrawn from sale in September 2024. This is a preservation and
interoperability effort: music written for the Sound Canvas should keep playing after the software
that played it stops being available, on platforms the original plugin never supported.

This repository is the reference implementation. It began as a port of the C# engine, which is now
archived and kept as the oracle each phase was verified against; both are built to the
\ref spec "specification" recovered from the DLL. Being C++ with a BSD 3-Clause licence means a
host that cannot take a .NET runtime — including GPL software such as
[Cog](https://github.com/losnoco/Cog) — can embed it directly.
