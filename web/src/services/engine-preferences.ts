// Remembers the vintage and the three effect toggles across visits, in localStorage.
//
// Two habits are taken from the theme script deliberately. Storage access throws where the browser
// has disabled it, and a preference is not worth failing the page over, so both directions swallow
// that. And the default is the *absence* of an entry rather than an entry saying "default", so a
// user who never touches the Engine panel leaves nothing behind and a change to the defaults
// reaches them.

import type { EngineSettings } from '../engine/protocol';
import * as assets from './asset-store';

/** Storage key, in the same namespace as the theme's tabula-sonora.theme. */
const KEY = 'tabula-sonora.engine';

/**
 * The loop switch's key — in the asset database rather than localStorage, deliberately: the DLL
 * lives there, and this setting is asked to live exactly as long as the instrument does. Async
 * where the others are not, because IndexedDB is.
 */
const LOOP_KEY = 'setting.loop';

/** Off unless remembered on; a storage failure reads as the default, never as an error. */
export async function readLoop(): Promise<boolean> {
    try {
        return (await assets.readValue(LOOP_KEY)) === '1';
    } catch {
        return false;
    }
}

/** Writes the loop switch, or removes the entry when off — absence is the default here too. */
export async function writeLoop(on: boolean): Promise<void> {
    try {
        await assets.writeValue(LOOP_KEY, on ? '1' : null);
    } catch {
        // The switch still holds for this page view; it just will not be remembered.
    }
}

/**
 * The output trim's own key. Separate from the engine entry: that format is shared with the
 * previous deployment of this page, and a fifth field would invalidate every preference already
 * stored there.
 */
const GAIN_KEY = 'tabula-sonora.gain';

/** Unity, the trim every DLL comparison rests on. */
export const defaultGain = 1.0;

export function readGain(): number {
    let stored: string | null;

    try {
        stored = localStorage.getItem(GAIN_KEY);
    } catch {
        return defaultGain;
    }

    if (stored === null) {
        return defaultGain;
    }

    const gain = Number(stored);
    return Number.isFinite(gain) && gain >= 0 && gain <= 2 ? gain : defaultGain;
}

/** Writes the trim, or removes the entry at unity — absence is the default here too. */
export function writeGain(gain: number): void {
    try {
        if (gain === defaultGain) {
            localStorage.removeItem(GAIN_KEY);
            return;
        }

        localStorage.setItem(GAIN_KEY, gain.toFixed(2));
    } catch {
        // The trim still holds for this page view; it just will not be remembered.
    }
}

/** Power-on state: SC-8820, with all three effects running as the module has them. */
export const defaultSettings: EngineSettings = { map: 4, reverb: true, chorus: true, delay: true };

export function isDefault(settings: EngineSettings): boolean {
    return settings.map === defaultSettings.map
        && settings.reverb === defaultSettings.reverb
        && settings.chorus === defaultSettings.chorus
        && settings.delay === defaultSettings.delay;
}

export function read(): EngineSettings {
    let stored: string | null;

    try {
        stored = localStorage.getItem(KEY);
    } catch {
        return { ...defaultSettings };
    }

    return parse(stored) ?? { ...defaultSettings };
}

/** Writes the settings, or removes the entry when they are the defaults. */
export function write(settings: EngineSettings): void {
    try {
        if (isDefault(settings)) {
            localStorage.removeItem(KEY);
            return;
        }

        localStorage.setItem(KEY, format(settings));
    } catch {
        // The choice still holds for this page view; it just will not be remembered.
    }
}

/** Formats settings as `map,reverb,chorus,delay` — for instance `3,1,1,0`. */
function format(settings: EngineSettings): string {
    const bit = (on: boolean) => (on ? '1' : '0');
    return `${settings.map},${bit(settings.reverb)},${bit(settings.chorus)},${bit(settings.delay)}`;
}

/**
 * Reads back what format wrote. Strict, and whole: a value that does not parse in every field is
 * discarded entirely rather than honoured in part — half of a stale format is worse than the
 * defaults, because it is a setting the user never chose.
 */
function parse(stored: string | null): EngineSettings | null {
    if (stored === null) {
        return null;
    }

    const fields = stored.split(',');
    if (fields.length !== 4) {
        return null;
    }

    const map = Number(fields[0]);
    if (!Number.isInteger(map) || map < 1 || map > 4) {
        return null;
    }

    const flag = (field: string) => (field === '1' ? true : field === '0' ? false : null);
    const reverb = flag(fields[1]!);
    const chorus = flag(fields[2]!);
    const delay = flag(fields[3]!);

    if (reverb === null || chorus === null || delay === null) {
        return null;
    }

    return { map, reverb, chorus, delay };
}
