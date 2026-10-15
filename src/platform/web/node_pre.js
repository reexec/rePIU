// Issue #67. Linked into the web loader with --pre-js. Under Node there is no
// window and no audio device, and SDL3's Emscripten video driver assumes a
// browser (it reads `window` while initialising), so Node runs SDL's dummy
// drivers unless the environment already names others. The Glide backend then
// falls back to its dummy and the audio outputs go silent, which is the
// headless run of design #67, decision 5. In a browser this does nothing.
if (typeof process === "object" && process.versions && process.versions.node) {
  Module.preRun = Module.preRun || [];
  Module.preRun.push(function () {
    if (!process.env.SDL_VIDEO_DRIVER) {
      ENV.SDL_VIDEO_DRIVER = "dummy";
    }
    if (!process.env.SDL_AUDIO_DRIVER) {
      ENV.SDL_AUDIO_DRIVER = "dummy";
    }
  });
}
