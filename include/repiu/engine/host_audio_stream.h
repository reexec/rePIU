#ifndef REPIU_ENGINE_HOST_AUDIO_STREAM_H_
#define REPIU_ENGINE_HOST_AUDIO_STREAM_H_

#include <SDL3/SDL_audio.h>

#include <string>

namespace repiu::engine
{

// Issue #81: the stream an audio output puts its PCM into. Natively it is
// bound to SDL's default playback device, as the outputs always opened it. In
// a browser page's Worker, where SDL has no audio device, it is bound to none
// and the browser audio mixer (browser_audio_mixer.h) takes from it. Either
// way the output puts PCM in, watches what is queued and sets its gain as
// before. See docs/design/20261017-i081-browser-sound.md, decision 1.
//
// On failure returns null and says why ("audio initialization failed: ..." or
// "stream creation failed: ...").
SDL_AudioStream* OpenHostAudioStream(const SDL_AudioSpec& source,
                                     std::string* failure);

// Empties, stops and destroys a stream OpenHostAudioStream made, and gives
// back what opening it took.
void CloseHostAudioStream(SDL_AudioStream* stream);

}  // namespace repiu::engine

#endif  // REPIU_ENGINE_HOST_AUDIO_STREAM_H_
