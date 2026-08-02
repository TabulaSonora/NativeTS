<template>
    <!--
        Every melodic sound the loaded ROM holds, as three columns that narrow: the vintage, then
        the instrument, then the variants of that instrument the vintage defines.

        Columns rather than a bank rail and a grid of 128, because the grid had the hierarchy upside
        down: asking "which banks define a variation of THIS instrument" is the question a player
        actually has, and the answer is usually two or three lines rather than 128.
    -->
    <section class="panel enter-rise">
        <h2>Instruments</h2>

        <p v-if="!catalogue" class="note">No ROM loaded.</p>
        <p v-else-if="channel === drumChannel" class="note">
            Channel {{ drumChannel + 1 }} is the drum part. Its program changes select a kit rather
            than an instrument, and they resolve through the drum map below, not through this table.
            Choose another channel to play an instrument.
        </p>
        <template v-else>
            <div class="row">
                <label>
                    Search
                    <input type="search" :value="search" placeholder="piano, strings, 88…"
                           @input="search = ($event.target as HTMLInputElement).value.trim()" />
                </label>

                <template v-if="search.length === 0">
                    <span class="tag tag-muted">{{ catalogue.banks.length }} banks</span>
                    <span class="tag tag-muted">{{ catalogue.nativeCount.toLocaleString('en-US') }} programs</span>
                    <span class="tag tag-muted">{{ catalogue.toneCount.toLocaleString('en-US') }} distinct tones</span>
                </template>
            </div>

            <template v-if="search.length > 0">
                <!-- Searching is the one time the hierarchy is in the way: a name can be anywhere,
                     so the columns collapse to a flat list of everything that matches. -->
                <p class="note">
                    {{ hits.length }} match{{ hits.length === 1 ? '' : 'es' }}{{ hits.length === searchLimit ? `, showing the first ${searchLimit}` : '' }}.
                </p>

                <div class="hits">
                    <button v-for="hit in hits" :key="`${hit.bank}:${hit.entry.program}`"
                            :class="['hit', selected(hit.bank, hit.entry.program) ? 'on' : '']"
                            @click="select(hit.bank, hit.entry.program)">
                        <span class="technical">{{ hit.bank }} ⋅ {{ hit.entry.program + 1 }}</span>
                        <span class="name">{{ hit.entry.name }}</span>
                    </button>
                </div>
            </template>
            <template v-else>
                <div class="miller">
                    <div class="column">
                        <div class="column-head">Sound map</div>
                        <button v-for="map in vintages" :key="map"
                                :class="['item', emulation === null && map === store.settings.map ? 'on' : '']"
                                @click="showVintage(map)">
                            <span class="name">{{ vintageName(map) }}</span>
                            <span class="technical">{{ bankCounts[map] ?? 0 }}</span>
                        </button>

                        <!-- The CM-64 banks belong here rather than among the variants: they are a
                             different module's whole sound set, carried for compatibility, and a
                             file selects one for the same reason it would select a vintage. -->
                        <button v-for="bank in emulationBanks" :key="`emu${bank}`"
                                :class="['item', emulation === bank ? 'on' : '']"
                                @click="showEmulation(bank)">
                            <span class="name">{{ bankName(bank) }}</span>
                            <span class="technical">{{ nativeCount(bank) }}</span>
                        </button>
                    </div>

                    <div class="column">
                        <div class="column-head">Instrument</div>
                        <template v-for="entry in instruments" :key="entry.program">
                            <!-- Families group the capital bank, whose programs are laid out the way
                                 General MIDI says. The CM-64 sets are not in that order at all. -->
                            <div v-if="emulation === null && entry.program % familySize === 0"
                                 class="group">{{ familyOf(entry.program) }}</div>

                            <button :class="['item', entry.program === program ? 'on' : '']"
                                    @click="open(entry.program)">
                                <span class="technical">{{ entry.program + 1 }}</span>
                                <span class="name">{{ entry.name }}</span>
                            </button>
                        </template>
                    </div>

                    <div class="column">
                        <div class="column-head">Variant</div>
                        <p v-if="emulation !== null" class="note column-note">
                            A compatibility set has no variations: the bank is the map.
                        </p>
                        <template v-else>
                            <button v-for="variant in variants" :key="variant.bank"
                                    :class="['item', selected(variant.bank, program) ? 'on' : '']"
                                    @click="select(variant.bank, program)">
                                <span class="technical">{{ variant.bank }}</span>
                                <span class="name">{{ variant.entry.name }}</span>
                            </button>
                        </template>
                    </div>
                </div>

                <p class="note">
                    <template v-if="emulation !== null">
                        Bank {{ emulation }}, the {{ bankName(emulation) }} half of the Roland
                        CM-64. 127 is the LA side — the MT-32's own sound set — and 126 is the PCM
                        side, the CM-32P's. Both are the same on every vintage.
                    </template>
                    <template v-else>
                        {{ vintageName(store.settings.map) }} defines
                        {{ variants.length === 1 ? 'only the capital tone' : `${variants.length} variations` }}
                        of program {{ program + 1 }}. Bank 0 is the capital; a bank not listed has
                        no entry for this program and would sound the capital anyway.
                    </template>
                </p>
            </template>
        </template>
    </section>
