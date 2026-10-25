#include "repiu/launcher/command_line_options.h"

#include <string_view>

namespace repiu::launcher
{
namespace
{

constexpr std::string_view kPostShaderOption = "--post-shader";

}  // namespace

CommandLineOptions ParseCommandLineOptions(
    const std::vector<std::string>& arguments)
{
    CommandLineOptions options;
    bool options_ended = false;
    for (std::size_t index = 0; index < arguments.size(); ++index)
    {
        const std::string& argument = arguments[index];
        if (options_ended)
        {
            options.positional.push_back(argument);
            continue;
        }
        if (argument == "--")
        {
            options_ended = true;
            continue;
        }

        std::string value;
        if (argument == kPostShaderOption)
        {
            if (index + 1 >= arguments.size())
            {
                options.error = "--post-shader needs a shader id "
                                "(none, crt, scanline, fsr, scalefx, anime4k or a "
                                ".glsl/.glslp file in shaders/)";
                return options;
            }
            value = arguments[++index];
        }
        else if (argument.size() > kPostShaderOption.size() &&
                 std::string_view(argument).substr(
                     0, kPostShaderOption.size()) == kPostShaderOption &&
                 argument[kPostShaderOption.size()] == '=')
        {
            value = argument.substr(kPostShaderOption.size() + 1);
        }
        else
        {
            options.positional.push_back(argument);
            continue;
        }

        if (value.empty())
        {
            options.error = "--post-shader was given an empty shader id";
            return options;
        }
        options.has_post_shader = true;
        options.post_shader = value;
    }
    return options;
}

}  // namespace repiu::launcher
