#include "repiu/hle/glide_fog.h"
#include "repiu/hle/glide_lfb.h"
#include "repiu/hle/glide_vertex.h"
#if defined(_WIN32)
#include "repiu/engine/gl_api.h"
#include "repiu/engine/glide_letterbox.h"
#include "repiu/engine/glide_opengl_backend.h"
#include "repiu/engine/glide_post_process.h"
#include "repiu/hle/glide_hle.h"
#include "repiu/hle/glide_texture_decode.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_opengl.h>
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace
{

bool Check(bool condition, const char* message)
{
    if (condition)
    {
        return true;
    }
    std::cerr << "glide_render_probe failure: " << message << '\n';
    return false;
}

float TableWorldDistance(std::uint32_t index)
{
    return std::ldexp(1.0F, 3 + static_cast<int>(index >> 2U)) /
        static_cast<float>(8U - (index & 3U));
}

#if defined(_WIN32)
// Task 768: the post-processing pass in a real context. A white picture goes
// through the scanline shader at full strength and must come back with dark
// and bright rows; the crt shader must draw and leave the curved-off corner
// black; a missing shader must leave no pass; and the Glide state a swap's
// present touches must be restored. Issue #69: the pass reads a texture (the
// front buffer's, in the backend) and draws into the window's framebuffer.
bool RunOpenGlPostShaderProbe()
{
    constexpr std::uint32_t kWidth = 64U;
    constexpr std::uint32_t kHeight = 48U;
    std::vector<std::uint8_t> white(kWidth * kHeight * 4U, 255U);
    repiu::engine::GlideOpenGlBackend backend;
    backend.BindHostThread();
    if (!Check(backend.OpenWindowed(kWidth, kHeight, 2U, 1U, 1U),
               "OpenGL backend did not open"))
    {
        return false;
    }
    int drawable_width = 0;
    int drawable_height = 0;
    SDL_Window* const window = SDL_GL_GetCurrentWindow();
    if (!Check(window != nullptr &&
                   SDL_GetWindowSizeInPixels(window, &drawable_width,
                                             &drawable_height) &&
                   drawable_width > 0 && drawable_height > 0,
               "drawable size unavailable"))
    {
        return false;
    }
    const auto width = static_cast<std::uint32_t>(drawable_width);
    const auto height = static_cast<std::uint32_t>(drawable_height);
    repiu::engine::GlApi gl;
    std::string missing;
    if (!Check(repiu::engine::LoadGlApi(&gl, &missing),
               "GL entry points unavailable"))
    {
        std::cerr << missing << '\n';
        return false;
    }
    // The picture the pass reads: a white texture the drawable's size, like
    // the front buffer of a white frame.
    GLuint white_texture = 0;
    glGenTextures(1, &white_texture);
    glBindTexture(GL_TEXTURE_2D, white_texture);
    {
        std::vector<std::uint8_t> pixels(width * height * 4U, 255U);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, static_cast<GLsizei>(width),
                     static_cast<GLsizei>(height), 0, GL_RGBA,
                     GL_UNSIGNED_BYTE, pixels.data());
    }
    const auto apply = [&](repiu::engine::GlidePostProcess& pass) {
        repiu::engine::GlStateSnapshot state;
        state.Capture();
        gl.bind_framebuffer(GL_FRAMEBUFFER, 0U);
        glDisable(GL_SCISSOR_TEST);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glClearColor(0.0F, 0.0F, 0.0F, 1.0F);
        glClear(GL_COLOR_BUFFER_BIT);
        pass.Apply(white_texture, 0U, 0U, width, height, kWidth, kHeight);
        state.Restore(gl);
    };
    const auto read_red = [&gl, width, height]() {
        std::vector<std::uint8_t> pixels(width * height * 4U);
        GLint previous = 0;
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previous);
        gl.bind_framebuffer(GL_READ_FRAMEBUFFER, 0U);
        glReadPixels(0, 0, static_cast<GLsizei>(width),
                     static_cast<GLsizei>(height), GL_RGBA, GL_UNSIGNED_BYTE,
                     pixels.data());
        gl.bind_framebuffer(GL_READ_FRAMEBUFFER,
                            static_cast<GLuint>(previous));
        std::vector<std::uint8_t> red(width * height);
        for (std::size_t index = 0; index < red.size(); ++index)
        {
            red[index] = pixels[index * 4U];
        }
        return red;
    };

    repiu::engine::GlidePostProcess post;
    std::string message;
    if (!Check(post.Initialize(&message), "post-process initialize failed") ||
        !Check(!post.Select("no_such_shader.glsl") && !post.active() &&
                   !post.last_error().empty() &&
                   post.active_id() == "none",
               "a missing shader was not refused") ||
        !Check(post.Select("scanline") && post.active() &&
                   post.parameters().size() == 2U,
               "scanline shader did not compile"))
    {
        if (!post.last_error().empty())
        {
            std::cerr << post.last_error() << '\n';
        }
        return false;
    }
    for (repiu::engine::PostShaderParameter& parameter : post.parameters())
    {
        // Full-strength gaps and no brightness boost: gap rows go black.
        parameter.value = 1.0F;
    }

    if (!Check(backend.PresentLfbSurface(white.data(), kWidth, kHeight, false,
                                         false),
               "white surface presentation failed"))
    {
        return false;
    }
    // A swap presents through the window's framebuffer and must hand the
    // game back the state it set through Glide and its (new) back buffer.
    repiu::hle::GlideAlphaBlendState translucent;
    translucent.rgb_source = 1U;
    translucent.rgb_destination = 5U;
    translucent.alpha_source = 4U;
    translucent.alpha_destination = 0U;
    translucent.valid = true;
    backend.SetAlphaBlend(translucent);
    GLint viewport_before[4]{};
    glGetIntegerv(GL_VIEWPORT, viewport_before);
    GLint program_before = 0;
    glGetIntegerv(GL_CURRENT_PROGRAM, &program_before);
    const bool swapped = backend.BufferSwap(0U);
    GLint viewport_after[4]{};
    glGetIntegerv(GL_VIEWPORT, viewport_after);
    GLint program_after = 0;
    glGetIntegerv(GL_CURRENT_PROGRAM, &program_after);
    GLint framebuffer_after = 0;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &framebuffer_after);
    const bool state_restored = swapped && glIsEnabled(GL_BLEND) == GL_TRUE &&
        std::memcmp(viewport_before, viewport_after,
                    sizeof(viewport_before)) == 0 &&
        program_before == program_after && framebuffer_after != 0 &&
        glGetError() == GL_NO_ERROR;
    translucent.rgb_source = 4U;
    translucent.rgb_destination = 0U;
    backend.SetAlphaBlend(translucent);
    apply(post);

    const std::vector<std::uint8_t> scanline = read_red();
    std::size_t bright_rows = 0;
    std::size_t dark_rows = 0;
    for (std::uint32_t y = 0; y < height; ++y)
    {
        const std::uint8_t value = scanline[y * width + width / 2U];
        bright_rows += value > 200U ? 1U : 0U;
        dark_rows += value < 60U ? 1U : 0U;
    }
    const bool scanlines_ok =
        bright_rows >= height / 3U && dark_rows >= height / 3U;

    bool crt_ok = post.Select("crt") && post.active() &&
        post.parameters().size() == 6U;
    if (crt_ok)
    {
        apply(post);
        const std::vector<std::uint8_t> crt = read_red();
        std::uint32_t lit = 0;
        for (std::uint32_t y = height / 4U; y < height * 3U / 4U; ++y)
        {
            lit = (std::max)(lit, static_cast<std::uint32_t>(
                                      crt[y * width + width / 2U]));
        }
        crt_ok = crt[0] == 0U && lit > 128U;
    }
    // Issue #90: the multi-pass upscalers. fsr is EASU into a target at the
    // window's size and RCAS into the window; scalefx and anime4k are five
    // passes (the first two or four through floating-point targets) and a
    // stock pass into the window. A white picture must come back bright
    // nearly everywhere (none of them invents detail on a flat field), and
    // the chain must still be selected afterwards, which it is not when a
    // pass target could not be made.
    const struct
    {
        const char* id;
        std::size_t passes;
        std::size_t parameters;
    } kChains[] = {{"fsr", 2U, 1U}, {"scalefx", 5U, 3U}, {"anime4k", 5U, 0U}};
    bool chains_ok = true;
    std::string chain_report;
    for (const auto& chain : kChains)
    {
        bool ok = post.Select(chain.id) && post.active() &&
            post.pass_count() == chain.passes &&
            post.parameters().size() == chain.parameters;
        std::size_t bright = 0;
        if (ok)
        {
            apply(post);
            const std::vector<std::uint8_t> red = read_red();
            for (const std::uint8_t value : red)
            {
                bright += value > 200U ? 1U : 0U;
            }
            ok = post.active() && post.last_error().empty() &&
                bright >= red.size() * 9U / 10U &&
                glGetError() == GL_NO_ERROR;
        }
        if (!post.last_error().empty())
        {
            std::cerr << chain.id << ": " << post.last_error() << '\n';
        }
        chain_report += std::string("\npost_shader_gl_") + chain.id + "=" +
            (ok ? "true" : "false") + " bright=" + std::to_string(bright) +
            "/" + std::to_string(width * height);
        chains_ok = chains_ok && ok;
    }
    const bool none_ok = post.Select("none") && !post.active();

    // Task 769: a window twice as wide as the picture's ratio pillarboxes it.
    // The viewport must be the centred content rect, and the readback the
    // guest sees must cover the picture only: the whole drawable is cleared
    // black first, so a readback that included the bars would not be white.
    bool letterbox_ok = false;
    SDL_SetWindowSize(window, static_cast<int>(kWidth * 4U),
                      static_cast<int>(kHeight * 2U));
    SDL_SyncWindow(window);
    backend.PumpEvents();
    int wide_width = 0;
    int wide_height = 0;
    if (SDL_GetWindowSizeInPixels(window, &wide_width, &wide_height) &&
        wide_width > 0 && wide_height > 0)
    {
        const repiu::engine::GlideLetterboxRect expected =
            repiu::engine::ComputeGlideLetterboxRect(
                kWidth, kHeight, static_cast<std::uint32_t>(wide_width),
                static_cast<std::uint32_t>(wide_height));
        const repiu::engine::GlideLetterboxRect& rect = backend.content_rect();
        GLint viewport[4]{};
        glGetIntegerv(GL_VIEWPORT, viewport);
        std::vector<std::uint8_t> picture;
        letterbox_ok = expected.x > 0U && rect.x == expected.x &&
            rect.y == expected.y && rect.width == expected.width &&
            rect.height == expected.height && viewport[0] == 0 &&
            viewport[1] == 0 &&
            viewport[2] == static_cast<GLint>(expected.width) &&
            viewport[3] == static_cast<GLint>(expected.height) &&
            backend.PresentLfbSurface(white.data(), kWidth, kHeight, false,
                                      false) &&
            backend.ReadbackFramebuffer(kWidth, kHeight, &picture);
        for (std::size_t index = 0; letterbox_ok && index < picture.size();
             index += 4U)
        {
            letterbox_ok = picture[index] == 255U;
        }
        std::cout << "post_shader_gl_letterbox_rect=" << rect.x << ","
                  << rect.y << "," << rect.width << "," << rect.height
                  << " drawable=" << wide_width << "x" << wide_height << '\n';
    }

    // Task 769: Alt+Enter goes fullscreen and a double click comes back,
    // through the backend's own event pump. The display mode must not change,
    // and fullscreen must still letterbox the picture.
    bool fullscreen_ok = false;
    SDL_DisplayMode mode_before{};
    const SDL_DisplayMode* const current_mode =
        SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(window));
    if (current_mode != nullptr)
    {
        mode_before = *current_mode;
        SDL_Event key{};
        key.type = SDL_EVENT_KEY_DOWN;
        key.key.windowID = SDL_GetWindowID(window);
        key.key.key = SDLK_RETURN;
        key.key.mod = SDL_KMOD_LALT;
        key.key.down = true;
        SDL_PushEvent(&key);
        backend.PumpEvents();
        SDL_SyncWindow(window);
        backend.PumpEvents();
        int full_width = 0;
        int full_height = 0;
        const repiu::engine::GlideLetterboxRect rect = backend.content_rect();
        const bool entered =
            (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN) != 0U &&
            SDL_GetWindowSizeInPixels(window, &full_width, &full_height);
        const repiu::engine::GlideLetterboxRect expected =
            repiu::engine::ComputeGlideLetterboxRect(
                kWidth, kHeight, static_cast<std::uint32_t>(full_width),
                static_cast<std::uint32_t>(full_height));
        const SDL_DisplayMode* const full_mode =
            SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(window));
        const bool mode_kept = full_mode != nullptr &&
            full_mode->w == mode_before.w && full_mode->h == mode_before.h;

        SDL_Event click{};
        click.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
        click.button.windowID = SDL_GetWindowID(window);
        click.button.button = SDL_BUTTON_LEFT;
        click.button.clicks = 2;
        click.button.down = true;
        SDL_PushEvent(&click);
        backend.PumpEvents();
        SDL_SyncWindow(window);
        backend.PumpEvents();
        const bool left =
            (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN) == 0U;
        fullscreen_ok = entered && mode_kept && left &&
            rect.x == expected.x && rect.y == expected.y &&
            rect.width == expected.width && rect.height == expected.height;
        std::cout << "glide_fullscreen_entered/mode-kept/left="
                  << entered << "/" << mode_kept << "/" << left
                  << " fullscreen=" << full_width << "x" << full_height
                  << " rect=" << rect.x << "," << rect.y << "," << rect.width
                  << "," << rect.height << '\n';
    }
    std::cout << "glide_fullscreen_toggle=" << (fullscreen_ok ? "true" : "false")
              << '\n';

    post.Shutdown();
    glDeleteTextures(1, &white_texture);
    backend.Close();

    std::cout << "post_shader_gl_state_restored="
              << (state_restored ? "true" : "false")
              << "\npost_shader_gl_scanline_rows=" << bright_rows << "/"
              << dark_rows << "/" << height
              << "\npost_shader_gl_scanline=" << (scanlines_ok ? "true" : "false")
              << "\npost_shader_gl_crt=" << (crt_ok ? "true" : "false")
              << chain_report
              << "\npost_shader_gl_none=" << (none_ok ? "true" : "false")
              << "\nglide_letterbox_gl=" << (letterbox_ok ? "true" : "false")
              << '\n';
    return Check(state_restored, "a swap left the game's GL state changed") &&
        Check(scanlines_ok, "scanline rows did not alternate") &&
        Check(crt_ok, "crt shader did not draw as expected") &&
        Check(chains_ok, "a multi-pass chain did not draw as expected") &&
        Check(none_ok, "none did not clear the pass") &&
        Check(letterbox_ok, "letterbox viewport or readback was wrong") &&
        Check(fullscreen_ok, "fullscreen toggle did not round-trip");
}

