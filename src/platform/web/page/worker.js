// Issue #72: the Worker that runs the engine (design 20261016-i072). The ROM
// files the user picked are mounted read-only with WORKERFS, which reads only
// the ranges asked for (FileReaderSync), so the CHD is never loaded whole; what
// the loader unpacks from it goes to memory (MEMFS). The page's canvas is
// handed to the engine through Module.repiuCanvas, and Module.repiuYield lets
// the engine hand the thread back to this Worker's event loop once a frame.
"use strict";

importScripts("repiu_browser.js", "mount_cache.js");

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
  const { canvas, game, blobs, audioPort, audioSampleRate, cacheKey, env } = message;
  const started = performance.now();
  const module = await createRepiu({
    repiuCanvas: canvas,
    repiuYield: true,
    repiuInput: inputQueue,
    // Issue #81: the page's AudioWorklet, and its context's sample rate (0:
    // no sound).
    repiuAudioPort: audioPort,
    repiuAudioSampleRate: audioSampleRate,
    // Issue #87: the engine's REPIU_* variables the page's address named
    // (diagnosis; node_pre.js puts them in ENV).
    repiuEnv: env || {},
    print: (text) => post("log", text),
    printErr: (text) => post("log", text),
    onAbort: (reason) => post("status", `Aborted: ${reason}`),
  });
  const FS = module.FS;
  FS.mkdir("/roms");
  FS.mount(FS.filesystems.WORKERFS, { blobs }, "/roms");
  // Issue #86: the files the loader unpacks from the CHD. When the browser's
  // storage holds them from an earlier run of the same CHD, they are mounted
  // read-only where the loader looks, and it skips the unpack (the game never
  // writes there).
  const mountPath = `/build/runtime_mounts/${game}`;
  let cached = null;
  try {
    cached = cacheKey ? await MountCache.blobsFor(game, cacheKey) : null;
  } catch (error) {
    post("cache", `Cache unreadable: ${error}`);
  }
  if (cached !== null) {
    FS.mkdirTree(mountPath);
    FS.mount(FS.filesystems.WORKERFS, { blobs: cached }, mountPath);
    post("cache", `Using the unpacked files saved earlier (${cached.length} files)`);
  }
  post("status", `${game} running (loaded in ${((performance.now() - started) / 1000).toFixed(1)} s)`);
  try {
    module.callMain([game]);
  } catch (error) {
    post("status", `Stopped: ${error}`);
  }
  // callMain returns at the engine's first yield, after the unpack. The copy
  // runs between the engine's turns.
  if (cached === null && cacheKey && MountCache.supported()) {
    let lastPercent = -1;
    post("cache", "Saving the unpacked files for later runs...");
    MountCache.store(game, cacheKey, FS, mountPath, (done, total) => {
      const percent = total > 0 ? Math.floor((done * 100) / total) : 100;
      if (percent !== lastPercent) {
        lastPercent = percent;
        post("cache", `Saving the unpacked files for later runs: ${percent}%`);
      }
    }).then((manifest) => {
      post("cache", `Saved ${manifest.files} unpacked files (${Math.round(manifest.bytes / 1048576)} MB); ` +
        "later runs skip the unpack");
    }, (error) => {
      post("cache", `Unpacked files not saved: ${error.message || error}`);
    });
  }
};
