// Issue #72, design decision 9: how the page turns what the user picked into a
// game. Pure functions, so the page (a classic script) and the Node test
// (rom_selection.test.js) share them.
//
// Two ways in:
// * One folder. Either the ROM-folder layout MAME and this repository use --
//   `<game>.zip` at the top and the CHD in `<game>/` -- or a flat folder with
//   the ZIP and the CHD side by side.
// * The ZIP and the CHD picked separately.
//
// Either way the result is { game, zip, chd, zips }: the game's MAME name, its
// ZIP and CHD, and every top-level ZIP to mount, the game's first. The others
// are there because the loader falls back to the parent ROM set's ZIP
// (pumpite.zip for pumpitea) when the game's ZIP lacks an entry, and mounting
// a ZIP the run never reads costs nothing.
"use strict";

(function (root) {
  function lower(name) {
    return name.toLowerCase();
  }

  function hasExtension(name, extension) {
    return lower(name).endsWith(extension);
  }

  // The MAME name a ZIP stands for: pumpitea.zip -> pumpitea. Lower case,
  // because that is how the loader spells its targets.
  function gameOf(zipName) {
    return lower(zipName.replace(/\.zip$/i, ""));
  }

  function byName(left, right) {
    return left.name < right.name ? -1 : left.name > right.name ? 1 : 0;
  }

  function candidate(game, zip, chd, zips) {
    const others = zips.filter((file) => file !== zip);
    return { game, zip, chd, zips: [zip].concat(others) };
  }

  // `entries` are { path, file }, `path` as a directory picker reports it
  // (webkitRelativePath): the chosen folder's own name, then the path inside.
  // Returns { candidates, problem }: the games found, sorted by name, and when
  // there are none, why, for the page to show.
  function findGamesInFolder(entries) {
    const zips = new Map();
    const nestedChds = new Map();
    const flatChds = [];
    for (const { path, file } of entries) {
      const inside = path.split("/").filter((part) => part.length !== 0).slice(1);
      if (inside.length === 1) {
        if (hasExtension(inside[0], ".zip")) {
          zips.set(gameOf(inside[0]), file);
        } else if (hasExtension(inside[0], ".chd")) {
          flatChds.push(file);
        }
      } else if (inside.length === 2 && hasExtension(inside[1], ".chd")) {
        const game = lower(inside[0]);
        if (!nestedChds.has(game)) {
          nestedChds.set(game, []);
        }
        nestedChds.get(game).push(file);
      }
    }

    const allZips = Array.from(zips.values()).sort(byName);
    const candidates = [];
    for (const [game, zip] of zips) {
      const nested = nestedChds.get(game);
      if (nested && nested.length !== 0) {
        candidates.push(candidate(game, zip, nested.slice().sort(byName)[0], allZips));
      } else if (flatChds.length === 1) {
        // A flat folder: the one CHD beside the ZIPs. With several ZIPs it is
        // not known which one is the game, so each is offered.
        candidates.push(candidate(game, zip, flatChds[0], allZips));
      }
    }
    candidates.sort((left, right) => (left.game < right.game ? -1 : left.game > right.game ? 1 : 0));

    let problem = null;
    if (candidates.length === 0) {
      if (zips.size === 0 && (flatChds.length !== 0 || nestedChds.size !== 0)) {
        problem = "This folder has a CHD but no ZIP. Pick the folder above it, the one with the game's ZIP, " +
          "or pick the ZIP and the CHD separately.";
      } else if (zips.size !== 0 && flatChds.length > 1) {
        problem = "This folder has several CHDs beside its ZIPs, so the pair is not clear. " +
          "Pick the ZIP and the CHD separately.";
      } else if (zips.size !== 0) {
        problem = "No ZIP in this folder has its CHD: expected <game>/<name>.chd beside <game>.zip.";
      } else {
        problem = "No ZIP or CHD in this folder.";
      }
    }
    return { candidates, problem };
  }

  // The ZIP and the CHD picked separately. Null until both are there.
  function pairSeparateFiles(zip, chd) {
    if (!zip || !chd) {
      return null;
    }
    return candidate(gameOf(zip.name), zip, chd, [zip]);
  }

  // What the Worker mounts under /roms for a candidate: the ZIPs at the top,
  // named by their games, and the CHD in the game's folder.
  function mountBlobs(chosen) {
    const blobs = chosen.zips.map((file) => ({ name: `${gameOf(file.name)}.zip`, data: file }));
    blobs.push({ name: `${chosen.game}/${chosen.chd.name}`, data: chosen.chd });
    return blobs;
  }

  const api = { findGamesInFolder, pairSeparateFiles, mountBlobs, gameOf };
  if (typeof module === "object" && module.exports) {
    module.exports = api;
  } else {
    root.RomSelection = api;
  }
})(typeof self !== "undefined" ? self : this);
