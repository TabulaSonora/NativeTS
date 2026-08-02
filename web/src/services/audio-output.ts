// The main-thread end of audio output: opens a context at the engine's own rate where the browser
// allows it, loads the worklet, and wires the worklet to the engine worker so blocks never touch
// this thread.

import { engineSampleRate } from '../engine/protocol';

export interface AudioStatus {
    sampleRate: number;
    state: 'closed' | 'suspended' | 'running';
    resampling: boolean;
}

let context: AudioContext | null = null;
let node: AudioWorkletNode | null = null;
let gain: GainNode | null = null;

/**
 * Opens the device and returns the MessagePort the engine worker should adopt.
 *
 * Ask for 32 kHz explicitly. Every browser that honours it gives us a path to the device with no
 * resampling anywhere in it; the ones that do not fall back to their own rate and the worklet
 * interpolates. Either way the engine itself never runs at anything but 32 kHz.
 *
 * 'interactive', not 'playback': the hint asks the browser for a device buffer, and 'playback' asks
 * for a deliberately large one. That buffer is added to whatever the pump is holding, and it cannot
 * be shortened afterwards — so choosing it would put a floor under live playing that no amount of
 * tuning on our side could lift. The ring already provides the safety a song needs.
 */
export async function start(): Promise<MessagePort | null> {
    if (context) {
        return null;
    }

    try {
        context = new AudioContext({ sampleRate: engineSampleRate, latencyHint: 'interactive' });
    } catch {
        context = new AudioContext({ latencyHint: 'interactive' });
    }

    await context.audioWorklet.addModule(new URL('../worklet/synth-processor.js', import.meta.url));

    node = new AudioWorkletNode(context, 'synth-processor', {
        numberOfInputs: 0,
        numberOfOutputs: 1,
        outputChannelCount: [2],
        processorOptions: { sourceRate: engineSampleRate },
    });

    gain = new GainNode(context, { gain: 1 });
    node.connect(gain).connect(context.destination);

    // Blocks, transport commands and queue reports all travel worker↔worklet on this channel; the
    // main thread only brokers the introduction.
    const channel = new MessageChannel();
    node.port.postMessage({ command: 'bind', port: channel.port2 }, [channel.port2]);
    return channel.port1;
}

export function isStarted(): boolean {
    return context !== null;
}

/** Browsers start a context suspended until a gesture. Every transport control calls this first. */
export async function resume(): Promise<void> {
    if (context && context.state !== 'running') {
        await context.resume();
    }
}

export function status(): AudioStatus {
    return {
        sampleRate: context ? context.sampleRate : 0,
        state: context ? (context.state as AudioStatus['state']) : 'closed',
        resampling: context ? Math.abs(context.sampleRate - engineSampleRate) > 0.5 : false,
    };
}

/** The output trim the buffer panel offers, applied at the device rather than in the engine. */
export function setGain(value: number): void {
    if (gain) {
        gain.gain.value = value;
    }
}

/** Hands a rendered file to the browser as a download. The bytes are already a complete WAV. */
export function download(name: string, bytes: ArrayBuffer): void {
    const blob = new Blob([bytes], { type: 'audio/wav' });
    const url = URL.createObjectURL(blob);

    const anchor = document.createElement('a');
    anchor.href = url;
    anchor.download = name;
    document.body.appendChild(anchor);
    anchor.click();
    anchor.remove();

    // Give the click a turn to be handled before the URL stops resolving.
    setTimeout(() => URL.revokeObjectURL(url), 10_000);
}
