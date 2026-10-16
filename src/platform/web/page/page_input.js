// Issue #76: the page's keyboard and gamepads, sent to the Worker that runs
// the engine (design 20261016-i076). The engine turns them into SDL events, so
// the native key layout and gamepad bindings apply as they are.
//
// Keys go by KeyboardEvent.code (the key's position) with when they happened.
// While the game runs every key but Ctrl/Meta combinations, F11 and F12 has
// its browser default stopped, so F5 (COIN1) does not reload and Tab (the
// OSD) does not move the focus. Gamepads are sampled once a frame and sent
// when they change.
"use strict";

(function (root) {
  // The browser keeps these: fullscreen and the developer tools.
  const kBrowserKeys = new Set(["F11", "F12"]);
  const kPads = 4;

  function modifiersOf(event) {
    return (event.shiftKey ? 1 : 0) | (event.ctrlKey ? 2 : 0) |
      (event.altKey ? 4 : 0) | (event.metaKey ? 8 : 0);
  }

  // The browser's shortcuts stay the browser's.
  function isGameKey(event) {
    return !event.ctrlKey && !event.metaKey && !kBrowserKeys.has(event.code) &&
      event.code !== "" && event.code !== "Unidentified";
  }

  function scaled(value) {
    const clamped = Math.max(-1, Math.min(1, value || 0));
    return Math.round(clamped * 32767);
  }

  // A standard-mapping pad as the engine takes it: bit i of `buttons` is
  // button i, the sticks scaled to SDL's -32767..32767, the triggers 0..32767.
  function padState(index, pad) {
    let buttons = 0;
    pad.buttons.forEach((button, bit) => {
      if (bit < 17 && (button.pressed || button.value > 0.5)) {
        buttons |= 1 << bit;
      }
    });
    const axes = [0, 1, 2, 3].map((axis) => scaled(pad.axes[axis]));
    const trigger = (bit) => (pad.buttons[bit] ? scaled(pad.buttons[bit].value) : 0);
    return { kind: "pad", index, buttons, axes, triggers: [trigger(6), trigger(7)] };
  }

  // Starts sending this page's input to `worker`; returns a function that stops it.
  function forwardInput(worker) {
    const send = (events) => worker.postMessage({ type: "input", events });

    const onKey = (event) => {
      if (!isGameKey(event)) {
        return;
      }
      event.preventDefault();
      send([{
        kind: "key",
        code: event.code,
        down: event.type === "keydown",
        repeat: event.repeat,
        mods: modifiersOf(event),
        time: performance.timeOrigin + event.timeStamp,
      }]);
    };
    // Keys held when the page loses focus are let go of, as a native window's.
    const onFocusLost = () => send([{ kind: "focus-lost" }]);
    const onVisibility = () => {
      if (document.visibilityState === "hidden") {
        onFocusLost();
      }
    };

    const last = new Array(kPads).fill(null);
    const reported = new Set();
    let frame = 0;
    const samplePads = () => {
      frame = requestAnimationFrame(samplePads);
      const pads = navigator.getGamepads ? navigator.getGamepads() : [];
      const events = [];
      for (let index = 0; index < kPads; ++index) {
        const pad = pads[index];
        const usable = pad && pad.connected && pad.mapping === "standard";
        if (pad && pad.connected && !usable && !reported.has(pad.id)) {
          reported.add(pad.id);
          console.warn(`rePIU: gamepad "${pad.id}" has no standard mapping and is not used`);
        }
        if (!usable) {
          if (last[index] !== null) {
            last[index] = null;
            events.push({ kind: "pad-removed", index });
          }
          continue;
        }
        const state = padState(index, pad);
        const key = JSON.stringify(state);
        if (key !== last[index]) {
          last[index] = key;
          events.push(state);
        }
      }
      if (events.length !== 0) {
        send(events);
      }
    };

    window.addEventListener("keydown", onKey);
    window.addEventListener("keyup", onKey);
    window.addEventListener("blur", onFocusLost);
    document.addEventListener("visibilitychange", onVisibility);
    frame = requestAnimationFrame(samplePads);
    return () => {
      window.removeEventListener("keydown", onKey);
      window.removeEventListener("keyup", onKey);
      window.removeEventListener("blur", onFocusLost);
      document.removeEventListener("visibilitychange", onVisibility);
      cancelAnimationFrame(frame);
    };
  }

  root.PageInput = { forwardInput, isGameKey, padState };
})(typeof self !== "undefined" ? self : this);
