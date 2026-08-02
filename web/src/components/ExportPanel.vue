<template>
    <section class="panel enter-rise">
        <h2>Export</h2>

        <!-- Secondary, not primary: this matters, but Play is already holding the page's one
             orange button and both are on screen at the same time. -->
        <div class="row">
            <button class="btn btn-secondary" :disabled="store.exporting || !store.song"
                    @click="run">
                {{ store.exporting ? 'Rendering…' : 'Render to WAV' }}
            </button>

            <template v-if="store.exporting">
                <button class="btn btn-ghost" @click="store.cancelExport()">Cancel</button>
                <progress max="1" :value="store.exportProgress"></progress>
                <span class="tag tag-muted">{{ Math.floor(store.exportProgress * 100) }}%</span>
            </template>
        </div>

        <p class="note">
            16-bit stereo at 32 kHz, the same path as <code>render --stream</code> on the command
            line — the file it produces is the one the command line produces.
        </p>

        <p v-if="error" class="error">{{ error }}</p>
    </section>
</template>

<script setup lang="ts">
import { ref } from 'vue';
import { useEngineStore } from '../stores/engine';

const store = useEngineStore();
const error = ref<string | null>(null);

async function run() {
    error.value = null;
    try {
        await store.exportWav();
    } catch (e) {
        error.value = e instanceof Error ? e.message : String(e);
    }
}
</script>