// Issue #69: a fixed scene drawn through the backend's Glide API only, so the
// same code draws it before and after the renderer moved off fixed-function
// OpenGL. The picture read back is written to a file and, given a reference,
// compared pixel by pixel. It touches every path the port rewrites: the
// combine functions, textures with repeat, clamp, perspective and filtering,
// the alpha test at its boundary, blending, depth, table fog, the constant
// color, points, lines, polygons, batches, culling, the color mask and the
// LFB blit in both orientations.
using repiu::hle::GlideDrawVertex;

GlideDrawVertex SceneVertex(float x, float y, std::uint32_t rgba)
{
    GlideDrawVertex vertex;
    vertex.x = x;
    vertex.y = y;
    vertex.r = static_cast<float>((rgba >> 24U) & 0xFFU) / 255.0F;
    vertex.g = static_cast<float>((rgba >> 16U) & 0xFFU) / 255.0F;
    vertex.b = static_cast<float>((rgba >> 8U) & 0xFFU) / 255.0F;
    vertex.a = static_cast<float>(rgba & 0xFFU) / 255.0F;
    return vertex;
}

GlideDrawVertex SceneTexturedVertex(float x, float y, float s, float t,
                                    float oow)
{
    GlideDrawVertex vertex = SceneVertex(x, y, 0xFFFFFFFFU);
    vertex.s = s * oow;
    vertex.t = t * oow;
    vertex.texture_oow = oow;
    vertex.fog_oow = oow;
    return vertex;
}

