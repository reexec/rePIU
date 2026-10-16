#ifndef REPIU_ENGINE_BROWSER_AUDIO_MIXER_H_
#define REPIU_ENGINE_BROWSER_AUDIO_MIXER_H_

#include <SDL3/SDL_audio.h>

#include <cstdint>

namespace repiu::engine
{

// Issue #81: the sound of an engine running in a browser page's Worker. The
// audio outputs' streams (host_audio_stream.h) are bound to no device there;
// this takes real time's worth from all of them at every host turn, mixes
// them as float stereo at the page's sample rate and sends the result to the
// page's AudioWorklet. Natively, and under Node, the platform has no browser
// audio and none of this does anything. One thread only, like the loop that
// pumps it. See docs/design/20261017-i081-browser-sound.md, decision 2.

// Whether the page asked for sound, and so whether streams go to the mixer.
bool BrowserAudioActive();

// The format the streams convert to for the mixer.
SDL_AudioSpec BrowserAudioMixFormat();

void RegisterBrowserAudioStream(SDL_AudioStream* stream);
void UnregisterBrowserAudioStream(SDL_AudioStream* stream);

// Sends what real time calls for since the last call. Called from the host
// turn after the audio outputs' pumps.
void PumpBrowserAudio();

struct BrowserAudioCounters
{
    std::uint64_t sent_frames = 0;
    std::uint64_t messages = 0;
    std::uint64_t short_frames = 0;
    std::uint64_t rebases = 0;
};
BrowserAudioCounters ReadBrowserAudioCounters();

}  // namespace repiu::engine

#endif  // REPIU_ENGINE_BROWSER_AUDIO_MIXER_H_
