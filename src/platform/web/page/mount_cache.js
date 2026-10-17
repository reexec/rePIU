// Issue #86: the game files the loader unpacks from the CHD, kept in the
// browser's origin private file system (OPFS) so later runs skip the unpack
// (design 20261017-i086). Used by the page (state, clear) and by the Worker
// (mount, store).
//
// Layout: rePIU/mounts/<game>/ holds the unpacked files at their paths under
// the loader's build/runtime_mounts/<game>, and cache.json, written last, holds
// the key (the CHD's name, size and modification time) and the totals. A
// folder without cache.json is an unfinished copy and is never used.
"use strict";

(function (root) {
  const kManifest = "cache.json";

  function keyOf(chd) {
    return { chdName: chd.name, chdSize: chd.size, chdModified: chd.lastModified };
  }

  function sameKey(a, b) {
    return a && b && a.chdName === b.chdName && a.chdSize === b.chdSize &&
      a.chdModified === b.chdModified;
  }

  function supported() {
    return typeof navigator !== "undefined" && navigator.storage &&
      typeof navigator.storage.getDirectory === "function";
  }

  async function mountsDirectory(create) {
    const top = await navigator.storage.getDirectory();
    const app = await top.getDirectoryHandle("rePIU", { create });
    return app.getDirectoryHandle("mounts", { create });
  }

  async function gameDirectory(game, create) {
    try {
      return await (await mountsDirectory(create)).getDirectoryHandle(game, { create });
    } catch {
      return null;
    }
  }

  async function readManifest(directory) {
    try {
      const file = await (await directory.getFileHandle(kManifest)).getFile();
      return JSON.parse(await file.text());
    } catch {
      return null;
    }
  }

  // The complete cache for `game`, or null.
  async function state(game) {
    if (!supported()) {
      return null;
    }
    const directory = await gameDirectory(game, false);
    return directory ? readManifest(directory) : null;
  }

  async function clear(game) {
    if (!supported()) {
      return;
    }
    try {
      await (await mountsDirectory(false)).removeEntry(game, { recursive: true });
    } catch {
      // Nothing to clear.
    }
  }

  // The cached files as WORKERFS blobs ({ name: "PIU/PIU.EXE", data: File }),
  // when the cache is complete and was made from the same CHD; else null.
  async function blobsFor(game, key) {
    if (!supported()) {
      return null;
    }
    const directory = await gameDirectory(game, false);
    const manifest = directory ? await readManifest(directory) : null;
    if (!manifest || !sameKey(manifest.key, key)) {
      return null;
    }
    const blobs = [];
    const walk = async (handle, prefix) => {
      for await (const [name, entry] of handle.entries()) {
        if (prefix === "" && name === kManifest) {
          continue;
        }
        if (entry.kind === "directory") {
          await walk(entry, `${prefix}${name}/`);
        } else {
          blobs.push({ name: `${prefix}${name}`, data: await entry.getFile() });
        }
      }
    };
    await walk(directory, "");
    return blobs.length === manifest.files ? blobs : null;
  }

  // Worker only (synchronous access handles). Copies the MEMFS folder `from`
  // to the cache for `game`, one file at a time with a turn of the event loop
  // between files so the engine keeps running, and writes the manifest last.
  // `report(done, total)` follows the bytes. Resolves to the manifest.
  async function store(game, key, FS, from, report) {
    const files = [];
    const walk = (path, relative) => {
      for (const name of FS.readdir(path)) {
        if (name === "." || name === "..") {
          continue;
        }
        const full = `${path}/${name}`;
        const rel = relative === "" ? name : `${relative}/${name}`;
        const mode = FS.stat(full).mode;
        if (FS.isDir(mode)) {
          walk(full, rel);
        } else if (FS.isFile(mode)) {
          files.push({ full, rel, size: FS.stat(full).size });
        }
      }
    };
    walk(from, "");
    const total = files.reduce((sum, file) => sum + file.size, 0);
    const estimate = navigator.storage.estimate ? await navigator.storage.estimate() : null;
    if (estimate && estimate.quota - estimate.usage < total) {
      throw new Error(`not enough browser storage: ${Math.round(total / 1048576)} MB needed, ` +
        `${Math.round((estimate.quota - estimate.usage) / 1048576)} MB free`);
    }
    await clear(game);
    const top = await gameDirectory(game, true);
    let done = 0;
    for (const file of files) {
      const parts = file.rel.split("/");
      let directory = top;
      for (const part of parts.slice(0, -1)) {
        directory = await directory.getDirectoryHandle(part, { create: true });
      }
      const handle = await directory.getFileHandle(parts[parts.length - 1], { create: true });
      const access = await handle.createSyncAccessHandle();
      try {
        access.truncate(0);
        access.write(FS.readFile(file.full), { at: 0 });
        access.flush();
      } finally {
        access.close();
      }
      done += file.size;
      report(done, total);
      await new Promise((resolve) => setTimeout(resolve, 0));
    }
    const manifest = { key, files: files.length, bytes: total, stored: Date.now() };
    const handle = await top.getFileHandle(kManifest, { create: true });
    const access = await handle.createSyncAccessHandle();
    try {
      access.truncate(0);
      access.write(new TextEncoder().encode(JSON.stringify(manifest)), { at: 0 });
      access.flush();
    } finally {
      access.close();
    }
    return manifest;
  }

  root.MountCache = { keyOf, sameKey, supported, state, clear, blobsFor, store };
})(typeof self !== "undefined" ? self : this);
