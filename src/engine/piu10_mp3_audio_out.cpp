#include "repiu/engine/piu10_mp3_audio_out.h"
#include "repiu/engine/host_audio_stream.h"
#include "repiu/engine/timer_latency_trace.h"
#include "repiu/engine/piu10_frame_sync_history.h"
#include "repiu/runtime/execution_model.h"

#include "repiu/sound/decoder_input_fifo.h"
#include "repiu/sound/mpeg_audio_frame.h"
#include "repiu/sound/stream_chunk_audit.h"

#include <SDL3/SDL.h>

#define MINIMP3_ONLY_MP3
#define MINIMP3_IMPLEMENTATION
#include <minimp3.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <span>
#include <thread>
#include <vector>

namespace repiu::engine
{
namespace
{

constexpr std::size_t kCompressedRingBytes = 4096U;
constexpr std::size_t kRingDrainBytes = 512U;
constexpr std::size_t kInitialCompatibleFrames = 3U;
constexpr int kDefaultSampleRate = 44100;
constexpr int kDefaultChannels = 2;
constexpr int kMaximumQueuedPcmBytes =
    kDefaultSampleRate * kDefaultChannels *
    static_cast<int>(sizeof(std::int16_t)) / 4;

bool FramesCompatible(const sound::MpegAudioFrameInfo& first,
                      const sound::MpegAudioFrameInfo& candidate)
{
    return first.sample_rate_hz == candidate.sample_rate_hz &&
        first.channels == candidate.channels &&
        first.samples_per_frame == candidate.samples_per_frame;
}

void LogStreamChunkDigest(const void* side_value,
                          const sound::StreamChunkDigest& digest)
{
    const auto* side = static_cast<const char*>(side_value);
    std::fprintf(
        stderr,
        "[repiu-piu10-mp3-audit] side=%s chunk=%llu end=%llu "
        "bytes=%zu hash=%016llX\n",
        side,
        static_cast<unsigned long long>(digest.chunk_index),
        static_cast<unsigned long long>(digest.end_offset),
        digest.byte_count,
        static_cast<unsigned long long>(digest.hash));
}

}  // namespace

struct Piu10Mp3AudioOut::Impl
{
    Impl() : compressed(Piu10Mp3AudioOut::kCompressedFifoBytes,
                        kCompressedRingBytes) {}

    sound::DecoderInputFifo compressed;
    std::mutex wake_mutex;
    std::condition_variable wake;
    std::thread worker;
    std::atomic<bool> shutdown{false};
    std::atomic<bool> ready{false};
    std::atomic<std::uint8_t> frame_sync{1U};
    std::atomic<std::uint64_t> received_bytes{0};
    std::atomic<std::uint64_t> dropped_bytes{0};
    std::atomic<std::uint64_t> decoded_frames{0};
    std::atomic<std::uint64_t> queued_pcm_frames{0};
    std::atomic<std::uint64_t> starvation_events{0};
    std::atomic<std::uint64_t> batched_bytes{0};
    std::atomic<std::uint64_t> sync_toggles{0};
    std::atomic<std::uint64_t> sync_multi_toggle_events{0};
    std::atomic<std::uint64_t> pcm_empty_events{0};
    std::atomic<std::uint64_t> pcm_queued_low_water_bytes{UINT64_MAX};
    std::atomic<std::uint64_t> playback_lag_bytes{0};
    int device_buffer_frames = 0;
    std::atomic<std::size_t> decoder_pending_bytes{0};
    std::atomic<int> reported_sample_rate{kDefaultSampleRate};
    std::atomic<int> reported_channels{kDefaultChannels};
    std::atomic<int> reported_frame_samples{1152};
    std::uint32_t startup_latency_ms = 0U;
    bool startup_latency_pending = true;
    bool stream_audit_enabled = false;
    sound::StreamChunkAudit producer_audit;
    sound::StreamChunkAudit consumer_audit;

