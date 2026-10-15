// Issue #72: the Worker that runs the engine (design 20261016-i072). The ROM
// files the user picked are mounted read-only with WORKERFS, which reads only
// the ranges asked for (FileReaderSync), so the CHD is never loaded whole; what
// the loader unpacks from it goes to memory (MEMFS). The page's canvas is
// handed to the engine through Module.repiuCanvas, and Module.repiuYield lets
// the engine hand the thread back to this Worker's event loop once a frame.
"use strict";

importScripts("repiu_browser.js");

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
  if (message.type !== "start") {
    return;
  }
  const { canvas, game, zip, chd } = message;
  const started = performance.now();
  const module = await createRepiu({
    repiuCanvas: canvas,
    repiuYield: true,
    print: (text) => post("log", text),
    printErr: (text) => post("log", text),
    onAbort: (reason) => post("status", `Aborted: ${reason}`),
  });
  const FS = module.FS;
  FS.mkdir("/roms");
  FS.mount(FS.filesystems.WORKERFS, {
    blobs: [
      { name: `${game}.zip`, data: zip },
      { name: `${game}/${chd.name}`, data: chd },
    ],
  }, "/roms");
  post("status", `${game} running (loaded in ${((performance.now() - started) / 1000).toFixed(1)} s)`);
  try {
    module.callMain([game]);
  } catch (error) {
    post("status", `Stopped: ${error}`);
  }
};
