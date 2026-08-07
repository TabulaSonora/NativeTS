# Tabula Sonora — the web player

The engine, compiled to WebAssembly, behind the same two-page player the retired Blazor deployment
served: Player ([`/`](https://tabula-sonora.kddlb.cl/)) for songs — SMF and every foreign format
`ts::formats::to_smf` converts — and Live
([`/live`](https://tabula-sonora.kddlb.cl/live)) for playing the instrument. Fully client-side —
the user supplies `SCCore.dll`, it is hashed and cached in IndexedDB, and nothing leaves the tab.

Live at [**tabula-sonora.kddlb.cl**](https://tabula-sonora.kddlb.cl). The long-form write-up — the
thread model, the sound-set browser, what the browser is allowed to remember — is
[the documentation's page on this build](https://tabulasonora.github.io/NativeTS/web.html);
this file is the build and deploy procedure only.

## Architecture

Three threads. The engine WASM lives in a dedicated Web Worker, which renders ahead of playback and
pushes 256-frame blocks over a MessagePort straight to an AudioWorklet's 4-second ring; the worklet
reports its queue depth every 10 ms *of audio*, and that report — not a timer — is what drives the
pump, which is why a 30 ms lead holds. The main thread only does UI, IndexedDB and Web MIDI.

The engine module comes from `apps/web` via the `web` CMake preset (Emscripten) and lands in
`src/engine/generated/`, which is gitignored.

## Build

```sh
# 1. The engine (needs cmake, ninja and an Emscripten SDK)
cmake --preset web && cmake --build --preset web

# 2. The app
npm install
npm run build        # or npm run dev
```

Any Emscripten install will do, and the preset finds it: `cmake/emscripten-toolchain.cmake` looks at
`$EMSDK` first, then at the layouts around whichever `emcc` is on `PATH`, then asks `em-config`. That
covers an emsdk checkout, a Homebrew `emscripten` on macOS and the Linux distribution packages alike.
`-DCMAKE_TOOLCHAIN_FILE=…` still overrides the lot. vcpkg is not involved; the one dependency,
nlohmann_json, is fetched during configure, so the build needs the network the first time.

## Deploy

Automatic. `.github/workflows/web.yml` runs on every push to main and on manual dispatch: it builds
the engine against a pinned Emscripten SDK, runs `npm ci && npm run build` — so `vue-tsc` gates the
deploy, and a type error fails the run instead of shipping — and then `netlify deploy --prod` from
the repository root, which is what puts the root `netlify.toml` in scope. Deploys are serialised and
never cancelled mid-flight, so a second push queues rather than interrupting an upload in progress.

Deploying by hand still works and is still the same command, for a branch or for when the runner is
unavailable. It replaces the live site immediately; there is no review step.

```sh
netlify deploy --prod --dir=web/dist
```

What CI cannot do is the smoke test below, which needs `SCCore.dll`. That stays a developer-machine
step, so a green run means the app type-checks and builds, not that the engine still renders
correctly.

## Verification

`apps/web/test/smoke.mjs` drives the module under node: full-hash ROM load, catalog sweeps, a
real-time render, and a WAV export to byte-compare against `tabula-sonora render --stream` — the
same file, not a similar one.

```sh
node apps/web/test/smoke.mjs --rom SCCore.dll --midi testdata/canyon.mid --map 4 --out web.wav
tabula-sonora render --dll SCCore.dll --map 4 --stream testdata/canyon.mid native.wav
cmp web.wav native.wav
```

Build both sides from the same commit before believing a mismatch. A stale native binary differs
from a fresh WASM one for reasons that have nothing to do with Emscripten, and the difference is
large enough to look like a real fault.
