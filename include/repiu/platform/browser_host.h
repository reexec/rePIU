#ifndef REPIU_PLATFORM_BROWSER_HOST_H_
#define REPIU_PLATFORM_BROWSER_HOST_H_

namespace repiu::platform
{

// Issue #72. What a browser page gives the engine running in its Worker: a
// canvas to draw into and an event loop to hand the thread back to. Win32 and
// Linux have neither and answer "none"; under Node the web build has neither
// either. See docs/design/20261016-i072-browser-page.md, decisions 5 and 6.

// Whether the page handed the Worker a canvas (an OffscreenCanvas).
bool BrowserCanvasAvailable();

// Makes a WebGL2 (OpenGL ES 3.0) context on the canvas current and says the
// canvas's size in pixels. The context has no depth, stencil or
// antialiasing: the renderer draws into its own framebuffers.
bool CreateBrowserGlContext(int* width, int* height);
bool BrowserCanvasSize(int* width, int* height);
void DestroyBrowserGlContext();

// Finds a WebGL2 function by its OpenGL ES name, for the context above.
using BrowserGlFunction = void (*)();
BrowserGlFunction BrowserGlProcAddress(const char* name);

// Hands the thread back to the page's event loop once -- the canvas shows
// what was drawn and messages are handled -- and returns when it is given
// back. Does nothing unless the page asked for yields; true when it yielded.
bool YieldToHostEventLoop();

}  // namespace repiu::platform

#endif  // REPIU_PLATFORM_BROWSER_HOST_H_
