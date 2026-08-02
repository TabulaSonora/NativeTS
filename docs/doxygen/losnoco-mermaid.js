/*
 * Mermaid, drawn in LoSnoCo colours.
 *
 * Doxygen emits its own module for client-side Mermaid and initialises it with the stock
 * `default` theme, which has no idea what page it is on. There is no Doxyfile setting for the
 * theme variables, so this file re-initialises the same module instance -- the import below
 * resolves to the one already in the module cache, provided the URL matches MERMAID_JS_URL in
 * docs/Doxyfile exactly -- against Mermaid's `base` theme, and hands it the palette read off
 * the page.
 *
 * Reading it off the page rather than writing it down here is what makes the diagrams follow
 * the dark-mode toggle: the toggle sets a class on <html>, the observer at the bottom notices,
 * and the graphs are drawn again from their source. Mermaid replaces the source with an <svg>
 * on first render, so the source is captured before that happens.
 *
 * Load order matters. Doxygen's module runs first and would auto-render on `load` with the
 * wrong palette; this one runs during the same deferred pass, before `load` fires, and turns
 * startOnLoad off in time.
 */

import mermaid from 'https://cdn.jsdelivr.net/npm/mermaid@11.12.0/dist/mermaid.esm.min.mjs';

// Captured up front: after the first render each <pre> holds an <svg> instead of the graph.
const graphs = Array.from(document.querySelectorAll('pre.mermaid'), (element) => ({
    element,
    source: element.textContent,
}));

// A custom property computes to the text it was written as, so asking <html> for --bg returns
// the light-dark() call rather than a colour Mermaid could parse. Assigning it to a real colour
// property and reading that back is what resolves it, theme and all.
const probe = document.createElement('span');
probe.style.display = 'none';

function resolve(name) {
    probe.style.backgroundColor = `var(${name})`;
    return getComputedStyle(probe).backgroundColor;
}

function themeVariables() {
    const style = getComputedStyle(document.documentElement);

    document.body.append(probe);

    const bg = resolve('--bg');
    const surface = resolve('--surface');
    const surface2 = resolve('--surface-2');
    const text = resolve('--text');
    const muted = resolve('--muted');
    const border = resolve('--border');
    const borderStrong = resolve('--border-strong');
    const purple = resolve('--purple-text');

    probe.remove();

    return {
        darkMode: style.colorScheme.includes('dark'),
        fontFamily: style.getPropertyValue('--font-sans').trim(),
        fontSize: '14px',

        background: surface,
        // A node is a card: the page's own background, outlined by a hairline.
        primaryColor: bg,
        primaryBorderColor: borderStrong,
        primaryTextColor: text,
        secondaryColor: surface2,
        secondaryBorderColor: border,
        secondaryTextColor: text,
        tertiaryColor: surface2,
        tertiaryBorderColor: border,
        tertiaryTextColor: text,

        mainBkg: bg,
        nodeBorder: borderStrong,
        nodeTextColor: text,
        titleColor: text,
        textColor: text,

        lineColor: muted,
        edgeLabelBackground: surface,

        clusterBkg: surface2,
        clusterBorder: border,

        // Notes are the annotation register, which is purple's.
        noteBkgColor: surface2,
        noteBorderColor: purple,
        noteTextColor: text,
    };
}

async function draw() {
    if (graphs.length === 0) {
        return;
    }

    mermaid.initialize({
        startOnLoad: false,
        theme: 'base',
        themeVariables: themeVariables(),
        flowchart: { curve: 'basis', useMaxWidth: true },
    });

    for (const graph of graphs) {
        graph.element.removeAttribute('data-processed');
        graph.element.textContent = graph.source;
    }

    await mermaid.run({ nodes: graphs.map((graph) => graph.element) });
}

await draw();

// The toggle swaps `dark-mode` and `light-mode` on <html>; either one means redraw.
const themeChanged = new MutationObserver(() => {
    draw().catch(() => {
        // A failed redraw leaves the previous SVG in place, which is the better of the two
        // wrong answers: a stale palette beats a page with holes in it.
    });
});

themeChanged.observe(document.documentElement, { attributeFilter: ['class'] });
