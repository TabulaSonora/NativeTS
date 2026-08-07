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

That makes the stream gate a **self-baseline**, and \ref self-baselines-report-drift is what it can
and cannot tell you: it says the render moved and by how much, never whether it should have. It was
regenerated three times in one day over the work described below, each time with the reason
recorded in the commit. Reading a red one as a failure has cost real work here.

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
   of what the old reference produced, not as a target. The `[render]`, `[stream]` and `[sampler]`
   gates measure against them; the two oracle gates below do not, and neither does anything added
   since.
2. **Authoritative** — generated from `SCCore.dll` itself, driven through its own exported API
   (`TG_initialize`, `TG_LongMidiIn`, `TG_Process`). This is the real target: agreement with the
   black box rather than with another reimplementation. `tools/dump_note_renders_oracle.py` sweeps
   239 single notes and drum hits across every tone map and `tools/dump_song_renders_oracle.py`
   drives the song corpus, both through the spec repository's `scdec` harness — natively on
   Windows, under wine elsewhere. **Both halves are live**: `[song][oracle][sccore][gate]` and
   `[note][oracle][sccore][gate]`.
3. **Constrained** — this engine at the hardware's 64 voices, which is the tier directly comparable
   to (2). The gap between them is the measurement that matters, and it answers a question worth
   asking precisely: *how much of the difference is missing features?* Every remaining gap in this
   engine — the untranscribed insertion-EFX algorithms above all — should show up here as a
   specific, attributable divergence rather than as a vague dissatisfaction.

The unlimited and 256-voice digests sit alongside (3) as regression checks on this engine only.
Nothing in the DLL can produce them, because the DLL has 64 voices.

\note Tier 2 is no longer blocked on the harness. What stood in the way was that `scdec` could
only play a built-in sequence; it now takes an **arbitrary Standard MIDI File** in `smf` mode and
renders a sweep of single notes in `notebatch` mode. The song generator drives `smf`, one process
per song. The note generator drives `notebatch` too, but with **one case per process, run
concurrently** — batching the sweep into one process is what \ref oracle-harvest-isolation is
about, and it is behind `--batch` now for a quick look only.

**Tier 2 records tolerances, not digests, and that is a property of the problem.** A song runs the
chorus and reverb, whose LFOs the DLL starts at a phase this port cannot yet derive, so two renders
that agree on every note still diverge sample by sample: whole-song correlation against the oracle
sits near 0.18 while every octave band agrees to a tenth of a dB. Sample identity is therefore not
merely unreachable, it is the wrong question. What the gate compares is length, then peak, RMS,
per-octave level and a coarse RMS envelope — the envelope catching a note that goes missing or
arrives late without being sensitive to phase. The oracle audio is kept beside the fixture so a
failure can be measured rather than only counted.

### A self-baseline reports drift, never correctness {#self-baselines-report-drift}

Tier 3 fixtures are generated from **this engine**. `fixtures/stream_renders.json` says so itself:
they "catch drift from the current engine, they do not confirm it is right", and a self-baseline
"cannot catch a mistake both sides share". That is easy to agree with in the abstract and easy to
forget the moment a gate turns red, so the rule is worth stating as a procedure.

**Only tier 2 settles whether a change is right**, and only while the oracle generator itself has
not moved since the fixture was rendered. Regenerate the oracle whenever `scdec`, the harness
invocation or the DLL changes, and treat a comparison across such a change as no comparison at all.

**A tier 3 gate going red is not a failure.** It is one bit of information — *the render moved since
the last generation* — and that bit is genuinely useful, because a change that moves 500,000 samples
has done something large that ought to be intended. But the sign is not in it. A red tier 3 is
either a regression or a newly more accurate change, and nothing in the fixture can tell you which.

So the procedure, when a change moves the render:

1. Note what tier 3 says, as a magnitude only: how much moved, and whether the frame and note counts
   held. Counts changing means a note went missing or arrived late, which is a different and worse
   class of problem than samples shifting.
2. Decide the sign against tier 2 — the note oracle first, since it is per-case and reports pitch in
   cents, then the song oracle. Tabulate the final result against the oracle render, not against the
   baseline.
3. Only then regenerate the tier 3 baseline, and record in the commit what moved and why.

