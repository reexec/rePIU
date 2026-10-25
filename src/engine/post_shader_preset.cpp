#include "repiu/engine/post_shader_preset.h"

#include <algorithm>
#include <cmath>
#include <locale>
#include <map>
#include <sstream>

namespace repiu::engine
{
namespace
{

std::string_view Trim(std::string_view text)
{
    while (!text.empty() &&
           (text.front() == ' ' || text.front() == '\t' ||
            text.front() == '\r'))
    {
        text.remove_prefix(1);
    }
    while (!text.empty() &&
           (text.back() == ' ' || text.back() == '\t' || text.back() == '\r'))
    {
        text.remove_suffix(1);
    }
    return text;
}

// `key = value` lines; `#` starts a comment, and a value may be quoted.
std::map<std::string, std::string> ReadKeys(std::string_view text)
{
    std::map<std::string, std::string> keys;
    std::size_t start = 0;
    while (start < text.size())
    {
        std::size_t end = text.find('\n', start);
        if (end == std::string_view::npos)
        {
            end = text.size();
        }
        std::string_view line = text.substr(start, end - start);
        start = end + 1U;
        // A '#' outside quotes starts a comment; one inside a quoted value,
        // such as a file name, is part of it.
        bool quoted = false;
        for (std::size_t index = 0; index < line.size(); ++index)
        {
            if (line[index] == '"')
            {
                quoted = !quoted;
            }
            else if (line[index] == '#' && !quoted)
            {
                line = line.substr(0, index);
                break;
            }
        }
        const std::size_t equals = line.find('=');
        if (equals == std::string_view::npos)
        {
            continue;
        }
        const std::string_view key = Trim(line.substr(0, equals));
        std::string_view value = Trim(line.substr(equals + 1U));
        if (value.size() >= 2U && value.front() == '"' && value.back() == '"')
        {
            value = value.substr(1, value.size() - 2U);
        }
        if (!key.empty())
        {
            keys[std::string(key)] = std::string(value);
        }
    }
    return keys;
}

const std::string* Find(const std::map<std::string, std::string>& keys,
                        const std::string& key)
{
    const auto found = keys.find(key);
    return found == keys.end() ? nullptr : &found->second;
}

bool ParseFloat(const std::string& text, float* value)
{
    std::istringstream stream(text);
    stream.imbue(std::locale::classic());
    float parsed = 0.0F;
    if (!(stream >> parsed) || !std::isfinite(parsed) || parsed <= 0.0F)
    {
        return false;
    }
    stream >> std::ws;
    if (!stream.eof())
    {
        return false;
    }
    *value = parsed;
    return true;
}

bool ParseBool(const std::string& text, bool* value)
{
    if (text == "true" || text == "1")
    {
        *value = true;
        return true;
    }
    if (text == "false" || text == "0")
    {
        *value = false;
        return true;
    }
    return false;
}

bool ParseScaleType(const std::string& text, PostShaderScaleType* value)
{
    if (text == "source")
    {
        *value = PostShaderScaleType::kSource;
        return true;
    }
    if (text == "viewport")
    {
        *value = PostShaderScaleType::kViewport;
        return true;
    }
    if (text == "absolute")
    {
        *value = PostShaderScaleType::kAbsolute;
        return true;
    }
    return false;
}

std::uint32_t ScaledSize(PostShaderScaleType type, float scale,
                         std::uint32_t input, std::uint32_t viewport)
{
    double size = scale;
    if (type == PostShaderScaleType::kSource)
    {
        size = static_cast<double>(input) * scale;
    }
    else if (type == PostShaderScaleType::kViewport)
    {
        size = static_cast<double>(viewport) * scale;
    }
    const double rounded = std::floor(size + 0.5);
    return rounded < 1.0
        ? 1U
        : static_cast<std::uint32_t>(std::min(rounded, 16384.0));
}

}  // namespace

bool ParsePostShaderPreset(std::string_view text,
                           const PostShaderFileReader& read,
                           PostShaderPreset* preset, std::string* error)
{
    std::string scratch;
    if (error == nullptr)
    {
        error = &scratch;
    }
    if (preset == nullptr || !read)
    {
        *error = "no destination or reader for the preset";
        return false;
    }
    // A UTF-8 byte order mark, as some editors save one.
    if (text.size() >= 3 && text[0] == '\xEF' && text[1] == '\xBB' &&
        text[2] == '\xBF')
    {
        text.remove_prefix(3);
    }
    const std::map<std::string, std::string> keys = ReadKeys(text);
    const std::string* count_text = Find(keys, "shaders");
    std::istringstream count_stream(count_text == nullptr ? std::string()
                                                          : *count_text);
    int count = 0;
    if (count_text == nullptr || !(count_stream >> count) || count < 1 ||
        count > 16)
    {
        *error = "preset needs a 'shaders' count from 1 to 16";
        return false;
    }

    PostShaderPreset parsed;
    for (int index = 0; index < count; ++index)
    {
        const std::string suffix = std::to_string(index);
        const auto key = [&suffix](const char* name)
        { return std::string(name) + suffix; };
        PostShaderPass pass;
        const std::string* shader = Find(keys, key("shader"));
        if (shader == nullptr || shader->empty())
        {
            *error = "preset has no " + key("shader");
            return false;
        }
        pass.path = *shader;
        if (!read(pass.path, &pass.text, error))
        {
            return false;
        }

        const std::string* type = Find(keys, key("scale_type"));
        const std::string* type_x = Find(keys, key("scale_type_x"));
        const std::string* type_y = Find(keys, key("scale_type_y"));
        const bool scaled =
            type != nullptr || type_x != nullptr || type_y != nullptr;
        // The unsuffixed key wins over its _x/_y forms, as libretro reads it.
        if ((type_x != nullptr &&
             !ParseScaleType(*type_x, &pass.scale_type_x)) ||
            (type_y != nullptr &&
             !ParseScaleType(*type_y, &pass.scale_type_y)) ||
            (type != nullptr && (!ParseScaleType(*type, &pass.scale_type_x) ||
                                 !ParseScaleType(*type, &pass.scale_type_y))))
        {
            *error = "preset has an unknown scale type for pass " + suffix;
            return false;
        }
        const std::string* scale = Find(keys, key("scale"));
        const std::string* scale_x = Find(keys, key("scale_x"));
        const std::string* scale_y = Find(keys, key("scale_y"));
        if ((scale_x != nullptr && !ParseFloat(*scale_x, &pass.scale_x)) ||
            (scale_y != nullptr && !ParseFloat(*scale_y, &pass.scale_y)) ||
            (scale != nullptr && (!ParseFloat(*scale, &pass.scale_x) ||
                                  !ParseFloat(*scale, &pass.scale_y))))
        {
            *error = "preset has a scale that is not a positive number for "
                     "pass " +
                suffix;
            return false;
        }
        const std::string* filter = Find(keys, key("filter_linear"));
        const std::string* floating = Find(keys, key("float_framebuffer"));
        if ((filter != nullptr && !ParseBool(*filter, &pass.filter_linear)) ||
            (floating != nullptr &&
             !ParseBool(*floating, &pass.float_framebuffer)))
        {
            *error = "preset has a value that is not true or false for pass " +
                suffix;
            return false;
        }
        pass.filter_specified = filter != nullptr;
        // An unscaled last pass draws into the window; an unscaled earlier
        // one keeps its input's size.
        pass.to_window = index + 1 == count && !scaled;
        parsed.passes.push_back(std::move(pass));
    }
    *preset = std::move(parsed);
    return true;
}

PostShaderPreset SinglePassPreset(std::string path, std::string text)
{
    PostShaderPreset preset;
    PostShaderPass pass;
    pass.path = std::move(path);
    pass.text = std::move(text);
    pass.to_window = true;
    preset.passes.push_back(std::move(pass));
    return preset;
}

void PostShaderPassSize(const PostShaderPass& pass, std::uint32_t input_width,
                        std::uint32_t input_height,
                        std::uint32_t viewport_width,
                        std::uint32_t viewport_height, std::uint32_t* width,
                        std::uint32_t* height)
{
    if (pass.to_window)
    {
        *width = std::max<std::uint32_t>(viewport_width, 1U);
        *height = std::max<std::uint32_t>(viewport_height, 1U);
        return;
    }
    *width =
        ScaledSize(pass.scale_type_x, pass.scale_x, input_width, viewport_width);
    *height = ScaledSize(pass.scale_type_y, pass.scale_y, input_height,
                         viewport_height);
}

}  // namespace repiu::engine
