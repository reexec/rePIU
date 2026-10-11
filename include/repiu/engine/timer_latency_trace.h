#pragma once

#include <cstdint>

namespace repiu::engine
{

// Issue #58. Where a timer tick spends the time between coming due and
// reaching the guest. On the original machine IRQ0 interrupts any guest
// instruction with IF set, so the ISR runs within microseconds of the PIT
// edge; here a tick lands only at a safe point the guest thread reaches, and
// the guest thread also spends time in host work (Glide calls handed to the
// host thread, DOS services) where no safe point exists. A device the ISR
// samples -- the PIU10 MP3 frame-sync -- is then read late, several ticks at
// one instant.
//
// `REPIU_TIMER_LATENCY_TRACE=<path>` records, on the steady clock the tick
// schedule uses:
//   due     every tick the schedule accepted, at its due time
//   inject  every INT 8 pushed, with the due time of the tick it carries
//   defer   every injection attempt a safe-point condition turned away
//   gate    Glide calls that held the guest thread 1 ms or more
//   dos     INT 21h services that held it 0.5 ms or more
//   toggle  MP3 frame-sync changes, and the guest reads that saw them
//   swap    buffer swaps
//   read    every PIU10 MP3 status read: the bit answered, whether inside a
//           timer handler, and the time it was answered for
// and writes them as text to the path at teardown. Off, each site costs one
// load of a static flag.

enum class TimerTraceEvent : std::uint8_t
{
    kDue = 1,
    kInject = 2,
    kDefer = 3,
    kGate = 4,
    kDos = 5,
    kToggle = 6,
    kSeen = 7,
    kSwap = 8,
    kStatusRead = 9,
};

// Why an injection attempt was turned away, in the order the checks run.
enum class TimerDeferReason : std::uint32_t
{
    kNotGuestCode = 1,
    kInterruptFlag = 2,
    kGuestCli = 3,
    kInService = 4,
    kPostReturnChain = 5,
    kCacheAddress = 6,
    kSelector = 7,
};

bool TimerLatencyTraceEnabled();
std::uint64_t TimerTraceNowNanoseconds();
// `value` is the event's subject (tick ordinal, reason, Glide ordinal, AH,
// toggle sequence); `extra` is a duration in microseconds for gate and dos;
// `aux_ns` is a second timestamp (an injected tick's due time).
void RecordTimerTrace(TimerTraceEvent event, std::uint64_t time_ns,
                      std::uint32_t value, std::uint32_t extra = 0U,
                      std::uint64_t aux_ns = 0U);
// Teardown, after the guest thread has stopped.
void WriteTimerLatencyTrace();

}  // namespace repiu::engine
