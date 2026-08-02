// Web MIDI input. Messages go straight through to the engine's own send_channel, which is what the
// hardware's MIDI in does — there is no sequencer in between and nothing is quantised.

export interface MidiDevice {
    id: string;
    name: string;
    connected: boolean;
}

let access: MIDIAccess | null = null;
let selected: string | null = null;

export const isSupported = typeof navigator !== 'undefined' && !!navigator.requestMIDIAccess;

export function isOpen(): boolean {
    return access !== null;
}

export async function open(onChange: () => void): Promise<MidiDevice[] | null> {
    if (!navigator.requestMIDIAccess) {
        return null;
    }

    access = await navigator.requestMIDIAccess({ sysex: false });
    access.onstatechange = () => onChange();
    return devices();
}

export function devices(): MidiDevice[] {
    if (!access) {
        return [];
    }

    return Array.from(access.inputs.values()).map(input => ({
        id: input.id,
        name: input.name ?? input.id,
        connected: input.id === selected,
    }));
}

export function listen(id: string | null,
                       onMessage: (status: number, data1: number, data2: number) => void): boolean {
    if (!access) {
        return false;
    }

    for (const input of access.inputs.values()) {
        input.onmidimessage = null;
    }

    selected = null;
    if (!id) {
        return true;
    }

    const input = access.inputs.get(id);
    if (!input) {
        return false;
    }

    input.onmidimessage = event => {
        const data = (event as MIDIMessageEvent).data;

        // Real-time messages (0xF8 and up) arrive constantly from a clock-sending device and the
        // engine has nothing to do with them.
        if (!data || data.length < 2 || data[0]! >= 0xf8) {
            return;
        }

        onMessage(data[0]!, data[1]!, data.length > 2 ? data[2]! : 0);
    };

    selected = id;
    return true;
}