repiu::hle::GlideAlphaCombineState SceneCombine(std::uint32_t function,
                                                std::uint32_t factor,
                                                std::uint32_t local,
                                                std::uint32_t other,
                                                bool invert = false)
{
    repiu::hle::GlideAlphaCombineState state;
    state.function = function;
    state.factor = factor;
    state.local = local;
    state.other = other;
    state.invert = invert;
    state.valid = true;
    return state;
}

repiu::hle::GlideAlphaBlendState SceneBlend(std::uint32_t source,
                                            std::uint32_t destination)
{
    repiu::hle::GlideAlphaBlendState state;
    state.rgb_source = source;
    state.rgb_destination = destination;
    state.alpha_source = 4U;
    state.alpha_destination = 0U;
    state.valid = true;
    return state;
}

bool DrawSceneQuad(repiu::engine::GlideOpenGlBackend& backend,
                   const GlideDrawVertex& top_left,
                   const GlideDrawVertex& top_right,
                   const GlideDrawVertex& bottom_right,
                   const GlideDrawVertex& bottom_left)
{
    return backend.DrawTriangle(top_left, top_right, bottom_right) &&
        backend.DrawTriangle(top_left, bottom_right, bottom_left);
}

// The combine, texture, alpha, blend, depth and fog parts of the scene.
bool DrawSceneGeometry(repiu::engine::GlideOpenGlBackend& backend)
{
    using repiu::engine::GlideBatchPrimitive;
    const auto iterated = SceneCombine(1U, 0U, 0U, 0U);
    bool ok = backend.SetDepthBufferMode(0U) &&
        backend.SetAlphaBlend(SceneBlend(4U, 0U)) &&
        backend.SetColorCombine(iterated) && backend.SetAlphaCombine(iterated) &&
        backend.SetAlphaTestFunction(7U) &&
        backend.BufferClear(0x00203040U, 0xFFU, 0xFFFFU);

    // Gouraud triangle.
    ok = ok &&
        backend.DrawTriangle(SceneVertex(4.0F, 4.0F, 0xFF0000FFU),
                             SceneVertex(60.0F, 8.0F, 0x00FF00FFU),
                             SceneVertex(20.0F, 50.0F, 0x0000FFFFU));

    // An 8x8 RGB565 checker, repeated twice, nearest, then a perspective
    // quad, then linear filtering with clamping past the edges.
    std::vector<std::uint8_t> checker(8U * 8U * 2U);
    for (std::uint32_t y = 0U; y < 8U; ++y)
    {
        for (std::uint32_t x = 0U; x < 8U; ++x)
        {
            const std::uint16_t texel = ((x + y) & 1U) != 0U
                ? static_cast<std::uint16_t>(0xFFE0U)
                : static_cast<std::uint16_t>(0x001FU | (x << 6U));
            checker[(y * 8U + x) * 2U] = static_cast<std::uint8_t>(texel);
            checker[(y * 8U + x) * 2U + 1U] =
                static_cast<std::uint8_t>(texel >> 8U);
        }
    }
    repiu::hle::GlideTextureDimensions dimensions;
    ok = ok &&
        Check(repiu::hle::CalculateGlideTextureDimensions(5U, 3U,
                                                         &dimensions) &&
                  dimensions.width == 8U && dimensions.height == 8U,
              "scene texture is not 8x8") &&
        backend.StoreTexture(0x1000U, 10U, 5U, 3U, checker.data(),
                             checker.size(), nullptr, nullptr) &&
        backend.SourceTexture(0x1000U);
    backend.SetTextureClampMode(0U, 0U);
    backend.SetTextureFilterMode(0U, 0U);
    backend.SetTextureCombineEnabled(true);
    ok = ok && backend.SetColorCombine(SceneCombine(3U, 8U, 0U, 1U)) &&
        DrawSceneQuad(backend, SceneTexturedVertex(64.0F, 4.0F, 0.0F, 0.0F, 1.0F),
                      SceneTexturedVertex(104.0F, 4.0F, 512.0F, 0.0F, 1.0F),
                      SceneTexturedVertex(104.0F, 44.0F, 512.0F, 512.0F, 1.0F),
                      SceneTexturedVertex(64.0F, 44.0F, 0.0F, 512.0F, 1.0F)) &&
        DrawSceneQuad(backend,
                      SceneTexturedVertex(108.0F, 4.0F, 0.0F, 0.0F, 1.0F),
                      SceneTexturedVertex(156.0F, 4.0F, 256.0F, 0.0F, 0.25F),
                      SceneTexturedVertex(156.0F, 44.0F, 256.0F, 256.0F, 0.25F),
                      SceneTexturedVertex(108.0F, 44.0F, 0.0F, 256.0F, 1.0F));
    backend.SetTextureClampMode(1U, 1U);
    backend.SetTextureFilterMode(1U, 1U);
    ok = ok &&
        DrawSceneQuad(backend,
                      SceneTexturedVertex(4.0F, 54.0F, -128.0F, -128.0F, 1.0F),
                      SceneTexturedVertex(44.0F, 54.0F, 384.0F, -128.0F, 1.0F),
                      SceneTexturedVertex(44.0F, 94.0F, 384.0F, 384.0F, 1.0F),
                      SceneTexturedVertex(4.0F, 94.0F, -128.0F, 384.0F, 1.0F));

    // A 16x16 ARGB4444 texture whose alpha nibble is the column: GREATER
    // against 0x88 keeps columns 9..15 and drops column 8, the boundary.
    std::vector<std::uint8_t> ramp(16U * 16U * 2U);
    for (std::uint32_t y = 0U; y < 16U; ++y)
    {
        for (std::uint32_t x = 0U; x < 16U; ++x)
        {
            const std::uint16_t texel =
                static_cast<std::uint16_t>((x << 12U) | 0x0F80U | (y & 0xFU));
            ramp[(y * 16U + x) * 2U] = static_cast<std::uint8_t>(texel);
            ramp[(y * 16U + x) * 2U + 1U] =
                static_cast<std::uint8_t>(texel >> 8U);
        }
    }
    backend.SetTextureClampMode(1U, 1U);
    backend.SetTextureFilterMode(0U, 0U);
    ok = ok &&
        backend.StoreTexture(0x2000U, 12U, 4U, 3U, ramp.data(), ramp.size(),
                             nullptr, nullptr) &&
        backend.SourceTexture(0x2000U) &&
        backend.SetAlphaCombine(SceneCombine(2U, 0U, 0U, 1U)) &&
        backend.SetAlphaTestReferenceValue(0x88U) &&
        backend.SetAlphaTestFunction(4U) &&
        DrawSceneQuad(backend,
                      SceneTexturedVertex(48.0F, 54.0F, 0.0F, 0.0F, 1.0F),
                      SceneTexturedVertex(96.0F, 54.0F, 256.0F, 0.0F, 1.0F),
                      SceneTexturedVertex(96.0F, 70.0F, 256.0F, 256.0F, 1.0F),
                      SceneTexturedVertex(48.0F, 70.0F, 0.0F, 256.0F, 1.0F)) &&
        backend.SetAlphaTestFunction(7U) &&
        backend.SetAlphaCombine(iterated);
    backend.SetTextureCombineEnabled(false);

    // Blending: half-transparent white over the gouraud triangle and the
    // repeated checker, then additive.
    ok = ok && backend.SetColorCombine(iterated) &&
        backend.SetAlphaBlend(SceneBlend(1U, 5U)) &&
        backend.DrawTriangle(SceneVertex(30.0F, 20.0F, 0xFFFFFF80U),
                             SceneVertex(90.0F, 30.0F, 0xFFFFFF80U),
                             SceneVertex(40.0F, 46.0F, 0x0080FF40U)) &&
        backend.SetAlphaBlend(SceneBlend(4U, 4U)) &&
        backend.DrawTriangle(SceneVertex(100.0F, 30.0F, 0x40000080U),
                             SceneVertex(150.0F, 30.0F, 0x40000080U),
                             SceneVertex(125.0F, 52.0F, 0x40000080U)) &&
        backend.SetAlphaBlend(SceneBlend(4U, 0U));

    // Depth: LESS keeps the nearer of overlapping triangles whatever the
    // order; the last one is nearest only on its left half.
    GlideDrawVertex near_left = SceneVertex(100.0F, 56.0F, 0x0000FFFFU);
    GlideDrawVertex near_right = SceneVertex(156.0F, 56.0F, 0x0000FFFFU);
    GlideDrawVertex near_bottom = SceneVertex(128.0F, 96.0F, 0x0000FFFFU);
    near_left.ooz = 60000.0F;
    near_right.ooz = 100.0F;
    near_bottom.ooz = 30000.0F;
    GlideDrawVertex far_a = SceneVertex(104.0F, 60.0F, 0xFF0000FFU);
    GlideDrawVertex far_b = SceneVertex(152.0F, 64.0F, 0xFF0000FFU);
    GlideDrawVertex far_c = SceneVertex(120.0F, 92.0F, 0xFF0000FFU);
    far_a.ooz = far_b.ooz = far_c.ooz = 20000.0F;
    GlideDrawVertex mid_a = SceneVertex(110.0F, 66.0F, 0x00FF00FFU);
    GlideDrawVertex mid_b = SceneVertex(150.0F, 70.0F, 0x00FF00FFU);
    GlideDrawVertex mid_c = SceneVertex(130.0F, 94.0F, 0x00FF00FFU);
    mid_a.ooz = mid_b.ooz = mid_c.ooz = 40000.0F;
    ok = ok && backend.SetDepthBufferMode(1U) &&
        backend.SetDepthBufferFunction(1U) && backend.SetDepthMask(true) &&
        backend.DrawTriangle(far_a, far_b, far_c) &&
        backend.DrawTriangle(mid_a, mid_b, mid_c) &&
        backend.DrawTriangle(near_left, near_right, near_bottom) &&
        backend.SetDepthMask(false) && backend.SetDepthBufferMode(0U);

    // Table fog across a quad whose w runs from 1 to 4096.
    repiu::hle::GlideFogTable fog{};
    for (std::uint32_t index = 0U; index < fog.size(); ++index)
    {
        fog[index] = static_cast<std::uint8_t>(index * 255U / 63U);
    }
    GlideDrawVertex fog_near_top = SceneVertex(4.0F, 98.0F, 0x2040FFFFU);
    GlideDrawVertex fog_far_top = SceneVertex(76.0F, 98.0F, 0x2040FFFFU);
    GlideDrawVertex fog_far_bottom = SceneVertex(76.0F, 116.0F, 0x2040FFFFU);
    GlideDrawVertex fog_near_bottom = SceneVertex(4.0F, 116.0F, 0x2040FFFFU);
    fog_near_top.fog_oow = fog_near_bottom.fog_oow = 1.0F;
    fog_far_top.fog_oow = fog_far_bottom.fog_oow = 1.0F / 4096.0F;
    ok = ok && backend.SetFogColor(0x00FF8000U) && backend.SetFogTable(fog) &&
        backend.SetFogMode(2U) &&
        DrawSceneQuad(backend, fog_near_top, fog_far_top, fog_far_bottom,
                      fog_near_bottom) &&
        backend.SetFogMode(0U);

    // The constant color as local, SCALE_OTHER_ADD_LOCAL with a constant
    // other, an inverted iterated color, and BLEND (7) by the local alpha.
    ok = ok && backend.SetConstantColor(0xFF40C0A0U) &&
        backend.SetColorCombine(SceneCombine(1U, 0U, 1U, 0U)) &&
        backend.DrawTriangle(SceneVertex(80.0F, 98.0F, 0xFFFFFFFFU),
                             SceneVertex(96.0F, 98.0F, 0xFFFFFFFFU),
                             SceneVertex(80.0F, 116.0F, 0xFFFFFFFFU)) &&
        backend.SetColorCombine(SceneCombine(4U, 1U, 0U, 2U)) &&
        backend.DrawTriangle(SceneVertex(98.0F, 98.0F, 0x804020FFU),
                             SceneVertex(114.0F, 98.0F, 0x204080FFU),
                             SceneVertex(98.0F, 116.0F, 0x408020FFU)) &&
        backend.SetColorCombine(SceneCombine(1U, 0U, 0U, 0U, true)) &&
        backend.DrawTriangle(SceneVertex(116.0F, 98.0F, 0xFF8000FFU),
                             SceneVertex(132.0F, 98.0F, 0x00FF80FFU),
                             SceneVertex(116.0F, 116.0F, 0x8000FFFFU)) &&
        backend.SetColorCombine(SceneCombine(7U, 3U, 0U, 2U)) &&
        backend.DrawTriangle(SceneVertex(134.0F, 98.0F, 0xFF000000U),
                             SceneVertex(156.0F, 98.0F, 0x00FF0080U),
                             SceneVertex(134.0F, 116.0F, 0x0000FFFFU)) &&
        backend.SetColorCombine(iterated);

    // Points, lines, a convex polygon and a batch.
    const GlideDrawVertex polygon[5] = {
        SceneVertex(60.0F, 74.0F, 0xFFFF00FFU),
        SceneVertex(74.0F, 78.0F, 0xFF00FFFFU),
        SceneVertex(72.0F, 92.0F, 0x00FFFFFFU),
        SceneVertex(58.0F, 94.0F, 0xFFFFFFFFU),
        SceneVertex(52.0F, 84.0F, 0x808080FFU)};
    const GlideDrawVertex batch[6] = {
        SceneVertex(78.0F, 74.0F, 0xFF4040FFU),
        SceneVertex(96.0F, 74.0F, 0xFF4040FFU),
        SceneVertex(78.0F, 84.0F, 0xFF4040FFU),
        SceneVertex(80.0F, 86.0F, 0x40FF40FFU),
        SceneVertex(96.0F, 86.0F, 0x40FF40FFU),
        SceneVertex(96.0F, 96.0F, 0x40FF40FFU)};
    ok = ok && backend.DrawPolygon(polygon, 5U) &&
        backend.DrawPrimitiveBatch(batch, 6U, GlideBatchPrimitive::kTriangles);
    for (std::uint32_t index = 0U; ok && index < 8U; ++index)
    {
        const float x = 4.0F + static_cast<float>(index) * 5.0F;
        ok = backend.DrawPoint(SceneVertex(x + 0.5F, 50.5F, 0xFFFFFFFFU)) &&
            backend.DrawLine(SceneVertex(x, 40.0F, 0xFFFF00FFU),
                             SceneVertex(x + 4.0F, 48.0F, 0x00FFFFFFU));
    }

    // Culling: of a clockwise and a counter-clockwise triangle, mode 1
    // keeps one; with the color mask off a triangle leaves no trace.
    ok = ok && backend.SetCullMode(1U) &&
        backend.DrawTriangle(SceneVertex(4.0F, 70.0F, 0xFFFFFFFFU),
                             SceneVertex(20.0F, 70.0F, 0xFFFFFFFFU),
                             SceneVertex(4.0F, 52.0F, 0xFFFFFFFFU)) &&
        backend.DrawTriangle(SceneVertex(22.0F, 52.0F, 0xFF00FFFFU),
                             SceneVertex(38.0F, 52.0F, 0xFF00FFFFU),
                             SceneVertex(22.0F, 70.0F, 0xFF00FFFFU)) &&
        backend.SetCullMode(0U) && backend.SetColorMask(false, true) &&
        backend.DrawTriangle(SceneVertex(0.0F, 0.0F, 0xFFFFFFFFU),
                             SceneVertex(160.0F, 0.0F, 0xFFFFFFFFU),
                             SceneVertex(0.0F, 120.0F, 0xFFFFFFFFU)) &&
        backend.SetColorMask(true, true);
    return ok;
}

