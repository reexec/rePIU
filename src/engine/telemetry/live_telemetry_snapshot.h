#pragma once

// Live telemetry mapping and execution-snapshot helpers extracted from
// execution_trampoline.cpp (Phase 1 increment 4). BuildDosEnvironmentBlock,
// which was interleaved among these functions, stays in the trampoline.

#include "thread_context.h"
#include "native_phase_sampler.h"

#include "repiu/platform/guest_cpu_context.h"
#include "repiu/platform/host_telemetry.h"
#include "repiu/platform/host_thread.h"

#include <cstdint>
#include <vector>

// Task 503d-14. What is behind the shared mapping below is the cross-process
// diagnostics: a section another process maps. It is not needed to run the
// guest, so Linux starts without it rather than with an invented counterpart.
// Task 759. The section itself is the platform layer's (host_telemetry.h); on
// a host without one the mapping is empty and the telemetry pointer null.

namespace repiu::engine
{

// Task 503d-19. What the host poll loop concluded.
//
// It used to answer with Win32 wait codes -- WAIT_OBJECT_0, WAIT_TIMEOUT,
// WAIT_FAILED and WAIT_ABANDONED_0 standing in for "the window closed" -- which
// are constants belonging to a wait this loop does not perform. It polls, and
// pumps Glide commands and delivers timer ticks between the questions.
//
// Naming the four outcomes is the same move 3b made when it answered `readable`
// instead of a protection bitmask, and 3d-18 when it answered `running` instead
// of STILL_ACTIVE.
enum class HostPollOutcome
{
    // The guest thread stopped by itself; the exit code is filled in.
    kGuestThreadExited,
    // The execution budget or the stall budget ran out.
    kTimedOut,
    // The host window asked to close, which is not a failure.
    kHostExitRequested,
    // The loop could not do its job -- a thread it was not given, and nothing
    // else today.
    kFailed,
};

struct SharedTelemetryMapping
{
    repiu::platform::HostSharedMapping mapping;
    SharedLiveTelemetry* telemetry = nullptr;

    SharedTelemetryMapping() = default;
    SharedTelemetryMapping(const SharedTelemetryMapping&) = delete;
    SharedTelemetryMapping& operator=(const SharedTelemetryMapping&) = delete;
    SharedTelemetryMapping(SharedTelemetryMapping&& other) noexcept
        : mapping(other.mapping), telemetry(other.telemetry)
    {
        other.mapping = repiu::platform::HostSharedMapping{};
        other.telemetry = nullptr;
    }

    ~SharedTelemetryMapping()
    {
        repiu::platform::CloseHostSharedMapping(&mapping);
    }
};

// One line of live progress telemetry, with the elapsed time and the host
// loop's iteration. Issue #64: written from the host turn (host_turn.cpp).
void WriteLiveTelemetrySnapshot(const ThreadContext& context,
                                std::uint32_t elapsed_milliseconds,
                                std::uint32_t poll_iteration);

// Task 503d-18: the thread is the layer's handle, and the loop asks it whether
// the guest is still running rather than reading GetExitCodeThread and comparing
// against a sentinel that is also a legal exit code.
// Task 503d-19: milliseconds are milliseconds on both hosts, and the outcome is
// named rather than borrowed from a wait. It left the fence with them -- the
// host loop is what drives a run, so it has to exist on the host that is being
// brought up.
HostPollOutcome PollThreadUntilExit(const repiu::platform::HostThread& thread,
                                    std::uint32_t timeout_milliseconds,
                                    std::uint32_t stall_timeout_milliseconds,
                                    ThreadContext* progress_context,
                                    ThreadContext* host_context,
                                    std::uint32_t* exit_code,
                                    bool* stall_timed_out);

SharedTelemetryMapping OpenSharedTelemetryMapping();

// Task 503d-14: CONTEXT becomes GuestCpuContext, which is an alias for it on
// Windows, so neither the definition nor its callers change.
void CopySnapshotFromContextRecord(
    const repiu::platform::GuestCpuContext& source,
    X86ExecutionSnapshot* snapshot);

void CopyThreadObservationToAttempt(const ThreadContext& context,
                                    MinimalExecutionAttempt* attempt);

} // namespace repiu::engine
