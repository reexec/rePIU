#include "repiu/engine/glide_framebuffer_pair.h"

#include "repiu/engine/gl_shader_dialect.h"
#include "repiu/engine/glide_post_process.h"

#include <algorithm>

namespace repiu::engine
{
namespace
{

// The unit square onto the whole viewport, sampled as it is.
constexpr char kPresentVertexBody[] =
    "in vec4 repiuPosition;\n"
    "in vec4 repiuTexCoord;\n"
    "out vec2 repiuUv;\n"
    "void main() {\n"
    "  gl_Position = vec4(repiuPosition.xy * 2.0 - 1.0, 0.0, 1.0);\n"
    "  repiuUv = repiuTexCoord.xy;\n"
    "}\n";
constexpr char kPresentFragmentBody[] =
    "uniform sampler2D repiuTexture;\n"
    "in vec2 repiuUv;\n"
    "out vec4 repiuFragColor;\n"
    "void main() {\n"
    "  repiuFragColor = texture(repiuTexture, repiuUv);\n"
    "}\n";

}  // namespace

bool GlideFramebufferPair::Initialize(const GlApi* api, bool depth,
                                      std::string* message)
{
    Shutdown();
    if (api == nullptr)
    {
        return false;
    }
    api_ = api;
    depth_ = depth;
    const std::string header = GlslHeader(api_->es);
    const std::string vertex = header + kPresentVertexBody;
    const std::string fragment = header + kPresentFragmentBody;
    present_program_ =
        BuildGlProgram(*api_, vertex.c_str(), fragment.c_str(), "repiuPosition",
                       nullptr, "repiuTexCoord", message);
    if (present_program_ == 0U || !quad_.Initialize(api_))
    {
        Shutdown();
        return false;
    }
    present_texture_ =
        api_->get_uniform_location(present_program_, "repiuTexture");
    return true;
}

void GlideFramebufferPair::Shutdown()
{
    if (api_ != nullptr)
    {
        DestroyBuffers();
        if (present_program_ != 0U)
        {
            api_->delete_program(present_program_);
        }
    }
    quad_.Shutdown();
    present_program_ = 0U;
    present_texture_ = -1;
    api_ = nullptr;
}

void GlideFramebufferPair::DestroyBuffers()
{
    if (framebuffers_[0] != 0U)
    {
        api_->bind_framebuffer(GL_FRAMEBUFFER, 0U);
        api_->delete_framebuffers(2, framebuffers_);
        glDeleteTextures(2, color_textures_);
    }
    if (depth_buffer_ != 0U)
    {
        api_->delete_renderbuffers(1, &depth_buffer_);
    }
    framebuffers_[0] = framebuffers_[1] = 0U;
    color_textures_[0] = color_textures_[1] = 0U;
    depth_buffer_ = 0U;
    width_ = 0U;
    height_ = 0U;
}

bool GlideFramebufferPair::Resize(std::uint32_t width, std::uint32_t height,
                                  std::string* message)
{
    if (api_ == nullptr || width == 0U || height == 0U)
    {
        return false;
    }
    if (ready() && width == width_ && height == height_)
    {
        BindDrawTarget();
        return true;
    }
    DestroyBuffers();
    GLint previous_texture = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &previous_texture);
    api_->gen_framebuffers(2, framebuffers_);
    glGenTextures(2, color_textures_);
    if (depth_)
    {
        api_->gen_renderbuffers(1, &depth_buffer_);
        api_->bind_renderbuffer(GL_RENDERBUFFER, depth_buffer_);
        api_->renderbuffer_storage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24,
                                   static_cast<GLsizei>(width),
                                   static_cast<GLsizei>(height));
    }
    // Clears below must reach the whole buffer whatever the game's state.
    GlStateSnapshot state;
    state.Capture();
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDepthMask(GL_TRUE);
    bool complete = true;
    for (std::uint32_t index = 0U; index < 2U; ++index)
    {
        glBindTexture(GL_TEXTURE_2D, color_textures_[index]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, static_cast<GLsizei>(width),
                     static_cast<GLsizei>(height), 0, GL_RGBA,
                     GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        api_->bind_framebuffer(GL_FRAMEBUFFER, framebuffers_[index]);
        api_->framebuffer_texture_2d(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                     GL_TEXTURE_2D, color_textures_[index], 0);
        if (depth_buffer_ != 0U)
        {
            api_->framebuffer_renderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                                           GL_RENDERBUFFER, depth_buffer_);
        }
        complete = complete &&
            api_->check_framebuffer_status(GL_FRAMEBUFFER) ==
                GL_FRAMEBUFFER_COMPLETE;
        glViewport(0, 0, static_cast<GLsizei>(width),
                   static_cast<GLsizei>(height));
        glClearColor(0.0F, 0.0F, 0.0F, 1.0F);
        api_->ClearDepth(1.0F);
        glClear(GL_COLOR_BUFFER_BIT |
                (depth_buffer_ != 0U ? GL_DEPTH_BUFFER_BIT : 0U));
    }
    state.Restore(*api_);
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previous_texture));
    width_ = width;
    height_ = height;
    if (!complete)
    {
        if (message != nullptr)
        {
            *message = "Glide offscreen framebuffers are incomplete";
        }
        DestroyBuffers();
        return false;
    }
    BindDrawTarget();
    return true;
}