// The LFB blit: a gradient surface, drawn over by a triangle.
bool DrawSceneLfb(repiu::engine::GlideOpenGlBackend& backend,
                  std::uint32_t width, std::uint32_t height, bool flip_v)
{
    std::vector<std::uint8_t> surface(width * height * 4U);
    for (std::uint32_t y = 0U; y < height; ++y)
    {
        for (std::uint32_t x = 0U; x < width; ++x)
        {
            std::uint8_t* const pixel = surface.data() + (y * width + x) * 4U;
            pixel[0] = static_cast<std::uint8_t>(x * 255U / width);
            pixel[1] = static_cast<std::uint8_t>(y * 255U / height);
            pixel[2] = static_cast<std::uint8_t>(y < height / 4U ? 255U : 0U);
            pixel[3] = 255U;
        }
    }
    const auto iterated = SceneCombine(1U, 0U, 0U, 0U);
    return backend.SetColorCombine(iterated) &&
        backend.SetAlphaCombine(iterated) &&
        backend.PresentLfbSurface(surface.data(), width, height, flip_v,
                                  false) &&
        backend.DrawTriangle(SceneVertex(10.0F, 10.0F, 0xFFFFFFFFU),
                             SceneVertex(50.0F, 10.0F, 0xFFFFFFFFU),
                             SceneVertex(10.0F, 40.0F, 0x000000FFU));
}

