#include "repiu/engine/cd_audio_wave_out.h"

#include "repiu/media/chd_cd_image.h"
#include "repiu/runtime/execution_model.h"

#include <SDL3/SDL.h>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

namespace repiu::engine
{
namespace
{
constexpr std::uint32_t kSectorBytes = 2352U;
constexpr std::uint32_t kSectorsPerBuffer = 8U;
constexpr std::uint32_t kQueuedBufferCount = 4U;
}

struct CdAudioWaveOut::Impl
{
    media::ChdCdImage image;
    SDL_AudioStream* stream = nullptr;
    std::thread worker;
    std::atomic<bool> shutdown{false};
    std::atomic<bool> playing{false};
    std::atomic<bool> paused{false};
    // Decode cursor: the frame after the last one handed to SDL. It runs ahead
    // of what is audible by however much is still sitting in the stream.
    std::atomic<std::uint32_t> queued_lba{0};
    // Position of the audio actually reaching the device. Sampled on the worker
    // thread only: the guest asks for it from inside a VEH handler, which must
    // not take SDL's stream lock.
    std::atomic<std::uint32_t> audible_lba{0};
    // Task 421 counters. Free-running, never reset, so a sampler outside this
    // thread can difference two readings.
    std::atomic<std::uint32_t> worker_iterations{0};
    std::atomic<std::uint32_t> underruns{0};
    // Reported position while stopped, paused, or finished.
    std::atomic<std::uint32_t> frozen_lba{0};
    std::atomic<std::uint32_t> start_lba{0};
    std::atomic<std::uint32_t> end_lba{0};
    // Bumped by every Play/Stop/Seek so the worker can tell that the run it
    // decoded a buffer for is no longer the current one.
    std::atomic<std::uint32_t> generation{0};
    std::string message;
    std::vector<std::uint8_t> buffer;

    std::chrono::milliseconds Step();

