#include "repiu/engine/gl_shader_dialect.h"

#include <cctype>

namespace repiu::engine
{
namespace
{

constexpr std::string_view kFragmentOutput = "repiuFragColor";

bool IsWordCharacter(char character)
{
    return std::isalnum(static_cast<unsigned char>(character)) != 0 ||
        character == '_';
}

// Replaces every whole-word occurrence of `word` with `replacement`.
std::string ReplaceWord(std::string_view text, std::string_view word,
                        std::string_view replacement)
{
    std::string result;
    result.reserve(text.size());
    std::size_t position = 0;
    while (position < text.size())
    {
        const std::size_t found = text.find(word, position);
        if (found == std::string_view::npos)
        {
            result.append(text.substr(position));
            break;
        }
        const std::size_t end = found + word.size();
        const bool starts_word = found == 0 || !IsWordCharacter(text[found - 1]);
        const bool ends_word = end == text.size() || !IsWordCharacter(text[end]);
        result.append(text.substr(position, found - position));
        result.append(starts_word && ends_word ? replacement : word);
        position = end;
    }
    return result;
}

// Drops a `#version` line at the start (after blank lines), which the header
// replaces.
std::string_view DropVersionLine(std::string_view text)
{
    std::size_t line_start = 0;
    while (line_start < text.size())
    {
        std::size_t line_end = text.find('\n', line_start);
        if (line_end == std::string_view::npos)
        {
            line_end = text.size();
        }
        std::string_view line = text.substr(line_start, line_end - line_start);
        std::size_t first = 0;
        while (first < line.size() &&
               std::isspace(static_cast<unsigned char>(line[first])) != 0)
        {
            ++first;
        }
        if (first == line.size())
        {
            line_start = line_end + 1U;
            continue;
        }
        if (line.substr(first).rfind("#version", 0) == 0)
        {
            return line_end < text.size() ? text.substr(line_end + 1U)
                                           : std::string_view{};
        }
        break;
    }
    return text;
}

// Whether a fragment body declares an output of its own: a line beginning
// with `out` that declares a `vec4` (libretro's `#version 130` style,
// `out COMPAT_PRECISION vec4 FragColor;`). Such a shader never writes
// `gl_FragColor` in the branch the header selects, and a second output
// without a layout location is an error in ES, so the header declares none
// for it (issue #90).
bool DeclaresFragmentOutput(std::string_view body)
{
    std::size_t line_start = 0;
    while (line_start < body.size())
    {
        std::size_t line_end = body.find('\n', line_start);
        if (line_end == std::string_view::npos)
        {
            line_end = body.size();
        }
        std::string_view line = body.substr(line_start, line_end - line_start);
        line_start = line_end + 1U;
        std::size_t first = 0;
        while (first < line.size() &&
               std::isspace(static_cast<unsigned char>(line[first])) != 0)
        {
            ++first;
        }
        line = line.substr(first);
        if (line.size() > 3U && line.substr(0, 3) == "out" &&
            std::isspace(static_cast<unsigned char>(line[3])) != 0 &&
            line.find("vec4") != std::string_view::npos &&
            line.find(';') != std::string_view::npos)
        {
            return true;
        }
    }
    return false;
}

}  // namespace

std::string GlslHeader(bool es)
{
    if (es)
    {
        return "#version 300 es\n"
               "precision highp float;\n"
               "precision highp int;\n";
    }
    return "#version 330 core\n";
}

const char* ImGuiGlslVersion(bool es)
{
    return es ? "#version 300 es" : "#version 330 core";
}

std::string AdaptLegacyGlslStage(std::string_view source, bool es,
                                 GlslStage stage)
{
    std::string body(DropVersionLine(source));
    const bool fragment = stage == GlslStage::kFragment;
    body = ReplaceWord(body, "attribute", "in");
    body = ReplaceWord(body, "varying", fragment ? "in" : "out");
    body = ReplaceWord(body, "texture2D", "texture");
    std::string result = GlslHeader(es);
    if (fragment)
    {
        body = ReplaceWord(body, "gl_FragColor", kFragmentOutput);
        if (!DeclaresFragmentOutput(body))
        {
            result.append("out vec4 ");
            result.append(kFragmentOutput);
            result.append(";\n");
        }
    }
    result.append(body);
    return result;
}

}  // namespace repiu::engine
