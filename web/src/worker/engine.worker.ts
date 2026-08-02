// The engine worker: owns the WASM module and keeps the audio device fed.
//
// The audio thread cannot run the engine, so nothing pulls blocks out of it; this pushes them in,
// staying a few tens of milliseconds ahead of the speaker. The engine renders far faster than
// realtime, so each wake-up does a fraction of a millisecond of work and gives the thread straight
// back. Living in a worker rather than on the main thread — the one thing this page does that the
// reference Blazor app could not — means a layout, a redraw or a busy tab cannot starve the device.
//
// How far ahead is a trade rather than a constant. Too much and every control — a fader, a key, a
// seek — is heard late by exactly that much; too little and the device runs dry, which is audible
// and unrecoverable. The lead is therefore settable and the page puts the starved-frame count next
// to it, because where the floor sits belongs to the machine.
//
// What makes a short lead safe is that filling is driven by the worklet's queue report on the
// audio clock rather than by a timer: a setTimeout is delayed arbitrarily by whatever else is
// happening, so a timer-driven pump has to carry a queue deep enough to survive the worst of it,
// and that depth is latency the player feels on every key. The report arrives every 10 ms of
// *audio*, whatever the page is doing.

import createTabulaSonoraModule from '../engine/generated/tabulasonora-engine.mjs';
import wasmUrl from '../engine/generated/tabulasonora-engine.wasm?url';
import {
    chunkFrames,
    defaultLeadFrames,
    engineSampleRate,
    type EngineSnapshot,
    type PlaybackMode,
    type PumpSnapshot,
    type RomInfo,
    type SongInfo,
    type TransportState,
    type WorkerRequest,
} from '../engine/protocol';

// Instantiating the module suspends this module's evaluation, and a message dispatched while no
// handler is registered is silently dropped — which loses whatever the page sent in its first few
// milliseconds (the remembered settings above all). Catch everything from the first turn and replay
// it once the real handler exists.
const early: MessageEvent[] = [];
onmessage = event => {
    early.push(event);
};

const module = await createTabulaSonoraModule({ locateFile: () => wasmUrl });

const number = (name: string, args: ('number' | 'string')[] = []) =>
    module.cwrap(name, 'number', args) as (...a: (number | string)[]) => number;
const text = (name: string, args: ('number' | 'string')[] = []) =>
    module.cwrap(name, 'string', args) as (...a: (number | string)[]) => string;
const action = (name: string, args: ('number' | 'string')[] = []) =>
    module.cwrap(name, null, args) as (...a: (number | string)[]) => void;

const api = {
    lastError: text('ts_web_last_error'),
    loadRom: number('ts_web_load_rom', ['number', 'number', 'string', 'string']),
    unloadRom: action('ts_web_unload_rom'),
    romInfo: text('ts_web_rom_info_json'),
    loadSong: number('ts_web_load_song', ['number', 'number', 'string']),
    unloadSong: action('ts_web_unload_song'),
    songInfo: text('ts_web_song_info_json'),
    setSettings: number('ts_web_set_settings', ['number', 'number', 'number', 'number']),
    setOutputGain: action('ts_web_set_output_gain', ['number']),
    setDrumMapRow: action('ts_web_set_drum_map_row', ['number']),
    seek: action('ts_web_seek', ['number']),
    panic: action('ts_web_panic'),
    songComplete: number('ts_web_song_complete'),
    renderBuffer: number('ts_web_render_buffer'),
    renderSong: number('ts_web_render_song', ['number']),
    renderLive: number('ts_web_render_live', ['number']),
    sendChannel: action('ts_web_send_channel', ['number', 'number', 'number']),
    sendControl: action('ts_web_send_control', ['number', 'number', 'number']),
    setMuted: action('ts_web_set_muted', ['number', 'number']),
    setSoloed: action('ts_web_set_soloed', ['number', 'number']),
    channelsReset: action('ts_web_channels_reset'),
    snapshot: text('ts_web_snapshot_json'),
    vintageCatalog: text('ts_web_vintage_catalog_json', ['number']),
    drumCatalog: text('ts_web_drum_catalog_json', ['number']),
    exportBegin: number('ts_web_export_begin'),
    exportStep: number('ts_web_export_step'),
    exportBytes: number('ts_web_export_bytes'),
    exportLength: number('ts_web_export_length'),
    exportAbort: action('ts_web_export_abort'),
};

let audioPort: MessagePort | null = null;

let state: TransportState = 'stopped';
let mode: PlaybackMode = 'live';
let leadFrames = defaultLeadFrames;
let filling = false;
let queued = 0;
let starved = 0;

let rom: RomInfo | null = null;
let song: SongInfo | null = null;

let exporting = false;
let exportProgress = 0;
let exportReplyId: number | null = null;

