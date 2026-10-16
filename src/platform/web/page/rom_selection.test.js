// Issue #72, design decision 9: the page's ROM-picking rules, checked under
// Node (`node src/platform/web/page/rom_selection.test.js`). Files are stand-ins
// with a name, which is all the rules read.
"use strict";

const assert = require("node:assert/strict");
const {
  findGamesInFolder, pairSeparateFiles, mountBlobs, gameOf,
} = require("./rom_selection.js");

function file(name) {
  return { name };
}

// A folder as a directory picker reports it: each path starts with the folder.
function folder(paths) {
  return paths.map((path) => ({ path, file: file(path.split("/").pop()) }));
}

const checks = [];
function check(name, body) {
  checks.push({ name, body });
}

check("the ROM-folder layout pairs <game>.zip with <game>/*.chd", () => {
  const { candidates, problem } = findGamesInFolder(folder([
    "roms/pumpitea.zip",
    "roms/pumpite.zip",
    "roms/pumpitea/010209_1821.chd",
    "roms/pumpitea/010209_1821.BIN",
    "roms/pumpitea/chdman.exe",
  ]));
  assert.equal(problem, null);
  assert.equal(candidates.length, 1);
  assert.equal(candidates[0].game, "pumpitea");
  assert.equal(candidates[0].zip.name, "pumpitea.zip");
  assert.equal(candidates[0].chd.name, "010209_1821.chd");
  // The game's ZIP first, the parent's after it.
  assert.deepEqual(candidates[0].zips.map((zip) => zip.name), ["pumpitea.zip", "pumpite.zip"]);
});

check("several games are all offered, sorted by name", () => {
  const { candidates } = findGamesInFolder(folder([
    "roms/pumpitea.zip",
    "roms/pumpit1.zip",
    "roms/pumpit2.zip",
    "roms/pumpitea/010209_1821.chd",
    "roms/pumpit1/19990930.chd",
    "roms/pumpit8/x.chd",
  ]));
  assert.deepEqual(candidates.map((entry) => entry.game), ["pumpit1", "pumpitea"]);
});

check("a flat folder pairs its one ZIP with the CHD beside it", () => {
  const { candidates, problem } = findGamesInFolder(folder([
    "picked/pumpitea.zip",
    "picked/010209_1821.chd",
  ]));
  assert.equal(problem, null);
  assert.equal(candidates.length, 1);
  assert.equal(candidates[0].game, "pumpitea");
  assert.equal(candidates[0].chd.name, "010209_1821.chd");
});

check("a flat folder with several ZIPs offers each with the CHD", () => {
  const { candidates } = findGamesInFolder(folder([
    "picked/pumpitea.zip",
    "picked/pumpite.zip",
    "picked/010209_1821.chd",
  ]));
  assert.deepEqual(candidates.map((entry) => entry.game), ["pumpite", "pumpitea"]);
});

check("the game's own subfolder alone has no ZIP, and says so", () => {
  const { candidates, problem } = findGamesInFolder(folder([
    "pumpitea/010209_1821.chd",
    "pumpitea/010209_1821.BIN",
  ]));
  assert.equal(candidates.length, 0);
  assert.match(problem, /CHD but no ZIP/);
});

check("a ZIP without its CHD is not a game", () => {
  const { candidates, problem } = findGamesInFolder(folder([
    "roms/pumpitea.zip",
    "roms/pumpit1/19990930.chd",
  ]));
  assert.equal(candidates.length, 0);
  assert.match(problem, /has its CHD/);
});

check("letter case does not split a pair, and the game is lower case", () => {
  const { candidates } = findGamesInFolder(folder([
    "ROMS/PUMPITEA.ZIP",
    "ROMS/PumpItEA/010209_1821.CHD",
  ]));
  assert.equal(candidates.length, 1);
  assert.equal(candidates[0].game, "pumpitea");
});

check("files deeper than <game>/ are not taken as CHDs", () => {
  const { candidates } = findGamesInFolder(folder([
    "roms/pumpitea.zip",
    "roms/pumpitea/old/010209_1821.chd",
  ]));
  assert.equal(candidates.length, 0);
});

check("separate picks pair once both are there", () => {
  assert.equal(pairSeparateFiles(file("pumpitea.zip"), null), null);
  assert.equal(pairSeparateFiles(null, file("010209_1821.chd")), null);
  const chosen = pairSeparateFiles(file("pumpitea.zip"), file("010209_1821.chd"));
  assert.equal(chosen.game, "pumpitea");
  assert.deepEqual(chosen.zips.map((zip) => zip.name), ["pumpitea.zip"]);
});

check("the mount puts the ZIPs at the top and the CHD in the game's folder", () => {
  const { candidates } = findGamesInFolder(folder([
    "roms/PumpITEA.zip",
    "roms/pumpite.zip",
    "roms/pumpitea/010209_1821.chd",
  ]));
  assert.deepEqual(mountBlobs(candidates[0]).map((blob) => blob.name),
    ["pumpitea.zip", "pumpite.zip", "pumpitea/010209_1821.chd"]);
});

check("a game is named by its ZIP", () => {
  assert.equal(gameOf("pumpitea.zip"), "pumpitea");
  assert.equal(gameOf("PUMPIT1.ZIP"), "pumpit1");
});

let failures = 0;
for (const { name, body } of checks) {
  try {
    body();
    console.log(`ok   ${name}`);
  } catch (error) {
    failures += 1;
    console.log(`FAIL ${name}\n     ${error.message}`);
  }
}
console.log(`rom_selection: ${checks.length - failures}/${checks.length}`);
process.exitCode = failures === 0 ? 0 : 1;
