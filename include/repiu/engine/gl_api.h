#ifndef REPIU_ENGINE_GL_API_H_
#define REPIU_ENGINE_GL_API_H_

#include <SDL3/SDL_opengl.h>

#include <string>

namespace repiu::engine
{

// Issue #69: the OpenGL functions beyond 1.1 the Glide renderer uses, found
// through SDL once a context is current. Every one of them exists in both
// OpenGL 3.3 core and OpenGL ES 3.0 (WebGL2), which is the whole subset the
// renderer is allowed: what the two do not share stays out of this table.
struct GlApi
{
    // Shaders and programs.
    PFNGLCREATESHADERPROC create_shader = nullptr;
    PFNGLSHADERSOURCEPROC shader_source = nullptr;
    PFNGLCOMPILESHADERPROC compile_shader = nullptr;
    PFNGLGETSHADERIVPROC get_shader_iv = nullptr;
    PFNGLGETSHADERINFOLOGPROC get_shader_info_log = nullptr;
    PFNGLDELETESHADERPROC delete_shader = nullptr;
    PFNGLCREATEPROGRAMPROC create_program = nullptr;
    PFNGLATTACHSHADERPROC attach_shader = nullptr;
    PFNGLBINDATTRIBLOCATIONPROC bind_attrib_location = nullptr;
    PFNGLLINKPROGRAMPROC link_program = nullptr;
    PFNGLGETPROGRAMIVPROC get_program_iv = nullptr;
    PFNGLGETPROGRAMINFOLOGPROC get_program_info_log = nullptr;
    PFNGLDELETEPROGRAMPROC delete_program = nullptr;
    PFNGLUSEPROGRAMPROC use_program = nullptr;
    PFNGLGETUNIFORMLOCATIONPROC get_uniform_location = nullptr;
    PFNGLUNIFORM1IPROC uniform_1i = nullptr;
    PFNGLUNIFORM1FPROC uniform_1f = nullptr;
    PFNGLUNIFORM2FPROC uniform_2f = nullptr;
    PFNGLUNIFORM4FPROC uniform_4f = nullptr;
    PFNGLUNIFORM1FVPROC uniform_1fv = nullptr;
    PFNGLUNIFORMMATRIX4FVPROC uniform_matrix_4fv = nullptr;
    PFNGLVERTEXATTRIB4FPROC vertex_attrib_4f = nullptr;

    // Buffers and vertex arrays.
    PFNGLGENBUFFERSPROC gen_buffers = nullptr;
    PFNGLDELETEBUFFERSPROC delete_buffers = nullptr;
    PFNGLBINDBUFFERPROC bind_buffer = nullptr;
    PFNGLBUFFERDATAPROC buffer_data = nullptr;
    PFNGLBUFFERSUBDATAPROC buffer_sub_data = nullptr;
    PFNGLGENVERTEXARRAYSPROC gen_vertex_arrays = nullptr;
    PFNGLDELETEVERTEXARRAYSPROC delete_vertex_arrays = nullptr;
    PFNGLBINDVERTEXARRAYPROC bind_vertex_array = nullptr;
    PFNGLVERTEXATTRIBPOINTERPROC vertex_attrib_pointer = nullptr;
    PFNGLENABLEVERTEXATTRIBARRAYPROC enable_vertex_attrib_array = nullptr;

    // Textures.
    PFNGLACTIVETEXTUREPROC active_texture = nullptr;

    // Framebuffers.
    PFNGLGENFRAMEBUFFERSPROC gen_framebuffers = nullptr;
    PFNGLDELETEFRAMEBUFFERSPROC delete_framebuffers = nullptr;
    PFNGLBINDFRAMEBUFFERPROC bind_framebuffer = nullptr;
    PFNGLFRAMEBUFFERTEXTURE2DPROC framebuffer_texture_2d = nullptr;
    PFNGLCHECKFRAMEBUFFERSTATUSPROC check_framebuffer_status = nullptr;
    PFNGLGENRENDERBUFFERSPROC gen_renderbuffers = nullptr;
    PFNGLDELETERENDERBUFFERSPROC delete_renderbuffers = nullptr;
    PFNGLBINDRENDERBUFFERPROC bind_renderbuffer = nullptr;
    PFNGLRENDERBUFFERSTORAGEPROC renderbuffer_storage = nullptr;
    PFNGLFRAMEBUFFERRENDERBUFFERPROC framebuffer_renderbuffer = nullptr;

    // The depth clear value: `glClearDepthf` (ES, and desktop 4.1) when the
    // context has it, the double `glClearDepth` of desktop 1.0 otherwise.
    PFNGLCLEARDEPTHFPROC clear_depth_f = nullptr;
    void(APIENTRY* clear_depth_d)(GLdouble) = nullptr;

    // Whether the current context is OpenGL ES (WebGL2 on the web).
    bool es = false;

    void ClearDepth(float depth) const;
};

// Resolves every entry for the current context. On failure names the first
// function that is missing.
bool LoadGlApi(GlApi* api, std::string* missing_function);

// The GL state the passes outside the Glide pipeline (the LFB blit, the
// present and the post-process pass) change, captured with queries both
// desktop core and ES answer, so each pass can put back exactly what it found.
// It replaces `glPushAttrib`, which neither has.
struct GlStateSnapshot
{
    GLboolean depth_test = GL_FALSE;
    GLboolean blend = GL_FALSE;
    GLboolean cull_face = GL_FALSE;
    GLboolean scissor_test = GL_FALSE;
    GLboolean color_mask[4] = {GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE};
    GLboolean depth_mask = GL_TRUE;
    GLint viewport[4] = {};
    GLint scissor_box[4] = {};
    GLint program = 0;
    GLint draw_framebuffer = 0;
    GLint read_framebuffer = 0;
    GLint active_texture = GL_TEXTURE0;
    GLint texture = 0;
    GLint vertex_array = 0;
    GLint array_buffer = 0;

    void Capture();
    void Restore(const GlApi& api) const;
};

}  // namespace repiu::engine

#endif  // REPIU_ENGINE_GL_API_H_
