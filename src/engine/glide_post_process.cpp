#include "repiu/engine/glide_post_process.h"

#include "repiu/engine/gl_api.h"
#include "repiu/engine/gl_shader_dialect.h"
#include "repiu/engine/gl_unit_quad.h"
#include "repiu/engine/post_shader_preset.h"

#include <SDL3/SDL.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <filesystem>

namespace repiu::engine
{

struct GlidePostProcess::Implementation
{
    // One pass of the chain: its program, uniform slots, target and settings
    // (issue #90).
    struct Pass
    {
        PostShaderPass settings;
        GLuint program = 0;
        GLint mvp_matrix = -1;
        GLint texture = -1;
        GLint input_size = -1;
        GLint texture_size = -1;
        GLint output_size = -1;
        GLint frame_count = -1;
        GLint frame_direction = -1;
        GLint orig_texture = -1;
        GLint orig_texture_size = -1;
        GLint orig_input_size = -1;
        // PassPrev1 to PassPrev(index + 1): texture, its two sizes.
        std::vector<std::array<GLint, 3>> previous;
        // One per entry of `parameters`, -1 where the pass has no such uniform.
        std::vector<GLint> parameter_locations;
        // The target a pass not drawn into the window draws into.
        GLuint framebuffer = 0;
        GLuint color = 0;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
    };

    GlApi gl;
    GlUnitQuad quad;

    bool initialized = false;
    std::filesystem::path directory;
    std::string directory_text;
    std::vector<PostShaderEntry> catalog;
    std::string active_id = kPostShaderNoneId;
    std::string last_error;
    std::vector<PostShaderParameter> parameters;
    std::vector<Pass> passes;
    // Draws the last pass's output into the window when that pass has a size
    // of its own.
    Pass stock;

