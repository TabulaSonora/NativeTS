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
- the reverb and chorus coefficients read out of the running engine
- any audio decoded or rendered from the above

With one exception, stated below, none of that is committed here and none of it is redistributed.
`.gitignore` excludes each category. `assets/manifest.json` is tracked because it is a map of
*where* those tables live, not the tables themselves — the same distinction the upstream
[TabulaSonora spec](https://github.com/TabulaSonora/spec) draws.

## The exception: `assets/presets.json`

`assets/presets.json` — about 27 KB of reverb and chorus coefficients, plus the delay preset table —
**is** committed, and it is Roland-derived. That is a deliberate departure from the rule above, and
the reason is that no rule-abiding alternative exists for every host.

Those coefficients are not stored in the DLL. The engine computes them at start-up from the GS macro
parameters, so the only way to obtain them is to run `SCCore.dll` — a 64-bit Windows binary — and
read its state. Anyone on a machine that is not Windows x64 cannot do it without borrowing one.
Shipping the file is what lets the engine have its effects at all on those hosts.

Nothing else changes. The wave ROM, the tables and any rendered audio remain excluded, and the
engine is still inert without a DLL you supply yourself. If you are redistributing this repository
and would rather not carry that file, delete it: the engine treats missing presets as a run-time
condition with instructions attached.

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
