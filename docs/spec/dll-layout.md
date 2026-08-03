# SCCore.dll data layout {#dll-layout}

Everything the Sound Canvas VA engine needs — the wave ROM, every synth curve and key-follow table,
and the patch directory — lives **inside `SCCore.dll`**. This page and the offset map are
the reason a downstream implementation can read the static data straight from the DLL and never
re-reverse an offset.

In this repository the offset map is `assets/manifest.json`, embedded into the library at build
time and read by ts::TableManifest. It is the same map the spec repository publishes as
`tables/manifest.json`; the spec copy is canonical, and this one tracks it.

## Pin the DLL version first

The tables are version-specific. This work was done against exactly this file:

| field | value |
|---|---|
| filename | `SCCore.dll` |
| size | **27,347,456** bytes |
| SHA-256 | `117E6AA147A96FBDE5E10D2CAF16C89965ACC1E44235FD245992216CC620BDB1` |
| SHA-1 | `CF9DCE5A0CABEE06792E884673B8BEEF806F1AED` |
| MD5 | `DBD9A30C168EFEF577D40A28D9ADF37D` |
| PE timestamp | `1572416468` = 2019-10-30 06:21:08 UTC |
| file mtime | 2020-01-19 UTC |
| product | Roland VS Sound Canvas VA |

The Win32 version resource is empty (no `FileVersion`), so identify the build by **hash + PE
timestamp + size**. A different SC-VA build may move tables. ts::RomImage enforces exactly this,
and ts::DllIdentity records what it checked.

## Address model

The DLL's preferred image base is `0x180000000`. Symbol VAs (as seen in a disassembler) map to raw
**file** offsets by a per-section constant:

| region | mapping |
|---|---|
| `.rdata` curve / key-follow tables | `file_offset = VA − 0x180000000 − 0x1000` |
| resample-kernel section (`g_interp_coef_table`) | `file_offset = VA − 0x180000000 − 0x1400` |
| data section — wave ROM + patch directory | own base offsets, one per region (see the manifest) |

This engine reads the raw file positionally, so it uses file offsets directly. If instead you read
the **loaded/relocated** image (for example via the spec repository's `scdec dumpmem`), use
`loaded_base + (VA − 0x180000000)` — the virtual RVA, which differs from the file offset by the
section skew above.

## What's where

- **Wave ROM** — two banks, file offsets `0x92700` (bank A, 16 MB: SC-88 `ver200` plus the first
  8 MB of SC-88Pro `rom_make`) and `0x1092730` (bank B, 8 MB: the rest of `rom_make` plus SC-8820
  `8820_wv0`), addressed in 1 MB blocks. The two banks are byte-for-byte the SC-8820's two physical
  wave mask ROMs (IC7 128 Mbit and IC39 64 Mbit) embedded back-to-back. Each sample is a
  block-floating-point ADPCM pair of streams — a per-sample delta and a per-16-sample scale
  nibble — decoded as `cumsum(delta << (scale + 10)) * CONST`. Read by ts::WaveRom and decoded by
  ts::Sampler. A wave's data may **start and end partway into a scale block**; two in five
  descriptors do, and the decoder indexes the scale stream by absolute sample position rather than
  rounding to a boundary. Bank A spans all sixteen regions its four-bit field can name
  (ts::WaveRom::bank_a_region_count), bank B eight.
- **Patch directory** (data section): `tone` (0x100-stride records = 0x24 header plus four 0x6e
  partial blocks), `multisample` (0x8c stride, key/velocity zone → wave number), `wavedesc` (ROM
  coordinates, root key and loop), plus the `layered` alternate-articulation table and three lookup
  LUTs. Drum kits are `0x50C`-stride records at VA `0x18AD950`, selected through a bank-row and
  program-map pair of LUTs. Read by ts::PatchDirectory, ts::WaveDescriptor and ts::DrumKitTable.
- **Synth curves and key-follow tables** (`.rdata`): the TVA, TVF and pitch envelope curves, the
  shared segment-rate machine (`g_rate_curve`, `g_env_rate_out`, `g_env_scale_curve`,
  `g_env_shape`), the LFO tables, the 4-tap resample kernel, and the pan table. The full list with
  offsets, sizes, dtypes and purpose is in the manifest's `cached_tables`; ts::TableSet names each
  one.

## Extracting the tables

The spec repository regenerates the map itself with `python tools/gen_manifest.py <SCCore.dll>`,
which locates every table in the DLL by byte-exact content match and re-emits the manifest with
fresh hashes. That is how the map is produced.

Here, the map is already embedded, and the equivalent local operation is to slice the tables out of
your own DLL:

```
tabula-sonora extract-tables tables/
```

Each output is a byte-for-byte slice of the DLL at the offset the manifest records — `size` bytes
at `file_offset`. All 50 cached tables match the DLL byte-for-byte at these offsets. One,
`kf_tvfenv`, is an over-read whose used rows 0–15 match; the unused high rows differ and are never
indexed.

\warning The extracted tables are Roland's data. They are gitignored in every repository in this
project and must not be redistributed.