    std::uint32_t frames = 0;
};

namespace
{

using Pass = GlidePostProcess::Implementation::Pass;

// The units a pass's inputs go on: Texture, OrigTexture, then PassPrevN.
constexpr GLint kTextureUnit = 0;
constexpr GLint kOrigUnit = 1;
constexpr GLint kFirstPreviousUnit = 2;

// Orthographic projection of the unit square onto clip space, column-major.
constexpr GLfloat kUnitSquareProjection[16] = {
    2.0F, 0.0F, 0.0F, 0.0F,
    0.0F, 2.0F, 0.0F, 0.0F,
    0.0F, 0.0F, -1.0F, 0.0F,
    -1.0F, -1.0F, 0.0F, 1.0F,
};

// The pass-through program for a chain whose last pass has its own size.
constexpr const char* kStockShader = R"glsl(
#if defined(VERTEX)
attribute vec4 VertexCoord;
attribute vec4 TexCoord;
uniform mat4 MVPMatrix;
varying vec2 tex_coord;
void main()
{
    gl_Position = MVPMatrix * VertexCoord;
    tex_coord = TexCoord.xy;
}
#elif defined(FRAGMENT)
uniform sampler2D Texture;
varying vec2 tex_coord;
void main()
{
    gl_FragColor = vec4(texture2D(Texture, tex_coord).rgb, 1.0);
}
#endif
)glsl";

// Compiles one pass's text into a program and finds its uniforms. `index` is
// its place in the chain, which bounds the PassPrevN it can read. Parameters
// are shared by name across the chain: the first pass to declare one gives
// its range.
bool BuildPass(const GlApi& gl, std::size_t index, Pass* pass,
               std::vector<PostShaderParameter>* parameters,
               std::string* error)
{
    PostShaderProgramSource source;
    if (!BuildPostShaderProgramSource(pass->settings.text, &source, error))
    {
        *error = pass->settings.path + ": " + *error;
        return false;
    }
    // Issue #69: the shaders are written in the GLSL 1.10 style of
    // RetroArch's compatibility contexts; carry them to the context's
    // dialect, binding the shared vertex layout's slots by their names.
    const std::string vertex =
        AdaptLegacyGlslStage(source.vertex, gl.es, GlslStage::kVertex);
    const std::string fragment =
        AdaptLegacyGlslStage(source.fragment, gl.es, GlslStage::kFragment);
    pass->program = BuildGlProgram(gl, vertex.c_str(), fragment.c_str(),
                                   "VertexCoord", "COLOR", "TexCoord", error);
    if (pass->program == 0U)
    {
        *error = pass->settings.path + ": " + *error;
        return false;
    }
    const auto uniform = [&gl, pass](const std::string& name)
    { return gl.get_uniform_location(pass->program, name.c_str()); };
    pass->mvp_matrix = uniform("MVPMatrix");
    pass->texture = uniform("Texture");
    pass->input_size = uniform("InputSize");
    pass->texture_size = uniform("TextureSize");
    pass->output_size = uniform("OutputSize");
    pass->frame_count = uniform("FrameCount");
    pass->frame_direction = uniform("FrameDirection");
    pass->orig_texture = uniform("OrigTexture");
    pass->orig_texture_size = uniform("OrigTextureSize");
    pass->orig_input_size = uniform("OrigInputSize");
    for (std::size_t back = 1; back <= index + 1U; ++back)
    {
        const std::string prefix = "PassPrev" + std::to_string(back);
        pass->previous.push_back({uniform(prefix + "Texture"),
                                  uniform(prefix + "TextureSize"),
                                  uniform(prefix + "InputSize")});
    }
    for (PostShaderParameter& parameter : source.parameters)
    {
        bool known = false;
        for (const PostShaderParameter& existing : *parameters)
        {
            known = known || existing.name == parameter.name;
        }
        if (!known)
        {
            parameters->push_back(std::move(parameter));
        }
    }
    return true;
}

void ReleasePass(const GlApi& gl, Pass* pass)
{
    if (pass->program != 0U)
    {
        gl.delete_program(pass->program);
    }
    if (pass->framebuffer != 0U)
    {
        gl.delete_framebuffers(1, &pass->framebuffer);
    }
    if (pass->color != 0U)
    {
        glDeleteTextures(1, &pass->color);
    }
    *pass = Pass{};
}

void ReleaseProgram(GlidePostProcess::Implementation* state)
{
    for (Pass& pass : state->passes)
    {
        ReleasePass(state->gl, &pass);
    }
    state->passes.clear();
    ReleasePass(state->gl, &state->stock);
    state->parameters.clear();
    state->active_id = kPostShaderNoneId;
}

// Sets the filter of the texture bound on the active unit.
void SetFilter(GLuint texture, bool linear)
{
    const GLint filter = linear ? GL_LINEAR : GL_NEAREST;
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
}

// Gives a pass a target of width by height, made again when the size changes.
// Leaves the target's framebuffer bound on success.
bool EnsureTarget(const GlApi& gl, Pass* pass, std::uint32_t width,
                  std::uint32_t height, std::string* error)
{
    if (pass->framebuffer != 0U && pass->width == width &&
        pass->height == height)
    {
        return true;
    }
    if (pass->color == 0U)
    {
        glGenTextures(1, &pass->color);
    }
    if (pass->framebuffer == 0U)
    {
        gl.gen_framebuffers(1, &pass->framebuffer);
    }
    gl.active_texture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, pass->color);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    // Sized formats, as both desktop core and ES 3.0 take them; a
    // floating-point target is colour-renderable in ES only with
    // EXT_color_buffer_float, which the completeness check below tells.
    glTexImage2D(GL_TEXTURE_2D, 0,
                 pass->settings.float_framebuffer ? GL_RGBA16F : GL_RGBA8,
                 static_cast<GLsizei>(width), static_cast<GLsizei>(height), 0,
                 GL_RGBA,
                 pass->settings.float_framebuffer ? GL_FLOAT
                                                  : GL_UNSIGNED_BYTE,
                 nullptr);
    gl.bind_framebuffer(GL_FRAMEBUFFER, pass->framebuffer);
    gl.framebuffer_texture_2d(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                              GL_TEXTURE_2D, pass->color, 0);
    if (gl.check_framebuffer_status(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
    {
        *error = pass->settings.path + ": cannot draw into a " +
            std::to_string(width) + "x" + std::to_string(height) +
            (pass->settings.float_framebuffer ? " floating-point" : "") +
            " target";
        pass->width = 0;
        pass->height = 0;
        return false;
    }
    pass->width = width;
    pass->height = height;
    return true;
}

// Makes a pass current and uploads its uniforms. Its inputs are already bound
// on their units.
void SetUniforms(GlidePostProcess::Implementation* state, const Pass& pass,
                 std::uint32_t input_width, std::uint32_t input_height,
                 std::uint32_t output_width, std::uint32_t output_height,
                 std::uint32_t orig_width, std::uint32_t orig_height,
                 const std::vector<std::array<std::uint32_t, 2>>&
                     previous_sizes)
{
    const GlApi& gl = state->gl;
    gl.use_program(pass.program);
    if (pass.mvp_matrix >= 0)
    {
        gl.uniform_matrix_4fv(pass.mvp_matrix, 1, GL_FALSE,
                              kUnitSquareProjection);
    }
    const auto size = [&gl](GLint location, std::uint32_t width,
                            std::uint32_t height)
    {
        if (location >= 0)
        {
            gl.uniform_2f(location, static_cast<GLfloat>(width),
                          static_cast<GLfloat>(height));
        }
    };
    if (pass.texture >= 0)
    {
        gl.uniform_1i(pass.texture, kTextureUnit);
    }
    // Every texture here is exactly its content's size, so the texture size
    // and the content size are the same, as libretro means them. For the
    // first pass both are the game's logical resolution (Task 768).
    size(pass.input_size, input_width, input_height);
    size(pass.texture_size, input_width, input_height);
    size(pass.output_size, output_width, output_height);
    if (pass.orig_texture >= 0)
    {
        gl.uniform_1i(pass.orig_texture, kOrigUnit);
    }
    size(pass.orig_texture_size, orig_width, orig_height);
    size(pass.orig_input_size, orig_width, orig_height);
    for (std::size_t back = 0;
         back < pass.previous.size() && back < previous_sizes.size(); ++back)
    {
        if (pass.previous[back][0] >= 0)
        {
            gl.uniform_1i(pass.previous[back][0],
                          kFirstPreviousUnit + static_cast<GLint>(back));
        }
        size(pass.previous[back][1], previous_sizes[back][0],
             previous_sizes[back][1]);
        size(pass.previous[back][2], previous_sizes[back][0],
             previous_sizes[back][1]);
    }
    if (pass.frame_count >= 0)
    {
        gl.uniform_1i(pass.frame_count, static_cast<GLint>(state->frames));
    }
    if (pass.frame_direction >= 0)
    {
        gl.uniform_1i(pass.frame_direction, 1);
    }
    for (std::size_t index = 0; index < pass.parameter_locations.size() &&
         index < state->parameters.size();
         ++index)
    {
        if (pass.parameter_locations[index] >= 0)
        {
            gl.uniform_1f(pass.parameter_locations[index],
                          state->parameters[index].value);
        }
    }
}

std::filesystem::path ExecutableDirectory()
{
    const char* base = SDL_GetBasePath();
    return base != nullptr ? std::filesystem::path(base)
                           : std::filesystem::path();
}

}  // namespace

GlidePostProcess::GlidePostProcess()
    : implementation_(std::make_unique<Implementation>())
{
}

GlidePostProcess::~GlidePostProcess() = default;

bool GlidePostProcess::Initialize(std::string* message)
{
    Implementation* state = implementation_.get();
    if (state->initialized)
    {
        return true;
    }
    std::string missing;
    if (!LoadGlApi(&state->gl, &missing) || !state->quad.Initialize(&state->gl))
    {
        if (message != nullptr)
        {
            *message = missing.empty()
                ? "OpenGL vertex arrays are unavailable"
                : "OpenGL entry point unavailable: " + missing;
        }
        return false;
    }
    state->directory = ResolvePostShaderDirectory(ExecutableDirectory());
    state->directory_text = state->directory.string();
    state->catalog = ListPostShaders(state->directory);
    state->initialized = true;
    return true;
}

void GlidePostProcess::Shutdown()
{
    Implementation* state = implementation_.get();
    if (!state->initialized)
    {
        return;
    }
    ReleaseProgram(state);
    state->quad.Shutdown();
    state->initialized = false;
}

bool GlidePostProcess::initialized() const
{
    return implementation_->initialized;
}

const std::vector<PostShaderEntry>& GlidePostProcess::catalog() const
{
    return implementation_->catalog;
}

const std::string& GlidePostProcess::shader_directory() const
{
    return implementation_->directory_text;
}

const std::string& GlidePostProcess::active_id() const
{
    return implementation_->active_id;
}

bool GlidePostProcess::active() const
{
    return !implementation_->passes.empty();
}

std::size_t GlidePostProcess::pass_count() const
{
    return implementation_->passes.size();
}

const std::string& GlidePostProcess::last_error() const
{
    return implementation_->last_error;
}

std::vector<PostShaderParameter>& GlidePostProcess::parameters()
{
    return implementation_->parameters;
}

bool GlidePostProcess::Select(std::string_view id)
{
    Implementation* state = implementation_.get();
    state->last_error.clear();
    if (!state->initialized)
    {
        state->last_error = "post-processing is not initialized";
        return false;
    }
    ReleaseProgram(state);
    if (id.empty() || id == kPostShaderNoneId)
    {
        std::fprintf(stderr, "[repiu-post] shader: none\n");
        return true;
    }

    const PostShaderEntry* entry = FindPostShader(state->catalog, id);
    PostShaderPreset preset;
    bool built = entry != nullptr;
    if (entry == nullptr)
    {
        state->last_error = "no shader named '" + std::string(id) + "' in " +
            state->directory_text + " or built in";
    }
    else
    {
        built = LoadPostShaderPreset(*entry, &preset, &state->last_error);
    }
    std::vector<Pass> passes(built ? preset.passes.size() : 0U);
    std::vector<PostShaderParameter> parameters;
    for (std::size_t index = 0; built && index < passes.size(); ++index)
    {
        passes[index].settings = std::move(preset.passes[index]);
        built = BuildPass(state->gl, index, &passes[index], &parameters,
                          &state->last_error);
    }
    if (built && !passes.back().settings.to_window)
    {
        state->stock.settings =
            SinglePassPreset("stock", kStockShader).passes.front();
        built = BuildPass(state->gl, passes.size(), &state->stock, &parameters,
                          &state->last_error);
    }
    state->passes = std::move(passes);
    if (!built)
    {
        const std::string error = state->last_error;
        ReleaseProgram(state);
        state->last_error = error;
        std::fprintf(stderr, "[repiu-post] shader '%.*s' not applied: %s\n",
                     static_cast<int>(id.size()), id.data(),
                     state->last_error.c_str());
        return false;
    }

    const GlApi& gl = state->gl;
    state->parameters = std::move(parameters);
    const auto locate = [state, &gl](Pass* pass)
    {
        if (pass->program == 0U)
        {
            return;
        }
        for (const PostShaderParameter& parameter : state->parameters)
        {
            pass->parameter_locations.push_back(
                gl.get_uniform_location(pass->program, parameter.name.c_str()));
        }
    };
    for (Pass& pass : state->passes)
    {
        locate(&pass);
    }
    locate(&state->stock);
    state->active_id = std::string(id);
    state->frames = 0;
    std::fprintf(stderr, "[repiu-post] shader: %s (%zu passes, %zu parameters)\n",
                 state->active_id.c_str(), state->passes.size(),
                 state->parameters.size());
    return true;
}

bool GlidePostProcess::Reload()
{
    Implementation* state = implementation_.get();
    if (!state->initialized)
    {
        return false;
    }
    const std::string id = state->active_id;
    state->catalog = ListPostShaders(state->directory);
    return Select(id);
}

bool GlidePostProcess::Apply(unsigned int input_texture, std::uint32_t x,
                             std::uint32_t y, std::uint32_t width,
                             std::uint32_t height,
                             std::uint32_t logical_width,
                             std::uint32_t logical_height)
{
    Implementation* state = implementation_.get();
    if (state->passes.empty() || input_texture == 0U || width == 0U ||
        height == 0U)
    {
        return false;
    }
    if (logical_width == 0U || logical_height == 0U)
    {
        logical_width = width;
        logical_height = height;
    }
    const GlApi& gl = state->gl;

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);

