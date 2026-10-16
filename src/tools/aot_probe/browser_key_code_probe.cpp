#include "browser_key_code_probe.h"

#include "repiu/input/browser_key_codes.h"

#include <SDL3/SDL_scancode.h>

#include <iostream>

namespace repiu::tools
{
namespace
{

using input::ScancodeForBrowserKeyCode;

bool Is(const char* code, const SDL_Scancode expected)
{
    return ScancodeForBrowserKeyCode(code) == expected;
}

// Letters, digits (1 to 9 then 0), F1 to F24 across SDL's two runs, the
// keypad digits.
bool ProbeRanges()
{
    return Is("KeyA", SDL_SCANCODE_A) && Is("KeyQ", SDL_SCANCODE_Q) && Is("KeyZ", SDL_SCANCODE_Z) &&
        Is("Digit1", SDL_SCANCODE_1) && Is("Digit9", SDL_SCANCODE_9) && Is("Digit0", SDL_SCANCODE_0) &&
        Is("F1", SDL_SCANCODE_F1) && Is("F5", SDL_SCANCODE_F5) && Is("F12", SDL_SCANCODE_F12) &&
        Is("F13", SDL_SCANCODE_F13) && Is("F24", SDL_SCANCODE_F24) &&
        Is("Numpad1", SDL_SCANCODE_KP_1) && Is("Numpad9", SDL_SCANCODE_KP_9) &&
        Is("Numpad0", SDL_SCANCODE_KP_0);
}

bool ProbeNamed()
{
    return Is("Tab", SDL_SCANCODE_TAB) && Is("Enter", SDL_SCANCODE_RETURN) &&
        Is("NumpadEnter", SDL_SCANCODE_KP_ENTER) && Is("Home", SDL_SCANCODE_HOME) &&
        Is("PageUp", SDL_SCANCODE_PAGEUP) && Is("End", SDL_SCANCODE_END) &&
        Is("PageDown", SDL_SCANCODE_PAGEDOWN) && Is("ArrowUp", SDL_SCANCODE_UP) &&
        Is("Quote", SDL_SCANCODE_APOSTROPHE) && Is("Backquote", SDL_SCANCODE_GRAVE) &&
        Is("AltLeft", SDL_SCANCODE_LALT) && Is("MetaRight", SDL_SCANCODE_RGUI) &&
        Is("Lang1", SDL_SCANCODE_LANG1) && Is("NumpadClear", SDL_SCANCODE_KP_CLEAR);
}

// Every key of the default JAMMA layout (jamma_input_bindings.cpp) by the
// code a browser sends for it.
bool ProbeDefaultLayout()
{
    return Is("KeyQ", SDL_SCANCODE_Q) && Is("KeyE", SDL_SCANCODE_E) && Is("KeyZ", SDL_SCANCODE_Z) &&
        Is("KeyC", SDL_SCANCODE_C) && Is("KeyS", SDL_SCANCODE_S) && Is("F5", SDL_SCANCODE_F5) &&
        Is("F1", SDL_SCANCODE_F1) && Is("F2", SDL_SCANCODE_F2) && Is("F3", SDL_SCANCODE_F3) &&
        Is("Numpad7", SDL_SCANCODE_KP_7) && Is("Numpad9", SDL_SCANCODE_KP_9) &&
        Is("Numpad1", SDL_SCANCODE_KP_1) && Is("Numpad3", SDL_SCANCODE_KP_3) &&
        Is("Numpad5", SDL_SCANCODE_KP_5);
}

bool ProbeUnknown()
{
    return Is("", SDL_SCANCODE_UNKNOWN) && Is("Key", SDL_SCANCODE_UNKNOWN) &&
        Is("Keya", SDL_SCANCODE_UNKNOWN) && Is("KeyAB", SDL_SCANCODE_UNKNOWN) &&
        Is("F0", SDL_SCANCODE_UNKNOWN) && Is("F25", SDL_SCANCODE_UNKNOWN) &&
        Is("F05", SDL_SCANCODE_UNKNOWN) && Is("Digit10", SDL_SCANCODE_UNKNOWN) &&
        Is("Numpad", SDL_SCANCODE_UNKNOWN) && Is("tab", SDL_SCANCODE_UNKNOWN) &&
        Is("Unidentified", SDL_SCANCODE_UNKNOWN);
}

}  // namespace

bool RunBrowserKeyCodeProbe()
{
    const bool ranges = ProbeRanges();
    const bool named = ProbeNamed();
    const bool layout = ProbeDefaultLayout();
    const bool unknown = ProbeUnknown();
    const bool all = ranges && named && layout && unknown;
    std::cout << "browser_key_code_ranges=" << (ranges ? "true" : "false")
              << "\nbrowser_key_code_named=" << (named ? "true" : "false")
              << "\nbrowser_key_code_default_layout=" << (layout ? "true" : "false")
              << "\nbrowser_key_code_unknown=" << (unknown ? "true" : "false")
              << "\nbrowser_key_code_all=" << (all ? "true" : "false") << "\n";
    return all;
}

}  // namespace repiu::tools
