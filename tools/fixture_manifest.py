"""The fixture hash manifest: what the oracle fixtures were, the last time they were generated.

Fixtures are gitignored, which means nothing in the tree records their state and nothing goes
visibly stale. A harness change that moves the reference renders therefore leaves every existing
copy silently wrong -- the gate keeps running, the numbers keep printing, and they are measured
against something that no longer exists. That has now happened twice in one day: once when `scdec`
stopped truncating event times, and again, far more violently, when it started passing real
`deltaFrames` (max sample delta 0.32, correlation 0.16 against the previous renders).

So every generator writes this manifest as its last step, and the gate checks it. The check is
deliberately narrow: it proves the fixture files are the ones the last regeneration produced, not
that regenerating again would produce the same bytes. Detecting the second thing needs a harness
that can be identified, so the harness's own hash is recorded here too -- for a human reading the
file, and for a future check that has scdec's path available. The test does not have it.
"""

from __future__ import annotations

import datetime
import hashlib
import json
import pathlib


MANIFEST_NAME = "fixture_manifest.json"

# Sibling-repo names, most-specific first, matching what `default_scdec()` in the generators
# already does: a checkout may call the spec repo either `spec` or `specv2`, so try `spec` and fall
# back. The last candidate is what gets named in messages when none of them exist, so it should be
# the one a fresh clone is most likely to want.
REFERENCE_REPOS = ("spec", "specv2")


def reference_manifest():
    """The checked-in manifest to compare against, or where to put one if none exists yet.

    Fixtures are far too large and too machine-specific to commit; their hashes are neither, and a
    committed hash is the only thing that lets a second developer find out that their fixtures
    disagree with the ones the last person generated. Absence is not an error -- a clone with no
    sibling spec repo still generates fine, it just has nothing to compare against.
    """
    candidates = [pathlib.Path("..") / name / "fixtures" / MANIFEST_NAME for name in REFERENCE_REPOS]
    return next((c for c in candidates if c.exists()), candidates[-1])


REFERENCE_MANIFEST = reference_manifest()


def _load(path):
    try:
        document = json.loads(pathlib.Path(path).read_text())
        return document.get("fixtures", {}) if isinstance(document, dict) else {}
    except (OSError, json.JSONDecodeError):
        return {}


def harness_source(harness):
    """The harness's `Program.cs`, found by walking up from the published binary.

    Three tiers of evidence, weakest last:

      1. the fixture hashes -- what actually got produced, and the only thing that can invalidate
         a gate run;
      2. this, the harness SOURCE hash -- stable across rebuilds, moves only when someone edits the
         code, so it is the signal worth reading;
      3. the harness BINARY hash -- moves on every `dotnet publish`, because the build embeds
         metadata, and therefore says nothing on its own.

    Reported, never acted on, for the same reason as the binary: a source edit that leaves every
    render identical (a comment, a probe elsewhere in the file) is not a reason to regenerate.
    """
    path = pathlib.Path(harness)
    for parent in [path, *path.parents]:
        candidate = parent / "Program.cs"
        if candidate.exists():
            return candidate
    return None


def _harness(path):
    """The (binary, source) harness hashes a manifest recorded; either may be None."""
    try:
        document = json.loads(pathlib.Path(path).read_text())
    except (OSError, json.JSONDecodeError):
        return None, None
    if not isinstance(document, dict):
        return None, None
    entry = document.get("harness") or {}
    return entry.get("sha256"), entry.get("sourceSha256")


