<template>
    <!--
        Changing any of these rebuilds the generator, which is why the note about voices stopping is
        on the panel rather than hidden in a tooltip: a vintage swap mid-song resumes at the same
        place, but whatever was ringing does not survive it.
    -->
    <section class="panel enter-rise">
        <h2>Engine</h2>

        <div class="row">
            <label>
                Vintage
                <select :value="store.settings.map" @change="mapChanged">
                    <option v-for="map in vintages" :key="map" :value="map">
                        {{ vintageName(map) }}
                    </option>
                </select>
            </label>
        </div>

        <div class="row toggles">
            <label>
                <input type="checkbox" :checked="store.settings.reverb"
                       @change="toggle('reverb', $event)" />
                Reverb
            </label>
            <label>
                <input type="checkbox" :checked="store.settings.chorus"
                       @change="toggle('chorus', $event)" />
                Chorus
            </label>
            <label>
                <input type="checkbox" :checked="store.settings.delay"
                       @change="toggle('delay', $event)" />
                Delay
            </label>
            <label>
                <input type="checkbox" :checked="store.settings.efx"
                       @change="toggle('efx', $event)" />
                EFX
            </label>
        </div>

        <div class="row">
            <label class="gain">
                Volume
                <input type="range" min="0" max="2" step="0.01" :value="store.outputGain"
                       @input="gainChanged" />
                <span class="gain-readout">{{ store.outputGain.toFixed(2) }}&times;</span>
            </label>
        </div>

        <div class="row">
            <button class="btn btn-secondary" @click="store.panic()">Panic</button>
            <button class="btn btn-secondary" :disabled="isDefault(store.settings)"
                    @click="restore">Restore defaults</button>
        </div>
    </section>
</template>

<script setup lang="ts">
import { defaultSettings, isDefault } from '../services/engine-preferences';
import { vintageName, vintages } from '../lib/tone-catalog';
import { useEngineStore } from '../stores/engine';

const store = useEngineStore();

function mapChanged(event: Event) {
    const map = Number((event.target as HTMLSelectElement).value);
    store.applySettings({ ...store.settings, map });
}

function toggle(key: 'reverb' | 'chorus' | 'delay' | 'efx', event: Event) {
    const on = (event.target as HTMLInputElement).checked;
    store.applySettings({ ...store.settings, [key]: on });
}

function gainChanged(event: Event) {
    const gain = Number((event.target as HTMLInputElement).value);
    store.setOutputGain(Math.min(2, Math.max(0, gain)));
}

// Storing the defaults and storing nothing are the same state, so this really does leave the
// browser as it was before the panel was ever touched.
function restore() {
    store.applySettings({ ...defaultSettings });
}
</script>
