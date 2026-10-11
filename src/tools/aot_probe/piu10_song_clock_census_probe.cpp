#include "piu10_song_clock_census_probe.h"

#include "repiu/engine/piu10_song_clock_census.h"

#include <cmath>
#include <algorithm>
#include <cstdint>
#include <iostream>
#include <vector>

namespace repiu::tools
{
namespace
{

using engine::Piu10SongClockCensus;
using engine::Piu10SongClockRead;
using engine::Piu10SongClockResync;

constexpr double kPitHz = 240.0;
// 1152 / 44100 s, rounded so a frame is a whole number of nanoseconds.
constexpr std::uint64_t kFrameNs = 26122449ULL;
constexpr double kFrameSeconds = 26122449.0 / 1e9;
constexpr std::uint64_t kTickNs = 1000000000ULL / 240ULL;

// A song played to the census as the guest's ISR would read it: one read per
// tick, the frame-sync bit flipping at every frame boundary. A nonzero
// `stall_ns` holds ticks back from `stall_from_ns` and then delivers them at
// once; `tick_drop` loses that many of them at the same moment.
struct Song
{
    std::uint64_t start_ns = 0;
    std::uint32_t frames = 0;
    std::uint64_t stall_from_ns = 0;
    std::uint64_t stall_ns = 0;
    std::uint32_t tick_drop = 0;
};

std::vector<Piu10SongClockResync> Play(Piu10SongClockCensus* census,
                                       const Song& song,
                                       std::uint32_t* ticks)
{
    std::vector<Piu10SongClockResync> resyncs;
    const std::uint64_t end_ns = song.start_ns + song.frames * kFrameNs;
    std::uint32_t dropped = 0;
    std::uint32_t held = 0;
    bool stall_settled = false;
    for (std::uint64_t now = song.start_ns; now <= end_ns + kTickNs;
         now += kTickNs)
    {
        const bool stalled = song.stall_ns != 0U &&
            now >= song.stall_from_ns &&
            now < song.stall_from_ns + song.stall_ns;
        if (stalled)
        {
            ++held;
            continue;
        }
        if (held != 0U && !stall_settled)
        {
            stall_settled = true;
            *ticks += held + 1U - song.tick_drop;
            dropped += song.tick_drop;
        }
        else
        {
            ++*ticks;
        }
        Piu10SongClockRead read;
        read.now_ns = now;
        // Frame n plays from start + n * frame; the bit is the parity of the
        // frames begun so far.
        const std::uint64_t begun = (now - song.start_ns) / kFrameNs + 1U;
        read.frame_sync = static_cast<std::uint8_t>(
            std::min<std::uint64_t>(begun, song.frames) & 1U);
        read.ticks_injected = *ticks;
        read.ticks_dropped = dropped;
        read.pit_hz = kPitHz;
        read.frame_seconds = kFrameSeconds;
        Piu10SongClockResync resync;
        if (census->Observe(read, &resync))
        {
            resyncs.push_back(resync);
        }
    }
    return resyncs;
}

}  // namespace

bool RunPiu10SongClockCensusProbe()
{
    // A clean song: ticks keep time with the music, so every overwrite after
    // the first moves the arrows by less than one tick.
    Piu10SongClockCensus clean;
    std::uint32_t ticks = 0;
    const auto clean_resyncs = Play(&clean, Song{1000000000ULL, 1100U}, &ticks);
    bool clean_ok = clean_resyncs.size() == 3U;
    for (const auto& resync : clean_resyncs)
    {
        clean_ok = clean_ok && resync.song == 1U &&
            resync.frame % 256U == 0U && std::fabs(resync.jump_ms) < 4.2 &&
            resync.dropped == 0U && resync.tick_gap_max_ms < 5.0 &&
            resync.toggle_gap_max_ms < 30.0;
    }

    // A 20 ms stall whose owed ticks all arrive: the arrows stand still and
    // catch up inside the window, so the overwrite does not move them. The
    // stall is shorter than an MP3 frame, so no frame-sync change is lost in
    // it (a longer one can hide an even number of them from the guest too).
    Piu10SongClockCensus stall;
    ticks = 0;
    const std::uint64_t start = 1000000000ULL;
    const std::uint64_t stall_from = start + 300U * kFrameNs + 2000000ULL;
    const auto stall_resyncs = Play(
        &stall, Song{start, 800U, stall_from, 20000000ULL, 0U}, &ticks);
    const bool stall_ok = stall_resyncs.size() == 2U &&
        stall_resyncs[0].tick_gap_max_ms > 18.0 &&
        stall_resyncs[0].tick_burst_max >= 4U &&
        std::fabs(stall_resyncs[0].jump_ms) < 4.2;

    // The same stall with three ticks lost: the overwrite jumps the arrows
    // ahead by those three ticks.
    Piu10SongClockCensus drop;
    ticks = 0;
    const auto drop_resyncs = Play(
        &drop, Song{start, 800U, stall_from, 20000000ULL, 3U}, &ticks);
    const bool drop_ok = drop_resyncs.size() == 2U &&
        drop_resyncs[0].dropped == 3U &&
        std::fabs(drop_resyncs[0].jump_ms - 3.0 * 1000.0 / kPitHz) < 4.2 &&
        drop.Summary().jump_max_ahead_ms > 8.0;

    // A second song after a silent gap starts its own count.
    Piu10SongClockCensus two;
    ticks = 0;
    Play(&two, Song{start, 600U}, &ticks);
    const auto second = Play(&two, Song{start + 600U * kFrameNs +
                                            3000000000ULL,
                                        600U},
                             &ticks);
    const bool songs_ok = two.Summary().songs == 2U && second.size() == 1U &&
        second[0].song == 2U && second[0].frame == 512U;

    const bool valid = clean_ok && stall_ok && drop_ok && songs_ok;
    std::cout << "piu10_song_clock_census_probe="
              << (valid ? "true" : "false") << " clean=" << clean_ok
              << " stall=" << stall_ok << " drop=" << drop_ok
              << " songs=" << songs_ok << '\n';
    return valid;
}

}  // namespace repiu::tools
