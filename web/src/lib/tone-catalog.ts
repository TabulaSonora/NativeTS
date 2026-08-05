// Naming helpers over the catalog data — the layout knowledge the ROM does not carry.

/** The four vintages, in the order the engine numbers them. */
export const vintages = [1, 2, 3, 4] as const;

/**
 * XG's selector, which is the module's own number for it rather than a fifth vintage slot.
 *
 * The engine takes it in the same field the vintages go in, and starting there is the whole point:
 * a file that never sends XG System On is played as XG anyway, which is what most XG files written
 * for a hardware module in XG mode need.
 */
export const xgMap = 0x77;

/**
 * Every tone map the engine's map field accepts. Not the same list as `vintages`: XG is a different
 * instrument's sound set living in the same ROM, not an older Sound Canvas, so the instrument
 * browser stays on the four while the engine selector offers all five.
 */
export const engineMaps = [...vintages, xgMap] as const;

/**
 * The module's name for a tone map. The plugin's own tone files name these 55Map, 88Map, 88ProMap
 * and 8820Map; the modules are what a reader recognises.
 */
export function vintageName(map: number): string {
    switch (map) {
        case 1: return 'SC-55';
        case 2: return 'SC-88';
        case 3: return 'SC-88Pro';
        case 4: return 'SC-8820';
        case xgMap: return 'XG';
        default: return `map ${map}`;
    }
}

const noteNames = ['C', 'C#', 'D', 'D#', 'E', 'F', 'F#', 'G', 'G#', 'A', 'A#', 'B'];

/** Names a MIDI note the way the on-screen keyboard labels its keys — middle C is C4. */
export function noteName(note: number): string {
    return `${noteNames[note % 12]}${Math.floor(note / 12) - 1}`;
}

/**
 * What a bank is, where it is something other than a variation of the capital tone. The top two
 * banks are the module's CM-64 compatibility map: 127 is the LA half — the MT-32 / CM-32L side —
 * and 126 is the PCM half, the CM-32P side.
 */
export function bankName(bank: number): string | null {
    switch (bank) {
        case 126: return 'CM-64 PCM';
        case 127: return 'CM-64 LA · MT-32';
        default: return null;
    }
}

/** The banks that are a whole other module's sound set rather than a variation. */
export const emulationBanks = [126, 127];

/** How many programs share a General MIDI family. */
export const familySize = 8;

const families = [
    'Piano', 'Chromatic percussion', 'Organ', 'Guitar',
    'Bass', 'Strings', 'Ensemble', 'Brass',
    'Reed', 'Pipe', 'Synth lead', 'Synth pad',
    'Synth effects', 'Ethnic', 'Percussive', 'Sound effects',
];

/**
 * The General MIDI family a program number falls in. GM lays its 128 programs out as sixteen
 * families of eight, and that layout is the spec's rather than this ROM's — it is a grouping of
 * program numbers, so it holds whatever the vintage names those programs.
 */
export function familyOf(program: number): string {
    return families[Math.floor(program / 8)]!;
}