    SDL_AudioStream* stream = nullptr;
    int source_sample_rate = kDefaultSampleRate;
    int source_channels = kDefaultChannels;
    std::atomic<bool> playback_started{false};
    std::deque<std::uint64_t> frame_start_pcm_offsets;
    std::uint64_t total_pcm_bytes_put = 0U;
    // Task 740. A playback clock between device pulls. The bytes the device
    // has taken (`total_pcm_bytes_put` less `SDL_GetAudioStreamQueued`) move
    // in device-buffer steps: 17 ms on WSLg, 46 ms or more on other hosts.
    // Before this clock, every frame boundary inside one step toggled
    // frame-sync in the same worker call, and the decoder refilled the whole
    // step at once, so the guest's demand and its frame count moved in the
    // same steps -- which is the arrow clock stalling and catching up. The
    // clock rises at the PCM rate from the pulled count before the latest
    // step, never runs backwards and never passes the pulled count, so it
    // is at most one device buffer behind the device and cannot drift.
    // Task 749. The line no longer restarts at every device step. A step
    // observed early jumped the clock forward by the unplayed remainder of
    // the previous step, and one observed late stalled it at the cap, so the
    // pull jitter of the audio thread (a few ms on WSLg's PulseAudio) became
    // 12 ms jumps and stalls in the song position: frame-sync toggles 18-33
    // ms apart for a 26.1 ms frame. The clock now free-runs at the PCM rate
    // from its own previous value and slews toward the device's count less
    // half a device buffer (the same average lag as before) with a four
    // second time constant, so jitter is absorbed and a real rate difference
    // is followed without a step. It still never passes the pulled count and
    // never runs backwards.
    std::uint64_t clock_reported = 0U;
    std::uint64_t clock_estimate = 0U;
    bool clock_valid = false;
    std::chrono::steady_clock::time_point clock_time{};
    std::chrono::steady_clock::time_point last_put_time{};
    bool last_put_valid = false;
    // Task 748. `REPIU_PIU10_MP3_POSITION_TRACE=1`: every frame-sync toggle
    // (the worker) and every guest read that first sees the new value (the
    // guest thread), each with a microsecond stamp, so the song position the
    // guest counts can be read event by event next to the host clock.
    const bool position_trace =
        std::getenv("REPIU_PIU10_MP3_POSITION_TRACE") != nullptr;
    const std::chrono::steady_clock::time_point trace_origin =
        std::chrono::steady_clock::now();
    std::atomic<std::int64_t> last_toggle_us{0};
    std::atomic<std::uint64_t> toggle_seq{0};
    mutable std::uint8_t last_seen_sync = 0xFFU;
    mutable std::uint8_t trace_last_seen_sync = 0xFFU;
    // Issue #58: when the bit flipped, for reads answered at a tick's due
    // time. Written by the worker, read by the guest thread.
    Piu10FrameSyncHistory frame_sync_history;
    mutable std::uint64_t reads_since_seen = 0U;
    std::string message;

