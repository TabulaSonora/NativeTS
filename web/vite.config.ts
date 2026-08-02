import { execSync } from 'node:child_process';
import { defineConfig } from 'vite';
import vue from '@vitejs/plugin-vue';

// Which build this is. The site is published by hand from a working tree, so the page carries the
// commit and a dirty flag — the only way to tell from the page what is live.
function git(command: string): string {
    try {
        return execSync(command, { encoding: 'utf8' }).trim();
    } catch {
        return '';
    }
}

const commit = git('git rev-parse HEAD') || 'unknown';
const dirty = git('git status --porcelain') !== '';

export default defineConfig({
    plugins: [vue()],

    define: {
        __COMMIT__: JSON.stringify(commit),
        __DIRTY__: JSON.stringify(dirty),
    },

    // The engine worker is an ES module — the Emscripten module it imports is one.
    worker: { format: 'es' },

    build: { target: 'es2022' },
});
