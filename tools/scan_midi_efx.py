#!/usr/bin/env python3
"""Document which insertion-EFX types a MIDI corpus uses, and pick test material for them.

The EFX block's 65 algorithms are being transcribed one tranche at a time, and both ends of that
work need the same thing from a corpus: which GS types files in the wild actually select, and
which files route enough notes through the block to hear the answer. Types the corpus leans on
are the ones worth transcribing next; files that lean on an already-implemented type are the ones
worth listening to now.

The parsing is *not* reimplemented here. `tabula-sonora scan-efx` does the reading with the
engine's own `smf::read` — so XMI, MUS, HMI and the rest are scanned exactly as they would be
played — and mirrors `ToneGenerator::send_sysex`'s framing: DT1 only, checksum folded to zero,
multi-byte runs walked across the `40 03` addresses, `40 4x 22` switches tracked per part, and
the resets that return the block to power-on honoured. This script only walks the archive,
batches files into scan-efx invocations, and aggregates the rows.

Two steps:

  * `scan` walks a directory tree and writes one JSON row per file that carries any EFX traffic:
    the types it selects (key, DLL display name, whether this engine's processor exists yet),
    parameter/send/switch write counts, which parts are routed through the block, and how many
    note-ons flow through it.
  * `report` aggregates the rows into two sorted lists — implemented types first, then the
    not-yet-implemented ones by prevalence, which is the transcription priority order — and
    prints the best few candidate files per type: most notes through the block first, smallest
    file breaking ties, since a focused file makes better test material than a long medley.

Usage:
    python3 tools/scan_midi_efx.py scan <archive dir> <efx.jsonl> [--cli <tabula-sonora>]
    python3 tools/scan_midi_efx.py report <efx.jsonl> [picks per type]

The scan needs the built CLI and an SCCore.dll (--dll / $TS_SCCORE_DLL / ./SCCore.dll): the type
names and the implemented flags are read from the DLL's own directory, not shipped. That also
means the rows are Roland-derived output — generate locally, do not redistribute, and neither the
archive nor the files this picks are anyone's to hand on either.
"""

import json
import os
import pathlib
import subprocess
import sys

# Every container `formats::to_smf` converts, plus plain SMF; the reader is the arbiter, these
# just keep the obviously-not-music files out of the batches.
SUFFIXES = {
    ".mid", ".midi", ".kar", ".rmi", ".mids", ".mus", ".xmi",
    ".gmf", ".hmp", ".hmi", ".xmf", ".lds",
}

# Paths per scan-efx invocation. The cost per batch is one DLL open, so bigger is faster, but a
# batch also dies whole if the CLI does, and argv space is finite.
BATCH = 200


def find_cli(explicit):
    """The built CLI, preferring an explicit path, then the usual build trees."""
    root = pathlib.Path(__file__).resolve().parent.parent
    candidates = [explicit] if explicit else []
    candidates += [
        root / "build" / preset / "apps" / "cli" / "tabula-sonora"
        for preset in ("release", "debug", "asan")
    ]
    for candidate in candidates:
        if candidate and pathlib.Path(candidate).is_file():
            return str(candidate)
    sys.exit("No tabula-sonora binary found; build one or pass --cli <path>.")


def run_scan(root, out_path, cli):
    paths = sorted(
        os.path.join(directory, name)
        for directory, _, names in os.walk(root)
        for name in names
        if os.path.splitext(name)[1].lower() in SUFFIXES
    )
    print(f"{len(paths)} files under {root}", file=sys.stderr)

    written = 0
    with open(out_path, "w", encoding="utf-8") as out:
        for start in range(0, len(paths), BATCH):
            batch = paths[start : start + BATCH]
            # Parse failures land on stderr and stay there; a malformed file is the archive's
            # problem, not the scan's.
            result = subprocess.run(
                [cli, "scan-efx", *batch], stdout=subprocess.PIPE, text=True, check=False
            )
            if result.returncode != 0 and not result.stdout:
                sys.exit(f"scan-efx failed on the batch starting at {batch[0]!r}")
            out.write(result.stdout)
            written += result.stdout.count("\n")
    print(f"{written} files with EFX traffic -> {out_path}", file=sys.stderr)


def run_report(rows_path, picks):
    rows = [json.loads(line) for line in open(rows_path, encoding="utf-8")]
    print(f"{len(rows)} files with EFX traffic\n")

    # One aggregate per type key, folding every row's tally together. "--" is the power-on
    # bucket: parts routed through the block by `40 4x 22` before any type was selected.
    types = {}
    for row in rows:
        for key, use in row["types"].items():
            entry = types.setdefault(
                key,
                {"name": use["name"], "implemented": use["implemented"],
                 "files": [], "selects": 0, "notes": 0},
            )
            entry["files"].append(row)
            entry["selects"] += use["selects"]
            entry["notes"] += use["notes"]

    def report_group(title, keys):
        print(f"== {title}")
        if not keys:
            print("   none found\n")
            return
        for key in keys:
            entry = types[key]
            print(f"   {key}  {entry['name']:<12}  {len(entry['files'])} files, "
                  f"{entry['selects']} selects, {entry['notes']} notes through the block")
            # Candidates: the most notes actually played through this type, smallest file
            # breaking ties -- audible and focused beats configured-but-silent.
            ranked = sorted(
                entry["files"],
                key=lambda row: (-row["types"][key]["notes"], row["size"]),
            )
            for row in ranked[:picks]:
                use = row["types"][key]
                print(f"      {row['size']:>8}  {row.get('seconds', 0):>7}s  "
                      f"{use['notes']:>6} notes  {row['path']}")
        print()

    # Prevalence order: the types most of the corpus reaches for first. Within the
    # not-yet-implemented list this is the transcription priority order.
    by_use = sorted(
        (key for key in types if key != "--"),
        key=lambda key: (-len(types[key]["files"]), -types[key]["notes"], key),
    )
    report_group("Implemented types (this engine renders them)",
                 [key for key in by_use if types[key]["implemented"]])
    report_group("Not yet implemented (pass through unchanged; priority order)",
                 [key for key in by_use if not types[key]["implemented"]])
    if "--" in types:
        report_group("Routed with no type selected (power-on Thru)", ["--"])


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--cli")]
    cli_arg = next((a.split("=", 1)[1] for a in sys.argv[1:] if a.startswith("--cli=")), None)
    if "--cli" in sys.argv:
        cli_arg = sys.argv[sys.argv.index("--cli") + 1]
        args = [a for a in args if a != cli_arg]

    if len(args) >= 3 and args[0] == "scan":
        run_scan(args[1], args[2], find_cli(cli_arg))
    elif len(args) >= 2 and args[0] == "report":
        run_report(args[1], int(args[2]) if len(args) > 2 else 5)
    else:
        print(__doc__, file=sys.stderr)
        sys.exit(2)


if __name__ == "__main__":
    main()
