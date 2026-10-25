#ifndef REPIU_ENGINE_GL_SHADER_DIALECT_H_
#define REPIU_ENGINE_GL_SHADER_DIALECT_H_

#include <string>
#include <string_view>

namespace repiu::engine
{

// Issue #69: the shaders are written once for the subset GLSL 3.30 core and
// GLSL ES 3.00 share, and only their first lines depend on the context:
// `#version 330 core` on desktop, `#version 300 es` with default precisions
// on ES (WebGL2). The context decides, not the platform, so the engine holds
// no platform branch.
std::string GlslHeader(bool es);

// The version line ImGui's OpenGL3 backend takes for the same context.
const char* ImGuiGlslVersion(bool es);

enum class GlslStage
{
    kVertex,
    kFragment
};

// Carries a GLSL 1.10-style stage (post-process shaders written for
// RetroArch-style compatibility contexts) to the current context: a leading
// `#version` line is dropped for the header, and the words the newer dialect
// removed are replaced as whole words -- `attribute` and `varying` become
// `in`/`out`, `texture2D` becomes `texture`, and `gl_FragColor` becomes a
// declared output -- unless the fragment body declares a `vec4` output of its
// own, as libretro's `#version 130` shaders do behind `__VERSION__ >= 130`
// (issue #90), since ES refuses two outputs without layout locations.
// Replacing words rather than defining macros keeps clear of the reserved
// `gl_` names. Anything else the old dialect had (`gl_TexCoord`, the
// fixed-function matrices) still fails to compile, which the caller reports
// as it reports any compile failure.
std::string AdaptLegacyGlslStage(std::string_view source, bool es,
                                 GlslStage stage);

}  // namespace repiu::engine

#endif  // REPIU_ENGINE_GL_SHADER_DIALECT_H_
