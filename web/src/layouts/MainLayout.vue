<template>
    <!--
        Everything both pages share: the identity of the page, the nav between them, and the ROM
        gate. The gate lives here rather than on each page because it is the same gate — neither
        page can do anything without a DLL — and because the stored-DLL load in RomLoader runs once
        for the session instead of once per navigation.

        Above the header rather than inside it, and that is not a layout preference: a sticky
        element is confined to its containing block, so a nav inside <header> would unstick and
        scroll away the moment the title block left the screen.
    -->
    <nav class="nav">
        <router-link to="/" exact-active-class="active" aria-current-value="page">Player</router-link>
        <router-link to="/live" active-class="active" aria-current-value="page">Live</router-link>
    </nav>

    <header>
        <div class="page-head">
            <div>
                <span class="sec-label">Sound Canvas VA</span>
                <h1>Tabula Sonora</h1>
            </div>
            <ThemeToggle />
        </div>

        <p class="lede">
            A native reimplementation of the Roland Sound Canvas VA synth voice, compiled to
            WebAssembly and running entirely in this tab. Your <code>SCCore.dll</code> is read
            here, cached here, and never leaves the machine.
        </p>
    </header>

    <main>
        <RomLoader />
        <p v-if="store.error" class="error">{{ store.error }}</p>
        <router-view />
    </main>

    <footer>
        <p>
            32 kHz internally, as the hardware is. Everything this page plays — the samples, the
            tables, even the effect coefficients — is decoded from the DLL you supplied; the one
            Roland-derived thing it carries is the drum kit name list.
        </p>
        <p class="colophon">
            <a href="https://github.com/TabulaSonora/NativeTS" target="_blank" rel="noopener">Source on GitHub</a>
            &middot;
            <a href="https://github.com/TabulaSonora/NativeTS/blob/main/LICENSE" target="_blank" rel="noopener">BSD 3-Clause</a>
            &middot;
            <a href="https://github.com/TabulaSonora/NativeTS/blob/main/NOTICE.md" target="_blank" rel="noopener">Third-party rights</a>

            <!-- Which build this is. The site is published by hand from a working tree, so without
                 this there is no way to tell from the page whether what is live is what was last
                 pushed. -->
            <template v-if="commit !== 'unknown'">
                &middot;
                <a class="technical" target="_blank" rel="noopener"
                   :href="`https://github.com/TabulaSonora/NativeTS/commit/${commit}`">
                    {{ commit.slice(0, 8) }}
                </a>
                <span v-if="dirty" class="tag tag-warn">modified</span>
            </template>
        </p>
    </footer>
</template>

<script setup lang="ts">
import RomLoader from '../components/RomLoader.vue';
import ThemeToggle from '../components/ThemeToggle.vue';
import { useEngineStore } from '../stores/engine';

const store = useEngineStore();
store.init();

const commit = __COMMIT__;
const dirty = __DIRTY__;
</script>
