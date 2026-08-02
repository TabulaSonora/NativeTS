<template>
    <template v-if="store.rom">
        <LivePlayPanel v-model:channel="channel" v-model:velocity="velocity" />
        <InstrumentPicker :channel="channel" />
        <DrumKitBrowser :velocity="velocity" />
        <MixerPanel />
        <EngineControlsPanel />
        <AudioBufferPanel />
    </template>
</template>

<script setup lang="ts">
import { ref } from 'vue';
import AudioBufferPanel from '../components/AudioBufferPanel.vue';
import DrumKitBrowser from '../components/DrumKitBrowser.vue';
import EngineControlsPanel from '../components/EngineControlsPanel.vue';
import InstrumentPicker from '../components/InstrumentPicker.vue';
import LivePlayPanel from '../components/LivePlayPanel.vue';
import MixerPanel from '../components/MixerPanel.vue';
import { useEngineStore } from '../stores/engine';

const store = useEngineStore();

// The channel the keyboard plays on and the strike ceiling, shared by the picker and the pads.
// Page state rather than store state: they describe this page's controls, not the engine.
const channel = ref(0);
const velocity = ref(127);
</script>
