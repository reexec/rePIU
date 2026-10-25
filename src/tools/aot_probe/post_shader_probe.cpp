#include "post_shader_probe.h"

#include "repiu/engine/post_shader_catalog.h"
#include "repiu/engine/post_shader_preset.h"
#include "repiu/engine/post_shader_source.h"
#include "repiu/launcher/launcher_settings.h"

#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <system_error>

namespace repiu::tools
{
namespace
{

using engine::BuildPostShaderProgramSource;
using engine::FindPostShader;
using engine::ListPostShaders;
using engine::LoadPostShaderPreset;
using engine::ParsePostShaderPreset;
using engine::PostShaderEntry;
using engine::PostShaderFileReader;
using engine::PostShaderPass;
using engine::PostShaderPassSize;
using engine::PostShaderPreset;
using engine::PostShaderProgramSource;
using engine::PostShaderScaleType;
using engine::SinglePassPreset;

bool Near(float left, float right)
{
    return std::fabs(left - right) < 0.0001F;
}

bool StartsWith(const std::string& text, const std::string& prefix)
{
    return text.compare(0, prefix.size(), prefix) == 0;
}

// The stage define lands first, or right after a `#version` line, and the
// rest of the text follows untouched.
bool ProbeAssembly()
{
    PostShaderProgramSource plain;
    std::string error;
    const bool plain_built = BuildPostShaderProgramSource(
        "#if defined(VERTEX)\nvoid main(){}\n#elif defined(FRAGMENT)\n"
        "void main(){}\n#endif\n",
        &plain, &error);
    const bool plain_ok = plain_built &&
        StartsWith(plain.vertex,
                   "#define VERTEX\n#define PARAMETER_UNIFORM\n#if defined") &&
        StartsWith(plain.fragment,
                   "#define FRAGMENT\n#define PARAMETER_UNIFORM\n#if ") &&
        plain.parameters.empty();

    PostShaderProgramSource versioned;
    const bool versioned_built = BuildPostShaderProgramSource(
        "#version 120\r\n#if defined(VERTEX)\n#elif defined(FRAGMENT)\n#endif",
        &versioned, &error);
    const bool versioned_ok = versioned_built &&
        StartsWith(versioned.vertex,
                   "#version 120\r\n#define VERTEX\n#define PARAMETER_UNIFORM\n"
                   "#if defined(VERTEX)") &&
        StartsWith(versioned.fragment, "#version 120\r\n#define FRAGMENT\n");

    // A file of a `#version` line alone still gets a line break before the
    // defines.
    PostShaderProgramSource version_only;
    const bool version_only_ok = BuildPostShaderProgramSource(
        "  #version 110 // VERTEX FRAGMENT", &version_only, &error) &&
        StartsWith(version_only.vertex,
                   "  #version 110 // VERTEX FRAGMENT\n#define VERTEX\n");

    return plain_ok && versioned_ok && version_only_ok;
}

bool ProbeParameters()
{
    PostShaderProgramSource source;
    std::string error;
    const bool built = BuildPostShaderProgramSource(
        "#pragma parameter STRENGTH \"Line strength\" 0.5 0.0 1.0 0.05\n"
        "  #pragma  parameter GLOW \"Glow, \\ odd chars\" 1 0 2\r\n"
        "#pragma parameter STRENGTH \"Again\" 0.9 0.0 1.0 0.1\n"
        "#pragma optimize(on)\n"
        "#if defined(VERTEX)\n#elif defined(FRAGMENT)\n#endif\n",
        &source, &error);
    const bool parsed = built && source.parameters.size() == 2U &&
        source.parameters[0].name == "STRENGTH" &&
        source.parameters[0].description == "Line strength" &&
        Near(source.parameters[0].initial, 0.5F) &&
        Near(source.parameters[0].value, 0.5F) &&
        Near(source.parameters[0].minimum, 0.0F) &&
        Near(source.parameters[0].maximum, 1.0F) &&
        Near(source.parameters[0].step, 0.05F) &&
        source.parameters[1].name == "GLOW" &&
        source.parameters[1].description == "Glow, \\ odd chars" &&
        Near(source.parameters[1].maximum, 2.0F) &&
        Near(source.parameters[1].step, 0.0F);

    // Every malformed shape fails with a reason rather than guessing.
    const char* const malformed[] = {
        "#pragma parameter 9BAD \"x\" 0 0 1\nVERTEX FRAGMENT",
        "#pragma parameter NAME unquoted 0 0 1\nVERTEX FRAGMENT",
        "#pragma parameter NAME \"open 0 0 1\nVERTEX FRAGMENT",
        "#pragma parameter NAME \"x\" 0 0\nVERTEX FRAGMENT",
        "#pragma parameter NAME \"x\" 0 2 1\nVERTEX FRAGMENT",
    };
    bool rejected = true;
    for (const char* text : malformed)
    {
        PostShaderProgramSource ignored;
        std::string reason;
        rejected = rejected &&
            !BuildPostShaderProgramSource(text, &ignored, &reason) &&
            !reason.empty();
    }
    PostShaderProgramSource ignored;
    const bool empty_rejected =
        !BuildPostShaderProgramSource("", &ignored, &error) &&
        !BuildPostShaderProgramSource("void main(){}", &ignored, &error) &&
        !BuildPostShaderProgramSource("VERTEX FRAGMENT", nullptr, &error);
    return parsed && rejected && empty_rejected;
}

// Issue #90: the five built-ins in order, each loading as a chain whose
// passes all assemble. crt and scanline are one pass into the window with
// parameters and the project's notice; fsr is two passes, the last into the
// window, with FSR_SHARPNESS; scalefx and anime4k are five passes each, the
// last at a size of its own (so not into the window), scalefx with its
// threshold parameters and anime4k with none, every file carrying its MIT
// notice.
bool ProbeBuiltins()
{
    const std::vector<PostShaderEntry> entries =
        ListPostShaders(std::filesystem::path("repiu_no_such_shader_dir"));
    bool ok = entries.size() == 5U && entries[0].id == "crt" &&
        entries[0].builtin && entries[1].id == "scanline" &&
        entries[1].builtin && entries[2].id == "fsr" && entries[2].builtin &&
        entries[3].id == "scalefx" && entries[3].builtin &&
        entries[4].id == "anime4k" && entries[4].builtin;
    const struct
    {
        const char* id;
        std::size_t passes;
        bool parameters;
        bool to_window;
        const char* notice;
    } kExpected[] = {
        {"crt", 1, true, true, "BSD 3-Clause"},
        {"scanline", 1, true, true, "BSD 3-Clause"},
        {"fsr", 2, true, true, "Advanced Micro Devices"},
        {"scalefx", 5, true, false, "Sp00kyFox"},
        {"anime4k", 5, false, false, "MIT License"},
    };
    for (const auto& expected : kExpected)
    {
        const PostShaderEntry* entry = FindPostShader(entries, expected.id);
        PostShaderPreset preset;
        std::string error;
        ok = ok && entry != nullptr &&
            LoadPostShaderPreset(*entry, &preset, &error) &&
            preset.passes.size() == expected.passes &&
            preset.passes.back().to_window == expected.to_window;
        bool parameters = false;
        for (const PostShaderPass& pass : preset.passes)
        {
            PostShaderProgramSource source;
            ok = ok && pass.text.find(expected.notice) != std::string::npos &&
                BuildPostShaderProgramSource(pass.text, &source, &error);
            parameters = parameters || !source.parameters.empty();
        }
        ok = ok && parameters == expected.parameters;
    }
    // anime4k's shape: four convolution layers at the game's size in float
    // targets, then depth-to-space at twice the size, OrigTexture read
    // linearly (pass 0's filter).
    const PostShaderEntry* anime4k = FindPostShader(entries, "anime4k");
    PostShaderPreset preset;
    std::string error;
    if (anime4k != nullptr && LoadPostShaderPreset(*anime4k, &preset, &error) &&
        preset.passes.size() == 5U)
    {
        for (std::size_t index = 0; index < 4U; ++index)
        {
            const PostShaderPass& pass = preset.passes[index];
            ok = ok && pass.float_framebuffer && !pass.to_window &&
                pass.scale_type_x == PostShaderScaleType::kSource &&
                Near(pass.scale_x, 1.0F) && Near(pass.scale_y, 1.0F) &&
                pass.filter_specified && pass.filter_linear == (index == 0U);
        }
        const PostShaderPass& last = preset.passes[4];
        ok = ok && !last.float_framebuffer && !last.to_window &&
            !last.filter_linear && Near(last.scale_x, 2.0F) &&
            Near(last.scale_y, 2.0F) &&
            last.text.find("OrigTexture") != std::string::npos;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        PostShaderPassSize(last, 640U, 480U, 2880U, 2160U, &width, &height);
        ok = ok && width == 1280U && height == 960U;
    }
    else
    {
        ok = false;
    }
    return ok;
}

// Files a preset may name, by relative path.
PostShaderFileReader ReaderOf(std::map<std::string, std::string> files)
{
    return [files = std::move(files)](const std::string& path,
                                      std::string* text, std::string* error)
    {
        const auto found = files.find(path);
        if (found == files.end())
        {
            *error = "no file " + path;
            return false;
        }
        *text = found->second;
        return true;
    };
}

// Issue #90: the `.glslp` subset. Keys are read with comments and quotes; a
// pass with no scale keeps its input's size, an unscaled last pass draws
// into the window.
bool ProbePresetParse()
{
    const char* kText =
        "shaders = \"3\"   # three passes\n"
        "shader0 = a.glsl\n"
        "scale_type0 = source\n"
        "scale0 = 2.0\n"
        "float_framebuffer0 = true\n"
        "filter_linear0 = false\n"
        "shader1 = sub/b.glsl\n"
        "scale_type_x1 = absolute\n"
        "scale_x1 = 320\n"
        "scale_type_y1 = viewport\n"
        "scale_y1 = 0.5\n"
        "shader2 = c.glsl\n"
        "filter_linear2 = true\n";
    PostShaderPreset preset;
    std::string error;
    if (!ParsePostShaderPreset(
            kText,
            ReaderOf({{"a.glsl", "A"}, {"sub/b.glsl", "B"}, {"c.glsl", "C"}}),
            &preset, &error) ||
        preset.passes.size() != 3U)
    {
        return false;
    }
    const PostShaderPass& a = preset.passes[0];
    const PostShaderPass& b = preset.passes[1];
    const PostShaderPass& c = preset.passes[2];
    bool ok = a.text == "A" &&
        a.scale_type_x == PostShaderScaleType::kSource &&
        a.scale_type_y == PostShaderScaleType::kSource &&
        Near(a.scale_x, 2.0F) && a.float_framebuffer && !a.filter_linear &&
        a.filter_specified && !a.to_window;
    ok = ok && b.path == "sub/b.glsl" && b.text == "B" &&
        b.scale_type_x == PostShaderScaleType::kAbsolute &&
        b.scale_type_y == PostShaderScaleType::kViewport &&
        Near(b.scale_x, 320.0F) && Near(b.scale_y, 0.5F) &&
        !b.float_framebuffer && b.filter_linear && !b.filter_specified &&
        !b.to_window;
    ok = ok && c.filter_linear && c.filter_specified && c.to_window;

    // Sizes from a 640x480 input shown at 2880x2160.
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    PostShaderPassSize(a, 640U, 480U, 2880U, 2160U, &width, &height);
    ok = ok && width == 1280U && height == 960U;
    PostShaderPassSize(b, 1280U, 960U, 2880U, 2160U, &width, &height);
    ok = ok && width == 320U && height == 1080U;
    PostShaderPassSize(c, 320U, 1080U, 2880U, 2160U, &width, &height);
    ok = ok && width == 2880U && height == 2160U;
    return ok;
}

// What is refused: no count, a count out of range, a missing shader line, a
// file the reader lacks, an unknown scale type, a scale that is not a
// positive number, a flag that is not true or false. An unscaled middle pass
// keeps its input's size rather than drawing into the window.
bool ProbePresetErrors()
{
    const PostShaderFileReader reader =
        ReaderOf({{"a.glsl", "A"}, {"b.glsl", "B"}});
    PostShaderPreset preset;
    std::string error;
    const char* const refused[] = {
        "shader0 = a.glsl\n",
        "shaders = 0\n",
        "shaders = 17\n",
        "shaders = 2\nshader0 = a.glsl\n",
        "shaders = 1\nshader0 = missing.glsl\n",
        "shaders = 1\nshader0 = a.glsl\nscale_type0 = window\n",
        "shaders = 1\nshader0 = a.glsl\nscale0 = -1\n",
        "shaders = 1\nshader0 = a.glsl\nscale0 = two\n",
        "shaders = 1\nshader0 = a.glsl\nfloat_framebuffer0 = yes\n",
    };
    bool ok = true;
    for (const char* text : refused)
    {
        error.clear();
        ok = ok && !ParsePostShaderPreset(text, reader, &preset, &error) &&
            !error.empty();
    }
    ok = ok && !ParsePostShaderPreset("shaders = 1\nshader0 = missing.glsl\n",
                                      reader, &preset, &error) &&
        error.find("missing.glsl") != std::string::npos;
    ok = ok && !ParsePostShaderPreset("shaders = 1\nshader0 = a.glsl\n",
                                      nullptr, &preset, &error);
    // A scale without a scale type is ignored, as libretro reads it.
    ok = ok &&
        ParsePostShaderPreset(
            "shaders = 2\nshader0 = a.glsl\nshader1 = b.glsl\nscale1 = 3\n",
            reader, &preset, &error) &&
        preset.passes.size() == 2U && !preset.passes[0].to_window &&
        preset.passes[1].to_window;
    ok = ok &&
        ParsePostShaderPreset("shaders = 2\nshader0 = a.glsl\nshader1 = "
                              "b.glsl\nscale_type1 = source\nscale1 = 3\n",
                              reader, &preset, &error) &&
        preset.passes.size() == 2U && !preset.passes[0].to_window &&
        !preset.passes[1].to_window;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    if (ok)
    {
        PostShaderPassSize(preset.passes[0], 640U, 480U, 1280U, 960U, &width,
                           &height);
        ok = ok && width == 640U && height == 480U;
        PostShaderPassSize(preset.passes[1], 640U, 480U, 1280U, 960U, &width,
                           &height);
        ok = ok && width == 1920U && height == 1440U;
    }
    return ok;
}

// libretro's quirks: a '#' inside a quoted value is not a comment, a UTF-8
// BOM is skipped, and the unsuffixed scale keys win over their _x/_y forms.
bool ProbePresetLibretroRules()
{
    const PostShaderFileReader reader =
        ReaderOf({{"pass#1.glsl", "P"}, {"a.glsl", "A"}});
    PostShaderPreset preset;
    std::string error;
    bool ok = ParsePostShaderPreset(
                  "shaders = 1\nshader0 = \"pass#1.glsl\" # the file\n",
                  reader, &preset, &error) &&
        preset.passes.size() == 1U && preset.passes[0].text == "P";
    ok = ok &&
        ParsePostShaderPreset("\xEF\xBB\xBFshaders = 1\nshader0 = a.glsl\n",
                              reader, &preset, &error) &&
        preset.passes.size() == 1U && preset.passes[0].text == "A";
    ok = ok &&
        ParsePostShaderPreset(
            "shaders = 1\nshader0 = a.glsl\nscale_type_x0 = absolute\n"
            "scale_type0 = source\nscale_x0 = 4.0\nscale0 = 2.0\n"
            "filter_linear0 = false\n",
            reader, &preset, &error) &&
        preset.passes.size() == 1U &&
        preset.passes[0].scale_type_x == PostShaderScaleType::kSource &&
        Near(preset.passes[0].scale_x, 2.0F) &&
        Near(preset.passes[0].scale_y, 2.0F) &&
        preset.passes[0].filter_specified && !preset.passes[0].filter_linear;
    ok = ok &&
        ParsePostShaderPreset("shaders = 1\nshader0 = a.glsl\n", reader,
                              &preset, &error) &&
        preset.passes.size() == 1U && !preset.passes[0].filter_specified;
    return ok;
}

// A single file is one pass into the window; a size never falls below one
// pixel.
bool ProbePresetSinglePass()
{
    const PostShaderPreset preset = SinglePassPreset("my.glsl", "text");
    bool ok = preset.passes.size() == 1U && preset.passes[0].to_window &&
        preset.passes[0].text == "text" && preset.passes[0].path == "my.glsl";
    PostShaderPass tiny;
    tiny.scale_type_x = PostShaderScaleType::kSource;
    tiny.scale_x = 0.001F;
    tiny.scale_y = 0.001F;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    PostShaderPassSize(tiny, 640U, 480U, 0U, 0U, &width, &height);
    ok = ok && width == 1U && height == 1U;
    PostShaderPassSize(preset.passes[0], 640U, 480U, 0U, 0U, &width,
                       &height);
    ok = ok && width == 1U && height == 1U;
    return ok;
}

bool ProbeCatalog()
{
    std::error_code error;
    const std::filesystem::path root =
        std::filesystem::temp_directory_path(error) / "repiu_post_shader_probe";
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root / "nested.glsl", error);
    std::filesystem::create_directories(root / "sub", error);
    const auto write = [&root](const char* name, const char* text) {
        std::ofstream stream(root / name, std::ios::binary | std::ios::trunc);
        stream << text;
    };
    write("zeta.glsl", "zeta");
    write("alpha.glsl", "alpha");
    write("readme.txt", "not a shader");
    // Issue #90: a preset beside its pass files, one in a subdirectory.
    write("chain.glslp",
          "shaders = 2\nshader0 = sub/first.glsl\nscale_type0 = source\n"
          "scale0 = 2\nshader1 = alpha.glsl\n");
    write("sub/first.glsl", "first");
    write("broken.glslp", "shaders = 1\nshader0 = sub/gone.glsl\n");