    std::int64_t TraceMicroseconds() const
    {
        return static_cast<std::int64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - trace_origin)
                .count());
    }

    std::uint64_t PcmBytesPerSecond() const
    {
        return static_cast<std::uint64_t>(source_sample_rate) *
            static_cast<std::uint64_t>(source_channels) *
            sizeof(std::int16_t);
    }

    // The playback clock for the queue state `queued`. Worker thread only.
    std::uint64_t DeviceBufferBytes() const
    {
        return static_cast<std::uint64_t>(
                   device_buffer_frames > 0 ? device_buffer_frames : 0) *
            static_cast<std::uint64_t>(source_channels) * sizeof(std::int16_t);
    }

    std::uint64_t PlaybackPosition(const std::uint64_t queued)
    {
        const std::uint64_t reported = total_pcm_bytes_put -
            std::min(total_pcm_bytes_put, queued);
        const auto now = std::chrono::steady_clock::now();
        clock_reported = reported;
        const std::uint64_t half_buffer = DeviceBufferBytes() / 2U;
        const std::uint64_t target =
            reported > half_buffer ? reported - half_buffer : 0U;
        if (!clock_valid)
        {
            if (reported == 0U)
            {
                playback_lag_bytes.store(0U, std::memory_order_relaxed);
                return 0U;
            }
            clock_valid = true;
            clock_estimate = target;
            clock_time = now;
        }
        const double elapsed_seconds =
            std::chrono::duration<double>(now - clock_time).count();
        clock_time = now;
        const double rate = static_cast<double>(PcmBytesPerSecond());
        // The slew: a fraction of the error per second, bounded to two per
        // cent of the rate so a burst of late pulls cannot become a jump.
        constexpr double kSlewSeconds = 4.0;
        constexpr double kSlewLimit = 0.02;
        const double error = static_cast<double>(target) -
            static_cast<double>(clock_estimate);
        double slew = error / (kSlewSeconds * rate);
        slew = std::max(-kSlewLimit, std::min(kSlewLimit, slew));
        const double advance = elapsed_seconds * rate * (1.0 + slew);
        std::uint64_t estimate = clock_estimate +
            static_cast<std::uint64_t>(advance > 0.0 ? advance : 0.0);
        estimate = std::min(estimate, reported);
        estimate = std::max(estimate, clock_estimate);
        clock_estimate = estimate;
        playback_lag_bytes.store(
            reported - estimate, std::memory_order_relaxed);
        return estimate;
    }

    // Whether the decoder should produce another frame: keep a quarter
    // second of PCM ahead of the playback clock, one frame at a time as the
    // clock advances, and refill at once only when a burst of device pulls
    // has taken more than half of it.
    bool NeedsPcm()
    {
        const int queued_value = SDL_GetAudioStreamQueued(stream);
        const std::uint64_t queued = queued_value > 0
            ? static_cast<std::uint64_t>(queued_value) : 0U;
        if (queued < static_cast<std::uint64_t>(kMaximumQueuedPcmBytes) / 2U)
        {
            return true;
        }
        const std::uint64_t ahead =
            total_pcm_bytes_put - PlaybackPosition(queued);
        return ahead < static_cast<std::uint64_t>(kMaximumQueuedPcmBytes);
    }

    void ResetPlaybackClock()
    {
        clock_reported = 0U;
        clock_estimate = 0U;
        clock_valid = false;
        clock_time = std::chrono::steady_clock::time_point{};
        last_put_valid = false;
        playback_lag_bytes.store(0U, std::memory_order_relaxed);
    }

    void AdvanceFrameSyncToPlayback()
    {
        if (stream == nullptr || frame_start_pcm_offsets.empty())
        {
            return;
        }
        const int queued_value = SDL_GetAudioStreamQueued(stream);
        const std::uint64_t queued = queued_value > 0
            ? static_cast<std::uint64_t>(queued_value) : 0U;
        // Task 740: the playback clock, not the pulled byte count, so that
        // one frame boundary is crossed at a time.
        const std::uint64_t position = PlaybackPosition(queued);
        std::uint32_t toggles = 0U;
        while (!frame_start_pcm_offsets.empty() &&
               frame_start_pcm_offsets.front() <= position)
        {
            const std::uint64_t frame_start = frame_start_pcm_offsets.front();
            const std::uint8_t flipped = static_cast<std::uint8_t>(
                frame_sync.fetch_xor(1U, std::memory_order_relaxed) ^ 1U);
            const std::uint64_t flip_ns = TimerTraceNowNanoseconds();
            frame_sync_history.Record(flip_ns, flipped);
            frame_start_pcm_offsets.pop_front();
            ++toggles;
            if (TimerLatencyTraceEnabled())
            {
                RecordTimerTrace(
                    TimerTraceEvent::kToggle, flip_ns,
                    static_cast<std::uint32_t>(
                        sync_toggles.load(std::memory_order_relaxed) +
                        toggles));
            }
            if (position_trace)
            {
                const std::int64_t now_us = TraceMicroseconds();
                const std::uint64_t seq =
                    toggle_seq.fetch_add(1U, std::memory_order_relaxed) + 1U;
                last_toggle_us.store(now_us, std::memory_order_relaxed);
                const double bytes_per_ms =
                    static_cast<double>(PcmBytesPerSecond()) / 1000.0;
                fprintf(stderr,
                        "[repiu-mp3-pos] toggle seq=%llu t_us=%lld"
                        " pos_ms=%.1f frame_ms=%.1f lag_ms=%.1f"
                        " queued_ms=%.1f\n",
                        static_cast<unsigned long long>(seq),
                        static_cast<long long>(now_us),
                        static_cast<double>(position) / bytes_per_ms,
                        static_cast<double>(frame_start) / bytes_per_ms,
                        static_cast<double>(clock_reported - position) /
                            bytes_per_ms,
                        static_cast<double>(queued) / bytes_per_ms);
            }
        }
        if (toggles != 0U)
        {
            sync_toggles.fetch_add(toggles, std::memory_order_relaxed);
            if (toggles > 1U)
            {
                sync_multi_toggle_events.fetch_add(
                    1U, std::memory_order_relaxed);
            }
        }
    }

    bool ValidateInitialFrames(const std::vector<std::uint8_t>& encoded,
                               std::size_t cursor,
                               sound::MpegAudioFrameInfo* first,
                               bool* needs_more) const
    {
        *needs_more = false;
        std::size_t probe = cursor;
        sound::MpegAudioFrameInfo initial;
        for (std::size_t index = 0; index < kInitialCompatibleFrames; ++index)
        {
            if (encoded.size() - probe < 4U)
            {
                *needs_more = true;
                return false;
            }
            sound::MpegAudioFrameInfo candidate;
            if (!sound::ParseMpegAudioFrameHeader(
                    std::span<const std::uint8_t>(
                        encoded.data() + probe, encoded.size() - probe),
                    &candidate))
            {
                return false;
            }
            if (encoded.size() - probe < candidate.frame_bytes)
            {
                *needs_more = true;
                return false;
            }
            if (index == 0U)
            {
                initial = candidate;
            }
            else if (!FramesCompatible(initial, candidate))
            {
                return false;
            }
            probe += candidate.frame_bytes;
        }
        *first = initial;
        return true;
    }

    bool QueueDecodedFrame(const mp3dec_frame_info_t& info,
                           const mp3d_sample_t* pcm, int samples)
    {
        if (samples <= 0 || info.channels <= 0 || info.hz <= 0)
        {
            return true;
        }
        if ((info.channels != source_channels || info.hz != source_sample_rate))
        {
            const SDL_AudioSpec source_spec{
                SDL_AUDIO_S16LE, info.channels, info.hz};
            if (!SDL_SetAudioStreamFormat(stream, &source_spec, nullptr))
            {
                std::fprintf(stderr,
                             "[repiu-piu10-mp3] PCM format update failed: %s\n",
                             SDL_GetError());
                return false;
            }
            source_channels = info.channels;
            source_sample_rate = info.hz;
            reported_channels.store(info.channels, std::memory_order_relaxed);
            reported_sample_rate.store(info.hz, std::memory_order_relaxed);
        }
        reported_frame_samples.store(samples, std::memory_order_relaxed);
        const int pcm_bytes = samples * info.channels *
            static_cast<int>(sizeof(mp3d_sample_t));
        if (startup_latency_pending)
        {
            const std::size_t silence_bytes =
                Piu10Mp3AudioOut::CalculateStartupSilenceBytes(
                    startup_latency_ms, info.hz, info.channels);
            if (silence_bytes != 0U)
            {
                std::vector<std::uint8_t> silence(silence_bytes, 0U);
                if (!SDL_PutAudioStreamData(
                        stream, silence.data(),
                        static_cast<int>(silence.size())))
                {
                    std::fprintf(
                        stderr,
                        "[repiu-piu10-mp3] latency queue failed: %s\n",
                        SDL_GetError());
                    return false;
                }
                total_pcm_bytes_put += silence_bytes;
            }
            startup_latency_pending = false;
        }
        const std::uint64_t frame_start = total_pcm_bytes_put;
        // Task 740. The queue is at its trough here, just before a refill.
        // A refill less than 200 ms after the previous one is a playing
        // song; the tail of a song drains without one.
        const auto put_time = std::chrono::steady_clock::now();
        if (playback_started.load(std::memory_order_relaxed) &&
            last_put_valid && put_time - last_put_time <
                std::chrono::milliseconds(200))
        {
            const int trough_value = SDL_GetAudioStreamQueued(stream);
            const std::uint64_t trough = trough_value > 0
                ? static_cast<std::uint64_t>(trough_value) : 0U;
            std::uint64_t low = pcm_queued_low_water_bytes.load(
                std::memory_order_relaxed);
            while (trough < low &&
                   !pcm_queued_low_water_bytes.compare_exchange_weak(
                       low, trough, std::memory_order_relaxed))
            {
            }
            if (trough == 0U)
            {
                pcm_empty_events.fetch_add(1U, std::memory_order_relaxed);
            }
        }
        last_put_time = put_time;
        last_put_valid = true;
        if (!SDL_PutAudioStreamData(stream, pcm, pcm_bytes))
        {
            std::fprintf(stderr,
                         "[repiu-piu10-mp3] PCM queue failed: %s\n",
                         SDL_GetError());
            return false;
        }
        frame_start_pcm_offsets.push_back(frame_start);
        total_pcm_bytes_put += static_cast<std::uint64_t>(pcm_bytes);
        AdvanceFrameSyncToPlayback();
        decoded_frames.fetch_add(1U, std::memory_order_relaxed);
        queued_pcm_frames.fetch_add(
            static_cast<std::uint64_t>(samples), std::memory_order_relaxed);
        if (!playback_started.exchange(true))
        {
            std::fprintf(stderr,
                         "[repiu-piu10-mp3] minimp3 playback started after "
                         "%llu guest bytes\n",
                         static_cast<unsigned long long>(
                             received_bytes.load(std::memory_order_relaxed)));
        }
        return true;
    }

    bool DecodeOne(std::vector<std::uint8_t>* encoded, std::size_t* cursor,
                   bool* stream_found, mp3dec_t* decoder)
    {
        if (encoded->size() - *cursor < 4U)
        {
            return false;
        }

        sound::MpegAudioFrameInfo frame;
        if (!sound::ParseMpegAudioFrameHeader(
                std::span<const std::uint8_t>(
                    encoded->data() + *cursor, encoded->size() - *cursor),
                &frame))
        {
            ++*cursor;
            if (*stream_found)
            {
                mp3dec_init(decoder);
                *stream_found = false;
            }
            return true;
        }

        if (!*stream_found)
        {
            bool needs_more = false;
            if (!ValidateInitialFrames(
                    *encoded, *cursor, &frame, &needs_more))
            {
                if (needs_more)
                {
                    return false;
                }
                ++*cursor;
                return true;
            }
            *stream_found = true;
        }
        else if (encoded->size() - *cursor < frame.frame_bytes)
        {
            return false;
        }

        std::array<mp3d_sample_t, MINIMP3_MAX_SAMPLES_PER_FRAME> pcm = {};
        mp3dec_frame_info_t info = {};
        const int samples = mp3dec_decode_frame(
            decoder, encoded->data() + *cursor,
            static_cast<int>(frame.frame_bytes), pcm.data(), &info);
        if (info.frame_bytes <= 0)
        {
            *cursor += frame.frame_bytes;
            mp3dec_init(decoder);
            *stream_found = false;
            return true;
        }
        *cursor += static_cast<std::size_t>(info.frame_bytes);
        QueueDecodedFrame(info, pcm.data(), samples);
        return true;
    }

    void CompactEncoded(std::vector<std::uint8_t>* encoded,
                        std::size_t* cursor)
    {
        if (*cursor == encoded->size())
        {
            encoded->clear();
            *cursor = 0U;
        }
        else if (*cursor >= 32768U)
        {
            encoded->erase(encoded->begin(), encoded->begin() + *cursor);
            *cursor = 0U;
        }
    }

    // What one pass of the decoder asks for next.
    enum class StepResult
    {
        // Decoded or took input; step again.
        kProgressed,
        // No PCM is needed yet; wait a little.
        kIdle,
        // PCM is needed but no input has arrived; wait for the guest to feed.
        kStarved,
    };

    // The decoder's state between passes. Issue #64: members rather than the
    // worker's locals, so the host turn can run the same passes on one thread.
    struct DecodeState
    {
        mp3dec_t decoder = {};
        std::vector<std::uint8_t> encoded;
        std::size_t cursor = 0U;
        bool stream_found = false;
        bool starvation_latched = false;
        std::array<std::uint8_t, kRingDrainBytes> incoming = {};
    };
    DecodeState decode;

    void ResetDecoder()
    {
        mp3dec_init(&decode.decoder);
        decode.encoded.clear();
        decode.encoded.reserve(16384U);
        decode.cursor = 0U;
        decode.stream_found = false;
        decode.starvation_latched = false;
    }

    // One pass: what the worker's loop did once.
    StepResult Step()
    {
        AdvanceFrameSyncToPlayback();
        if (!NeedsPcm())
        {
            return StepResult::kIdle;
        }

        const std::size_t count = compressed.Pop(decode.incoming);
        if (count != 0U)
        {
            if (stream_audit_enabled)
            {
                consumer_audit.Consume(
                    std::span<const std::uint8_t>(decode.incoming.data(),
                                                  count),
                    "consumer", LogStreamChunkDigest);
            }
            decode.encoded.insert(decode.encoded.end(), decode.incoming.begin(),
                                  decode.incoming.begin() + count);
            decode.starvation_latched = false;
        }

        bool progressed = false;
        while (NeedsPcm())
        {
            const std::size_t previous_cursor = decode.cursor;
            if (!DecodeOne(&decode.encoded, &decode.cursor, &decode.stream_found,
                           &decode.decoder))
            {
                break;
            }
            const std::size_t consumed = decode.cursor - previous_cursor;
            if (!compressed.Consume(consumed))
            {
                std::fprintf(
                    stderr,
                    "[repiu-piu10-mp3] compressed inflight "
                    "accounting failure consumed=%zu inflight=%zu\n",
                    consumed, compressed.inflight_size());
                shutdown.store(true, std::memory_order_release);
                break;
            }
            progressed = true;
            CompactEncoded(&decode.encoded, &decode.cursor);
        }
        CompactEncoded(&decode.encoded, &decode.cursor);
        decoder_pending_bytes.store(
            decode.encoded.size() - decode.cursor, std::memory_order_relaxed);

        if (!progressed && count == 0U)
        {
            if (playback_started.load(std::memory_order_relaxed) &&
                decode.encoded.size() == decode.cursor &&
                !decode.starvation_latched)
            {
                starvation_events.fetch_add(1U, std::memory_order_relaxed);
                decode.starvation_latched = true;
            }
            return StepResult::kStarved;
        }
        return StepResult::kProgressed;
    }

    void Run()
    {
        while (!shutdown.load(std::memory_order_acquire))
        {
            switch (Step())
            {
                case StepResult::kIdle:
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                    break;
                case StepResult::kStarved:
                {
                    std::unique_lock<std::mutex> lock(wake_mutex);
                    wake.wait_for(lock, std::chrono::milliseconds(2), [this]() {
                        return shutdown.load(std::memory_order_acquire) ||
                            compressed.ring_size() != 0U;
                    });
                    break;
                }
                case StepResult::kProgressed:
                    break;
            }
        }
        decoder_pending_bytes.store(0U, std::memory_order_relaxed);
    }
};

