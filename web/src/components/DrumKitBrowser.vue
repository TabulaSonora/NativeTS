<template>
    <!--
        The drum maps: every kit the program map reaches, and for each of them which key plays what.
        Keys are named from the melodic tone table, because drum sounds are melodic tones. Kit names
        are not in the ROM at all and are carried in drum-kit-names.ts instead.
    -->
    <section class="panel enter-rise">
        <h2>Drum kits</h2>

        <p v-if="!catalogue" class="note">No ROM loaded.</p>
        <template v-else>
            <div class="row">
                <label>
                    Kit set
                    <select :value="row" @change="rowChanged">
                        <option v-for="(_, index) in rowMaps" :key="index" :value="index">
                            {{ rowName(index) }}
                        </option>
                    </select>
                </label>

                <span class="tag tag-muted">{{ catalogue.kits.length }} kits</span>
            </div>

            <div class="kits">
                <button v-for="kit in catalogue.kits" :key="kit.kit"
                        :class="['kit', kit.kit === drumKit ? 'on' : '']"
                        @click="selectKit(kit)">
                    <span class="name">{{ kitName(kit) }}</span>
                    <span class="count">{{ kit.keys.length }} keys</span>
                </button>
            </div>

            <template v-if="showing">
                <div class="pads">
                    <button v-for="key in showing.keys" :key="key.note" class="pad"
                            :title="explain(key)"
                            @pointerdown="strike(key.note, $event)">
                        <span class="technical">{{ noteName(key.note) }}</span>
                        <span class="name">{{ key.name }}</span>
                    </button>
                </div>

                <p class="note">
                    Kit #{{ showing.kit }}, {{ showing.keys.length }} keys. A pad strikes channel
                    {{ drumChannel + 1 }} as hard as where you hit it — soft at the left edge, up to
                    the panel's ceiling at the right. Drums ring out, so a pad has nothing to
                    release. Hover one for its level, coarse pitch, mute group and pan.
                </p>
            </template>
        </template>
    </section>
</template>

<script setup lang="ts">
import { computed, ref, watch } from 'vue';
import { drumChannel, type DrumCatalog, type DrumKeyEntry, type DrumKitEntry } from '../engine/protocol';
import { forRow, rowMaps, rowName } from '../lib/drum-kit-names';
import { noteName } from '../lib/tone-catalog';
import { engine } from '../services/engine-client';
import { useEngineStore } from '../stores/engine';

const props = withDefaults(defineProps<{ velocity?: number }>(), { velocity: 127 });

const store = useEngineStore();

const catalogue = ref<DrumCatalog | null>(null);

const row = computed(() => store.snapshot?.engine?.effectiveDrumMapRow ?? 0);
const drumKit = computed(() => store.snapshot?.engine?.drumKit ?? -1);

watch(
    () => [store.rom?.sha256, row.value] as const,
    async ([sha, currentRow]) => {
        catalogue.value = sha ? await store.drumCatalog(currentRow) : null;
    },
    { immediate: true },
);

// Which kit is showing is not this panel's state: it is the engine's, because selecting one sends
// the program change that selects it and a song or a controller moves it the same way.
const showing = computed(() => catalogue.value?.kits.find(k => k.kit === drumKit.value) ?? null);

function kitName(kit: DrumKitEntry): string {
    const named = forRow(row.value, kit.programs[0]!);
    if (named) {
        return named;
    }
    const programs = kit.programs.slice(0, 3).map(p => p + 1).join(', ');
    return `PC ${programs}`;
}

function rowChanged(event: Event) {
    const chosen = Number((event.target as HTMLSelectElement).value);
    engine.post({ type: 'setDrumMapRow', row: chosen });

    // The row only decides how the *next* program change resolves, so without re-sending the one
    // in force the part would go on sounding the other map's kit while this panel showed the new
    // map's list.
    const part = store.channels[drumChannel];
    if (part && part.program !== undefined) {
        store.sendChannel(0xC0 | drumChannel, part.program, 0);
    }
}

// Selecting a kit is a program change, not a setting: the engine has no other way in, and an
// undefined program leaves the kit alone rather than silencing the part.
async function selectKit(kit: DrumKitEntry) {
    store.sendChannel(0xC0 | drumChannel, kit.programs[0]!, 0);
    await store.armForKeys();
}

// Pad width in CSS pixels, mirroring --pad-width in app.css; the pad grid uses a fixed track
// rather than a stretching one to keep the two agreeing.
const padWidth = 112;

// Left to right is soft to hard. Pointer-down rather than click, so a pad answers on the way down
// like a drum does — and pads have nothing to release, because drums ring out.
async function strike(note: number, event: PointerEvent) {
    await store.armForKeys();
    const force = Math.min(127, Math.max(1, Math.round((event.offsetX / padWidth) * props.velocity)));
    store.sendChannel(0x90 | drumChannel, note, force);
}

function explain(key: DrumKeyEntry): string {
    return `tone #${key.tone} · level ${key.level} · pitch ${key.pitch} · `
        + `group ${key.group === 0 ? 'none' : key.group} · pan ${key.pan}`;
}
</script>
