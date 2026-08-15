# Getting started {#getting-started}

## What you need

A legally obtained `SCCore.dll` from a Sound Canvas VA installation. The library pins one exact
build and refuses any other, because a different build moves every table offset:

| field | value |
|---|---|
| size | 27,347,456 bytes |
| SHA-256 | `117e6aa147a96fbde5e10d2caf16c89965acc1e44235fd245992216cc620bdb1` |
| PE timestamp | 2019-10-30 |

That build is the one shipped in **SOUND Canvas VA 1.1.6**. The release number is how you find the
right installer and nothing more — the DLL carries no version resource, so the hash, the size and
the timestamp are what identify it.

\note If all you want is to hear the engine, you do not need any of what follows. The same code
runs at [tabula-sonora.kddlb.cl](https://tabula-sonora.kddlb.cl), which takes the same DLL and
reads it in the browser — see \ref web.

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
| `--map 1..4`, `--map xg` | SC-55, SC-88, SC-88Pro, SC-8820 — the same program resolves to different tones. `xg` is not a fifth vintage: it starts the engine in XG mode, below. Names work too (`--map sc88pro`) |
| `--mute 1,2` / `--solo 5,6` | channels as a mixer labels them, 1–16 |
| `--tail SEC`, `--end SEC` | release tail, and truncation |
| `--volume G` | linear gain on the finished mix |
| `--drum-map 0..5` | drum map row, when you want one the vintage would not pick |
| `--no-reverb`, `--no-chorus`, `--no-delay`, `--no-efx` | effects are on by default, as the module has them |
| `--gsws` | what the Microsoft GS Wavetable Synth gives you: the SC-55 map with all four effect blocks off. It supplies the map rather than forcing it, so an explicit `--map` still wins |
| `--loops N` | play-throughs of the loop body; `1` is the default, `-1` never stops |
| `--fade SEC` | the fade that follows a finite loop count, so it ends rather than cutting |
| `--stream` | limit polyphony to the hardware's 64 voices |
| `--polyphony N` | voice limit outright; `0` grows the pool on demand, and is the default |
| `--ports 1\|2\|4` | 16, 32 or 64 parts; two is the hardware |
| `--module-resampler` | the module's own 4-tap resampler and its 4× pitch increment ceiling, instead of the wide band-limiting one. What a render being compared against `SCCore.dll` needs; it also restores the module's held portamento |
| `--flush-per-sysex` | let every SysEx message start a fresh input-queue window, so a bulk dump larger than one control tick is delivered whole instead of being silently truncated. The module drops the remainder and cannot be flushed out of doing so, so this plays a file as written rather than as the hardware receives it |

Every render goes through the block loop — there is no second renderer to choose between, and
`--stream` no longer selects one. What it selects is the module's own voice limit, so that the
stealing can be heard as the module would do it. The default instead grows the pool, so every note
in the file sounds; `render` says afterwards which of the two happened, because a file that never
ran out renders identically at any limit.

`--ports 4` is past what the module can do and wants a voice limit raised to suit — sixty-four
parts sharing sixty-four voices would steal without pause. See \ref architecture.

**The input does not have to be a Standard MIDI File.** ts::formats::to_smf converts the formats
game music actually shipped in — RIFF-MIDI, DirectMusic `MIDS`, DOOM `MUS`, Miles `XMI`, `GMF`,
both HMI containers, Mobile XMF and the LDS tracker — into an in-memory SMF before the one reader
sees it, so every front end gained them at once and nothing downstream knows the difference. A file
that is already an SMF is passed through untouched.

Loop points come out of the same parse: ts::smf::load scans the four marker dialects the corpus
uses — Touhou's CC 2/4 pair, RPG Maker's CC 111, the XMI/EMIDI CC 116–119 set, and
`loopStart`/`loopEnd` markers — and reports the surviving points in samples on ts::smf::Song. They
sit unused until `--loops` asks for them. Tracks carrying an EMIDI designation (CC 110) for some
other synthesizer are dropped whole during the parse, so a multi-synth score does not double its
voices here.

Two more subcommands exist for analysis: `render-note` writes a single note as raw interleaved
float32, `dump-effect` writes a send effect's impulse response, and `bench` times the render path
stage by stage.

```
tabula-sonora render-note 48 60 100 1.0 note.f32 4
tabula-sonora dump-effect reverb 4 48000 impulse.f32
tabula-sonora bench song.mid
```

`render-note` drives the note through the block loop the way the oracle gate does — same warm-up,
same event delay, output stage on — so its output is directly comparable against a case from the
note sweep. `--channel 9` makes it a drum hit, `--tail` sets the seconds rendered past the note-off
(1.8 matches the sweep), and `--per-note` selects the isolated renderer instead. That last one takes
the ideal `pow(2, x/12000)` for its rate rather than the module's ramp table, so its pitch can sit
several cents off; do not compare it against an oracle case.

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

`tabula-sonora-tui` is a full-screen mixer over the *running* engine: one strip per part the file
actually addresses, with the tone each program resolved to, live volume, expression and pan, a
per-channel voice count, and mute and solo that take effect on a note already sounding. It opens
four ports rather than the hardware's two, and raises the voice limit to match — a player is handed
whatever it is given, and a file whose parts the engine cannot reach is a silence a listener cannot
diagnose. `l` toggles looping while it plays and `--loop` starts with it on; the transport line
shows the state beside the clock. While looping the source has no end — the position wraps at the
file's loop points, or over the whole song when it declares none.

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

Send events between `render` calls and they take effect at the top of the next one. To place them
*inside* it, ts::ToneGenerator::send_channel_at takes a sample offset — the module stamps every
message with `offset * 1000 / host_rate` milliseconds and releases it when its chunk counter reaches
that, so placement is real but quantised to the millisecond. The offset leads rather than trails,
which keeps a four-argument call from meaning two different things.

Two options make the engine keep the module's own timing rather than this engine's convenience:
ts::ToneGeneratorOptions::event_delay_blocks holds a message for four 32-sample chunks the way the
module's rings do, and clearing ts::ToneGeneratorOptions::bypass_output_filter runs its output
stage. Both default to the convenient setting, and anything being compared against the module wants
both — see \ref verification.
 Polyphony is the hardware's own 64 voices; past that the allocator
steals, taking whole notes rather than half of one and fading what it takes.
ts::ToneGeneratorOptions::polyphony raises that limit, and
ts::ToneGeneratorOptions::unlimited_polyphony makes the pool grow rather than steal — right for an
offline render, wrong on an audio thread, since growing allocates.

The engine has 32 parts over two ports — `send_channel` has an overload taking a port index, and
ts::ToneGenerator::part is indexed `port * 16 + channel`. A host that never names a port drives
port A and behaves exactly as a sixteen-part engine.
ts::ToneGeneratorOptions::ports takes 1, 2 or 4, and four is an extension past the module rather
than something it does; ts::ToneGenerator::max_port_count says so in the reference.

A file says which port a track belongs to with a meta event — `FF 21` (MIDI Port) carries the
number, and `FF 09` (Device Name) names an output that ts::MidiEvent::port numbers in order of
first appearance — so a sequence that addresses more than sixteen channels routes itself. An
untagged file is all port 0, exactly as before, and a file asking for a port the engine does not
have folds onto one it does rather than falling silent.

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

\note Events at the same tick are dispatched in the order the file writes them, which is not always
the order it seems to intend. A note-on plays whatever program was selected when it arrived, so a
program change written *after* it at the same tick reaches the following note and not that one.
That is what the module does — a running engine has no way to look ahead — and some files,
`canyon.mid` among them, put their program changes last at tick 0, so their first few notes sound
on the previous patch.

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
ts::ToneGenerator engine(notes, { .map = ts::ToneMap::sc8820 });

auto player = ts::SequencePlayer::from_file(engine, "song.mid");
auto result = player.render_to_end();

// result.left / result.right are float, at result.sample_rate (32 kHz).
```

ts::SequencePlayer::render_to_end streams the whole file into memory from wherever the player
currently is; ts::SequencePlayer::render fills a pair of buffers instead, which is the same work in
whatever sized pieces the caller wants. Nothing about the engine changes between them.

To mute or solo parts — for a mixer UI, say — hold a ts::ChannelMask and mutate it freely. Its
flags are atomics, so it is safe to toggle from another thread while a render runs, and it is read
at the mix rather than at note-on, so a part goes quiet on the next block and comes back on the
one after.

```cpp
ts::ChannelMask channels;
channels.set_muted(9, true);                  // drop the drum part

ts::ToneGenerator engine(notes, { .channels = &channels });
```

Writing the result out is ts::wav::write.

## XG

The module speaks a second SysEx dialect, and it is a **mode** rather than a set of extra messages:
XG System On moves every part onto the XG tone and drum maps, and any Roland message or GM reset
moves them back. A file that carries `F0 43 10 4C 00 00 7E 00 F7` switches the engine over on its
own and nothing needs configuring.

Many XG files send no System On at all, because a real XG module is already in XG when it powers on.
For those, say so:

```
tabula-sonora render song.mid out.wav --map xg
```

```cpp
ts::ToneGenerator engine(notes, { .map = ts::ToneMap::xg });
```

ts::ToneMap::xg is the switch, rather than a separate flag, because on the module the two are one
thing. It is a *starting* state: a file may still change mode, and ts::ToneGenerator::reset returns
to it. Nothing infers XG from the shape of a file's bank selects — a bank LSB of 18 is a legitimate
GS map selector, so guessing would break the files that mean it.

What changes under XG is worth knowing if you display anything:

- The bank pair is inverted. The **LSB** carries the variation; the MSB chooses melodic, the SFX
  voice bank (`0x40`), the SFX kits (`0x7E`) or the drum kits (`0x7F`).
- **Any** channel can be a drum part, chosen by bank select alone. Channel 10 is no longer the
  answer to "is this drums", in either direction.
- Every part moves to the XG map at once, so one map for the whole mixer is wrong the moment a file
  switches.

So the engine answers per part rather than leaving a caller to infer:

```cpp
if (engine.xg_mode()) { /* the file, or the host, put it here */ }

for (int part = 0; part < engine.parts(); ++part) {
    if (engine.part_is_drum(part)) {
        const int kit = engine.part_drum_kit(part);
        std::cout << notes.drums().kit_name(kit) << '\n';      // "analog kit"
    } else {
        const int tone = notes.directory().program_to_tone(
            engine.part(part).program,
            engine.part_tone_map(part),        // ToneMap::xg while the mode holds
            engine.part_lookup_bank(part));    // not part().bank under XG
        std::cout << notes.directory().tone(tone)->name() << '\n';
    }
}
```

ts::ToneGenerator::part_lookup_bank is the one most easily missed: under XG it is the bank *LSB*,
except for bank MSB 64, where the module substitutes the SFX voice column. Reading `part().bank`
instead names a real instrument — the wrong one, silently.

ts::DrumKitTable::kit_name reads the kit's own name out of its ROM record. Three things not to
assume about the result are documented on it; the shortest is that the casing is the ROM's, and an
ALL-CAPS kit name on XG-flavoured material means the drum row is *not* following XG.

For a front end that needs to know what an XG address means without acting on it,
ts::decode_xg_sysex classifies a message and remaps its part number, and ts::decode_xg_multi_part
turns a Multi Part parameter into the same ts::ControlUpdate vocabulary a Control Change decodes to.
Part numbers reach `0x3F`; range-check them against ts::ToneGenerator::parts and **ignore** what
does not fit, rather than masking it into range.

ts::tone_map_choices is the name/value list the command line validates against, if you are building
one of your own.

## Holding the engine {#holding-the-engine}

The chain borrows downward. A ts::NoteRenderer keeps a reference to the image it was built over,
ts::ToneGenerator keeps one to the renderer, and ts::SequencePlayer keeps a pointer to the engine —
no layer owns anything below it, which is what every *must outlive it* in the headers is saying. The snippets above are stack locals in a single scope, and that is all a `main` needs.

A host built the other way cannot do that. A plugin with `startup()` and `shutdown()`, or a session
object that outlives any one call, holds the chain as members and fills them in later — and
ts::RomImage has no default constructor and no copy. It exists only as the return of
ts::RomImage::open or ts::RomImage::from_memory, so at the point the host itself is constructed there
is nothing to construct a member from.

`std::unique_ptr` is what to reach for, and `std::make_unique<const ts::RomImage>` is well formed: it
move-constructs the heap object out of whatever the factory returned.

```cpp
std::unique_ptr<const ts::RomImage> rom;
std::unique_ptr<ts::NoteRenderer> notes;
std::unique_ptr<ts::ToneGenerator> engine;

// startup(), in order — each layer needs the one below it to exist first.
rom    = std::make_unique<const ts::RomImage>(
             ts::RomImage::open(dll_path, ts::RomVerification::quick));
notes  = std::make_unique<ts::NoteRenderer>(*rom);
engine = std::make_unique<ts::ToneGenerator>(*notes, options);

// shutdown(), in reverse — which is also what lets a host reopen against a different DLL.
engine.reset();
notes.reset();
rom.reset();
```

The pointer rather than the value, because the pointee does not move when the host object does. Hold
the chain by value in a struct that is later moved and the renderer's reference to the image, and the
engine's reference to the renderer, both go on addressing the moved-from shells. Nothing
announces it. Behind a `std::unique_ptr` the addresses the layers above captured never change, so the
question does not arise.

`std::optional<ts::RomImage>` works too — `rom.emplace(ts::RomImage::open(dll_path))` — and is what
this repository's own WebAssembly session uses, in `apps/web/src/web_session.hpp`. It is the lighter
choice when the owner is pinned in place and never moved. The pointer is the safer default.

The worked example is [Cog](https://github.com/losnoco/Cog/tree/sparkle/Plugins/MIDI/MIDI)'s MIDI
plugin: its `TSPlayer` holds a `std::unique_ptr<const ts::RomImage>`, fills it with
`std::make_unique` in `startup()` and calls `rom.reset()` in `shutdown()` — which is also what lets
`setSCCore` point the player at a different DLL and rebuild the engine over it, without the player
object itself going anywhere.

\note ts::RomImage::from_memory adds a second lifetime. Nothing is copied — the bytes are read in
place — so the buffer has to stay alive and unchanged for as long as the image is used, exactly as a
file image needs its handle open. A host that supplies memory owns two things, not one.

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
