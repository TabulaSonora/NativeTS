#!/usr/bin/env python3
"""Generate assets/builds.json -- the registry of SCCore.dll builds this engine can read.

assets/manifest.json records every offset in one build's coordinates: the 2019-10-30 x64 one, because
that is the build the engine's behaviour was reverse-engineered from. Roland shipped earlier releases
too, and between them `.rdata` was re-packed -- the table *data* is the same but it was split and
re-ordered, so a table can span several differently-shifted segments and some have no single base at
all. See specv2/docs/DLL_VERSIONS.md.

Reading those older releases is about making the data easier to come by. The engine ships no Roland
data at all; it needs an SCCore.dll the reader already owns, so every release it can read is one more
way for somebody to supply one. A non-pinned build is described here as a *translation*: a piecewise
map from reference offsets to that build's, plus its own wave-ROM bank offsets (the ROM is excluded
from the segment map -- 24 MB of bytes identical in every build would swamp it -- and is found by
scanning block magic instead).

Usage:
    python3 tools/gen_build_registry.py [--spec ../specv2] [--dll-dir ..] [-o assets/builds.json]

Reads the spec repo's versions.json and segments-*.json, re-verifies every segment against the actual
DLLs, and writes a compact registry. Segments are emitted as flat [start, end, shift] triples because
there are ~800 per build and one JSON object each would triple the asset for no gain.
"""
import argparse, hashlib, json, os, struct, subprocess, sys, tempfile

ROM_MAGIC = bytes.fromhex("A4EBA52BE929")
ROM_BLOCK = 0x100000

# The SOUND Canvas VA release each build ships in. Provenance only -- nothing verifies it, because
# SCCore.dll carries no version resource at all -- but it is how a person names the file they have,
# so it is what the front ends print. The 2016-03-09 x86/x64 pair are one release built twice.
RELEASE_VERSION = {
    "2016-03-09-x64": "1.0.3",
    "2016-03-09-x86": "1.0.3",
}

def parse_hex(text):
    text = text.strip()
    negative = text.startswith("0x-") or text.startswith("-")
    digits = text.replace("0x-", "").replace("-", "").replace("0x", "")
    return -int(digits, 16) if negative else int(digits, 16)

def pe_timestamp(data):
    e_lfanew = struct.unpack_from("<I", data, 0x3C)[0]
    return struct.unpack_from("<I", data, e_lfanew + 8)[0]

def pe_layout(data):
    """Image base and section table -- what turns a VA read out of this build into a file offset."""
    e = struct.unpack_from("<I", data, 0x3C)[0]
    opt = e + 24
    is64 = struct.unpack_from("<H", data, opt)[0] == 0x20B
    base = struct.unpack_from("<Q" if is64 else "<I", data, opt + (24 if is64 else 28))[0]
    count = struct.unpack_from("<H", data, e + 6)[0]
    opt_size = struct.unpack_from("<H", data, e + 20)[0]
    sections = []
    for i in range(count):
        o = opt + opt_size + 40 * i
        vsize, rva, rsize, raw = struct.unpack_from("<IIII", data, o + 8)
        if rsize:
            sections.append({"rva": "0x%x" % rva, "virtual_size": "0x%x" % vsize,
                             "raw": "0x%x" % raw, "raw_size": "0x%x" % rsize})
    return "0x%x" % base, sections

def wave_rom_banks(data):
    """The two bank blobs, by contiguity. Builds differ in which order they store them."""
    offsets, o = [], data.find(ROM_MAGIC)
    while o >= 0:
        offsets.append(o)
        o = data.find(ROM_MAGIC, o + 1)
    runs, run = [], []
    for x in offsets:
        if run and x - run[-1] != ROM_BLOCK:
            runs.append(run)
            run = []
        run.append(x)
    if run:
        runs.append(run)
    banks = {}
    for r in runs:
        size = len(r) * ROM_BLOCK
        # bank A is the 16 MB blob (SC-88 + first half of SC-88Pro), bank B the 8 MB one.
        banks["wave_rom_bank_A" if size >= 16 * 1024 * 1024 else "wave_rom_bank_B"] = r[0]
    return banks

