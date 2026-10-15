// Issue #72: the browser page's canvas as the Glide backend's window, for the
// engine running in the page's Worker, where SDL's video does not run. See
// docs/design/20261016-i072-browser-page.md, decision 5.

#include "repiu/engine/glide_host_window.h"

#include "repiu/platform/browser_host.h"

namespace repiu::engine
{

std::unique_ptr<GlideHostWindow> CreateGlideHostWindowSdl();

namespace
{

class GlideHostWindowBrowser final : public GlideHostWindow
{
public:
    ~GlideHostWindowBrowser() override
    {
        Close();
    }

    // The canvas's size is the page's choice; `width` and `height` are only
    // what the window would have been.
    bool Open(const char* /*title*/, int /*width*/, int /*height*/,
              std::string* stage, std::string* detail) override
    {
        int width = 0;
        int height = 0;
        if (!repiu::platform::CreateBrowserGlContext(&width, &height))
        {
            *stage = "browser canvas";
            *detail = "no WebGL2 context on the page's canvas";
            return false;
        }
        open_ = true;
        return true;
    }

    void Close() override
    {
        if (open_)
        {
            repiu::platform::DestroyBrowserGlContext();
            open_ = false;
        }
    }

    void MakeCurrent() override
    {
    }

    bool DrawableSize(int* width, int* height) const override
    {
        return open_ && repiu::platform::BrowserCanvasSize(width, height);
    }

    // The page lays the canvas out; the window controls do nothing here.
    bool SetSize(int /*width*/, int /*height*/) override
    {
        return false;
    }

    bool IsFullscreen() const override
    {
        return false;
    }

    bool SetFullscreen(bool /*fullscreen*/) override
    {
        return false;
    }

    bool SetTitle(const char* /*title*/) override
    {
        return true;
    }

    // The canvas shows the window's framebuffer when the Worker yields
    // (decision 6); there is nothing to swap.
    bool Present() override
    {
        return open_;
    }

    // Reported as applied, so the backend does not pace swaps with a busy
    // wait (Task 745): the yields pace them.
    bool SetSwapInterval(int interval) override
    {
        interval_ = interval;
        return true;
    }

    bool SwapInterval(int* interval) const override
    {
        *interval = interval_;
        return true;
    }

    double RefreshRateHz() const override
    {
        return 0.0;
    }

    std::string VideoDriver() const override
    {
        return "browser";
    }

    std::uint32_t SdlWindowId() const override
    {
        return 0U;
    }

    void* SdlWindow() const override
    {
        return nullptr;
    }

    void* SdlContext() const override
    {
        return nullptr;
    }

private:
    bool open_ = false;
    int interval_ = 1;
};

}  // namespace

std::unique_ptr<GlideHostWindow> CreateGlideHostWindow()
{
    if (repiu::platform::BrowserCanvasAvailable())
    {
        return std::make_unique<GlideHostWindowBrowser>();
    }
    return CreateGlideHostWindowSdl();
}

}  // namespace repiu::engine