**This was learned the expensive way.** A drum partial's random-pitch draw was disabled for weeks on
the strength of a tier 3 gate reporting 1,183,722 samples differing, read as the reference
disagreeing. It was not the reference. Re-measured against tier 2 the change was neutral on the note
oracle and one case *better* on the song oracle, and it went back in. The same misreading was in the
way of the GS block-order fix for two attempts: the implementation that finally landed was
substantially the one that had been written and reverted, and the only thing that changed was asking
the oracle instead of the baseline.

**A corollary about instruments generally.** A probe built to measure one consumer of a shared
resource is blind to the others, and passing it is not evidence about them. The random-pan probes
that pin the note-on draw cadence use tones with no random LFO and a zero random-pitch depth, so
they passed throughout both mistakes above. Agreement with an instrument you built is not agreement
with the module.

### The oracle is only as isolated as its harvest {#oracle-harvest-isolation}

The previous section says tier 2 settles what tier 3 cannot. It needs one qualification, learned the
same way: **a tier 2 case is authoritative only if it was harvested without carrying state from the
case before it**, and that is a property of the harness rather than of the DLL.

`scdec notebatch` rendered the whole 239-case sweep in one process with a `GsReset()` between cases,
under a comment asserting that a case "must not depend on what preceded it". For most cases that
holds. It does not hold for a tone with a random LFO. Those live in a 128-slot pool with a LIFO
freelist; `partial_shared_node_free` clears the tick gate and pushes the slot back, and
`partial_shared_node_alloc` resets only the link fields. The random shapes' registers at `+0x77` have
exactly one writer in the whole decompilation, the per-tick save-back. A GS reset frees the nodes and
does not zero them, so the next case's LFO opens on the previous case's values.

Measured, rendering a case alone in a fresh process against its value in the sweep:

| case | sweep fixture | isolated | this engine |
| --- | --- | --- | --- |
| `program 48 note 84 map 1` | 1045.7788 Hz | **1042.3225** | 1042.37 |
| `program 48 note 72 map 4` | 519.4312 | **519.4312** | 521.113 |
| `program 52 note 60 map 4` | 262.5991 | **263.4802** | 263.402 |

Two of the three cases the note gate was failing were the fixture being wrong: this engine agreed
with the isolated reference to within half a cent while the sweep value sat 5.7 and 5.8 cents away.
The third is a real error in this engine. Reading all three as defects would have sent someone
looking for a fault in two places that did not have one.

`dump_note_renders_oracle.py` now renders each case in its own process, concurrently, and keeps the
batched path behind `--batch` for a quick look only. When reproducing any of this, note that only
the **first** case in a `notebatch` file is genuinely isolated — a two-case file has its second case
inheriting from its first, which is its own way of getting a wrong answer confidently.

The song oracle was checked for the same fault and does not have it: `dump_song_renders_oracle.py`
already spawns one process per song. Re-rendering `panwet.mid` and `test_poly_bend.mid` alone
reproduced their fixture audio byte for byte, and both sit tenth and eleventh in the sweep with nine
songs rendered before them, so the test had something to find.

The rule that falls out is short. *Tier 3 says the render moved. Tier 2 says whether it should have
— provided the tier 2 case was harvested in isolation.* Neither instrument is trustworthy about the
question it was not built to answer.

### A gate the corpus could not supply {#sharing-lfo1}

`partial_alloc_node @ 1800029e0` claims **one** LFO1 node per note and sets its refcount at `+3` to
the number of enabled partials, then claims LFO2 nodes in a second loop, one per partial. This port
built both per partial, so a two-partial tone ran two independent LFO1s. For most tones that is
invisible — two runners with identical timing produce identical output. For the three *random*
shapes it is not, because those draw from the shared generator when the phase wraps: two runners
draw twice per wrap and walk apart where the module draws once and every partial hears the same
number.

Twelve tones in the table have a random LFO1 and eight of those have two partials. **None of the 185
melodic cases in the note sweep reaches one.** The authoritative gate was blind to the change, and
the first attempt to measure it read three sweep cases as byte-identical before and after — which
was true and meant nothing.

The trap underneath that is worth recording, because it produced a confident wrong answer twice.
`header[0x0E] & 0x1f` is an *index into* `g_lfo_wave_map`, and that map sends 5, 6 and 7 to the
random shapes while sending 1, 2 and 3 to ordinary geometric ones. Scanning the table for a raw
selector of 1–3 finds 43 tones, 23 of them multi-partial, and not one of them is random. Two of
those — `Vibraphone` and `Tenor Sax` — are in the sweep, which is exactly why the null result looked
plausible.

