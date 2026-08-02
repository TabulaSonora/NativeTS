# Notice on third-party rights

## What this project licenses

The BSD 3-Clause licence in `LICENSE` covers **this repository's own contents**: the C++ source, the
tests, the build system, and `assets/manifest.json`. All of it is original work, written from
published reverse-engineering notes and from the
[C# implementation](https://github.com/TabulaSonora/DotNetAdministravit) this ports. No decompiler
output and no transcribed Roland source is present.

That licence does **not**, and cannot, grant you any right in Roland's software or data.

## What you must supply yourself

This engine is inert without `SCCore.dll` from a Roland SOUND Canvas VA installation — specifically
the build shipped in **SOUND Canvas VA 1.1.6**, which is the only one the table offsets are valid
for. That file — and everything derived from it — remains Roland Corporation's:

- the 24 MB wave ROM embedded in it, which is the literal Sound Canvas hardware mask ROM
- the synth curve, key-follow and patch-directory tables
- the reverb and chorus coefficients the engine derives from it
- any audio decoded or rendered from the above

None of that is committed here and none of it is redistributed. `.gitignore` excludes each
category. `assets/manifest.json` is tracked because it is a map of *where* those tables live, not
the tables themselves — the same distinction the upstream
[TabulaSonora spec](https://github.com/TabulaSonora/spec) draws.

## No committed coefficient file

Earlier versions shipped `assets/presets.json`, ~27 KB of Roland-derived reverb and chorus
coefficients, because those numbers were believed to be obtainable only by running the DLL — a
64-bit Windows binary — and reading the engine's start-up state. They are not. The coefficients are
encoded in the file: per-character tap rows stored relative to a ring base the loader adds, and
coefficient rows in a signed-14-bit float encoding. `EffectProgrammer` decodes them straight from
the DLL on any platform, matching a live harvest field for field, so nothing Roland-derived is
committed. The engine reads its coefficients out of the DLL you supply, exactly as it reads the wave
ROM and the tables.

## Obtaining the DLL

Obtain the DLL from your own licensed installation. Sound Canvas VA was discontinued in September
2024.

## Purpose

This is a preservation and interoperability effort on a discontinued product. It exists so that
music written for the Sound Canvas can still be played, on platforms the original plugin never
supported and after it has stopped being sold.

## Compatibility note

BSD 3-Clause is GPL-compatible, so this code can be incorporated into GPL-licensed projects —
including [Cog](https://github.com/losnoco/Cog) (GPL-2.0) — without a separate grant or exception.
Providing a native engine that such a host can embed directly is the reason this port exists.
