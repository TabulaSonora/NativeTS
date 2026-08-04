# Verification {#verification}

The engine is checked against **three oracles**, and they are not equal in authority.

**The real `SCCore.dll`** — its own captured internal state (per-voice gain, filter registers,
controller sweeps) and its rendered audio, both taken with the
[spec repository](https://github.com/TabulaSonora/spec)'s `scdec` harness driving the DLL through
its own exported API. Where anything disagrees, **the hardware wins**.

**The reference implementation** — the Python in the same repository — is swept over whole input
domains to produce expected values. This catches the sign-extension, truncation-direction and
integer-width mistakes that a port invites.

**The C# engine** — the archived
[DotNetAdministravit](https://github.com/TabulaSonora/DotNetAdministravit), which this port was
written against phase by phase and reproduced bit for bit. It is scaffolding rather than a target:
it was never an independent witness to the hardware, only a careful one, and where the two now
disagree the DLL decides. Its fixtures are kept for the coverage they still have —
\ref past-the-csharp-engine says exactly how much that is.

## What the port itself is held to

Against the C# engine the gates are digests, not tolerances. Each compares this engine's output to
a fixture generated from the C# CLI:

| gate | tag | comparison |
|---|---|---|
| single note | `[render][sccore][gate]` | SHA-256 of the whole render plus eight literal samples |
| block loop, real time | `[stream][sccore][gate]` | a whole WAV, sample by sample: worst error ≤ 1 LSB, under 0.02% differing |
| predictor stream | `[sampler][sccore][gate]` | against an independent decoder |
| **single note, against the DLL** | `[note][oracle][sccore][gate]` | 239 notes and drum hits: length, level, octave bands, a coarse envelope and the fundamental's tuning, within stated tolerances |
| **whole song, against the DLL** | `[song][oracle][sccore][gate]` | length, level, octave bands and a coarse envelope, within stated tolerances |

Only the real-time gate has any tolerance against the C# engine, and it is one LSB. A fifth,
`[song]`, compared whole songs rendered offline; it was retired with the renderer that produced
them, and the oracle gate above is what replaced it.

Two of those three are now partly historical, and \ref past-the-csharp-engine says why. The note
gate's fixture predates the exact-start wave decode, so every case whose wave begins mid-block is
skipped as **superseded** rather than failed, leaving 111 the fixture can still speak for; the
stream gate has no unaffected case at all, so its references are this engine's own output, kept as
a regression baseline. The archived C# checkout cannot be re-run to refresh either, which is the
whole reason the oracle gates matter.

The note gate has since been *superseded rather than retired*: the sweep it introduced now runs
against the DLL as `[note][oracle][sccore][gate]`, and where the two disagree the DLL decides. The
C# digest is kept beside it because it compares samples, and a one-LSB drift is worth catching even
against a reference that has been overtaken.

The fixtures are Roland-derived and so are not committed. Each pins the DLL's SHA-256 and is
regenerated locally by the scripts in `tools/`; a gate whose fixture index is missing skips with the
command that would produce it rather than failing. The test data itself comes from `TS_SCCORE` and
`TS_TABLES`, or is discovered beside the repository, and anything unfindable is skipped rather than
failed.

## What is proven

These results were established for the C# engine against the reference and the DLL, and carried
over to this port, which reproduced that engine bit for bit — except where
\ref past-the-csharp-engine records that it no longer does:

| | result |
|---|---|
| static tables | all 50 byte-identical to the extracted cache |
| sample codec | bit-exact against the engine's own predictor |
| patch directory | 470 of 512 programs reproduce the engine's observed zones |
| per-voice gain | within **5.4e-05** of the engine's gain word |
| filter cutoff | **0.10%** mean error over a 2.4 s sweep |
| pitch and LFO | exact against the reference, tick for tick |
| pan law | within **3.0e-05** of a measured controller sweep |
| send effects | all 26 networks matched by impulse response |
| full song | ~1 LSB against the reference over 7.9 million samples |

## The goal is audible fidelity, not bit accuracy

Worth stating before the departures below, because it is what licenses them. This applies to the
comparison against the *hardware*, not against the C# engine — that one is exact.

Individual layers are held to bit exactness where the hardware is deterministic: tables, codec,
tick streams. The rendered output is not, and the comparison against the DLL asserts correlation,
level and spectrum against stated tolerances instead.

Part of the residual is inaudible by construction. In a dense passage the few-millisecond amplitude
envelope is dominated by beating between simultaneous notes, which is chaotically sensitive:
onestop's harpsichord section correlates at 0.72 on a 4 ms envelope, rises to 0.91 as the window
widens to 250 ms, and matches the DLL's spectrum within 0.5 dB in every band. Nothing there is
wrong. A metric that looks bad is a lead to investigate, not a defect in itself.

`tools/compare_render.py` computes those correlation and RMS figures. It is a diagnostic for
judging a residual against the DLL, not a test gate — nothing in the suite passes on correlation.

## Where this engine departs from the reference

Four cases, each deliberate, each because the hardware was measured and disagreed.

**The loop's last sample.** The reference stops decoding one sample short of a loop's data end, so
its forward loop substitutes the loop's *first* sample for the last and plays it twice per pass.
The hardware does not.

On long loops this is inaudible. On the SC-55 glockenspiel, whose loops are 60–161 samples, it was
audible as glitchiness — and it also dulled the timbre measurably:

| note | centroid before | after | real DLL |
|---|---|---|---|
| 91 | 6192 Hz | 7333 Hz | 7434 Hz |
| 96 | 7390 Hz | 8857 Hz | 8912 Hz |

This engine follows the hardware: the loop period is inclusive of the data end, asserted in
`sampler_tests.cpp`. Looping is decided by whether a sustain region exists rather than by the
descriptor's loop flag, which is asserted beside it.

**Note-off waits for the control tick.** The reference releases at the note-off sample; the engine
acts on it at its next 100 Hz tick. Measured by sweeping the hold past a boundary — note-off
anywhere in 1000–1008 ms produced the same release, which stepped a whole tick later at 1010 ms,
and one landing exactly on a tick still waited a full one. Releasing immediately runs the tail up
to 10 ms early: inaudible on a pad, most of a short release. Across seven patches this took the
release-onset error from 6.0 ms to 2.3 ms. `ts::SegmentEnvelope::defer_to_control_tick` is the
implementation, and `modulation_tests.cpp` asserts that a note-off landing exactly on a boundary
still waits a full tick.

**The filter envelope's velocity response.** The reference feeds raw MIDI velocity to the depth
scaler. The engine feeds it through one of sixteen response curves selected by `block[0x2e]` —
ts::PartialParameters::velocity_curve here, indexing the `g_vel_sens` rows of ts::TableSet. Row 0
is the identity, so most of the library agrees either way; 13.5% of filtered partials do not.
Brass 1 selects row 1, which reads velocity 100 as 71 — on raw velocity its filter sits about a
third of an octave too open for the whole note, measuring +3.5 dB at 4–8 kHz and +6.3 dB above it.

**A note re-struck under the sustain pedal.** A note-off arriving with the damper down is parked
rather than acted on. The reference leaves that parked entry in place when the note is re-struck,
so the pedal's lift releases the strike the player is still holding. This engine clears it: a new
strike supersedes a note-off the pedal is still holding for that note. onestop.mid's harpsichord
passage rides the pedal every half second over constantly re-struck notes and loses 24 notes to the
reference behaviour — all of them in that passage, none elsewhere in the song, each cut 20–80 ms
after sounding.

\note The last two are enforced by the digest gates rather than by tests named for them. They are
load-bearing for a byte-identical render, so a regression in either shows up as a failed SHA-256,
but the failure will not name the cause.

## What the C# fixtures can still speak for {#past-the-csharp-engine}

Two corrections towards the DLL have taken this engine off bit-exactness with the C# port, and
neither can be carried back: `DotNetAdministravit` is archived, so its fixtures cannot be
regenerated to follow.

**Waves decode from their exact start.** Two in five descriptors — 1,660 of 4,259 — put a wave's
data start partway into a 32-sample exponent block. The codec permits it: it stores differences and
no absolute value per block, so a wave may begin and end mid-block, and the decoder has only to
index the exponents by absolute sample position. Rounding the start down to a block boundary began
integrating up to 31 samples early, and because the predictor has no leak and nothing downstream
blocks DC, those extra deltas displaced the wave for its whole length rather than adding a moment
of lead-in.

The change is measurably *inert* against the DLL everywhere it could be checked — the largest
correlation shift over drum keys and unaligned melodic programs is 2e-4 — so it rests on the
format's semantics rather than on an audible improvement. What cross-checks it is
`tools/dump_predictors.py`, an independent Python oracle of the documented formula, which agrees
with the C++ decode over all 3,703 waves and 25.6 M samples.

**The pitch ramp.** The engine does not step a voice's pitch once per control tick: it records the
pitch entering the block and the pitch leaving it and glides between them, writing a fresh sampler
increment every eight samples. A renderer that only applies post-tick values never sounds a pitch
envelope's start level at all, and the sub-sample residue the glide leaves behind is frozen into
the sampler's phase for the rest of the note. Drum tone 1946 is the case that exposed it — its
pitch envelope drops 6.671 semitones inside block 0 — and against the DLL it moves from a
correlation of 0.859 to 0.99999, taking a +1.11 dB error down to +0.03 dB. ts::PitchRamp is the
implementation.

The fixture fallout was handled by counting it rather than hiding it. The note gate marks the cases
that touch a mid-block wave as superseded. The stream gate has no unaffected case at all, so
`tools/dump_stream_renders.py` generates from *this* engine now rather than from the archived one,
and what it feeds is a **regression baseline**: it catches a change nobody meant to make, and it
cannot catch a mistake both sides share. The gate that has an outside opinion is the oracle one.

## Where the reference departs from its own manual

**Part EQ defaults off, and the SC-8820 manual says on.** The manual's parameter table gives
`40 4x 20` (EQ ON/OFF) a default of `01 ON`. In the reference engine the part reset writes
`part+0x450` to zero, and **nothing anywhere in the binary ever writes one to it** — the only path
that sets it is the SysEx handler itself. A module that is never told to switch the EQ on therefore
never does.

This engine follows the binary, in ts::Part::eq_enabled. The disagreement is silent until a stream
also sends a non-flat `40 02`: a flat EQ is exactly transparent, so with the block at its defaults
both readings sound identical. On a stream that sets an EQ curve without ever addressing
`40 4x 20`, they differ completely — one hears the EQ on every part, the other on none.

\warning This has not been checked against the DLL as an oracle, only read out of it. It is the
one claim on this page resting on absence of evidence rather than measurement, and a file that sets
`40 02` and no part EQ would settle it in a single render.

## The per-note renderer was retired

This project long treated the offline per-note renderer as the authoritative one, and the digests
were taken from it. That reasoning does not survive contact with what you actually hear.

A per-note renderer renders each note whole and independently, then sums. It therefore cannot place
a mid-song change in its true position relative to the notes around it: an effect type that switches
between two notes has to be resolved *per note*, not at the moment it arrives. The block loop has no
such problem, because it is a block loop — the change lands where the stream put it. So on the one
property that matters most for judging fidelity, the architecture that was treated as the reference
is the one that cannot be right.

The polyphony limit was the only remaining reason to prefer it, and that reason is gone:
`ToneGeneratorOptions::unlimited_polyphony` grows the pool rather than stealing, so the block loop
can render every note in a file. The per-note renderer no longer does anything the block loop cannot.

It is now gone. `SequenceRenderer` was deleted with its tests and the `[song]` gate that used it,
and every `render` goes through the block loop — `--stream` selects the hardware's voice limit
rather than a second renderer. What survives under the old name is
ts::NoteRenderer::render_note, which renders one note in isolation and is not a song path at all.

The immediate reason to delete it rather than leave it standing was that it was about to cost
something, and that has since been borne out. The other ten control-matrix destinations — cutoff,
amplitude, the LFO rates and depths — move *while* a note sounds, and a renderer that builds each
note whole could only have taken them as ten more pre-computed curves per note. They were wired
into the block loop instead, where each one is read at the tick that uses it, and none of that
machinery had to be written twice.

### Three tiers of digest, in order of authority

The digests are being re-based onto three tiers, which are **not** equal and should never be
collapsed into one number:

1. **Historical** — the existing fixtures, generated from the archived C# engine. Kept as a record
   of what the old reference produced, not as a target. They are what every gate in the suite
   currently measures against.
2. **Authoritative** — generated from `SCCore.dll` itself, driven through its own exported API
   (`TG_initialize`, `TG_LongMidiIn`, `TG_Process`). This is the real target: agreement with the
   black box rather than with another reimplementation. `tools/dump_note_renders_oracle.py` sweeps
   180 single notes across every tone map and `tools/dump_song_renders_oracle.py` drives the song
   corpus, both through the spec repository's `scdec` harness — natively on Windows, under wine
   elsewhere. **Both halves are live**: `[song][oracle][sccore][gate]` and
   `[note][oracle][sccore][gate]`.
3. **Constrained** — this engine at the hardware's 64 voices, which is the tier directly comparable
   to (2). The gap between them is the measurement that matters, and it answers a question worth
   asking precisely: *how much of the difference is missing features?* Every remaining gap in this
   engine — insertion EFX above all — should show up here as a specific, attributable divergence
   rather than as a vague dissatisfaction.

The unlimited and 256-voice digests sit alongside (3) as regression checks on this engine only.
Nothing in the DLL can produce them, because the DLL has 64 voices.

\note Tier 2 is no longer blocked on the harness. What stood in the way was that `scdec` could
only play a built-in sequence; it now takes an **arbitrary Standard MIDI File** in `smf` mode and
renders a whole sweep of single notes in one process in `notebatch` mode, which is what the two
generators here drive.

**Tier 2 records tolerances, not digests, and that is a property of the problem.** A song runs the
chorus and reverb, whose LFOs the DLL starts at a phase this port cannot yet derive, so two renders
that agree on every note still diverge sample by sample: whole-song correlation against the oracle
sits near 0.18 while every octave band agrees to a tenth of a dB. Sample identity is therefore not
merely unreachable, it is the wrong question. What the gate compares is length, then peak, RMS,
per-octave level and a coarse RMS envelope — the envelope catching a note that goes missing or
arrives late without being sensitive to phase. The oracle audio is kept beside the fixture so a
failure can be measured rather than only counted.

### What the first authoritative measurement says

`onestop.mid` at the SC-8820 map, this engine at the hardware's 64 voices against the DLL's own
render of the same file:

| | |
|---|---|
| length | 29 samples in 7.9 million — the two round the same intent differently, one to the sample and one to the 32-sample block |
| peak | within 0.0009 of full scale |
| RMS | **0.05 dB** |
| octave bands | within 0.5 dB in seven of eight; **1.94 dB** at 8 kHz |
| envelope, worst 1/64th | **1.72 dB** |

The top octave is where the residual lives, which is what one would expect of an engine whose
remaining gaps — insertion EFX above all — are bright. That is the number to watch as those close.

\note The comparison is run at 64 voices deliberately. The DLL has that many and steals; a render
with more of them is measuring a different instrument, however much better it may sound.

### What 239 single notes say

A song averages every patch it touches into eight numbers, so a tone that resolves wrong can hide
behind sixteen that resolve right. `[note][oracle][sccore][gate]` asks the question one level down:
37 programs across the GM map, five keys each, plus nine drum kits on six keys each, at three
velocities and all four tone maps, every one driven exactly as `scdec notebatch` drove the DLL —
same warm-up, same six controllers, same 320-sample chunks with the note-off landing on a control
tick.

| | |
|---|---|
| overall level, median | **0.09 dB**, 90th percentile 0.32 dB |
| octave bands the note reaches, median | **0.17 dB**, 95th percentile 1.8 dB |
| envelope, median worst window | **0.72 dB** |
| programs needing no allowance at all | **27 of 36** |
| drum keys needing no allowance at all | **33 of 54** |

The measurement turned on one distinction, which is worth stating because the obvious version of
the gate gets it wrong. Sorting all 1254 comparable bands by how far each sits below its own note's
loudest band splits them in two. Above 40 dB down, the two engines agree to a median 0.17 dB. Below
it, they disagree by up to 31 dB — because a band 60 dB under the fundamental holds the analysis
window's leakage and the engine's own noise floor, and comparing it measures which engine has the
quieter arithmetic rather than which one plays the right note. Those bands are held to a floor
instead: whatever is in them must stay 25 dB under the note's own level, so an artefact cannot
creep up into audibility unnoticed. **This engine's floor is the higher of the two**, by up to
31 dB in the emptiest bands and worst on `Syn.Bass 1`, where it comes within 29.7 dB of the note.

The nine programs that do need an allowance fall into two groups:

- **Patches that deviate in *time* while their spectrum and level agree.** `Bass & Lead` was chased
  to the bottom, and what it found is in \ref the-engine-plays-sharp below: the two engines beat at
  different rates because their partials are tuned differently. `Nylon Gt.` is a separate defect —
  it decays about 1.5× too fast, a clean monotonic drift of −0.96 dB per envelope window. It is no
  longer alone: six drum tones do the same thing, and \ref widening-the-note-sweep says so.
- **Patches with a noise component** — `Whistle`, `Synth Drum`, `Seashore`, `Atmosphere`. The
  obvious explanation is that the shared pseudo-random source is at a different point when the note
  starts, and that was measured and **is not it**: returning the generator to its seed before every
  case moves these four by hundredths of a dB, and makes `Whistle` slightly worse. The gate does it
  anyway, so that no case depends on the ones that ran before it, but the cause lies elsewhere.

`Whistle` is the outlier of the four by a wide margin — 21 dB in a band the note genuinely reaches
and 2 dB of overall level. It remains unexplained.

### What the sweep could not see, and now can {#widening-the-note-sweep}

This gate was introduced as the instrument for the song gate's low-end lead — whether
`roland_sc88_y03`'s missing bass is a patch rendering wrong or a note never arriving. **It was not
one yet, and the two reasons were both in exactly the place that lead lives.**

The octave bands started at 125 Hz — the band spans 88 to 177 Hz — on the reasoning that most
single notes have nothing below it. That is true and it was the wrong band to leave out: the sweep's
lowest key sounds at 65.4 Hz, so its fundamental fell outside every band the gate measured. And
every case was melodic, so the drum kits were unreached by any number of them — a program change on
channel 10 resolves through its own pair of lookups into its own table, with the kit's coarse-pitch
plane supplying the key instead of transposing the sample, and none of that is exercised by a
melodic note. The bass drum is the one sound in a GS arrangement with real energy under 90 Hz.

The sweep now carries a 63 Hz band and 54 drum cases: nine kit programs — every one of which all
four tone maps define, and each of which resolves to a *different* kit record on each map, so 36
kits are covered — across six keys chosen for their mechanisms rather than their names.

**Eighty-one cases now have real content at 63 Hz, where none could be measured before.** The
drum half is broadly right: forty of the fifty-four agree on level to under half a decibel, and 33
of them need no allowance at all. The twenty-one that do say four things.

- **Crash cymbals decay too fast, and then stop dead.** Six cases, six kits, three distinct tones.
  `Crash Cym.1` tracks the module 2 to 4 dB low all the way down and then reaches *exactly* zero at
  1.84 s while the module is still sounding 35 dB under its own attack.

  \warning **That diagnosis was wrong, and \ref the-drum-ring-was-invented says what it actually
  was.** The envelope was never the problem: this engine builds exactly the module's, target for
  target and millisecond for millisecond. It was a ring timer this port had invented.
- **The Orchestra kit's timpani**, on all three keys the sweep plays it at: the attack arrives
  4.7 dB low at half the module's peak, and the tail then runs 12 dB long. Every other kit key is
  either right or slightly light; this one is quiet and then loud. It is also the sweep's clearest
  test of the kit coarse-pitch plane — the Orchestra kit tunes the timpani per key, so its `pitch`
  byte is 42, 45 and 46 against a neutral 60, while almost every other case sits at neutral.
- **Two SFX kit entries** — `Pick Scrape` 4 dB loud, `Gt.CutNoise` 10 dB quiet.
- **Hi-hats, where the module has a low-frequency floor this engine does not.** The module's
  `TR-808 CHH` reads −46 dB at 63 Hz against a −34.8 dB loudest band; this engine reads −147, which
  is silence. Neither carries a hi-hat down there. This is a real limit of the relative-band rule:
  the hat's whole spectrum is only 26 dB wide, so nothing 40 dB below its loudest band exists for
  the split to put on the floor side, and the comparison measures two noise floors against each
  other. The bounds those cases carry are the ugliest in the file and the comment says why.

One case is silent on both sides and is checked for it rather than skipped: key 36 of the SFX kit at
the SC-55 map is an **undefined kit entry**, tone `0xFFFF`, and the module answers with a peak of
0.00003 — its own noise floor. The rest of the sweep starts at 0.004, so the line between them is
drawn across an empty gap. An undefined kit key that started sounding would be a real defect, and
this is the only case that could catch it.

#### What it says about `roland_sc88_y03`

The file this was meant to answer for is about 6.5 dB light at 63 Hz. Reading the sweep against it:

| | |
|---|---|
| melodic cases with real 63 Hz content (52) | median **−0.13 dB**, worst −11.8 (`Seashore`, unpitched) |
| drum cases with real 63 Hz content (29) | median **−0.94 dB** |
| bass drums specifically (8) | **−0.51 to −3.23 dB**, median −0.88 |

**Nothing that carries bass is anywhere near 6.5 dB light.** The worst kick in the sweep is the
TR-808's at −3.2 dB and the median is under one. The only deficits past 3 dB at 63 Hz belong to
hi-hats and to two unpitched noise patches, none of which put a note there.

Reading the file itself sharpens it further. `roland_sc88_y03` drives **drum program 48 — the
Orchestra kit**, the one kit the sweep flags as badly wrong, and it plays the timpani range 36
times. But its low end is not the timpani: 324 of its notes inside the 63 Hz band are
**`Acoustic Bs.`, program 32, on channel 9**. That program was not in the sweep, so it was added —
which is why it sits out of order at the end of the list, the velocity and tone map a case gets
being a rotation over that list's index.

**It renders within 0.2 dB of the module at every one of its five keys, and −0.67 dB at 63 Hz on the
lowest.** So the file's dominant bass source is right, its bass drum is right to 0.7 dB, and the one
kit key it plays that is badly wrong is a timpani it strikes 36 times out of 810.

There is a mild systematic bias worth recording — drums run about a decibel light at 63 Hz where
melodic notes run a tenth — but a decibel is not six and a half. Nothing this sweep covers renders
quiet enough to explain that file, which leaves the other reading: **some of its bass is not
arriving.** That is a question about note handling, not about patches, and it is the next place to
look.

### The drum ring was invented {#the-drum-ring-was-invented}

The sweep above reported that crash cymbals decay too fast, and pointed at the amplitude envelope's
segment rates because six unrelated tones did the same thing. **The envelope was never wrong.** The
way to find that out was to stop inferring it from audio and go and read it.

`scdec tvatrace` plays one note and lifts the amplitude envelope straight out of the module's voice
before any of it has run — the four segment targets `tva_compute_env_levels` writes to
`voice+0x16/0x1d2/0x1d4/0x1d6`, and the four durations `tva_compute_env_rates` writes to
`voice+0x12/0x1c6/0x1c8/0x1ca`. For `Crash Cym.1` on the SC-88 map at velocity 127 the module says
targets `49163, 49163, 0, 0` and durations `0, 302, 4672, 0` ms.

This engine builds `49163, 49163, 0, 0` and `0, 302, 4672, 0`. **Identical.** So is `GS Crash`, and
so is every other case checked. The decay law — two key-follow tables, two velocity level-scales,
the three part biases, the `< 9` skip, the `0xa0000/duration` step — is right.

What was wrong is that the voice never got to run it. A drum here was force-released after a fixed
**1.8 seconds**, a number this port made up, and the module has nothing of the kind. A crash whose
envelope the module runs for 4.97 s was being cut off at 1.81 — which is exactly where the render
goes to digital zero.

The module's actual rule is per key, and it is in the kit record. `DrumKitTable` read five planes of
a 0x50C-byte record; the sixth, at **0x480**, is GS's `Rx.Note Off`, and bit 0 says whether that key
answers a note-off at all. Reading it back is the whole argument: in the SC-88 Standard kit exactly
**one** key sets it — key 25, the snare roll. The Orchestra kit adds key 88, Applause. The SFX kit
sets it on 52 of 128. That is the documented GS behaviour, found in the ROM rather than assumed, and
a fixed timer was standing in for it.

So a drum now ignores note-off unless its key says otherwise, and the voice ends when its own
envelope reaches silence — which is what `SegmentEnvelope::is_finished` had no way to express, since
it only knew about releases. The ring survives as a backstop for the one case the envelope cannot
end on its own: a last segment whose target is above zero, which would otherwise hold a voice
forever.

| | before | after |
|---|---|---|
| drum rows needing an allowance | 21 | **18** |
| drum rows improved / worsened | — | **14 / 3** |
| `Pick Scrape` band error | 37.9 dB | **closed** |
| `808 Kick` | a row | **closed** |
| the two 940 dB envelope markers | 940.96, 939.41 | **gone** |

`Pick Scrape` is the confirmation worth having: it is one of the 52 SFX keys that *does* answer a
note-off, so the fix moved it the other way — it now stops when told to instead of ringing for the
timer — and 38 dB of band error went with it.

On whole songs it is a trade, and the rows record both halves. **RMS improved on eight of the nine
songs it moved at all**, and `roland_allstars`'s band error closed; peaks rose a little on three,
because more drum voices now overlap, and `onestop`, `macross2` and `bigben` each needed a widened
row. Widening a ratchet is not free and those three say why they moved. Everything that improved was
tightened in the same commit.

### The module has a DC offset, and we do not {#the-module-has-dc}

Chasing the crash turned up something else, which is not fixed and is worth stating precisely.

The module's rendered output carries a **large negative DC offset that scales with the envelope** —
so it is in the signal, not added downstream. On `Crash Cym.1` the mean is −0.68 of the RMS,
present from the first sounding sample. This engine's is −0.008.

It is not general: across 238 sounding cases the median is 0.4% of RMS for the module and 0.07% for
this port. It is **eight cases**, and they are the six crash cymbals and — this is the useful part —
**`Whistle`**, the melodic sweep's sharpest unexplained lead. Two of the gate's open leads are one
lead.

It also inflates what the level metrics say about those cases, because RMS and the RMS envelope both
include DC. Measuring the AC content alone:

| | with DC | AC only |
|---|---|---|
| `Crash Cym.1` | −2.55 dB | **−1.63 dB** |
| `Whistle` | −2.23 dB | **−1.47 dB** |

So roughly a third of what those rows call a level error is a DC term this engine does not
reproduce. The bounds are left where the measurement puts them — the gate compares what the module
actually outputs — but the cause is now named.

**The sine kick is the same DC, heard.** `transcendental.mid` builds its drum beat by hand: the
SC-55 map's `Sine Wave` (bank 8, program 80, bend range 24) struck with a `+24` bend that dives
four octaves per hit. Soloed against the module, each of its two kick channels renders **3.63 dB
light** here, and the whole song reads 3 dB light in the song gate. Chasing that as a control-slew
difference went nowhere for a measured reason: `tvftrace` shows the module's per-voice state at
this patch is *identical* to this engine's — cutoff units 133916 at key 39 and 171208 at key 60 to
the unit, pitch-ramp targets exactly ±2 octaves, cutoff unmoved by bend. The per-voice control
ramps are not a gap either. `voice_set_ramp_target_0` sets `step = ((target − current) × rate) >>
13` stepped per sample toward a clamp at the target, the divider masks are `{0, 7, 31, 127}` from
bits 12–13 of the rate word, and the pitch path's rate word is `0x4FFF` — rate 4095, mask 0 —
which converges in about two samples. The module's bend is as instant as ours.

What differs is the output. At key 39 the patch's lowpass sits at ~45 Hz, so the bent-up sine at
311 Hz is 36 dB down in **both** engines — but the module's render is dominated by a **0 Hz
component** the filter passes untouched, an envelope-shaped DC transient per hit. That thump *is*
the kick, and it is the same in-signal DC this section measures on the crashes and `Whistle`,
reaching audibility through a filter narrow enough to leave nothing else. One cause now stands
behind the crash rows, the `Whistle` lead, and the loudest surviving song-gate rows.

Two more measured leads from the same pass, both unfixed:

- **A one-entry offset in the envelope shape**, now corrected. `env_ramp_segment` interpolates from
  the table entry it lands on *up* to the next one, weighted by the complement of the phase
  fraction; this port read the pair downward, weighted by the fraction. That runs the whole curve
  about 0.4% of a segment ahead — a flat **0.38 dB** wherever the shape is steep. `g_env_shape` is
  258 entries rather than 256, and the extra two exist for precisely the `shape[k+1]` the module
  needs at `k = 255`, which is what settles the reading.
- **This engine starts every note 128 samples early**, and \ref the-midi-pipeline-costs-128-samples
  says why.

### The MIDI pipeline costs 128 samples {#the-midi-pipeline-costs-128-samples}

The module's first sounding sample lands at **exactly 128 after the note-on, in all 238 cases** —
not a median, not a spread, the same integer every time. This engine's lands at 1. Measured by first
departure from the idle level rather than by a fraction of the peak, so the attack rate cannot
contaminate it.

It is not an artefact of how the gate drives either side. `scdec onsetprobe` sweeps the render chunk
size across 32, 64, 128, 160, 320 and 512 frames and the answer is 128 every time, so it is not
block-boundary quantisation of the caller's calls. Sweeping the frames rendered *between the program
change and the note-on* is what shows the shape: at 0, 16 and 32 frames the onset lands at a fixed
128 samples from the program change, and from 64 frames on it is 128 samples from the note-on.

`TG_Process` says what it is. The MIDI ring is drained **after** `render_block()`, and each queued
event carries a timestamp gated against a block counter that only advances once per rendered block.
The decompilation's own traced chain has the event crossing four stages — the ready buffer, the
per-port FIFO, the parser state machine, then `part_start_voices` — and each is advanced by one
per-block callback. Four blocks of 32 samples is 128.

Cross-correlating the two renders confirms it end to end: `Syn.Bass 1` peaks at **r = 0.92 at a lag
of 128–129 samples**, so our render is very nearly the module's, advanced.

The other host rates are the same latency seen through the module's resampler — it runs at 32 kHz
internally and converts on the way out, so 64 kHz reads 255 and 16 kHz reads 48. At 32 kHz, which is
what this engine renders at and what both gates compare, the number is 128 and it is exact.

**It is not implemented, and that is a decision rather than an omission.** Modelling it faithfully
means queueing events against a block counter the way `TG_Process` does — which makes
`ToneGenerator::send_channel` asynchronous, so a program change no longer takes effect before the
next call returns. A good deal of the suite reads state back immediately after sending, and every
one of those would have to render four blocks first. The equivalent shortcut, delaying the output by
128 samples, is exactly the same thing for a time-invariant engine and is one line — but it buys the
alignment by asserting a latency the engine does not actually have.

### The engine plays sharp {#the-engine-plays-sharp}

`Bass & Lead` deviates in the envelope and nowhere else, which looked like an LFO starting at a
phase this port cannot derive — the same thing the song gate names for the effect LFOs. It is not.
Tracing the module's own LFO object with `scdec lfotrace` shows LFO1 running at 6 Hz with a TVA
depth of 682 against a 0x7F00 full scale: 0.18 dB, far too shallow to see, and far too fast to
survive an 87 ms RMS window in the first place.

What actually modulates is **beating between the patch's two partials**, and the two engines beat at
different rates:

| | module | here |
|---|---|---|
| partials near C6 | 1043.8 and 1045.5 Hz | 1045.9 and 1048.3 Hz |
| spacing | 1.7 Hz | 2.4 Hz |
| envelope modulation, over a 6 s note | 1.75 Hz | 2.33 Hz |

The spacing and the modulation agree in each engine, which is what identifies the mechanism. The
spacing differs because *both partials are sharp* — by 3.5 and 4.6 cents — and unequally, so the
interval between them opens up.

That is not confined to one patch. Comparing the fundamental of every pitched case in the sweep
against the module's own render of the same note:

| | |
|---|---|
| single-partial cases, note ≥ 72 | median **+2.47 cents**, 11 of 51 within a cent |
| multi-partial cases | median +3.47 cents, standard deviation 1.0 |
| measured over 0.0–0.5 s | +2.67 cents |
| measured over 0.5–1.0 s | +2.48 cents |

Holding across both halves of the note rules out a pitch-envelope attack artefact: it is steady
tuning. The direction is consistent — this engine is sharp, the module flat of equal temperament.

**It is not the shared formula.** `scdec postrace` reads the module's sampler read position per
control tick, so the module's playback ratio is exact — an integer step in 16.16. Compared against
`2^((base_pitch − native)/12000)` computed here for the same note:

| patch, map | module's ratio | ours | |
|---|---|---|---|
| `Trombone`, SC-88Pro | 1.079414431 | 1.079415269 | **0.001 cents** |
| `Piano 1`, SC-8820 | 0.558837891 | 0.559063217 | 4 milli-semitones |
| `Fingered Bs.`, SC-88 | 0.790908813 | 0.792326339 | 31 milli-semitones |
| `Clarinet`, SC-88Pro | 0.943016603 | 0.945238305 | 41 milli-semitones |

One patch matching the module's own ratio to a thousandth of a cent rules out an error in anything
every patch shares — the `1024` neutral in `native = root_key × 1000 + 1024 − fine_tune` included,
which was the leading candidate. **Whatever is wrong is a per-patch term.** Read the trace one tick
at a time: a longer baseline crosses a loop wrap and the read position jumps backwards, which turns
a sound measurement into a wild one.

**It is the native pitch, and nothing else.** The error was localised by elimination, every step
measured rather than argued:

| candidate | verdict |
|---|---|
| the `1024` neutral, or anything shared | `Trombone` matches the module's ratio to 0.001 cents |
| pitch start jitter | **depth is zero for every partial in the sweep** — it never fires |
| pitch-envelope sustain | **zero for every partial** |
| per-partial coarse tune | neutral on 162 of 177 cases, which still miss by a median 2.47 cents |
| the estimator, confused by vibrato or beating | no-vibrato single-partial cases still miss by a median 1.88 cents |
| a loop off by one sample | error × loop length is nowhere near a multiple of 1731 cents·samples |
| the base-pitch chain | see below |

That last one is the decisive measurement. `scdec portatrace` reads the module's *own* computed
pitch out of `voice+0x6c`, so it can be compared against `base_pitch` directly instead of inferred:

| patch | module's `voice+0x6c` | our `base_pitch` | |
|---|---|---|---|
| `Sweep Pad`, `Vibraphone`, `Nylon Gt.` | 59989 / 72003 | 59989 / 72003 | **exact** |
| `Piano 1` | 59988 / 72004 | 59989 / 72003 | 1 milli-semitone |
| `Harpsichord`, `Church Org.1` | 59994 / 72002 | 59989 / 72003 | 5 milli-semitones |

The base chain agrees to within half a cent and is exact in half the cases, while the whole error
reaches 45 milli-semitones. Everything else having been ruled out, the residual is in
`native = root_key × 1000 + 1024 − fine_tune`.

**The missing term is per *wave*.** Recovering the module's `native` for all 127 single-partial
cases — `native_ours + 10 × the measured cents`, which the `base_pitch` agreement above licenses —
and grouping by the wave each case resolves to:

| wave | residual, milli-semitones |
|---|---|
| `Vibraphone` root 59 | +3.0, +3.0 |
| `Fretless Bs.` root 90 | +17.3, +17.5 |
| `Sweep Pad` root 55 | +16.8, +17.6 |
| `Trumpet` root 56 | +56.0, +56.9 |
| `Sitar` root 61 | −82.1, −79.7, −75.5 |

Across the thirteen waves the sweep hits more than once, on different keys and different tone maps,
the **median spread within a wave is 3.4 milli-semitones** — a third of a cent. Each wave has its
own constant offset, and it is a function of neither `root_key` nor `fine_tune`: waves sharing a
`fine_tune` disagree, and root 60 alone spans −9 to +47.

That pointed at the eight descriptor bytes this port never reads — `0x0E` to `0x15` — two of which
form a second 16-bit field whose most common value is `0x0400`, the same neutral 1024 as
`fine_tune`, on 3,814 of 4,259 records.

**That lead is dead, and the DLL's own code says so.** `partial_compute_pitch @ 18005fc20` computes
*two* native pitches:

```c
voice+0x1fc = root*1000 - fine + 0x400                       // what this port implements
voice+0x200 = (voice+0x1fc) - *(ushort *)(desc + 0x0e) + 0x400
```

So the second field is real and is exactly a second fine tune — but `voice+0x200` is **written and
never read anywhere in the binary**, while `voice+0x1fc` is what `voice_pitch_keyfollow` subtracts
to form the exponent. This port's `native` is character-for-character the module's, and the missing
term is not in the descriptor.

\warning **The cause is still unknown.** Reading the real pitch routine has ruled out the formula
itself, the second fine tune, and the jitter (below), and the base chain was already measured to
agree within 5 milli-semitones. The spec's note says why nobody had pinned it down: it records this
formula as *"verified to ~0.5%"*, and half a percent is 8.6 cents, the exact range of what is left.

Two things worth following from that routine, neither of them the sharpness.

It selects the key-follow row as *row 2 when the partial node's `+0x169` is zero, otherwise
`+0x168`, skipping the curve entirely when that is zero* — which is not this port's
`clamp((block[0x13] − 0x40) >> 2, 0, 7)` applied unconditionally.

And it reads the pitch-jitter depth from **block +0x12**, immediately after the coarse tune at
+0x11. This port reads +0x1a, on no authority anyone recorded, and the ROM agrees with the
decompilation: +0x12 is non-zero on 223 of the 4,726 partial blocks with nineteen distinct depths,
+0x1a on nineteen blocks with two.

\warning **That correction is not applied, because it makes the song gate worse.** With +0x12,
`robyn_show_me_love` goes from inside the default 0.01 peak bound to 0.036 outside it and `rainy`
breaches its row, while the level, spectrum and envelope of both stay passing. One fragile
sample-level metric moving on two songs and nothing else points at the *draw order* rather than the
byte: jitter on 223 partials instead of 19 puts many more voices on the one shared generator, so
any disagreement about which voice draws when becomes audible. The byte is a one-character change
waiting on that question; making it now would trade a known-wrong constant for wrong-sounding
songs.

### Now it is measured

The gate compares the fundamental of each render against the module's, in cents, and holds it to a
ratchet like everything else. Adding it is the actual repair available today: the cause is not
isolated, but the defect can no longer grow, and the next person to look has an instrument.

| | |
|---|---|
| comparable cases | 177 of 180 |
| median | **+2.21 cents** |
| within one cent | 25 of 177 |
| worst | `Tenor Sax` at 23 cents, on the 65 Hz key alone |

Part of that spread is the measurement, not the engine: a 0.5 Hz bin is 13 cents wide at the
sweep's lowest key, so the estimator is least able to speak exactly where the notes are lowest.
`Synth Drum` and `Seashore` have no fundamental at all, and their rows say so — a bound of 110
cents is not a check, and saying that out loud beats skipping them where nobody would notice.

Two things about *how this was missed* are worth keeping. **No gate in this project measured
pitch.** Level, spectrum and envelope all pass: 2.5 cents is far inside a third-octave band, moves
no RMS, and changes no envelope — except on a patch where it changes a beat rate, which is the only
reason it surfaced at all. And the note gate found it only because a single note has one
fundamental to measure; a song has sixteen parts and no measurable pitch at all.

### Measuring the control matrix against the module

The eleven destinations were closed one at a time and each was measured against the DLL before it
was believed. Three things about the method are worth keeping, because the obvious version of each
does not work.

**Measure differentially.** The same probe file is rendered twice per engine — once with the
destination assigned to the mod wheel, once with that route switched off — and what is compared is
the difference. A patch's own envelope drifts across a ten-second note by more than most of these
destinations move, so an absolute measurement is swamped by it; the difference cancels it exactly.

**Step the source, don't just deflect it.** Each file holds one note and steps the wheel through
0, 32, 64, 96 and 127, so every destination produces a curve. This is what caught the one real bug:
against `40 21 00` at depth 0x7f the module ramps evenly to 24 semitones across the whole wheel,
and this engine reached the rail by 64 and then sat still. The endpoints agreed. Only the shape
disagreed — a depth byte stored raw where `sysex_part_control_matrix` clamps it to 0x28–0x58, which
is what makes the pitch law's clamp the top of the scale rather than a rail a real stream can hit.

**The wheel is never inert.** `40 2x 04` starts at 0x0a, so moving the wheel adds vibrato whatever
else it is assigned to. That route leaks into every probe and has to be switched off or accounted
for; mistaking it for the destination under test is the easy way to "verify" nothing.

What the module and this engine produce, at the wheel's five steps:

| Destination | Measured | Module | Here |
|---|---|---|---|
| pitch | pitch, cents | 0 / 608 / 1210 / 1817 / 2400 | 0 / 605 / 1210 / 1814 / 2400 |
| amplitude | level, dB | 0 / 2.0 / 3.6 / 4.9 / 6.0 | 0 / 1.9 / 3.5 / 4.9 / 6.0 |
| lfo1_rate | vibrato rate, Hz | 5.86 / 7.81 / 10.74 / 12.70 / 15.62 | identical |
| lfo1_pitch | vibrato depth, Hz | 0.3 / 42.0 / 85.7 / 130.2 / 173.6 | 0.2 / 42.4 / 85.3 / 129.9 / 173.8 |
| lfo1_tva | tremolo depth | 0.008 / 0.014 / 0.024 / 0.035 / 0.044 | 0.005 / 0.013 / 0.025 / 0.037 / 0.047 |
| lfo2_rate | vibrato rate, Hz | 1.95 / 3.91 / 6.84 / 9.77 / 11.72 | identical |
| lfo2_tva | level, dB | 0 / 0.1 / 0.4 / 0.6 / 1.2 | 0 / 0.1 / 0.4 / 0.8 / 0.7 |
| tvf_cutoff | brightness, dB | 0 / −0.3 / 0.8 / 0.8 / 0.8 | 0 / −0.5 / 0.8 / 0.8 / 0.8 |

Amplitude's +6.0 dB at full deflection is the doubling the law predicts, reached from the other
direction. Both rate destinations land in the same analysis bin as the module at every step. The
three destinations not in the table are the thinly-measured ones under Known limits.

### Measuring a random modulation

The three random LFO shapes cannot be checked the way the rest are, because there is no value to
agree on: the point of the parameter is that the value is drawn. What *can* be checked is
everything around the draw, and each part needs its own measurement.

**The timing is exact.** A held note on a patch whose LFO2 is a sample-and-hold, with the matrix
driving its pitch depth to full scale, gives a pitch track that sits flat and steps. The module and
this engine step at the same moments — both plateau, both break at t ≈ 336 ms into the window, both
carry the same 5 Hz LFO1 vibrato on top of the plateau. That is the redraw-on-wrap law and the
rate, and it either matches or it visibly does not.

**The depth matches, measured as spread rather than extent.** Peak-to-peak of a random signal is
itself random and says little — the two engines' figures scattered between 4 and 168 cents across
one sweep. The standard deviation converges:

| | Module | Here |
|---|---|---|
| pitch spread, route assigned | 272.9 cents | 273.0 cents |
| pitch spread, route switched off | 21.7 cents | 22.0 cents |

Four tenths of a percent on a quantity built entirely out of random draws. The control row is the
part that makes it meaningful: with the route off, both engines fall back to the same small default
vibrato, so the assigned row is measuring the random shape and not the patch.

**The values differ**, and Known limits says why. Nothing in this section depends on them.

### What the LFO delay does and does not hold back

Wiring the matrix into the LFO depths exposed a difference that had been invisible while the mod
wheel was the only controller reaching them. An LFO's delay gates its fade-in, and the fade scales
the *patch's* depth; a controller's depth is summed after it. So during the delay the module
modulates at full controller strength, and this engine — which suppressed the whole update until
the delay elapsed — did not.

Measured on program 43, whose LFO1 delay runs 710 ms, with the mod wheel driving that LFO's
amplitude depth. Tremolo depth as the standard deviation of the level track with the note's own
envelope detrended away:

| Window | Module | Before | After |
|---|---|---|---|
| inside the delay, 0.2–0.6 s | 14.73 | **0.51** | 14.31 |
| across it, 0.6–0.9 s | 13.72 | 10.03 | 12.83 |
| after it, 1.0–2.0 s | 12.12 | 12.12 | 12.00 |

The bottom row is what makes the top row a finding rather than a mismeasurement: the depth law was
already right, and agreed to one per cent as soon as the delay was out of the way. Only the gate
was wrong.

Two method notes. The probe uses the **amplitude** destination rather than pitch, because a
600-cent vibrato defeats every cheap pitch estimator — the tracker jumped between harmonics and
reported noise — while a tremolo rides on a level track that is easy to measure. And the level has
to be detrended, or the note's own attack envelope dominates the window that matters most.

### Choosing a corpus by coverage, and what widening it found

The authoritative gate ran on **one file** for a long time, and one file cannot tell you what it is
not exercising. `tools/scan_midi_archive.py` picks a corpus by coverage instead of by ear: it
byte-scans an archive for Roland GS SysEx, parses the shortlist to see what each file would
actually drive, and runs a greedy set cover. Over a 128,000-file archive it found 20,688 files
carrying GS SysEx, of which **288 genuinely drive the control matrix** — and those 288 assign every
one of the eleven destinations from every one of the six sources, including the three destinations
that could previously only be measured thinly. Ten files cover every route they use.

Beside them sit seven of Roland's own demonstration disks, at the map each was written for. Those
are the general backbone rather than a feature hunt: densely and competently sequenced by the
people who built the module, so they exercise ordinary playing in a way a file selected for one
SysEx address does not. One of them touches 43 programs.

The corpus went from one file to eighteen, and the first thing it found was a fault in the
**measurement**, not the engine. The harness drives the DLL through a single port; this engine
defaults to two, which spreads a multi-port file's tracks over thirty-two parts instead of folding
them onto sixteen. That is a different arrangement of the same notes and not a comparison at all.
It could not show up while the corpus was one single-port file. The first multi-port song added put
the low band 10 dB out and the level 4 dB down; pinning the gate to one port took the worst band to
1.3 dB.

What remains is recorded per song in `known_deviations`, as a ratchet: each bound is the measured
deviation plus a little headroom, so nothing may get worse and closing one means tightening its
row. `TS_STRICT_SONGS=1` holds every song to the defaults, which is how a row's current deviation
is measured when it is due to be tightened.

**Three of those rows were mis-attributed, and the correction is the more useful lesson.**
`shangai`, `macross2` and `ff5_1_16_harvest` all *assign* the matrix's CC1 or CC2 sources, so while
those sources were unimplemented it looked obvious that this was why they deviated. Implementing
them — correctly, and verified against the module — moved none of the three by a hundredth of a dB.
Reading the files says why: `macross2` assigns both routes at depth 0x40, the neutral value, so the
file switches them off itself; `shangai` points both at CC#2 and never sends it; and
`ff5_1_16_harvest` does drive its route, 4,335 times, but on one channel where a cutoff sweep
cannot reach the 63 and 125 Hz bands that are what deviates. Assigning a route is not driving it,
driving it is not driving it *audibly*, and a scan that records the first is evidence of neither.
`tools/scan_midi_archive.py` now reports whether the assigned controller is ever sent.

The sharpest remaining lead is `roland_sc88_y03`, about 6.5 dB light at 63 Hz and 5 dB at 125 Hz
*by the same amount at every tone map*, so it is not patch resolution: some bass is not arriving.
The note gate has since been widened until it could speak to this, and it agrees — every sound the
file leans on down there renders within about a decibel. See \ref widening-the-note-sweep.

That is the argument for coverage over taste. One file said 1.94 dB in the top octave. Eighteen say
where to look.

### What the digests should be

Three per file, all from the block loop, because one number cannot say both things:

| Polyphony | What it proves |
| --- | --- |
| **64** | the hardware's limit, so that voice stealing is *consistent* — the audible character, not a defect |
| **256** | enough headroom that no ordinary file steals, catching anything that depends on the limit |
| **unlimited** | uninterrupted rendering, where every note in the file sounds |

The 64-voice digest is the one that should eventually be comparable against `SCCore.dll` driven
through its own API, once the engine is feature complete. That is the real target: not agreement
with another reimplementation, but agreement with the black box.

The principle behind all three: **match the original library by default, and exceed it only on
request.** A mode that sounds better than the module is a feature to opt into, never the baseline.

Two observations from the current corpus, whose shape outlives the particular digests — the wave
decode correction moved every one of them. `canyon.mid` and `sc50nn.mid` produce the *same* digest
at all three settings: neither ever exceeds 64 voices, so for them the three-way check is free but
uninformative. `th07_19_user_gm.mid` (173,183 notes) differs at 64 and agrees at both 256 and
unlimited — it steals heavily at the hardware limit, and 256 is already enough for it. That the top
two agree bit for bit is worth having: it says the growing pool converges on what a large enough
fixed pool reaches rather than doing anything of its own. `render` reports which case a file falls
into, so this does not have to be guessed at.

## Known limits

Stated plainly, because they are not covered by the numbers above:

- **Voice stealing is an approximation.** The original's allocator was located and named during
  reverse engineering but its selection rules were never traced. The policy in ts::VoicePool — free
  slot, then oldest releasing note, then oldest held note — is an invention, isolated so it can be
  replaced without touching any DSP. The test that covers it says so in as many words.
- **The LFO has no hardware trace.** It is verified against the reference, which the spec project
  separately reports as bit-exact against the live engine. That is one link removed from the DLL.
- **Insertion EFX is out of scope**, as it is upstream. The 67-algorithm subsystem is not
  implemented; the GS SysEx that selects it is parsed and dropped. The groundwork is in:
  `tools/dump_efx_table.py` reads the DLL's own directory of all sixty-five effects by name and
  points at the algorithm and parameter handler behind each. The names are Roland's, so they are
  decoded from your own copy rather than committed here.
- **Drum tones with the 4-partial layout are not reversed** upstream. A melodic tone here has two
  partial slots (ts::Tone::partial_slots), and asking for more throws rather than guessing. General
  MIDI kits resolve to ordinary melodic tones, so the common path works.
- **The random LFO waveforms do not draw the same numbers as the module**, though they now draw
  from the same generator by the same law. Shapes 1, 2 and 3 are implemented (see below); what is
  not reproduced is *which* draw a given voice gets. The generator is one shared sequence and every
  consumer advances it — the LFO shapes, the pitch start jitter, the random pan — so aligning the
  values would mean matching the module's consumption order voice for voice, which is an accident
  of its allocator rather than a behaviour. The shape, rate, step timing and depth are right and
  the particular numbers are not, which is the intended side of the "audible fidelity, not bit
  accuracy" line: a random modulation that steps at the right moments to the right *sort* of value
  is the parameter working.
- **All six of the control matrix's sources are now consumed**, including the two assignable
  controllers. Their numbers come from `40 1x 1F` and `20`, default to General Purpose 1 and 2, and
  are clamped to 0-95 — measured, not assumed: assigning 100 and then sending CC#95 modulates on
  the module while sending CC#16 does not, so it clamps rather than rejecting the assignment.
  Pointing a source at a controller does not take that controller's other meaning away. All eleven
  destinations are consumed, from all six sources — the mod wheel, both aftertouches and bend. CC1 and CC2 need
  their assignable controller numbers tracked first, so every destination's clamp sees a smaller
  total than the module's would with all six deflected at once. That difference only shows at the
  rail.
- **Two destinations are wired but thinly measured.** The two LFOs' filter depths agree with the
  module wherever they could be made to move, but no probe was found that drives them hard enough
  to be conclusive — the patches whose filters are open enough to hear a cutoff sweep barely change
  brightness under one. LFO2's pitch depth was in this list until the random waveforms were
  implemented, which is what made a patch that could demonstrate it available at all.
- **Some GS part parameters are recognised and dropped**, each because nothing under them is
  modelled: assign mode (`40 1x 14`, one voice-allocation policy here) and per-key Rx note-on/off
  in the drum setup (`40 2x 07`/`08`), whose law is not recovered.

## Methodology worth borrowing

Three habits earned their place during development:

**Run the suite on every toolchain you ship from.** This is a fixed-point engine, and the one thing
it cannot afford is arithmetic that depends on the compiler. It found exactly that:
ts::PitchChain::start_jitter_milli_semitones tests the sign of a value truncated to sixteen bits,
written as C# spells it — `(short)(draw << 1) >= 0`, which is bit 14. **MSVC compiles that as bit
15 under optimisation.** The same source is correct at `/Od`, correct under GCC and clang at every
level, and correct even in the miscompiled build if the truncation is evaluated into a variable one
line away; it is wrong only when it feeds the branch directly. The effect was that every Windows
render of a patch with a non-zero pitch-envelope jitter byte differed from every Linux one, in a
way no single-platform run could see. The fix is to test the bit rather than the cast — nothing
left to optimise wrongly — and the range sweep that caught it now also pins the four boundary draws
by name, so a recurrence says *which* bit rather than only that the range moved.

**Guard against passing vacuously.** Twelve of the twenty-six effect comparisons were once green
while testing nothing at all — the fixture windows were shorter than the delays, so both sides were
silent and agreed perfectly. A "produced no output" assertion caught it.

**A narrow test set hides broad defects.** One MIDI file exercised no mod wheel and fifteen pitch
bends; three others used hundreds of each and immediately exposed two real bugs. The spec project
records the same lesson three separate times.
