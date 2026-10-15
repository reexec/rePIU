// Issue #72: the SDL window and GL context the Glide backend used to manage
// itself, moved behind GlideHostWindow unchanged.

#include "repiu/engine/glide_host_window.h"

#include "repiu/platform/host_gpu_driver.h"

#include <SDL3/SDL.h>

namespace repiu::engine
{

class GlideHostWindowSdl final : public GlideHostWindow
{
public:
    ~GlideHostWindowSdl() override
    {
        Close();
    }

    bool Open(const char* title, int width, int height, std::string* stage,
              std::string* detail) override
    {
        Close();
        const auto fail = [&](const char* name) {
            *stage = name;
            *detail = SDL_GetError();
            return false;
        };
        if (!SDL_InitSubSystem(SDL_INIT_VIDEO))
        {
            return fail("SDL video");
        }
        SDL_GL_ResetAttributes();
        SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
        SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
        SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
        SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
        SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);
        // Issue #69: the depth buffer belongs to the offscreen pair; the
        // window's framebuffer only receives finished pictures.
        SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);
        const repiu::platform::GlContextRequest request =
            repiu::platform::RequestedGlContext();
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,
                            request.es ? SDL_GL_CONTEXT_PROFILE_ES
                                       : SDL_GL_CONTEXT_PROFILE_CORE);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, request.major);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, request.minor);
        window_ = SDL_CreateWindow(title, width, height,
                                   SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
        if (window_ == nullptr)
        {
            fail("SDL window");
            SDL_QuitSubSystem(SDL_INIT_VIDEO);
            return false;
        }
        context_ = SDL_GL_CreateContext(window_);
        if (context_ == nullptr)
        {
            fail("SDL GL context");
            SDL_DestroyWindow(window_);
            window_ = nullptr;
            SDL_QuitSubSystem(SDL_INIT_VIDEO);
            return false;
        }
        if (!SDL_GL_MakeCurrent(window_, context_))
        {
            fail("SDL GL current");
            SDL_GL_DestroyContext(context_);
            context_ = nullptr;
            SDL_DestroyWindow(window_);
            window_ = nullptr;
            SDL_QuitSubSystem(SDL_INIT_VIDEO);
            return false;
        }
        return true;
    }

    void Close() override
    {
        if (context_ != nullptr)
        {
            SDL_GL_MakeCurrent(window_, nullptr);
            SDL_GL_DestroyContext(context_);
            context_ = nullptr;
        }
        if (window_ != nullptr)
        {
            SDL_DestroyWindow(window_);
            window_ = nullptr;
            SDL_QuitSubSystem(SDL_INIT_VIDEO);
        }
    }

    void MakeCurrent() override
    {
        if (window_ != nullptr && context_ != nullptr)
        {
            SDL_GL_MakeCurrent(window_, context_);
        }
    }

    bool DrawableSize(int* width, int* height) const override
    {
        return window_ != nullptr &&
            SDL_GetWindowSizeInPixels(window_, width, height);
    }

    bool SetSize(int width, int height) override
    {
        return window_ != nullptr && SDL_SetWindowSize(window_, width, height);
    }

    bool IsFullscreen() const override
    {
        return window_ != nullptr &&
            (SDL_GetWindowFlags(window_) & SDL_WINDOW_FULLSCREEN) != 0U;
    }

    bool SetFullscreen(bool fullscreen) override
    {
        return window_ != nullptr &&
            SDL_SetWindowFullscreen(window_, fullscreen);
    }

    bool SetTitle(const char* title) override
    {
        return window_ != nullptr && SDL_SetWindowTitle(window_, title);
    }

    bool Present() override
    {
        return window_ != nullptr && SDL_GL_SwapWindow(window_);
    }

    bool SetSwapInterval(int interval) override
    {
        return SDL_GL_SetSwapInterval(interval);
    }

    bool SwapInterval(int* interval) const override
    {
        return SDL_GL_GetSwapInterval(interval);
    }

    double RefreshRateHz() const override
    {
        const SDL_DisplayID display =
            window_ != nullptr ? SDL_GetDisplayForWindow(window_) : 0U;
        const SDL_DisplayMode* const mode =
            display != 0U ? SDL_GetCurrentDisplayMode(display) : nullptr;
        return mode != nullptr ? static_cast<double>(mode->refresh_rate)
                               : 0.0;
    }

    std::string VideoDriver() const override
    {
        const char* const driver = SDL_GetCurrentVideoDriver();
        return driver != nullptr ? driver : "";
    }

    std::uint32_t SdlWindowId() const override
    {
        return window_ != nullptr ? SDL_GetWindowID(window_) : 0U;
    }

    void* SdlWindow() const override
    {
        return window_;
    }

    void* SdlContext() const override
    {
        return context_;
    }

private:
    SDL_Window* window_ = nullptr;
    SDL_GLContext context_ = nullptr;
};

std::unique_ptr<GlideHostWindow> CreateGlideHostWindowSdl()
{
    return std::make_unique<GlideHostWindowSdl>();
}

}  // namespace repiu::engine
