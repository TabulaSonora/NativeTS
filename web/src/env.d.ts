/// <reference types="vite/client" />

// Build stamps injected by vite.config.ts.
declare const __COMMIT__: string;
declare const __DIRTY__: boolean;

declare module '*.vue' {
    import type { DefineComponent } from 'vue';
    const component: DefineComponent<object, object, unknown>;
    export default component;
}
