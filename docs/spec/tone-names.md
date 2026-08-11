# Tone name suffixes {#tone-names}

Roland's tone names carry a suffix vocabulary. `Piano 1w` is not a different piano from `Piano 1`,
it is the *same* piano with one flag flipped, and the letter says which flag. This page settles what
each suffix means by reading the tone table rather than by reading a manual — every claim below is a
byte in `SCCore.dll` that a sequencer's choice of bank actually reaches.

The melodic tone table is 2363 records of stride `0x100` at file offset `0x18f1810`
(`tone_a.bin`, ts::Tone). The first twelve bytes of each record are the ASCII name, padded with
spaces — which is why suffixes are sometimes crowded (`Tubularbellw`, `Bari.Sax  :L`) and why the
last character of the field is where you look.

Confidence tags follow \ref spec: `[confirmed]` the data plainly says this, `[likely]` strong
inference, `[guess]` plausible but thin.

## The suffixes

| suffix | meaning | what it is in the data |
|---|---|---|
| `.o`, ` o` | **off** — the tone carries a key-off layer | slot-0 partial's block `+0x00` is `0xff` `[confirmed]` |
| `w` | **wide** — stereo spread | partial `+0x6b` is `1` `[likely]` |
| `:` / `:L` | **legato pair** — `:L` is the alternate reachable only in mono/solo mode | an alternate-articulation record, not a tone-table property `[confirmed]` |
| `d` | **detuned** | its own pre-detuned multisample `[likely]` |
| `v` | detuned/doubled electric piano — the GS "Detuned EP" slot | own multisample, plus the `w` flag `[likely]`; the letter's expansion is a `[guess]` |
| `S` / `F` | rotary speaker **slow** / **fast** | `Rotary Org.S`, `Rotary Org.F` `[confirmed]` |
| `Mt` | **mute** | `ConcertBD Mt` `[likely]` |
| `m` | unresolved | `Concert BD m` differs from `Concert BD` in exactly one byte `[guess]` |
| plural `s` | section rather than solo | `Vibraphones`, `French Horns`, `Finger Snaps` `[likely]` |

Everything below is the evidence.

### `.o` — a key-off layer

Ten partials in the whole table have `0xff` at block `+0x00`, and the engine treats that as *fire on
note-off* rather than *release on note-off*: `PartialVoice::note_off`
(`src/realtime/partial_voice.cpp`) consumes the note-off to arm the layer instead of starting a
release. Seven of the ten sit in tones Roland named with the suffix
— `Harpsi.o` (44, 1647), `Clav.o` (53), `Organ o` (99), `Nylon Gt.o` (163, 1499, 1669) — and the
other three are `MandolinTrem`, `Aqua` and `Biwa 3`, which carry the layer without advertising it.

The layer is a *separate tone*, not a free extra on the capital one. `Harpsichord` at bank 0 has one
partial and no release slot; `Harpsi.o` at bank 24 is a two-partial tone whose slot 0 is the key-off
layer. A sequencer has to select it.

No piano has one. The instruments that do are the plucked and the stopped ones — the jack falling
back, the fret release — so the obvious guess that Roland also sampled a piano's damper thump is
wrong.

### `w` — wide

Partial block `+0x6b` is `0` across almost the whole table and `1` on 28 tones. Twenty-two of those
28 are named with the `w` suffix; the other six are `St.Strings`, `St.Strings 2`, `St.Strings 3`,
`St.SlowStr.`, `Old Upright`, and the two `v` electric pianos — i.e. exactly the tones whose *names*
already say stereo. Going the other way, every tone whose name ends in a suffix `w` sets the flag
except one: `Pop Piano w` (13) is byte-for-byte identical to `Pop Piano` (11), flag included, so the
name promises a width the record does not deliver.

What makes this legible is how small the diffs are. `Marimba` (71) and `Marimba w` (72) differ in
two bytes of `0x100`, both of them `+0x6b`. Same for `Vibraphone`/`Vibraphone w`,
`Xylophone`/`Xylophone w`, `Piano 3`/`Piano 3w`. The suffix is one flag and nothing else.

Two caveats. The flag's consumer inside `SCCore.dll` has not been traced, so "wide" here is read off
the naming correlation rather than off a decompiled spread calculation — hence `[likely]` and not
`[confirmed]`; this engine does not implement it. And `w` is *not* always a flag: `HonkyTonk w`
(1640) gets its width the honest way, with its two partials panned to 94 and 34 and coarse-tuned
apart by ten steps, on top of setting the flag.

### `:` and `:L` — the legato pair

Twenty-seven tones end in `:` and twenty-seven more in `:L`, always adjacent and always the same
base name: `Violin    :` / `Violin    :L`, `Strings   :` / `Strings   :L`, `Tenor Sax :` /
`Tenor Sax :L`, and so on through the violins, violas, cellos, string sections, trumpet, french
horn, saxes, piccolo, flute, shakuhachi, and a handful of guitars and basses.

Neither member is reachable by a plain program change. Both are named by a record in the
alternate-articulation table (`layered_1896690.bin`, stride `0x18`, ts::AlternateEntry): its own
12-byte name is the base plus `": "`, its threshold byte is 3, and its two ts::ProgramReference
targets differ only in the map field — `0x75` selects the `:` tone and `0x76` the `:L` tone at the
same bank and program. Those two values are not real tone maps; they are the pseudo-maps that split
the pair.

