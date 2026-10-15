#include "repiu/engine/glide_opengl_shader.h"

#include "repiu/engine/gl_shader_dialect.h"
#include "repiu/engine/gl_unit_quad.h"
#include "repiu/engine/glide_gl_error_policy.h"

#include "repiu/engine/gl_api.h"

#include <SDL3/SDL_opengl.h>

#include <string>

namespace repiu::engine
{

struct GlideOpenGlShader::Implementation
{
    GlApi gl;
    GLuint program = 0;
    GLint alpha_function = -1;
    GLint alpha_factor = -1;
    GLint alpha_local = -1;
    GLint alpha_other = -1;
    GLint alpha_invert = -1;
    GLint color_function = -1;
    GLint color_factor = -1;
    GLint color_local = -1;
    GLint color_other = -1;
    GLint color_invert = -1;
    GLint texture_enable = -1;
    GLint texture_sampler = -1;
    GLint blit_mode = -1;
    GLint constant_color = -1;
    GLint fog_mode = -1;
    GLint fog_color = -1;
    GLint fog_table = -1;
    GLint projection = -1;
    GLint point_size = -1;
    GLint alpha_test_function = -1;
    GLint alpha_test_reference = -1;
};


GlideOpenGlShader::GlideOpenGlShader() = default;

GlideOpenGlShader::~GlideOpenGlShader()
{
    Shutdown();
}

bool GlideOpenGlShader::Initialize()
{
    Shutdown();
    implementation_ = std::make_unique<Implementation>();
    std::string missing;
    if (!LoadGlApi(&implementation_->gl, &missing))
    {
        message_ = "required OpenGL entry point is unavailable: " + missing;
        implementation_.reset();
        return false;
    }
    // Issue #69: attributes in the shared vertex layout's slots and a uniform
    // projection replace ftransform(), gl_Color and gl_MultiTexCoord0, and the
    // alpha test (glAlphaFunc, absent from core and ES) compares the final
    // alpha here. The combine and fog expressions are unchanged.
    constexpr char kVertexBody[] =
        "in vec3 repiuPosition;\n"
        "in vec4 repiuColor;\n"
        "in vec4 repiuTexCoord;\n"
        "uniform mat4 repiuProjection;\n"
        "uniform float repiuPointSize;\n"
        "out vec4 repiuIteratedColor;\n"
        "out vec4 repiuTextureFogCoord;\n"
        "void main() {\n"
        "  gl_Position = repiuProjection * vec4(repiuPosition, 1.0);\n"
        "  gl_PointSize = repiuPointSize;\n"
        "  repiuIteratedColor = repiuColor;\n"
        "  repiuTextureFogCoord = repiuTexCoord;\n"
        "}\n";
    constexpr char kFragmentBody[] =
        "uniform int repiuAlphaFunction;\n"
        "uniform int repiuAlphaFactor;\n"
        "uniform int repiuAlphaLocal;\n"
        "uniform int repiuAlphaOther;\n"
        "uniform int repiuAlphaInvert;\n"
        "uniform int repiuColorFunction;\n"
        "uniform int repiuColorFactor;\n"
        "uniform int repiuColorLocal;\n"
        "uniform int repiuColorOther;\n"
        "uniform int repiuColorInvert;\n"
        "uniform int repiuTextureEnable;\n"
        "uniform sampler2D repiuTexture;\n"
        "uniform int repiuBlitMode;\n"
        "uniform vec4 repiuConstantColor;\n"
        "uniform int repiuFogMode;\n"
        "uniform vec4 repiuFogColor;\n"
        "uniform float repiuFogTable[64];\n"
        "uniform int repiuAlphaTestFunction;\n"
        "uniform float repiuAlphaTestReference;\n"
        "in vec4 repiuIteratedColor;\n"
        "in vec4 repiuTextureFogCoord;\n"
        "out vec4 repiuFragColor;\n"
        "float repiuFogFactor(float oow) {\n"
        "  if (oow <= 0.0) return 0.0;\n"
        "  float w = 1.0 / oow;\n"
        "  if (w <= 1.0) return repiuFogTable[0];\n"
        "  if (w >= 52428.8) return repiuFogTable[63];\n"
        "  float group = floor(log2(w));\n"
        "  float normalized = w / exp2(group);\n"
        "  int offset = 0;\n"
        "  float lower = 1.0;\n"
        "  float upper = 8.0 / 7.0;\n"
        "  if (normalized > 8.0 / 7.0) { offset = 1; lower = 8.0 / 7.0; upper = 4.0 / 3.0; }\n"
        "  if (normalized > 4.0 / 3.0) { offset = 2; lower = 4.0 / 3.0; upper = 8.0 / 5.0; }\n"
        "  if (normalized > 8.0 / 5.0) { offset = 3; lower = 8.0 / 5.0; upper = 2.0; }\n"
        "  int index = int(group) * 4 + offset;\n"
        "  float fraction = clamp((normalized - lower) / (upper - lower), 0.0, 1.0);\n"
        "  return mix(repiuFogTable[index], repiuFogTable[index + 1], fraction);\n"
        "}\n"
        "bool repiuAlphaTestPasses(float alpha) {\n"
        "  float reference = repiuAlphaTestReference;\n"
        "  if (repiuAlphaTestFunction == 0) return false;\n"
        "  if (repiuAlphaTestFunction == 1) return alpha < reference;\n"
        "  if (repiuAlphaTestFunction == 2) return alpha == reference;\n"
        "  if (repiuAlphaTestFunction == 3) return alpha <= reference;\n"
        "  if (repiuAlphaTestFunction == 4) return alpha > reference;\n"
        "  if (repiuAlphaTestFunction == 5) return alpha != reference;\n"
        "  if (repiuAlphaTestFunction == 6) return alpha >= reference;\n"
        "  return true;\n"
        "}\n"
        "void main() {\n"
        "  vec2 uv = repiuTextureFogCoord.xy / max(repiuTextureFogCoord.w, 0.00000000000000000001);\n"
        "  if (repiuBlitMode != 0) { repiuFragColor = texture(repiuTexture, uv); return; }\n"
        "  vec4 tex = repiuTextureEnable != 0 ? texture(repiuTexture, uv) : vec4(1.0);\n"
        "  float a_loc = (repiuAlphaLocal == 0) ? repiuIteratedColor.a : ((repiuAlphaLocal == 1) ? repiuConstantColor.a : 0.0);\n"
        "  float a_oth = (repiuAlphaOther == 0) ? repiuIteratedColor.a : ((repiuAlphaOther == 1) ? tex.a : ((repiuAlphaOther == 2) ? repiuConstantColor.a : 0.0));\n"
        "  float a_fac = (repiuAlphaFactor == 0) ? 0.0 : ((repiuAlphaFactor == 1) ? a_loc : ((repiuAlphaFactor == 2) ? a_oth : ((repiuAlphaFactor == 8) ? 1.0 : 0.0)));\n"
        "  float alpha = 1.0;\n"
        "  if (repiuAlphaFunction == 1) alpha = a_loc;\n"
        "  else if (repiuAlphaFunction == 2) alpha = a_oth;\n"
        "  else if (repiuAlphaFunction == 3) alpha = a_oth * a_fac;\n"
        "  else if (repiuAlphaFunction == 4) alpha = a_oth * a_fac + a_loc;\n"
        "  else if (repiuAlphaFunction == 7) alpha = (a_oth - a_loc) * a_fac + a_loc;\n"
        "  if (repiuAlphaInvert != 0) alpha = 1.0 - alpha;\n"
        "  vec3 c_loc = (repiuColorLocal == 0) ? repiuIteratedColor.rgb : ((repiuColorLocal == 1) ? repiuConstantColor.rgb : vec3(0.0));\n"
        "  vec3 c_oth = (repiuColorOther == 0) ? repiuIteratedColor.rgb : ((repiuColorOther == 1) ? tex.rgb : ((repiuColorOther == 2) ? repiuConstantColor.rgb : vec3(0.0)));\n"
        "  vec3 c_fac = (repiuColorFactor == 0) ? vec3(0.0) : ((repiuColorFactor == 1) ? c_loc : ((repiuColorFactor == 2) ? vec3(a_oth) : ((repiuColorFactor == 3) ? vec3(a_loc) : ((repiuColorFactor == 8) ? vec3(1.0) : vec3(0.0)))));\n"
        "  vec3 color = vec3(1.0);\n"
        "  if (repiuColorFunction == 1) color = c_loc;\n"
        "  else if (repiuColorFunction == 2) color = c_oth;\n"
        "  else if (repiuColorFunction == 3) color = c_oth * c_fac;\n"
        "  else if (repiuColorFunction == 4) color = c_oth * c_fac + c_loc;\n"
        "  else if (repiuColorFunction == 7) color = (c_oth - c_loc) * c_fac + c_loc;\n"
        "  if (repiuColorInvert != 0) color = vec3(1.0) - color;\n"
        "  if (repiuFogMode == 2) color = mix(color, repiuFogColor.rgb, clamp(repiuFogFactor(repiuTextureFogCoord.z), 0.0, 1.0));\n"
        "  if (!repiuAlphaTestPasses(clamp(alpha, 0.0, 1.0))) discard;\n"
        "  repiuFragColor = vec4(color, alpha);\n"
        "}\n";
    const std::string header = GlslHeader(implementation_->gl.es);
    const std::string vertex_source = header + kVertexBody;
    const std::string fragment_source = header + kFragmentBody;
    implementation_->program = BuildGlProgram(
        implementation_->gl, vertex_source.c_str(), fragment_source.c_str(),
        "repiuPosition", "repiuColor", "repiuTexCoord", &message_);
    if (implementation_->program == 0U)
    {
        message_ = "Glide GLSL program: " + message_;
        implementation_.reset();
        return false;
    }
    implementation_->alpha_function =
        implementation_->gl.get_uniform_location(implementation_->program,
                                              "repiuAlphaFunction");
    implementation_->alpha_factor =
        implementation_->gl.get_uniform_location(implementation_->program,
                                              "repiuAlphaFactor");
    implementation_->alpha_local =
        implementation_->gl.get_uniform_location(implementation_->program,
                                              "repiuAlphaLocal");
    implementation_->alpha_other =
        implementation_->gl.get_uniform_location(implementation_->program,
                                              "repiuAlphaOther");
    implementation_->alpha_invert =
        implementation_->gl.get_uniform_location(implementation_->program,
                                              "repiuAlphaInvert");
    implementation_->color_function =
        implementation_->gl.get_uniform_location(implementation_->program,
                                              "repiuColorFunction");
    implementation_->color_factor =
        implementation_->gl.get_uniform_location(implementation_->program,
                                              "repiuColorFactor");
    implementation_->color_local =
        implementation_->gl.get_uniform_location(implementation_->program,
                                              "repiuColorLocal");
    implementation_->color_other =
        implementation_->gl.get_uniform_location(implementation_->program,
                                              "repiuColorOther");
    implementation_->color_invert =
        implementation_->gl.get_uniform_location(implementation_->program,
                                              "repiuColorInvert");
    implementation_->texture_enable =
        implementation_->gl.get_uniform_location(implementation_->program,
                                              "repiuTextureEnable");
    implementation_->texture_sampler =
        implementation_->gl.get_uniform_location(implementation_->program,
                                              "repiuTexture");
    implementation_->blit_mode =
        implementation_->gl.get_uniform_location(implementation_->program,
                                              "repiuBlitMode");
    implementation_->constant_color =
        implementation_->gl.get_uniform_location(implementation_->program,
                                              "repiuConstantColor");
    implementation_->fog_mode =
        implementation_->gl.get_uniform_location(implementation_->program,
                                              "repiuFogMode");
    implementation_->fog_color =
        implementation_->gl.get_uniform_location(implementation_->program,
                                              "repiuFogColor");
    implementation_->fog_table =
        implementation_->gl.get_uniform_location(implementation_->program,
                                              "repiuFogTable");
    implementation_->projection =
        implementation_->gl.get_uniform_location(implementation_->program,
                                                 "repiuProjection");
    implementation_->point_size =
        implementation_->gl.get_uniform_location(implementation_->program,
                                                 "repiuPointSize");
    implementation_->alpha_test_function =
        implementation_->gl.get_uniform_location(implementation_->program,
                                                 "repiuAlphaTestFunction");
    implementation_->alpha_test_reference =
        implementation_->gl.get_uniform_location(implementation_->program,
                                                 "repiuAlphaTestReference");
    if (implementation_->alpha_function < 0 ||
        implementation_->alpha_factor < 0 ||
        implementation_->alpha_local < 0 ||
        implementation_->alpha_other < 0 ||
        implementation_->alpha_invert < 0 ||
        implementation_->color_function < 0 ||
        implementation_->color_factor < 0 ||
        implementation_->color_local < 0 ||
        implementation_->color_other < 0 ||
        implementation_->color_invert < 0 ||
        implementation_->texture_enable < 0 ||
        implementation_->texture_sampler < 0 ||
        implementation_->blit_mode < 0 ||
        implementation_->constant_color < 0 ||
        implementation_->fog_mode < 0 ||
        implementation_->fog_color < 0 ||
        implementation_->fog_table < 0 ||
        implementation_->projection < 0 ||
        implementation_->point_size < 0 ||
        implementation_->alpha_test_function < 0 ||
        implementation_->alpha_test_reference < 0)
    {
        message_ = "Glide GLSL alpha-combine uniforms are unavailable";
        Shutdown();
        return false;
    }
    implementation_->gl.use_program(implementation_->program);
    // Seed the combine functions to LOCAL (1) so a draw that precedes the game's
    // grColorCombine/grAlphaCombine setup (or a retained unsupported equation)
    // emits the iterated vertex color rather than a black fragment. The game's
    // observed init combine (1,0,0,2,0) later confirms this same value.
    implementation_->gl.uniform_1i(implementation_->color_function, 1);
    implementation_->gl.uniform_1i(implementation_->color_factor, 0);
    implementation_->gl.uniform_1i(implementation_->color_local, 0);
    implementation_->gl.uniform_1i(implementation_->color_other, 2);
    implementation_->gl.uniform_1i(implementation_->color_invert, 0);
    implementation_->gl.uniform_1i(implementation_->alpha_function, 1);
    implementation_->gl.uniform_1i(implementation_->alpha_factor, 0);
    implementation_->gl.uniform_1i(implementation_->alpha_local, 0);
    implementation_->gl.uniform_1i(implementation_->alpha_other, 2);
    implementation_->gl.uniform_1i(implementation_->alpha_invert, 0);
    // Texture sampling starts disabled and binds to texture unit 0 (R3).
    implementation_->gl.uniform_1i(implementation_->texture_enable, 0);
    implementation_->gl.uniform_1i(implementation_->texture_sampler, 0);
    implementation_->gl.uniform_1i(implementation_->blit_mode, 0);
    implementation_->gl.uniform_1i(implementation_->fog_mode, 0);
    implementation_->gl.uniform_1f(implementation_->point_size, 1.0F);
    // GR_CMP_ALWAYS until the game sets an alpha test.
    implementation_->gl.uniform_1i(implementation_->alpha_test_function, 7);
    implementation_->gl.uniform_1f(implementation_->alpha_test_reference,
                                   0.0F);
    implementation_->gl.uniform_4f(implementation_->constant_color,
                                1.0F, 1.0F, 1.0F, 1.0F);
    implementation_->gl.uniform_4f(implementation_->fog_color,
                                0.0F, 0.0F, 0.0F, 1.0F);
    GLfloat empty_fog_table[hle::kGlideFogTableEntryCount]{};
    implementation_->gl.uniform_1fv(implementation_->fog_table,
                                 hle::kGlideFogTableEntryCount,
                                 empty_fog_table);
    message_ = "Glide GLSL combine program initialized";
    return true;
}

void GlideOpenGlShader::SetTextureEnabled(bool enabled)
{
    if (!implementation_ || implementation_->program == 0 ||
        implementation_->texture_enable < 0)
    {
        return;
    }
    implementation_->gl.use_program(implementation_->program);
    implementation_->gl.uniform_1i(implementation_->texture_enable,
                                enabled ? 1 : 0);
}

void GlideOpenGlShader::SetBlitMode(bool enabled)
{
    if (!implementation_ || implementation_->program == 0 ||
        implementation_->blit_mode < 0)
    {
        return;
    }
    implementation_->gl.use_program(implementation_->program);
    implementation_->gl.uniform_1i(implementation_->blit_mode,
                                enabled ? 1 : 0);
}

bool GlideOpenGlShader::SetFogMode(std::uint32_t mode)
{
    if (!implementation_ || implementation_->program == 0 ||
        implementation_->fog_mode < 0 || (mode != 0U && mode != 2U))
    {
        return false;
    }
    implementation_->gl.use_program(implementation_->program);
    implementation_->gl.uniform_1i(implementation_->fog_mode,
                                static_cast<GLint>(mode));
    return !GlideGlErrorCheckPolicyEnabled() || glGetError() == GL_NO_ERROR;
}

bool GlideOpenGlShader::SetFogColor(std::uint32_t argb)
{
    if (!implementation_ || implementation_->program == 0 ||
        implementation_->fog_color < 0)
    {
        return false;
    }
    const float a = static_cast<float>((argb >> 24) & 0xFF) / 255.0F;
    const float r = static_cast<float>((argb >> 16) & 0xFF) / 255.0F;
    const float g = static_cast<float>((argb >> 8) & 0xFF) / 255.0F;
    const float b = static_cast<float>(argb & 0xFF) / 255.0F;
    implementation_->gl.use_program(implementation_->program);
    implementation_->gl.uniform_4f(implementation_->fog_color, r, g, b, a);
    return !GlideGlErrorCheckPolicyEnabled() || glGetError() == GL_NO_ERROR;
}

bool GlideOpenGlShader::SetFogTable(const hle::GlideFogTable& table)
{
    if (!implementation_ || implementation_->program == 0 ||
        implementation_->fog_table < 0 ||
        implementation_->projection < 0 ||
        implementation_->point_size < 0 ||
        implementation_->alpha_test_function < 0 ||
        implementation_->alpha_test_reference < 0)
    {
        return false;
    }
    GLfloat normalized[hle::kGlideFogTableEntryCount]{};
    for (std::size_t index = 0U; index < table.size(); ++index)
    {
        normalized[index] = static_cast<GLfloat>(table[index]) / 255.0F;
    }
    implementation_->gl.use_program(implementation_->program);
    implementation_->gl.uniform_1fv(
        implementation_->fog_table,
        static_cast<GLsizei>(hle::kGlideFogTableEntryCount), normalized);
    return !GlideGlErrorCheckPolicyEnabled() || glGetError() == GL_NO_ERROR;
}

bool GlideOpenGlShader::SetAlphaCombine(
    const hle::GlideAlphaCombineState& state)
{
    if (!implementation_ || implementation_->program == 0 || !state.valid)
    {
        message_ = "unsupported Glide alpha-combine equation";
        return false;
    }
    implementation_->gl.use_program(implementation_->program);
    implementation_->gl.uniform_1i(implementation_->alpha_function,
                                static_cast<GLint>(state.function));
    implementation_->gl.uniform_1i(implementation_->alpha_factor,
                                static_cast<GLint>(state.factor));
    implementation_->gl.uniform_1i(implementation_->alpha_local,
                                static_cast<GLint>(state.local));
    implementation_->gl.uniform_1i(implementation_->alpha_other,
                                static_cast<GLint>(state.other));
    implementation_->gl.uniform_1i(implementation_->alpha_invert,
                                state.invert ? 1 : 0);
    if (GlideGlErrorCheckPolicyEnabled() && glGetError() != GL_NO_ERROR)
    {
        message_ = "failed to apply Glide alpha-combine uniforms";
        return false;
    }
    message_ = "observed Glide alpha-combine equation applied with GLSL";
    return true;
}

bool GlideOpenGlShader::SetColorCombine(
    const hle::GlideColorCombineState& state)
{
    if (!implementation_ || implementation_->program == 0 || !state.valid)
    {
        message_ = "unsupported Glide color-combine equation";
        return false;
    }
    implementation_->gl.use_program(implementation_->program);
    implementation_->gl.uniform_1i(implementation_->color_function,
                                static_cast<GLint>(state.function));
    implementation_->gl.uniform_1i(implementation_->color_factor,
                                static_cast<GLint>(state.factor));
    implementation_->gl.uniform_1i(implementation_->color_local,
                                static_cast<GLint>(state.local));
    implementation_->gl.uniform_1i(implementation_->color_other,
                                static_cast<GLint>(state.other));
    implementation_->gl.uniform_1i(implementation_->color_invert,
                                state.invert ? 1 : 0);
    if (GlideGlErrorCheckPolicyEnabled() && glGetError() != GL_NO_ERROR)
    {
        message_ = "failed to apply Glide color-combine uniforms";
        return false;
    }
    message_ = "observed Glide color-combine equation applied with GLSL";
    return true;
}

bool GlideOpenGlShader::SetConstantColor(std::uint32_t argb)
{
    if (!implementation_ || implementation_->program == 0 ||
        implementation_->constant_color < 0)
    {
        return false;
    }
    implementation_->gl.use_program(implementation_->program);
    const float a = static_cast<float>((argb >> 24) & 0xFF) / 255.0F;
    const float r = static_cast<float>((argb >> 16) & 0xFF) / 255.0F;
    const float g = static_cast<float>((argb >> 8) & 0xFF) / 255.0F;
    const float b = static_cast<float>(argb & 0xFF) / 255.0F;
    implementation_->gl.uniform_4f(implementation_->constant_color, r, g, b, a);
    return true;
}

void GlideOpenGlShader::SetProjection(const float matrix[16])
{
    if (!implementation_ || implementation_->program == 0)
    {
        return;
    }
    implementation_->gl.use_program(implementation_->program);
    implementation_->gl.uniform_matrix_4fv(implementation_->projection, 1,
                                           GL_FALSE, matrix);
}

void GlideOpenGlShader::SetPointSize(float size)
{
    if (!implementation_ || implementation_->program == 0)
    {
        return;
    }
    implementation_->gl.use_program(implementation_->program);
    implementation_->gl.uniform_1f(implementation_->point_size, size);
}

void GlideOpenGlShader::SetAlphaTest(std::uint32_t function, float reference)
{
    if (!implementation_ || implementation_->program == 0)
    {
        return;
    }
    implementation_->gl.use_program(implementation_->program);
    implementation_->gl.uniform_1i(implementation_->alpha_test_function,
                                   static_cast<GLint>(function));
    implementation_->gl.uniform_1f(implementation_->alpha_test_reference,
                                   reference);
}

void GlideOpenGlShader::Use()
{
    if (implementation_ && implementation_->program != 0)
    {
        implementation_->gl.use_program(implementation_->program);
    }
}

void GlideOpenGlShader::Shutdown()
{
    if (implementation_ && implementation_->program != 0 &&
        implementation_->gl.delete_program != nullptr)
    {
        implementation_->gl.use_program(0);
        implementation_->gl.delete_program(implementation_->program);
    }
    implementation_.reset();
}

}  // namespace repiu::engine
