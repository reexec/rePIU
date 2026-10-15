// Issue #72: the page's side. It picks the ROM files, hands them and the
// canvas to the Worker that runs the engine, and shows what the engine prints.
"use strict";

const files = document.getElementById("files");
const start = document.getElementById("start");
const status = document.getElementById("status");
const log = document.getElementById("log");
const canvas = document.getElementById("screen");

const kLogLines = 400;
const kHistoryLines = 50000;
const lines = [];
// Every line the engine printed, for the console and for diagnosis; the log
// box shows only the last few hundred.
window.repiuLogHistory = [];
let flushPending = false;

function appendLog(text) {
  if (window.repiuLogHistory.length < kHistoryLines) {
    window.repiuLogHistory.push(text);
  }
  lines.push(text);
  if (lines.length > kLogLines) {
    lines.splice(0, lines.length - kLogLines);
  }
  if (!flushPending) {
    flushPending = true;
    requestAnimationFrame(() => {
      flushPending = false;
      log.textContent = lines.join("\n");
      log.scrollTop = log.scrollHeight;
    });
  }
}

// The game is named by its ZIP, as MAME names it (pumpitea.zip -> pumpitea).
function chosenGame() {
  const picked = Array.from(files.files);
  const zip = picked.find((file) => file.name.toLowerCase().endsWith(".zip"));
  const chd = picked.find((file) => file.name.toLowerCase().endsWith(".chd"));
  if (!zip || !chd) {
    return null;
  }
  return { game: zip.name.replace(/\.zip$/i, ""), zip, chd };
}

files.addEventListener("change", () => {
  const chosen = chosenGame();
  start.disabled = chosen === null;
  status.textContent = chosen
    ? `${chosen.game}: ${chosen.zip.name}, ${chosen.chd.name} ` +
      `(${(chosen.chd.size / 1048576).toFixed(0)} MB)`
    : "Pick both the ZIP and the CHD.";
});

start.addEventListener("click", () => {
  const chosen = chosenGame();
  if (chosen === null) {
    return;
  }
  start.disabled = true;
  files.disabled = true;
  status.textContent = `Starting ${chosen.game}...`;
  const offscreen = canvas.transferControlToOffscreen();
  const worker = new Worker("worker.js");
  worker.onmessage = (event) => {
    const message = event.data;
    if (message.type === "log") {
      appendLog(message.text);
    } else if (message.type === "status") {
      status.textContent = message.text;
    }
  };
  worker.onerror = (event) => {
    status.textContent = `Worker error: ${event.message}`;
  };
  worker.postMessage(
    { type: "start", canvas: offscreen, game: chosen.game, zip: chosen.zip, chd: chosen.chd },
    [offscreen]);
});