The gate had to be built. `Stream` and `Bubble` sit at bank 4 and bank 5 of program 122 on every map
including the SC-55's, so one bank select reaches a multi-partial random-LFO1 tone; six one-note
files at three keys, rendered through `scdec smf` and through this engine at one port and 64 voices,
give twelve comparisons. They live in the spec repository under `testdata/lfo/`, with the recipe
beside them, and `tools/compare_randlfo1_probes.py` is what runs them.

It is a **tool rather than a suite gate**, and that was decided by trying the other thing first.
Adding the six files to the song corpus put them through the song gate's five measures, and four of
those measures are built for songs: the bands are taken from a window a quarter of the way in, the
envelope splits the render into 64 windows, and the balance reports the worst of them. On a
3.6-second one-note file driven by a random process the envelope and balance swing on the draw —
0.39 and 0.41 against a 0.06 bound — for reasons that have nothing to do with LFO1. Six rows loose
enough to pass would have said nothing, and `song_oracle_tests.cpp` is right that a row there is a
debt rather than a dispensation. What answered cleanly was the octave bands over the *whole* render,
which is the one measure the song gate does not take.

| against the DLL, mean of 12 | per-partial LFO1 | shared LFO1 |
|---|---|---|
| worst octave band | 11.4770 dB | **11.0379 dB** |
| mean octave band | 2.4161 dB | **2.2352 dB** |
| overall level | 0.1436 dB | **0.1145 dB** |
| peak | **0.006638** | 0.007284 |

The worst band improved on every one of the six distinct cases and the mean band on five. Peak went
the other way, and that ordering is what one should expect rather than a puzzle: on a noise-driven
tone the peak is a single sample of a random process and the octave bands are its statistics, so the
spectrum is the measure with something to say and the peak is the one most able to move on luck. The
change is in the binary; the numbers say it did not cost anything, and the spectrum says it helped.

\note A shared node has one phase and one random register but **per-partial depths** — the module
keeps three shorts per partial inside the node itself at `+0x7c + i*8`. Sharing the runner without
splitting the depths would have been a different and wrong change.

The probes found something larger than what they were built for, which is the ordinary return on
pointing a measurement somewhere nothing had looked. **`Bubble` is 14–20 dB loud in the 2 kHz band**
on all three keys while its overall level agrees to within 0.15 dB, and **`Stream` sits 5–7 dB low
at 500 Hz** on two of three. A single octave band that far out while the total level is right is
energy in the wrong place rather than too much of it — the signature of a filter or a wave chosen
wrong, not of a gain. Both are ordinary GS variation tones one bank select away, and neither the
note sweep nor the song corpus covers either, so nothing else here would have found it. Sharing
LFO1 improved both bands slightly and closed neither.

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
remaining gaps — the untranscribed insertion-EFX algorithms above all — are bright. That is the
number to watch as those close.

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

\note Those five numbers are the sweep's *first* authoritative reading, kept because they are what
the tolerances were set from. Two things have moved under them since and neither has been
re-tabulated: the fixture was re-harvested one case per process (\ref oracle-harvest-isolation), and
voices now start in GS block order. What is measured as of the re-harvest is the standing: **8 of
239 cases fail an assertion**, four melodic and four drums, and every one of them is marginal
against its own bound — the widest is a 63 Hz band 39 dB under its note, and the largest level
disagreement is 2.7 dB on a case allowed 2.3. The one pitch failure is the 5.6-cent case that
\ref what-is-left-in-the-pitch-chain shows is not a tuning error at all.

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

Chasing the crash turned up something else, which is worth stating precisely. **Since resolved —
the cause and the fix are at the end of this section**, and the measurements below are kept as
the record of how it was cornered.

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
reaching audibility through a filter narrow enough to leave nothing else.

**The origin, found and implemented.** The module's decoder does not zero its predictor at a
wave's data start. It zeroes at the 32-sample exponent-block boundary *below* the start and
integrates the preamble deltas in on the way, so an unaligned wave rides their sum as a constant
under every sample. Proved by subtraction: `scdec drumpred` reads the crash voice's live
predictor, and against a start-at-zero decode of the same wave the difference is **exactly
−0.041015625 at correlation 1.0** — which is precisely the sum of the crash's eleven preamble
deltas read from the ROM. The sine-kick zone's nineteen sum to −0.139, the whistle's fourteen to
zero, which is why each sounded the way it did. The decode now seeds its predictor with that
integral (`WaveStreams::preamble_delta`, `DecodedWave::seed`, and the realtime ping-pong reader's
own integrator). Rendered against the module afterwards: the crash's DC tracks it to the fourth
decimal, the kick probe's envelope follows the module's through the whole gesture, and the two
songs this chase started from close from **−3.1 dB and −5.6 dB of RMS to +0.12 and −0.05 dB**.

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