void GlideFramebufferPair::SetDrawTargetFront(bool front)
{
    draw_target_front_ = front;
    BindDrawTarget();
}

void GlideFramebufferPair::BindDrawTarget() const
{
    if (api_ == nullptr || !ready())
    {
        return;
    }
    const std::uint32_t index = draw_target_front_ ? 1U - back_ : back_;
    api_->bind_framebuffer(GL_FRAMEBUFFER, framebuffers_[index]);
}

void GlideFramebufferPair::Exchange()
{
    back_ = 1U - back_;
    BindDrawTarget();
}

bool GlideFramebufferPair::ReadBack(std::uint32_t x, std::uint32_t y,
                                    std::uint32_t width, std::uint32_t height,
                                    std::uint8_t* rgba8) const
{
    if (api_ == nullptr || !ready() || rgba8 == nullptr || x >= width_ ||
        y >= height_)
    {
        return false;
    }
    width = std::min(width, width_ - x);
    height = std::min(height, height_ - y);
    // The read binding goes back to the draw target, which is what the game
    // has bound; asking GL instead would stall a threaded driver.
    api_->bind_framebuffer(GL_READ_FRAMEBUFFER, framebuffers_[back_]);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(static_cast<GLint>(x), static_cast<GLint>(y),
                 static_cast<GLsizei>(width), static_cast<GLsizei>(height),
                 GL_RGBA, GL_UNSIGNED_BYTE, rgba8);
    const std::uint32_t target = draw_target_front_ ? 1U - back_ : back_;
    api_->bind_framebuffer(GL_READ_FRAMEBUFFER, framebuffers_[target]);
    return true;
}

void GlideFramebufferPair::Present(std::uint32_t drawable_width,
                                   std::uint32_t drawable_height,
                                   const GlideLetterboxRect& rect,
                                   GlidePostProcess* post_process,
                                   std::uint32_t logical_width,
                                   std::uint32_t logical_height)
{
    if (api_ == nullptr || !ready())
    {
        return;
    }
    api_->bind_framebuffer(GL_FRAMEBUFFER, 0U);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    glViewport(0, 0, static_cast<GLsizei>(drawable_width),
               static_cast<GLsizei>(drawable_height));
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glClearColor(0.0F, 0.0F, 0.0F, 1.0F);
    glClear(GL_COLOR_BUFFER_BIT);
    if (rect.width == 0U || rect.height == 0U)
    {
        return;
    }
    const GLuint front = color_textures_[1U - back_];
    if (post_process != nullptr && post_process->active())
    {
        post_process->Apply(front, rect.x, rect.y, rect.width, rect.height,
                            logical_width, logical_height);
        return;
    }
    // Alpha is left as cleared, so no translucency reaches a compositor
    // through the window's alpha channel.
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);
    glViewport(static_cast<GLint>(rect.x), static_cast<GLint>(rect.y),
               static_cast<GLsizei>(rect.width),
               static_cast<GLsizei>(rect.height));
    api_->active_texture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, front);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    api_->use_program(present_program_);
    api_->uniform_1i(present_texture_, 0);
    quad_.Draw();
}

}  // namespace repiu::engine
