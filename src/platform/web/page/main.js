// Issue #72: the page's side. It picks the ROM files, hands them and the
// canvas to the Worker that runs the engine, and shows what the engine prints.
// Design decision 9: the ROMs come as one folder or as the ZIP and the CHD
// separately (rom_selection.js holds the rules); whichever was picked last is
// what starts. Design decision 10: while it runs, the elapsed time, the line
// count and the latest line are kept current, since the loader unpacks the
// CHD for a minute or more before the game draws anything. Issue #76: once
// started, the page's keys and gamepads go to the Worker (page_input.js).
"use strict";

const folderInput = document.getElementById("folder");
const zipInput = document.getElementById("zip");
const chdInput = document.getElementById("chd");
const gameSelect = document.getElementById("game");
const gameLabel = document.getElementById("game-label");
const start = document.getElementById("start");
const status = document.getElementById("status");
const log = document.getElementById("log");
const logCount = document.getElementById("log-count");
const copyButton = document.getElementById("copy");
const activity = document.getElementById("activity");
const canvas = document.getElementById("screen");
const soundLine = document.getElementById("sound");

const kLogLines = 400;
const kHistoryLines = 50000;
const lines = [];
// Every line the engine printed, for the console and for diagnosis; the log
// box shows only the last few hundred.
window.repiuLogHistory = [];
let flushPending = false;
let lineCount = 0;
let lastLine = "";

function appendLog(text) {
  if (window.repiuLogHistory.length < kHistoryLines) {
    window.repiuLogHistory.push(text);
  }
  lineCount += 1;
  lastLine = text;
  copyButton.disabled = false;
  lines.push(text);
  if (lines.length > kLogLines) {
    lines.splice(0, lines.length - kLogLines);
  }
  if (!flushPending) {
    flushPending = true;
    requestAnimationFrame(() => {
      flushPending = false;
      // Follow the output only while the reader is at its end, so scrolling
      // back to read is not undone by the next line.
      const following = log.scrollTop + log.clientHeight >= log.scrollHeight - 24;
      log.textContent = lines.join("\n");
      if (following) {
        log.scrollTop = log.scrollHeight;
      }
      logCount.textContent = `Engine output (${lineCount} line${lineCount === 1 ? "" : "s"})`;
    });
  }
}

copyButton.addEventListener("click", async () => {
  const text = window.repiuLogHistory.join("\n");
  try {
    await navigator.clipboard.writeText(text);
    copyButton.textContent = "Copied";
  } catch {
    // Without clipboard access, select the box so it can be copied by hand.
    const range = document.createRange();
    range.selectNodeContents(log);
    getSelection().removeAllRanges();
    getSelection().addRange(range);
    copyButton.textContent = "Selected";
  }
  setTimeout(() => { copyButton.textContent = "Copy"; }, 1500);
});

// The elapsed time and the latest line, while the engine runs. On the page's
// own thread, so it keeps counting while the Worker is busy.
let clock = null;
function startClock() {
  const began = performance.now();
  const tick = () => {
    const seconds = Math.floor((performance.now() - began) / 1000);
    const elapsed = `${Math.floor(seconds / 60)}:${String(seconds % 60).padStart(2, "0")}`;
    activity.textContent = lineCount === 0
      ? `${elapsed} elapsed, waiting for the engine's first line`
      : `${elapsed} elapsed, ${lineCount} line${lineCount === 1 ? "" : "s"}. Latest: ${lastLine}`;
  };
  tick();
  clock = setInterval(tick, 500);
}
function stopClock() {
  if (clock !== null) {
    clearInterval(clock);
    clock = null;
  }
}

function showStatus(text, problem) {
  status.textContent = text;
  status.classList.toggle("problem", Boolean(problem));
}

function describe(chosen) {
  const megabytes = (chosen.chd.size / 1048576).toFixed(0);
  // The folder's other ZIPs go along for the loader's parent-set fallback.
  const others = chosen.zips.length - 1;
  const extra = others > 0 ? `, plus the folder's ${others} other ZIP${others === 1 ? "" : "s"}` : "";
  return `${chosen.game}: ${chosen.zip.name}, ${chosen.chd.name} (${megabytes} MB)${extra}`;
}

// The games the current pick offers, and which one starts.
let candidates = [];

function showCandidates(found, problem) {
  candidates = found;
  gameSelect.replaceChildren(...found.map((chosen, index) => {
    const option = document.createElement("option");
    option.value = String(index);
    option.textContent = chosen.game;
    return option;
  }));
  const choose = found.length > 1;
  gameSelect.hidden = !choose;
  gameLabel.hidden = !choose;
  start.disabled = found.length === 0;
  if (found.length === 0) {
    showStatus(problem, true);
  } else {
    showStatus(describe(found[0]), false);
  }
}

function selected() {
  return candidates[gameSelect.hidden ? 0 : Number(gameSelect.value)] || null;
}

