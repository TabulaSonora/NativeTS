// Persistent storage for the one file the engine needs and the page cannot ship: the user's own
// SCCore.dll. The DLL is 27 MB; the bytes go from the picked File straight into IndexedDB, and the
// worker sees them exactly once, on the way into the engine.
//
// Same database, store and key as the previous deployment of this page, so a returning visitor's
// stored DLL carries straight over.

const DB_NAME = 'tabula-sonora';
const DB_VERSION = 1;
const STORE = 'assets';

/** The key the ROM lives under. */
export const romKey = 'sccore';

export interface StoredAsset {
    key: string;
    name: string;
    size: number;
    sha256: string;
    savedAt: string;
}

function open(): Promise<IDBDatabase> {
    return new Promise((resolve, reject) => {
        const request = indexedDB.open(DB_NAME, DB_VERSION);
        request.onupgradeneeded = () => request.result.createObjectStore(STORE);
        request.onsuccess = () => resolve(request.result);
        request.onerror = () => reject(request.error);
    });
}

function transact<T>(db: IDBDatabase, mode: IDBTransactionMode,
                     work: (store: IDBObjectStore) => IDBRequest | void): Promise<T> {
    return new Promise((resolve, reject) => {
        const tx = db.transaction(STORE, mode);
        const request = work(tx.objectStore(STORE));
        tx.oncomplete = () => resolve((request ? request.result : undefined) as T);
        tx.onerror = () => reject(tx.error);
        tx.onabort = () => reject(tx.error);
    });
}

async function sha256Hex(buffer: ArrayBuffer): Promise<string> {
    const digest = await crypto.subtle.digest('SHA-256', buffer);
    return Array.from(new Uint8Array(digest))
        .map(b => b.toString(16).padStart(2, '0'))
        .join('');
}

interface Record {
    name: string;
    bytes: ArrayBuffer;
    sha256: string;
    savedAt: string;
}

/**
 * Ask the browser not to evict us. Without it a 27 MB record is "best effort" storage and can be
 * cleared under disk pressure, which would silently send the user back to the file picker.
 */
export async function requestPersistence(): Promise<boolean> {
    if (!navigator.storage?.persist) {
        return false;
    }

    return (await navigator.storage.persisted()) || (await navigator.storage.persist());
}

/**
 * Metadata only. The bytes stay in IndexedDB until the engine actually asks for them, so a page
 * load that finds nothing cached costs one small read rather than 27 MB of decoding.
 */
export async function describe(key: string): Promise<StoredAsset | null> {
    const db = await open();
    try {
        const record = await transact<Record | undefined>(db, 'readonly', store => store.get(key));
        if (!record) {
            return null;
        }

        return {
            key,
            name: record.name,
            size: record.bytes.byteLength,
            sha256: record.sha256,
            savedAt: record.savedAt,
        };
    } finally {
        db.close();
    }
}

/** Reads a picked file, hashes it, and stores it. */
export async function putFromFile(key: string, file: File): Promise<StoredAsset> {
    const bytes = await file.arrayBuffer();
    const sha256 = await sha256Hex(bytes);
    const record: Record = { name: file.name, bytes, sha256, savedAt: new Date().toISOString() };

    const db = await open();
    try {
        await transact(db, 'readwrite', store => store.put(record, key));
    } finally {
        db.close();
    }

    return { key, name: record.name, size: bytes.byteLength, sha256, savedAt: record.savedAt };
}

export async function remove(key: string): Promise<void> {
    const db = await open();
    try {
        await transact(db, 'readwrite', store => store.delete(key));
    } finally {
        db.close();
    }
}

/**
 * A small setting stored beside the DLL, in the same database and object store. What earns a
 * setting a slot here rather than in localStorage is wanting the DLL's lifetime: the loop switch
 * rides with the instrument, survives a localStorage clear that spares site data, and leaves with
 * the database if the user clears that.
 *
 * The value is a plain string under its own key; absence is the default, as everywhere else.
 */
export async function readValue(key: string): Promise<string | null> {
    const db = await open();
    try {
        const value = await transact<unknown>(db, 'readonly', store => store.get(key));
        return typeof value === 'string' ? value : null;
    } finally {
        db.close();
    }
}

/** Writes a small setting, or removes it when `value` is null — absence is the default. */
export async function writeValue(key: string, value: string | null): Promise<void> {
    const db = await open();
    try {
        await transact(db, 'readwrite',
                       store => (value === null ? store.delete(key) : store.put(value, key)));
    } finally {
        db.close();
    }
}

/**
 * Reads the stored bytes back. The structured clone IndexedDB hands out is this caller's own, so
 * it can be transferred to the worker without touching the stored copy.
 */
export async function read(key: string): Promise<ArrayBuffer | null> {
    const db = await open();
    try {
        const record = await transact<Record | undefined>(db, 'readonly', store => store.get(key));
        return record ? record.bytes : null;
    } finally {
        db.close();
    }
}
