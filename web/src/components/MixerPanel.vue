<template>
    <!--
        The parts, live, one row each. Channels are labelled the way a mixer labels them, 1–16 on
        port A and 17 up on port B, not the way MIDI numbers them; every port's channel 10 is its
        drum part, so those are marked.

        The module has more parts than most files use. The first sixteen are always here — they are
        what a keyboard plays into and what a General MIDI file addresses — and the rest appear only
        when a loaded song actually reaches them. A row for a part nothing can reach is a row that
        can only mislead.

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
            <div v-for="channel in visible" :key="channel"
                 :class="['strip', audible(channel) ? '' : 'silent', idle(channel) ? 'unused' : '']">
                <!--
                    The voice count is always in the layout and only sometimes visible. Adding and
                    removing it as notes come and go changed the height of the row it was in, so a
                    busy passage made the whole stack twitch.
                -->
                <span class="label">
                    {{ channel + 1 }}{{ isDrums(channel) ? ' ⋅ drums' : '' }}
                    <span class="voices technical" :class="{ quiet: !part(channel).voices }"
                          aria-hidden="true">{{ part(channel).voices || 0 }}</span>
                </span>

                <span class="sounding" :title="sounding(channel)">{{ sounding(channel) }}</span>

                <span class="buttons">
                    <button class="btn btn-secondary btn-tiny"
                            :aria-pressed="part(channel).muted ? 'true' : 'false'"
                            :aria-label="`Mute channel ${channel + 1}`"
                            @click="store.setMuted(channel, !part(channel).muted)">M</button>
                    <button class="btn btn-secondary btn-tiny"
                            :aria-pressed="part(channel).soloed ? 'true' : 'false'"
                            :aria-label="`Solo channel ${channel + 1}`"
                            @click="store.setSoloed(channel, !part(channel).soloed)">S</button>
                </span>

                <span v-if="part(channel).program !== undefined" class="faders">
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
import { computed, ref } from 'vue';
import { drumChannel, type ChannelSnapshot } from '../engine/protocol';
import { engine } from '../services/engine-client';
import { useEngineStore } from '../stores/engine';

const store = useEngineStore();

// Parts per port, which is how a part index splits into the port it lives on and the channel a
// MIDI message would name.
const partsPerPort = 16;

/**
 * Which strips to draw.
 *
 * The first sixteen unconditionally: they are port A, what the on-screen keyboard plays into and
 * what any ordinary file addresses, and a mixer whose rows appear and vanish with each song would
 * be worse than one with a few quiet rows. Past that, only the parts the loaded song actually
 * reaches — the engine has thirty-two and nothing but a multi-port file can sound the second
 * sixteen, so drawing them empty says the module has something to offer that it does not.
 */
const visible = computed<number[]>(() => {
    const available = store.channels.length;
    const rows: number[] = [];
    for (let channel = 0; channel < Math.min(partsPerPort, available); channel++) {
        rows.push(channel);
    }
    for (const part of store.song?.usedParts ?? []) {
        if (part >= partsPerPort && part < available) {
            rows.push(part);
        }
    }
    return rows;
});

function part(channel: number): Partial<ChannelSnapshot> {
    return store.channels[channel] ?? {};
}

// Every port has a drum part, not just the first.
function isDrums(channel: number): boolean {
    return channel % partsPerPort === drumChannel;
}

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
    const strip = part(channel);
    if (strip.program === undefined) {
        return powerOn[controller]!;
    }
    switch (controller) {
        case 7: return strip.volume!;
        case 10: return strip.pan!;
        case 91: return strip.reverbSend!;
        default: return strip.chorusSend!;
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
    const anySoloed = store.channels.some(strip => strip.soloed);
    const strip = part(channel);
    return anySoloed ? strip.soloed === true : strip.muted !== true;
}

// A visible channel the loaded song never addresses — one of the first sixteen, since the rest are
// only drawn when they are used. Dimmed rather than hidden: it is still playable from a keyboard.
function idle(channel: number): boolean {
    const song = store.song;
    return song !== null && !song.usedParts.includes(channel);
}

function sounding(channel: number): string {
    const strip = part(channel);
    if (strip.program === undefined) {
        return '';
    }
    if (isDrums(channel)) {
        const kits = store.snapshot?.engine?.drumKits;
        const kit = kits?.[Math.floor(channel / partsPerPort)] ?? -1;
        return `kit #${kit}`;
    }
    return strip.name ?? '—';
}

function allOn() {
    engine.post({ type: 'channelsReset' });
}

// Not a Panic: this touches the four faders and nothing else, so a song keeps playing through it.
// Only the strips on screen, because those are the ones whose faders a listener can see move.
function restore() {
    for (const channel of visible.value) {
        for (const fader of faders) {
            store.sendControl(channel, fader.controller, powerOn[fader.controller]!);
        }
    }
}
</script>