bool WriteSceneImage(const std::string& path, std::uint32_t width,
                     std::uint32_t height, const std::vector<std::uint8_t>& rgba)
{
    std::ofstream file(path, std::ios::binary);
    const std::uint32_t header[2] = {width, height};
    file.write(reinterpret_cast<const char*>(header), sizeof(header));
    file.write(reinterpret_cast<const char*>(rgba.data()),
               static_cast<std::streamsize>(rgba.size()));
    return static_cast<bool>(file);
}

bool CompareSceneImage(const std::string& name, const std::string& path,
                       std::uint32_t width, std::uint32_t height,
                       const std::vector<std::uint8_t>& rgba)
{
    std::ifstream file(path, std::ios::binary);
    std::uint32_t header[2] = {};
    file.read(reinterpret_cast<char*>(header), sizeof(header));
    std::vector<std::uint8_t> reference(rgba.size());
    file.read(reinterpret_cast<char*>(reference.data()),
              static_cast<std::streamsize>(reference.size()));
    if (!file || header[0] != width || header[1] != height)
    {
        std::cout << "glide_scene_" << name << "_reference=unreadable "
                  << path << '\n';
        return false;
    }
    std::size_t differing = 0U;
    int largest = 0;
    for (std::size_t index = 0U; index < rgba.size(); index += 4U)
    {
        int pixel_largest = 0;
        for (std::size_t channel = 0U; channel < 3U; ++channel)
        {
            pixel_largest = (std::max)(
                pixel_largest, std::abs(static_cast<int>(rgba[index + channel]) -
                                        static_cast<int>(reference[index + channel])));
        }
        differing += pixel_largest != 0 ? 1U : 0U;
        largest = (std::max)(largest, pixel_largest);
    }
    std::cout << "glide_scene_" << name << "_differing_pixels=" << differing
              << "/" << rgba.size() / 4U << " largest_delta=" << largest
              << '\n';
    return differing == 0U;
}

