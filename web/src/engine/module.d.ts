// The Emscripten module the `web` CMake preset drops into src/engine/generated/. Only what the
// worker actually touches is typed; cwrap covers the rest.

declare module '*/tabulasonora-engine.mjs' {
    export interface EngineModule {
        // eslint-disable-next-line @typescript-eslint/no-explicit-any
        cwrap(
            name: string,
            returnType: 'number' | 'string' | null,
            argTypes: ('number' | 'string')[],
        ): (...args: (number | string)[]) => any;
        _malloc(size: number): number;
        _free(pointer: number): void;
        HEAPU8: Uint8Array;
        HEAPF32: Float32Array;
    }

    const createTabulaSonoraModule: (options?: {
        locateFile?: (path: string) => string;
    }) => Promise<EngineModule>;

    export default createTabulaSonoraModule;
}

declare module '*.wasm?url' {
    const url: string;
    export default url;
}
