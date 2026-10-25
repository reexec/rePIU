#include "repiu/engine/post_shader_catalog.h"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <system_error>

namespace repiu::engine
{
namespace
{

// Generated at configure time from the files under src/engine/post_shaders/
// (Task 768, issue #90); defines kPostShaderBuiltinFiles, each a name
// relative to that directory and its text.
#include "repiu/engine/post_shader_builtins.inc"

// A built-in and the embedded file it starts from: a single `.glsl` pass, or
// a `.glslp` preset whose shader names resolve beside it.
struct BuiltinShader
{
    const char* id;
    const char* main_file;
};

constexpr BuiltinShader kBuiltinShaders[] = {
    {"crt", "crt.glsl"},
    {"scanline", "scanline.glsl"},
    {"fsr", "fsr/fsr.glslp"},
    {"scalefx", "scalefx/scalefx.glslp"},
    {"anime4k", "anime4k/anime4k.glslp"},
};

constexpr const char* kShaderDirectoryName = "shaders";
constexpr const char* kShaderExtension = ".glsl";
constexpr const char* kPresetExtension = ".glslp";

bool EndsWith(std::string_view text, std::string_view suffix)
{
    return text.size() >= suffix.size() &&
        text.substr(text.size() - suffix.size()) == suffix;
}

const std::string_view* FindBuiltinFile(std::string_view name)
{
    for (const BuiltinFile& file : kPostShaderBuiltinFiles)
    {
        if (file.name == name)
        {
            return &file.text;
        }
    }
    return nullptr;
}

// `relative` joined to the directory of `base`, '/'-separated, with "./" and
// "dir/../" folded so preset paths name embedded files.
std::string JoinBuiltinPath(std::string_view base, std::string_view relative)
{
    std::vector<std::string> parts;
    const std::size_t slash = base.rfind('/');
    std::string joined = slash == std::string_view::npos
        ? std::string()
        : std::string(base.substr(0, slash + 1));
    joined.append(relative);
    std::size_t start = 0;
    while (start <= joined.size())
    {
        std::size_t end = joined.find_first_of("/\\", start);
        if (end == std::string::npos)
        {
            end = joined.size();
        }
        const std::string part = joined.substr(start, end - start);
        if (part == "..")
        {
            if (!parts.empty())
            {
                parts.pop_back();
            }
        }
        else if (!part.empty() && part != ".")
        {
            parts.push_back(part);
        }
        start = end + 1U;
    }
    std::string result;
    for (const std::string& part : parts)
    {
        result += (result.empty() ? "" : "/") + part;
    }
    return result;
}

bool ReadHostFile(const std::filesystem::path& path, std::string* text,
                  std::string* error)
{
    std::ifstream stream(path, std::ios::binary);
    if (!stream)
    {
        if (error != nullptr)
        {
            *error = "cannot open " + path.string();
        }
        return false;
    }
    std::ostringstream contents;
    contents << stream.rdbuf();
    *text = contents.str();
    return true;
}

}  // namespace

std::filesystem::path ResolvePostShaderDirectory(
    const std::filesystem::path& executable_directory)
{
    std::error_code error;
    const std::filesystem::path working_candidate = kShaderDirectoryName;
    if (std::filesystem::is_directory(working_candidate, error))
    {
        return working_candidate;
    }
    if (!executable_directory.empty())
    {
        const std::filesystem::path executable_candidate =
            executable_directory / kShaderDirectoryName;
        if (std::filesystem::is_directory(executable_candidate, error))
        {
            return executable_candidate;
        }
    }
    return working_candidate;
}

std::vector<PostShaderEntry> ListPostShaders(
    const std::filesystem::path& directory)
{
    std::vector<PostShaderEntry> entries;
    for (const BuiltinShader& builtin : kBuiltinShaders)
    {
        PostShaderEntry entry;
        entry.id = builtin.id;
        entry.builtin = true;
        entries.push_back(std::move(entry));
    }

    std::vector<PostShaderEntry> files;
    std::error_code error;
    std::filesystem::directory_iterator iterator(directory, error);
    const std::filesystem::directory_iterator end;
    while (!error && iterator != end)
    {
        const std::filesystem::directory_entry& item = *iterator;
        std::error_code status_error;
        if (item.is_regular_file(status_error) &&
            (item.path().extension() == kShaderExtension ||
             item.path().extension() == kPresetExtension))
        {
            PostShaderEntry entry;
            entry.id = item.path().filename().string();
            entry.path = item.path();
            files.push_back(std::move(entry));
        }
        iterator.increment(error);
    }
    std::sort(files.begin(), files.end(),
              [](const PostShaderEntry& left, const PostShaderEntry& right)
              { return left.id < right.id; });
    for (PostShaderEntry& entry : files)
    {
        entries.push_back(std::move(entry));
    }
    return entries;
}

const PostShaderEntry* FindPostShader(
    const std::vector<PostShaderEntry>& entries, std::string_view id)
{
    for (const PostShaderEntry& entry : entries)
    {
        if (entry.id == id)
        {
            return &entry;
        }
    }
    return nullptr;
}

bool LoadPostShaderPreset(const PostShaderEntry& entry,
                          PostShaderPreset* preset, std::string* error)
{
    std::string scratch;
    if (error == nullptr)
    {
        error = &scratch;
    }
    if (preset == nullptr)
    {
        *error = "no destination for the shader";
        return false;
    }
    if (entry.builtin)
    {
        const BuiltinShader* builtin = nullptr;
        for (const BuiltinShader& candidate : kBuiltinShaders)
        {
            builtin = entry.id == candidate.id ? &candidate : builtin;
        }
        const std::string_view* main =
            builtin == nullptr ? nullptr : FindBuiltinFile(builtin->main_file);
        if (main == nullptr)
        {
            *error = "no built-in shader named " + entry.id;
            return false;
        }
        if (!EndsWith(builtin->main_file, kPresetExtension))
        {
            *preset = SinglePassPreset(builtin->main_file, std::string(*main));
            return true;
        }
        const std::string base = builtin->main_file;
        return ParsePostShaderPreset(
            *main,
            [&base](const std::string& relative, std::string* text,
                    std::string* read_error)
            {
                const std::string name = JoinBuiltinPath(base, relative);
                const std::string_view* file = FindBuiltinFile(name);
                if (file == nullptr)
                {
                    *read_error = "no built-in shader file " + name;
                    return false;
                }
                text->assign(*file);
                return true;
            },
            preset, error);
    }
    std::string text;
    if (!ReadHostFile(entry.path, &text, error))
    {
        return false;
    }
    if (entry.path.extension() != kPresetExtension)
    {
        *preset = SinglePassPreset(entry.path.string(), std::move(text));
        return true;
    }
    const std::filesystem::path base = entry.path.parent_path();
    return ParsePostShaderPreset(
        text,
        [&base](const std::string& relative, std::string* file_text,
                std::string* read_error)
        {
            return ReadHostFile(base / std::filesystem::path(relative),
                                file_text, read_error);
        },
        preset, error);
}

}  // namespace repiu::engine
