#include "repiu/engine/timer_latency_trace.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>

namespace repiu::engine
{
namespace
{

struct TraceRecord
{
    std::uint64_t time_ns;
    std::uint64_t aux_ns;
    std::uint32_t value;
    std::uint32_t extra;
    std::uint8_t event;
};

// About ten minutes of a busy song: 240 due and 240 inject a second, a few
// hundred defers, toggles and swaps, and the long host calls.
constexpr std::size_t kTraceCapacity = 1U << 21;

struct TraceBuffer
{
    std::string path;
    std::unique_ptr<TraceRecord[]> records;
    std::atomic<std::size_t> next{0};
    std::atomic<std::size_t> overflow{0};
};

TraceBuffer* Buffer()
{
    static TraceBuffer* const buffer = []() -> TraceBuffer* {
        const char* const path = std::getenv("REPIU_TIMER_LATENCY_TRACE");
        if (path == nullptr || path[0] == '\0')
        {
            return nullptr;
        }
        auto* created = new TraceBuffer;
        created->path = path;
        created->records = std::make_unique<TraceRecord[]>(kTraceCapacity);
        return created;
    }();
    return buffer;
}

const char* EventName(std::uint8_t event)
{
    switch (static_cast<TimerTraceEvent>(event))
    {
        case TimerTraceEvent::kDue: return "due";
        case TimerTraceEvent::kInject: return "inject";
        case TimerTraceEvent::kDefer: return "defer";
        case TimerTraceEvent::kGate: return "gate";
        case TimerTraceEvent::kDos: return "dos";
        case TimerTraceEvent::kToggle: return "toggle";
        case TimerTraceEvent::kSeen: return "seen";
        case TimerTraceEvent::kSwap: return "swap";
        case TimerTraceEvent::kStatusRead: return "read";
    }
    return "unknown";
}

}  // namespace

bool TimerLatencyTraceEnabled()
{
    static const bool enabled = Buffer() != nullptr;
    return enabled;
}

std::uint64_t TimerTraceNowNanoseconds()
{
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

void RecordTimerTrace(TimerTraceEvent event, std::uint64_t time_ns,
                      std::uint32_t value, std::uint32_t extra,
                      std::uint64_t aux_ns)
{
    TraceBuffer* const buffer = Buffer();
    if (buffer == nullptr)
    {
        return;
    }
    const std::size_t index =
        buffer->next.fetch_add(1U, std::memory_order_relaxed);
    if (index >= kTraceCapacity)
    {
        buffer->overflow.fetch_add(1U, std::memory_order_relaxed);
        return;
    }
    buffer->records[index] = TraceRecord{
        time_ns, aux_ns, value, extra, static_cast<std::uint8_t>(event)};
}

void WriteTimerLatencyTrace()
{
    TraceBuffer* const buffer = Buffer();
    if (buffer == nullptr)
    {
        return;
    }
    std::FILE* const file = std::fopen(buffer->path.c_str(), "w");
    if (file == nullptr)
    {
        std::fprintf(stderr, "[repiu-timer-trace] cannot write %s\n",
                     buffer->path.c_str());
        return;
    }
    const std::size_t written = std::min(
        buffer->next.load(std::memory_order_relaxed), kTraceCapacity);
    std::fprintf(file, "# event time_ns value extra aux_ns\n");
    for (std::size_t index = 0; index < written; ++index)
    {
        const TraceRecord& record = buffer->records[index];
        std::fprintf(file, "%s %llu %u %u %llu\n", EventName(record.event),
                     static_cast<unsigned long long>(record.time_ns),
                     record.value, record.extra,
                     static_cast<unsigned long long>(record.aux_ns));
    }
    std::fclose(file);
    std::fprintf(stderr,
                 "[repiu-timer-trace] wrote %zu events to %s (overflow %zu)\n",
                 written, buffer->path.c_str(),
                 buffer->overflow.load(std::memory_order_relaxed));
}

}  // namespace repiu::engine
