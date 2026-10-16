// Issue #81: plays the sound the engine sends from its Worker (design
// 20261017-i081, decision 4). The page hands this processor one end of a
// MessageChannel whose other end the Worker holds; each message is a
// Float32Array of interleaved stereo at this context's sample rate. They go
// into a ring buffer, and process() takes 128 frames at a time.
"use strict";

// Seconds of sound the ring holds.
const kCapacitySeconds = 1;
// Over this much queued (the Worker's clock against the audio device's), the
// oldest is dropped down to kTrimToSeconds.
const kTrimAboveSeconds = 0.3;
const kTrimToSeconds = 0.1;

class RepiuAudioProcessor extends AudioWorkletProcessor {
  constructor() {
    super();
    this.capacity = Math.ceil(sampleRate * kCapacitySeconds);
    this.left = new Float32Array(this.capacity);
    this.right = new Float32Array(this.capacity);
    this.readIndex = 0;
    this.count = 0;
    this.started = false;
    this.underruns = 0;
    this.trimmed = 0;
    this.received = 0;
    this.peak = 0;
    this.lastReport = currentTime;
    this.port.onmessage = (event) => {
      if (event.data && event.data.type === "engine") {
        event.data.port.onmessage = (message) => this.push(message.data);
      }
    };
  }

  push(samples) {
    const frames = samples.length >> 1;
    this.started = true;
    this.received += frames;
    let writeIndex = (this.readIndex + this.count) % this.capacity;
    for (let frame = 0; frame < frames; ++frame) {
      const l = samples[frame * 2];
      const r = samples[frame * 2 + 1];
      this.left[writeIndex] = l;
      this.right[writeIndex] = r;
      const magnitude = Math.max(Math.abs(l), Math.abs(r));
      if (magnitude > this.peak) {
        this.peak = magnitude;
      }
      writeIndex = (writeIndex + 1) % this.capacity;
      if (this.count < this.capacity) {
        this.count += 1;
      } else {
        // Full: the oldest frame goes.
        this.readIndex = (this.readIndex + 1) % this.capacity;
      }
    }
    if (this.count > sampleRate * kTrimAboveSeconds) {
      const drop = this.count - Math.floor(sampleRate * kTrimToSeconds);
      this.readIndex = (this.readIndex + drop) % this.capacity;
      this.count -= drop;
      this.trimmed += drop;
    }
  }

  process(inputs, outputs) {
    const output = outputs[0];
    const left = output[0];
    const right = output.length > 1 ? output[1] : output[0];
    let short = false;
    for (let index = 0; index < left.length; ++index) {
      if (this.count > 0) {
        left[index] = this.left[this.readIndex];
        right[index] = this.right[this.readIndex];
        this.readIndex = (this.readIndex + 1) % this.capacity;
        this.count -= 1;
      } else {
        left[index] = 0;
        right[index] = 0;
        short = true;
      }
    }
    if (short && this.started) {
      this.underruns += 1;
    }
    if (currentTime - this.lastReport >= 1) {
      this.lastReport = currentTime;
      this.port.postMessage({
        type: "stats",
        bufferedMs: Math.round((this.count / sampleRate) * 1000),
        underruns: this.underruns,
        trimmedMs: Math.round((this.trimmed / sampleRate) * 1000),
        receivedSeconds: this.received / sampleRate,
        peak: this.peak,
      });
      this.peak = 0;
    }
    return true;
  }
}

registerProcessor("repiu-audio", RepiuAudioProcessor);
