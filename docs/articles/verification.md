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
| **whole song, against the DLL** | `[song][oracle][sccore][gate]` | length, level, octave bands and a coarse envelope, within stated tolerances |

Only the real-time gate has any tolerance against the C# engine, and it is one LSB. A fifth,
`[song]`, compared whole songs rendered offline; it was retired with the renderer that produced
them, and the oracle gate above is what replaced it.

Two of those three are now partly historical, and \ref past-the-csharp-engine says why. The note
gate's fixture predates the exact-start wave decode, so every case whose wave begins mid-block is
skipped as **superseded** rather than failed, leaving 111 the fixture can still speak for; the
stream gate has no unaffected case at all, so its references are this engine's own output, kept as
a regression baseline. The archived C# checkout cannot be re-run to refresh either, which is the
whole reason the oracle gate matters.

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
   elsewhere. The song half is **live**: `[song][oracle][sccore][gate]`.
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
- **Two of the control matrix's six sources go nowhere yet.** All eleven destinations are consumed
  now, but only from four sources — the mod wheel, both aftertouches and bend. CC1 and CC2 need
  their assignable controller numbers tracked first, so every destination's clamp sees a smaller
  total than the module's would with all six deflected at once. That difference only shows at the
  rail.
- **Two destinations are wired but thinly measured.** The two LFOs' filter depths agree with the
  module wherever they could be made to move, but no probe was found that drives them hard enough
  to be conclusive — the patches whose filters are open enough to hear a cutoff sweep barely change
  brightness under one. LFO2's pitch depth was in this list until the random waveforms were
  implemented, which is what made a patch that could demonstrate it available at all.
- **Some GS part parameters are recognised and dropped**, each because nothing under them is
  modelled: assign mode (`40 1x 14`, one voice-allocation policy here), the CC1/CC2 controller
  numbers (`40 1x 1F`/`20`), and per-key Rx note-on/off in the drum setup (`40 2x 07`/`08`), whose
  law is not recovered.

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
