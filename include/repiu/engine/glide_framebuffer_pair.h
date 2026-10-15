#ifndef REPIU_ENGINE_GLIDE_FRAMEBUFFER_PAIR_H_
#define REPIU_ENGINE_GLIDE_FRAMEBUFFER_PAIR_H_

#include "repiu/engine/gl_api.h"
#include "repiu/engine/gl_unit_quad.h"
#include "repiu/engine/glide_letterbox.h"

#include <cstdint>
#include <string>

namespace repiu::engine
{

class GlidePostProcess;

// Issue #69: Glide's front and back buffers as two offscreen framebuffers
// (design 20261016-i069, decision 6). The game draws, clears and blits into
// them only; the window's own framebuffer receives a finished picture at the
// swap and nothing else. That is what WebGL needs -- it clears the window's
// framebuffer after showing it and shows whatever was drawn when the Worker
// yields -- and it makes the page-flip model hold whatever the driver does:
// after a swap, back is the previous front (design 729).
//
// Each framebuffer has an RGBA8 color texture the size of the picture
// rectangle, so the picture keeps the window's resolution; the one depth
// buffer, when there is one, is attached to both, as Glide's single auxiliary
// buffer is shared.
class GlideFramebufferPair
{
public:
    GlideFramebufferPair() = default;
    ~GlideFramebufferPair() = default;

    GlideFramebufferPair(const GlideFramebufferPair&) = delete;
    GlideFramebufferPair& operator=(const GlideFramebufferPair&) = delete;

    // Builds the present program and quad in the current context. The
    // framebuffers themselves are made by the first Resize.
    bool Initialize(const GlApi* api, bool depth, std::string* message);
    void Shutdown();

    // (Re)creates both framebuffers when the size changes, cleared to opaque
    // black and the far depth, and binds the draw target.
    bool Resize(std::uint32_t width, std::uint32_t height,
                std::string* message);

    // grRenderBuffer: where draws and clears land from now on.
    void SetDrawTargetFront(bool front);
    bool draw_target_front() const { return draw_target_front_; }
    void BindDrawTarget() const;

    // grBufferSwap: back becomes front and front becomes back. The draw
    // target is bound again.
    void Exchange();

    // Reads the back buffer as RGBA8, row 0 at the bottom, and binds the
    // draw target for reading again. `width` and `height` are clipped to the
    // buffer.
    bool ReadBack(std::uint32_t x, std::uint32_t y, std::uint32_t width,
                  std::uint32_t height, std::uint8_t* rgba8) const;

    // Draws the front buffer into the window's framebuffer: the whole
    // drawable cleared to opaque black (the letterbox bars), then the picture
    // in `rect`, through the post-process shader when one is active. Leaves
    // the window's framebuffer bound for the OSD; the caller restores the rest.
    void Present(std::uint32_t drawable_width, std::uint32_t drawable_height,
                 const GlideLetterboxRect& rect,
                 GlidePostProcess* post_process, std::uint32_t logical_width,
                 std::uint32_t logical_height);

    std::uint32_t width() const { return width_; }
    std::uint32_t height() const { return height_; }
    bool ready() const { return framebuffers_[0] != 0U; }

private:
    void DestroyBuffers();

    const GlApi* api_ = nullptr;
    bool depth_ = false;
    GLuint framebuffers_[2] = {};
    GLuint color_textures_[2] = {};
    GLuint depth_buffer_ = 0;
    // Index of the back buffer in the arrays above; the other is front.
    std::uint32_t back_ = 0;
    bool draw_target_front_ = false;
    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
    GLuint present_program_ = 0;
    GLint present_texture_ = -1;
    GlUnitQuad quad_;
};

}  // namespace repiu::engine

#endif  // REPIU_ENGINE_GLIDE_FRAMEBUFFER_PAIR_H_
