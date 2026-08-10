// AudioWorklet processor for the 1984 WASM POC.
//
// The main thread drains the emulator's audio ring into interleaved
// Float32Array chunks and posts them here; this processor buffers them and
// plays them on the audio thread, so main-thread jitter (rAF, canvas draws)
// cannot starve or glitch the output.
class POCAudioProcessor extends AudioWorkletProcessor {
  constructor() {
    super();
    this.chunks = [];
    this.port.onmessage = (e) => {
      if (e.data === 'clear') this.chunks.length = 0;
      else this.chunks.push(e.data);
    };
  }
  process(inputs, outputs) {
    const outL = outputs[0][0];
    const outR = outputs[0][1];
    let written = 0;
    while (written < outL.length && this.chunks.length > 0) {
      const c = this.chunks[0];
      const take = Math.min(c.length, (outL.length - written) * 2);
      let src = 0;
      for (let i = 0; i < take; i += 2) {
        outL[written + (i >> 1)] = c[src];
        outR[written + (i >> 1)] = c[src + 1];
        src += 2;
      }
      written += take >> 1;
      if (take === c.length) this.chunks.shift();
      else this.chunks[0] = c.subarray(take);
    }
    for (let i = written; i < outL.length; i++) { outL[i] = 0; outR[i] = 0; }
    return true;
  }
}
registerProcessor('poc-audio', POCAudioProcessor);