</template>

<script setup lang="ts">
import { computed, ref, watch } from 'vue';
import { drumChannel, type CatalogBank, type VintageCatalog } from '../engine/protocol';
import {
    bankName,
    emulationBanks,
    familyOf,
    familySize,
    vintageName,
    vintages,
} from '../lib/tone-catalog';
import { useEngineStore } from '../stores/engine';

const props = defineProps<{ channel: number }>();

const store = useEngineStore();

// A vintage defines thousands of slots and a one-letter search matches most of them; a cap keeps a
// keystroke from laying out a page of results nobody reads.
const searchLimit = 120;

const search = ref('');
const browsing = ref<number | null>(null);

const catalogue = ref<VintageCatalog | null>(null);
const bankCounts = ref<Record<number, number>>({});

// The catalog follows the ROM and the vintage; the first-column counts need all four sweeps once.
watch(
    () => [store.rom?.sha256, store.settings.map] as const,
    async ([sha, map]) => {
        if (!sha) {
            catalogue.value = null;
            return;
        }
        catalogue.value = await store.catalog(map);
        const counts: Record<number, number> = {};
        for (const vintage of vintages) {
            counts[vintage] = (await store.catalog(vintage))?.banks.length ?? 0;
        }
        bankCounts.value = counts;
    },
    { immediate: true },
);

const part = computed(() => store.channels[props.channel]);

// Which compatibility set is being browsed, or null for an ordinary vintage. Read from the part
// rather than kept, so a file or a controller that selects bank 127 moves the first column too —
// the browser says what the channel IS, not what was last clicked in it.
const emulation = computed(() => {
    const bank = part.value?.bank;
    return bank !== undefined && bankName(bank) !== null ? bank : null;
});

// Which instrument the third column is showing. With nothing browsed yet it follows the part, so
// opening the panel shows the variants of whatever is actually sounding rather than of program 1.
const program = computed(() => browsing.value ?? part.value?.program ?? 0);

function findBank(bank: number): CatalogBank | null {
    return catalogue.value?.banks.find(b => b.bank === bank) ?? catalogue.value?.banks[0] ?? null;
}

function nativeCount(bank: number): number {
    return findBank(bank)?.nativeCount ?? 0;
}

const instruments = computed(() => {
    if (!catalogue.value) {
        return [];
    }
    if (emulation.value !== null) {
        return findBank(emulation.value)?.programs.filter(e => e.kind === 'native') ?? [];
    }
    return catalogue.value.banks[0]?.programs ?? [];
});

// Every bank that defines this program itself. A bank whose slot is a capital fallback is left out
// on purpose: it would sound identical to bank 0.
const variants = computed(() => {
    if (!catalogue.value) {
        return [];
    }
    return catalogue.value.banks
        .map(bank => ({ bank: bank.bank, entry: bank.programs[program.value]! }))
        .filter(v => v.entry.kind === 'native');
});

// Number as well as name, because "88" is how a bank is looked for and a program is as often known
// by its number as by what it is called.
const hits = computed(() => {
    if (!catalogue.value || search.value.length === 0) {
        return [];
    }
    const query = search.value.toLowerCase();
    const found = [];
    for (const bank of catalogue.value.banks) {
        for (const entry of bank.programs) {
            if (entry.kind !== 'native') {
                continue;
            }
            if (entry.name.toLowerCase().includes(query)
                || String(entry.program + 1) === search.value
                || String(bank.bank) === search.value) {
                found.push({ bank: bank.bank, entry });
                if (found.length >= searchLimit) {
                    return found;
                }
            }
        }
    }
    return found;
});

function selected(bank: number, prog: number): boolean {
    return part.value?.bank === bank && part.value?.program === prog;
}

// Leaving a compatibility set means going back to an ordinary bank, which the part has no memory
// of; the capital is the only answer that is certainly there.
async function showVintage(map: number) {
    const wasEmulation = emulation.value !== null;
    await store.applySettings({ ...store.settings, map });
    if (wasEmulation) {
        select(0, program.value);
    }
}

async function showEmulation(bank: number) {
    // The first program of the set, because the one in force belongs to a different sound map and
    // would land on whatever happens to sit at that number here.
    const first = findBank(bank)?.programs.find(e => e.kind === 'native')?.program ?? 0;
    select(bank, first);
}

// Opening an instrument sounds it as well as listing its variants: the point of the panel is to
// hear things. Into the bank being browsed, NOT bank 0 — inside a compatibility set the capital is
// a different sound map.
function open(prog: number) {
    browsing.value = prog;
    select(emulation.value ?? 0, prog);
}

async function select(bank: number, prog: number) {
    // Bank first, then program: a program change is what latches the pair, so the other order
    // would sound the old bank's tone until the next note picked up the new one.
    store.sendControl(props.channel, 0, bank);
    store.sendChannel(0xC0 | props.channel, prog, 0);

    browsing.value = prog;
    await store.armForKeys();
}
</script>
