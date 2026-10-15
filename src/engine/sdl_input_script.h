#pragma once

// Task 748. A scripted keyboard for reproductions: `REPIU_INPUT_SCRIPT=<file>`
// names a text file of `<ms> <key name> [hold ms]` lines (key names as
// `host_key_names` spells them: `F2`, `Keypad5`, `Q` ...). A thread pushes an
// SDL key-down at each time, measured from the moment the Glide window opened,
// and the matching key-up after the hold (150 ms when omitted). The events go
// through the same pump, bindings and JAMMA timeline as a real keyboard, so
// the guest cannot tell them apart -- and unlike XTest under WSLg they do not
// depend on which window has focus. Lines starting with `#` are comments.

#include <SDL3/SDL.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace repiu::engine
{

struct SdlInputScriptStep
{
    std::uint32_t at_ms = 0;
    SDL_Keycode key = SDLK_UNKNOWN;
    std::uint32_t hold_ms = 150;
    std::string name;
};

// Parses the script text. Malformed lines are skipped and counted.
std::vector<SdlInputScriptStep> ParseSdlInputScript(const std::string& text,
                                                    std::uint32_t* skipped);

class SdlInputScriptPlayer
{
public:
    ~SdlInputScriptPlayer();

    // Reads REPIU_INPUT_SCRIPT; returns false (and stays idle) when unset,
    // unreadable or empty.
    bool StartFromEnvironment(SDL_WindowID window);
    void Stop();

    // Issue #64. On one thread (RunsHostOnOneThread) there is no player
    // thread, and the host's event pump calls this to push what is due. A
    // no-op while the thread plays the script.
    void Pump();

    std::uint64_t pushed_events() const
    {
        return pushed_.load(std::memory_order_relaxed);
    }

private:
    void Run();
    void Push(const SdlInputScriptStep& step, bool down);
    // Pushes every event due at `now`; the time of the next one, or empty when
    // the script is done.
    std::optional<std::chrono::steady_clock::time_point> Advance(
        std::chrono::steady_clock::time_point now);

    std::vector<SdlInputScriptStep> steps_;
    SDL_WindowID window_ = 0;
    std::chrono::steady_clock::time_point origin_{};
    std::chrono::steady_clock::time_point release_at_{};
    std::size_t next_ = 0U;
    bool key_down_ = false;
    bool started_ = false;
    std::atomic<bool> stop_{false};
    std::atomic<std::uint64_t> pushed_{0};
    std::thread thread_;
};

}  // namespace repiu::engine
