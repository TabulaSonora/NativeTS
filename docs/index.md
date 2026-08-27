# Tabula Sonora {#mainpage}

A native C++20 implementation of the Roland Sound Canvas VA synth voice. It reads the wave ROM and
synth tables out of `SCCore.dll` **as a data file** — the DLL is never loaded as code — so the
engine is portable and has no Windows dependency at all.

MIDI file in, audio out, on one core.

```cpp
#include <tabulasonora/rom_image.hpp>
#include <tabulasonora/tone_generator.hpp>
#include <tabulasonora/sequence_player.hpp>

auto rom = ts::RomImage::open("SCCore.dll");
ts::NoteRenderer notes(rom);
ts::ToneGenerator engine(notes, {
    .polyphony = ts::ToneGeneratorOptions::unlimited_polyphony,
    .map = ts::ToneMap::sc55,   // the vintage to resolve programs against
});

auto player = ts::SequencePlayer::from_file(engine, "song.mid");
auto result = player.render_to_end(/*tail_seconds=*/2.2);
```

`result.left` and `result.right` are the finished mix at ts::ToneGenerator::sample_rate.

## Where to start

| | |
|---|---|
| \subpage getting-started | Supplying the DLL, the first render, driving the engine live |
| \subpage architecture | How a note becomes sound, and where the clock domains sit |
| \subpage web | The engine compiled to WebAssembly, and the app around it |
| \subpage verification | What is proven, how, and against which oracle |
| \subpage spec | The normative specification this engine is built to |
| [API reference](annotated.html) | Every public type |

Or hear it first. The same engine is live at
[**tabula-sonora.kddlb.cl**](https://tabula-sonora.kddlb.cl) — a
[player](https://tabula-sonora.kddlb.cl/) for Standard MIDI Files and a
[live instrument](https://tabula-sonora.kddlb.cl/live), running in the browser with nothing to
build. It needs the same DLL this library does, and reads it in the page.

## The library, by area

The headers under `include/tabulasonora` divide the way the engine does. Everything is in
namespace `ts`.

| area | what it does | start at |
|---|---|---|
| ROM | Opens the DLL as data and slices the tables out of it | ts::RomImage, ts::TableManifest, ts::TableSet, ts::WaveRom |
| Patches | Turns a program change into a tone, and a note into a wave | ts::PatchDirectory, ts::Tone, ts::DrumKitTable, ts::WaveDescriptor |
| DSP | The per-voice signal path | ts::Sampler, ts::Interpolator, ts::StateVariableFilter, ts::PitchChain, ts::TvaChain, ts::TvfChain, ts::LfoEngine |
| Modulation | What the stream can move while a note sounds | ts::ControlMatrix, ts::PartModifiers, ts::PitchRamp, ts::ControlDecode |
| Effects | The three send effects, the insertion EFX block, the part EQ, and their coefficient tables | ts::Reverb, ts::Chorus, ts::SystemDelay, ts::InsertionEffect, ts::Equalizer, ts::EffectPresets, ts::EffectProgrammer |
| MIDI | Reading a music file into something renderable, whatever it was written as | ts::MidiEvent, ts::Sequence, ts::smf::Song, ts::smf::SongLoop, ts::formats::to_smf |
| The engine | The block loop, 32 samples at a time, and the parts it drives | ts::ToneGenerator, ts::Part, ts::VoicePool, ts::FrameRing |
| Render | Playing a file through it, live or into a buffer | ts::SequencePlayer, ts::RenderOptions, ts::NoteRenderer, ts::wav::write |

## You must supply the DLL

This engine is inert on its own. Roland's wave ROM and tables are not redistributed here — see
`NOTICE.md`, which also covers what little is. You need a legally obtained `SCCore.dll` from a
Sound Canvas VA installation you have licensed. Three builds are recognised — **SOUND Canvas VA
1.1.6** (2019, x64) and the **1.0.3** pair (2016, x64 and x86). Offsets are recorded in 1.1.6's
coordinates because that is the build the behaviour was reverse-engineered from; the 1.0.3 builds
hold the same data re-packed, and the engine translates through a per-build segment map.

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
archived and kept as a record; both are built to the \ref spec "specification" recovered from the
DLL. Being C++ with a BSD 3-Clause licence means a host that cannot take a .NET runtime — including
GPL software such as [Cog](https://github.com/losnoco/Cog) — can embed it directly.

And the plugin is itself a port: `SCCore.dll` carries the SC-8820's own mask ROMs and reproduces
its voice down to the fixed-point arithmetic. The lineage runs hardware → plugin → here, and this
end of it is not confined to what the previous two could do — it will run 64 parts over four ports
where the module has 32 and the shipped DLL reaches 16, and will grow its voice pool past the
hardware's 64 rather than steal. Everything of that kind is opt-in, because the default has a job:
**match the module, and exceed it only on request.**

## How this was written

The bulk of this code was generated by large language models, working from the specification and
under review. That is not a footnote: it is how every commit in the repository was made, and
`git log` is the record — each one carries a `Co-Authored-By` trailer naming the model that wrote
it, so `git blame` answers the question for any particular line. Claude Fable 5 and Claude Opus 5,
to date.

```
git log --format='%(trailers:key=Co-Authored-By,valueonly,unfold)' | sort | uniq -c
```

What makes that acceptable is the property the rest of these pages are about: nothing in this
engine is trusted because it looks right. A port whose bar is a SHA-256 match against another
implementation's output is checkable by machine, and the phases were gated that way — a phase did
not start until the previous one was byte-exact. \ref verification sets out what is proven, against
which oracle, and where this engine knowingly differs. The constants were recovered by measurement,
and the claims resting on thin evidence are tagged as such rather than smoothed over.