    // Step only: the worker thread, or the host turn on one thread.
    std::uint32_t ComputeAudiblePosition() const
    {
        if (stream == nullptr)
        {
            return frozen_lba.load();
        }
        const std::uint32_t queued = queued_lba.load();
        const int pending = SDL_GetAudioStreamQueued(stream);
        const std::uint32_t pending_frames = pending > 0
            ? static_cast<std::uint32_t>(pending) / kSectorBytes : 0U;
        const std::uint32_t begin = start_lba.load();
        return queued > begin + pending_frames ? queued - pending_frames : begin;
    }
};

// One buffer into the stream: what the worker's loop did once. The answer is how
// long to wait before the next step, zero for "step again". Issue #64: the
// worker thread repeats it, or on one thread the host turn does (Pump).
std::chrono::milliseconds CdAudioWaveOut::Impl::Step()
{
    worker_iterations.fetch_add(1, std::memory_order_relaxed);
    const bool active = playing.load() && !paused.load();
    if (active)
    {
        audible_lba = ComputeAudiblePosition();
        // An empty stream while frames remain means the device ran dry:
        // the music itself gapped, which the position alone would not
        // show.
        if (SDL_GetAudioStreamQueued(stream) <= 0 &&
            queued_lba.load() < end_lba.load())
        {
            underruns.fetch_add(1, std::memory_order_relaxed);
        }
    }
    if (!active)
    {
        return std::chrono::milliseconds(5);
    }
    if (SDL_GetAudioStreamQueued(stream) >=
        static_cast<int>(kSectorBytes * kSectorsPerBuffer *
                         kQueuedBufferCount))
    {
        return std::chrono::milliseconds(2);
    }
    // Decoding a buffer takes long enough that Play/Stop/Seek can land
    // mid-iteration. The generation stamp lets the commit below drop a
    // buffer that belongs to a run the caller has already replaced.
    const std::uint32_t run_generation = generation.load();
    const std::uint32_t begin = queued_lba.load();
    if (begin >= end_lba.load())
    {
        // Everything is queued, but playback is not over until the
        // stream drains. Reporting "stopped" here would cut the tail.
        if (SDL_GetAudioStreamQueued(stream) <= 0)
        {
            frozen_lba = end_lba.load();
            playing = false;
            return std::chrono::milliseconds(0);
        }
        return std::chrono::milliseconds(2);
    }
    const std::uint32_t sectors = std::min(
        kSectorsPerBuffer, end_lba.load() - begin);
    bool read = true;
    for (std::uint32_t sector = 0; sector < sectors; ++sector)
    {
        std::uint8_t* pcm = buffer.data() +
            static_cast<std::size_t>(sector) * kSectorBytes;
        if (!image.ReadRawSector(begin + sector, pcm, kSectorBytes))
        {
            read = false;
            break;
        }
        for (std::uint32_t byte = 0; byte < kSectorBytes; byte += 2U)
        {
            std::swap(pcm[byte], pcm[byte + 1U]);
        }
    }
    if (!read)
    {
        message = image.message();
        frozen_lba = begin;
        playing = false;
        return std::chrono::milliseconds(0);
    }
    if (generation.load() != run_generation || !playing.load())
    {
        return std::chrono::milliseconds(0);
    }
    if (!SDL_PutAudioStreamData(stream, buffer.data(),
                                sectors * kSectorBytes))
    {
        message = std::string("SDL CD-DA queue failed: ") +
            SDL_GetError();
        frozen_lba = begin;
        playing = false;
        return std::chrono::milliseconds(0);
    }
    if (generation.load() == run_generation)
    {
        queued_lba = begin + sectors;
    }
    return std::chrono::milliseconds(0);
}

CdAudioWaveOut::CdAudioWaveOut() : impl_(std::make_unique<Impl>()) {}
CdAudioWaveOut::~CdAudioWaveOut() { Close(); }

bool CdAudioWaveOut::Open(const std::filesystem::path& chd_path)
{
    Close();
    if (!impl_->image.Open(chd_path))
    {
        impl_->message = impl_->image.message();
        return false;
    }
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO))
    {
        impl_->message = std::string("SDL audio initialization failed: ") +
            SDL_GetError();
        impl_->image.Close();
        return false;
    }
    const SDL_AudioSpec format{SDL_AUDIO_S16LE, 2, 44100};
    impl_->stream = SDL_OpenAudioDeviceStream(
        SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &format, nullptr, nullptr);
    if (impl_->stream == nullptr)
    {
        impl_->message = std::string("SDL CD-DA stream creation failed: ") +
            SDL_GetError();
        impl_->image.Close();
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        return false;
    }
    impl_->shutdown = false;
    impl_->buffer.assign(kSectorBytes * kSectorsPerBuffer, 0U);
    // Issue #64: on one thread the host turn pumps the stream instead.
    if (!runtime::execution_model::RunsHostOnOneThread())
    {
        impl_->worker = std::thread([this]() {
            while (!impl_->shutdown.load())
            {
                const std::chrono::milliseconds wait = impl_->Step();
                if (wait.count() != 0)
                {
                    std::this_thread::sleep_for(wait);
                }
            }
        });
    }
    impl_->message = "SDL3 CD-DA audio stream ready";
    return true;
}

// Issue #64. A bounded number of buffers, so one turn cannot run long; the
// queue ceiling is reached in a few.
void CdAudioWaveOut::Pump()
{
    constexpr int kStepsPerPump = 8;
    if (impl_->stream == nullptr || impl_->worker.joinable())
    {
        return;
    }
    for (int step = 0; step < kStepsPerPump; ++step)
    {
        if (impl_->Step().count() != 0)
        {
            return;
        }
    }
}

bool CdAudioWaveOut::Play(std::uint32_t start_lba, std::uint32_t frame_count)
{
    const media::ChdCdTrack* track = impl_->image.FindTrackByLba(start_lba);
    if (impl_->stream == nullptr || track == nullptr || !track->audio ||
        frame_count == 0U)
    {
        impl_->message = "CD-DA play range is not an audio track";
        return false;
    }
    impl_->playing = false;
    impl_->generation.fetch_add(1);
    SDL_ClearAudioStream(impl_->stream);
    impl_->start_lba = start_lba;
    impl_->queued_lba = start_lba;
    impl_->frozen_lba = start_lba;
    impl_->audible_lba = start_lba;
    impl_->end_lba = std::min(start_lba + frame_count, track->end_lba);
    impl_->paused = false;
    impl_->playing = true;
    SDL_ResumeAudioStreamDevice(impl_->stream);
    impl_->message = "CD-DA playback started through SDL3";
    return true;
}

