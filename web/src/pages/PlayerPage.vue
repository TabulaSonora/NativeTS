<template>
    <template v-if="store.rom">
        <TransportPanel />
        <MixerPanel />
        <EngineControlsPanel />
        <ExportPanel />
        <AudioBufferPanel />
    </template>
</template>

<script setup lang="ts">
import AudioBufferPanel from '../components/AudioBufferPanel.vue';
import EngineControlsPanel from '../components/EngineControlsPanel.vue';
import ExportPanel from '../components/ExportPanel.vue';
import MixerPanel from '../components/MixerPanel.vue';
import TransportPanel from '../components/TransportPanel.vue';
import { useEngineStore } from '../stores/engine';

const store = useEngineStore();
</script>
