<template>
    <!--
        Auto, light or dark. Auto is the absence of a choice rather than a third palette: it removes
        the data-theme attribute and lets prefers-color-scheme decide, which is what losnoco.css is
        written around. The work is all in the inline script in index.html — the theme has to be
        applied before the first paint, long before this component exists — and this calls back into
        that rather than keeping a second copy of the storage key and the apply step.
    -->
    <div class="theme-toggle" role="group" aria-label="Colour theme">
        <button
            v-for="option in choices"
            :key="option.value"
            type="button"
            class="theme-choice"
            :aria-pressed="choice === option.value ? 'true' : 'false'"
            @click="choose(option.value)"
        >
            {{ option.caption }}
        </button>
    </div>
</template>

<script setup lang="ts">
import { ref } from 'vue';

declare global {
    var tabulaTheme: { get(): string; set(choice: string): void };
}

const choices = [
    { value: 'auto', caption: 'Auto' },
    { value: 'light', caption: 'Light' },
    { value: 'dark', caption: 'Dark' },
];

const choice = ref(globalThis.tabulaTheme.get());

function choose(value: string) {
    choice.value = value;
    globalThis.tabulaTheme.set(value);
}
</script>
