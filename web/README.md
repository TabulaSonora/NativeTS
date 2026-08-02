# Tabula Sonora — the web player

The engine, compiled to WebAssembly, behind the same two-page player the retired Blazor deployment
served: Player (`/`) for Standard MIDI Files and Live (`/live`) for playing the instrument. Fully
client-side — the user supplies `SCCore.dll`, it is hashed and cached in IndexedDB, and nothing
leaves the tab.

## Architecture

Three threads. The engine WASM lives in a dedicated Web Worker, which renders ahead of playback and
pushes 256-frame blocks over a MessagePort straight to an AudioWorklet's 4-second ring; the worklet
reports its queue depth every 10 ms *of audio*, and that report — not a timer — is what drives the
pump, which is why a 30 ms lead holds. The main thread only does UI, IndexedDB and Web MIDI.

The engine module comes from `apps/web` via the `web` CMake preset (Emscripten, from WSL on this
machine) and lands in `src/engine/generated/`, which is gitignored.

## Build

```sh
# 1. The engine (WSL; needs cmake, ninja and the emsdk on PATH)
cmake --preset web && cmake --build --preset web

# 2. The app
npm install
npm run build        # or npm run dev

# 3. Deploy (manual, replaces the live site)
netlify deploy --prod --dir=web/dist
```

## Verification

`apps/web/test/smoke.mjs` drives the module under node: full-hash ROM load, catalog sweeps, a
real-time render, and a WAV export to byte-compare against `tabula-sonora render --stream` — the
same file, not a similar one.
