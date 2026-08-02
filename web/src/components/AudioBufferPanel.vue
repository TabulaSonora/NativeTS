<template>
    <!--
        How far ahead the pump runs, and what that is costing.

        The lead is the latency between a control moving and the change being heard, so the
        interesting question is how low it will go before the device starves — and that answer
        belongs to the machine and the browser, not to a constant chosen here. The readouts are
        beside the control because a number to lower it against is the whole point: starved frames
        say it has gone too far, and the realtime factor says whether there was ever any headroom
        to spend.
    -->
    <section class="panel enter-rise">
        <h2>Audio</h2>

        <div class="row">
            <label>
                Buffer
                <input type="range" :min="minimumLeadFrames" :max="maximumLeadFrames"
                       :step="leadStepFrames" :value="store.leadFrames" @input="moved" />
                <span class="technical clock">{{ milliseconds(store.leadFrames) }} ms</span>
            </label>

            <button class="btn btn-secondary btn-tiny" @click="store.resetStarved()">Reset count</button>

            <button class="btn btn-secondary btn-tiny"
                    :disabled="store.leadFrames === defaultLeadFrames"
                    @click="store.setLead(defaultLeadFrames)">Default</button>
        </div>

        <div class="row readouts">
            <template v-if="started">
                <span class="tag tag-muted">
                    {{ store.audio.sampleRate.toLocaleString('en-US') }} Hz{{ store.audio.resampling ? ' (resampled)' : '' }}
                </span>
                <span class="tag tag-muted">{{ milliseconds(store.queued) }} ms queued</span>

                <span v-if="store.realtimeFactor > 0"
                      :class="['tag', store.realtimeFactor < 1.5 ? 'tag-warn' : 'tag-success']">
                    {{ store.realtimeFactor.toFixed(1) }}&times; realtime
                </span>

                <!-- The one that decides it. Anything above zero since the last reset means the
                     device ran out of audio and invented some, which is the dropout you are
                     listening for. -->
                <span :class="['tag', store.starved > 0 ? 'tag-error' : 'tag-success']">
                    {{ store.starved }} starved frames
                </span>
            </template>
            <span v-else class="note">Nothing is playing yet, so there is nothing to measure.</span>
        </div>
    </section>
</template>

<script setup lang="ts">
import { computed } from 'vue';
import {
    defaultLeadFrames,
    leadStepFrames,
    maximumLeadFrames,
    minimumLeadFrames,
} from '../engine/protocol';
import { useEngineStore } from '../stores/engine';

const store = useEngineStore();

const started = computed(() => store.audio.state !== 'closed');

function milliseconds(frames: number): number {
    return Math.round((frames * 1000) / store.sampleRate);
}

function moved(event: Event) {
    const frames = Number((event.target as HTMLInputElement).value);
    store.setLead(Math.min(maximumLeadFrames, Math.max(minimumLeadFrames, frames)));
}
</script>
