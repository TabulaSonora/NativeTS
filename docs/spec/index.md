# Specification {#spec}

This engine is not a guess at how the Sound Canvas sounded. It is built to a specification
recovered from `SCCore.dll` itself, maintained in the
[TabulaSonora/spec](https://github.com/TabulaSonora/spec) repository, which both this
implementation and the archived C# one were written against.

The pages here are the part of that specification a reader of *this* codebase needs — the
normative structure, the vocabulary, and the file layout:

| | |
|---|---|
| \subpage signal-flow | How a MIDI event becomes sound inside the DLL, stage by stage, with the map from its symbols to these classes |
| \subpage dll-layout | Where the wave ROM, the tables and the patch directory sit in the file, and how VAs become file offsets |
| \subpage glossary | The domain vocabulary — partial, multisample, tone, TVA, TVF, key-follow — for a reader who does not already have it |
| \subpage tone-names | What Roland's tone name suffixes mean — `.o`, `w`, `:L`, `d`, `v` — and which byte of the tone record each one is |

## Confidence tags

The specification tags each finding with how well it is established, and those tags are carried
through here unchanged:

| tag | meaning |
|---|---|
| `[confirmed]` | the code plainly does this |
| `[likely]` | strong inference |
| `[guess]` | plausible but thin evidence |

They matter. A `[guess]` that this engine implements is a place where a future measurement could
change the audio, and knowing which claims are load-bearing is the difference between a bug and a
deliberate approximation. \ref verification sets out what is proven and where this engine knowingly
departs from the reference.

## What is not reproduced here

The specification is larger than these three pages, and the rest of it stays where it is rather
than being duplicated into this site:

- **[FINDINGS.md](https://github.com/TabulaSonora/spec/blob/main/docs/FINDINGS.md)** — the master
  record, over 4,000 lines. It is a *chronological log* rather than a reference document: it
  contains retractions and superseded sections in place, which is exactly what makes it honest and
  exactly what makes it unsuitable for republishing as settled fact. Read it for the evidence
  behind any claim on these pages — and for the findings that have landed in this engine since
  these three pages were curated, the pitch ramp and the filter's stability ceiling among them.
- **[COMPARING_RENDERS.md](https://github.com/TabulaSonora/spec/blob/main/docs/COMPARING_RENDERS.md)**
  — what actually distinguishes a good render from a bad one when sample identity is unreachable.
  It is the argument the DLL-derived tolerance fixtures are built on; \ref verification says where
  they sit.
- **[SYMBOLS.md](https://github.com/TabulaSonora/spec/blob/main/docs/SYMBOLS.md)** — the recovered
  symbol map, 749 of the DLL's 1,045 functions named by address.
- **[PROVENANCE.md](https://github.com/TabulaSonora/spec/blob/main/docs/PROVENANCE.md)** — the
  evidence that `SCCore.dll` is a port of the hardware rather than a fresh emulation.
- **[HARDWARE_ROMS.md](https://github.com/TabulaSonora/spec/blob/main/docs/HARDWARE_ROMS.md)** —
  the SC-8820's physical mask ROMs, and how they correspond to the two banks embedded in the DLL.

Function names throughout are project labels recovered from behaviour, not Roland's own. They are
hypotheses that fit the code, and some may be wrong.