// Averaged over a few seconds of audio and then restarted, so the reading follows the passage
// being played rather than smearing a dense one into everything that came before it. It is the one
// measurement that distinguishes "not enough throughput" from "enough throughput, badly scheduled".
let renderSeconds = 0;
let renderedFrames = 0;
let realtimeFactor = 0;

function measure() {
    const window = engineSampleRate * 3;
    if (renderedFrames < window) {
        return;
    }

    if (renderSeconds > 0) {
        realtimeFactor = renderedFrames / engineSampleRate / renderSeconds;
    }

    renderedFrames = 0;
    renderSeconds = 0;
}

/** Copies one rendered chunk out of the heap and transfers it to the ring. */
function pushChunk() {
    const source = new Float32Array(module.HEAPF32.buffer, api.renderBuffer(), chunkFrames * 2);
    const samples = new Float32Array(chunkFrames * 2);
    samples.set(source);
    audioPort?.postMessage({ command: 'push', samples, frames: chunkFrames }, [samples.buffer]);
    queued += chunkFrames;
}

function fill(fromQueued: number) {
    // Reports keep arriving while a fill is in flight; without this they would interleave into the
    // same buffer and the same generator.
    if (filling || state !== 'playing') {
        return;
    }

    filling = true;
    try {
        queued = fromQueued;
        const live = mode === 'live';

        while (queued < leadFrames) {
            if (!live && api.songComplete() !== 0) {
                break;
            }

            const before = performance.now();
            const result = live ? api.renderLive(chunkFrames) : api.renderSong(chunkFrames);
            renderSeconds += (performance.now() - before) / 1000;

            if (result !== 0) {
                fatal(api.lastError());
                return;
            }

            renderedFrames += chunkFrames;
            pushChunk();
        }

        measure();

        if (!live && api.songComplete() !== 0 && fromQueued === 0) {
            stop();
        }
    } finally {
        filling = false;
    }
}

function play(nextMode: PlaybackMode) {
    if (!rom || !audioPort) {
        throw new Error('Load a DLL and start the audio device first.');
    }

    // What is queued belongs to the other mode: arming live over a stopped song would otherwise
    // play out the song's tail under the first key.
    if (mode !== nextMode) {
        audioPort.postMessage({ command: 'flush' });
        queued = 0;
    }

    mode = nextMode;
    state = 'playing';

    // Fill the ring before letting the worklet drain it, so playback does not begin on an underrun
    // the listener would hear as a stutter at the top of every song.
    fill(queued);
    audioPort.postMessage({ command: 'play' });
    publish();
}

function pause() {
    state = 'paused';
    audioPort?.postMessage({ command: 'pause' });
    publish();
}

function stop() {
    state = 'stopped';
    audioPort?.postMessage({ command: 'pause' });
    audioPort?.postMessage({ command: 'flush' });
    queued = 0;
    publish();
}

/** The queue has to go on a seek: it holds the passage being left, and playing it out after the
 *  jump would be heard as the seek arriving late. */
function seek(sample: number) {
    api.seek(sample);
    audioPort?.postMessage({ command: 'flush' });
    queued = 0;

    if (state === 'playing') {
        fill(0);
        audioPort?.postMessage({ command: 'play' });
    }
    publish();
}

function placeBytes(bytes: Uint8Array): number {
    const pointer = module._malloc(bytes.length);
    module.HEAPU8.set(bytes, pointer);
    return pointer;
}

function parse<T>(json: string): T | null {
    return json === 'null' ? null : (JSON.parse(json) as T);
}

/** Runs the export a quarter-second per turn of the queue, so pump work interleaves and playback
 *  keeps running off the playback generator while the export generator renders. */
function exportPump() {
    if (!exporting) {
        return;
    }

    const progress = api.exportStep();
    if (progress < 0) {
        exporting = false;
        reply(exportReplyId!, false, api.lastError());
        return;
    }

    exportProgress = progress;

    if (progress < 1) {
        setTimeout(exportPump, 0);
        return;
    }

    const bytes = module.HEAPU8.slice(api.exportBytes(), api.exportBytes() + api.exportLength());
    api.exportAbort();
    exporting = false;
    exportProgress = 1;
    reply(exportReplyId!, true, undefined, undefined);
    postMessage({ type: 'exportDone', bytes: bytes.buffer }, { transfer: [bytes.buffer] });
}

function snapshot(): PumpSnapshot {
    return {
        type: 'snapshot',
        engine: rom ? parse<EngineSnapshot>(api.snapshot()) : null,
        rom,
        song,
        state,
        mode,
        queued,
        starved,
        leadFrames,
        realtimeFactor,
        exporting,
        exportProgress,
    };
}

function publish() {
    postMessage(snapshot());
}

function fatal(error: string) {
    state = 'stopped';
    postMessage({ type: 'fatal', error });
}

