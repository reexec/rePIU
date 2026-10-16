#include "repiu/engine/browser_audio_mixer.h"

#include "repiu/platform/browser_host.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <vector>

namespace repiu::engine
{
namespace
{

using Clock = std::chrono::steady_clock;

// Sent ahead at the start, to fill the worklet's buffer.
constexpr auto kLead = std::chrono::milliseconds(100);
// Longer than this without a turn (loading, the CHD unpack) and the backlog
// is dropped rather than sent in one burst.
constexpr auto kStall = std::chrono::milliseconds(250);
// A message carries at least this much, so a 2 ms turn does not post one each.
constexpr std::uint64_t kMinimumFrames = 256U;

struct Mixer
{
    std::vector<SDL_AudioStream*> streams;
    bool started = false;
    Clock::time_point base;
    std::uint64_t sent_since_base = 0;
    std::vector<float> mix;
    std::vector<float> part;
    BrowserAudioCounters counters;
};

Mixer& State()
{
    static Mixer mixer;
    return mixer;
}

}  // namespace

bool BrowserAudioActive()
{
    return repiu::platform::BrowserAudioSampleRate() > 0;
}

SDL_AudioSpec BrowserAudioMixFormat()
{
    return SDL_AudioSpec{SDL_AUDIO_F32, 2,
                         repiu::platform::BrowserAudioSampleRate()};
}

void RegisterBrowserAudioStream(SDL_AudioStream* const stream)
{
    if (stream != nullptr)
    {
        State().streams.push_back(stream);
    }
}

void UnregisterBrowserAudioStream(SDL_AudioStream* const stream)
{
    auto& streams = State().streams;
    streams.erase(std::remove(streams.begin(), streams.end(), stream),
                  streams.end());
}

void PumpBrowserAudio()
{
    Mixer& mixer = State();
    const int rate = repiu::platform::BrowserAudioSampleRate();
    if (rate <= 0 || mixer.streams.empty())
    {
        return;
    }
    const Clock::time_point now = Clock::now();
    const auto frames_in = [rate](Clock::duration span) {
        return static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(span)
                .count()) *
            static_cast<std::uint64_t>(rate) / 1000000U;
    };
    // Real time since the base, the lead included, less what was sent. After
    // a stall the base moves to now: what the outputs queued meanwhile still
    // plays, but no burst of silence-filled catch-up is sent.
    if (!mixer.started ||
        frames_in(now - mixer.base) > mixer.sent_since_base +
                frames_in(kLead + kStall))
    {
        if (mixer.started)
        {
            ++mixer.counters.rebases;
        }
        mixer.started = true;
        mixer.base = now - kLead;
        mixer.sent_since_base = 0;
    }
    const std::uint64_t due_total = frames_in(now - mixer.base);
    if (due_total < mixer.sent_since_base + kMinimumFrames)
    {
        return;
    }
    const std::uint64_t frames = due_total - mixer.sent_since_base;
    const std::size_t samples = static_cast<std::size_t>(frames) * 2U;
    mixer.mix.assign(samples, 0.0F);
    mixer.part.resize(samples);
    const int wanted_bytes = static_cast<int>(samples * sizeof(float));
    for (SDL_AudioStream* const stream : mixer.streams)
    {
        const int got = SDL_GetAudioStreamData(stream, mixer.part.data(),
                                               wanted_bytes);
        const std::size_t got_samples =
            got > 0 ? static_cast<std::size_t>(got) / sizeof(float) : 0U;
        if (got_samples < samples)
        {
            mixer.counters.short_frames += (samples - got_samples) / 2U;
        }
        for (std::size_t index = 0; index < got_samples; ++index)
        {
            mixer.mix[index] += mixer.part[index];
        }
    }
    for (float& sample : mixer.mix)
    {
        sample = std::clamp(sample, -1.0F, 1.0F);
    }
    repiu::platform::SendBrowserAudio(mixer.mix.data(),
                                      static_cast<std::size_t>(frames));
    mixer.sent_since_base += frames;
    mixer.counters.sent_frames += frames;
    ++mixer.counters.messages;
}

BrowserAudioCounters ReadBrowserAudioCounters()
{
    return State().counters;
}

}  // namespace repiu::engine