Piu10Mp3AudioOut::Piu10Mp3AudioOut() : impl_(std::make_unique<Impl>()) {}
Piu10Mp3AudioOut::~Piu10Mp3AudioOut() { Close(); }

std::size_t Piu10Mp3AudioOut::CalculateStartupSilenceBytes(
    std::uint32_t latency_ms, int sample_rate, int channels)
{
    if (sample_rate <= 0 || channels <= 0)
    {
        return 0U;
    }
    const std::uint64_t silence_frames =
        (static_cast<std::uint64_t>(sample_rate) * latency_ms) / 1000U;
    return static_cast<std::size_t>(
        silence_frames * static_cast<std::uint64_t>(channels) *
        sizeof(mp3d_sample_t));
}

bool Piu10Mp3AudioOut::Open()
{
    Close();
    // Issue #81: a device natively, the page's mixer in a browser.
    const SDL_AudioSpec format{
        SDL_AUDIO_S16LE, kDefaultChannels, kDefaultSampleRate};
    std::string failure;
    impl_->stream = OpenHostAudioStream(format, &failure);
    if (impl_->stream == nullptr)
    {
        impl_->message = "SDL PIU10 MP3 " + failure;
        std::fprintf(stderr, "[repiu-piu10-mp3] %s\n", impl_->message.c_str());
        Close();
        return false;
    }

    impl_->compressed.Reset();
    impl_->source_sample_rate = kDefaultSampleRate;
    impl_->source_channels = kDefaultChannels;
    impl_->reported_sample_rate.store(
        kDefaultSampleRate, std::memory_order_relaxed);
    impl_->reported_channels.store(
        kDefaultChannels, std::memory_order_relaxed);
    impl_->decoder_pending_bytes.store(0U, std::memory_order_relaxed);
    impl_->shutdown.store(false, std::memory_order_release);
    impl_->frame_sync.store(1U, std::memory_order_relaxed);
    impl_->frame_start_pcm_offsets.clear();
    impl_->total_pcm_bytes_put = 0U;
    impl_->ResetPlaybackClock();
    impl_->sync_toggles.store(0U, std::memory_order_relaxed);
    impl_->sync_multi_toggle_events.store(0U, std::memory_order_relaxed);
    impl_->pcm_empty_events.store(0U, std::memory_order_relaxed);
    impl_->pcm_queued_low_water_bytes.store(
        UINT64_MAX, std::memory_order_relaxed);
    impl_->device_buffer_frames = 0;
    {
        const SDL_AudioDeviceID device =
            SDL_GetAudioStreamDevice(impl_->stream);
        SDL_AudioSpec device_spec = {};
        int device_frames = 0;
        if (device != 0U &&
            SDL_GetAudioDeviceFormat(device, &device_spec, &device_frames))
        {
            impl_->device_buffer_frames = device_frames;
        }
    }
    impl_->startup_latency_pending = true;
    impl_->received_bytes.store(0U, std::memory_order_relaxed);
    impl_->dropped_bytes.store(0U, std::memory_order_relaxed);
    impl_->decoded_frames.store(0U, std::memory_order_relaxed);
    impl_->queued_pcm_frames.store(0U, std::memory_order_relaxed);
    impl_->starvation_events.store(0U, std::memory_order_relaxed);
    impl_->batched_bytes.store(0U, std::memory_order_relaxed);
    impl_->producer_audit.Reset();
    impl_->consumer_audit.Reset();
    impl_->playback_started.store(false, std::memory_order_relaxed);
    impl_->ResetDecoder();
    // Issue #64: on one thread the host turn runs the decoder instead.
    if (!runtime::execution_model::RunsHostOnOneThread())
    {
        impl_->worker = std::thread([this]() { impl_->Run(); });
    }
    SDL_ResumeAudioStreamDevice(impl_->stream);
    impl_->ready.store(true, std::memory_order_release);
    impl_->message = "PIU10 persistent MP3 ready through minimp3 and SDL3";
    std::fprintf(stderr, "[repiu-piu10-mp3] %s\n", impl_->message.c_str());
    return true;
}

