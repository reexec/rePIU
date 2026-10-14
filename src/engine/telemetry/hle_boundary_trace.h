#pragma once

// Issue #61: one line per fault that reaches DispatchGuestFault, and one per
// injected timer interrupt, so the direct and emulated models' runs can be
// compared line by line.
//
// Diagnostic instrumentation only: it reads the fault and the guest bytes at
// EIP and writes them out; it never touches guest registers, guest memory or
// control flow.
//
// REPIU_HLE_BOUNDARY_TRACE names the output file; unset or empty is OFF.
// REPIU_HLE_BOUNDARY_TRACE_LIMIT caps the line count (default 1,000,000).
// Both models make the same dispatches until the first timer tick lands,
// because everything before it is deterministic; after it, where each tick
// falls depends on host timing.

#include <cstdint>

namespace repiu::platform
{
struct FaultEvent;
}

namespace repiu::engine
{

bool HleBoundaryTraceEnabled();

// A fault as DispatchGuestFault receives it, before any handler runs, with
// the exit the previous fault left the dispatcher by.
void RecordHleBoundaryFault(const repiu::platform::FaultEvent& fault,
                            const char* previous_exit);

// A timer interrupt frame built at `interrupted_eip`, entering `handler_eip`.
void RecordHleBoundaryInjection(std::uint32_t interrupted_eip, std::uint32_t handler_eip);

}  // namespace repiu::engine
