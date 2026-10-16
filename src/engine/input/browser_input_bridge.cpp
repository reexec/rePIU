#include "browser_input_bridge.h"

#include "repiu/input/browser_key_codes.h"

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_timer.h>

#include <cstdio>

namespace repiu::engine
{
namespace
{

using repiu::platform::BrowserInputEvent;

// The standard gamepad mapping's buttons (https://w3c.github.io/gamepad/#remapping)
// as SDL gamepad buttons. 6 and 7 are the triggers, which are axes in SDL and
// come in BrowserInputEvent::triggers instead.
constexpr int kNoButton = -1;
constexpr int kStandardToSdl[17] = {
    SDL_GAMEPAD_BUTTON_SOUTH,          SDL_GAMEPAD_BUTTON_EAST,
    SDL_GAMEPAD_BUTTON_WEST,           SDL_GAMEPAD_BUTTON_NORTH,
    SDL_GAMEPAD_BUTTON_LEFT_SHOULDER,  SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER,
    kNoButton,                         kNoButton,
    SDL_GAMEPAD_BUTTON_BACK,           SDL_GAMEPAD_BUTTON_START,
    SDL_GAMEPAD_BUTTON_LEFT_STICK,     SDL_GAMEPAD_BUTTON_RIGHT_STICK,
    SDL_GAMEPAD_BUTTON_DPAD_UP,        SDL_GAMEPAD_BUTTON_DPAD_DOWN,
    SDL_GAMEPAD_BUTTON_DPAD_LEFT,      SDL_GAMEPAD_BUTTON_DPAD_RIGHT,
    SDL_GAMEPAD_BUTTON_GUIDE,
};

// The virtual gamepad has SDL's buttons up to DPAD_RIGHT and its six axes,
// numbered as SDL numbers them, so SDL maps it to a gamepad one to one.
constexpr int kVirtualButtons = SDL_GAMEPAD_BUTTON_DPAD_RIGHT + 1;
constexpr int kVirtualAxes = SDL_GAMEPAD_AXIS_RIGHT_TRIGGER + 1;

SDL_Keymod ModifiersOf(const std::uint8_t bits)
{
    SDL_Keymod mod = SDL_KMOD_NONE;
    if ((bits & BrowserInputEvent::kShift) != 0U)
    {
        mod |= SDL_KMOD_LSHIFT;
    }
    if ((bits & BrowserInputEvent::kControl) != 0U)
    {
        mod |= SDL_KMOD_LCTRL;
    }
    if ((bits & BrowserInputEvent::kAlt) != 0U)
    {
        mod |= SDL_KMOD_LALT;
    }
    if ((bits & BrowserInputEvent::kMeta) != 0U)
    {
        mod |= SDL_KMOD_LGUI;
    }
    return mod;
}

// When the page saw it, on SDL's clock: the Worker takes messages only when
// the engine yields, once a frame, so the age is taken back off.
Uint64 TimestampOf(const BrowserInputEvent& event)
{
    const Uint64 now = SDL_GetTicksNS();
    return event.age_nanoseconds < now ? now - event.age_nanoseconds : now;
}

}  // namespace

BrowserInputBridge::~BrowserInputBridge()
{
    for (std::uint8_t index = 0; index < pads_.size(); ++index)
    {
        RemovePad(index);
    }
}

void BrowserInputBridge::Pump()
{
    BrowserInputEvent event;
    while (repiu::platform::TakeBrowserInputEvent(&event))
    {
        switch (event.kind)
        {
            case BrowserInputEvent::Kind::kKey:
                PushKey(event);
                break;
            case BrowserInputEvent::Kind::kFocusLost:
                PushFocusLost();
                break;
            case BrowserInputEvent::Kind::kPad:
                UpdatePad(event);
                break;
            case BrowserInputEvent::Kind::kPadRemoved:
                RemovePad(event.pad);
                break;
        }
    }
}

void BrowserInputBridge::PushKey(const BrowserInputEvent& event)
{
    const SDL_Scancode scancode = repiu::input::ScancodeForBrowserKeyCode(event.code);
    if (scancode == SDL_SCANCODE_UNKNOWN)
    {
        if (unknown_keys_reported_ < 8U)
        {
            ++unknown_keys_reported_;
            std::fprintf(stderr, "[repiu-browser-input] no SDL key for \"%s\"\n", event.code);
        }
        return;
    }
    const SDL_Keymod mod = ModifiersOf(event.modifiers);
    SDL_Event key{};
    key.key.type = event.down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    key.key.timestamp = TimestampOf(event);
    key.key.windowID = 0U;
    key.key.which = 0U;
    key.key.scancode = scancode;
    // As a key event reports it: the key in the current layout, not shifted.
    key.key.key = SDL_GetKeyFromScancode(scancode, SDL_KMOD_NONE, true);
    key.key.mod = mod;
    key.key.raw = 0U;
    key.key.down = event.down;
    key.key.repeat = event.repeat;
    if (SDL_PushEvent(&key))
    {
        ++keys_pushed_;
    }
}

void BrowserInputBridge::PushFocusLost()
{
    SDL_Event focus{};
    focus.window.type = SDL_EVENT_WINDOW_FOCUS_LOST;
    focus.window.timestamp = SDL_GetTicksNS();
    focus.window.windowID = 0U;
    SDL_PushEvent(&focus);
}

void BrowserInputBridge::UpdatePad(const BrowserInputEvent& event)
{
    if (event.pad >= pads_.size())
    {
        return;
    }
    VirtualPad& pad = pads_[event.pad];
    if (pad.joystick == nullptr)
    {
        SDL_VirtualJoystickDesc desc;
        SDL_INIT_INTERFACE(&desc);
        desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
        desc.naxes = static_cast<Uint16>(kVirtualAxes);
        desc.nbuttons = static_cast<Uint16>(kVirtualButtons);
        desc.button_mask = (1U << kVirtualButtons) - 1U;
        desc.axis_mask = (1U << kVirtualAxes) - 1U;
        desc.name = "rePIU browser gamepad";
        pad.id = SDL_AttachVirtualJoystick(&desc);
        pad.joystick = pad.id != 0 ? SDL_OpenJoystick(pad.id) : nullptr;
        if (pad.joystick == nullptr)
        {
            std::fprintf(stderr, "[repiu-browser-input] gamepad %u not attached: %s\n",
                         static_cast<unsigned>(event.pad), SDL_GetError());
            if (pad.id != 0)
            {
                SDL_DetachVirtualJoystick(pad.id);
            }
            pad = VirtualPad{};
            return;
        }
    }
    for (int standard = 0; standard < 17; ++standard)
    {
        const int button = kStandardToSdl[standard];
        if (button != kNoButton)
        {
            SDL_SetJoystickVirtualButton(pad.joystick, button,
                                         (event.buttons & (1U << standard)) != 0U);
        }
    }
    for (int axis = 0; axis < 4; ++axis)
    {
        SDL_SetJoystickVirtualAxis(pad.joystick, SDL_GAMEPAD_AXIS_LEFTX + axis, event.axes[axis]);
    }
    SDL_SetJoystickVirtualAxis(pad.joystick, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, event.triggers[0]);
    SDL_SetJoystickVirtualAxis(pad.joystick, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, event.triggers[1]);
}

void BrowserInputBridge::RemovePad(const std::uint8_t index)
{
    if (index >= pads_.size() || pads_[index].joystick == nullptr)
    {
        return;
    }
    VirtualPad& pad = pads_[index];
    SDL_CloseJoystick(pad.joystick);
    SDL_DetachVirtualJoystick(pad.id);
    pad = VirtualPad{};
}

}  // namespace repiu::engine