folderInput.addEventListener("change", () => {
  // The other way is cleared, so what starts is never a mix of the two.
  zipInput.value = "";
  chdInput.value = "";
  const entries = Array.from(folderInput.files, (file) => ({ path: file.webkitRelativePath, file }));
  const { candidates: found, problem } = RomSelection.findGamesInFolder(entries);
  showCandidates(found, problem);
});

function separateChanged() {
  folderInput.value = "";
  const chosen = RomSelection.pairSeparateFiles(zipInput.files[0], chdInput.files[0]);
  if (chosen) {
    showCandidates([chosen], null);
  } else {
    showCandidates([], zipInput.files[0] ? "Now pick the CHD." : "Now pick the ZIP.");
  }
}
zipInput.addEventListener("change", separateChanged);
chdInput.addEventListener("change", separateChanged);

gameSelect.addEventListener("change", () => {
  const chosen = selected();
  if (chosen) {
    showStatus(describe(chosen), false);
  }
});

// Issue #81: the sound. The AudioContext is made inside the Start click, as a
// browser plays sound only after a user gesture; the worklet gets one end of a
// MessageChannel and the Worker the other, so the sound does not pass through
// this thread. Null when the browser gives no sound; the game runs anyway.
async function startSound() {
  let context;
  try {
    context = new AudioContext({ latencyHint: "interactive" });
  } catch (error) {
    soundLine.textContent = `Sound unavailable: ${error}`;
    return null;
  }
  try {
    await context.audioWorklet.addModule("audio_worklet.js");
    const node = new AudioWorkletNode(context, "repiu-audio",
      { numberOfInputs: 0, outputChannelCount: [2] });
    node.connect(context.destination);
    const channel = new MessageChannel();
    node.port.postMessage({ type: "engine", port: channel.port1 }, [channel.port1]);
    node.port.onmessage = (event) => {
      const stats = event.data;
      if (stats.type === "stats") {
        soundLine.textContent =
          `Sound: ${stats.bufferedMs} ms buffered, peak ${stats.peak.toFixed(2)}, ` +
          `${stats.underruns} underrun${stats.underruns === 1 ? "" : "s"}, ` +
          `${stats.receivedSeconds.toFixed(1)} s received`;
      }
    };
    // Never waited on: when the browser holds sound back (no gesture it
    // accepts) resume() does not settle, and the game must start anyway. The
    // next click or key on the page tries again.
    if (context.state !== "running") {
      context.resume();
      const retry = () => {
        context.resume();
        if (context.state === "running") {
          window.removeEventListener("pointerdown", retry);
          window.removeEventListener("keydown", retry);
        }
      };
      window.addEventListener("pointerdown", retry);
      window.addEventListener("keydown", retry);
    }
    soundLine.textContent = `Sound: ${context.sampleRate} Hz, waiting for the engine`;
    return { port: channel.port2, sampleRate: context.sampleRate };
  } catch (error) {
    soundLine.textContent = `Sound unavailable: ${error}`;
    context.close();
    return null;
  }
}

start.addEventListener("click", async () => {
  const chosen = selected();
  if (chosen === null) {
    return;
  }
  // Before anything awaits, so the AudioContext is made inside the gesture.
  const soundStarting = startSound();
  for (const control of [start, folderInput, zipInput, chdInput, gameSelect]) {
    control.disabled = true;
  }
  showStatus(`Starting ${chosen.game}...`, false);
  startClock();
  // The drawing size, read before the canvas is handed over.
  const surface = { element: canvas, width: canvas.width, height: canvas.height };
  const offscreen = canvas.transferControlToOffscreen();
  const worker = new Worker("worker.js");
  worker.onmessage = (event) => {
    const message = event.data;
    if (message.type === "log") {
      appendLog(message.text);
    } else if (message.type === "status") {
      const finished = /^(Stopped|Aborted)/.test(message.text);
      if (finished) {
        ended();
      }
      showStatus(message.text, finished);
    }
  };
  let stopInput = null;
  const ended = () => {
    stopClock();
    if (stopInput !== null) {
      stopInput();
      stopInput = null;
    }
  };
  worker.onerror = (event) => {
    ended();
    showStatus(`Worker error: ${event.message}`, true);
  };
  const sound = await soundStarting;
  const transfer = [offscreen];
  if (sound !== null) {
    transfer.push(sound.port);
  }
  worker.postMessage(
    {
      type: "start",
      canvas: offscreen,
      game: chosen.game,
      blobs: RomSelection.mountBlobs(chosen),
      audioPort: sound !== null ? sound.port : null,
      audioSampleRate: sound !== null ? sound.sampleRate : 0,
    },
    transfer);
  // The focus leaves the Start button, which a key would otherwise press.
  start.blur();
  stopInput = PageInput.forwardInput(worker, surface);
});
