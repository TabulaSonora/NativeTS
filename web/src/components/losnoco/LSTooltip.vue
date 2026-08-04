<template>
    <!--
        A tooltip that behaves like the native one and looks like the design system.

        `title` is genuinely good — it needs no script, it never clips, and screen readers know it.
        What it cannot do is style, wrap a long string readably, or appear on a touch device, and
        the mixer's patch detail is exactly the kind of long technical string it renders worst. So
        this replaces `title` only where the content earns it, and the two are never both set on
        one element: that produces the browser's tooltip *and* this one.

        Teleported to the body because the panels it is used inside establish their own stacking
        and scroll boxes, and a popover clipped by its parent is worse than no popover.
    -->
    <span
        ref="anchor"
        class="ls-tooltip-anchor"
        :aria-describedby="open ? id : undefined"
        @pointerenter="show"
        @pointerleave="hide"
        @focusin="show"
        @focusout="hide"
        @keydown.escape="hide"
    >
        <slot />
    </span>

    <Teleport to="body">
        <span v-if="open && text" :id="id" class="ls-tooltip" role="tooltip" :style="position">
            {{ text }}
        </span>
    </Teleport>
</template>

<script setup lang="ts">
import { computed, onBeforeUnmount, ref, useId } from 'vue';

const props = defineProps<{ text?: string }>();

const id = useId();
const anchor = ref<HTMLElement | null>(null);
const open = ref(false);
const rect = ref<DOMRect | null>(null);

// Pointer *enter* rather than hover intent with a delay: these labels are read while reaching for
// a control, and a delay makes the interface feel like it is deciding whether to answer.
function show() {
    if (!anchor.value || !props.text) {
        return;
    }
    rect.value = anchorRect();
    open.value = true;
    window.addEventListener('scroll', hide, { capture: true, passive: true });
}

/**
 * The box to point at.
 *
 * The anchor is `display: contents` so that wrapping a control in a tooltip cannot disturb the
 * grid or flex layout it sits in — but an element that generates no box also measures as an empty
 * rect, so the real geometry comes from the child it wrapped.
 */
function anchorRect(): DOMRect | null {
    const element = anchor.value;
    if (!element) {
        return null;
    }
    const own = element.getBoundingClientRect();
    if (own.width > 0 || own.height > 0) {
        return own;
    }
    const child = element.firstElementChild;
    return child ? child.getBoundingClientRect() : own;
}

function hide() {
    open.value = false;
    window.removeEventListener('scroll', hide, { capture: true });
}

onBeforeUnmount(hide);

/**
 * Above the anchor, flipped below when there is no room, and never off either edge.
 *
 * Fixed positioning against the viewport, measured at open time. The listener above closes on any
 * scroll rather than tracking it: a tooltip is open for a moment, and a scroll during that moment
 * means the pointer has left anyway.
 */
const position = computed(() => {
    const box = rect.value;
    if (!box) {
        return '';
    }

    const margin = 8;
    const estimatedWidth = Math.min(320, Math.max(160, (props.text?.length ?? 0) * 7));
    const below = box.bottom + margin;
    const flip = box.top < 96 && below + 64 < window.innerHeight;

    const left = Math.round(
        Math.min(
            Math.max(margin, box.left + box.width / 2 - estimatedWidth / 2),
            Math.max(margin, window.innerWidth - estimatedWidth - margin),
        ),
    );

    return flip
        ? `left: ${left}px; top: ${Math.round(below)}px; max-width: ${estimatedWidth}px;`
        : `left: ${left}px; bottom: ${Math.round(window.innerHeight - box.top + margin)}px; `
          + `max-width: ${estimatedWidth}px;`;
});
</script>

<style scoped>
.ls-tooltip-anchor {
    display: contents;
}
</style>

<!--
    Unscoped: the tooltip is teleported to the body, so a scoped attribute selector would not
    reach it. Named specifically enough that living in the global sheet costs nothing.
-->
<style>
.ls-tooltip {
    position: fixed;
    z-index: 60;
    padding: var(--space-xs) var(--space-sm);
    border: 1px solid var(--border-strong);
    border-radius: var(--radius-note);
    background: var(--surface);
    color: var(--text);
    font-size: var(--text-caption);
    line-height: 1.45;
    /* Structural: a popover has to separate from whatever it covers. */
    box-shadow: 0 2px 10px rgb(0 0 0 / 18%);
    pointer-events: none;
    animation: ls-tooltip-in var(--dur-medium) var(--ease-out);
}

@keyframes ls-tooltip-in {
    from { opacity: 0; transform: translateY(2px); }
    to   { opacity: 1; transform: none; }
}

@media (prefers-reduced-motion: reduce) {
    .ls-tooltip {
        animation: none;
    }
}
</style>
