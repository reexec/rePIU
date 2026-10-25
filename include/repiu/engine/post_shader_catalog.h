#ifndef REPIU_ENGINE_POST_SHADER_CATALOG_H_
#define REPIU_ENGINE_POST_SHADER_CATALOG_H_

#include "repiu/engine/post_shader_preset.h"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace repiu::engine
{

// Task 768. The post-processing shaders a run can choose from: the ones built
// into the executable and the `.glsl` files and `.glslp` presets (issue #90)
// in the shader directory. Neither the launcher nor the OSD needs GL to list
// them, so this module has none.

// The id that selects no pass at all, and the environment variable the
// selection is read from (the launcher publishes `[Video] post_shader` there).
inline constexpr const char* kPostShaderNoneId = "none";
inline constexpr const char* kPostShaderVariable = "REPIU_POST_SHADER";

struct PostShaderEntry
{
    // Built-ins are a bare name (`crt`); files are their file name with its
    // extension (`my_crt.glsl`, `my_chain.glslp`), so the two can never
    // collide.
    std::string id;
    bool builtin = false;
    // Empty for a built-in.
    std::filesystem::path path;
};

// `shaders/` under the working directory when it exists, otherwise under
// `executable_directory` when that exists, otherwise the working-directory
// candidate -- the rule `cfg/` follows.
[[nodiscard]] std::filesystem::path ResolvePostShaderDirectory(
    const std::filesystem::path& executable_directory);

// Built-ins first in a fixed order, then the directory's regular `.glsl` and
// `.glslp` files sorted by name. A missing or unreadable directory lists
// built-ins only.
[[nodiscard]] std::vector<PostShaderEntry> ListPostShaders(
    const std::filesystem::path& directory);

// The entry with `id`, or null. `none` is not an entry.
[[nodiscard]] const PostShaderEntry* FindPostShader(
    const std::vector<PostShaderEntry>& entries, std::string_view id);

// The shader's passes: a `.glsl` file or built-in as one pass into the window,
// a `.glslp` preset with its shaders read beside it (embedded ones for a
// built-in). False, with `error` set, when a file cannot be read or the preset
// does not parse.
[[nodiscard]] bool LoadPostShaderPreset(const PostShaderEntry& entry,
                                        PostShaderPreset* preset,
                                        std::string* error);

}  // namespace repiu::engine

#endif  // REPIU_ENGINE_POST_SHADER_CATALOG_H_