**It is implemented, and off by default.** ts::ToneGeneratorOptions::event_delay_blocks holds a
message for that many 32-sample chunks before any part sees it, which is the module's staging rather
than a delay bolted onto the output — messages are queued raw and matched to parts when they are
*dispatched*, so a SysEx that repoints a part's receive channel in between takes effect first.
Setting it to 4 moves this engine's onset from 1 to 129.

The default is zero, and the reason is the suite rather than the engine: a good deal of it reads
state back on the line after sending, which a real pipeline defers past. That makes it the right
setting for a test of a *law* and the wrong one for a test of a *render*, so the note gate sets it
to 4 and everything comparing against the oracle should.

Two more pieces of the same 148 samples came out of the same look, and they are worth separating
because guessing at the split is what went wrong the first time. ts::OutputFilter — the module's
`tg_output_filter`, which runs at every host rate including its own — is worth exactly **one**
sample here: at 1:1 its phase accumulator sits at zero, the interpolation weight with it, and the
output is the previous input. It was the obvious candidate for the remaining 20 and it is not the
answer. And `TG_ShortMidiIn` stamps a message with `offset * 1000 / host_rate` milliseconds rather
than keeping its sample offset, which ts::ToneGenerator::send_channel_at reproduces — a millisecond
being one 32-sample chunk at 32 kHz is why the module can compare a stamp against a chunk count at
all.

Together those account for the whole of it. On the gate's own footing — the fixture's oracle audio
against this engine with both settings on — the module's onset is 131 and this engine's is 130, and
their peaks land at 1017 and 1018. **One sample apart on both measures.**

An earlier reading of this put 19 samples beyond explanation and reached for the output stage to
cover them. Both halves of that were wrong. The 19 came from comparing against a `voicesolo`
capture, which is a different harness with a different pre-roll and was never on the same footing as
the fixture; and the configuration it was measured in was not staging at all, because the queue took
its origin from the previous buffer rather than the one about to be rendered. There was nothing left
over to attribute.

### The engine plays sharp {#the-engine-plays-sharp}

\note **This is a record of an investigation that is now closed**, kept because the eliminations in
it are what the answer rests on. The engine no longer plays sharp: the median case sits 0.09 cents
from the module against the 2.2 cents below. The two things that closed it are the second fine
tune, fired on the loop crossing (\ref the-second-fine-tune-is-per-voice), and the module's own
approximated exponential (\ref the-exponential-is-the-modules). Numbers below the fold are the ones
measured at the time.

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

That pointed at the eight descriptor bytes this port did not read — `0x0E` to `0x15` — two of which
form a second 16-bit field whose most common value is `0x0400`, the same neutral 1024 as
`fine_tune`, on 3,415 of 4,259 records. **That was the right place to look**, and the rest of this
section is the record of it being dismissed and then confirmed.

`partial_compute_pitch @ 18005fc20` computes *two* native pitches:

```c
voice+0x1fc = root*1000 - fine + 0x400                       // what this port implemented
voice+0x200 = (voice+0x1fc) - *(ushort *)(desc + 0x0e) + 0x400
```

