// Issue #67. Linked into the web loaders with --pre-js. SDL3's Emscripten video
// driver assumes a page's main thread (it reads `window` while initialising),
// and its audio needs an AudioContext. Neither exists under Node (the headless
// run of design #67, decision 5) or in a Worker (issue #72, where the page's
// canvas is handed over through Module.repiuCanvas instead), so wherever there
// is no `window`, SDL's dummy drivers are the default unless the environment
// already names others. The Glide backend then draws to the page's canvas, or
// falls back to its dummy under Node; the audio outputs go silent.
if (typeof window === "undefined") {
  Module.preRun = Module.preRun || [];
  Module.preRun.push(function () {
    const named = (name) =>
      typeof process === "object" && process.env && process.env[name];
    if (!named("SDL_VIDEO_DRIVER")) {
      ENV.SDL_VIDEO_DRIVER = "dummy";
    }
    // Issue #72: no SDL audio at all. SDL's Emscripten dummy audio drives
    // itself with setInterval, whose callback failed in a Worker with a
    // function signature mismatch; with no driver the audio outputs report
    // that they could not open and stay silent, which is what the dummy gave.
    // The page's sound (an AudioWorklet) is unit 2.
    if (!named("SDL_AUDIO_DRIVER")) {
      ENV.SDL_AUDIO_DRIVER = "none";
    }
  });
}
