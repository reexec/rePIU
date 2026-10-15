#ifndef REPIU_ENGINE_GLIDE_HOST_WINDOW_H_
#define REPIU_ENGINE_GLIDE_HOST_WINDOW_H_

#include <cstdint>
#include <memory>
#include <string>

namespace repiu::engine
{

// Issue #72: the window the Glide backend draws into and its GL context --
// everything the backend asked SDL for besides events. Two implementations:
// an SDL window (Win32, Linux, and Node, where SDL's dummy video makes the
// backend fall back to its dummy) and a browser canvas (the OffscreenCanvas a
// Worker was handed, where SDL's video does not run). See
// docs/design/20261016-i072-browser-page.md, decision 5.
class GlideHostWindow
{
public:
    virtual ~GlideHostWindow() = default;

    // Opens the window `width` by `height` with the GL context the platform
    // asks for (RequestedGlContext), current on this thread. On failure,
    // `stage` names the step ("SDL video", "SDL window", ...) and `detail`
    // says why; nothing stays open.
    virtual bool Open(const char* title, int width, int height,
                      std::string* stage, std::string* detail) = 0;
    // Destroys the context and the window. The context is current while the
    // caller's GL objects go, so they go before this.
    virtual void Close() = 0;
    virtual void MakeCurrent() = 0;

    virtual bool DrawableSize(int* width, int* height) const = 0;
    virtual bool SetSize(int width, int height) = 0;
    virtual bool IsFullscreen() const = 0;
    virtual bool SetFullscreen(bool fullscreen) = 0;
    virtual bool SetTitle(const char* title) = 0;

    // Shows what the window's framebuffer holds.
    virtual bool Present() = 0;
    virtual bool SetSwapInterval(int interval) = 0;
    virtual bool SwapInterval(int* interval) const = 0;
    virtual double RefreshRateHz() const = 0;
    virtual std::string VideoDriver() const = 0;

    // SDL's window id, for the event filters and the input script; 0 when
    // the window is not SDL's.
    virtual std::uint32_t SdlWindowId() const = 0;
    // The SDL window and context ImGui's backends take; null when the window
    // is not SDL's, which leaves the OSD off.
    virtual void* SdlWindow() const = 0;
    virtual void* SdlContext() const = 0;
};

// The browser canvas when the platform has one, an SDL window otherwise.
std::unique_ptr<GlideHostWindow> CreateGlideHostWindow();

}  // namespace repiu::engine

#endif  // REPIU_ENGINE_GLIDE_HOST_WINDOW_H_
