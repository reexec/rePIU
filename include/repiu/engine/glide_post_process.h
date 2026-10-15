#ifndef REPIU_ENGINE_GLIDE_POST_PROCESS_H_
#define REPIU_ENGINE_GLIDE_POST_PROCESS_H_

#include "repiu/engine/post_shader_catalog.h"
#include "repiu/engine/post_shader_source.h"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace repiu::engine
{

// Task 768. The presentation-only post-processing pass of the Glide backend.
// At the buffer swap the finished picture is drawn into the window through the
// selected shader. Nothing the guest can read changes: the pass reads the
// front buffer's texture and writes only the window's framebuffer (issue #69:
// the game draws into offscreen framebuffers, so no copy is needed).
//
// Threading: every method runs on the backend's host thread with the game's
// GL context current, except the destructor, which touches no GL so that it is
// safe after the context is gone; call Shutdown while it is still current.
class GlidePostProcess
{
public:
    GlidePostProcess();
    ~GlidePostProcess();

    GlidePostProcess(const GlidePostProcess&) = delete;
    GlidePostProcess& operator=(const GlidePostProcess&) = delete;

    // Resolves the GL entry points and lists the shaders. False leaves the
    // pass inert and `message` says why.
    bool Initialize(std::string* message);
    void Shutdown();
    bool initialized() const;

    const std::vector<PostShaderEntry>& catalog() const;
    const std::string& shader_directory() const;

    // `none`, or the id of the shader currently drawn.
    const std::string& active_id() const;
    bool active() const;

    // Why the last Select or Reload left no shader, or empty.
    const std::string& last_error() const;

    // Compiles and selects `id`; `none` or an empty id deselects. A failure
    // leaves no shader selected, sets last_error, and returns false.
    bool Select(std::string_view id);

    // Lists the directory again and recompiles the active shader, so an edited
    // file takes effect without restarting.
    bool Reload();

    // The active shader's `#pragma parameter` values, adjustable in place.
    std::vector<PostShaderParameter>& parameters();

    // Draws `input_texture` (the finished picture, row 0 at the bottom) into
    // the currently bound framebuffer's rectangle at (`x`, `y`), in GL pixel
    // coordinates (Task 769: the letterbox content rect), which is also the
    // shader's OutputSize. `logical_*` is the guest's screen size, reported to
    // the shader as InputSize and TextureSize. It changes the viewport, the
    // enables, the color mask, the program and the bindings, which the caller
    // restores.
    void Apply(unsigned int input_texture, std::uint32_t x, std::uint32_t y,
               std::uint32_t width, std::uint32_t height,
               std::uint32_t logical_width, std::uint32_t logical_height);

    struct Implementation;

private:
    std::unique_ptr<Implementation> implementation_;
};

}  // namespace repiu::engine

#endif  // REPIU_ENGINE_GLIDE_POST_PROCESS_H_
