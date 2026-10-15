#include "repiu/engine/gl_api.h"

#include <SDL3/SDL.h>

#include <cstdint>
#include <cstring>

namespace repiu::engine
{
namespace
{

GlProcAddressLoader g_loader = nullptr;

GlFunction LookUp(const char* name)
{
    if (g_loader != nullptr)
    {
        return g_loader(name);
    }
    return reinterpret_cast<GlFunction>(SDL_GL_GetProcAddress(name));
}

// A GL loader reports failure as 0, 1, 2, 3 or -1 rather than only null (the
// `wglGetProcAddress` convention SDL inherits on Windows). The address is held
// as a function pointer (SDL3 returns `SDL_FunctionPointer`) because C++ does
// not guarantee that a function pointer fits in an object pointer (Task 505).
template <typename Function>
bool Resolve(const char* name, Function* function)
{
    const GlFunction address = LookUp(name);
    const auto value = reinterpret_cast<std::uintptr_t>(address);
    if (address == nullptr || value <= 3U || value == ~std::uintptr_t{0})
    {
        *function = nullptr;
        return false;
    }
    *function = reinterpret_cast<Function>(address);
    return true;
}

}  // namespace

void SetGlProcAddressLoader(GlProcAddressLoader loader)
{
    g_loader = loader;
}

void GlApi::ClearDepth(float depth) const
{
    if (clear_depth_f != nullptr)
    {
        clear_depth_f(depth);
    }
    else if (clear_depth_d != nullptr)
    {
        clear_depth_d(static_cast<GLdouble>(depth));
    }
}

bool LoadGlApi(GlApi* api, std::string* missing_function)
{
    std::string scratch;
    if (missing_function == nullptr)
    {
        missing_function = &scratch;
    }
    *api = GlApi{};
#define REPIU_GL_RESOLVE(name, field)                                         \
    if (!Resolve(name, &api->field))                                          \
    {                                                                         \
        *missing_function = name;                                             \
        return false;                                                         \
    }
    REPIU_GL_RESOLVE("glCreateShader", create_shader)
    REPIU_GL_RESOLVE("glShaderSource", shader_source)
    REPIU_GL_RESOLVE("glCompileShader", compile_shader)
    REPIU_GL_RESOLVE("glGetShaderiv", get_shader_iv)
    REPIU_GL_RESOLVE("glGetShaderInfoLog", get_shader_info_log)
    REPIU_GL_RESOLVE("glDeleteShader", delete_shader)
    REPIU_GL_RESOLVE("glCreateProgram", create_program)
    REPIU_GL_RESOLVE("glAttachShader", attach_shader)
    REPIU_GL_RESOLVE("glBindAttribLocation", bind_attrib_location)
    REPIU_GL_RESOLVE("glLinkProgram", link_program)
    REPIU_GL_RESOLVE("glGetProgramiv", get_program_iv)
    REPIU_GL_RESOLVE("glGetProgramInfoLog", get_program_info_log)
    REPIU_GL_RESOLVE("glDeleteProgram", delete_program)
    REPIU_GL_RESOLVE("glUseProgram", use_program)
    REPIU_GL_RESOLVE("glGetUniformLocation", get_uniform_location)
    REPIU_GL_RESOLVE("glUniform1i", uniform_1i)
    REPIU_GL_RESOLVE("glUniform1f", uniform_1f)
    REPIU_GL_RESOLVE("glUniform2f", uniform_2f)
    REPIU_GL_RESOLVE("glUniform4f", uniform_4f)
    REPIU_GL_RESOLVE("glUniform1fv", uniform_1fv)
    REPIU_GL_RESOLVE("glUniformMatrix4fv", uniform_matrix_4fv)
    REPIU_GL_RESOLVE("glVertexAttrib4f", vertex_attrib_4f)
    REPIU_GL_RESOLVE("glGenBuffers", gen_buffers)
    REPIU_GL_RESOLVE("glDeleteBuffers", delete_buffers)
    REPIU_GL_RESOLVE("glBindBuffer", bind_buffer)
    REPIU_GL_RESOLVE("glBufferData", buffer_data)
    REPIU_GL_RESOLVE("glBufferSubData", buffer_sub_data)
    REPIU_GL_RESOLVE("glGenVertexArrays", gen_vertex_arrays)
    REPIU_GL_RESOLVE("glDeleteVertexArrays", delete_vertex_arrays)
    REPIU_GL_RESOLVE("glBindVertexArray", bind_vertex_array)
    REPIU_GL_RESOLVE("glVertexAttribPointer", vertex_attrib_pointer)
    REPIU_GL_RESOLVE("glEnableVertexAttribArray", enable_vertex_attrib_array)
    REPIU_GL_RESOLVE("glActiveTexture", active_texture)
    REPIU_GL_RESOLVE("glGenFramebuffers", gen_framebuffers)
    REPIU_GL_RESOLVE("glDeleteFramebuffers", delete_framebuffers)
    REPIU_GL_RESOLVE("glBindFramebuffer", bind_framebuffer)
    REPIU_GL_RESOLVE("glFramebufferTexture2D", framebuffer_texture_2d)
    REPIU_GL_RESOLVE("glCheckFramebufferStatus", check_framebuffer_status)
    REPIU_GL_RESOLVE("glGenRenderbuffers", gen_renderbuffers)
    REPIU_GL_RESOLVE("glDeleteRenderbuffers", delete_renderbuffers)
    REPIU_GL_RESOLVE("glBindRenderbuffer", bind_renderbuffer)
    REPIU_GL_RESOLVE("glRenderbufferStorage", renderbuffer_storage)
    REPIU_GL_RESOLVE("glFramebufferRenderbuffer", framebuffer_renderbuffer)
#undef REPIU_GL_RESOLVE
    if (!Resolve("glClearDepthf", &api->clear_depth_f) &&
        !Resolve("glClearDepth", &api->clear_depth_d))
    {
        *missing_function = "glClearDepthf";
        return false;
    }
    const char* const version =
        reinterpret_cast<const char*>(glGetString(GL_VERSION));
    api->es = version != nullptr &&
        std::strncmp(version, "OpenGL ES", 9U) == 0;
    return true;
}

void GlStateSnapshot::Capture()
{
    depth_test = glIsEnabled(GL_DEPTH_TEST);
    blend = glIsEnabled(GL_BLEND);
    cull_face = glIsEnabled(GL_CULL_FACE);
    scissor_test = glIsEnabled(GL_SCISSOR_TEST);
    glGetBooleanv(GL_COLOR_WRITEMASK, color_mask);
    glGetBooleanv(GL_DEPTH_WRITEMASK, &depth_mask);
    glGetIntegerv(GL_VIEWPORT, viewport);
    glGetIntegerv(GL_SCISSOR_BOX, scissor_box);
    glGetIntegerv(GL_CURRENT_PROGRAM, &program);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw_framebuffer);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read_framebuffer);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &active_texture);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vertex_array);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &array_buffer);
}

