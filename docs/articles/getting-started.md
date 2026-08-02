# Getting started {#getting-started}

## What you need

A legally obtained `SCCore.dll` from a Sound Canvas VA installation. The library pins one exact
build and refuses any other, because a different build moves every table offset:

| field | value |
|---|---|
| size | 27,347,456 bytes |
| SHA-256 | `117e6aa147a96fbde5e10d2caf16c89965acc1e44235fd245992216cc620bdb1` |
| PE timestamp | 2019-10-30 |

You also need CMake 3.24 or newer, a C++20 compiler, and
[vcpkg](https://github.com/microsoft/vcpkg) with `VCPKG_ROOT` set.

```
cmake --preset release
cmake --build --preset release
ctest --preset release
```

Presets: `debug`, `release`, `asan` (ASan and UBSan), `tsan`, `player`. On Windows there are also
`debug-vs`, `release-vs` and `player-vs`, which use the Visual Studio generator and so configure
from any shell — the Ninja presets expect a compiler already on the `PATH`, which means a Developer
prompt.

## There is no prepare step

If you know the C# engine, this is the part that changed. That project needed a one-off `prepare`
run on **Windows x64** to harvest the reverb and chorus coefficients by executing `SCCore.dll` and
reading the running engine's state, because those numbers were believed to exist nowhere in the
file. It wrote a `presets.json` you then had to keep alongside the binary.

They were in the file all along, encoded. ts::EffectProgrammer decodes them from your own copy —
ring taps stored relative to a base the loader adds, and coefficients in a signed-14-bit floating
format — matching a live harvest exactly, across all eight reverb macros, all eight chorus macros
and both GM defaults, with no residual.

So nothing here ever loads the DLL as code, on any platform, for any purpose. There is no preset
file to ship, no Windows-only step, and no preparation at all: point the engine at a DLL and
render.

## Pointing at the DLL

Every front end finds the ROM the same way — `--dll`, then `$TS_SCCORE_DLL`, then `./SCCore.dll`.
Pin it once and the path stops appearing in commands:

```
export TS_SCCORE_DLL=~/roms/SCCore.dll
```

Two subcommands tell you where you stand. `info` verifies a DLL and describes it; `manifest`
reports the build the embedded offset map is pinned to, and needs no DLL at all.

```
tabula-sonora info
tabula-sonora manifest
```

`extract-tables` writes every static table out as a `.bin` slice, which the test suite can use as a
cache. It reads the file and never runs it.

```
tabula-sonora extract-tables tables/
```

\note The extracted tables are Roland's data. They are gitignored here and should not be
redistributed — regenerate them from your own DLL.

## Render a file

```
tabula-sonora render song.mid out.wav --map 4
```

| option | meaning |
|---|---|
| `--map 1..4` | SC-55, SC-88, SC-88Pro, SC-8820 — the same program resolves to different tones |
| `--mute 1,2` / `--solo 5,6` | channels as a mixer labels them, 1–16 |
| `--tail SEC`, `--end SEC` | release tail, and truncation |
| `--volume G` | linear gain on the finished mix |
| `--drum-map 0..5` | drum map row, when you want one the vintage would not pick |
| `--no-reverb`, `--no-chorus`, `--no-delay` | effects are on by default, as the module has them |
| `--stream` | render through the real-time block loop instead of the offline path |

`--stream` is worth knowing about. It drives the same block loop the players use, so the difference
the architecture makes — a 64-voice limit that actually steals, live controllers, effect types that
change mid-song — can be heard against the offline render of the same file.

Two more subcommands exist for analysis: `render-note` writes a single note as raw interleaved
float32, `dump-effect` writes a send effect's impulse response, and `bench` times the render path
stage by stage.

```
tabula-sonora render-note 48 60 100 1.0 note.f32 4
tabula-sonora dump-effect reverb 4 48000 impulse.f32
tabula-sonora bench song.mid
```

## Play a file

The two players are not built by default, since they are the only things that pull in an audio
backend and a UI toolkit:

```
cmake --preset player && cmake --build --preset player      # player-vs on Windows
```

```
tabula-sonora-play song.mid
tabula-sonora-tui  song.mid
```

`tabula-sonora-play` is a one-line terminal transport and stays usable when stdin is a pipe. Space
pauses, the arrow keys seek five seconds, `,` and `.` seek thirty, `Home` returns to the start, `q`
quits. Playback starts immediately — the song is synthesised through the block loop as it plays, so
there is nothing to wait for and a long file costs no more memory than a short one. `--prerender`
renders the whole song first instead, which makes seeking exact.

`tabula-sonora-tui` is a full-screen mixer over the *running* engine: sixteen parts with the tone
each program resolved to, live volume, expression and pan, a per-channel voice count, and mute and
solo that take effect on a note already sounding.

Both build on Windows as well, and both drive the same `ts::audio` core, so the ring protocol and
the transport exist once. The player takes every render option above, plus:

| option | meaning |
|---|---|
| `--prerender` | render the whole song before playing instead of streaming it |
| `--list-devices` | enumerate outputs and exit |
| `--device NAME\|N` | pick an output by name fragment or index |
| `--latency MS` | how far ahead of the device to run |
| `--buffer FRAMES` | device period, in frames |
| `--gain G` | linear gain on the way out |

If it stutters, raise `--latency`.

## Driving the engine live

ts::ToneGenerator is the engine itself: MIDI in, blocks out, nothing known in advance.

```cpp
#include <tabulasonora/rom_image.hpp>
#include <tabulasonora/note_renderer.hpp>
#include <tabulasonora/tone_generator.hpp>

auto rom = ts::RomImage::open(dll_path);
ts::NoteRenderer notes(rom);
ts::ToneGenerator engine(notes);

engine.send_channel(0xC0, 48, 0);     // program change: strings
engine.send_channel(0x90, 60, 100);   // note on

std::vector<float> left(512), right(512);
engine.render(left, right);           // hold it for as long as you like

engine.send_channel(0x80, 60, 0);     // note off, whenever
```

Send events between `render` calls and they land on the block boundary, which is the grid the
engine itself applies them on. Polyphony is the hardware's own 64 voices; past that the allocator
steals, taking whole notes rather than half of one and fading what it takes.

The engine has 32 parts over two ports — `send_channel` has an overload taking a port index, and
ts::ToneGenerator::part is indexed `port * 16 + channel`. A host that never names a port drives
port A and behaves exactly as a sixteen-part engine.

\note ts::ToneGenerator is not thread-safe: events and rendering must come from the same thread. To
get audio to an audio callback on another thread, hand blocks across ts::FrameRing, the lock-free
single-producer, single-consumer ring the players use.

To play a file rather than drive it by hand, ts::SequencePlayer dispatches a parsed event list as
it renders, and `seek` replays the file's controllers up to a position so that jumping into the
middle sounds the way playing up to there would.

```cpp
auto player = ts::SequencePlayer::from_file(engine, "song.mid");
player.seek(60 * ts::ToneGenerator::sample_rate);
player.render(left, right);
```

\note One thing genuinely differs from ts::SequenceRenderer. The offline path latches a note's
program, bank and pan by looking them up at the note's own position, which picks up a program
change written *after* the note-on at the same tick. A running engine cannot: the note-on arrives
first and plays whatever program was already selected — which is what the module does. Some files,
`canyon.mid` among them, put their program changes last at tick 0, so the first few notes come out
on a different patch here.

## From code

The library is the deliverable and the front ends are demonstrations of it, so `ts::tabulasonora`
is packaged for import. Two ways in, both giving the same target name:

```cmake
find_package(TabulaSonora REQUIRED)          # against an installed tree
add_subdirectory(NativeTS)                   # or FetchContent, in-tree

target_link_libraries(host PRIVATE ts::tabulasonora)
```

An importing project needs nothing else. nlohmann_json is a *build* dependency and header-only, so
it does not appear in the installed package, and neither do this project's warning flags. What does
come through is `ts::numeric_semantics`, deliberately: `-ffp-contract=off` is a correctness
requirement for the inline DSP in the public headers, not a preference, so a consumer compiles
those headers under it too. C++20 comes through the same way.

```cpp
// The image must stay open: wave data is read on demand, not cached up front.
auto rom = ts::RomImage::open(dll_path);

ts::NoteRenderer notes(rom);
ts::SequenceRenderer renderer(notes);
auto result = renderer.render_file("song.mid", { .map = ts::ToneMap::sc8820 });

// result.left / result.right are float, at result.sample_rate (32 kHz).
```

To mute or solo parts — for a mixer UI, say — hold a ts::ChannelMask and mutate it freely. Its
flags are atomics, so it is safe to toggle from another thread while a render runs, and the
renderer snapshots it once so a mid-render change cannot make some notes of a part sound and others
not.

```cpp
ts::ChannelMask channels;
channels.set_muted(9, true);                  // drop the drum part

ts::RenderOptions options{ .channels = &channels };
auto dry = renderer.render(sequence, options);
```

Writing the result out is ts::wav::write.

## Rendering one note

ts::NoteRenderer exposes the voice directly, which is useful for analysis or for driving your own
scheduler:

```cpp
ts::NoteRenderer notes(rom);
auto voice = notes.render_note(/*program=*/73, /*note=*/72, /*velocity=*/100,
                               /*hold_seconds=*/1.0, /*tail_seconds=*/1.8);

std::print("{}\n", voice.name);   // "Flute"
```

`voice.mono` is the pre-pan sum of the partials — the pan-independent source the sends are taken
from.
