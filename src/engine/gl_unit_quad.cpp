#include "repiu/engine/gl_unit_quad.h"

#include "repiu/engine/glide_vertex_stream.h"

namespace repiu::engine
{
namespace
{

GLuint CompileStage(const GlApi& api, GLenum type, const char* source,
                    std::string* message)
{
    const GLuint shader = api.create_shader(type);
    api.shader_source(shader, 1, &source, nullptr);
    api.compile_shader(shader);
    GLint compiled = GL_FALSE;
    api.get_shader_iv(shader, GL_COMPILE_STATUS, &compiled);
    if (compiled == GL_TRUE)
    {
        return shader;
    }
    char log[1024]{};
    api.get_shader_info_log(shader, sizeof(log), nullptr, log);
    *message = std::string(type == GL_VERTEX_SHADER ? "vertex" : "fragment") +
        " shader failed to compile: " + log;
    api.delete_shader(shader);
    return 0;
}

}  // namespace

bool GlUnitQuad::Initialize(const GlApi* api)
{
    Shutdown();
    if (api == nullptr)
    {
        return false;
    }
    api_ = api;
    constexpr GLfloat kCorners[4][4] = {{0.0F, 0.0F, 0.0F, 1.0F},
                                        {1.0F, 0.0F, 0.0F, 1.0F},
                                        {0.0F, 1.0F, 0.0F, 1.0F},
                                        {1.0F, 1.0F, 0.0F, 1.0F}};
    api_->gen_vertex_arrays(1, &vertex_array_);
    api_->gen_buffers(1, &buffer_);
    if (vertex_array_ == 0U || buffer_ == 0U)
    {
        Shutdown();
        return false;
    }
    GLint previous_vertex_array = 0;
    GLint previous_buffer = 0;
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &previous_vertex_array);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &previous_buffer);
    api_->bind_vertex_array(vertex_array_);
    api_->bind_buffer(GL_ARRAY_BUFFER, buffer_);
    api_->buffer_data(GL_ARRAY_BUFFER, sizeof(kCorners), kCorners,
                      GL_STATIC_DRAW);
    api_->enable_vertex_attrib_array(kGlideVertexPositionSlot);
    api_->vertex_attrib_pointer(kGlideVertexPositionSlot, 4, GL_FLOAT,
                                GL_FALSE, 0, nullptr);
    api_->enable_vertex_attrib_array(kGlideVertexTexCoordSlot);
    api_->vertex_attrib_pointer(kGlideVertexTexCoordSlot, 4, GL_FLOAT,
                                GL_FALSE, 0, nullptr);
    api_->bind_vertex_array(static_cast<GLuint>(previous_vertex_array));
    api_->bind_buffer(GL_ARRAY_BUFFER, static_cast<GLuint>(previous_buffer));
    return true;
}

void GlUnitQuad::Shutdown()
{
    if (api_ != nullptr)
    {
        if (buffer_ != 0U)
        {
            api_->delete_buffers(1, &buffer_);
        }
        if (vertex_array_ != 0U)
        {
            api_->delete_vertex_arrays(1, &vertex_array_);
        }
    }
    api_ = nullptr;
    vertex_array_ = 0U;
    buffer_ = 0U;
}

void GlUnitQuad::Draw() const
{
    if (api_ == nullptr)
    {
        return;
    }
    api_->bind_vertex_array(vertex_array_);
    // The color slot has no array here; a program that reads it sees white.
    api_->vertex_attrib_4f(kGlideVertexColorSlot, 1.0F, 1.0F, 1.0F, 1.0F);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

GLuint BuildGlProgram(const GlApi& api, const char* vertex_source,
                      const char* fragment_source, const char* position_name,
                      const char* color_name, const char* tex_coord_name,
                      std::string* message)
{
    std::string scratch;
    if (message == nullptr)
    {
        message = &scratch;
    }
    const GLuint vertex =
        CompileStage(api, GL_VERTEX_SHADER, vertex_source, message);
    if (vertex == 0U)
    {
        return 0U;
    }
    const GLuint fragment =
        CompileStage(api, GL_FRAGMENT_SHADER, fragment_source, message);
    if (fragment == 0U)
    {
        api.delete_shader(vertex);
        return 0U;
    }
    const GLuint program = api.create_program();
    api.attach_shader(program, vertex);
    api.attach_shader(program, fragment);
    if (position_name != nullptr)
    {
        api.bind_attrib_location(program, kGlideVertexPositionSlot,
                                 position_name);
    }
    if (color_name != nullptr)
    {
        api.bind_attrib_location(program, kGlideVertexColorSlot, color_name);
    }
    if (tex_coord_name != nullptr)
    {
        api.bind_attrib_location(program, kGlideVertexTexCoordSlot,
                                 tex_coord_name);
    }
    api.link_program(program);
    api.delete_shader(vertex);
    api.delete_shader(fragment);
    GLint linked = GL_FALSE;
    api.get_program_iv(program, GL_LINK_STATUS, &linked);
    if (linked != GL_TRUE)
    {
        char log[1024]{};
        api.get_program_info_log(program, sizeof(log), nullptr, log);
        *message = std::string("program failed to link: ") + log;
        api.delete_program(program);
        return 0U;
    }
    return program;
}

}  // namespace repiu::engine
