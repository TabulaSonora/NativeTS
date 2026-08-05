<template>
    <section class="panel transport-panel enter-rise">
        <h2>Song</h2>

        <div class="row">
            <!-- Everything ts::formats::to_smf converts, not only SMF. All but LDS are recognised
                 by content, so the list is a file-picker convenience rather than the test; LDS has
                 no magic at all and is recognised through the name, which is why the name is what
                 gets handed to the worker beside the bytes. -->
            <input type="file"
                   accept=".mid,.midi,.smf,.rmi,.rmid,.mids,.mus,.xmi,.xmid,.gmf,.hmi,.hmp,.hmq,.xmf,.mxmf,.lds"
                   @change="load" />
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
                <!-- The switch, not a per-song choice: it is remembered beside the DLL and holds
                     for every song until turned off. Wraps at the file's own loop points, or over
                     the whole song when it declares none. -->
                <LSTooltip :text="store.songHasLoop
                               ? 'This file declares loop points; the song wraps at them'
                               : 'No loop points in this file; the whole song wraps'">
                    <label class="loop-switch">
                        <input type="checkbox" :checked="store.looping" @change="toggleLoop" />
                        Loop
                    </label>
                </LSTooltip>
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
                <span v-if="store.songHasLoop" class="tag tag-muted"
                      title="The file declares its own loop points">loop points</span>
            </div>
        </template>

        <p v-if="error" class="error">{{ error }}</p>
    </section>
</template>

<script setup lang="ts">
import { ref } from 'vue';
import { useEngineStore } from '../stores/engine';
import LSTooltip from './losnoco/LSTooltip.vue';

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
        // Picking a file is the gesture; see the store's autoplaySong. A false return is the
        // browser declining, not a failure, and needs no message — the song is loaded and Play is
        // right there.
        await store.autoplaySong();
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

function toggleLoop(event: Event) {
    store.setLooping((event.target as HTMLInputElement).checked);
}
</script>