// Issue #64. A bounded number of passes, so one turn cannot run long.
void Piu10Mp3AudioOut::Pump()
{
    constexpr int kStepsPerPump = 16;
    if (!impl_->ready.load(std::memory_order_acquire) ||
        impl_->worker.joinable() ||
        impl_->shutdown.load(std::memory_order_acquire))
    {
        return;
    }
    for (int step = 0; step < kStepsPerPump; ++step)
    {
        if (impl_->Step() != Impl::StepResult::kProgressed)
        {
            return;
        }
    }
}

void Piu10Mp3AudioOut::Close()
{
    impl_->ready.store(false, std::memory_order_release);
    impl_->shutdown.store(true, std::memory_order_release);
    impl_->wake.notify_all();
    if (impl_->worker.joinable())
    {
        impl_->worker.join();
    }
    const Piu10Mp3AudioStats final_stats = stats();
    if (final_stats.received_bytes != 0U)
    {
        std::fprintf(
            stderr,
            "[repiu-piu10-mp3] received/dropped/decoded/pcm/starved/"
            "batched/ring-high/inflight/inflight-high="
            "%llu/%llu/%llu/%llu/%llu/%llu/%zu/%zu/%zu\n",
            static_cast<unsigned long long>(final_stats.received_bytes),
            static_cast<unsigned long long>(final_stats.dropped_bytes),
            static_cast<unsigned long long>(final_stats.decoded_frames),
            static_cast<unsigned long long>(final_stats.queued_pcm_frames),
            static_cast<unsigned long long>(final_stats.starvation_events),
            static_cast<unsigned long long>(final_stats.batched_bytes),
            final_stats.ring_high_water, final_stats.inflight_bytes,
            final_stats.inflight_high_water);
    }
    if (impl_->stream_audit_enabled)
    {
        const sound::StreamChunkDigest producer =
            impl_->producer_audit.partial_digest();
        const sound::StreamChunkDigest consumer =
            impl_->consumer_audit.partial_digest();
        if (producer.byte_count != 0U)
        {
            LogStreamChunkDigest(
                "producer-final", producer);
        }
        if (consumer.byte_count != 0U)
        {
            LogStreamChunkDigest(
                "consumer-final", consumer);
        }
    }
    CloseHostAudioStream(impl_->stream);
    impl_->stream = nullptr;
}