void GlStateSnapshot::Restore(const GlApi& api) const
{
    const auto set = [](GLenum capability, GLboolean enabled) {
        if (enabled == GL_TRUE)
        {
            glEnable(capability);
        }
        else
        {
            glDisable(capability);
        }
    };
    set(GL_DEPTH_TEST, depth_test);
    set(GL_BLEND, blend);
    set(GL_CULL_FACE, cull_face);
    set(GL_SCISSOR_TEST, scissor_test);
    glColorMask(color_mask[0], color_mask[1], color_mask[2], color_mask[3]);
    glDepthMask(depth_mask);
    glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
    glScissor(scissor_box[0], scissor_box[1], scissor_box[2], scissor_box[3]);
    api.use_program(static_cast<GLuint>(program));
    api.bind_framebuffer(GL_DRAW_FRAMEBUFFER,
                         static_cast<GLuint>(draw_framebuffer));
    api.bind_framebuffer(GL_READ_FRAMEBUFFER,
                         static_cast<GLuint>(read_framebuffer));
    api.active_texture(static_cast<GLenum>(active_texture));
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(texture));
    api.bind_vertex_array(static_cast<GLuint>(vertex_array));
    api.bind_buffer(GL_ARRAY_BUFFER, static_cast<GLuint>(array_buffer));
}

}  // namespace repiu::engine
