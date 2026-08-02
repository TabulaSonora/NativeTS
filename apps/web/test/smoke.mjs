// Node smoke test for the Emscripten engine module.
//
//   node apps/web/test/smoke.mjs --rom /path/to/SCCore.dll --midi song.mid [--out out.wav]
//       [--seconds 10] [--map 4] [--no-reverb] [--no-chorus] [--no-delay]
//
// Loads the ROM with full verification, parses the MIDI, renders a few seconds through the
// real-time path to prove audio comes out, then runs the export path and writes the WAV — the file
// to byte-compare against a native `tabula-sonora render` of the same song and settings.

import { readFile, writeFile } from 'node:fs/promises';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';

const here = dirname(fileURLToPath(import.meta.url));
const modulePath = join(here, '..', '..', '..', 'build', 'web', 'apps', 'web', 'tabulasonora-engine.mjs');

function arg(name, fallback = null) {
    const index = process.argv.indexOf(`--${name}`);
    return index >= 0 ? process.argv[index + 1] : fallback;
}

function flag(name) {
    return process.argv.includes(`--${name}`);
}

const romPath = arg('rom', process.env.TS_SCCORE_DLL ?? process.env.TS_SCCORE);
const midiPath = arg('midi');
const outPath = arg('out');
const seconds = Number(arg('seconds', '10'));
const map = Number(arg('map', '4'));

if (!romPath || !midiPath) {
    console.error('usage: smoke.mjs --rom SCCore.dll --midi song.mid [--out out.wav]');
    process.exit(2);
}

const { default: createTabulaSonoraModule } = await import(modulePath);
const module = await createTabulaSonoraModule();

const call = (name, ret, args) => module.cwrap(name, ret, args);
const api = {
    lastError: call('ts_web_last_error', 'string', []),
    sampleRate: call('ts_web_sample_rate', 'number', []),
    maxRenderFrames: call('ts_web_max_render_frames', 'number', []),
    loadRom: call('ts_web_load_rom', 'number', ['number', 'number', 'string', 'string']),
    romInfo: call('ts_web_rom_info_json', 'string', []),
    loadSong: call('ts_web_load_song', 'number', ['number', 'number', 'string']),
    songInfo: call('ts_web_song_info_json', 'string', []),
    setSettings: call('ts_web_set_settings', 'number', ['number', 'number', 'number', 'number']),
    renderBuffer: call('ts_web_render_buffer', 'number', []),
    renderSong: call('ts_web_render_song', 'number', ['number']),
    snapshot: call('ts_web_snapshot_json', 'string', []),
    vintageCatalog: call('ts_web_vintage_catalog_json', 'string', ['number']),
    drumCatalog: call('ts_web_drum_catalog_json', 'string', ['number']),
    seek: call('ts_web_seek', null, ['number']),
    exportBegin: call('ts_web_export_begin', 'number', []),
    exportStep: call('ts_web_export_step', 'number', []),
    exportBytes: call('ts_web_export_bytes', 'number', []),
    exportLength: call('ts_web_export_length', 'number', []),
};

const fail = (what) => {
    console.error(`FAIL ${what}: ${api.lastError()}`);
    process.exit(1);
};

// The session takes ownership of the ROM allocation; the MIDI one stays ours to free.
const place = (bytes) => {
    const pointer = module._malloc(bytes.length);
    module.HEAPU8.set(bytes, pointer);
    return pointer;
};

console.log(`sample rate ${api.sampleRate()}, max render ${api.maxRenderFrames()} frames`);

const rom = await readFile(romPath);
console.time('load_rom (full verify)');
if (api.loadRom(place(rom), rom.length, 'SCCore.dll', '') !== 0) fail('load_rom');
console.timeEnd('load_rom (full verify)');
console.log('rom:', api.romInfo());

const midi = await readFile(midiPath);
const midiPointer = place(midi);
if (api.loadSong(midiPointer, midi.length, 'song.mid') !== 0) fail('load_song');
module._free(midiPointer);
console.log('song:', api.songInfo());

if (api.setSettings(map, flag('no-reverb') ? 0 : 1, flag('no-chorus') ? 0 : 1,
                    flag('no-delay') ? 0 : 1) !== 0) fail('set_settings');

// Real-time path: render and prove it is not silence.
const chunk = api.maxRenderFrames();
const total = Math.floor(seconds * api.sampleRate());
let peak = 0;
let sum = 0;
console.time(`render ${seconds}s`);
for (let done = 0; done < total; done += chunk) {
    const frames = Math.min(chunk, total - done);
    if (api.renderSong(frames) !== 0) fail('render_song');
    const samples = new Float32Array(module.HEAPF32.buffer, api.renderBuffer(), frames * 2);
    for (const sample of samples) {
        const magnitude = Math.abs(sample);
        if (magnitude > peak) peak = magnitude;
        sum += sample * sample;
    }
}
console.timeEnd(`render ${seconds}s`);
const rms = Math.sqrt(sum / (total * 2));
console.log(`peak ${peak.toFixed(4)}, rms ${rms.toFixed(6)}`);
if (peak === 0) {
    console.error('FAIL: rendered silence');
    process.exit(1);
}
console.log('snapshot:', api.snapshot().slice(0, 200), '…');

// Catalog sweep: the documented per-vintage bank counts are asserted by the caller; here just
// prove the sweeps run and report their sizes.
for (const vintage of [1, 2, 3, 4]) {
    const catalog = JSON.parse(api.vintageCatalog(vintage));
    console.log(`map ${vintage}: ${catalog.banks.length} banks, ${catalog.nativeCount} native, ${catalog.toneCount} tones`);
}
for (const row of [0, 1]) {
    const drums = JSON.parse(api.drumCatalog(row));
    console.log(`drum row ${row}: ${drums.kits.length} kits`);
}

// Export path: the whole song from the top through a second generator, then the WAV bytes.
if (api.exportBegin() !== 0) fail('export_begin');
console.time('export');
let progress = 0;
while (progress < 1) {
    progress = api.exportStep();
    if (progress < 0) fail('export_step');
}
console.timeEnd('export');

const wav = new Uint8Array(module.HEAPU8.buffer, api.exportBytes(), api.exportLength()).slice();
console.log(`wav: ${wav.length} bytes`);
if (outPath) {
    await writeFile(outPath, wav);
    console.log(`wrote ${outPath}`);
}

console.log('OK');
