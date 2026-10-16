#include "repiu/engine/host_audio_stream.h"

#include "repiu/engine/browser_audio_mixer.h"

#include <SDL3/SDL.h>

namespace repiu::engine
{

SDL_AudioStream* OpenHostAudioStream(const SDL_AudioSpec& source,
                                     std::string* const failure)
{
    if (BrowserAudioActive())
    {
        // A browser Worker: no device, the mixer takes from the stream.
        const SDL_AudioSpec mix = BrowserAudioMixFormat();
        SDL_AudioStream* const stream = SDL_CreateAudioStream(&source, &mix);
        if (stream == nullptr)
        {
            *failure = std::string("stream creation failed: ") + SDL_GetError();
            return nullptr;
        }
        RegisterBrowserAudioStream(stream);
        return stream;
    }
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO))
    {
        *failure =
            std::string("audio initialization failed: ") + SDL_GetError();
        return nullptr;
    }
    SDL_AudioStream* const stream = SDL_OpenAudioDeviceStream(
        SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &source, nullptr, nullptr);
    if (stream == nullptr)
    {
        *failure = std::string("stream creation failed: ") + SDL_GetError();
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        return nullptr;
    }
    return stream;
}

void CloseHostAudioStream(SDL_AudioStream* const stream)
{
    if (stream == nullptr)
    {
        return;
    }
    const bool device = SDL_GetAudioStreamDevice(stream) != 0U;
    SDL_ClearAudioStream(stream);
    if (device)
    {
        SDL_PauseAudioStreamDevice(stream);
    }
    else
    {
        UnregisterBrowserAudioStream(stream);
    }
    SDL_DestroyAudioStream(stream);
    if (device)
    {
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
    }
}

}  // namespace repiu::engine