ts::AlternateEntry documents the gate: an inter-note timing threshold, with the alternate reachable
only in mono/solo mode. That is legato — the second name reads as `L` for the articulation you get
when the next note arrives before the last one is done. The instrument list is the confirmation:
these are the bowed, blown and slid instruments, the ones with a real legato transition, and the
distorted guitars and blip basses, the ones with a slide.

The pair is not a stereo split, which the `L` invites you to assume. `Violin    :L` is a
single-partial record holding what is slot 1 of the two-partial `Violin    :`.

Twenty-seven pairs exist in the tone table but only twenty-five are wired: `Cello     :` /
`Cello     :L` (364, 365) and `Cello Atk.:` / `Cello Atk.:L` (366, 367) are records no alternate
entry names, because the two cello entries point at the duplicate pairs at 1449–1452 instead. The
table itself holds each of its 25 records twice, at index *n* and *n*+25.

### `d` and `v` — the detune bank

`Piano 1d` (1457, 1636) lives at bank 16 program 0 and `E.Piano 1v` / `E.Piano 2v` (1641, 1644) at
bank 16 programs 4 and 5 — the GS variation slots that GM2 names "Detuned Piano" and "Detuned EP 1/2".
None of the three gets there by detuning a partial: each selects its own multisample (755/771 for
the pianos, 398+772 and 419+26 for the electrics) that is already doubled in the wave. The two `v`
tones additionally set the `w` flag.

So `d` and `v` mark the same *kind* of variation, and why Roland spelled the electric pianos with a
different letter is not recoverable from the data. Vibrato is the plausible reading and it is a
guess.

### The rest

`Rotary Org.S` (bank 16) and `Rotary Org.F` (bank 24) are the slow and fast rotary speeds of
`Rotary Org.` (bank 8) — the only place uppercase `S`/`F` carry meaning.

`Concert BD m` (2359) differs from `Concert BD` (1087) in exactly one byte, `p0+0x60` in the TVA
release region, `77 → 29`. `ConcertBD Mt` (1088, 2362) changes that byte and `+0x62` together and is
the one the SC-8820 map actually reaches at bank 9 program 116, so `Mt` is a mute and `m` is
something else — a shorter or longer tail, unresolved. `Concert BD m` is not reachable through any
map at all.

Trailing `s` is just English: `Vibraphones`, `French Horns`, `Finger Snaps`, `808 Claves`. It marks
a section against a solo, not a synthesis parameter.

## Names that are not display names

The drum tone table shares the melodic table — drum sounds *are* melodic tones (see
ts::DrumKitTable) — so ROM names for individual drums sit alongside instrument names. They are never
shown: the module displays kit names, not the name of the tone under one key. That makes them a
private namespace with its own conventions, and reading them as user-facing suffixes will mislead.

- **`B` and `P`** — 31 and 37 tones, used *only* by the four `L/R` kits (`STANDARD L/R`, `ROOM L/R`,
  `JAZZ L/R`, `BRUSH 2 L/R`). Those kits stack three copies of the same drums across the keyboard:
  the `B` set on the GM keys 35–53, an unsuffixed `85…` set from 95, and the `P` set from 107. A
  `P` variant differs from its plain tone in the filter alone — `Room Tom 5` vs `Room Tom 5 P` is
  cutoff `112 → 54`, resonance `64 → 103`, filter type `0 → 4`, and nothing else.
- **`_c`, `_t`, `_i`, `_k`, `_r`** — articulation tags on individual hits (`Ride__c`, `Real6_t`,
  `Jang-Gu_c` / `_k` / `_r` for the Korean drum's three strokes).
- **`82`, `85` prefixes** — which generation of drum wave, not a suffix at all.
- **`R ` prefix** — 65 tones, every one of them belonging to the `RHYTHM FX 3` kit.

Melodic prefixes worth knowing while reading the table: `St.` is stereo, `rev`/`Rev` is a reversed
sample, and `TC`, `LP`, `MG`, `OB`, `JP8`, `D-50`, `SH-101`, `CS` name the instrument being
imitated.

## Reproducing this

Everything here comes out of `tone_a.bin`, `layered_1896690.bin`, the three directory LUTs and the
drum kit records — all already loaded by ts::TableSet, all listed in `assets/manifest.json`. The
useful predicates are: a key-off layer is `partial.raw()[0x00] == 0xff`, the wide flag is
`partial.raw()[0x6b]`, and a tone is legato-paired when ts::PatchDirectory::lut3_raw returns a value
at or above `alternate_space_start`.

`tools/dump_patch_map.py` applies all three and writes the whole directory — every natively assigned
slot in the five tone maps, the 25 legato pairs, the drum kits, and every tone with its `keyOff` and
`wide` attributes — as one JSON document:

```
python3 tools/dump_patch_map.py <SCCore.dll> patch-map.json
```

The output is Roland-derived and gitignored; regenerate it from your own licensed DLL. Its 3448
melodic slots agree slot for slot with `tools/dump_patch_resolution.py`, which resolves the same
lookup independently.
