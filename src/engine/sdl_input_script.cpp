#include "sdl_input_script.h"

#include "repiu/input/host_key_names.h"
#include "repiu/runtime/execution_model.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace repiu::engine
{

std::vector<SdlInputScriptStep> ParseSdlInputScript(const std::string& text,
                                                    std::uint32_t* skipped)
{
    std::vector<SdlInputScriptStep> steps;
    std::uint32_t skipped_lines = 0U;
    std::istringstream lines(text);
    std::string line;
    while (std::getline(lines, line))
    {
        if (!line.empty() && line.back() == '\r')
        {
            line.pop_back();
        }
        std::istringstream fields(line);
        std::string first;
        if (!(fields >> first) || first[0] == '#')
        {
            continue;
        }
        SdlInputScriptStep step;
        char* end = nullptr;
        const unsigned long at = std::strtoul(first.c_str(), &end, 10);
        std::string name;
        if (end == first.c_str() || *end != '\0' || !(fields >> name) ||
            !repiu::input::FindHostKeyByName(name, &step.key))
        {
            ++skipped_lines;
            continue;
        }
        step.at_ms = static_cast<std::uint32_t>(at);
        step.name = name;
        unsigned long hold = 0UL;
        if (fields >> hold)
        {
            step.hold_ms = static_cast<std::uint32_t>(hold);
        }
        steps.push_back(step);
    }
    if (skipped != nullptr)
    {
        *skipped = skipped_lines;
    }
    return steps;
}

SdlInputScriptPlayer::~SdlInputScriptPlayer()
{
    Stop();
}

bool SdlInputScriptPlayer::StartFromEnvironment(const SDL_WindowID window)
{
    const char* const path = std::getenv("REPIU_INPUT_SCRIPT");
    if (path == nullptr || *path == '\0' || started_)
    {
        return false;
    }
    std::ifstream file(path);
    if (!file)
    {
        std::fprintf(stderr, "[repiu-input-script] cannot read %s\n", path);
        return false;
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    std::uint32_t skipped = 0U;
    steps_ = ParseSdlInputScript(buffer.str(), &skipped);
    std::fprintf(stderr,
                 "[repiu-input-script] %s: %zu steps, %u lines skipped\n",
                 path, steps_.size(), static_cast<unsigned>(skipped));
    if (steps_.empty())
    {
        return false;
    }
    window_ = window;
    origin_ = std::chrono::steady_clock::now();
    next_ = 0U;
    key_down_ = false;
    started_ = true;
    stop_.store(false, std::memory_order_relaxed);
    // Issue #64: on one thread the host turn plays the script (Pump).
    if (!runtime::execution_model::RunsHostOnOneThread())
    {
        thread_ = std::thread([this]() { Run(); });
    }
    return true;
}

void SdlInputScriptPlayer::Stop()
{
    stop_.store(true, std::memory_order_relaxed);
    if (thread_.joinable())
    {
        thread_.join();
    }
    // A key held when the run stops is released, as the thread always did.
    if (key_down_ && next_ < steps_.size())
    {
        Push(steps_[next_], false);
        key_down_ = false;
    }
    started_ = false;
}

void SdlInputScriptPlayer::Pump()
{
    if (!started_ || thread_.joinable())
    {
        return;
    }
    Advance(std::chrono::steady_clock::now());
}

void SdlInputScriptPlayer::Push(const SdlInputScriptStep& step, const bool down)
{
    SDL_Event event{};
    event.key.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    event.key.timestamp = SDL_GetTicksNS();
    event.key.windowID = window_;
    event.key.which = 0U;
    event.key.scancode = SDL_GetScancodeFromKey(step.key, nullptr);
    event.key.key = step.key;
    event.key.mod = SDL_KMOD_NONE;
    event.key.raw = 0U;
    event.key.down = down;
    event.key.repeat = false;
    if (SDL_PushEvent(&event))
    {
        pushed_.fetch_add(1U, std::memory_order_relaxed);
    }
}

std::optional<std::chrono::steady_clock::time_point> SdlInputScriptPlayer::Advance(
    const std::chrono::steady_clock::time_point now)
{
    while (next_ < steps_.size())
    {
        const SdlInputScriptStep& step = steps_[next_];
        if (!key_down_)
        {
            const auto press_at = origin_ + std::chrono::milliseconds(step.at_ms);
            if (now < press_at)
            {
                return press_at;
            }
            Push(step, true);
            std::fprintf(stderr, "[repiu-input-script] t=%u key=%s hold=%u\n",
                         static_cast<unsigned>(step.at_ms), step.name.c_str(),
                         static_cast<unsigned>(step.hold_ms));
            key_down_ = true;
            release_at_ = now + std::chrono::milliseconds(step.hold_ms);
        }
        if (now < release_at_)
        {
            return release_at_;
        }
        Push(step, false);
        key_down_ = false;
        ++next_;
    }
    return std::nullopt;
}

void SdlInputScriptPlayer::Run()
{
    while (!stop_.load(std::memory_order_relaxed))
    {
        const auto now = std::chrono::steady_clock::now();
        const std::optional<std::chrono::steady_clock::time_point> next =
            Advance(now);
        if (!next.has_value())
        {
            return;
        }
        const auto remaining = *next - now;
        std::this_thread::sleep_for(
            remaining > std::chrono::milliseconds(20)
                ? std::chrono::milliseconds(20)
                : std::chrono::duration_cast<std::chrono::milliseconds>(
                      remaining) + std::chrono::milliseconds(1));
    }
}

}  // namespace repiu::engine