    // Draws the current program's quad into the window's content rect. Alpha
    // is left as it was, so no translucency reaches a compositor through the
    // window's alpha channel.
    const auto draw_to_window = [&]()
    {
        gl.bind_framebuffer(GL_FRAMEBUFFER, 0U);
        glViewport(static_cast<GLint>(x), static_cast<GLint>(y),
                   static_cast<GLsizei>(width), static_cast<GLsizei>(height));
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);
        // The unit square; texture row 0 is the bottom scanline.
        state->quad.Draw();
        ++state->frames;
    };

    // A single pass into the window: Task 768's path. Its picture and
    // PassPrev1 name that same texture, so they read unit 0 rather than
    // units nothing is bound on.
    if (state->passes.size() == 1U && state->passes.front().settings.to_window)
    {
        const Pass& pass = state->passes.front();
        gl.active_texture(GL_TEXTURE0);
        // The shaders sample between the picture's pixels (the crt warp), so
        // the input is filtered unless the preset says otherwise; the present
        // pass sets its own filter each frame.
        SetFilter(input_texture,
                  !pass.settings.filter_specified || pass.settings.filter_linear);
        SetUniforms(state, pass, logical_width, logical_height, width, height,
                    logical_width, logical_height,
                    {{logical_width, logical_height}});
        if (pass.orig_texture >= 0)
        {
            gl.uniform_1i(pass.orig_texture, kTextureUnit);
        }
        if (!pass.previous.empty() && pass.previous.front()[0] >= 0)
        {
            gl.uniform_1i(pass.previous.front()[0], kTextureUnit);
        }
        draw_to_window();
        return true;
    }

    // Each pass's output, newest last, beginning with the game's picture.
    std::vector<GLuint> outputs = {static_cast<GLuint>(input_texture)};
    std::vector<std::array<std::uint32_t, 2>> sizes = {
        {logical_width, logical_height}};
    // OrigTexture is read with the first pass's filter: it is the same
    // texture object the first pass reads as Texture.
    gl.active_texture(GL_TEXTURE0 + kOrigUnit);
    SetFilter(outputs.front(), state->passes.front().settings.filter_linear);
    std::string error;
    const std::size_t count = state->passes.size();
    for (std::size_t index = 0; index <= count; ++index)
    {
        const bool stock = index == count;
        if (stock && state->stock.program == 0U)
        {
            break;
        }
        Pass& pass = stock ? state->stock : state->passes[index];
        const std::uint32_t pass_input_width = sizes.back()[0];
        const std::uint32_t pass_input_height = sizes.back()[1];
        std::uint32_t pass_width = width;
        std::uint32_t pass_height = height;
        const bool to_window = stock || pass.settings.to_window;
        if (!to_window)
        {
            PostShaderPassSize(pass.settings, pass_input_width,
                               pass_input_height, width, height, &pass_width,
                               &pass_height);
            if (!EnsureTarget(gl, &pass, pass_width, pass_height, &error))
            {
                // Back to the window target with the picture on unit 0 and
                // no GL error left for the backend's own check to trip on;
                // the caller draws the picture plainly from here on.
                gl.bind_framebuffer(GL_FRAMEBUFFER, 0U);
                gl.active_texture(GL_TEXTURE0);
                glBindTexture(GL_TEXTURE_2D, input_texture);
                ReleaseProgram(state);
                gl.use_program(0U);
                while (glGetError() != GL_NO_ERROR)
                {
                }
                state->last_error = error;
                std::fprintf(stderr, "[repiu-post] shader dropped: %s\n",
                             error.c_str());
                return false;
            }
        }
        // Inputs: PassPrevN on units 2 and up (back == outputs.size() is the
        // picture itself, PassPrev(i + 1)), OrigTexture on unit 1, Texture on
        // unit 0 with this pass's filter.
        for (std::size_t back = 1;
             back <= outputs.size() && back <= pass.previous.size(); ++back)
        {
            if (pass.previous[back - 1U][0] >= 0)
            {
                gl.active_texture(GL_TEXTURE0 + kFirstPreviousUnit +
                                  static_cast<GLenum>(back - 1U));
                glBindTexture(GL_TEXTURE_2D, outputs[outputs.size() - back]);
            }
        }
        gl.active_texture(GL_TEXTURE0 + kOrigUnit);
        glBindTexture(GL_TEXTURE_2D, outputs.front());
        gl.active_texture(GL_TEXTURE0);
        SetFilter(outputs.back(), stock || pass.settings.filter_linear);
        // PassPrevN of pass i: N = 1 is its input, N = i + 1 the picture.
        std::vector<std::array<std::uint32_t, 2>> previous_sizes;
        for (std::size_t back = 1; back <= sizes.size(); ++back)
        {
            previous_sizes.push_back(sizes[sizes.size() - back]);
        }
        SetUniforms(state, pass, pass_input_width, pass_input_height,
                    pass_width, pass_height, logical_width, logical_height,
                    previous_sizes);
        if (to_window)
        {
            draw_to_window();
            return true;
        }
        // Into the pass's own target, every channel: a pass's output may be
        // four data channels rather than a colour.
        gl.bind_framebuffer(GL_FRAMEBUFFER, pass.framebuffer);
        glViewport(0, 0, static_cast<GLsizei>(pass_width),
                   static_cast<GLsizei>(pass_height));
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        state->quad.Draw();
        outputs.push_back(pass.color);
        sizes.push_back({pass_width, pass_height});
    }
    // Not reached: a chain's last pass draws into the window or the stock pass
    // does. Leave the window bound all the same.
    gl.bind_framebuffer(GL_FRAMEBUFFER, 0U);
    return false;
}

}  // namespace repiu::engine
