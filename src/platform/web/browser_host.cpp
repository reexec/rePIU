// Issue #72. The browser page's canvas and event loop, for the engine running
// in the page's Worker. worker.js puts the OffscreenCanvas the page
// transferred in `Module.repiuCanvas` and sets `Module.repiuYield`; under Node
// neither exists and every function here answers "none".

#include "repiu/platform/browser_host.h"

#include <emscripten.h>

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

bool YieldToHostEventLoop()
{
    if (RepiuBrowserYieldWanted() == 0)
    {
        return false;
    }
    RepiuBrowserYield();
    return true;
}

}  // namespace repiu::platform
