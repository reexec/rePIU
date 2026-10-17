#ifndef REPIU_ENGINE_GLIDE_OSD_H_
#define REPIU_ENGINE_GLIDE_OSD_H_

#include "repiu/engine/frame_rate_history.h"
#include "repiu/engine/gl_renderer_identity.h"

#include <atomic>
#include <memory>
#include <string>
#include <vector>

struct ImGuiStyle;

namespace repiu::engine
{

class GlidePostProcess;

// Issue #45. The display options the OSD shows and may change. The backend
// fills `fullscreen` and `keep_aspect` with the current state before Render;
// a checkbox the operator flips sets the new value and its `_changed` flag,
// and the backend applies it at its next event pump.
struct GlideOsdDisplayOptions
{
    bool fullscreen = false;
    bool keep_aspect = true;
    bool fullscreen_changed = false;
    bool keep_aspect_changed = false;
};

// Task 761. The in-game on-screen display the launcher section of
// ARCHITECTURE.md promised: the same Dear ImGui layer, drawn inside the Glide
// backend's SDL window while the guest runs. It holds the LFB high-precision
// presentation toggle and, since Task 768, the screen shader menu; since #5 it
// opens with what draws the picture (GL renderer, vendor, version, driver).
// Since #15 it takes re2DJ's layout: pinned across the full width at the top,
// as tall as its content, opening with the name, version, build date and
// target profile, and with text and spacing that follow the window height.
//
// Threading: every method runs on the backend's host thread, the one that owns
// the SDL window and the GL context. The toggle the checkbox flips is an
// atomic the guest thread reads, which is why Render takes it as such.
//
// SDL and ImGui types stay out of this header the way the backend keeps them
// out of its own: the window, context, and event parameters are opaque
// pointers the implementation casts back.
class GlideOsd
{
public:
    GlideOsd();
    ~GlideOsd();

    GlideOsd(const GlideOsd&) = delete;
    GlideOsd& operator=(const GlideOsd&) = delete;

    // Creates the ImGui context against an SDL window and its current GL
    // context. False leaves the OSD inert and `message` says why.
    // `glsl_version` is the context's version line for ImGui's OpenGL3
    // backend (issue #69: GL 3.3 core natively, ES 3.0 on the web).
    bool Initialize(void* sdl_window, void* gl_context,
                    const char* glsl_version, std::string* message);
    void Shutdown();
    bool initialized() const { return initialized_; }
    // Issue #76: with no SDL window (a browser page's canvas), the size the
    // overlay lays out in, in drawable pixels. Ignored with a window.
    void SetDisplaySize(float width, float height)
    {
        display_width_ = width;
        display_height_ = height;
    }

    bool visible() const { return visible_; }
    // Issue #55: opening also gives the overlay focus, so a pad's D-pad moves
    // through its items at once.
    void ToggleVisible()
    {
        visible_ = !visible_;
        focus_pending_ = visible_;
    }

    // Task 769: true while the overlay is open and the pointer is over one of
    // its widgets, so a double click there is not a fullscreen toggle.
    bool WantsMouse() const;

    // Forwards one SDL_Event to ImGui. Called for every event while the OSD is
    // visible so the checkbox can be clicked; game input keeps flowing
    // regardless, because the cabinet's controls must never go dead.
    void ProcessEvent(const void* sdl_event);

    // Draws the overlay when visible. Call on the host thread with the game's
    // GL context current, immediately before the buffer swap; the ImGui GL3
    // backend saves and restores the GL state it touches. `post_process` may
    // be null, which hides the shader menu; choosing a shader there compiles
    // it on the spot, which this thread and context allow. `display` may be
    // null, which hides the fullscreen and keep-aspect checkboxes.
    void Render(std::atomic<bool>* lfb_high_precision,
                std::atomic<bool>* texture_full_precision,
                GlidePostProcess* post_process,
                GlideOsdDisplayOptions* display = nullptr);

    // #5. The renderer section at the top of the overlay. Set once, after the
    // GL context exists; until then the section is not drawn.
    void SetRendererIdentity(const GlRendererIdentity& identity);

    // #15. The lines that open the overlay, in order: the name, version and
    // build date, then the target profile.
    void SetInfoLines(const std::vector<std::string>& lines);

    // Issue #88. The backend's one-second frame rate measurement, taken over
    // `seconds`; recorded while the overlay is closed too, for the graph at
    // its top right.
    void RecordFrameRate(float frames_per_second, double seconds)
    {
        frame_rates_.Record(frames_per_second, seconds);
    }

private:
    bool initialized_ = false;
    bool windowless_ = false;
    float display_width_ = 0.0F;
    float display_height_ = 0.0F;
    bool visible_ = false;
    // Issue #55: focus to give on the next frame, and whether a menu was open
    // on the last one (B then closes the menu rather than the overlay).
    bool focus_pending_ = false;
    bool popup_open_last_frame_ = false;
    bool has_renderer_identity_ = false;
    GlRendererIdentity renderer_identity_;
    std::vector<std::string> info_lines_;
    FrameRateHistory frame_rates_;
    // #15. The style as first set up, which every scale starts again from,
    // and the scale last applied.
    std::unique_ptr<ImGuiStyle> base_style_;
    float applied_scale_ = 0.0F;
};

}  // namespace repiu::engine

#endif  // REPIU_ENGINE_GLIDE_OSD_H_
