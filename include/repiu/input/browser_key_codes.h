#ifndef REPIU_INPUT_BROWSER_KEY_CODES_H_
#define REPIU_INPUT_BROWSER_KEY_CODES_H_

#include <SDL3/SDL_scancode.h>

#include <string_view>

namespace repiu::input
{

// Issue #76. The SDL scancode for a browser key, named as KeyboardEvent.code
// names it ("KeyQ", "F5", "Numpad7", "Home"): the key's position, whatever the
// layout or NumLock says, which is what SDL's scancodes are too. The browser
// page sends keys this way and the engine turns them into SDL key events, so
// the bindings match as they do natively. SDL_SCANCODE_UNKNOWN for a name the
// table does not know. See docs/design/20261016-i076-browser-input.md,
// decision 2.
[[nodiscard]] SDL_Scancode ScancodeForBrowserKeyCode(std::string_view code);

}  // namespace repiu::input

#endif  // REPIU_INPUT_BROWSER_KEY_CODES_H_