**This section twice concluded that lead was dead**, on the grounds that `voice+0x200` is *written
and never read anywhere in the binary* while `voice+0x1fc` is what `voice_pitch_keyfollow`
subtracts — three occurrences in the whole listing, two of them on unrelated pointers. Both times
the search was sound and the conclusion was wrong: the consumer walks the voice array with its
pointer four bytes into the struct, so it spells `voice+0x200` as displacement `0x1fc` and no search
for the literal offset can find it. [Where it is read](#the-second-fine-tune-consumer) has the
instruction pair.

### The second fine tune, measured {#the-second-fine-tune}

What exposed it was a listener's report that the alto sax "goes out of whack above C4" — a
key-dependent complaint that the sweep above cannot see, because it samples three C's an octave
apart and a multisample zone boundary falls between them. Walking `Alto Sax` chromatically from C3
to E6 on the SC-55 map, against the module's render of the same notes, gives a **staircase**: flat
inside each zone and jumping at every boundary, while the module holds within ±8 cents across the
whole keyboard.

| zone record | `desc[0x0e]` | predicted `−(w − 1024)` | measured, milli-semitones |
|---|---|---|---|
| 3423 | 989 | +35 | +57 … +66 |
| 3424 | 944 | +80 | +72 … +107 |
| 3425 | 774 | +250 | +262 … +291 |
| 3426 | 704 | **+320** | **+296 … +343** |
| 3427 | 968 | +56 | +38 … +90 |

The term tracks the error across a 320 milli-semitone range and seven consecutive zones of one
patch. At C5–F5 ignoring it cost **a third of a semitone**, which is the audible defect; the
engine's worst error on that sweep fell from **+34.33 cents to +4.29** when `native` became
`voice+0x200`. It is neutral on 3,415 of 4,259 records — 80% — which is why an engine ignoring it
still sounded nearly right everywhere the note sweep happened to look, and why the median moved so
little that no aggregate caught it.

### Where it is read, and the instrument that can tell {#the-second-fine-tune-consumer}

The exported decompilation left the descriptor fetch as `LAB_18005c390` — a location, never promoted
to a function — so what `partial_compute_pitch` actually reads had never been seen. In Ghidra it is
two instructions:

```asm
18005c390  LEA RAX,[0x181a1e81e]
18005c397  RET
```

It ignores its argument and returns a fixed address, so the pitch chain does not read the ROM record
at all: it reads a **staging buffer**. `partial_load_params @ 18005ee30` fills that buffer with a
verbatim 22-byte copy — `4+4+4+4+4+2` — of the record `voice+0x138` points at, so the staged fields
are the record's, unmodified. The sibling fetches are the same shape: `181a749b8` returns a fixed
`0x181a1e6f8`, `181a74920` returns `0x181a1e7b0`, and the wave descriptor sits at `0x181a1e81e`
directly after the 0x6e partial block. Nothing in the chain is per-voice except what is staged into
it.

Twice this article said `voice+0x200` is written once and read nowhere, and twice that was wrong.
`voices_control_update @ 1800849a0` walks the voices with its pointer at **`voice + 4`**, so the
pair at `180084c13` reads as displacement `0x1fc`/`0x1f8` and no search for the literal offset could
ever find it:

```asm
MOV EAX,dword ptr [RDI + 0x1fc]   ; voice+0x200
MOV dword ptr [RDI + 0x1f8],EAX   ; voice+0x1fc
```

That is `voice+0x1fc = voice+0x200` — the second fine tune replacing the first-only native — and it
is **not run every tick**. It sits inside `if (voice+0x16c == 1) if (voice+4 != 0) { voice+4 = 0; if
(voice+0x1b0 != 0) { …retrigger…; continue; } … }`, so it fires once, when a per-voice flag is set,
and is skipped entirely down the retrigger branch.

### The pitch word, and why the increment was the wrong thing to measure {#the-pitch-word}

Every measurement in the two sections above was taken through the module's sampler increment, and
that was a mistake worth naming.

`voice_pitch_block_init @ 180082e10` turns a pitch into the ramp word with

```c
cur = 0x38000 + ((x & 0x7fffff) >> 22) + (x << 9) / 0x177 + ((x << 9) >> 31);
```

— `512/375` is `16384/12000`, so one unit is a 16,384th of an octave, or 375/512 of a
milli-semitone, and `0x38000` is the whole of the constant when the master tune is neutral. That
word is `g_voice_ramp_pitch @ 181a1cbf0` +8 (current) and +0xc (target); `scdec postrace` with
`TS_POSTRACE_BLOCK=32` dumps both, and once a note settles `cur == tgt` **is the module's pitch as
an integer**. `tabula-sonora pitch` now prints ours in the same word.

The 16.16 increment at +0x14 is one step further on, and that step is a *table*. Solve
`cur = C + 16384·log2(inc/65536)` on three notes of one flute wave and `C` comes out 229398, 229393
and 229422 — it is not one constant, because the exponential is approximated. The error reaches
tens of ramp units, and tens of ramp units is **about two cents**: the same size as the effects
being argued over. Comparing `pow(2, x/12000)` against a traced increment therefore measures the
pitch chain and the module's `exp2` at once and cannot separate them. Every "with the term / without
the term" number in the table above was taken that way and none of them decides anything.

Measured on the word instead — 550 settled single-partial melodic cases, programs 0 to 124 in steps
of four, notes 24 to 108 in steps of three, both tone maps, dropping any voice whose word is still
moving:

| | |
|---|---|
| exact (within one unit, 0.73 mst) | **225 of 550 — 41%** |
| median | **3 units = 2.2 mst = 0.22 cents** |
| 90th percentile | 45 units = 33 mst |
| worst | 222 units = 163 mst = 16.3 cents |

The chain is five times closer than audio estimation made it look, and the second fine tune is
plainly read: on the 254 cases whose record carries a non-neutral one, applying it gives 84 exact
against 45 and a median of 3 units against 17.

### The gate is per voice, not per patch {#the-second-fine-tune-is-per-voice}

Which is where the contradiction dissolves. Classifying each of those 254 cases as *applied* (the
word matches with the term), *ignored* (it matches without it) or *neither* gives 84 / 37 / 133 —
and the split does not follow the wave. Wave 2883 applies the term on eight notes and ignores it on
seven; waves 361, 362, 1774, 2512, 2514, 2515, 2778, 2779, 2784, 2785, 2787 and 3404 all split the
same way. One wave, one descriptor, one partial block, two answers.

So no static property can be the gate, and both previous attempts to find one were looking in the
wrong place — including this article's own "Alto Sax and Piano 1 want the term; Vibraphone, Nylon
Gt. and Trumpet do not". That was never a patch property. It is the runtime flag above: the copy
runs when `voice+4` is set and `voice+0x1b0` is clear, and what sets `voice+4` is still unknown.

**What sets `voice+4` is the decoder reaching the wave's loop point**, and that is now measured
rather than guessed. `scdec postrace` with `TS_POSTRACE_BLOCK=32` on `Clarinet` note 57, whose wave
carries a second fine tune of 1274, reads the ramp word settled at 218182 for the first two control
ticks and then, at sample 352:

```
320: v0 pos=142.378 | pitchramp flags=0000 cur=218182 tgt=218182 step=0    inc=40778
352: v0 pos=162.289 | pitchramp flags=0001 cur=218182 tgt=218522 step=169  inc=40778
384: v0 pos=182.451 | pitchramp flags=0000 cur=218522 tgt=218522 step=0    inc=41367
```

340 units is 250 milli-semitones, which is exactly `1274 − 1024`. Two things follow, and both matter
to a port. The term is **retargeted, not assigned**: `flags=0001` with a step is the ordinary pitch
ramp, gliding over two slots the way a bend does, so an engine that writes the new native straight
into the voice produces a discontinuity the module does not have. And the trigger is a *position*,
not a clock — the same trace on `Atmosphere` fires at sample 13440, because its loop is longer — so
nothing computed from a nominal rate survives a glide, a bend or a pitch envelope. This engine
latches the crossing in the wave reader and moves the offset between forming the block's entry ratio
and its exit ratio, which is that retarget by construction.

\note A pitch envelope is sufficient but not necessary: 8 of the 9 cases whose block carries a
non-zero depth at `+0x18` apply the term, but so do 76 of the 208 that do not. It is a clue about
which voices get their ramp recomputed, not the rule.

### The module's exponential is part of the instrument {#the-exponential-is-the-modules}

The section above treats the approximated `exp2` as a hazard to measure *around*. It is also the
pitch the module plays, and for a long time this engine did not play it.

`ramp_env_step_eval` decodes the ramp word into a 16.16 increment out of `g_ramp_exp_tbl` — 257
entries, linearly interpolated, one octave — every eight samples, for the whole life of every voice.
The table is **not** a true exponential. Against one it drifts linearly to **4.66 cents flat** across
the octave and snaps back at the boundary:

| table index | 0 | 64 | 128 | 192 | 255 | 256 |
|---|---|---|---|---|---|---|
| against `2^(i/256)` | 0.00 | −1.17 | −2.34 | −3.51 | −4.66 | 0.00 |

So the module's sounding pitch carries a sawtooth of that size against equal temperament, reset at
every octave of the ramp word. This engine used to hand a *settled* voice the exact `pow(2, …)` and
reserve the table for the glide, on the reasoning that a steady note should render as it always had.
That spends the sawtooth as a **disagreement**: two parts whose pitch words sit at different points
of the octave are put a different distance out, and they beat against each other at a rate the
module does not have. It is also what made the two engines' partials 3.5 and 4.6 cents apart at the
top of this section — the same 4.66, seen through one patch.

Taking every voice through the table closes it. On the 155 pitched cases of the note gate, against
the module's own render of each:

| | before | with the table | and the second fine tune |
|---|---|---|---|
| median error | 2.44 cents | 0.37 | **0.09** |
| inside one cent | 24 of 155 | 100 | **119** |
| inside three cents | 88 | 133 | **148** |

Five of the note gate's nine melodic rows closed outright — `Whistle`, `Syn.Bass 1`, `Nylon Gt.`,
`Violin`, `Harp`, `Tenor Sax` at 23 cents and `Atmosphere` — and `Bass & Lead`'s partials, the case
this section opens with, now agree with the module's to a tenth of a cent. What reported it was a
song rather than a gate: `earthbnd.mid` at the SC-55 map, where a clarinet and a square lead hold
the same line and beat against each other in this engine and not in Sound Canvas VA.

### What is left, and what it is not {#what-is-left-in-the-pitch-chain}

The row selection is now the module's. `partial_compute_pitch` picks the `g_kf_pitch` row as *2 when
the voice's `+0x169` is zero, otherwise the block's `+0x17`, skipping the curve entirely when that
byte is zero* — and `+0x169` is a copy of the part's `+0x10`. That byte has to be zero here: `+0x17`
is zero on 3,900 of the 4,096 partial blocks, so a non-zero `+0x169` would leave almost every
partial with no curve at all, and no curve measures 16 of 550 exact against row 2's 225 on the same
cases. The row is 2, always. This port used `clamp((block[0x13] − 0x40) >> 2, 0, 3)`, which lands on
rows 0, 1 and 3 for 760 of the 4,096 blocks; every case the pitch word can reach carries
`block[0x13] = 0x4a` and already resolved to 2, so the correction is made because the binary says so
rather than because a measurement moved.

It is **not** silent, though it is close to it. `onestop.mid` renders to a different hash, and
`shangai.mid`'s song-gate peak deviation moves from 0.0320 to 0.0322 — a hair worse — while every
other song row is bit-identical and all three gates fail or pass exactly the rows they did before.
That is what a correction with no measurement behind it looks like: the blocks it touches are ones
the pitch word cannot reach, so the only honest claim is that the binary reads row 2.

What the pitch word said about the note-gate rows failing *at the time* is worth having. Two of the
three have since stopped failing and the reason is not in this table — the sweep was re-harvested
one case per process — but the measurement stands on its own:

| case | partial 0 | partial 1 |
|---|---|---|
| `prog 66 note 72 map 2` | 1 unit | exact |
| `prog 38 note 72 map 4` | exact | 1 unit |
| `prog 99 note 48 map 1` | **71 units** | exact |

Two of the three notes were tuned right to within three quarters of a milli-semitone, so their band
and envelope failures were not a pitch problem and chasing tuning would not have fixed them. The
third is wave 2778 missing its second fine tune of 52 — exactly the per-voice gate above.

\note One candidate died here and is recorded so it is not rediscovered: the partial block's signed
16-bit at `+0x16` reproduces `Alto Sax` at key 41 *exactly* — base 40928 against the module's
implied 40928 — and makes 13 of 13 non-zero cases worse across the corpus. It was a term fitted to
a single data point, which is what one data point is always able to do.

Ruled out as sources of a per-wave offset, each by reading the routine rather than guessing: the
multisample record's per-zone byte at `+0x6b`, which `multisample_select_wave` returns and which
lands in `voice+0x140`, is a **level** — `g_level_curve` indexes on it — not a tuning;
`g_wave_desc_table_b` and `_g_multisample_table_b` are aliased to the A tables at init, so the A/B
selector picks between two pointers to the same memory; and the three part-level terms in
`DAT_181a22830` — `−0x7e8`, `(part[0x448]·1000) >> 13` and the global ushort at `0x181a1e6f8` — are
the same for every voice and sum to zero at neutral tune, which is why the fitted constant lands on
`0x38000` exactly.

One more thing the routine reads differently, and the way this one resolved is the more useful
lesson. The pitch-jitter depth appeared to come from **block +0x12**, immediately after the coarse
tune at +0x11, where this port read +0x1a; the ROM seemed to agree, +0x12 being non-zero on 223 of
the 4,726 partial blocks against +0x1a's nineteen.

**Both bytes are right, because they are two different features.** They share
`start_jitter_milli_semitones` and nothing else. `partial_compute_pitch @ 18005fc20` tests +0x12 and
moves the **base pitch**; `pitch_env_apply_stage @ 1800600c0` tests +0x1a and adds its result to the
**pitch envelope's start level**. The module reads both, in that order, and the port now does too.
That 223 blocks use one and nineteen the other is not evidence for either — it is two features used
at different rates.

Switching the byte did make the song gate worse, and the reason is plain in hindsight: it *moved*
the draw rather than adding the one that was missing, so a +0x12 patch drew in the wrong place and
every +0x1a patch stopped drawing at all. The gate was reporting a real regression about a change
that was half right, which is the failure mode a one-byte hypothesis invites. See
\ref self-baselines-report-drift for the other half of how this was nearly settled on the wrong
instrument.

Draw *order* was a genuine defect underneath it, and it is fixed separately: a chunk's voices now
start in GS block order with the drum part first, matching `tg_start_pending_voices @ 18008f020`,
and each part spends `voices + 1` draws before its notes are set up. On `STREETS.MID` those two
together took the song gate's balance measure from 0.1220 to 0.0864 against a 0.06 bound, and
`roland_sc88_y03` moved from failing its balance row to passing it.

\note The one pitch row the note gate still fails, `program 48 note 72 map 4` at 5.6 cents, was
chased to the bottom and **is not a tuning error**. Driving the same case through the block loop
puts this engine's spectral peaks at 519.04, 519.53, 521.00 and 521.48 Hz — the same four
frequencies as the oracle, to the resolution of the analysis. Only their *amplitudes* differ, by one
to two dB, and the fundamental estimator picks a different one of the four as a result. A partial
that is 1 dB loud reads here as a note that is 5.6 cents sharp, which is worth knowing before
trusting the gate's cents column on any tone whose partials beat.

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
- **Insertion EFX exists as a block, but only three of its algorithms do.** ts::InsertionEffect
  transcribes the block machinery in full — the register file, the per-type preset fill, the two
  decrementing delay lines, the `40 03` selection and parameter SysEx, and the `40 4x 22` part
  routing — and decodes the directory, presets, defaults and parameter curves from the user's own
  DLL, `tools/dump_efx_table.py` being where that reading was worked out. Of the 65 types, Thru,
  Overdrive and Distortion have their processors transcribed and verified against the live engine
  (a Thru-routed part is level-transparent in the DLL and within 1.5% here; Overdrive matches to
  1.4% RMS and tracks the no-EFX baseline's per-band spread). The other 62 pass the signal through
  unchanged, with routing and send levels still honoured, and report it via
  ts::InsertionEffect::implemented — so a host can say which effect it is *not* rendering rather
  than rendering it wrong. One number in the block is calibrated rather than traced: the ×4 between
  the algorithm's halved output and the dry mix comes from those live renders, decomposed only as
  far as the 2.0 ramp target the apply handlers write.
- **Drum tones with the 4-partial layout are not reversed** upstream. A melodic tone here has two
  partial slots (ts::Tone::partial_slots), and asking for more throws rather than guessing. General
  MIDI kits resolve to ordinary melodic tones, so the common path works.
- **The random LFO waveforms do not draw the same numbers as the module**, though they now draw
  from the same generator by the same law. Shapes 1, 2 and 3 are implemented (see below); what is
  not reproduced is *which* draw a given voice gets. This bullet used to say the consumption order
  was "an accident of its allocator rather than a behaviour", and that was wrong on both counts.
  It is a behaviour, it is in the binary, and most of it is now matched: five consumers each draw
  only when their own byte is non-zero, a chunk's voices are set up in GS block order with the drum
  part first, and each part spends `voices + 1` draws before its notes. What remains is **the LFO
  node pool**. The module's 128 slots carry the random registers at `+0x77`, nothing zeroes them on
  free or on a GS reset, and a note claiming a recycled slot opens on the previous note's values
  where this engine opens at zero. Until that is modelled the values cannot align, and the shape,
  rate, step timing and depth being right is the intended side of the "audible fidelity, not bit
  accuracy" line: a random modulation that steps at the right moments to the right *sort* of value
  is the parameter working.
- **LFO1 is now shared per note, as the module shares it.** That was a departure until it was
  measured; \ref sharing-lfo1 has what the measurement took to build. Still open is the *part-level*
  branch of the same allocator — bit 5 of the tone header's `+0x0e` and of the partial block's
  `+0x06`, set on 100 tones and 121 blocks, which puts the node on the part rather than on the note
  so it outlives the note that claimed it. Nothing here reads either bit.
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
