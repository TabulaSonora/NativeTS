// The audio-thread end of the engine.
//
// The worker renders ahead of playback and posts blocks here; this drains them at whatever rate the
// AudioContext runs. Everything upstream of this file is at the engine's own 32 kHz, and when the
// context agrees to run at 32 kHz too, nothing resamples on the way to the device — the same
// property the desktop player gets by opening the device at 32 kHz. When the browser refuses that
// rate, the linear interpolation below is the whole of the host-rate conversion, and it is the one
// place in the signal path that is not the engine's own arithmetic.
//
// Ported from the reference web app's synth-processor.js, with one addition: a 'bind' command on
// the node port hands over a MessagePort, and from then on blocks, transport commands and queue
// reports all travel on that port — which is how the worker talks to this thread directly, with
// the main thread out of the loop.

const RING_SECONDS = 4;

class SynthProcessor extends AudioWorkletProcessor {
    constructor(options) {
        super();

        this.sourceRate = options.processorOptions.sourceRate;
        this.ratio = this.sourceRate / sampleRate;
        this.resampling = Math.abs(this.ratio - 1) > 1e-9;

        this.capacity = Math.ceil(this.sourceRate * RING_SECONDS);
        this.left = new Float32Array(this.capacity);
        this.right = new Float32Array(this.capacity);
        this.write = 0;
        this.read = 0;
        this.available = 0;

        // Fractional read position between two source frames, only used when resampling.
        this.phase = 0;
        this.playing = false;
        this.starved = 0;
        this.reported = -1;

        // Until a 'bind' arrives, commands and reports use the node's own port.
        this.data = this.port;
        this.port.onmessage = event => this.receive(event.data);
    }

    receive(message) {
        if (message.command === 'bind') {
            this.data = message.port;
            this.data.onmessage = event => this.receive(event.data);
            return;
        }

        if (message.command === 'push') {
            // One buffer, left channel then right, as it came off the engine's heap.
            this.push(message.samples, message.frames);
            return;
        }

        if (message.command === 'play') {
            this.playing = true;
            return;
        }

        if (message.command === 'pause') {
            this.playing = false;
            return;
        }

        if (message.command === 'flush') {
            this.write = this.read = this.available = 0;
            this.phase = 0;
            this.playing = false;
            return;
        }

        // Starvation is counted since the context opened, which is what you want for "has this ever
        // dropped out" and exactly what you do not want while tuning the lead down: the count from
        // the setting before last would sit there looking like the current one.
        if (message.command === 'resetStarved') {
            this.starved = 0;
        }
    }

    push(samples, frames) {
        const count = Math.min(frames, this.capacity - this.available);
        for (let i = 0; i < count; i++) {
            this.left[this.write] = samples[i];
            this.right[this.write] = samples[frames + i];
            this.write = (this.write + 1) % this.capacity;
        }

        this.available += count;
    }

    // Drops one source frame off the front, returning it.
    take(out) {
        if (this.available === 0) {
            out[0] = 0;
            out[1] = 0;
            return false;
        }

        out[0] = this.left[this.read];
        out[1] = this.right[this.read];
        this.read = (this.read + 1) % this.capacity;
        this.available--;
        return true;
    }

    peek(offset, out) {
        const index = (this.read + offset) % this.capacity;
        out[0] = this.left[index];
        out[1] = this.right[index];
    }

    process(_inputs, outputs) {
        const output = outputs[0];
        const left = output[0];
        const right = output[1] ?? output[0];
        const frames = left.length;

        if (!this.playing) {
            left.fill(0);
            right.fill(0);
            return true;
        }

        const a = [0, 0];
        const b = [0, 0];

        if (!this.resampling) {
            for (let i = 0; i < frames; i++) {
                if (!this.take(a)) {
                    this.starved++;
                }

                left[i] = a[0];
                right[i] = a[1];
            }
        } else {
            for (let i = 0; i < frames; i++) {
                // Two source frames must be present before either can be interpolated between.
                if (this.available < 2) {
                    this.starved++;
                    left[i] = 0;
                    right[i] = 0;
                    continue;
                }

                this.peek(0, a);
                this.peek(1, b);

                const t = this.phase;
                left[i] = a[0] + ((b[0] - a[0]) * t);
                right[i] = a[1] + ((b[1] - a[1]) * t);

                this.phase += this.ratio;
                while (this.phase >= 1 && this.available > 0) {
                    this.phase -= 1;
                    this.take(a);
                }
            }
        }

        this.report();
        return true;
    }

    // The pump on the other side steers by this. Reporting every render quantum would be 375
    // messages a second; every 10 ms is frequent enough to hold the shortest lead the pump offers,
    // which is the tightest thing it has to keep from starving.
    report() {
        const now = currentFrame;
        if (this.reported >= 0 && now - this.reported < sampleRate / 100) {
            return;
        }

        this.reported = now;
        this.data.postMessage({ queued: this.available, starved: this.starved });
    }
}

registerProcessor('synth-processor', SynthProcessor);