// Draws one scene in a fresh window and reads it back at the drawable's full
// resolution, so a scaled window compares every rendered pixel.
bool RenderScene(const char* name, std::uint32_t origin, bool lfb, bool flip_v,
                 const std::string& output_prefix,
                 const std::string& reference_prefix)
{
    constexpr std::uint32_t kWidth = 160U;
    constexpr std::uint32_t kHeight = 120U;
    repiu::engine::GlideOpenGlBackend backend;
    backend.BindHostThread();
    if (!Check(backend.OpenWindowed(kWidth, kHeight, 2U, 1U, origin),
               "scene backend did not open") ||
        !Check(!backend.is_dummy(), "scene backend fell back to the dummy"))
    {
        return false;
    }
    int drawable_width = 0;
    int drawable_height = 0;
    SDL_Window* const window = SDL_GL_GetCurrentWindow();
    if (!Check(window != nullptr &&
                   SDL_GetWindowSizeInPixels(window, &drawable_width,
                                             &drawable_height) &&
                   drawable_width > 0 && drawable_height > 0,
               "scene drawable size unavailable"))
    {
        backend.Close();
        return false;
    }
    const bool drawn = lfb ? DrawSceneLfb(backend, kWidth, kHeight, flip_v)
                           : DrawSceneGeometry(backend);
    std::vector<std::uint8_t> rgba;
    const auto width = static_cast<std::uint32_t>(drawable_width);
    const auto height = static_cast<std::uint32_t>(drawable_height);
    const bool read = drawn && backend.ReadbackFramebuffer(width, height, &rgba);
    backend.Close();
    if (!Check(drawn, "scene draw failed") ||
        !Check(read, "scene readback failed"))
    {
        return false;
    }
    std::uint64_t hash = 14695981039346656037ULL;
    for (const std::uint8_t value : rgba)
    {
        hash = (hash ^ value) * 1099511628211ULL;
    }
    std::cout << "glide_scene_" << name << "=" << width << "x" << height
              << " fnv1a=" << std::hex << hash << std::dec << '\n';
    const std::string suffix = std::string(".") + name + ".rgba";
    if (!Check(WriteSceneImage(output_prefix + suffix, width, height, rgba),
               "scene image write failed"))
    {
        return false;
    }
    return reference_prefix.empty() ||
        CompareSceneImage(name, reference_prefix + suffix, width, height, rgba);
}

