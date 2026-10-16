// Issue #72: the Worker that runs the engine (design 20261016-i072). The ROM
// files the user picked are mounted read-only with WORKERFS, which reads only
// the ranges asked for (FileReaderSync), so the CHD is never loaded whole; what
// the loader unpacks from it goes to memory (MEMFS). The page's canvas is
// handed to the engine through Module.repiuCanvas, and Module.repiuYield lets
// the engine hand the thread back to this Worker's event loop once a frame.
"use strict";

importScripts("repiu_browser.js");

// Issue #76: the page's keys and gamepads, oldest first, until the engine
// takes them (Module.repiuInput, read by src/platform/web/browser_host.cpp).
// Messages arrive while the engine yields, once a frame. Bounded, so input
// sent while the engine is not taking any (the CHD unpack) cannot pile up.
const kInputQueueLimit = 1024;
const inputQueue = [];

function post(type, text) {
  self.postMessage({ type, text });
}

// An error after the first yield surfaces here rather than at callMain: the
// engine resumes from this Worker's event loop.
self.addEventListener("error", (event) => {
  const stack = event.error && event.error.stack ? event.error.stack : event.message;
  post("log", `[worker] ${stack}`);
});

self.onmessage = async (event) => {
  const message = event.data;
  if (message.type === "input") {
    inputQueue.push(...message.events);
    if (inputQueue.length > kInputQueueLimit) {
      inputQueue.splice(0, inputQueue.length - kInputQueueLimit);
    }
    return;
  }
  if (message.type !== "start") {
    return;
  }
  // Design decision 9: the page names the mount (rom_selection.js mountBlobs):
  // the ZIPs at the top, the game's first, and the CHD in `<game>/`.
  const { canvas, game, blobs, audioPort, audioSampleRate } = message;
  const started = performance.now();
  const module = await createRepiu({
    repiuCanvas: canvas,
    repiuYield: true,
    repiuInput: inputQueue,
    // Issue #81: the page's AudioWorklet, and its context's sample rate (0:
    // no sound).
    repiuAudioPort: audioPort,
    repiuAudioSampleRate: audioSampleRate,
    print: (text) => post("log", text),
    printErr: (text) => post("log", text),
    onAbort: (reason) => post("status", `Aborted: ${reason}`),
  });
  const FS = module.FS;
  FS.mkdir("/roms");
  FS.mount(FS.filesystems.WORKERFS, { blobs }, "/roms");
  post("status", `${game} running (loaded in ${((performance.now() - started) / 1000).toFixed(1)} s)`);
  try {
    module.callMain([game]);
  } catch (error) {
    post("status", `Stopped: ${error}`);
  }
};