def report_drift(fixture_paths, root=None, reference=None, harness=None):
    """Prints which fixtures now hash differently from the checked-in reference run.

    This is the whole point of committing the manifest. A regeneration that changes nothing is the
    normal case and should say so plainly; a regeneration that moves a reference render is the case
    that has silently invalidated other people's gate runs twice in one day, and it should be
    impossible to miss at the moment it happens.
    """
    reference_path = pathlib.Path(reference) if reference is not None else reference_manifest()
    if not reference_path.exists():
        print(f"note: no checked-in manifest at {reference_path}; nothing to compare against")
        return

    known = _load(reference_path)
    if not known:
        print(f"note: {reference_path} lists no fixtures; nothing to compare against")
        return

    harness_sha = None
    source_sha = None
    if harness is not None and pathlib.Path(harness).exists():
        harness_sha = file_sha256(pathlib.Path(harness))
        source = harness_source(harness)
        if source is not None:
            source_sha = file_sha256(source)

    root = pathlib.Path(root) if root is not None else pathlib.Path.cwd()
    changed, added, matched = [], [], []
    for raw in fixture_paths:
        path = pathlib.Path(raw)
        if not path.exists():
            continue
        try:
            key = str(path.resolve().relative_to(root.resolve()))
        except ValueError:
            key = path.name
        digest = file_sha256(path)
        entry = known.get(key) or known.get(path.name)
        if entry is None:
            added.append(key)
        elif entry.get("sha256") != digest:
            changed.append((key, entry.get("sha256", "?"), digest))
        else:
            matched.append(key)

    for key in matched:
        print(f"  unchanged since the checked-in run: {key}")
    for key in added:
        print(f"  NEW, not in the checked-in manifest: {key}")
    for key, was, now in changed:
        print(f"  *** CHANGED since the checked-in run: {key}")
        print(f"        was {was}")
        print(f"        now {now}")
    # The harness hash is reported, never acted on. A `dotnet publish` embeds build metadata, so
    # scdec's binary hash moves on every rebuild whether or not a line of `Program.cs` changed --
    # gating on it would declare fixtures stale after a no-op rebuild, and a check that cries wolf
    # is a check people learn to skip. The outputs are the evidence: identical renders from a
    # different binary are a rebuild, and that is not a reason to regenerate anything.
    reference_binary, reference_source = _harness(reference_path)
    binary_moved = harness_sha is not None and reference_binary and harness_sha != reference_binary
    source_moved = source_sha is not None and reference_source and source_sha != reference_source

    if source_moved:
        print(f"  harness SOURCE differs from the checked-in run ({reference_source[:12]}"
              f" -> {source_sha[:12]}).")
        if not changed:
            print("  Every output is identical anyway, so nothing needs regenerating -- an edit")
            print("  that does not reach the renders is not a reason to invalidate them.")
    elif binary_moved and not changed:
        print(f"  note: harness binary differs ({reference_binary[:12]} -> {harness_sha[:12]}) but")
        print("  the source and every output are identical. That is a rebuild -- `dotnet publish`")
        print("  embeds build metadata -- and means nothing. Nothing to regenerate.")

    if changed:
        print("  Every other developer's fixtures are now stale. Copy this manifest to")
        print(f"  {reference_path} and commit it, or their gate will keep measuring")
        print("  against renders yours no longer produces.")


def file_sha256(path: pathlib.Path) -> str:
    """Hashes a file in chunks, so a large render does not have to be resident."""
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def update(fixture_paths, *, harness=None, generator=None, root=None) -> pathlib.Path:
    """Records the hash of each fixture, merging into whatever the manifest already holds.

    Merging rather than replacing, because the song and note generators run separately and each
    knows only its own outputs. A generator that rewrote the whole file would erase the other's
    entry and make the gate demand a regeneration that had already happened.

    Paths are stored relative to the repository root so the manifest is portable between machines,
    which the absolute paths in a generator's argv are not.
    """
    paths = [pathlib.Path(p) for p in fixture_paths if pathlib.Path(p).exists()]
    if not paths:
        return pathlib.Path()

    root = pathlib.Path(root) if root is not None else pathlib.Path.cwd()
    manifest_path = paths[0].parent / MANIFEST_NAME

    document = {"fixtures": {}}
    if manifest_path.exists():
        try:
            document = json.loads(manifest_path.read_text())
            document.setdefault("fixtures", {})
        except (json.JSONDecodeError, OSError):
            # A corrupt manifest is not worth failing a generation over; the gate will ask for a
            # regeneration, which is what a corrupt manifest should cause anyway.
            document = {"fixtures": {}}

    for path in paths:
        try:
            key = str(path.resolve().relative_to(root.resolve()))
        except ValueError:
            key = path.name
        document["fixtures"][key] = {
            "sha256": file_sha256(path),
            "bytes": path.stat().st_size,
        }

    document["generatedAt"] = datetime.datetime.now(datetime.timezone.utc).isoformat(
        timespec="seconds"
    )
    if generator is not None:
        document.setdefault("generators", {})[str(generator)] = document["generatedAt"]
    if harness is not None:
        harness_path = pathlib.Path(harness)
        if harness_path.exists():
            entry = {
                "path": harness_path.name,
                "sha256": file_sha256(harness_path),
            }
            source = harness_source(harness_path)
            if source is not None:
                # The one worth reading: stable across rebuilds, moves only on a real edit.
                entry["source"] = source.name
                entry["sourceSha256"] = file_sha256(source)
            document["harness"] = entry

    document["_note"] = (
        "Written by the fixture generators, one entry per fixture they produced. The gate verifies "
        "these hashes and refuses to judge fixtures that do not match, because fixtures are "
        "gitignored and would otherwise go stale invisibly. Regenerating any fixture rewrites its "
        "entry here; entries for fixtures generated by a different tool are left alone."
    )

    manifest_path.write_text(json.dumps(document, indent=2) + "\n")
    return manifest_path
