// The main thread's handle on the engine worker: request/response over postMessage, plus the
// snapshot stream the stores redraw from.
//
// A module-scope singleton on purpose — the reference app's hard-won lesson is that the engine and
// its transport belong to the layout, not to a page: a page is disposed on every navigation, and
// the first nav click would otherwise silence the instrument.

import type { PumpSnapshot, WorkerMessage, WorkerRequest } from '../engine/protocol';

type Pending = { resolve: (value: unknown) => void; reject: (error: Error) => void };

// Omit does not distribute over a union on its own, and a non-distributed Omit collapses the
// request variants into their common keys.
type DistributiveOmit<T, K extends PropertyKey> = T extends unknown ? Omit<T, K> : never;

class EngineClient {
    private readonly worker: Worker;
    private readonly pending = new Map<number, Pending>();
    private nextId = 1;

    /** The latest rolled-up state; null until the worker has instantiated the module. */
    snapshot: PumpSnapshot | null = null;

    onSnapshot: ((snapshot: PumpSnapshot) => void) | null = null;
    onExportDone: ((bytes: ArrayBuffer) => void) | null = null;
    onFatal: ((error: string) => void) | null = null;

    constructor() {
        this.worker = new Worker(new URL('../worker/engine.worker.ts', import.meta.url), {
            type: 'module',
        });

        this.worker.onmessage = event => this.receive(event.data as WorkerMessage);
    }

    private receive(message: WorkerMessage) {
        switch (message.type) {
            case 'reply': {
                const pending = this.pending.get(message.id);
                if (pending) {
                    this.pending.delete(message.id);
                    if (message.ok) {
                        pending.resolve(message.result);
                    } else {
                        pending.reject(new Error(message.error ?? 'engine error'));
                    }
                }
                return;
            }

            case 'snapshot':
                this.snapshot = message;
                this.onSnapshot?.(message);
                return;

            case 'exportDone':
                this.onExportDone?.(message.bytes);
                return;

            case 'fatal':
                this.onFatal?.(message.error);
                return;
        }
    }

    /** Fire-and-forget: controls whose effect comes back through the snapshot stream. */
    post(request: WorkerRequest, transfer: Transferable[] = []): void {
        this.worker.postMessage(request, transfer);
    }

    /** Request/response for calls with a result or a failure worth surfacing. */
    call<T>(request: DistributiveOmit<Extract<WorkerRequest, { id: number }>, 'id'>,
            transfer: Transferable[] = []): Promise<T> {
        const id = this.nextId++;
        return new Promise<T>((resolve, reject) => {
            this.pending.set(id, { resolve: resolve as (value: unknown) => void, reject });
            this.worker.postMessage({ ...request, id }, transfer);
        });
    }
}

export const engine = new EngineClient();
