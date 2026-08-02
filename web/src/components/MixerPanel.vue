<template>
    <!--
        Sixteen parts, live. Channels are labelled the way a mixer labels them, 1–16, not the way
        MIDI numbers them; channel 10 is the drum part, so it is marked.

        Two kinds of control, and the difference matters. Mute and solo go to the channel mask,
        which sits at the mix where no MIDI message reaches, so they take effect on the next block
        and nothing can undo them. The faders send Control Changes — the engine has no other way
        in, and tracking the value behind its back would be a second source of truth for something
        the file also writes — so a running sequence overwrites them at its next controller event,
        exactly as it would on the module.
    -->
    <section class="panel enter-rise">
        <h2>Mixer</h2>

        <div class="strips">
            <div v-for="(part, channel) in store.channels" :key="channel"
                 :class="['strip', audible(channel) ? '' : 'silent', idle(channel) ? 'unused' : '']">
                <span class="label">
                    {{ channel + 1 }}{{ channel === drumChannel ? ' ⋅ drums' : '' }}
                    <span v-if="part.voices" class="voices technical">{{ part.voices }}</span>
                </span>

                <span class="sounding" :title="sounding(channel)">{{ sounding(channel) }}</span>

                <span class="buttons">
                    <button class="btn btn-secondary btn-tiny"
                            :aria-pressed="part.muted ? 'true' : 'false'"
                            :aria-label="`Mute channel ${channel + 1}`"
                            @click="store.setMuted(channel, !part.muted)">M</button>
                    <button class="btn btn-secondary btn-tiny"
                            :aria-pressed="part.soloed ? 'true' : 'false'"
                            :aria-label="`Solo channel ${channel + 1}`"
                            @click="store.setSoloed(channel, !part.soloed)">S</button>
                </span>

                <span v-if="part.program !== undefined" class="faders">
                    <label v-for="fader in faders" :key="fader.controller" class="fader"
                           :title="`${fader.name} — CC${fader.controller}`">
                        <span class="technical">{{ fader.short }}</span>
                        <input type="range" min="0" max="127"
                               :value="value(channel, fader.controller)"
                               :aria-label="`${fader.name} on channel ${channel + 1}`"
                               @pointerdown="grab(channel, fader.controller)"
                               @pointerup="release"
                               @pointercancel="release"
                               @change="release"
                               @input="move(channel, fader.controller, $event)" />
                    </label>
                </span>
            </div>
        </div>

        <div class="row">
            <button class="btn btn-secondary" @click="allOn">All on</button>
            <button class="btn btn-secondary" @click="restore">Faders to power-on</button>
        </div>
    </section>
</template>

<script setup lang="ts">
import { ref } from 'vue';
import { drumChannel } from '../engine/protocol';
import { engine } from '../services/engine-client';
import { useEngineStore } from '../stores/engine';

const store = useEngineStore();

// Volume and pan are what a mixer is for; the two sends are here because on this module they are
// half of the sound — a GM reset leaves every part's reverb send at 40, and a part pulled out of
// the reverb is a different instrument, not a quieter one.
const faders = [
    { controller: 7, name: 'Volume', short: 'vol' },
    { controller: 10, name: 'Pan', short: 'pan' },
    { controller: 91, name: 'Reverb send', short: 'rev' },
    { controller: 93, name: 'Chorus send', short: 'cho' },
];

// What a GM reset leaves behind, which is where the faders start and what "put them back" means.
const powerOn: Record<number, number> = { 7: 100, 10: 64, 91: 40, 93: 0 };

const dragging = ref<{ channel: number; controller: number; value: number } | null>(null);

function read(channel: number, controller: number): number {
    const part = store.channels[channel];
    if (!part || part.program === undefined) {
        return powerOn[controller]!;
    }
    switch (controller) {
        case 7: return part.volume!;
        case 10: return part.pan!;
        case 91: return part.reverbSend!;
        default: return part.chorusSend!;
    }
}

// The fader being dragged renders the value it last emitted rather than the part's. They are the
// same number in the ordinary case, but not while a song is writing that controller too — and
// there, redrawing the file's value into the control under the finger would fight the drag.
function value(channel: number, controller: number): number {
    const grip = dragging.value;
    return grip && grip.channel === channel && grip.controller === controller
        ? grip.value
        : read(channel, controller);
}

function grab(channel: number, controller: number) {
    dragging.value = { channel, controller, value: read(channel, controller) };
}

function release() {
    dragging.value = null;
}

function move(channel: number, controller: number, event: Event) {
    const value = Number((event.target as HTMLInputElement).value);
    store.sendControl(channel, controller, value);

    const grip = dragging.value;
    if (grip && grip.channel === channel && grip.controller === controller) {
        dragging.value = { ...grip, value };
    }
}

// Solo wins: once anything is soloed, only soloed channels are audible regardless of mutes.
function audible(channel: number): boolean {
    const channels = store.channels;
    const anySoloed = channels.some(part => part.soloed);
    const part = channels[channel];
    if (!part) {
        return true;
    }
    return anySoloed ? part.soloed === true : part.muted !== true;
}

// A channel the loaded song never addresses. Only meaningful on the player: with no song, every
// channel is one the keyboard could be pointed at.
function idle(channel: number): boolean {
    const song = store.song;
    return song !== null && (song.usedChannels & (1 << channel)) === 0;
}

function sounding(channel: number): string {
    const part = store.channels[channel];
    if (!part || part.program === undefined) {
        return '';
    }
    if (channel === drumChannel) {
        return `kit #${store.snapshot?.engine?.drumKit ?? -1}`;
    }
    return part.name ?? '—';
}

function allOn() {
    engine.post({ type: 'channelsReset' });
}

// Not a Panic: this touches the four faders and nothing else, so a song keeps playing through it.
function restore() {
    for (let channel = 0; channel < 16; channel++) {
        for (const fader of faders) {
            store.sendControl(channel, fader.controller, powerOn[fader.controller]!);
        }
    }
}
</script>
