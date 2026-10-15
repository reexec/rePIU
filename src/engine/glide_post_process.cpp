#include "repiu/engine/glide_post_process.h"

#include "repiu/engine/gl_api.h"
#include "repiu/engine/gl_shader_dialect.h"
#include "repiu/engine/gl_unit_quad.h"

#include <SDL3/SDL.h>

#include <cstdint>
#include <cstdio>
#include <filesystem>

namespace repiu::engine
{

struct GlidePostProcess::Implementation
{
    GlApi gl;
    GlUnitQuad quad;

    bool initialized = false;
    std::filesystem::path directory;
    std::string directory_text;
    std::vector<PostShaderEntry> catalog;
    std::string active_id = kPostShaderNoneId;
    std::string last_error;
    std::vector<PostShaderParameter> parameters;
    std::vector<GLint> parameter_locations;

    GLuint program = 0;
    GLint mvp_matrix = -1;
    GLint texture = -1;
    GLint input_size = -1;
    GLint texture_size = -1;
    GLint output_size = -1;
    GLint frame_count = -1;
    GLint frame_direction = -1;

    std::uint32_t frames = 0;
};

namespace
{

// Orthographic projection of the unit square onto clip space, column-major.
constexpr GLfloat kUnitSquareProjection[16] = {
    2.0F, 0.0F, 0.0F, 0.0F,
    0.0F, 2.0F, 0.0F, 0.0F,
    0.0F, 0.0F, -1.0F, 0.0F,
    -1.0F, -1.0F, 0.0F, 1.0F,
};

void ReleaseProgram(GlidePostProcess::Implementation* state)
{
    if (state->program != 0U)
    {
        state->gl.delete_program(state->program);
        state->program = 0;
    }
    state->parameters.clear();
    state->parameter_locations.clear();
    state->active_id = kPostShaderNoneId;
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
    return implementation_->program != 0U;
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
    std::string text;
    PostShaderProgramSource source;
    if (entry == nullptr)
    {
        state->last_error = "no shader named '" + std::string(id) + "' in " +
            state->directory_text + " or built in";
    }
    else if (LoadPostShaderText(*entry, &text, &state->last_error) &&
             BuildPostShaderProgramSource(text, &source, &state->last_error))
    {
        // Issue #69: the shaders are written in the GLSL 1.10 style of
        // RetroArch's compatibility contexts; carry them to the context's
        // dialect, binding the shared vertex layout's slots by their names.
        const std::string vertex = AdaptLegacyGlslStage(
            source.vertex, state->gl.es, GlslStage::kVertex);
        const std::string fragment = AdaptLegacyGlslStage(
            source.fragment, state->gl.es, GlslStage::kFragment);
        state->program =
            BuildGlProgram(state->gl, vertex.c_str(), fragment.c_str(),
                           "VertexCoord", "COLOR", "TexCoord",
                           &state->last_error);
    }

    if (state->program == 0U)
    {
        std::fprintf(stderr, "[repiu-post] shader '%.*s' not applied: %s\n",
                     static_cast<int>(id.size()), id.data(),
                     state->last_error.c_str());
        return false;
    }

    const GlApi& gl = state->gl;
    state->active_id = std::string(id);
    state->mvp_matrix = gl.get_uniform_location(state->program, "MVPMatrix");
    state->texture = gl.get_uniform_location(state->program, "Texture");
    state->input_size = gl.get_uniform_location(state->program, "InputSize");
    state->texture_size =
        gl.get_uniform_location(state->program, "TextureSize");
    state->output_size = gl.get_uniform_location(state->program, "OutputSize");
    state->frame_count = gl.get_uniform_location(state->program, "FrameCount");
    state->frame_direction =
        gl.get_uniform_location(state->program, "FrameDirection");
    state->parameters = std::move(source.parameters);
    for (const PostShaderParameter& parameter : state->parameters)
    {
        state->parameter_locations.push_back(
            gl.get_uniform_location(state->program, parameter.name.c_str()));
    }
    state->frames = 0;
    std::fprintf(stderr, "[repiu-post] shader: %s (%zu parameters)\n",
                 state->active_id.c_str(), state->parameters.size());
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

void GlidePostProcess::Apply(unsigned int input_texture, std::uint32_t x,
                             std::uint32_t y, std::uint32_t width,
                             std::uint32_t height,
                             std::uint32_t logical_width,
                             std::uint32_t logical_height)
{
    Implementation* state = implementation_.get();
    if (state->program == 0U || input_texture == 0U || width == 0U ||
        height == 0U)
    {
        return;
    }
    if (logical_width == 0U || logical_height == 0U)
    {
        logical_width = width;
        logical_height = height;
    }
    const GlApi& gl = state->gl;

    gl.active_texture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, input_texture);
    // The shaders sample between the picture's pixels (the crt warp), so the
    // input is filtered; the present pass sets its own filter each frame.
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    glViewport(static_cast<GLint>(x), static_cast<GLint>(y),
               static_cast<GLsizei>(width), static_cast<GLsizei>(height));
    // Alpha is left as it was, so no translucency reaches a compositor
    // through the window's alpha channel.
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);

    gl.use_program(state->program);
    if (state->mvp_matrix >= 0)
    {
        gl.uniform_matrix_4fv(state->mvp_matrix, 1, GL_FALSE,
                              kUnitSquareProjection);
    }
    if (state->texture >= 0)
    {
        gl.uniform_1i(state->texture, 0);
    }
    if (state->input_size >= 0)
    {
        gl.uniform_2f(state->input_size, static_cast<GLfloat>(logical_width),
                      static_cast<GLfloat>(logical_height));
    }
    if (state->texture_size >= 0)
    {
        gl.uniform_2f(state->texture_size, static_cast<GLfloat>(logical_width),
                      static_cast<GLfloat>(logical_height));
    }
    if (state->output_size >= 0)
    {
        gl.uniform_2f(state->output_size, static_cast<GLfloat>(width),
                      static_cast<GLfloat>(height));
    }
    if (state->frame_count >= 0)
    {
        gl.uniform_1i(state->frame_count, static_cast<GLint>(state->frames));
    }
    if (state->frame_direction >= 0)
    {
        gl.uniform_1i(state->frame_direction, 1);
    }
    for (std::size_t index = 0; index < state->parameters.size(); ++index)
    {
        if (state->parameter_locations[index] >= 0)
        {
            gl.uniform_1f(state->parameter_locations[index],
                          state->parameters[index].value);
        }
    }

    // The unit square; texture row 0 is the bottom scanline.
    state->quad.Draw();
    ++state->frames;
}

}  // namespace repiu::engine
