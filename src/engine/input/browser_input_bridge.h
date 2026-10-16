#ifndef REPIU_ENGINE_INPUT_BROWSER_INPUT_BRIDGE_H_
#define REPIU_ENGINE_INPUT_BROWSER_INPUT_BRIDGE_H_

#include "repiu/platform/browser_host.h"

#include <SDL3/SDL_joystick.h>

#include <array>
#include <cstdint>

namespace repiu::engine
{

// Issue #76. The browser page's input, put into SDL: keys become SDL key
// events (SDL_PushEvent), the page losing focus becomes
// SDL_EVENT_WINDOW_FOCUS_LOST, and each gamepad becomes an SDL virtual
// gamepad. From there the event pump's existing path -- the OSD, the BIOS
// keyboard, the JAMMA bindings and timeline, SdlPadInput and its chords --
// handles them as it handles a native window's. Natively the page sends
// nothing and Pump does nothing. Called by the thread that pumps SDL events,
// before it polls them. See docs/design/20261016-i076-browser-input.md.
class BrowserInputBridge
{
public:
    BrowserInputBridge() = default;
    ~BrowserInputBridge();
    BrowserInputBridge(const BrowserInputBridge&) = delete;
    BrowserInputBridge& operator=(const BrowserInputBridge&) = delete;

    // Takes everything the page sent since the last call.
    void Pump();

    [[nodiscard]] std::uint64_t keys_pushed() const
    {
        return keys_pushed_;
    }

private:
    struct VirtualPad
    {
        SDL_JoystickID id = 0;
        SDL_Joystick* joystick = nullptr;
    };

    void PushKey(const repiu::platform::BrowserInputEvent& event);
    void PushFocusLost();
    void UpdatePad(const repiu::platform::BrowserInputEvent& event);
    void RemovePad(std::uint8_t index);

    // The page's gamepad indexes 0 to 3.
    std::array<VirtualPad, 4> pads_{};
    std::uint64_t keys_pushed_ = 0U;
    std::uint32_t unknown_keys_reported_ = 0U;
};

}  // namespace repiu::engine

#endif  // REPIU_ENGINE_INPUT_BROWSER_INPUT_BRIDGE_H_