bool Piu10Mp3AudioOut::SetGain(float gain)
{
    if (!impl_->ready.load(std::memory_order_acquire) ||
        !std::isfinite(gain) || gain < 0.0F)
    {
        return false;
    }
    if (!SDL_SetAudioStreamGain(impl_->stream, gain))
    {
        std::fprintf(stderr,
                     "[repiu-piu10-mp3] stream gain update failed: %s\n",
                     SDL_GetError());
        return false;
    }
    return true;
}

void Piu10Mp3AudioOut::SetStreamAuditEnabled(bool enabled)
{
    impl_->stream_audit_enabled = enabled;
}

void Piu10Mp3AudioOut::SetStartupLatencyMs(std::uint32_t latency_ms)
{
    impl_->startup_latency_ms = latency_ms;
}

bool Piu10Mp3AudioOut::WriteByte(std::uint8_t value)
{
    if (!impl_->ready.load(std::memory_order_acquire))
    {
        return false;
    }
    const bool was_empty = impl_->compressed.ring_size() == 0U;
    if (!impl_->compressed.PushByte(value))
    {
        impl_->dropped_bytes.fetch_add(1U, std::memory_order_relaxed);
        return false;
    }
    const std::uint64_t received =
        impl_->received_bytes.fetch_add(1U, std::memory_order_relaxed) + 1U;
    if (impl_->stream_audit_enabled)
    {
        impl_->producer_audit.Consume(
            std::span<const std::uint8_t>(&value, 1U),
            "producer", LogStreamChunkDigest);
    }
    if (was_empty || (received & 0xFFU) == 0U)
    {
        impl_->wake.notify_one();
    }
    return true;
}

