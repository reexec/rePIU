// Issue #72. The browser page's canvas and event loop, for the engine running
// in the page's Worker. worker.js puts the OffscreenCanvas the page
// transferred in `Module.repiuCanvas` and sets `Module.repiuYield`; under Node
// neither exists and every function here answers "none".

#include "repiu/platform/browser_host.h"

#include <emscripten.h>
#include <emscripten/html5_webgl.h>

namespace
{

EM_JS(int, RepiuBrowserCanvasAvailable, (), {
    return (typeof Module !== "undefined" && Module.repiuCanvas) ? 1 : 0;
});

// The context is registered with Emscripten's GL layer so the C GL calls
// reach it. No depth, stencil or antialiasing (issue #69: the renderer draws
// into its own framebuffers), and an opaque canvas.
EM_JS(int, RepiuCreateBrowserGlContext, (int* width, int* height), {
    const canvas = Module.repiuCanvas;
    if (!canvas) {
        return 0;
    }
    const attributes = {
        alpha: false,
        depth: false,
        stencil: false,
        antialias: false,
        premultipliedAlpha: false,
        preserveDrawingBuffer: false,
        powerPreference: "high-performance",
    };
    const context = canvas.getContext("webgl2", attributes);
    if (!context) {
        return 0;
    }
    attributes.majorVersion = 2;
    attributes.minorVersion = 0;
    attributes.enableExtensionsByDefault = 1;
    const handle = GL.registerContext(context, attributes);
    if (!handle || !GL.makeContextCurrent(handle)) {
        return 0;
    }
    Module.repiuGlHandle = handle;
    HEAP32[width >> 2] = canvas.width;
    HEAP32[height >> 2] = canvas.height;
    return 1;
});

EM_JS(int, RepiuBrowserCanvasSize, (int* width, int* height), {
    const canvas = Module.repiuCanvas;
    if (!canvas) {
        return 0;
    }
    HEAP32[width >> 2] = canvas.width;
    HEAP32[height >> 2] = canvas.height;
    return 1;
});

EM_JS(void, RepiuDestroyBrowserGlContext, (), {
    if (Module.repiuGlHandle) {
        GL.makeContextCurrent(0);
        GL.deleteContext(Module.repiuGlHandle);
        Module.repiuGlHandle = 0;
    }
});

EM_JS(int, RepiuBrowserYieldWanted, (), {
    return (typeof Module !== "undefined" && Module.repiuYield) ? 1 : 0;
});

// One MessageChannel round rather than setTimeout(0): a Worker clamps nested
// timeouts to 4 ms, which would cost a fifth of every 16 ms frame. Needs the
// page's build (-sASYNCIFY); it is only reached when worker.js asked for it.
EM_ASYNC_JS(void, RepiuBrowserYield, (), {
    await new Promise((resolve) => {
        const channel = new MessageChannel();
        channel.port1.onmessage = () => {
            channel.port1.close();
            resolve();
        };
        channel.port2.postMessage(0);
    });
});

// Issue #87. A timer for all but the last millisecond, then MessageChannel
// rounds up to the deadline: a timer only keeps whole milliseconds and a
// Worker clamps nested ones to 4 ms, so a late wake-up is what the next
// frame's deadline makes up rather than what this one waits for.
EM_ASYNC_JS(void, RepiuBrowserWait, (int microseconds), {
    const deadline = performance.now() + microseconds / 1000;
    const remaining = deadline - performance.now();
    if (remaining > 2) {
        await new Promise((resolve) => setTimeout(resolve, Math.floor(remaining - 1)));
    }
    while (performance.now() < deadline) {
        await new Promise((resolve) => {
            const channel = new MessageChannel();
            channel.port1.onmessage = () => {
                channel.port1.close();
                resolve();
            };
            channel.port2.postMessage(0);
        });
    }
});

// Issue #76. worker.js queues what the page sends in `Module.repiuInput`
// (an array, oldest first). One item per call: `ints` gets kind, down,
// repeat, modifiers, pad, buttons, four axes, two triggers, and how long ago
// the page saw it in microseconds (-1 when unknown); `code` the key name.
EM_JS_DEPS(repiu_browser_input, "$stringToUTF8");
EM_JS(int, RepiuTakeBrowserInput, (char* code, int code_size, int* ints), {
    const queue = (typeof Module !== "undefined") ? Module.repiuInput : null;
    if (!queue || queue.length === 0) {
        return 0;
    }
    const item = queue.shift();
    const kinds = { "key": 0, "focus-lost": 1, "pad": 2, "pad-removed": 3,
                    "mouse-move": 4, "mouse-button": 5, "mouse-wheel": 6 };
    const kind = kinds[item.kind];
    if (kind === undefined) {
        return 0;
    }
    stringToUTF8(item.code || "", code, code_size);
    const values = [
        kind, item.down ? 1 : 0, item.repeat ? 1 : 0, item.mods | 0, item.index | 0,
        item.buttons | 0,
    ];
    const axes = item.axes || [];
    const triggers = item.triggers || [];
    for (let i = 0; i < 4; ++i) {
        values.push(axes[i] | 0);
    }
    for (let i = 0; i < 2; ++i) {
        values.push(triggers[i] | 0);
    }
    const now = performance.timeOrigin + performance.now();
    values.push((typeof item.time === "number")
        ? Math.min(2147483647, Math.max(0, Math.round((now - item.time) * 1000))) : -1);
    values.push(item.x | 0, item.y | 0, item.clicks | 0, item.wheelX | 0, item.wheelY | 0);
    for (let i = 0; i < values.length; ++i) {
        HEAP32[(ints >> 2) + i] = values[i];
    }
    return 1;
});

// Issue #81: worker.js puts the MessagePort to the page's AudioWorklet and
// the AudioContext's sample rate in Module.
EM_JS(int, RepiuBrowserAudioSampleRate, (), {
    return (typeof Module !== "undefined" && Module.repiuAudioPort &&
            Module.repiuAudioSampleRate) ? (Module.repiuAudioSampleRate | 0) : 0;
});

// A copy out of the heap, handed over with the message rather than copied
// again.
EM_JS(void, RepiuSendBrowserAudio, (const float* samples, int count), {
    const start = samples >> 2;
    const block = HEAPF32.slice(start, start + count);
    Module.repiuAudioPort.postMessage(block, [block.buffer]);
});

}  // namespace