void CdAudioWaveOut::Stop()
{
    // MSCDEX 85h keeps the position so 88h can pick it up again. Resuming from
    // the decode cursor would skip whatever was queued but never heard, so the
    // audible position is what gets remembered.
    const bool was_playing = impl_->playing.load();
    // Task 423. MSCDEX 85h on a drive that is already stopped is a no-op, but
    // this used to recompute `paused` from `was_playing` every time, so the
    // second Stop cleared the pause the first had just established. Task 422
    // recorded the guest issuing about sixty Stops a second with the flag
    // flickering 1, 0, 0, ... Returning here keeps both the flag and the
    // remembered position, which is what 88h resumes from.
    if (!was_playing && impl_->paused.load())
    {
        return;
    }
    const std::uint32_t position = was_playing ? impl_->audible_lba.load()
                                               : impl_->frozen_lba.load();
    impl_->playing = false;
    impl_->generation.fetch_add(1);
    impl_->paused = was_playing;
    impl_->frozen_lba = position;
    impl_->queued_lba = position;
    if (impl_->stream != nullptr)
    {
        SDL_ClearAudioStream(impl_->stream);
        SDL_PauseAudioStreamDevice(impl_->stream);
    }
}

bool CdAudioWaveOut::Seek(std::uint32_t lba)
{
    if (impl_->image.FindTrackByLba(lba) == nullptr)
    {
        impl_->message = "CD-DA seek target is outside the disc";
        return false;
    }
    if (impl_->playing.load())
    {
        Stop();
    }
    impl_->paused = false;
    impl_->generation.fetch_add(1);
    impl_->frozen_lba = lba;
    impl_->queued_lba = lba;
    impl_->audible_lba = lba;
    return true;
}

bool CdAudioWaveOut::Resume()
{
    if (impl_->stream == nullptr || !impl_->paused.load() ||
        impl_->queued_lba.load() >= impl_->end_lba.load())
    {
        return false;
    }
    impl_->paused = false;
    impl_->playing = true;
    SDL_ResumeAudioStreamDevice(impl_->stream);
    return true;
}

void CdAudioWaveOut::Close()
{
    impl_->shutdown = true;
    impl_->playing = false;
    if (impl_->stream != nullptr)
    {
        SDL_ClearAudioStream(impl_->stream);
        SDL_PauseAudioStreamDevice(impl_->stream);
    }
    if (impl_->worker.joinable())
    {
        impl_->worker.join();
    }
    if (impl_->stream != nullptr)
    {
        SDL_DestroyAudioStream(impl_->stream);
        impl_->stream = nullptr;
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
    }
    impl_->image.Close();
}

bool CdAudioWaveOut::playing() const { return impl_->playing.load(); }
bool CdAudioWaveOut::paused() const { return impl_->paused.load(); }
std::uint32_t CdAudioWaveOut::current_lba() const
{
    return impl_->playing.load() ? impl_->audible_lba.load()
                                 : impl_->frozen_lba.load();
}
// Task 421: read together so the poll thread can attribute a wrong position
// rather than guess. `worker_iterations` is the direct evidence for a starved
// worker: the position only refreshes at the top of that loop, so if the count
// does not move between two samples, neither could the position.
void CdAudioWaveOut::FillPositionSample(
    CdAudioPositionEntry* entry) const
{
    if (entry == nullptr)
    {
        return;
    }
    entry->current_lba = current_lba();
    entry->queued_lba = impl_->queued_lba.load();
    const int pending = impl_->stream != nullptr
        ? SDL_GetAudioStreamQueued(impl_->stream) : 0;
    entry->stream_bytes = pending > 0
        ? static_cast<std::uint32_t>(pending) : 0U;
    entry->start_lba = impl_->start_lba.load();
    entry->end_lba = impl_->end_lba.load();
    entry->worker_iterations = impl_->worker_iterations.load();
    entry->underruns = impl_->underruns.load();
    entry->generation = impl_->generation.load();
    entry->playing = impl_->playing.load();
    entry->paused = impl_->paused.load();
}

std::uint32_t CdAudioWaveOut::last_play_start_lba() const
{
    return impl_->start_lba.load();
}
std::uint32_t CdAudioWaveOut::last_play_end_lba() const
{
    return impl_->end_lba.load();
}
const std::string& CdAudioWaveOut::message() const { return impl_->message; }

}  // namespace repiu::engine
