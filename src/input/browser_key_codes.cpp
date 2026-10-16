#include "repiu/input/browser_key_codes.h"

// Issue #76. KeyboardEvent.code names (the UI Events KeyboardEvent code
// values, https://www.w3.org/TR/uievents-code/) to SDL scancodes (the USB HID
// usages SDL_scancode.h follows). The ranges -- letters, digits, F1 to F24,
// the keypad digits -- are computed; the rest is the table.

#include <array>
#include <cstddef>
#include <utility>

namespace repiu::input
{
namespace
{

struct Entry
{
    std::string_view code;
    SDL_Scancode scancode;
};

constexpr std::array<Entry, 66> kNamed = {{
    {"Enter", SDL_SCANCODE_RETURN},
    {"Escape", SDL_SCANCODE_ESCAPE},
    {"Backspace", SDL_SCANCODE_BACKSPACE},
    {"Tab", SDL_SCANCODE_TAB},
    {"Space", SDL_SCANCODE_SPACE},
    {"Minus", SDL_SCANCODE_MINUS},
    {"Equal", SDL_SCANCODE_EQUALS},
    {"BracketLeft", SDL_SCANCODE_LEFTBRACKET},
    {"BracketRight", SDL_SCANCODE_RIGHTBRACKET},
    {"Backslash", SDL_SCANCODE_BACKSLASH},
    {"Semicolon", SDL_SCANCODE_SEMICOLON},
    {"Quote", SDL_SCANCODE_APOSTROPHE},
    {"Backquote", SDL_SCANCODE_GRAVE},
    {"Comma", SDL_SCANCODE_COMMA},
    {"Period", SDL_SCANCODE_PERIOD},
    {"Slash", SDL_SCANCODE_SLASH},
    {"CapsLock", SDL_SCANCODE_CAPSLOCK},
    {"PrintScreen", SDL_SCANCODE_PRINTSCREEN},
    {"ScrollLock", SDL_SCANCODE_SCROLLLOCK},
    {"Pause", SDL_SCANCODE_PAUSE},
    {"Insert", SDL_SCANCODE_INSERT},
    {"Home", SDL_SCANCODE_HOME},
    {"PageUp", SDL_SCANCODE_PAGEUP},
    {"Delete", SDL_SCANCODE_DELETE},
    {"End", SDL_SCANCODE_END},
    {"PageDown", SDL_SCANCODE_PAGEDOWN},
    {"ArrowRight", SDL_SCANCODE_RIGHT},
    {"ArrowLeft", SDL_SCANCODE_LEFT},
    {"ArrowDown", SDL_SCANCODE_DOWN},
    {"ArrowUp", SDL_SCANCODE_UP},
    {"NumLock", SDL_SCANCODE_NUMLOCKCLEAR},
    {"NumpadDivide", SDL_SCANCODE_KP_DIVIDE},
    {"NumpadMultiply", SDL_SCANCODE_KP_MULTIPLY},
    {"NumpadSubtract", SDL_SCANCODE_KP_MINUS},
    {"NumpadAdd", SDL_SCANCODE_KP_PLUS},
    {"NumpadEnter", SDL_SCANCODE_KP_ENTER},
    {"NumpadDecimal", SDL_SCANCODE_KP_PERIOD},
    {"NumpadEqual", SDL_SCANCODE_KP_EQUALS},
    {"NumpadComma", SDL_SCANCODE_KP_COMMA},
    {"NumpadClear", SDL_SCANCODE_KP_CLEAR},
    {"IntlBackslash", SDL_SCANCODE_NONUSBACKSLASH},
    {"IntlRo", SDL_SCANCODE_INTERNATIONAL1},
    {"KanaMode", SDL_SCANCODE_INTERNATIONAL2},
    {"IntlYen", SDL_SCANCODE_INTERNATIONAL3},
    {"Convert", SDL_SCANCODE_INTERNATIONAL4},
    {"NonConvert", SDL_SCANCODE_INTERNATIONAL5},
    // The Korean Han/Yeong and Hanja keys.
    {"Lang1", SDL_SCANCODE_LANG1},
    {"Lang2", SDL_SCANCODE_LANG2},
    {"ContextMenu", SDL_SCANCODE_APPLICATION},
    {"Power", SDL_SCANCODE_POWER},
    {"Help", SDL_SCANCODE_HELP},
    {"AudioVolumeMute", SDL_SCANCODE_MUTE},
    {"AudioVolumeUp", SDL_SCANCODE_VOLUMEUP},
    {"AudioVolumeDown", SDL_SCANCODE_VOLUMEDOWN},
    {"MediaTrackNext", SDL_SCANCODE_MEDIA_NEXT_TRACK},
    {"MediaTrackPrevious", SDL_SCANCODE_MEDIA_PREVIOUS_TRACK},
    {"MediaStop", SDL_SCANCODE_MEDIA_STOP},
    {"MediaPlayPause", SDL_SCANCODE_MEDIA_PLAY_PAUSE},
    {"ControlLeft", SDL_SCANCODE_LCTRL},
    {"ShiftLeft", SDL_SCANCODE_LSHIFT},
    {"AltLeft", SDL_SCANCODE_LALT},
    {"MetaLeft", SDL_SCANCODE_LGUI},
    {"ControlRight", SDL_SCANCODE_RCTRL},
    {"ShiftRight", SDL_SCANCODE_RSHIFT},
    {"AltRight", SDL_SCANCODE_RALT},
    {"MetaRight", SDL_SCANCODE_RGUI},
}};

// "<prefix><n>" with n in [first, last]: the number, or -1.
int NumberAfter(const std::string_view code, const std::string_view prefix,
                const int first, const int last)
{
    if (code.size() <= prefix.size() || code.substr(0, prefix.size()) != prefix)
    {
        return -1;
    }
    int value = 0;
    for (const char digit : code.substr(prefix.size()))
    {
        if (digit < '0' || digit > '9' || value > 100)
        {
            return -1;
        }
        value = value * 10 + (digit - '0');
    }
    // No leading zeros: "F05" is not a key.
    if (code.size() > prefix.size() + 1U && code[prefix.size()] == '0')
    {
        return -1;
    }
    return value >= first && value <= last ? value : -1;
}

SDL_Scancode Offset(const SDL_Scancode base, const int by)
{
    return static_cast<SDL_Scancode>(static_cast<int>(base) + by);
}

}  // namespace

SDL_Scancode ScancodeForBrowserKeyCode(const std::string_view code)
{
    // KeyA .. KeyZ
    if (code.size() == 4U && code.substr(0, 3) == "Key" && code[3] >= 'A' && code[3] <= 'Z')
    {
        return Offset(SDL_SCANCODE_A, code[3] - 'A');
    }
    // Digit1 .. Digit9, then Digit0, as on the keyboard and in SDL.
    if (const int digit = NumberAfter(code, "Digit", 0, 9); digit >= 0)
    {
        return digit == 0 ? SDL_SCANCODE_0 : Offset(SDL_SCANCODE_1, digit - 1);
    }
    // Numpad1 .. Numpad9, then Numpad0.
    if (const int digit = NumberAfter(code, "Numpad", 0, 9); digit >= 0)
    {
        return digit == 0 ? SDL_SCANCODE_KP_0 : Offset(SDL_SCANCODE_KP_1, digit - 1);
    }
    // F1 .. F12 and F13 .. F24 are two runs in SDL.
    if (const int number = NumberAfter(code, "F", 1, 24); number >= 0)
    {
        return number <= 12 ? Offset(SDL_SCANCODE_F1, number - 1)
                            : Offset(SDL_SCANCODE_F13, number - 13);
    }
    for (const Entry& entry : kNamed)
    {
        if (entry.code == code)
        {
            return entry.scancode;
        }
    }
    return SDL_SCANCODE_UNKNOWN;
}

}  // namespace repiu::input