def main():
    here = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    ap = argparse.ArgumentParser()
    ap.add_argument("--spec", default=os.path.join(here, "..", "specv2"))
    ap.add_argument("--dll-dir", default=os.path.join(here, ".."))
    ap.add_argument("-o", "--output", default=os.path.join(here, "assets", "builds.json"))
    args = ap.parse_args()

    versions = json.load(open(os.path.join(args.spec, "tables", "versions.json")))
    manifest = json.load(open(os.path.join(here, "assets", "manifest.json")))
    pinned_sha = manifest["dll"]["sha256"].lower()

    pinned_path = None
    images = {}
    for key, build in versions["builds"].items():
        path = os.path.join(args.dll_dir, build["filename"])
        if not os.path.exists(path):
            sys.exit("missing DLL for %s: %s" % (key, path))
        data = open(path, "rb").read()
        got = hashlib.sha256(data).hexdigest()
        if got != build["sha256"]:
            sys.exit("%s: sha256 mismatch (registry says %s, file is %s)" % (path, build["sha256"], got))
        images[key] = data
        if got == pinned_sha:
            pinned_path = key
    if pinned_path is None:
        sys.exit("none of the registered builds is the manifest's pinned build")
    pinned = images[pinned_path]

    out = {
        "_note": "SCCore.dll builds this engine can read. Offsets in manifest.json are expressed in the "
                 "coordinates of the build marked \"pinned\" -- the one the behaviour was reverse-"
                 "engineered from, not a better copy of the data. Every other build carries "
                 "\"segments\", a piecewise map "
                 "[pinned_start, pinned_end, shift] translating a pinned file offset to that build's "
                 "(target = pinned - shift). Generated by tools/gen_build_registry.py; each segment is "
                 "re-verified byte-for-byte against the real DLLs at generation time.",
        "builds": [],
    }

    for key, build in sorted(versions["builds"].items()):
        data = images[key]
        # Recompute rather than inherit `path` from the loop above, whose last iteration leaves it
        # pointing at whichever build sorted last -- which silently made the effect locator compare
        # the reference build against itself.
        build_path = os.path.join(args.dll_dir, build["filename"])
        entry = {
            "id": key,
            "file_name": build["filename"],
            "product": "Roland VS SOUND Canvas VA",
            "version": RELEASE_VERSION.get(key, manifest["dll"].get("version", "")),
            "description": build["description"],
            "architecture": build["arch"],
            "size": build["size"],
            "sha256": build["sha256"],
            "sha1": build["sha1"],
            "md5": build["md5"],
            "pe_timestamp": build["pe_timestamp"],
            "pinned": build["sha256"].lower() == pinned_sha,
            "image_base": pe_layout(data)[0],
            "sections": pe_layout(data)[1],
            "wave_rom": {k: "0x%x" % v for k, v in sorted(wave_rom_banks(data).items())},
        }
        if not entry["pinned"]:
            # Where EffectProgrammer's own reads land in this build. Separate from "segments"
            # because four of them are pointer tables, which a content map can never place: their
            # entries are image VAs, so they differ in every build by construction.
            with tempfile.NamedTemporaryFile(suffix=".json", delete=False) as scratch:
                fx_path = scratch.name
            subprocess.run([sys.executable,
                            os.path.join(here, "tools", "locate_effect_tables.py"),
                            os.path.join(args.dll_dir, manifest["dll"]["filename"]),
                            build_path, fx_path], check=True, stdout=subprocess.DEVNULL)
            effects = json.load(open(fx_path))
            os.unlink(fx_path)
            missing = sorted(k for k, v in effects.items() if v is None)
            if missing:
                sys.exit("%s: effect tables unresolved: %s" % (key, ", ".join(missing)))
            entry["effects"] = effects

            # Tables found byte-exact as a whole blob in this build. A proven offset for the whole
            # table is better evidence than stitching it out of segments, and it places short ones
            # (`g_ramp_divider` is four bytes) that no window search could ever anchor.
            exact = {}
            for name, offset in sorted(build.get("tables", {}).items()):
                exact[name] = offset
            if exact:
                entry["tables"] = exact
            seg_path = os.path.join(args.spec, build["segment_map"])
            segments = json.load(open(seg_path))["segments"]
            triples, verified = [], 0
            for s in segments:
                a, b, d = parse_hex(s["pinned_start"]), parse_hex(s["pinned_end"]), parse_hex(s["shift"])
                t = a - d
                if t < 0 or t + (b - a) > len(data):
                    continue
                if data[t:t + (b - a)] != pinned[a:b]:
                    sys.exit("%s: segment 0x%x..0x%x does not verify" % (key, a, b))
                triples.append([a, b, d])
            triples.sort()
            # Byte-extending adjacent segments can make two of them claim the same few bytes. Both
            # claims verified above, so the overlap is bytes that read the same either way; clip the
            # later one so the map stays a function rather than a relation.
            clipped, previous_end = [], None
            for start, stop, shift in triples:
                if previous_end is not None and start < previous_end:
                    start = previous_end
                    if start >= stop:
                        continue
                clipped.append([start, stop, shift])
                previous_end = stop
            triples = clipped
            entry["segments"] = triples
            verified = sum(stop - start for start, stop, _ in triples)
            for start, stop, shift in triples:
                if data[start - shift:stop - shift] != pinned[start:stop]:
                    sys.exit("%s: clipped segment 0x%x..0x%x does not verify" % (key, start, stop))
            entry["verified_bytes"] = verified
            print("%-16s %d segments, %d bytes re-verified byte-equal" % (key, len(triples), verified))
        else:
            print("%-16s pinned build (identity mapping)" % key)
        out["builds"].append(entry)

    with open(args.output, "w") as f:
        json.dump(out, f, indent=1)
    print("wrote %s (%d bytes)" % (args.output, os.path.getsize(args.output)))

if __name__ == "__main__":
    main()
