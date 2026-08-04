// The reactive mirror of the worker's pump state, plus the actions every panel drives it through.
// One store, updated from the snapshot stream at the worker's own cadence (10 Hz plus every state
// change), so panels redraw from data that is already here rather than asking the worker questions.

import { defineStore } from 'pinia';
import {
    defaultLeadFrames,
    engineSampleRate,
    type DrumCatalog,
    type EngineSettings,
    type PumpSnapshot,
    type VintageCatalog,
} from '../engine/protocol';
import { engine } from '../services/engine-client';
import * as audio from '../services/audio-output';
import * as preferences from '../services/engine-preferences';

// The catalog sweeps cost 16 384 lookups per vintage, so what was built is held on to — keyed by
// the ROM's hash so loading a different DLL naturally invalidates everything.
const vintageCatalogs = new Map<string, VintageCatalog | null>();
const drumCatalogs = new Map<string, DrumCatalog | null>();

export const useEngineStore = defineStore('engine', {
    state: () => ({
        ready: false,
        snapshot: null as PumpSnapshot | null,
        audio: audio.status(),
        leadFrames: defaultLeadFrames,
        outputGain: 1.0,
        error: null as string | null,
        settings: { map: 4, reverb: true, chorus: true, delay: true } as EngineSettings,

        /** The loop switch, mirrored here for the UI; the worker's session holds the truth. */
        looping: false,
    }),

    getters: {
        rom: state => state.snapshot?.rom ?? null,
        song: state => state.snapshot?.song ?? null,
        transport: state => state.snapshot?.state ?? 'stopped',
        mode: state => state.snapshot?.mode ?? 'live',
        channels: state => state.snapshot?.engine?.channels ?? [],
        activeVoices: state => state.snapshot?.engine?.activeVoices ?? 0,
        noteCount: state => state.snapshot?.engine?.noteCount ?? 0,
        queued: state => state.snapshot?.queued ?? 0,
        starved: state => state.snapshot?.starved ?? 0,
        realtimeFactor: state => state.snapshot?.realtimeFactor ?? 0,
        exporting: state => state.snapshot?.exporting ?? false,
        exportProgress: state => state.snapshot?.exportProgress ?? 0,

        /**
         * The position the listener is actually hearing, in samples — not where the renderer has
         * got to: that runs a lead ahead, and a progress bar driven from it would show the song
         * finishing before it is heard to.
         */
        audiblePosition(state): number {
            const rendered = state.snapshot?.engine?.position ?? 0;
            return Math.max(0, rendered - (state.snapshot?.queued ?? 0));
        },

        lengthSamples(state): number {
            return state.snapshot?.song?.lengthSamples ?? 0;
        },

        /** Whether the loaded song declares its own loop points; looping wraps whole without. */
        songHasLoop: state => state.snapshot?.song?.hasLoop ?? false,

        sampleRate: () => engineSampleRate,
    },

    actions: {
        init() {
            // Read the remembered settings before any ROM loads, so the first generator is built
            // in the remembered vintage rather than rebuilt immediately after.
            // A plain copy, not the store field: postMessage cannot structured-clone a Pinia
            // reactive proxy, and the DataCloneError surfaces as a silent rejection here.
            this.settings = preferences.read();
            engine.call({ type: 'setSettings', settings: { ...this.settings } }).catch(error => {
                this.error = String(error);
            });

            this.outputGain = preferences.readGain();
            if (this.outputGain !== preferences.defaultGain) {
                engine.post({ type: 'setOutputGain', gain: this.outputGain });
            }

            // The loop switch lives in the asset database beside the DLL, so reading it is
            // asynchronous where the other preferences are not. Off needs no message: it is the
            // session's own starting state.
            preferences.readLoop().then(on => {
                if (on) {
                    this.looping = true;
                    engine.post({ type: 'setLooping', on: true });
                }
            });

            engine.onSnapshot = snapshot => {
                this.ready = true;
                this.snapshot = snapshot;
                this.leadFrames = snapshot.leadFrames;
            };
            engine.onFatal = error => {
                this.error = error;
            };
            engine.onExportDone = bytes => {
                const name = (this.song?.name ?? 'render').replace(/\.midi?$/i, '');
                audio.download(`${name}.wav`, bytes);
            };
        },

        /** Opens the device inside the calling gesture and hands its port to the worker. */
        async ensureAudio() {
            const port = await audio.start();
            if (port) {
                engine.post({ type: 'bindAudio', port }, [port]);
            }
            await audio.resume();
            this.audio = audio.status();
        },

        async loadRom(bytes: ArrayBuffer, name: string, expectedSha256: string | null) {
            this.error = null;
            await engine.call({ type: 'loadRom', bytes, name, expectedSha256 }, [bytes]);
        },

        async unloadRom() {
            await engine.call({ type: 'unloadRom' });
        },

        async loadSong(bytes: ArrayBuffer, name: string) {
            this.error = null;
            await engine.call({ type: 'loadSong', bytes, name }, [bytes]);
        },

        async playSong() {
            await this.ensureAudio();
            await engine.call({ type: 'play', mode: 'song' });
        },

        /** Opens the device for live playing, without starting any loaded song. */
        async armLive() {
            await this.ensureAudio();
            await engine.call({ type: 'play', mode: 'live' });
        },

        /**
         * Opens the device for live playing unless a song is already running — a key pressed over
         * a running song plays into the same generator and needs no mode change.
         */
        async armForKeys() {
            if (this.mode === 'song' && this.transport === 'playing') {
                return;
            }
            await this.armLive();
        },

        async pause() {
            await engine.call({ type: 'pause' });
        },

        async stop() {
            await engine.call({ type: 'stop' });
        },

        async seek(sample: number) {
            await engine.call({ type: 'seek', sample: Math.max(0, Math.round(sample)) });
        },

        async applySettings(settings: EngineSettings) {
            this.settings = settings;
            preferences.write(settings);
            // Spread defensively: a caller could hand back the store's own reactive object, and a
            // proxy dies in postMessage's structured clone.
            await engine.call({ type: 'setSettings', settings: { ...settings } });
        },

        setLead(frames: number) {
            this.leadFrames = frames;
            engine.post({ type: 'setLead', frames });
        },

        setOutputGain(gain: number) {
            this.outputGain = gain;
            preferences.writeGain(gain);
            engine.post({ type: 'setOutputGain', gain });
        },

        setLooping(on: boolean) {
            this.looping = on;
            // Fire-and-forget on both sides: the switch takes effect at the next rendered block,
            // and the remembered copy is best-effort the way every preference here is.
            void preferences.writeLoop(on);
            engine.post({ type: 'setLooping', on });
        },

        resetStarved() {
            engine.post({ type: 'resetStarved' });
        },

        sendControl(channel: number, controller: number, value: number) {
            engine.post({ type: 'sendControl', channel, controller, value });
        },

        sendChannel(status: number, data1: number, data2: number) {
            engine.post({ type: 'sendChannel', status, data1, data2 });
        },

        setMuted(channel: number, muted: boolean) {
            engine.post({ type: 'setMuted', channel, muted });
        },

        setSoloed(channel: number, soloed: boolean) {
            engine.post({ type: 'setSoloed', channel, soloed });
        },

        panic() {
            engine.post({ type: 'panic' });
        },

        async exportWav() {
            await engine.call({ type: 'exportWav' });
        },

        async catalog(map: number): Promise<VintageCatalog | null> {
            const key = `${this.rom?.sha256 ?? ''}:${map}`;
            if (!vintageCatalogs.has(key)) {
                vintageCatalogs.set(key, await engine.call({ type: 'catalog', map }));
            }
            return vintageCatalogs.get(key) ?? null;
        },

        async drumCatalog(row: number): Promise<DrumCatalog | null> {
            const key = `${this.rom?.sha256 ?? ''}:${row}`;
            if (!drumCatalogs.has(key)) {
                drumCatalogs.set(key, await engine.call({ type: 'drumCatalog', row }));
            }
            return drumCatalogs.get(key) ?? null;
        },

        cancelExport() {
            engine.post({ type: 'exportCancel' });
        },
    },
});