std::size_t Piu10Mp3AudioOut::WriteBytes(
    std::span<const std::uint8_t> values)
{
    if (!impl_->ready.load(std::memory_order_acquire) || values.empty())
    {
        return 0U;
    }
    const std::size_t ring_bytes = impl_->compressed.ring_size();
    const bool was_empty = ring_bytes == 0U;
    const std::size_t count = impl_->compressed.PushBatch(values);
    if (count == 0U)
    {
        return 0U;
    }
    impl_->received_bytes.fetch_add(count, std::memory_order_relaxed);
    impl_->batched_bytes.fetch_add(count, std::memory_order_relaxed);
    if (impl_->stream_audit_enabled)
    {
        impl_->producer_audit.Consume(
            values.first(count), "producer",
            LogStreamChunkDigest);
    }
    if (was_empty)
    {
        impl_->wake.notify_one();
    }
    return count;
}

bool Piu10Mp3AudioOut::available() const
{
    return impl_->ready.load(std::memory_order_acquire);
}

bool Piu10Mp3AudioOut::demand() const
{
    return available() &&
        impl_->compressed.demand();
}

std::uint8_t Piu10Mp3AudioOut::frame_sync() const
{
    return NoteGuestFrameSyncRead(
        impl_->frame_sync.load(std::memory_order_relaxed),
        TimerTraceNowNanoseconds());
}

std::uint8_t Piu10Mp3AudioOut::FrameSyncAt(std::uint64_t time_ns) const
{
    return NoteGuestFrameSyncRead(
        impl_->frame_sync_history.ValueAt(
            time_ns, impl_->frame_sync.load(std::memory_order_relaxed)),
        time_ns);
}

std::uint8_t Piu10Mp3AudioOut::NoteGuestFrameSyncRead(
    std::uint8_t value, std::uint64_t answer_ns) const
{
    if (TimerLatencyTraceEnabled() && value != impl_->trace_last_seen_sync)
    {
        impl_->trace_last_seen_sync = value;
        RecordTimerTrace(TimerTraceEvent::kSeen, TimerTraceNowNanoseconds(),
                         value, 0U, answer_ns);
    }
    if (impl_->position_trace)
    {
        ++impl_->reads_since_seen;
        if (value != impl_->last_seen_sync)
        {
            const std::int64_t now_us = impl_->TraceMicroseconds();
            fprintf(stderr,
                    "[repiu-mp3-pos] seen seq=%llu t_us=%lld delay_us=%lld"
                    " reads=%llu\n",
                    static_cast<unsigned long long>(
                        impl_->toggle_seq.load(std::memory_order_relaxed)),
                    static_cast<long long>(now_us),
                    static_cast<long long>(
                        now_us -
                        impl_->last_toggle_us.load(std::memory_order_relaxed)),
                    static_cast<unsigned long long>(impl_->reads_since_seen));
            impl_->last_seen_sync = value;
            impl_->reads_since_seen = 0U;
        }
    }
    return value;
}

