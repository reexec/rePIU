#ifndef REPIU_ENGINE_POST_SHADER_PRESET_H_
#define REPIU_ENGINE_POST_SHADER_PRESET_H_

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace repiu::engine
{

// Issue #90. A post-processing shader as a chain of passes, in the subset of
// the libretro `.glslp` preset format rePIU reads (re2DJ #27's). A single
// `.glsl` file is a chain of one pass drawn straight into the window. Each
// pass reads the one before it as `Texture`; every pass can also read the
// game's picture as `OrigTexture` and the output of the pass N before it as
// `PassPrevNTexture` (`PassPrev(i + 1)` of pass i being the game's picture
// again). Each name has `...TextureSize` and `...InputSize` uniforms beside
// it, both its pixel size.
//
// Keys read, for pass i: `shaders`, `shaderi`, `scale_typei` (or `_x`/`_y`:
// `source`, `viewport`, `absolute`), `scalei` (or `_x`/`_y`),
// `filter_lineari`, `float_framebufferi`. As libretro reads them,
// `scale_typei` and `scalei` win over their `_x`/`_y` forms, a `scalei`
// without any scale type is ignored, `#` outside quotes starts a comment, and
// a UTF-8 BOM is skipped. Others, such as `srgb_framebuffer`, `wrap_mode`,
// `mipmap_input`, `alias` and lookup textures, are not supported and ignored.
enum class PostShaderScaleType
{
    // A multiple of the pass's input size.
    kSource,
    // A multiple of the picture's size in the window.
    kViewport,
    // A size in pixels.
    kAbsolute,
};

struct PostShaderPass
{
    // The file it came from, for messages, and its text.
    std::string path;
    std::string text;
    PostShaderScaleType scale_type_x = PostShaderScaleType::kSource;
    PostShaderScaleType scale_type_y = PostShaderScaleType::kSource;
    float scale_x = 1.0F;
    float scale_y = 1.0F;
    // Whether the pass samples its `Texture` with linear filtering; nearest
    // otherwise, and `filter_specified` whether the preset said. Unsaid, a
    // chained pass samples linearly and a lone pass into the window keeps
    // the filter the single-pass path always used (linear).
    bool filter_linear = true;
    bool filter_specified = false;
    // Whether the pass draws into a floating-point target, for passes whose
    // output is data rather than colour (signed or above 1).
    bool float_framebuffer = false;
    // Whether the pass draws straight into the window: the last pass, when
    // its preset gives it no scale. Its size is then the picture's.
    bool to_window = false;
};

struct PostShaderPreset
{
    std::vector<PostShaderPass> passes;
};

// Reads a file a preset names, relative to the preset.
using PostShaderFileReader = std::function<bool(
    const std::string& relative_path, std::string* text, std::string* error)>;

// A `.glslp` preset's passes, each shader read through `read`. False, with
// `error` saying why, on a missing `shaders` count, a missing shader line, a
// value that does not parse, or a file `read` cannot give.
[[nodiscard]] bool ParsePostShaderPreset(std::string_view text,
                                         const PostShaderFileReader& read,
                                         PostShaderPreset* preset,
                                         std::string* error);

// One `.glsl` file as a chain of one pass into the window.
[[nodiscard]] PostShaderPreset SinglePassPreset(std::string path,
                                                std::string text);

// The size a pass draws into, from its input's size and the picture's size in
// the window. At least one pixel each way.
void PostShaderPassSize(const PostShaderPass& pass, std::uint32_t input_width,
                        std::uint32_t input_height,
                        std::uint32_t viewport_width,
                        std::uint32_t viewport_height, std::uint32_t* width,
                        std::uint32_t* height);

}  // namespace repiu::engine

#endif  // REPIU_ENGINE_POST_SHADER_PRESET_H_
