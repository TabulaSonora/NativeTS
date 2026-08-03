# NativeTS

A native C++20 implementation of the Roland Sound Canvas VA synth voice. It reads the wave ROM and
synth tables out of `SCCore.dll` **as a data file** — the DLL is never loaded as code — so the engine
is portable and has no Windows dependency at all.

This is a port of [TabulaSonora/DotNetAdministravit](https://github.com/TabulaSonora/DotNetAdministravit),
the C# implementation, built to the specification in [TabulaSonora/spec](https://github.com/TabulaSonora/spec).
This repository is now the reference reimplementation: the C# project is archived and stays as the
oracle these phases were verified against, but new work happens here.
It exists so that hosts which cannot take a .NET runtime can embed the engine directly — BSD 3-Clause
is GPL-compatible, so [Cog](https://github.com/losnoco/Cog) and its like can link it without a
separate grant.

**Status: it plays.** A Standard MIDI File renders to a WAV through the same real-time block loop
the players and the browser build drive, comfortably faster than realtime on one core. A terminal
player and a full-screen mixer are in as well.

The oracle is `SCCore.dll` itself — its captured internal state and its rendered audio, driven
through its own exported API. See
[Verification](https://tabulasonora.github.io/NativeTS/verification.html) for what is proven and
how.

```
tabula-sonora render song.mid out.wav --map 4
tabula-sonora render song.mid out.wav --stream --solo 1,2       # at the hardware's 64 voices
tabula-sonora render-note 48 60 100 1.0 note.f32 4
tabula-sonora dump-effect reverb 4 48000 impulse.f32
tabula-sonora bench song.mid                      # time the render path stage by stage
tabula-sonora info                                # verify a DLL and describe it
tabula-sonora extract-tables tables/

tabula-sonora-play song.mid                       # space, arrows, , / . , home, q
tabula-sonora-tui  song.mid                       # full-screen mixer
tabula-sonora-play --list-devices
```

Every front end finds the ROM the same way: `--dll`, then `$TS_SCCORE_DLL`, then `./SCCore.dll`.
Pin it once and the path stops appearing in commands:

```
export TS_SCCORE_DLL=~/roms/SCCore.dll
```

The test suite pins its data separately: `TS_SCCORE` for the DLL and `TS_TABLES` for an extracted
tables directory. Unset, it looks beside the repository and in the sibling C# checkout, and skips
what it cannot find rather than failing.

Every render goes through the block loop, so `--stream` does not select a different renderer — it
selects the module's own 64-voice limit, and the stealing can be heard as the module would do it.
The default grows the pool instead, so every note in the file sounds. `--polyphony N` and
`--ports 1|2|4` go further in that direction, past what the module can do.

## What "faithful" means here

Almost every constant in this engine was recovered by measurement against the real DLL. Changing one
on aesthetic grounds is a regression even when it sounds nicer.

Faithful means matching **the module**, not matching another implementation of it. The oracle is
`SCCore.dll`: its static tables, its captured internal state, and its rendered audio. Where a
reimplementation and the hardware disagree, the hardware wins — which has happened, and cost this
engine its bit-exactness against the C# port it grew out of. That port was the scaffolding, not the
standard; it is archived and kept as a record.

## Past the module

Sound Canvas VA is itself a port: `SCCore.dll` carries the SC-8820's own mask ROMs and reproduces
its voice, down to the fixed-point arithmetic. So the lineage runs hardware → plugin → here, and
this end of it is not confined to what the previous ones could do.

| | the module | here |
|---|---|---|
| parts | 32, of which the shipped DLL can reach 16 — one `and r8b,0Fh` hides the rest | 16, 32 or 64, over one, two or four ports |
| polyphony | 64 voices, stolen when full | any limit, or a pool that grows so every note in the file sounds |
| platforms | a 64-bit Windows VST/AU host | anything with a C++20 compiler, including WebAssembly in a browser |
| licence | withdrawn from sale in September 2024 | BSD 3-Clause, embeddable in GPL software |

The rule for all of it: **match the module by default, and exceed it only on request.** A mode that
sounds better than the hardware is a feature to opt into, never the baseline — which is what keeps
"more parts" from quietly becoming "a different instrument".

### Why C++20 specifically

The original's control path is 16-bit fixed point, and a number of its expressions depend on
*wrapping* and on truncation direction rather than merely tolerating them. C++20 is the first
standard that defines enough of that to port safely: signed integers are mandated two's complement,
`>>` on a signed value is an arithmetic shift, `<<` is congruent modulo 2^N, and narrowing
conversions to signed types are modular.

What it still leaves undefined is signed overflow from `+`, `-` and `*` — so every expression that is
*meant* to overflow goes through the helpers in `src/dsp/fixed.hpp`, and an ordinary `a * b` in this
codebase should be read as a claim that the product fits.

Two build flags are correctness requirements rather than tuning knobs, and both are set in
`CMakeLists.txt` with the reasoning next to them:

- `-ffp-contract=off` — clang defaults to `fast` and will fuse `a*b+c` into an FMA, which breaks the
  float/double narrowing the DSP depends on.
- `-fwrapv` — belt-and-braces alongside the helpers above.

**Never add `-ffast-math`.** The block loop relies on exact signed-zero behaviour.

## Building

Needs CMake 3.24+, a C++20 compiler, and [vcpkg](https://github.com/microsoft/vcpkg) with
`VCPKG_ROOT` set.

```
cmake --preset release
cmake --build --preset release
ctest --preset release
```

Presets: `debug`, `release`, `asan` (ASan + UBSan), `tsan`, `player`. On Windows there are also
`debug-vs`, `release-vs` and `player-vs`: the Ninja presets expect a compiler already on the PATH,
which means a Developer prompt, while these use the Visual Studio generator, which finds MSVC on
its own and so configures from any shell. The cost is `compile_commands.json`, which that generator
cannot write. They are multi-config, so the configuration lives in the build preset, not the cache.

The `asan` preset deliberately
excludes the `signed-integer-overflow` and `shift` checks — enabling them unaudited would fire on
arithmetic that is *supposed* to wrap. The `tsan` preset runs only the `ring` label, because the ring
that hands blocks to an audio callback is the only concurrent code here; the engine itself is
single-threaded by contract.

The two players are not built by default, since they are the only things that pull in an audio
backend and a UI toolkit:

```
cmake --preset player && cmake --build --preset player      # player-vs on Windows
```

Both players build on Windows as well: ftxui carries its own console support, and the transport
reads the console through `conio` — scan codes rather than escape sequences, with no terminal mode
to take or restore. `tabula-sonora-play` is a one-line transport, and stays usable when stdin is a
pipe.
`tabula-sonora-tui` is a full-screen mixer over the *running* engine: one strip per part the file
addresses, with the tone each program resolved to, live volume, expression and pan, a per-channel
voice count, and mute and solo that take effect on a note already sounding. It opens four ports and
raises the voice limit to match, so a file that addresses more than the hardware's thirty-two parts
still plays. Both drive the same `ts::audio` core, so the ring protocol and the transport exist
once.

```
./build/release/apps/cli/tabula-sonora manifest
```

reports the DLL build the embedded offset map is pinned to.

### In the browser

[**tabula-sonora.kddlb.cl**](https://tabula-sonora.kddlb.cl) is the engine running in the browser,
with nothing to build: a [player](https://tabula-sonora.kddlb.cl/) for Standard MIDI Files and a
[live instrument](https://tabula-sonora.kddlb.cl/live). It takes the same DLL this library does and
reads it in the page.

The `web` preset compiles the whole engine to WebAssembly with Emscripten — the toolchain file
comes from `$EMSDK`, and vcpkg is not involved — and `web/` holds the Vue app that serves it: the
Player and Live pages the retired Blazor deployment had, fully client-side, with the user's own
`SCCore.dll` cached in the browser. The engine runs in a Web Worker feeding an AudioWorklet, and a
browser WAV export is byte-identical to `tabula-sonora render --stream` of the same song. See
[`web/README.md`](web/README.md) and
[the documentation's page on it](https://tabulasonora.github.io/NativeTS/web.html).

```
cmake --preset web && cmake --build --preset web    # under the emsdk environment
cd web && npm install && npm run build
```

## Embedding the engine

The library is the deliverable here and the front ends are demonstrations of it, so `ts::tabulasonora`
is packaged for import. Two ways in, both giving the same target name:

```cmake
find_package(TabulaSonora REQUIRED)          # against an installed tree
add_subdirectory(NativeTS)                   # or FetchContent, in-tree

target_link_libraries(host PRIVATE ts::tabulasonora)
```

The chain borrows downward — a `NoteRenderer` holds a reference to the `RomImage`, an engine one to
the renderer — and `RomImage` is factory-only, with no default constructor and no copy. A host that
outlives a function scope therefore holds the three as `std::unique_ptr` members, built with
`std::make_unique` in order and reset in reverse, which is what
[Holding the engine](https://tabulasonora.github.io/NativeTS/getting-started.html#holding-the-engine)
sets out.

```
cmake --preset release
cmake --build --preset release
cmake --install build/release --prefix /usr/local
```

Installing puts the archive, the headers under `include/tabulasonora`, the package config and
[`NOTICE.md`](NOTICE.md) in place — the notice travels with the binary because the binary is useless
without a DLL whose contents remain Roland's. The CLI and the players are not installed; they are
built for working on the engine, not for shipping.

An importing project needs nothing else. nlohmann_json is a *build* dependency — it parses the
embedded offset map and any pinned preset file, and is header-only, so it does not appear in the
installed package, and neither do this project's warning flags. What does come through is `ts::numeric_semantics`, and deliberately:
`-ffp-contract=off` is a correctness requirement for the inline DSP in the public headers, not a
preference, so a consumer compiles those headers under it too. C++20 comes through the same way.

Building the library out of tree gets only the library — the tests and the CLI default to off when
this is not the top-level project, so no consumer is asked for Catch2 or CLI11. The archive is built
position-independent, so a host can link it into a plugin bundle.

`ctest -L package` is the check that all of the above is true: it installs the build into a scratch
prefix and configures, builds and runs [`tests/package`](tests/package) against it as a project that
has never heard of this source tree. The ways an export set breaks — an include directory still
pointing into the source tree, a private dependency leaking into the interface, a missing standard
requirement — are all silent here and fatal in somebody else's project.

## Documentation

[**tabulasonora.github.io/NativeTS**](https://tabulasonora.github.io/NativeTS/) — the guides,
the API reference for every public type, and the curated slice of the specification this engine is
built to. The `.github/workflows/docs.yml` workflow publishes it on every push to `main`.

| | |
|---|---|
| [Getting started](https://tabulasonora.github.io/NativeTS/getting-started.html) | Supplying the DLL, the first render, driving the engine live |
| [Architecture](https://tabulasonora.github.io/NativeTS/architecture.html) | How a note becomes sound, and where the clock domains sit |
| [In the browser](https://tabulasonora.github.io/NativeTS/web.html) | The WebAssembly build and the app around it — [live here](https://tabula-sonora.kddlb.cl) |
| [Verification](https://tabulasonora.github.io/NativeTS/verification.html) | What is proven, how, and against which oracle |
| [Specification](https://tabulasonora.github.io/NativeTS/spec.html) | Signal flow, DLL layout and the glossary, as recovered |
| [API reference](https://tabulasonora.github.io/NativeTS/annotated.html) | Every public type |

Building it locally needs [Doxygen](https://www.doxygen.nl/) 1.9.8 or newer — 1.14+ for the Mermaid
diagrams — and nothing else. No compiler, no vcpkg, no DLL:

```
doxygen docs/Doxyfile          # from the repository root
```

The site lands in `build/docs`. `cmake -DTS_BUILD_DOCS=ON` adds a `docs` target that runs the same
command, for editors that want one.

The reference is generated from the header comments, which are complete, and the build treats a
broken cross-reference or an unknown command as an error rather than a warning. Diagrams are
Mermaid, rendered by Doxygen itself.

## You need your own `SCCore.dll`

The engine is inert without one, from a Sound Canvas VA installation you have licensed. The offsets
are pinned to exactly one build — the one shipped in **SOUND Canvas VA 1.1.6**:

| field | value |
|---|---|
| size | 27,347,456 bytes |
| SHA-256 | `117e6aa147a96fbde5e10d2caf16c89965acc1e44235fd245992216cc620bdb1` |
| PE timestamp | 2019-10-30 |

A different build moves every table offset, so the ROM reader refuses to open one. The release
number is how you find the right installer; it is not what identifies the file. The DLL carries no
version resource at all, so the hash, the timestamp and the size are the identity.

## What is and is not in this repository

Nothing Roland-derived is committed. `assets/manifest.json` is the offset *map*, not the data. The
effect coefficients are no exception: the reverb and chorus numbers, once thought to exist only in
the running engine's state, are encoded in the DLL and `EffectProgrammer` decodes them from your own
copy, matching a live harvest exactly. [`NOTICE.md`](NOTICE.md) has the detail.

The wave ROM, the extracted tables and any rendered audio are gitignored. Regenerate the tables from
your own DLL with the CLI, which does this by reading the file and never by running it — the DLL
comes from `--dll` or `$TS_SCCORE_DLL`:

```
tabula-sonora extract-tables tables/
```

## How this was written

The bulk of this code was generated by large language models, working from the specification and
under review. That is not a footnote: it is how every commit in this repository was made, and
`git log` is the record — each one carries a `Co-Authored-By` trailer naming the model that wrote
it.

```
git log --format='%(trailers:key=Co-Authored-By,valueonly,unfold)' | sort | uniq -c
```

Claude Fable 5 and Claude Opus 5, to date. The attribution is per commit rather than a blanket
notice at the top, so `git blame` answers the question for any particular line.

What makes that acceptable here is the same thing the rest of this README is about: nothing in this
engine is trusted because it looks right. A port whose bar is a SHA-256 match against another
implementation's output is checkable by machine, and the phases were gated that way — a phase did
not start until the previous one was byte-exact. See
[Verification](https://tabulasonora.github.io/NativeTS/verification.html) for what is proven and
what is not. The constants were recovered by measurement, and the places where the evidence is thin
are tagged as such rather than smoothed over.

## Licence

BSD 3-Clause — see [`LICENSE`](LICENSE). That covers this repository's own code only; see
[`NOTICE.md`](NOTICE.md) for what remains Roland's and must be supplied from your own installation.