    const std::vector<PostShaderEntry> entries = ListPostShaders(root);
    const bool listed = entries.size() == 9U && entries[0].id == "crt" &&
        entries[1].id == "scanline" && entries[2].id == "fsr" &&
        entries[3].id == "scalefx" && entries[4].id == "anime4k" &&
        entries[5].id == "alpha.glsl" && !entries[5].builtin &&
        entries[6].id == "broken.glslp" && entries[7].id == "chain.glslp" &&
        !entries[7].builtin && entries[8].id == "zeta.glsl";

    PostShaderPreset preset;
    std::string load_error;
    const PostShaderEntry* alpha = FindPostShader(entries, "alpha.glsl");
    const bool found = alpha != nullptr &&
        LoadPostShaderPreset(*alpha, &preset, &load_error) &&
        preset.passes.size() == 1U && preset.passes[0].text == "alpha" &&
        preset.passes[0].to_window &&
        FindPostShader(entries, "crt") == &entries[0] &&
        FindPostShader(entries, engine::kPostShaderNoneId) == nullptr &&
        FindPostShader(entries, "alpha") == nullptr;

    const PostShaderEntry* chain = FindPostShader(entries, "chain.glslp");
    const bool chained = chain != nullptr &&
        LoadPostShaderPreset(*chain, &preset, &load_error) &&
        preset.passes.size() == 2U && preset.passes[0].text == "first" &&
        preset.passes[0].path == "sub/first.glsl" &&
        !preset.passes[0].to_window && Near(preset.passes[0].scale_x, 2.0F) &&
        preset.passes[1].text == "alpha" && preset.passes[1].to_window;

