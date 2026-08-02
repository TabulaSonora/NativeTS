# NativeTS

A native C++20 implementation of the Roland Sound Canvas VA synth voice. It reads the wave ROM and
synth tables out of `SCCore.dll` **as a data file** — the DLL is never loaded as code — so the engine
is portable and has no Windows dependency at all.

This is a port of [TabulaSonora/DotNetAdministravit](https://github.com/TabulaSonora/DotNetAdministravit),
the C# implementation, built to the specification in [TabulaSonora/spec](https://github.com/TabulaSonora/spec).
It exists so that hosts which cannot take a .NET runtime can embed the engine directly — BSD 3-Clause
is GPL-compatible, so [Cog](https://github.com/losnoco/Cog) and its like can link it without a
separate grant.

**Status: it plays.** A Standard MIDI File renders to a WAV that is **byte-for-byte identical to the
C# engine** — checked on a 123-second, 4,366-note file across all four tone maps and every effect and
gain option. Faster, too: about 7 seconds against 10.5 for the same render. The real-time block loop,
a terminal player and a full-screen mixer are in as well.

```
tabula-sonora render song.mid out.wav --map 4
tabula-sonora render song.mid out.wav --stream --solo 1,2
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

`render --stream` drives the same block loop the player does, so the difference the architecture
makes — a 64-voice limit that actually steals, live controllers, effect types that change mid-song —
can be heard against the offline render of the same file.

## What "faithful" means here

Almost every constant in this engine was recovered by measurement against the real DLL. Changing one
on aesthetic grounds is a regression even when it sounds nicer.

The C# engine is the oracle for this port, and the bar is **bit-exactness where that engine is
bit-exact** — the static tables, the sample codec, the pitch and LFO tick streams — with rendered
audio matching to float epsilon. Each porting phase ends by diffing against a C# render; a phase does
not start until the previous one is exact.

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

Presets: `debug`, `release`, `asan` (ASan + UBSan), `tsan`, `player`. The `asan` preset deliberately
excludes the `signed-integer-overflow` and `shift` checks — enabling them unaudited would fire on
arithmetic that is *supposed* to wrap. The `tsan` preset runs only the `ring` label, because the ring
that hands blocks to an audio callback is the only concurrent code here; the engine itself is
single-threaded by contract.

The two players are not built by default, since they are the only things that pull in an audio
backend and a UI toolkit:

```
cmake --preset player && cmake --build --preset player
```

`tabula-sonora-play` is a one-line transport, and stays usable when stdin is a pipe.
`tabula-sonora-tui` is a full-screen mixer over the *running* engine: sixteen parts with the tone
each program resolved to, live volume, expression and pan, a per-channel voice count, and mute and
solo that take effect on a note already sounding. Both drive the same `ts::audio` core, so the ring
protocol and the transport exist once.

```
./build/release/apps/cli/tabula-sonora manifest
```

reports the DLL build the embedded offset map is pinned to.

## Embedding the engine

The library is the deliverable here and the front ends are demonstrations of it, so `ts::tabulasonora`
is packaged for import. Two ways in, both giving the same target name:

```cmake
find_package(TabulaSonora REQUIRED)          # against an installed tree
add_subdirectory(NativeTS)                   # or FetchContent, in-tree

target_link_libraries(host PRIVATE ts::tabulasonora)
```

```
cmake --preset release
cmake --build --preset release
cmake --install build/release --prefix /usr/local
```

Installing puts the archive, the headers under `include/tabulasonora`, the package config and
[`NOTICE.md`](NOTICE.md) in place — the notice travels with the binary because the reverb and chorus
coefficients are compiled into it. The CLI and the players are not installed; they are built for
working on the engine, not for shipping.

An importing project needs nothing else. nlohmann_json is a *build* dependency — it parses the two
embedded assets and is header-only, so it does not appear in the installed package, and neither do
this project's warning flags. What does come through is `ts::numeric_semantics`, and deliberately:
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

Nothing Roland-derived is committed except one file, and it is named. `assets/manifest.json` is the
offset *map*, not the data. `assets/presets.json` — the reverb and chorus coefficients — **is**
Roland-derived and **is** committed, because those numbers are computed by the engine at start-up and
cannot be regenerated by anyone without a Windows x64 machine. [`NOTICE.md`](NOTICE.md) sets out the
reasoning and how to remove the file if you would rather not carry it.

The wave ROM, the extracted tables and any rendered audio are gitignored. Regenerate the tables from
your own DLL with the C# tool, which does this by reading the file and never by running it:

```
tabula-sonora extract-tables tables/
```

## Licence

BSD 3-Clause — see [`LICENSE`](LICENSE). That covers this repository's own code only; see
[`NOTICE.md`](NOTICE.md) for what remains Roland's and must be supplied from your own installation.