namespace repiu::platform
{

bool BrowserCanvasAvailable()
{
    return RepiuBrowserCanvasAvailable() != 0;
}

bool CreateBrowserGlContext(int* width, int* height)
{
    return width != nullptr && height != nullptr &&
        RepiuCreateBrowserGlContext(width, height) != 0;
}

bool BrowserCanvasSize(int* width, int* height)
{
    return width != nullptr && height != nullptr &&
        RepiuBrowserCanvasSize(width, height) != 0;
}

void DestroyBrowserGlContext()
{
    RepiuDestroyBrowserGlContext();
}

BrowserGlFunction BrowserGlProcAddress(const char* name)
{
    return reinterpret_cast<BrowserGlFunction>(
        emscripten_webgl_get_proc_address(name));
}

bool YieldToHostEventLoop()
{
    if (RepiuBrowserYieldWanted() == 0)
    {
        return false;
    }
    RepiuBrowserYield();
    return true;
}

bool WaitInHostEventLoop(const std::uint32_t microseconds)
{
    if (RepiuBrowserYieldWanted() == 0)
    {
        return false;
    }
    RepiuBrowserWait(static_cast<int>(microseconds));
    return true;
}

int BrowserAudioSampleRate()
{
    return RepiuBrowserAudioSampleRate();
}

void SendBrowserAudio(const float* const interleaved, const std::size_t frames)
{
    if (interleaved == nullptr || frames == 0U || BrowserAudioSampleRate() <= 0)
    {
        return;
    }
    RepiuSendBrowserAudio(interleaved, static_cast<int>(frames * 2U));
}

bool TakeBrowserInputEvent(BrowserInputEvent* const event)
{
    if (event == nullptr)
    {
        return false;
    }
    *event = BrowserInputEvent{};
    int ints[18] = {};
    if (RepiuTakeBrowserInput(event->code, static_cast<int>(sizeof(event->code)), ints) == 0)
    {
        return false;
    }
    event->kind = static_cast<BrowserInputEvent::Kind>(ints[0]);
    event->down = ints[1] != 0;
    event->repeat = ints[2] != 0;
    event->modifiers = static_cast<std::uint8_t>(ints[3]);
    event->pad = static_cast<std::uint8_t>(ints[4]);
    event->buttons = static_cast<std::uint32_t>(ints[5]);
    for (int i = 0; i < 4; ++i)
    {
        event->axes[i] = static_cast<std::int16_t>(ints[6 + i]);
    }
    event->triggers[0] = static_cast<std::int16_t>(ints[10]);
    event->triggers[1] = static_cast<std::int16_t>(ints[11]);
    event->age_nanoseconds = ints[12] > 0 ? static_cast<std::uint64_t>(ints[12]) * 1000U : 0U;
    event->mouse_x = static_cast<std::int16_t>(ints[13]);
    event->mouse_y = static_cast<std::int16_t>(ints[14]);
    event->clicks = static_cast<std::uint8_t>(ints[15] > 0 ? ints[15] : 1);
    event->wheel_x = static_cast<std::int16_t>(ints[16]);
    event->wheel_y = static_cast<std::int16_t>(ints[17]);
    return true;
}

}  // namespace repiu::platform
