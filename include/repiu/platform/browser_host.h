#ifndef REPIU_PLATFORM_BROWSER_HOST_H_
#define REPIU_PLATFORM_BROWSER_HOST_H_

#include <cstddef>
#include <cstdint>

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

// Issue #87. Like YieldToHostEventLoop, but returns only once `microseconds`
// have passed, the page's event loop running meanwhile (a timer, then rounds
// for the last part). Does nothing unless the page asked for yields; true
// when it waited.
bool WaitInHostEventLoop(std::uint32_t microseconds);

// Issue #76. One input the page forwarded to this Worker: a key, the page
// losing focus, or a gamepad's state. See
// docs/design/20261016-i076-browser-input.md.
struct BrowserInputEvent
{
    enum class Kind : std::uint8_t
    {
        kKey,
        kFocusLost,
        kPad,
        kPadRemoved,
        // Issue #76 follow-up: the mouse over the page's canvas, for the OSD.
        kMouseMove,
        kMouseButton,
        kMouseWheel,
    };
    // Bits of `modifiers`.
    static constexpr std::uint8_t kShift = 0x01U;
    static constexpr std::uint8_t kControl = 0x02U;
    static constexpr std::uint8_t kAlt = 0x04U;
    static constexpr std::uint8_t kMeta = 0x08U;

    Kind kind = Kind::kKey;
    // kKey: KeyboardEvent.code, pressed or released, an auto-repeat, and the
    // modifiers held.
    char code[32] = {};
    bool down = false;
    bool repeat = false;
    std::uint8_t modifiers = 0;
    // How long before it was taken the page saw it; zero when not known.
    std::uint64_t age_nanoseconds = 0;
    // kPad and kPadRemoved: the page's gamepad index. kPad: bit i of
    // `buttons` is the standard mapping's button i; the sticks' axes (left x,
    // left y, right x, right y) and the triggers (left, right) are scaled to
    // SDL's ranges.
    std::uint8_t pad = 0;
    std::uint32_t buttons = 0;
    std::int16_t axes[4] = {};
    std::int16_t triggers[2] = {};

    // kMouseMove and kMouseButton: where, in the canvas's pixels.
    // kMouseButton: which (1 left, 2 middle, 3 right) in `pad`, `down`, and
    // `clicks` 2 for a double click. kMouseWheel: notches,
    // up and right positive.
    std::int16_t mouse_x = 0;
    std::int16_t mouse_y = 0;
    std::uint8_t clicks = 1;
    std::int16_t wheel_x = 0;
    std::int16_t wheel_y = 0;
};

// Takes the oldest input the page sent, if any. Win32, Linux and Node have no
// page and answer false.
bool TakeBrowserInputEvent(BrowserInputEvent* event);

// Issue #81. The page's sound: the sample rate of its AudioContext, or 0 when
// the page asked for no sound (and on Win32, Linux and Node, which have no
// page). See docs/design/20261017-i081-browser-sound.md, decision 3.
int BrowserAudioSampleRate();

// Sends `frames` frames of interleaved float stereo to the page's
// AudioWorklet. Does nothing without a page.
void SendBrowserAudio(const float* interleaved, std::size_t frames);

}  // namespace repiu::platform

#endif  // REPIU_PLATFORM_BROWSER_HOST_H_