double Piu10Mp3AudioOut::frame_seconds() const
{
    const int rate =
        impl_->reported_sample_rate.load(std::memory_order_relaxed);
    const int samples =
        impl_->reported_frame_samples.load(std::memory_order_relaxed);
    return rate > 0 ? static_cast<double>(samples) / rate : 0.0;
}

Piu10Mp3AudioStats Piu10Mp3AudioOut::stats() const
{
    return Piu10Mp3AudioStats{
        impl_->received_bytes.load(std::memory_order_relaxed),
        impl_->dropped_bytes.load(std::memory_order_relaxed),
        impl_->decoded_frames.load(std::memory_order_relaxed),
        impl_->queued_pcm_frames.load(std::memory_order_relaxed),
        impl_->starvation_events.load(std::memory_order_relaxed),
        impl_->batched_bytes.load(std::memory_order_relaxed),
        impl_->sync_toggles.load(std::memory_order_relaxed),
        impl_->sync_multi_toggle_events.load(std::memory_order_relaxed),
        impl_->pcm_empty_events.load(std::memory_order_relaxed),
        impl_->pcm_queued_low_water_bytes.load(std::memory_order_relaxed) ==
                UINT64_MAX
            ? 0U
            : impl_->pcm_queued_low_water_bytes.load(
                  std::memory_order_relaxed),
        impl_->device_buffer_frames,
        impl_->compressed.ring_size(), impl_->compressed.ring_high_water(),
        impl_->compressed.inflight_size(),
        impl_->compressed.inflight_high_water()};
}

Piu10Mp3AudioSnapshot Piu10Mp3AudioOut::Snapshot() const
{
    Piu10Mp3AudioSnapshot snapshot;
    snapshot.compressed_ring_bytes = impl_->compressed.ring_size();
    snapshot.decoder_pending_bytes =
        impl_->decoder_pending_bytes.load(std::memory_order_relaxed);
    snapshot.compressed_inflight_bytes = impl_->compressed.inflight_size();
    snapshot.received_bytes =
        impl_->received_bytes.load(std::memory_order_relaxed);
    snapshot.decoded_frames =
        impl_->decoded_frames.load(std::memory_order_relaxed);
    snapshot.frame_sync =
        impl_->frame_sync.load(std::memory_order_relaxed);
    snapshot.demand = demand();
    snapshot.sync_toggles =
        impl_->sync_toggles.load(std::memory_order_relaxed);
    snapshot.sync_multi_toggle_events =
        impl_->sync_multi_toggle_events.load(std::memory_order_relaxed);
    snapshot.starvation_events =
        impl_->starvation_events.load(std::memory_order_relaxed);
    snapshot.pcm_empty_events =
        impl_->pcm_empty_events.load(std::memory_order_relaxed);
    {
        const std::uint64_t rate = impl_->PcmBytesPerSecond();
        snapshot.playback_lag_ms = rate != 0U
            ? static_cast<double>(impl_->playback_lag_bytes.load(
                  std::memory_order_relaxed)) * 1000.0 /
                static_cast<double>(rate)
            : 0.0;
    }
    snapshot.pcm_sample_rate =
        impl_->reported_sample_rate.load(std::memory_order_relaxed);
    snapshot.pcm_channels =
        impl_->reported_channels.load(std::memory_order_relaxed);

    snapshot.available =
        impl_->ready.load(std::memory_order_acquire) && impl_->stream != nullptr;
    if (!snapshot.available)
    {
        return snapshot;
    }

    snapshot.pcm_queued_bytes = SDL_GetAudioStreamQueued(impl_->stream);
    const std::uint64_t pcm_bytes_per_second =
        static_cast<std::uint64_t>(snapshot.pcm_sample_rate) *
        static_cast<std::uint64_t>(snapshot.pcm_channels) *
        sizeof(mp3d_sample_t);
    if (snapshot.pcm_queued_bytes >= 0 && pcm_bytes_per_second != 0U)
    {
        snapshot.pcm_queued_ms =
            static_cast<double>(snapshot.pcm_queued_bytes) * 1000.0 /
            static_cast<double>(pcm_bytes_per_second);
    }

    const SDL_AudioDeviceID device =
        SDL_GetAudioStreamDevice(impl_->stream);
    SDL_AudioSpec device_spec = {};
    if (device != 0U && SDL_GetAudioDeviceFormat(
            device, &device_spec, &snapshot.device_buffer_frames))
    {
        snapshot.device_sample_rate = device_spec.freq;
        if (snapshot.device_sample_rate > 0)
        {
            snapshot.device_buffer_ms =
                static_cast<double>(snapshot.device_buffer_frames) * 1000.0 /
                static_cast<double>(snapshot.device_sample_rate);
        }
    }
    return snapshot;
}

const std::string& Piu10Mp3AudioOut::message() const
{
    return impl_->message;
}

}  // namespace repiu::engine
