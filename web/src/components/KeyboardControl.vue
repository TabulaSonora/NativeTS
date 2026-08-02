<template>
    <!--
        An on-screen keyboard for playing without hardware. Pointer events rather than mouse events,
        so it works under a finger as well as a cursor, and no pointer capture is kept so that
        dragging across the keys glissandos instead of holding the first one.

        How far down the key the pointer lands is the velocity, which is the nearest a mouse gets to
        playing dynamics: near the pivot at the top is soft, at the front edge is as hard as the
        panel's ceiling allows.
    -->
    <div class="keyboard" @pointerleave="releaseAll">
        <button
            v-for="key in keys"
            :key="key.note"
            :class="['key', key.black ? 'black' : 'white', held.has(key.note) ? 'down' : '']"
            :style="key.style"
            @pointerdown="down(key.note, $event)"
            @pointerup="up(key.note)"
            @pointerenter="enter(key.note, $event)"
            @pointerleave="up(key.note)"
        >{{ key.note === 60 ? 'C4' : '' }}</button>
    </div>
</template>

<script setup lang="ts">
import { computed, reactive } from 'vue';

const props = withDefaults(defineProps<{
    firstNote?: number;
    keyCount?: number;
    velocity?: number;
}>(), { firstNote: 48, keyCount: 25, velocity: 127 });

const emit = defineEmits<{
    noteOn: [note: number, velocity: number];
    noteOff: [note: number];
}>();

// White and black key heights in CSS pixels, mirroring --key-height and --key-height-black in
// app.css. A pointer event carries where in the element it landed but not how big the element is,
// so the two agree by hand: change these and change the stylesheet.
const whiteHeight = 128;
const blackHeight = 80;

function isBlack(note: number): boolean {
    return [1, 3, 6, 8, 10].includes(note % 12);
}

// White keys are laid out in sequence and black keys float between them, so a black key's position
// is the count of white keys below it rather than its semitone index.
const keys = computed(() => {
    const list = [];
    let whites = 0;
    for (let offset = 0; offset < props.keyCount; offset++) {
        const note = props.firstNote + offset;
        const black = isBlack(note);
        list.push({
            note,
            black,
            style: black
                ? `left: calc(${whites} * var(--key-width) - var(--key-width) * 0.3);`
                : `left: calc(${whites} * var(--key-width));`,
        });
        if (!black) {
            whites++;
        }
    }
    return list;
});

const held = reactive(new Set<number>());

// Down the key is harder, which is the way a struck key behaves. offsetY is relative to the key
// because the key has no element children — its label is a bare text node.
function velocityAt(note: number, event: PointerEvent): number {
    const depth = event.offsetY / (isBlack(note) ? blackHeight : whiteHeight);
    return Math.min(127, Math.max(1, Math.round(depth * props.velocity)));
}

function down(note: number, event: PointerEvent) {
    if (held.has(note)) {
        return;
    }
    held.add(note);
    // Release implicit capture so dragging onto the next key glissandos.
    (event.target as HTMLElement).releasePointerCapture?.(event.pointerId);
    emit('noteOn', note, velocityAt(note, event));
}

// Buttons is a bitmask of what is currently pressed; zero means the pointer is only passing over,
// which must not sound a note. A glissando takes its velocity from the height the pointer is
// dragged at, so leaning into the keys mid-run gets louder.
function enter(note: number, event: PointerEvent) {
    if (event.buttons !== 0) {
        down(note, event);
    }
}

function up(note: number) {
    if (held.delete(note)) {
        emit('noteOff', note);
    }
}

function releaseAll() {
    for (const note of [...held]) {
        up(note);
    }
}
</script>
