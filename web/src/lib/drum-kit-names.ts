// What each drum kit is called.
//
// The DLL has no kit names. It names every drum *sound* — those come out of the melodic tone table,
// because drum sounds are melodic tones — but nothing in it says that program 9 selects "ROOM".
// Those names live in the plugin's companion SCVSC.drf, and this is the kit-name half of that file,
// transcribed. See NOTICE.md: it is the one piece of Roland-derived data this web app carries, and
// it lives here in the app rather than in the engine library so a host that links the engine does
// not acquire it by accident.
//
// Only two of the file's five pages are here, because only two are reachable by name. The drum
// program map's row 0 defines the programs on the file's SC-8820 page and row 1 those on its
// SC-88Pro page. Rows 2 and 3 carry the SC-88 and SC-55 maps and no page names their kits, so those
// fall back to being labelled by the programs that select them.
//
// One kit is deliberately unnamed: program 127 — the CM-64/32L kit, the MT-32's drum set — is
// defined by the ROM on both rows and appears on neither of these pages, so it keeps its number.

import { vintageName, xgMap } from './tone-catalog';

const pages: Record<number, Record<number, string>> = {
    4: {
        0: 'STANDARD 1',
        1: 'STANDARD 2',
        2: 'STANDARD L/R',
        8: 'ROOM',
        9: 'HIP HOP',
        10: 'JUNGLE',
        11: 'TECHNO',
        12: 'ROOM L/R',
        13: 'HOUSE',
        16: 'POWER',
        24: 'ELECTRONIC',
        25: 'TR-808',
        26: 'DANCE',
        27: 'CR-78',
        28: 'TR-606',
        29: 'TR-707',
        30: 'TR-909',
        32: 'JAZZ',
        33: 'JAZZ L/R',
        40: 'BRUSH',
        41: 'BRUSH 2',
        42: 'BRUSH 2 L/R',
        48: 'ORCHESTRA',
        49: 'ETHNIC',
        50: 'KICK & SNARE',
        51: 'KICK & SNARE2',
        52: 'ASIA',
        53: 'CYMBAL&CLAPS',
        54: 'GAMELAN 1',
        55: 'GAMELAN 2',
        56: 'SFX',
        57: 'RHYTHM FX',
        58: 'RHYTHM FX 2',
        59: 'RHYTHM FX 3',
        60: 'SFX 2',
        61: 'VOICE',
        62: 'CYM&CLAPS 2',
    },
    3: {
        0: 'STANDARD 1',
        1: 'STANDARD 2',
        2: 'STANDARD 3',
        8: 'ROOM',
        9: 'HIP HOP',
        10: 'JUNGLE',
        11: 'TECHNO',
        16: 'POWER',
        24: 'ELECTRONIC',
        25: 'TR-808',
        26: 'DANCE',
        27: 'CR-78',
        28: 'TR-606',
        29: 'TR-707',
        30: 'TR-909',
        32: 'JAZZ',
        40: 'BRUSH',
        48: 'ORCHESTRA',
        49: 'ETHNIC',
        50: 'KICK & SNARE',
        52: 'ASIA',
        53: 'CYMBAL&CLAPS',
        56: 'SFX',
        57: 'RHYTHM FX',
        58: 'RHYTHM FX 2',
    },
};

/**
 * The tone map whose kit list each drum map row holds. Measured rather than assumed: the set of
 * programs each row defines matches one page's set of kits exactly, in both cases plus program
 * 127, which the ROM carries and no page offers.
 */
export const rowMaps = [4, 3, 2, 1, xgMap];

/** Names the kit a program selects, or null where nothing names it. */
export function forRow(row: number, program: number): string | null {
    return pages[rowMaps[row] ?? -1]?.[program] ?? null;
}

/** Names the kit set a drum map row holds, as the module whose list it is. */
export function rowName(row: number): string {
    const map = rowMaps[row];
    return map !== undefined ? vintageName(map) : `row ${row}`;
}
