#!/usr/bin/env python3
"""Does a matrix depth reach an LFO that is still inside its delay?

It does, on the module: the delay gates the fade-in, and the fade scales the patch's own
depth rather than a controller's. This measures that, on program 43, whose LFO1 delay runs
710 ms.

Usage:
    python3 tools/probe_lfo_delay.py <scdec.exe> <tabula-sonora> <SCCore.dll> <work dir>

Renders Roland-derived audio into the working directory: generate locally, do not
redistribute.

Uses the amplitude destination rather than pitch: a tremolo rides on a level track that is easy to
measure, where a 600-cent vibrato defeats every cheap pitch estimator. The note's own attack is
removed by detrending, so what is left is the modulation.
"""
import sys, pathlib, subprocess, statistics
sys.path.insert(0, str(pathlib.Path("tools").resolve()))
import probe_control_matrix as p

SCDEC = str(pathlib.Path(sys.argv[1]).resolve())
CLI = str(pathlib.Path(sys.argv[2]).resolve())
DLL = str(pathlib.Path(sys.argv[3]).resolve())
WORK = pathlib.Path(sys.argv[4]); WORK.mkdir(parents=True, exist_ok=True)
LABEL = sys.argv[5] if len(sys.argv) > 5 else "ours"

p.STEPS = [127]
p.STEP_SECONDS = 3.0
p.set_track(128, 1024)          # 4 ms hop, 32 ms window

midi = WORK / "delay.mid"
# Mod wheel -> LFO1 amplitude depth, on program 43: LFO1 delay 920 a tick, so 71 ticks / 710 ms.
p.build(midi, 0x06, 0x7F, 43, 60)

def render(out, oracle):
    cmd = ([SCDEC, DLL, "smf", str(midi.resolve()), str(out.resolve()), "4", "0.5"] if oracle
           else [CLI, "render", str(midi), str(out), "--map", "4", "--tail", "0.5", "--dll", DLL])
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        raise SystemExit(f"render failed: {r.stderr[-300:]}")

def detrended(series, span=60):
    """Removes the note's own envelope, leaving modulation faster than ~240 ms."""
    out = []
    for i in range(len(series)):
        lo, hi = max(0, i - span // 2), min(len(series), i + span // 2)
        out.append(series[i] - statistics.fmean(series[lo:hi]))
    return out

results = {}
for name, oracle in (("oracle", True), (LABEL, False)):
    out = WORK / f"delay-{name}.wav"
    render(out, oracle)
    series = p.series_for(p.read_mono(out), "level", int(0.5 * p.RATE), int(2.2 * p.RATE))
    results[name] = detrended(series)

# 710 ms is 177 hops. Sampled well inside each side of it.
print(f"{'window':<26}" + "".join(f"{k:>16}" for k in results))
for label, lo, hi in (("inside the delay (0.2-0.6s)", 50, 150),
                      ("across it     (0.6-0.9s)", 150, 225),
                      ("after it      (1.0-2.0s)", 250, 500)):
    row = f"{label:<26}"
    for k in results:
        window = results[k][lo:hi]
        row += f"{statistics.pstdev(window) * 1000:>16.3f}"
    print(row)
print("\n(modulation depth, RMS x1000, note envelope removed)")
