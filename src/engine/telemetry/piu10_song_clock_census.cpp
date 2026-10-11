#include "repiu/engine/piu10_song_clock_census.h"

#include <cmath>

namespace repiu::engine
{
namespace
{

double Milliseconds(std::uint64_t nanoseconds)
{
    return static_cast<double>(nanoseconds) / 1000000.0;
}

std::int64_t Microseconds(double milliseconds)
{
    return static_cast<std::int64_t>(std::llround(milliseconds * 1000.0));
}

void RaiseMaximum(std::atomic<std::int64_t>* maximum, std::int64_t value)
{
    std::int64_t current = maximum->load(std::memory_order_relaxed);
    while (value > current &&
           !maximum->compare_exchange_weak(
               current, value, std::memory_order_relaxed))
    {
    }
}

}  // namespace

bool Piu10SongClockCensus::Observe(const Piu10SongClockRead& read,
                                   Piu10SongClockResync* resync)
{
    if (!primed_)
    {
        primed_ = true;
        last_sync_ = read.frame_sync;
        last_ticks_ = read.ticks_injected;
        last_tick_ns_ = read.now_ns;
        return false;
    }

    // The feeder reads the status from inside the INT 8 handler, so a read
    // that sees a new tick count is the guest's view of a tick arriving. Only
    // gaps inside a playing song count; between songs the feeder is idle.
    const bool in_song = frames_ != 0U &&
        read.now_ns - last_toggle_ns_ < kPiu10SongClockSongGapNanoseconds;
    if (read.ticks_injected != last_ticks_)
    {
        const double gap_ms = Milliseconds(read.now_ns - last_tick_ns_);
        if (in_song && window_valid_ && gap_ms > window_tick_gap_max_ms_)
        {
            window_tick_gap_max_ms_ = gap_ms;
            window_burst_max_ = read.ticks_injected - last_ticks_;
        }
        last_ticks_ = read.ticks_injected;
        last_tick_ns_ = read.now_ns;
    }

    if (read.frame_sync == last_sync_)
    {
        return false;
    }
    last_sync_ = read.frame_sync;

    if (!in_song)
    {
        ++song_;
        songs_.fetch_add(1U, std::memory_order_relaxed);
        frames_ = 0U;
        window_valid_ = false;
    }
    else if (window_valid_)
    {
        const double toggle_gap_ms =
            Milliseconds(read.now_ns - last_toggle_ns_);
        if (toggle_gap_ms > window_toggle_gap_max_ms_)
        {
            window_toggle_gap_max_ms_ = toggle_gap_ms;
        }
    }
    ++frames_;
    last_toggle_ns_ = read.now_ns;

    if (frames_ % kPiu10SongClockResyncFrames != 0U)
    {
        return false;
    }

    bool completed = false;
    if (window_valid_ && resync != nullptr && read.pit_hz > 0.0)
    {
        Piu10SongClockResync result;
        result.song = song_;
        result.frame = frames_;
        result.ticks = read.ticks_injected - window_ticks_;
        result.dropped = read.ticks_dropped - window_dropped_;
        result.window_ms = Milliseconds(read.now_ns - window_start_ns_);
        result.audio_ms = static_cast<double>(kPiu10SongClockResyncFrames) *
            read.frame_seconds * 1000.0;
        result.jump_ms = result.audio_ms -
            static_cast<double>(result.ticks) * 1000.0 / read.pit_hz;
        result.tick_gap_max_ms = window_tick_gap_max_ms_;
        result.toggle_gap_max_ms = window_toggle_gap_max_ms_;
        result.tick_burst_max = window_burst_max_;
        result.frames = window_frames_;
        result.uneven_frames = window_uneven_frames_;
        Record(result);
        *resync = result;
        completed = true;
    }

    window_valid_ = true;
    window_start_ns_ = read.now_ns;
    window_ticks_ = read.ticks_injected;
    window_dropped_ = read.ticks_dropped;
    window_tick_gap_max_ms_ = 0.0;
    window_toggle_gap_max_ms_ = 0.0;
    window_burst_max_ = 0U;
    window_frames_ = 0U;
    window_uneven_frames_ = 0U;
    return completed;
}

void Piu10SongClockCensus::ObserveSwap(std::uint64_t now_ns,
                                       std::uint32_t ticks_injected,
                                       double pit_hz)
{
    const bool in_song = frames_ != 0U &&
        now_ns - last_toggle_ns_ < kPiu10SongClockSongGapNanoseconds;
    const std::uint32_t ticks = ticks_injected - swap_ticks_;
    const bool counted = swap_seen_ && in_song;
    if (swap_seen_)
    {
        // A running mean of the swap interval over about 32 frames.
        const double interval = static_cast<double>(now_ns - swap_ns_);
        swap_interval_ns_ = swap_interval_ns_ == 0.0
            ? interval
            : swap_interval_ns_ + (interval - swap_interval_ns_) / 32.0;
    }
    swap_seen_ = true;
    swap_ticks_ = ticks_injected;
    swap_ns_ = now_ns;
    if (!counted)
    {
        return;
    }
    const std::uint32_t bucket =
        ticks < kPiu10SongFrameTickBuckets - 1U
            ? ticks : kPiu10SongFrameTickBuckets - 1U;
    frame_ticks_[bucket].fetch_add(1U, std::memory_order_relaxed);
    if (!window_valid_)
    {
        return;
    }
    ++window_frames_;
    const double share = pit_hz * swap_interval_ns_ / 1e9;
    if (share > 0.0 && std::fabs(static_cast<double>(ticks) - share) >= 2.0)
    {
        ++window_uneven_frames_;
    }
}

void Piu10SongClockCensus::Record(const Piu10SongClockResync& resync)
{
    resyncs_.fetch_add(1U, std::memory_order_relaxed);
    if (std::fabs(resync.jump_ms) >= kPiu10SongClockReportJumpMs)
    {
        jumps_over_report_.fetch_add(1U, std::memory_order_relaxed);
    }
    if (resync.jump_ms > 0.0)
    {
        RaiseMaximum(&jump_max_ahead_us_, Microseconds(resync.jump_ms));
    }
    else
    {
        RaiseMaximum(&jump_max_back_us_, Microseconds(-resync.jump_ms));
    }
    RaiseMaximum(&tick_gap_max_us_, Microseconds(resync.tick_gap_max_ms));
    RaiseMaximum(&toggle_gap_max_us_,
                 Microseconds(resync.toggle_gap_max_ms));
}

Piu10SongClockSummary Piu10SongClockCensus::Summary() const
{
    Piu10SongClockSummary summary;
    summary.songs = songs_.load(std::memory_order_relaxed);
    summary.resyncs = resyncs_.load(std::memory_order_relaxed);
    summary.jumps_over_report =
        jumps_over_report_.load(std::memory_order_relaxed);
    summary.jump_max_ahead_ms = static_cast<double>(
        jump_max_ahead_us_.load(std::memory_order_relaxed)) / 1000.0;
    summary.jump_max_back_ms = static_cast<double>(
        jump_max_back_us_.load(std::memory_order_relaxed)) / 1000.0;
    summary.tick_gap_max_ms = static_cast<double>(
        tick_gap_max_us_.load(std::memory_order_relaxed)) / 1000.0;
    summary.toggle_gap_max_ms = static_cast<double>(
        toggle_gap_max_us_.load(std::memory_order_relaxed)) / 1000.0;
    for (std::uint32_t bucket = 0; bucket < kPiu10SongFrameTickBuckets;
         ++bucket)
    {
        summary.frame_ticks[bucket] =
            frame_ticks_[bucket].load(std::memory_order_relaxed);
    }
    return summary;
}

}  // namespace repiu::engine
