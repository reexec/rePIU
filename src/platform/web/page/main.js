// Issue #72: the page's side. It picks the ROM files, hands them and the
// canvas to the Worker that runs the engine, and shows what the engine prints.
// Design decision 9: the ROMs come as one folder or as the ZIP and the CHD
// separately (rom_selection.js holds the rules); whichever was picked last is
// what starts.
"use strict";

const folderInput = document.getElementById("folder");
const zipInput = document.getElementById("zip");
const chdInput = document.getElementById("chd");
const gameSelect = document.getElementById("game");
const gameLabel = document.getElementById("game-label");
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

start.addEventListener("click", () => {
  const chosen = selected();
  if (chosen === null) {
    return;
  }
  for (const control of [start, folderInput, zipInput, chdInput, gameSelect]) {
    control.disabled = true;
  }
  showStatus(`Starting ${chosen.game}...`, false);
  const offscreen = canvas.transferControlToOffscreen();
  const worker = new Worker("worker.js");
  worker.onmessage = (event) => {
    const message = event.data;
    if (message.type === "log") {
      appendLog(message.text);
    } else if (message.type === "status") {
      showStatus(message.text, false);
    }
  };
  worker.onerror = (event) => {
    showStatus(`Worker error: ${event.message}`, true);
  };
  worker.postMessage(
    { type: "start", canvas: offscreen, game: chosen.game, blobs: RomSelection.mountBlobs(chosen) },
    [offscreen]);
});
