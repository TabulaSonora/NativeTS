// The message contracts between the three threads. The worker owns the WASM engine and the pump;
// the worklet owns the ring; the main thread owns the UI, the context and the stores. Blocks and
// queue reports travel worker↔worklet directly over a bound MessagePort, so nothing audible waits
// on the main thread.

export type TransportState = 'stopped' | 'playing' | 'paused';
export type PlaybackMode = 'song' | 'live';

/** One channel strip of the engine snapshot. Empty object before a ROM is loaded. */
export interface ChannelSnapshot {
    program: number;
    bank: number;
    name: string;
    volume: number;
    pan: number;
    expression: number;
    reverbSend: number;
    chorusSend: number;
    voices: number;
    muted: boolean;
    soloed: boolean;

    /**
     * Whether this part is sounding drums *now*.
     *
     * Not "is this channel 10". GS reroutes a part to the drum path over SysEx and XG does it from
     * bank select alone, so under XG any channel can be drums and channel 10 can be melodic.
     */
    drums: boolean;

    /** The kit sounding on a drum part, or -1. `name` already carries the kit's name. */
    kit: number;

    /** The tone map this part resolves against, which XG changes for every part at once. */
    map: number;
}

export interface EngineSnapshot {
    position: number;
    activeVoices: number;
    noteCount: number;
    drumKit: number;

    /** The kit loaded on each port's drum part, in port order. */
    drumKits: number[];
    effectiveDrumMapRow: number;

    /** Whether the engine is in XG mode, which a file enters and leaves while it plays. */
    xgMode: boolean;
    songComplete: boolean;

    /** Whether the song repeats instead of ending — the loop switch, as the session holds it. */
    looping: boolean;
    channels: Partial<ChannelSnapshot>[];
}

export interface RomInfo {
    name: string;
    size: number;
    sha256: string;
    verified: boolean;
}

export interface SongInfo {
    name: string;
    lengthSamples: number;

    /**
     * The parts the file addresses, as `port * 16 + channel`, ascending. Ports are already folded
     * onto the ones the engine has, so this is what will sound rather than what the file asked for.
     */
    usedParts: number[];

    /**
     * Whether the file declares loop points — markers, RPG Maker's CC 111, the XMI controller
     * pairs — and where, in samples. Looping works without them (the whole song wraps), so this
     * is a badge, not a gate.
     */
    hasLoop: boolean;
    loopStartSamples: number;
    loopEndSamples: number;
}

export interface EngineSettings {
    map: number;
    reverb: boolean;
    chorus: boolean;
    delay: boolean;
}

export interface CatalogEntry {
    program: number;
    tone: number;
    name: string;
    kind: 'native' | 'capitalFallback' | 'indirectOnly' | 'unassigned';
}

export interface CatalogBank {
    bank: number;
    nativeCount: number;
    programs: CatalogEntry[];
}

export interface VintageCatalog {
    map: number;
    nativeCount: number;
    toneCount: number;
    banks: CatalogBank[];
}

export interface DrumKeyEntry {
    note: number;
    tone: number;
    name: string;
    level: number;
    pitch: number;
    group: number;
    pan: number;
}

export interface DrumKitEntry {
    kit: number;
    programs: number[];
    keys: DrumKeyEntry[];
}

export interface DrumCatalog {
    row: number;
    kits: DrumKitEntry[];
}

/** Requests the main thread sends the worker. Ones with an `id` get a WorkerReply back. */
export type WorkerRequest =
    | { type: 'bindAudio'; port: MessagePort }
    | { id: number; type: 'loadRom'; bytes: ArrayBuffer; name: string; expectedSha256: string | null }
    | { id: number; type: 'unloadRom' }
    | { id: number; type: 'loadSong'; bytes: ArrayBuffer; name: string }
    | { id: number; type: 'unloadSong' }
    | { id: number; type: 'setSettings'; settings: EngineSettings }
    | { id: number; type: 'catalog'; map: number }
    | { id: number; type: 'drumCatalog'; row: number }
    | { id: number; type: 'play'; mode: PlaybackMode }
    | { id: number; type: 'pause' }
    | { id: number; type: 'stop' }
    | { id: number; type: 'seek'; sample: number }
    | { id: number; type: 'exportWav' }
    | { type: 'exportCancel' }
    | { type: 'setLead'; frames: number }
    | { type: 'setOutputGain'; gain: number }
    | { type: 'setDrumMapRow'; row: number | null }
    | { type: 'setLooping'; on: boolean }
    | { type: 'sendChannel'; status: number; data1: number; data2: number }
    | { type: 'sendControl'; channel: number; controller: number; value: number }
    | { type: 'setMuted'; channel: number; muted: boolean }
    | { type: 'setSoloed'; channel: number; soloed: boolean }
    | { type: 'channelsReset' }
    | { type: 'panic' }
    | { type: 'resetStarved' };

export interface WorkerReply {
    type: 'reply';
    id: number;
    ok: boolean;
    error?: string;
    result?: unknown;
}

/** The rolled-up state the worker posts on every pass and after every state change. */
export interface PumpSnapshot {
    type: 'snapshot';
    engine: EngineSnapshot | null;
    rom: RomInfo | null;
    song: SongInfo | null;
    state: TransportState;
    mode: PlaybackMode;
    queued: number;
    starved: number;
    leadFrames: number;
    realtimeFactor: number;
    exporting: boolean;
    exportProgress: number;
}

export type WorkerMessage =
    | WorkerReply
    | PumpSnapshot
    | { type: 'exportDone'; bytes: ArrayBuffer }
    | { type: 'fatal'; error: string };

/** Frames rendered per pass — 8 ms at the engine's rate. */
export const chunkFrames = 256;

/** The engine's own rate; the context asks for it and the worklet resamples if refused. */
export const engineSampleRate = 32000;

/** The lead the pump starts at — 30 ms. See the pump for why short-and-report-driven is safe. */
export const defaultLeadFrames = 960;

/** Shortest lead the control offers — 8 ms, one chunk. */
export const minimumLeadFrames = chunkFrames;

/** Longest lead the control offers — one second. */
export const maximumLeadFrames = engineSampleRate;

/** The lead slider's step: one millisecond exactly, so whole-millisecond settings are reachable. */
export const leadStepFrames = engineSampleRate / 1000;

/** The channel routed to the drum path — GM channel 10. */
export const drumChannel = 9;
