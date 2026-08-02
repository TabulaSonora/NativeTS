<template>
    <!--
        The gate everything else is behind. The DLL is the user's own, it is read in the browser and
        stored in the browser, and it is never sent anywhere — worth saying on the page itself,
        because "drop a 27 MB file here" is otherwise a reasonable thing to be suspicious of.
    -->
    <section class="panel enter-rise">
        <h2>Sound Canvas ROM</h2>

        <template v-if="store.rom">
            <dl class="identity">
                <dt>file</dt>
                <dd class="technical">{{ store.rom.name }}</dd>
                <dt>size</dt>
                <dd class="technical">{{ store.rom.size.toLocaleString('en-US') }} bytes</dd>
                <dt>sha-256</dt>
                <dd class="technical hash">{{ store.rom.sha256 }}</dd>
                <dt>checked</dt>
                <dd class="technical">
                    {{ store.rom.verified
                        ? 'full hash, this session'
                        : 'size and PE timestamp; hash taken when stored' }}
                </dd>
            </dl>

            <div class="row">
                <span class="tag tag-success">Verified against the pinned build</span>
                <span class="note">Tables loaded.</span>
            </div>

            <div class="row">
                <button class="btn btn-secondary" :disabled="busy" @click="forget">
                    Forget stored DLL
                </button>
                <span v-if="persistent" class="tag tag-muted">Storage is persistent</span>
                <span v-else class="tag tag-warn">Storage is best-effort; the browser may evict it</span>
            </div>
        </template>

        <template v-else>
            <p>
                This needs <code>SCCore.dll</code> from your own Sound Canvas VA installation —
                exactly 27,347,456 bytes. It is read and cached in this browser, on this machine,
                and is never uploaded.
                <!-- Which build, and why only that one, is a fair question to have at the file
                     picker. The answer is long enough to belong in the documentation rather than
                     on the panel. -->
                <a href="https://tabulasonora.github.io/NativeTS/getting-started.html"
                   target="_blank" rel="noopener">Which build, and why</a>.
            </p>

            <!-- The one primary action while the page is still behind this gate. Once a ROM is
                 loaded the panel above replaces it and Play in the transport takes the orange. -->
            <div class="row">
                <input type="file" class="file-primary" accept=".dll" :disabled="busy"
                       @change="picked" />
                <span v-if="busy" class="note">{{ step }}</span>
            </div>
        </template>

        <p v-if="error" class="error">{{ error }}</p>
    </section>
</template>

<script setup lang="ts">
import { onMounted, ref } from 'vue';
import * as assets from '../services/asset-store';
import { useEngineStore } from '../stores/engine';

const store = useEngineStore();

const busy = ref(false);
const persistent = ref(false);
const error = ref<string | null>(null);
const step = ref('');

// A DLL stored on a previous visit is loaded without asking. Getting straight to the music is the
// entire point of caching it.
onMounted(async () => {
    const stored = await assets.describe(assets.romKey);
    if (stored) {
        await load(stored);
    }
});

async function picked(event: Event) {
    const input = event.target as HTMLInputElement;
    const file = input.files?.[0];
    if (!file) {
        return;
    }

    busy.value = true;
    error.value = null;
    step.value = 'reading the file and computing its SHA-256 — 27 MB, a few seconds…';

    try {
        persistent.value = await assets.requestPersistence();
        const stored = await assets.putFromFile(assets.romKey, file);
        await load(stored);
    } catch (e) {
        error.value = e instanceof Error ? e.message : String(e);
    } finally {
        busy.value = false;
    }
}

async function load(stored: assets.StoredAsset) {
    busy.value = true;
    step.value = `reading ${stored.size.toLocaleString('en-US')} bytes out of browser storage…`;

    try {
        const bytes = await assets.read(stored.key);
        if (!bytes) {
            error.value = 'The stored file could not be read back. Pick it again.';
            return;
        }

        step.value = 'verifying the build and loading the tables…';

        await store.loadRom(bytes, stored.name, stored.sha256);
        persistent.value = await assets.requestPersistence();
        error.value = null;
    } catch (e) {
        // The identity gate is the whole reason this can be trusted; say what it found and drop
        // the file rather than leaving a wrong one cached to fail again on the next visit.
        error.value = e instanceof Error ? e.message : String(e);
        await assets.remove(stored.key);
    } finally {
        busy.value = false;
    }
}

async function forget() {
    busy.value = true;
    try {
        await assets.remove(assets.romKey);
        await store.unloadRom();
        error.value = null;
    } finally {
        busy.value = false;
    }
}
</script>
