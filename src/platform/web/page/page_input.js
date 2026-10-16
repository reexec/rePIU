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

  // Where a mouse event lands on `surface`, in the drawing buffer's pixels:
  // the canvas is laid out at one size and draws at another.
  function canvasPoint(event, surface) {
    const box = surface.element.getBoundingClientRect();
    const x = box.width > 0 ? (event.clientX - box.left) * surface.width / box.width : 0;
    const y = box.height > 0 ? (event.clientY - box.top) * surface.height / box.height : 0;
    return {
      x: Math.max(0, Math.min(surface.width - 1, Math.round(x))),
      y: Math.max(0, Math.min(surface.height - 1, Math.round(y))),
    };
  }

  // Starts sending this page's input to `worker`; returns a function that stops
  // it. `surface` ({ element, width, height }: the canvas and its drawing size)
  // gets its mouse forwarded too, for the OSD (issue #76 follow-up).
  function forwardInput(worker, surface) {
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

    // The mouse over the canvas: moves, buttons (1 left, 2 middle, 3 right, as
    // SDL numbers them) and the wheel, in the canvas's drawing pixels.
    const time = (event) => performance.timeOrigin + event.timeStamp;
    const onMouseMove = (event) => {
      const point = canvasPoint(event, surface);
      send([{ kind: "mouse-move", x: point.x, y: point.y, time: time(event) }]);
    };
    const onMouseButton = (event) => {
      if (event.button > 2) {
        return;
      }
      event.preventDefault();
      const point = canvasPoint(event, surface);
      send([{
        kind: "mouse-button",
        index: [1, 2, 3][event.button],
        down: event.type === "mousedown",
        clicks: Math.min(Math.max(event.detail, 1), 255),
        x: point.x,
        y: point.y,
        time: time(event),
      }]);
    };
    const onWheel = (event) => {
      event.preventDefault();
      send([{
        kind: "mouse-wheel",
        wheelX: Math.sign(event.deltaX),
        wheelY: -Math.sign(event.deltaY),
        time: time(event),
      }]);
    };
    const onContextMenu = (event) => event.preventDefault();

    window.addEventListener("keydown", onKey);
    window.addEventListener("keyup", onKey);
    window.addEventListener("blur", onFocusLost);
    document.addEventListener("visibilitychange", onVisibility);
    if (surface) {
      surface.element.addEventListener("mousemove", onMouseMove);
      surface.element.addEventListener("mousedown", onMouseButton);
      surface.element.addEventListener("mouseup", onMouseButton);
      surface.element.addEventListener("wheel", onWheel, { passive: false });
      surface.element.addEventListener("contextmenu", onContextMenu);
    }
    frame = requestAnimationFrame(samplePads);
    return () => {
      window.removeEventListener("keydown", onKey);
      window.removeEventListener("keyup", onKey);
      window.removeEventListener("blur", onFocusLost);
      document.removeEventListener("visibilitychange", onVisibility);
      if (surface) {
        surface.element.removeEventListener("mousemove", onMouseMove);
        surface.element.removeEventListener("mousedown", onMouseButton);
        surface.element.removeEventListener("mouseup", onMouseButton);
        surface.element.removeEventListener("wheel", onWheel);
        surface.element.removeEventListener("contextmenu", onContextMenu);
      }
      cancelAnimationFrame(frame);
    };
  }

  root.PageInput = { forwardInput, isGameKey, padState, canvasPoint };
})(typeof self !== "undefined" ? self : this);
