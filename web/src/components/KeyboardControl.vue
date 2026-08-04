<template>
    <!--
        An on-screen keyboard for playing without hardware. Pointer events rather than mouse events,
        so it works under a finger as well as a cursor, and no pointer capture is kept so that
        dragging across the keys glissandos instead of holding the first one.

        How far down the key the pointer lands is the velocity, which is the nearest a mouse gets to
        playing dynamics: near the pivot at the top is soft, at the front edge is as hard as the
        panel's ceiling allows.
    -->
    <div ref="root" class="keyboard" :style="width" @pointerleave="releaseAll">
        <button
            v-for="key in keys"
            :key="key.note"
            :class="['key', key.black ? 'black' : 'white', held.has(key.note) ? 'down' : '']"
            :style="key.style"
            @pointerdown="down(key.note, $event)"
            @pointerup="up(key.note)"
            @pointerenter="enter(key.note, $event)"
            @pointerleave="up(key.note)"
        >{{ key.label }}</button>
    </div>
</template>

<script setup lang="ts">
import { computed, onBeforeUnmount, onMounted, reactive, ref } from 'vue';

const props = withDefaults(defineProps<{
    firstNote?: number;
    keyCount?: number;
    velocity?: number;
}>(), { firstNote: 0, keyCount: 0, velocity: 127 });

const emit = defineEmits<{
    noteOn: [note: number, velocity: number];
    noteOff: [note: number];
}>();

// White and black key heights in CSS pixels, mirroring --key-height and --key-height-black in
// app.css. A pointer event carries where in the element it landed but not how big the element is,
// so the two agree by hand: change these and change the stylesheet.
const whiteHeight = 128;
const blackHeight = 80;

// The white key the layout aims for. Not a floor and not a ceiling: it is what the octave count is
// chosen against, and the keys then divide the panel exactly, so the real width lands within a few
// pixels either side of it.
const targetWhite = 34;

/// Octaves, not keys: a keyboard that ends mid-octave reads as cut off rather than as sized.
const minOctaves = 2;
const maxOctaves = 7;

function isBlack(note: number): boolean {
    return [1, 3, 6, 8, 10].includes(note % 12);
}

const root = ref<HTMLElement | null>(null);
const available = ref(0);

let observer: ResizeObserver | null = null;
onMounted(() => {
    if (!root.value) {
        return;
    }
    available.value = root.value.clientWidth;
    // The panel is resizable and the page is responsive, so the count cannot be decided once at
    // mount. `contentRect` is the box the keys divide, which is the number this needs.
    observer = new ResizeObserver(entries => {
        const box = entries[0]?.contentRect;
        if (box) {
            available.value = box.width;
        }
    });
    observer.observe(root.value);
});
onBeforeUnmount(() => {
    observer?.disconnect();
    observer = null;
});

/**
 * How much keyboard fits, in whole octaves.
 *
 * Width buys *keys*, not bigger keys — a wider panel should let you play a wider range, which is
 * what an instrument does with the space. Below two octaves there is not enough left to play, and
 * above seven there is no more piano.
 */
const octaves = computed(() => {
    const whites = Math.floor((available.value || targetWhite * 7 * 2) / targetWhite);
    return Math.max(minOctaves, Math.min(maxOctaves, Math.floor(whites / 7)));
});

/**
 * Where the range starts — always a C, and placed so middle C stays near the middle.
 *
 * An instrument that grew only upward would push middle C to the left edge as the panel widened,
 * and the hand would have to move to find it. Growing around it means the note under the same
 * place on screen stays roughly the same note.
 */
const firstNote = computed(() => {
    if (props.firstNote > 0) {
        return props.firstNote;
    }
    const below = Math.floor(octaves.value / 2);
    return Math.max(12, Math.min(108 - octaves.value * 12, 60 - below * 12));
});

// One extra key so the range ends on the C it started on, which is how a keyboard is counted.
const keyCount = computed(() => (props.keyCount > 0 ? props.keyCount : octaves.value * 12 + 1));

// White keys are laid out in sequence and black keys float between them, so a black key's position
// is the count of white keys below it rather than its semitone index.
const keys = computed(() => {
    const list = [];
    let whites = 0;
    for (let offset = 0; offset < keyCount.value; offset++) {
        const note = firstNote.value + offset;
        const black = isBlack(note);
        list.push({
            note,
            black,
            // Every C, not only middle C. One label was orientation enough across two octaves; over
            // seven it is a lone landmark in a field of identical keys, and counting to find G5 is
            // not what an instrument should ask.
            label: !black && note % 12 === 0 ? `C${note / 12 - 1}` : '',
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

/**
 * The chosen octaves then divide the panel exactly, so no strip is left over on the right.
 *
 * Every key is positioned off `--key-width` — the white keys' own width, the black keys' 0.6 of
 * it, and every `left` — so redefining that one property in percent is the whole of the layout:
 * the percentages resolve against `.keyboard`, which is the keys' containing block.
 *
 * Only the width follows the panel. The heights stay the fixed pixel values `velocityAt` divides
 * by, so where a pointer lands down the key still means the same velocity at every size.
 */
const width = computed(() => {
    const whites = keys.value.filter(key => !key.black).length;
    return `--key-width: calc(100% / ${Math.max(1, whites)});`;
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
