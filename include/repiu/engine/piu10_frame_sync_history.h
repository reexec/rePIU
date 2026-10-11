#pragma once

#include <atomic>
#include <cstdint>

namespace repiu::engine
{

// Issue #58. The PIU10 games count MP3 frames by watching the frame-sync bit
// (status bit 2 at 0x02DA) from their INT 8 handler, and every 256 frames set
// the song clock their arrows read from that count. The original ISR runs at
// 240 Hz, so it sees every change of a bit that flips every 26.1 ms. Here the
// ticks reach the guest late and in bursts -- on Win32 with vsync four at once,
// once a frame -- and a burst read the bit as it is now, four times at the
// same instant: two flips between bursts cancelled and the game lost 52 ms of
// song position for good.
//
// The JAMMA inputs already answer a read inside an injected handler with the
// state at that tick's due time (Tasks 492/493). This gives the frame-sync bit
// the same treatment: the audio worker records when the bit flipped, and a
// read inside a handler is answered with the bit as it was at the tick's due
// time. The answer's time never runs backwards, so a read outside every
// handler (which sees the present) cannot be followed by an older state that
// the game would count as one more flip.
class Piu10FrameSyncHistory
{
public:
    // Enough flips for the tick backlog's bound (64 ticks, 0.27 s) many times
    // over: 256 flips are 6.7 s of a song.
    static constexpr std::uint32_t kCapacity = 256U;

    // The audio worker, after flipping the bit to `value` at `time_ns`.
    void Record(std::uint64_t time_ns, std::uint8_t value);

    // The bit as it was at `time_ns`. `live` is the bit now, returned when no
    // recorded flip is later than `time_ns`.
    std::uint8_t ValueAt(std::uint64_t time_ns, std::uint8_t live) const;

private:
    struct Edge
    {
        std::uint64_t time_ns = 0;
        std::uint8_t value = 0;
    };

    mutable std::atomic_flag lock_ = ATOMIC_FLAG_INIT;
    Edge edges_[kCapacity] = {};
    std::uint32_t next_ = 0;
    std::uint32_t size_ = 0;
};

// The time a read is answered for: the guest's own time, which is the due
// time of the tick whose handler is reading or, outside every handler, of
// the latest tick delivered. Reads outside a handler are frequent (the
// CAT702 exchange shares the status port), and answering those at the
// present would run the answers ahead of the handler's ticks. Never later
// than `now_ns`, never earlier than an answer already given, and the present
// before any tick has been delivered. Guest thread only.
class Piu10FrameSyncAnswerClock
{
public:
    std::uint64_t Answer(std::uint64_t guest_ns, std::uint64_t now_ns);

private:
    std::uint64_t last_answer_ns_ = 0;
};

}  // namespace repiu::engine
