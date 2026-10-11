#pragma once

#include <atomic>
#include <cstdint>

namespace repiu::engine
{

// Issue #58. The PIU10 games keep the song clock their arrows read on the
// timer: the INT 8 handler adds one per tick and then runs the MP3 feeder,
// which counts frame-sync changes (status bit 2 at 0x02DA) and, every 256
// counted frames, overwrites the song clock with `frames x ticks-per-frame`
// (pumpipx3: the ISR at 0x0401213D, the feeder at 0x0402D86B, the setter at
// 0x040121CD, 6.2688 ticks per frame). Between overwrites the arrows run on
// delivered ticks; at an overwrite they snap to the MP3 position. Ticks that
// stall, go missing or bunch up, or an MP3 position that stalls, therefore
// show as an arrow jump about every 6.7 seconds.
//
// This census replays that bookkeeping from the guest's own reads of the
// status port, so every overwrite's jump can be measured without knowing the
// game's addresses: the guest thread calls `Observe` on each status read with
// the frame-sync bit it is about to return and the tick counters at that
// moment. The game's ticks-per-frame constant is replaced by the host's
// (PIT rate x samples per frame / sample rate), which for pumpipx3 differs
// from the game's by 0.03%, a steady -1.9 ms per overwrite that the real
// cabinet shares.

// Frames between overwrites, as the game counts them.
constexpr std::uint32_t kPiu10SongClockResyncFrames = 256U;
// A frame-sync change after this long without one starts a new song: the
// game resets its frame count when it starts a stream, and the stream is
// silent in between.
constexpr std::uint64_t kPiu10SongClockSongGapNanoseconds = 1000000000ULL;
// Buckets of the per-frame tick histogram; the last one holds that many or
// more. At 60 frames a second a frame owes four ticks.
constexpr std::uint32_t kPiu10SongFrameTickBuckets = 10U;
// Jumps at or above this size are printed even when the per-overwrite log
// is off. One tick is 4.2 ms.
constexpr double kPiu10SongClockReportJumpMs = 8.0;

struct Piu10SongClockRead
{
    std::uint64_t now_ns = 0;
    std::uint8_t frame_sync = 0;
    std::uint32_t ticks_injected = 0;
    std::uint32_t ticks_dropped = 0;
    double pit_hz = 0.0;
    // Seconds of audio in one MP3 frame (1152 / 44100 for the PIU10 songs).
    double frame_seconds = 0.0;
};

// One overwrite. `jump_ms` is how far the arrows move when it happens:
// positive is a jump ahead (the ticks fell behind the music), negative a step
// back (the ticks ran ahead of it).
struct Piu10SongClockResync
{
    std::uint32_t song = 0;
    std::uint32_t frame = 0;
    double jump_ms = 0.0;
    std::uint32_t ticks = 0;
    std::uint32_t dropped = 0;
    double window_ms = 0.0;
    double audio_ms = 0.0;
    // The longest wait between two reads that saw a new tick: how long the
    // arrows stood still inside this window.
    double tick_gap_max_ms = 0.0;
    // The longest wait between two observed frame-sync changes: how long the
    // MP3 position stood still.
    double toggle_gap_max_ms = 0.0;
    // Ticks the guest saw at once after its longest tick gap.
    std::uint32_t tick_burst_max = 0;
    // Swaps in the window, and those whose interval carried a tick count two
    // or more away from the frame's share: frames in which the arrows stood
    // nearly still or leapt.
    std::uint32_t frames = 0;
    std::uint32_t uneven_frames = 0;
};

struct Piu10SongClockSummary
{
    std::uint32_t songs = 0;
    std::uint32_t resyncs = 0;
    std::uint32_t jumps_over_report = 0;
    double jump_max_ahead_ms = 0.0;
    double jump_max_back_ms = 0.0;
    double tick_gap_max_ms = 0.0;
    double toggle_gap_max_ms = 0.0;
    // Ticks delivered between consecutive buffer swaps during songs. The
    // arrows advance by the song clock once per drawn frame, so this is how
    // far they move from one frame to the next.
    std::uint32_t frame_ticks[kPiu10SongFrameTickBuckets] = {};
};

class Piu10SongClockCensus
{
public:
    // Returns true and fills `resync` when this read completed an overwrite
    // window. Guest thread only.
    bool Observe(const Piu10SongClockRead& read, Piu10SongClockResync* resync);

    // Called at every buffer swap with the tick counter at that moment.
    // Guest thread only, like `Observe`. A frame's share of ticks follows
    // the measured swap rate, since with vsync the game draws at whatever
    // the display refreshes at.
    void ObserveSwap(std::uint64_t now_ns, std::uint32_t ticks_injected,
                     double pit_hz);

    // Safe from any thread.
    Piu10SongClockSummary Summary() const;

private:
    void Record(const Piu10SongClockResync& resync);

    bool primed_ = false;
    std::uint8_t last_sync_ = 0;
    std::uint64_t last_toggle_ns_ = 0;
    std::uint32_t song_ = 0;
    std::uint32_t frames_ = 0;
    // The previous overwrite; invalid until a song's first 256 frames pass,
    // since the window before it starts at the game's own clock reset, which
    // this census cannot see.
    bool window_valid_ = false;
    std::uint64_t window_start_ns_ = 0;
    std::uint32_t window_ticks_ = 0;
    std::uint32_t window_dropped_ = 0;
    double window_tick_gap_max_ms_ = 0.0;
    double window_toggle_gap_max_ms_ = 0.0;
    std::uint32_t window_burst_max_ = 0;
    std::uint32_t last_ticks_ = 0;
    std::uint64_t last_tick_ns_ = 0;
    bool swap_seen_ = false;
    std::uint32_t swap_ticks_ = 0;
    std::uint64_t swap_ns_ = 0;
    double swap_interval_ns_ = 0.0;
    std::uint32_t window_frames_ = 0;
    std::uint32_t window_uneven_frames_ = 0;

    std::atomic<std::uint32_t> frame_ticks_[kPiu10SongFrameTickBuckets] = {};
    std::atomic<std::uint32_t> songs_{0};
    std::atomic<std::uint32_t> resyncs_{0};
    std::atomic<std::uint32_t> jumps_over_report_{0};
    std::atomic<std::int64_t> jump_max_ahead_us_{0};
    std::atomic<std::int64_t> jump_max_back_us_{0};
    std::atomic<std::int64_t> tick_gap_max_us_{0};
    std::atomic<std::int64_t> toggle_gap_max_us_{0};
};

}  // namespace repiu::engine