    const PostShaderEntry* broken = FindPostShader(entries, "broken.glslp");
    load_error.clear();
    const bool broken_reported = broken != nullptr &&
        !LoadPostShaderPreset(*broken, &preset, &load_error) &&
        load_error.find("gone.glsl") != std::string::npos;

    PostShaderEntry missing;
    missing.id = "gone.glsl";
    missing.path = root / "gone.glsl";
    load_error.clear();
    const bool missing_reported =
        !LoadPostShaderPreset(missing, &preset, &load_error) &&
        !load_error.empty();

    std::filesystem::remove_all(root, error);
    return listed && found && chained && broken_reported && missing_reported;
}

}  // namespace

bool RunPostShaderProbe()
{
    const bool assembly = ProbeAssembly();
    const bool parameters = ProbeParameters();
    const bool builtins = ProbeBuiltins();
    const bool preset = ProbePresetParse() && ProbePresetErrors() &&
        ProbePresetLibretroRules() && ProbePresetSinglePass();
    const bool catalog = ProbeCatalog();
    // The launcher publishes into the variable the engine reads.
    const bool variable = std::strcmp(launcher::kLauncherPostShaderVariable,
                                      engine::kPostShaderVariable) == 0;
    const bool all =
        assembly && parameters && builtins && preset && catalog && variable;
    std::cout << "post_shader_assembly=" << (assembly ? "true" : "false")
              << "\npost_shader_parameters=" << (parameters ? "true" : "false")
              << "\npost_shader_builtins=" << (builtins ? "true" : "false")
              << "\npost_shader_preset=" << (preset ? "true" : "false")
              << "\npost_shader_catalog=" << (catalog ? "true" : "false")
              << "\npost_shader_variable=" << (variable ? "true" : "false")
              << "\npost_shader_all=" << (all ? "true" : "false")
              << '\n';
    return all;
}

}  // namespace repiu::tools
