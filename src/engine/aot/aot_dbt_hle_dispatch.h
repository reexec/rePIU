#pragma once

#include "thread_context.h"

#include <cstdint>

namespace repiu::engine
{

enum class AotDbtHleFallbackReason : std::uint32_t
{
    kInvalidSite = 0,
    kVehRequired,
    kUnhandled,
    kTargetMiss,
    kStateMismatch,
    kUnknown,
};

void RecordAotDbtHleFallback(
    ThreadContext* context,
    AotDbtHleFallbackReason reason);

void* GetAotDbtHleDispatchThunkAddress();

// Issue #61. Whether the HLE boundary at `guest_eip` must be served by
// running its bytes into the fault handler rather than by direct dispatch
// (most INTs, IRET, far transfers, writes to a segment register or ESP), as
// the dynamic backend decides it. `eax` picks the INT 21h services that may
// be dispatched directly.
bool AotDbtHleRequiresVehMediation(ThreadContext* context, std::uint32_t guest_eip,
                                   std::uint32_t eax);

}  // namespace repiu::engine