function reply(id: number, ok: boolean, error?: string, result?: unknown) {
    postMessage({ type: 'reply', id, ok, error, result });
}

function handle(request: WorkerRequest) {
    switch (request.type) {
        case 'bindAudio':
            audioPort = request.port;
            // The worklet reports its queue depth from the audio thread, on the audio clock.
            // Filling from that rather than from a timer is what makes a short lead safe.
            audioPort.onmessage = event => {
                queued = event.data.queued;
                starved = event.data.starved;
                if (state === 'playing') {
                    fill(event.data.queued);
                }
            };
            return;

        case 'loadRom': {
            const bytes = new Uint8Array(request.bytes);
            // The session takes ownership of the allocation, error paths included.
            const ok = api.loadRom(placeBytes(bytes), bytes.length, request.name,
                                   request.expectedSha256 ?? '') === 0;
            rom = ok ? parse<RomInfo>(api.romInfo()) : rom;
            reply(request.id, ok, ok ? undefined : api.lastError(), rom);
            publish();
            return;
        }

        case 'unloadRom':
            stop();
            api.unloadRom();
            rom = null;
            song = null;
            reply(request.id, true);
            publish();
            return;

        case 'loadSong': {
            const bytes = new Uint8Array(request.bytes);
            const pointer = placeBytes(bytes);
            const ok = api.loadSong(pointer, bytes.length, request.name) === 0;
            module._free(pointer);
            song = ok ? parse<SongInfo>(api.songInfo()) : song;
            if (ok) {
                stop();
            }
            reply(request.id, ok, ok ? undefined : api.lastError(), song);
            publish();
            return;
        }

        case 'unloadSong':
            stop();
            api.unloadSong();
            song = null;
            reply(request.id, true);
            publish();
            return;

        case 'setSettings': {
            const s = request.settings;
            const ok = api.setSettings(s.map, s.reverb ? 1 : 0, s.chorus ? 1 : 0,
                                       s.delay ? 1 : 0) === 0;
            reply(request.id, ok, ok ? undefined : api.lastError());
            publish();
            return;
        }

        case 'catalog':
            reply(request.id, true, undefined, parse(api.vintageCatalog(request.map)));
            return;

        case 'drumCatalog':
            reply(request.id, true, undefined, parse(api.drumCatalog(request.row)));
            return;

        case 'play':
            try {
                play(request.mode);
                reply(request.id, true);
            } catch (error) {
                reply(request.id, false, String(error));
            }
            return;

        case 'pause':
            pause();
            reply(request.id, true);
            return;

        case 'stop':
            stop();
            reply(request.id, true);
            return;

        case 'seek':
            seek(request.sample);
            reply(request.id, true);
            return;

        case 'exportWav':
            try {
                if (api.exportBegin() !== 0) {
                    throw new Error(api.lastError());
                }
                exporting = true;
                exportProgress = 0;
                exportReplyId = request.id;
                setTimeout(exportPump, 0);
            } catch (error) {
                reply(request.id, false, String(error));
            }
            return;

        case 'exportCancel':
            exporting = false;
            api.exportAbort();
            exportProgress = 0;
            publish();
            return;

        case 'setLead':
            leadFrames = request.frames;
            return;

        case 'setOutputGain':
            api.setOutputGain(request.gain);
            return;

        case 'setDrumMapRow':
            api.setDrumMapRow(request.row ?? -1);
            publish();
            return;

        case 'sendChannel':
            api.sendChannel(request.status, request.data1, request.data2);
            return;

        case 'sendControl':
            api.sendControl(request.channel, request.controller, request.value);
            return;

        case 'setMuted':
            api.setMuted(request.channel, request.muted ? 1 : 0);
            return;

        case 'setSoloed':
            api.setSoloed(request.channel, request.soloed ? 1 : 0);
            return;

        case 'channelsReset':
            api.channelsReset();
            return;

        case 'panic':
            api.panic();
            return;

        case 'resetStarved':
            starved = 0;
            audioPort?.postMessage({ command: 'resetStarved' });
            return;
    }
}

function dispatch(event: MessageEvent) {
    try {
        handle(event.data as WorkerRequest);
    } catch (error) {
        const request = event.data as { id?: number };
        if (typeof request.id === 'number') {
            reply(request.id, false, String(error));
        } else {
            fatal(String(error));
        }
    }
}

onmessage = dispatch;
early.forEach(dispatch);
early.length = 0;

// Filling is the worklet's job. This exists to redraw at a rate a person can read, and to keep the
// queue moving if the worklet ever stops reporting.
setInterval(() => {
    if (state === 'playing') {
        fill(queued);
    }
    publish();
}, 100);

// Tell the main thread the module is up; the first snapshot doubles as the ready signal.
publish();
