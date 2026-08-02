<template>
    <!--
        The keyboard and what plays into it. Which sound it plays is the instrument picker's
        business, and the channel chosen here is what that picker sends its program changes to.
    -->
    <section class="panel enter-rise">
        <h2>Play it</h2>

        <div class="row">
            <span v-if="!midiSupported" class="tag tag-warn">
                This browser has no Web MIDI. The on-screen keyboard still works.
            </span>
            <button v-else-if="!midiOpen" class="btn btn-secondary" @click="connect">
                Connect MIDI input
            </button>
            <template v-else>
                <label>
                    Input
                    <select @change="listen">
                        <option value="">— none —</option>
                        <option v-for="device in midiDevices" :key="device.id" :value="device.id"
                                :selected="device.connected">{{ device.name }}</option>
                    </select>
                </label>
                <span class="tag tag-muted">{{ midiDevices.length }} device(s)</span>
            </template>

            <!-- This page's one primary action, and only while it is worth pressing: nothing is
                 audible until a context exists, and a browser will only start one inside a gesture.
                 Once the device is running it steps back to secondary. -->
            <button :class="['btn', armed ? 'btn-secondary' : 'btn-primary']" :disabled="armed"
                    @click="store.armForKeys()">
                Start audio
            </button>

            <span v-if="store.mode === 'song' && store.transport === 'playing'" class="note">
                Playing a song, so keys come through the song's own buffer and arrive late. Pause
                it, or press Start audio, to play at low latency.
            </span>
        </div>

        <div class="row">
            <label>
                Channel
                <select :value="channel" @change="channelPicked">
                    <option v-for="c in 16" :key="c" :value="c - 1">
                        {{ c }}{{ c - 1 === drumChannel ? ' (drums)' : '' }}
                    </option>
                </select>
            </label>

            <!-- A ceiling rather than the velocity itself: how hard a key or a pad is struck comes
                 from where on it the pointer landed, and this is what the hardest strike sends. -->
            <label>
                Max velocity
                <input type="range" min="1" max="127" :value="velocity" @input="velocityMoved" />
                <span class="technical">{{ velocity }}</span>
            </label>

            <span class="note">{{ sounding }}</span>
        </div>

        <KeyboardControl :velocity="velocity" @note-on="noteOn" @note-off="noteOff" />
    </section>
</template>

<script setup lang="ts">
import { computed, ref } from 'vue';
import { drumChannel } from '../engine/protocol';
import * as midi from '../services/midi-in';
import { useEngineStore } from '../stores/engine';
import KeyboardControl from './KeyboardControl.vue';

const props = defineProps<{ channel: number; velocity: number }>();
const emit = defineEmits<{ 'update:channel': [value: number]; 'update:velocity': [value: number] }>();

const store = useEngineStore();

const midiSupported = midi.isSupported;
const midiOpen = ref(midi.isOpen());
const midiDevices = ref<midi.MidiDevice[]>(midi.devices());

const armed = computed(() => store.transport === 'playing');

// What the chosen channel is currently set to sound. The drum part is not a program in the melodic
// sense at all, so it says so rather than showing an instrument name that would be a plain lie
// about what a key will do.
const sounding = computed(() => {
    const part = store.channels[props.channel];
    if (!part || part.program === undefined) {
        return '';
    }
    if (props.channel === drumChannel) {
        return `Drum part — program ${part.program + 1} selects the kit.`;
    }
    return `Bank ${part.bank}, program ${part.program! + 1} — ${part.name ?? 'nothing'}.`;
});

async function connect() {
    const devices = await midi.open(() => {
        midiDevices.value = midi.devices();
    });
    if (devices) {
        midiOpen.value = true;
        midiDevices.value = devices;
    }
}

async function listen(event: Event) {
    const id = (event.target as HTMLSelectElement).value;
    midi.listen(id === '' ? null : id, (status, data1, data2) => {
        store.sendChannel(status, data1, data2);
    });
    midiDevices.value = midi.devices();
    await store.armForKeys();
}

function channelPicked(event: Event) {
    emit('update:channel', Number((event.target as HTMLSelectElement).value));
}

function velocityMoved(event: Event) {
    emit('update:velocity', Number((event.target as HTMLInputElement).value));
}

async function noteOn(note: number, velocity: number) {
    await store.armForKeys();
    store.sendChannel(0x90 | props.channel, note, velocity);
}

function noteOff(note: number) {
    store.sendChannel(0x80 | props.channel, note, 0);
}
</script>