bool RunOpenGlSceneProbe(const std::string& output_prefix,
                         const std::string& reference_prefix)
{
    const bool upper_left =
        RenderScene("upper_left", 0U, false, false, output_prefix,
                    reference_prefix);
    const bool lower_left =
        RenderScene("lower_left", 1U, false, false, output_prefix,
                    reference_prefix);
    const bool lfb = RenderScene("lfb", 0U, true, false, output_prefix,
                                 reference_prefix);
    const bool lfb_flipped = RenderScene("lfb_flipped", 0U, true, true,
                                         output_prefix, reference_prefix);
    return Check(upper_left && lower_left && lfb && lfb_flipped,
                 "scene differs from the reference");
}
#endif

}  // namespace

int main(int argc, char** argv)
{
    float producer_fields[repiu::hle::kGlideProducerVertexDwordCount] = {};
    producer_fields[0] = 10.0F;
    producer_fields[1] = 20.0F;
    producer_fields[3] = 255.0F;
    producer_fields[4] = 127.5F;
    producer_fields[5] = 63.75F;
    producer_fields[7] = 31.875F;
    producer_fields[8] = 0.5F;
    producer_fields[9] = 128.0F;
    producer_fields[10] = 64.0F;
    std::array<std::uint32_t,
               repiu::hle::kGlideProducerVertexDwordCount> producer{};
    std::memcpy(producer.data(), producer_fields, sizeof(producer_fields));
    repiu::hle::GlideDrawVertex vertex;
    if (!Check(repiu::hle::DecodeGlideProducerVertex(
                   producer.data(), producer.size(), &vertex),
               "producer vertex decode failed") ||
        !Check(vertex.x == 10.0F && vertex.y == 20.0F,
               "producer position decode failed") ||
        !Check(std::abs(vertex.r - 1.0F) < 0.0001F &&
                   std::abs(vertex.g - 0.5F) < 0.0001F &&
                   std::abs(vertex.b - 0.25F) < 0.0001F &&
                   std::abs(vertex.a - 0.125F) < 0.0001F,
               "producer color decode failed") ||
        !Check(vertex.s == 128.0F && vertex.t == 64.0F &&
                   vertex.fog_oow == 0.5F && vertex.texture_oow == 0.5F,
               "producer texture and oow decode failed") ||
        !Check(!repiu::hle::DecodeGlideProducerVertex(
                    producer.data(), producer.size() - 1U, &vertex),
               "short producer vertex was accepted"))
    {
        return 1;
    }

    const std::uint8_t rgb565_red[] = {0x00U, 0xF8U};
    const std::uint8_t rgb565_blue[] = {0x1FU, 0x00U};
    std::vector<std::uint8_t> rgba8;
    if (!Check(repiu::hle::DecodeGlideLfb565ToRgba8(
                   rgb565_red, sizeof(rgb565_red), 1U, 1U,
                   repiu::hle::kGlideColorFormatArgb, &rgba8),
               "RGB565 red decode failed") ||
        !Check(rgba8[0] == 255U && rgba8[1] == 0U && rgba8[2] == 0U,
               "RGB565 red selected the wrong channel") ||
        !Check(repiu::hle::DecodeGlideLfb565ToRgba8(
                   rgb565_red, sizeof(rgb565_red), 1U, 1U,
                   repiu::hle::kGlideColorFormatAbgr, &rgba8),
               "BGR565 blue decode failed") ||
        !Check(rgba8[0] == 0U && rgba8[1] == 0U && rgba8[2] == 255U,
               "BGR565 high channel was not blue") ||
        !Check(repiu::hle::DecodeGlideLfb565ToRgba8(
                   rgb565_blue, sizeof(rgb565_blue), 1U, 1U,
                   repiu::hle::kGlideColorFormatAbgr, &rgba8),
               "BGR565 red decode failed") ||
        !Check(rgba8[0] == 255U && rgba8[1] == 0U && rgba8[2] == 0U,
               "BGR565 low channel was not red"))
    {
        return 1;
    }

    const std::uint8_t cyan[] = {0U, 255U, 255U, 255U};
    std::uint8_t encoded[2] = {};
    if (!Check(repiu::hle::EncodeRgba8ToGlideLfb565(
                   cyan, sizeof(cyan), 1U, 1U,
                   repiu::hle::kGlideColorFormatAbgr, encoded,
                   sizeof(encoded)),
               "BGR565 cyan encode failed") ||
        !Check(encoded[0] == 0xE0U && encoded[1] == 0xFFU,
               "BGR565 cyan packing is incorrect") ||
        !Check(repiu::hle::DecodeGlideLfb565ToRgba8(
                   encoded, sizeof(encoded), 1U, 1U,
                   repiu::hle::kGlideColorFormatAbgr, &rgba8),
               "BGR565 cyan round trip failed") ||
        !Check(rgba8[0] == 0U && rgba8[1] == 255U && rgba8[2] == 255U,
               "BGR565 cyan round trip changed channels"))
    {
        return 1;
    }

    repiu::hle::GlideFogTable table{};
    for (std::uint32_t index = 0U; index < table.size(); ++index)
    {
        table[index] = static_cast<std::uint8_t>(index * 4U);
        std::uint32_t lower_index = 0U;
        float fraction = 1.0F;
        const float oow = 1.0F / TableWorldDistance(index);
        if (!Check(repiu::hle::CalculateGlideFogTableSample(
                       oow, &lower_index, &fraction),
                   "table knot was rejected") ||
            !Check(lower_index == index ||
                       (index > 0U && lower_index == index - 1U &&
                        std::abs(fraction - 1.0F) < 0.0001F),
                   "table knot selected the wrong interval"))
        {
            return 1;
        }
    }

    const float lower_distance = TableWorldDistance(20U);
    const float upper_distance = TableWorldDistance(21U);
    const float midpoint = (lower_distance + upper_distance) * 0.5F;
    std::uint32_t lower_index = 0U;
    float fraction = 0.0F;
    if (!Check(repiu::hle::CalculateGlideFogTableSample(
                   1.0F / midpoint, &lower_index, &fraction),
               "midpoint was rejected") ||
        !Check(lower_index == 20U, "midpoint lower index is incorrect") ||
        !Check(std::abs(fraction - 0.5F) < 0.0001F,
               "midpoint interpolation is incorrect") ||
        !Check(std::abs(repiu::hle::EvaluateGlideFogTable(
                           table, 1.0F / midpoint) -
                       (static_cast<float>(table[20]) +
                        static_cast<float>(table[21])) /
                           (2.0F * 255.0F)) <
                   0.0001F,
               "table factor interpolation is incorrect"))
    {
        return 1;
    }

    if (!Check(repiu::hle::EvaluateGlideFogTable(table, 2.0F) ==
                   static_cast<float>(table[0]) / 255.0F,
               "near distance did not clamp to entry 0") ||
        !Check(repiu::hle::EvaluateGlideFogTable(table, 0.000001F) ==
                   static_cast<float>(table[63]) / 255.0F,
               "far distance did not clamp to entry 63") ||
        !Check(repiu::hle::EvaluateGlideFogTable(table, 0.0F) == 0.0F,
               "invalid oow did not use the safe no-fog value"))
    {
        return 1;
    }

#if defined(_WIN32)
    using repiu::engine::GlideOpenGlCullFace;
    const auto point_size = [](const std::uint32_t drawable_width,
                               const std::uint32_t drawable_height) {
        return repiu::engine::CalculateGlidePointSize(
            640U, 480U, drawable_width, drawable_height);
    };
    if (!Check(point_size(640U, 480U) == 1.0F,
               "1x point scale is incorrect") ||
        !Check(point_size(1280U, 960U) == 2.0F,
               "2x point scale is incorrect") ||
        !Check(point_size(1920U, 1440U) == 3.0F,
               "3x point scale is incorrect") ||
        !Check(point_size(1280U, 720U) == 1.5F,
               "non-uniform point scale did not preserve square points") ||
        !Check(point_size(320U, 240U) == 1.0F,
               "downscaled point size fell below one pixel") ||
        !Check(repiu::engine::CalculateGlidePointSize(
                   0U, 480U, 1280U, 960U) == 1.0F,
               "invalid logical size did not use one pixel"))
    {
        return 1;
    }

    GlideOpenGlCullFace cull_face = GlideOpenGlCullFace::kDisabled;
    if (!Check(repiu::engine::TranslateGlideOpenGlCullMode(
                   0U, false, &cull_face) &&
                   cull_face == GlideOpenGlCullFace::kDisabled,
               "cull disable translation failed") ||
        !Check(repiu::engine::TranslateGlideOpenGlCullMode(
                   1U, true, &cull_face) &&
                   cull_face == GlideOpenGlCullFace::kBack,
               "lower-left negative cull translation failed") ||
        !Check(repiu::engine::TranslateGlideOpenGlCullMode(
                   2U, true, &cull_face) &&
                   cull_face == GlideOpenGlCullFace::kFront,
               "lower-left positive cull translation failed") ||
        !Check(repiu::engine::TranslateGlideOpenGlCullMode(
                   1U, false, &cull_face) &&
                   cull_face == GlideOpenGlCullFace::kFront,
               "upper-left negative cull translation failed") ||
        !Check(repiu::engine::TranslateGlideOpenGlCullMode(
                   2U, false, &cull_face) &&
                   cull_face == GlideOpenGlCullFace::kBack,
               "upper-left positive cull translation failed") ||
        !Check(!repiu::engine::TranslateGlideOpenGlCullMode(
                    3U, false, &cull_face),
               "invalid cull mode was accepted"))
    {
        return 1;
    }

    // Issue #69: --opengl-scene <output-prefix> [<reference-prefix>].
    if (argc >= 3 && std::strcmp(argv[1], "--opengl-scene") == 0)
    {
        if (!RunOpenGlSceneProbe(argv[2], argc >= 4 ? argv[3] : ""))
        {
            return 1;
        }
    }
    if (argc == 2 && std::strcmp(argv[1], "--opengl-post-shader") == 0)
    {
        if (!RunOpenGlPostShaderProbe())
        {
            return 1;
        }
    }
    if (argc == 2 && std::strcmp(argv[1], "--opengl-lfb") == 0)
    {
        constexpr std::uint32_t kWidth = 64U;
        constexpr std::uint32_t kHeight = 48U;
        std::vector<std::uint8_t> source(kWidth * kHeight * 4U, 0U);
        for (std::size_t index = 0U; index < source.size(); index += 4U)
        {
            source[index] = 240U;
            source[index + 1U] = 32U;
            source[index + 2U] = 16U;
            source[index + 3U] = 255U;
        }
        repiu::engine::GlideOpenGlBackend backend;
        backend.BindHostThread();
        if (!Check(backend.OpenWindowed(kWidth, kHeight, 2U, 1U, 1U),
                   "OpenGL backend did not open") ||
            !Check(backend.PresentLfbSurface(source.data(), kWidth, kHeight,
                                             false, false),
                   "LFB surface presentation failed"))
        {
            return 1;
        }
        std::vector<std::uint8_t> result;
        if (!Check(backend.ReadbackFramebuffer(kWidth, kHeight, &result),
                   "LFB framebuffer readback failed"))
        {
            return 1;
        }
        std::size_t red_pixels = 0U;
        for (std::size_t index = 0U; index + 3U < result.size(); index += 4U)
        {
            if (result[index] > 200U && result[index + 1U] < 64U &&
                result[index + 2U] < 64U)
            {
                ++red_pixels;
            }
        }
        if (!Check(red_pixels > kWidth * kHeight * 9U / 10U,
                   "LFB blit did not reach the back buffer"))
        {
            return 1;
        }
    }
#else
    (void)argc;
    (void)argv;
#endif
    std::cout << "glide_render_probe=pass\n";
    return 0;
}
