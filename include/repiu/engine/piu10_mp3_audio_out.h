#ifndef REPIU_ENGINE_PIU10_MP3_AUDIO_OUT_H_
#define REPIU_ENGINE_PIU10_MP3_AUDIO_OUT_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>

namespace repiu::engine
{

struct Piu10Mp3AudioStats
{
    std::uint64_t received_bytes = 0;
    std::uint64_t dropped_bytes = 0;
    std::uint64_t decoded_frames = 0;
    std::uint64_t queued_pcm_frames = 0;
    std::uint64_t starvation_events = 0;
    std::uint64_t batched_bytes = 0;
    // Task 740. Frame-sync toggles handed to the guest, and the worker calls
    // that toggled more than once at a time -- a guest polling the status at
    // 240 Hz reads an even count of those as no change and loses the frame.
    std::uint64_t sync_toggles = 0;
    std::uint64_t sync_multi_toggle_events = 0;
    // Task 740. Measured where the decoder refills the PCM queue, one frame
    // at a time while a song plays: the least PCM found there, and how often
    // the queue was found empty with the previous refill less than 200 ms
    // before (a gap on the speaker, not the tail of a song).
    std::uint64_t pcm_empty_events = 0;
    std::uint64_t pcm_queued_low_water_bytes = 0;
    // The SDL device's buffer in sample frames: the step in which it pulls
    // PCM, and so the step in which everything derived from the queue moved
    // before Task 740.
    int device_buffer_frames = 0;
    std::size_t ring_bytes = 0;
    std::size_t ring_high_water = 0;
    std::size_t inflight_bytes = 0;
    std::size_t inflight_high_water = 0;
};

struct Piu10Mp3AudioSnapshot
{
    bool available = false;
    int pcm_queued_bytes = -1;
    double pcm_queued_ms = 0.0;
    int pcm_sample_rate = 0;
    int pcm_channels = 0;
    int device_buffer_frames = 0;
    double device_buffer_ms = 0.0;
    int device_sample_rate = 0;
    std::size_t compressed_ring_bytes = 0;
    std::size_t decoder_pending_bytes = 0;
    std::size_t compressed_inflight_bytes = 0;
    std::uint64_t received_bytes = 0;
    std::uint64_t decoded_frames = 0;
    std::uint8_t frame_sync = 0;
    bool demand = false;
    std::uint64_t sync_toggles = 0;
    std::uint64_t sync_multi_toggle_events = 0;
    std::uint64_t starvation_events = 0;
    std::uint64_t pcm_empty_events = 0;
    // Task 740. How far the playback clock sits behind the bytes the device
    // has pulled; at most one device buffer while the device keeps time.
    double playback_lag_ms = 0.0;
};

class Piu10Mp3AudioOut
{
public:
    static constexpr std::size_t kCompressedFifoBytes = 0x0E00U;

    Piu10Mp3AudioOut();
    ~Piu10Mp3AudioOut();

    Piu10Mp3AudioOut(const Piu10Mp3AudioOut&) = delete;
    Piu10Mp3AudioOut& operator=(const Piu10Mp3AudioOut&) = delete;

    static std::size_t CalculateStartupSilenceBytes(
        std::uint32_t latency_ms, int sample_rate, int channels);

    bool Open();
    void Close();
    bool SetGain(float gain);
    void SetStartupLatencyMs(std::uint32_t latency_ms);
    void SetStreamAuditEnabled(bool enabled);
    bool WriteByte(std::uint8_t value);
    std::size_t WriteBytes(std::span<const std::uint8_t> values);

    bool available() const;
    bool demand() const;
    std::uint8_t frame_sync() const;
    // Issue #58. The frame-sync bit as it was at `time_ns` (steady clock),
    // for a read inside an injected timer handler; see
    // piu10_frame_sync_history.h.
    std::uint8_t FrameSyncAt(std::uint64_t time_ns) const;
    // Issue #58. Seconds of audio in the latest decoded frame; 1152 / 44100
    // before the first one.
    double frame_seconds() const;
    Piu10Mp3AudioStats stats() const;
    Piu10Mp3AudioSnapshot Snapshot() const;
    const std::string& message() const;

private:
    std::uint8_t NoteGuestFrameSyncRead(std::uint8_t value,
                                        std::uint64_t answer_ns) const;

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace repiu::engine

#endif  // REPIU_ENGINE_PIU10_MP3_AUDIO_OUT_H_
