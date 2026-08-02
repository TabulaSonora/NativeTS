<template>
    <section class="panel enter-rise">
        <h2>Song</h2>

        <div class="row">
            <input type="file" accept=".mid,.midi" @change="load" />
            <span v-if="store.song" class="technical">
                {{ store.song.name }} — {{ format(store.lengthSamples) }}
            </span>
        </div>

        <template v-if="store.song">
            <!-- Play is the page's single primary action once a ROM is loaded, so it is the only
                 orange button on screen; everything else here, and Render to WAV below, stays
                 secondary. -->
            <div class="row transport">
                <button class="btn btn-primary" @click="play"
                        :disabled="store.transport === 'playing' && store.mode === 'song'">Play</button>
                <button class="btn btn-secondary" @click="store.pause()"
                        :disabled="store.transport !== 'playing'">Pause</button>
                <button class="btn btn-ghost" @click="store.seek(0)">Rewind</button>
                <button class="btn btn-ghost" @click="nudge(-10)">&minus;10s</button>
                <button class="btn btn-ghost" @click="nudge(10)">+10s</button>
            </div>

            <input type="range" class="scrub"
                   min="0" :max="store.lengthSamples" step="320"
                   :value="store.audiblePosition"
                   @change="scrub" />

            <div class="row readouts">
                <span class="technical clock">
                    {{ format(store.audiblePosition) }} / {{ format(store.lengthSamples) }}
                </span>
                <span class="tag tag-muted">{{ store.activeVoices }} of 64 voices</span>
                <span class="tag tag-muted">{{ store.noteCount }} notes</span>
            </div>
        </template>

        <p v-if="error" class="error">{{ error }}</p>
    </section>
</template>

<script setup lang="ts">
import { ref } from 'vue';
import { useEngineStore } from '../stores/engine';

const store = useEngineStore();
const error = ref<string | null>(null);

function format(samples: number): string {
    const seconds = Math.floor(samples / store.sampleRate);
    return `${Math.floor(seconds / 60)}:${String(seconds % 60).padStart(2, '0')}`;
}

async function load(event: Event) {
    const file = (event.target as HTMLInputElement).files?.[0];
    if (!file) {
        return;
    }

    error.value = null;
    try {
        await store.stop();
        await store.loadSong(await file.arrayBuffer(), file.name);
    } catch (e) {
        error.value = e instanceof Error ? e.message : String(e);
    }
}

async function play() {
    error.value = null;
    try {
        await store.playSong();
    } catch (e) {
        error.value = e instanceof Error ? e.message : String(e);
    }
}

function nudge(seconds: number) {
    store.seek(store.audiblePosition + seconds * store.sampleRate);
}

function scrub(event: Event) {
    store.seek(Number((event.target as HTMLInputElement).value));
}
</script>
