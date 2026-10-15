#include "live_telemetry_snapshot.h"
#include "boundary/timer_interrupt_boundary.h"
#include "execution/execution_internal.h"
#include "repiu/runtime/execution_timeout.h"
#include "repiu/engine/timer_latency_trace.h"

#include <mutex>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <sstream>

#include "repiu/platform/guest_cpu_context.h"
#include "repiu/platform/host_error_stream.h"
#include "repiu/platform/host_time.h"
#include "repiu/platform/virtual_memory.h"

namespace repiu::engine
{
namespace
{

// Task 715. A few guest dwords beside the elapsed time, once per poll.
//
// Some questions are about the guest's own state over wall time -- whether its
// timer-driven counters advance as fast on one host as on the other -- and the
// engine's counters cannot answer them. REPIU_LIVE_GUEST_PEEK names a runtime
// offset (relative to the runtime base, so one value means the same guest
// variable on every host) and optionally a dword count:
// `REPIU_LIVE_GUEST_PEEK=0x16FA0C,6`. The read is from the host thread and
// unsynchronized with the guest, which is what a counter sample needs.
void WriteLiveGuestPeek(const ThreadContext& context,
                        const std::uint32_t elapsed_milliseconds)
{
    struct PeekSetting
    {
        bool enabled = false;
        std::uint32_t offset = 0U;
        std::uint32_t count = 1U;
    };
    static const PeekSetting setting = [] {
        PeekSetting parsed;
        const char* const text = std::getenv("REPIU_LIVE_GUEST_PEEK");
        if (text == nullptr || *text == 0)
        {
            return parsed;
        }
        char* end = nullptr;
        const unsigned long offset = std::strtoul(text, &end, 0);
        if (end == text || offset > 0xFFFFFFFFUL)
        {
            return parsed;
        }
        parsed.offset = static_cast<std::uint32_t>(offset);
        if (*end == ',')
        {
            const unsigned long count = std::strtoul(end + 1, nullptr, 0);
            parsed.count = count == 0UL ? 1U
                : static_cast<std::uint32_t>(std::min(count, 16UL));
        }
        parsed.enabled = true;
        return parsed;
    }();
    if (!setting.enabled || context.runtime_base == 0U ||
        setting.offset > 0xFFFFFFFFU - context.runtime_base)
    {
        return;
    }
    const std::uint32_t address = context.runtime_base + setting.offset;
    const auto* const words = reinterpret_cast<const std::uint32_t*>(
        static_cast<std::uintptr_t>(address));
    if (!repiu::platform::IsRangeReadable(
            words, setting.count * sizeof(std::uint32_t)))
    {
        return;
    }
    char line[256] = {};
    int length = std::snprintf(
        line, sizeof(line),
        "[repiu-live-peek] elapsed_ms=%lu offset=0x%08X values=",
        static_cast<unsigned long>(elapsed_milliseconds), setting.offset);
    for (std::uint32_t index = 0U; index < setting.count && length > 0 &&
         static_cast<std::size_t>(length) + 12U < sizeof(line); ++index)
    {
        length += std::snprintf(line + length, sizeof(line) - length,
                                index == 0U ? "%08X" : " %08X",
                                words[index]);
    }
    if (length > 0 && static_cast<std::size_t>(length) + 1U < sizeof(line))
    {
        line[length++] = '\n';
        repiu::platform::WriteHostErrorStream(
            line, static_cast<std::size_t>(length));
    }
}

// Task 717. REPIU_LIVE_GUEST_SCAN=<runtime offset>,<bytes>,<dword> reports,
// beside each live line, where in the range the dword occurs: how many times,
// and the first few runs of consecutive occurrences. A fill that overwrites a
// structure shows up here as the run that covers it, and its ends say where the
// fill began and stopped, which a fixed peek cannot.
void WriteLiveGuestScan(const ThreadContext& context,
                        const std::uint32_t elapsed_milliseconds)
{
    struct ScanSetting
    {
        bool enabled = false;
        std::uint32_t offset = 0U;
        std::uint32_t bytes = 0U;
        std::uint32_t pattern = 0U;
    };
    static const ScanSetting setting = [] {
        ScanSetting parsed;
        const char* const text = std::getenv("REPIU_LIVE_GUEST_SCAN");
        if (text == nullptr || *text == 0)
        {
            return parsed;
        }
        char* end = nullptr;
        const unsigned long offset = std::strtoul(text, &end, 0);
        if (end == text || *end != ',')
        {
            return parsed;
        }
        char* bytes_end = nullptr;
        const unsigned long bytes = std::strtoul(end + 1, &bytes_end, 0);
        if (bytes_end == end + 1 || *bytes_end != ',')
        {
            return parsed;
        }
        parsed.offset = static_cast<std::uint32_t>(offset);
        parsed.bytes = static_cast<std::uint32_t>(bytes) & ~3U;
        parsed.pattern = static_cast<std::uint32_t>(
            std::strtoul(bytes_end + 1, nullptr, 0));
        parsed.enabled = parsed.bytes != 0U;
        return parsed;
    }();
    if (!setting.enabled || context.runtime_base == 0U ||
        setting.offset > 0xFFFFFFFFU - context.runtime_base ||
        setting.bytes > 0xFFFFFFFFU - context.runtime_base - setting.offset)
    {
        return;
    }
    const std::uint32_t address = context.runtime_base + setting.offset;
    const auto* const words = reinterpret_cast<const std::uint32_t*>(
        static_cast<std::uintptr_t>(address));
    if (!repiu::platform::IsRangeReadable(words, setting.bytes))
    {
        return;
    }
    constexpr std::uint32_t kMaxRuns = 6U;
    std::uint32_t run_start[kMaxRuns] = {};
    std::uint32_t run_end[kMaxRuns] = {};
    std::uint32_t runs = 0U;
    std::uint32_t matches = 0U;
    bool in_run = false;
    const std::uint32_t count = setting.bytes / 4U;
    for (std::uint32_t index = 0U; index < count; ++index)
    {
        const bool hit = words[index] == setting.pattern;
        if (hit)
        {
            ++matches;
            if (!in_run)
            {
                if (runs < kMaxRuns)
                {
                    run_start[runs] = setting.offset + index * 4U;
                }
                in_run = true;
            }
            if (runs < kMaxRuns)
            {
                run_end[runs] = setting.offset + index * 4U + 4U;
            }
        }
        else if (in_run)
        {
            in_run = false;
            ++runs;
        }
    }
    if (in_run)
    {
        ++runs;
    }
    char line[512] = {};
    int length = std::snprintf(
        line, sizeof(line),
        "[repiu-live-scan] elapsed_ms=%lu pattern=0x%08X matches=%u runs=%u",
        static_cast<unsigned long>(elapsed_milliseconds), setting.pattern,
        matches, runs);
    for (std::uint32_t index = 0U; index < std::min(runs, kMaxRuns) &&
         length > 0 && static_cast<std::size_t>(length) + 40U < sizeof(line);
         ++index)
    {
        length += std::snprintf(line + length, sizeof(line) - length,
                                " 0x%08X-0x%08X", run_start[index],
                                run_end[index]);
    }
    if (length > 0 && static_cast<std::size_t>(length) + 1U < sizeof(line))
    {
        line[length++] = '\n';
        repiu::platform::WriteHostErrorStream(
            line, static_cast<std::size_t>(length));
    }
}

}  // namespace

// Task 503d-16. The .cpp takes the boundary the header already drew in
// Task 503d-16: not fenced. What it formats is the engine's own progress, and
// the two counters are milliseconds and an iteration number rather than
// anything Windows owns -- so they are spelled by what they mean. The format
// already casts them at the call, which is why `%lu` still reads correctly.
void WriteLiveTelemetrySnapshot(const ThreadContext& context,
                                std::uint32_t elapsed_milliseconds,
                                std::uint32_t poll_iteration)
{
    char buffer[768] = {};
    const int length = std::snprintf(
        buffer,
        sizeof(buffer),
        "[repiu-live] elapsed_ms=%lu poll=%lu phase=%u heartbeat=%u "
        "dispatch_entry=%u dispatch_exit=%u last_eip=0x%08X "
        "progress=%u single_step=%u aot=%u/%u fast=%u/%u/%u "
        "routea=%u/%u "
        "span=%u/%u/%u/%u/%u "
        "span_reject_cache=%u/%u/%u/%u/%u span_write=%u "
        "span_cancel_last=0x%08X/0x%08X "
        "selguard=%u/%u/%u/%u/%u "
        "posthle=%u/%u "
        "prov=%u/%u/%u/%u/%u/%u/%u/%u "
        "reject=0x%08X:0x%08X/0x%02X bytes=%08X%08X\r\n",
        static_cast<unsigned long>(elapsed_milliseconds),
        static_cast<unsigned long>(poll_iteration),
        context.live_telemetry_phase.load(std::memory_order_relaxed),
        context.live_telemetry_heartbeat.load(std::memory_order_relaxed),
        context.exception_dispatch_entry_count.load(
            std::memory_order_relaxed),
        context.exception_dispatch_exit_count.load(
            std::memory_order_relaxed),
        context.exception_dispatch_last_eip.load(std::memory_order_relaxed),
        context.diagnostic_progress_count.load(std::memory_order_relaxed),
        context.single_step_trace_count.load(std::memory_order_relaxed),
        context.aot_boundary_count.load(std::memory_order_relaxed),
        context.aot_reentry_count.load(std::memory_order_relaxed),
        context.native_fast_path.entry_count.load(std::memory_order_relaxed),
        context.native_fast_path.return_count.load(std::memory_order_relaxed),
        context.native_fast_path.cancel_count.load(std::memory_order_relaxed),
        context.routea_sensitive_count.load(std::memory_order_relaxed),
        context.routea_segment_sensitive_count.load(
            std::memory_order_relaxed),
        context.native_fast_path.linear_span_entry_count.load(
            std::memory_order_relaxed),
        context.native_fast_path.linear_span_boundary_count.load(
            std::memory_order_relaxed),
        context.native_fast_path.linear_span_cancel_count.load(
            std::memory_order_relaxed),
        context.native_fast_path.linear_span_instruction_total.load(
            std::memory_order_relaxed),
        context.native_fast_path.linear_span_reject_count.load(
            std::memory_order_relaxed),
        context.native_fast_path.linear_span_reject_cache_hit_count.load(
            std::memory_order_relaxed),
        context.native_fast_path.linear_span_reject_cache_miss_count.load(
            std::memory_order_relaxed),
        context.native_fast_path.linear_span_reject_cache_stale_count.load(
            std::memory_order_relaxed),
        context.native_fast_path.linear_span_reject_cache_store_count.load(
            std::memory_order_relaxed),
        context.native_fast_path
            .linear_span_reject_cache_capacity_skip_count.load(
                std::memory_order_relaxed),
        context.native_fast_path.linear_span_write_fault_cancel_count.load(
            std::memory_order_relaxed),
        context.native_fast_path.linear_span_last_cancel_code.load(
            std::memory_order_relaxed),
        context.native_fast_path.linear_span_last_cancel_eip.load(
            std::memory_order_relaxed),
        context.aot_selector_guard_native_site_count.load(
            std::memory_order_relaxed),
        context.aot_selector_guard_hle_site_count.load(
            std::memory_order_relaxed),
        context.aot_selector_guard_unresolved_site_count.load(
            std::memory_order_relaxed),
        context.aot_selector_guard_hle_exit_count.load(
            std::memory_order_relaxed),
        context.aot_selector_guard_mismatch_count.load(
            std::memory_order_relaxed),
        context.aot_dbt_hle_translation_attempt_count.load(
            std::memory_order_relaxed),
        context.aot_dbt_hle_translation_success_count.load(
            std::memory_order_relaxed),
        context.aot_breakpoint_provenance_counts[0].load(
            std::memory_order_relaxed),
        context.aot_breakpoint_provenance_counts[1].load(
            std::memory_order_relaxed),
        context.aot_breakpoint_provenance_counts[2].load(
            std::memory_order_relaxed),
        context.aot_breakpoint_provenance_counts[3].load(
            std::memory_order_relaxed),
        context.aot_breakpoint_provenance_counts[4].load(
            std::memory_order_relaxed),
        context.aot_breakpoint_provenance_counts[5].load(
            std::memory_order_relaxed),
        context.aot_breakpoint_provenance_counts[6].load(
            std::memory_order_relaxed),
        context.aot_breakpoint_provenance_counts[7].load(
            std::memory_order_relaxed),
        context.native_fast_path.last_rejected_candidate.load(
            std::memory_order_relaxed),
        context.native_fast_path.last_rejected_instruction.load(
            std::memory_order_relaxed),
        context.native_fast_path.last_rejected_opcode.load(
            std::memory_order_relaxed),
        context.native_fast_path.last_rejected_bytes_high.load(
            std::memory_order_relaxed),
        context.native_fast_path.last_rejected_bytes_low.load(
            std::memory_order_relaxed));
    if (length <= 0)
    {
        return;
    }

    // Task 503d-16: 3d-14 built this for exactly this shape -- one buffer, one
    // unbuffered write, no stdio lock. This caller runs on the host thread
    // beside a guest thread it can suspend, which is the reason that mattered.
    const std::size_t byte_count =
        static_cast<std::size_t>(length) < sizeof(buffer)
            ? static_cast<std::size_t>(length)
            : sizeof(buffer) - 1U;
    repiu::platform::WriteHostErrorStream(buffer, byte_count);
    WriteLiveGuestPeek(context, elapsed_milliseconds);
    WriteLiveGuestScan(context, elapsed_milliseconds);
}

// 3d-14: the section another process maps. Task 759: the platform layer opens
// and maps it; what the engine checks is that it is the telemetry it expects.
SharedTelemetryMapping OpenSharedTelemetryMapping()
{
    SharedTelemetryMapping result;
    result.mapping = repiu::platform::OpenHostSharedMappingFromEnvironment(
        kLiveTelemetryEnvironment);
    result.telemetry =
        static_cast<SharedLiveTelemetry*>(result.mapping.view);
    if (result.telemetry != nullptr &&
        (result.telemetry->magic != kLiveTelemetryMagic ||
         result.telemetry->version != kLiveTelemetryVersion))
    {
        result.telemetry = nullptr;
        repiu::platform::CloseHostSharedMapping(&result.mapping);
    }
    return result;
}

void CopySnapshotFromContextRecord(const repiu::platform::GuestCpuContext& source,
                                   X86ExecutionSnapshot* snapshot);

// Nothing calls this today (Task 759 found no caller). `thread` is the host's
// thread handle.
void CaptureSuspendedThreadSnapshot(void* thread,
                                    X86ExecutionSnapshot* snapshot)
{
    if (thread == nullptr || snapshot == nullptr)
    {
        return;
    }
    repiu::platform::GuestCpuContext thread_context = {};
    if (repiu::platform::ReadSuspendedHostThreadContext(thread,
                                                        &thread_context))
    {
        CopySnapshotFromContextRecord(thread_context, snapshot);
    }
}

void CopySnapshotFromContextRecord(const repiu::platform::GuestCpuContext& source,
                                   X86ExecutionSnapshot* snapshot)
{
    if (snapshot == nullptr)
    {
        return;
    }

// Task 507 and Task 613: the question is whether this is the fixed-width guest
// context, not whether the host is a 32-bit process. Linux x64 carries the
// same guest fields in its platform context and must copy them as well; only a
// native Windows x64 CONTEXT lacks these Eip/Eax-style fields.
    snapshot->captured = true;
    snapshot->eip = source.Eip;
    snapshot->eax = source.Eax;
    snapshot->ebx = source.Ebx;
    snapshot->ecx = source.Ecx;
    snapshot->edx = source.Edx;
    snapshot->esi = source.Esi;
    snapshot->edi = source.Edi;
    snapshot->esp = source.Esp;
    snapshot->ebp = source.Ebp;
    snapshot->eflags = source.EFlags;
    snapshot->cs = static_cast<std::uint16_t>(source.SegCs);
    snapshot->ds = static_cast<std::uint16_t>(source.SegDs);
    snapshot->es = static_cast<std::uint16_t>(source.SegEs);
    snapshot->ss = static_cast<std::uint16_t>(source.SegSs);
    snapshot->fs = static_cast<std::uint16_t>(source.SegFs);
    snapshot->gs = static_cast<std::uint16_t>(source.SegGs);
}


bool BuildSingleStepSnapshot(const ThreadContext& context,
                             X86ExecutionSnapshot* snapshot)
{
    if (snapshot == nullptr)
    {
        return false;
    }

    const std::uint32_t count =
        context.single_step_trace_count.load(std::memory_order_relaxed);
    if (count == 0)
    {
        *snapshot = X86ExecutionSnapshot{};
        return false;
    }

    snapshot->captured = true;
    snapshot->eip =
        context.single_step_eip.load(std::memory_order_relaxed);
    snapshot->eax =
        context.single_step_eax.load(std::memory_order_relaxed);
    snapshot->ebx =
        context.single_step_ebx.load(std::memory_order_relaxed);
    snapshot->ecx =
        context.single_step_ecx.load(std::memory_order_relaxed);
    snapshot->edx =
        context.single_step_edx.load(std::memory_order_relaxed);
    snapshot->esi =
        context.single_step_esi.load(std::memory_order_relaxed);
    snapshot->edi =
        context.single_step_edi.load(std::memory_order_relaxed);
    snapshot->esp =
        context.single_step_esp.load(std::memory_order_relaxed);
    snapshot->ebp =
        context.single_step_ebp.load(std::memory_order_relaxed);
    snapshot->eflags =
        context.single_step_eflags.load(std::memory_order_relaxed);
    snapshot->cs = static_cast<std::uint16_t>(
        context.single_step_cs.load(std::memory_order_relaxed));
    snapshot->ds = static_cast<std::uint16_t>(
        context.single_step_ds.load(std::memory_order_relaxed));
    snapshot->es = static_cast<std::uint16_t>(
        context.single_step_es.load(std::memory_order_relaxed));
    snapshot->ss = static_cast<std::uint16_t>(
        context.single_step_ss.load(std::memory_order_relaxed));
    snapshot->fs = static_cast<std::uint16_t>(
        context.single_step_fs.load(std::memory_order_relaxed));
    snapshot->gs = static_cast<std::uint16_t>(
        context.single_step_gs.load(std::memory_order_relaxed));
    return true;
}

void CopyThreadObservationToAttempt(const ThreadContext& context,
                                    MinimalExecutionAttempt* attempt)
{
    if (attempt == nullptr)
    {
        return;
    }

    attempt->dos_termination_captured = context.dos_termination_captured;
    attempt->dos_termination_ax = context.dos_termination_ax;
    attempt->dos_termination_eip = context.dos_termination_eip;
    attempt->dos_termination_esp = context.dos_termination_esp;
    std::memcpy(attempt->dos_termination_stack,
                context.dos_termination_stack,
                sizeof(attempt->dos_termination_stack));

    attempt->single_step_trace_count =
        context.single_step_trace_count.load(std::memory_order_relaxed);
    // Task 337: read after the guest thread has stopped, so plain counters.
    attempt->veh_single_step_exception_count =
        context.veh_single_step_exception_count;
    attempt->veh_breakpoint_exception_count =
        context.veh_breakpoint_exception_count;
    attempt->veh_access_violation_exception_count =
        context.veh_access_violation_exception_count;
    attempt->veh_other_exception_count = context.veh_other_exception_count;
    attempt->veh_other_exception_code_overflow =
        context.veh_other_exception_code_overflow;
    for (std::uint32_t index = 0;
         index < ThreadContext::kOtherExceptionCodeCapacity; ++index)
    {
        attempt->veh_other_exception_codes[index] =
            context.veh_other_exception_codes[index];
        attempt->veh_other_exception_code_counts[index] =
            context.veh_other_exception_code_counts[index];
    }
    attempt->veh_single_step_run_total = context.veh_single_step_run_total;
    attempt->veh_single_step_run_max = context.veh_single_step_run_max;
    std::memcpy(attempt->veh_single_step_run_buckets,
                context.veh_single_step_run_buckets,
                sizeof(attempt->veh_single_step_run_buckets));
    attempt->hle_reentry_reject_not_pending =
        context.hle_reentry_reject_not_pending;
    attempt->hle_reentry_reject_segment_write =
        context.hle_reentry_reject_segment_write;
    attempt->hle_reentry_reject_outside_arena =
        context.hle_reentry_reject_outside_arena;
    attempt->hle_reentry_reject_quarantined =
        context.hle_reentry_reject_quarantined;
    attempt->hle_reentry_reject_cache_miss =
        context.hle_reentry_reject_cache_miss;
    attempt->hle_reentry_reject_span_unsafe =
        context.hle_reentry_reject_span_unsafe;
    attempt->hle_reentry_success = context.hle_reentry_success;
    attempt->hle_reentry_segment_write_resumed =
        context.hle_reentry_segment_write_resumed;
    attempt->quarantine_trace_count = context.quarantine_trace_count;
    attempt->quarantine_unknown_source_count =
        context.quarantine_unknown_source_count;
    attempt->quarantine_deferred_count = context.quarantine_deferred_count;
    attempt->guest_page_write_history_overflow =
        context.guest_page_write_history_overflow;
    for (std::uint32_t index = 0;
         index < ThreadContext::kQuarantineTraceCapacity; ++index)
    {
        attempt->quarantine_trace[index] = {
            context.quarantine_trace[index].page,
            context.quarantine_trace[index].source,
            context.quarantine_trace[index].destination,
            context.quarantine_trace[index].byte_count};
    }
    static_assert(
        sizeof(attempt->generation_failure_trace) /
            sizeof(attempt->generation_failure_trace[0]) ==
            ThreadContext::kGenerationFailureTraceCapacity,
        "generation failure trace capacities must match");
    static_assert(
        sizeof(attempt->generation_failure_trace[0].message) ==
            ThreadContext::kGenerationFailureMessageCapacity,
        "generation failure message capacities must match");
    attempt->generation_failure_trace_count =
        context.generation_failure_trace_count;
    attempt->generation_failure_trace_overflow =
        context.generation_failure_trace_overflow;
    for (std::uint32_t index = 0;
         index < ThreadContext::kGenerationFailureTraceCapacity; ++index)
    {
        const auto& source = context.generation_failure_trace[index];
        auto& destination = attempt->generation_failure_trace[index];
        destination.target = source.target;
        destination.page = source.page;
        destination.quarantined = source.quarantined;
        destination.terminal = source.terminal;
        std::memcpy(destination.message, source.message,
                    sizeof(destination.message));
    }
    // Task 741: the reentry memo counters.
    for (std::uint32_t kind = 0; kind < 3U; ++kind)
    {
        attempt->aot_reentry_memo_hits[kind] =
            context.aot_reentry_memo_hits[kind];
        attempt->aot_reentry_memo_misses[kind] =
            context.aot_reentry_memo_misses[kind];
    }
    attempt->aot_reentry_memo_generation =
        context.aot_reentry_memo_generation;
    // Task 741: the 32 busiest of the hashed table, in count order.
    static_assert(
        sizeof(attempt->breakpoint_site_census) /
            sizeof(attempt->breakpoint_site_census[0]) ==
            ThreadContext::kBreakpointSiteReportCapacity,
        "breakpoint site census report capacities must match");
    std::vector<std::uint32_t> breakpoint_site_order;
    for (std::uint32_t index = 0;
         index < ThreadContext::kBreakpointSiteCensusCapacity; ++index)
    {
        if (context.breakpoint_site_census[index].count != 0U)
        {
            breakpoint_site_order.push_back(index);
        }
    }
    std::sort(breakpoint_site_order.begin(), breakpoint_site_order.end(),
              [&context](std::uint32_t left, std::uint32_t right) {
                  return context.breakpoint_site_census[left].count >
                      context.breakpoint_site_census[right].count;
              });
    attempt->breakpoint_site_census_size = std::min<std::uint32_t>(
        static_cast<std::uint32_t>(breakpoint_site_order.size()),
        ThreadContext::kBreakpointSiteReportCapacity);
    attempt->breakpoint_site_census_distinct =
        context.breakpoint_site_census_size;
    attempt->breakpoint_site_census_overflow =
        context.breakpoint_site_census_overflow;
    for (std::uint32_t rank = 0;
         rank < attempt->breakpoint_site_census_size; ++rank)
    {
        const auto& source =
            context.breakpoint_site_census[breakpoint_site_order[rank]];
        auto& destination = attempt->breakpoint_site_census[rank];
        destination.host_address = source.host_address;
        destination.guest_address = source.guest_address;
        destination.in_cache = source.in_cache;
        destination.guest_mapped = source.guest_mapped;
        destination.count = source.count;
        for (std::uint32_t slot = 0;
             slot < ThreadContext::kBreakpointSiteExitCapacity; ++slot)
        {
            destination.exit_sites[slot] = source.exit_sites[slot];
            destination.exit_counts[slot] = source.exit_counts[slot];
        }
        destination.exit_overflow = source.exit_overflow;
    }
    static_assert(
        sizeof(attempt->port_io_address_census) /
            sizeof(attempt->port_io_address_census[0]) ==
            ThreadContext::kPortIoAddressCensusCapacity,
        "port I/O address census capacities must match");
    attempt->port_io_address_census_size =
        context.port_io_address_census_size;
    attempt->port_io_address_census_overflow =
        context.port_io_address_census_overflow;
    for (std::uint32_t index = 0;
         index < ThreadContext::kPortIoAddressCensusCapacity; ++index)
    {
        attempt->port_io_address_census[index] = {
            context.port_io_address_census[index].guest_address,
            context.port_io_address_census[index].count,
            context.port_io_address_census[index].cache_count,
            context.port_io_address_census[index].mapped_count,
            context.port_io_address_census[index].reentry_pending_count,
            context.port_io_address_census[index].entry_transition_count,
            context.port_io_address_census[index].entry_previous_code,
            context.port_io_address_census[index].entry_previous_eip,
            context.port_io_address_census[index].entry_flags,
            context.port_io_address_census[index].entry_prev_single_step,
            context.port_io_address_census[index].entry_prev_breakpoint,
            context.port_io_address_census[index].entry_prev_access_violation,
            context.port_io_address_census[index].entry_prev_other,
            context.port_io_address_census[index].entry_previous_exit_site,
            context.port_io_address_census[index].entry_previous_exit_eip};
    }
    static_assert(
        sizeof(attempt->arena_port_io_entry_trace) /
            sizeof(attempt->arena_port_io_entry_trace[0]) ==
            ThreadContext::kArenaPortIoEntryTraceCapacity,
        "arena port I/O entry trace capacities must match");
    attempt->arena_port_io_entry_trace_count =
        context.arena_port_io_entry_trace_count;
    for (std::uint32_t index = 0;
         index < ThreadContext::kArenaPortIoEntryTraceCapacity; ++index)
    {
        const auto& source = context.arena_port_io_entry_trace[index];
        attempt->arena_port_io_entry_trace[index] = {
            source.guest_address, source.previous_code, source.previous_eip,
            source.previous_in_cache, source.trap_flag, source.reentry_pending,
            source.legacy_fallback, source.single_step_trace};
    }
    // Task 410: the exit-site histogram. The snapshot array is sized generously
    // and asserted against the enumeration, so adding a site cannot silently
    // truncate the report.
    static_assert(
        kVehExitSiteCount <=
            MinimalExecutionAttempt::kVehExitSiteSnapshotCapacity,
        "VEH exit site snapshot capacity must cover the enumeration");
    attempt->veh_arena_single_step_count =
        context.veh_arena_single_step_count;
    for (std::uint32_t site = 0; site < kVehExitSiteCount; ++site)
    {
        attempt->veh_arena_single_step_exit_site_counts[site] =
            context.veh_arena_single_step_exit_site_counts[site];
    }
    attempt->single_step_hotspot_profile =
        context.single_step_hotspot_profile != nullptr
            ? SnapshotSingleStepHotspotProfile(
                  *context.single_step_hotspot_profile)
            : SingleStepHotspotProfileSnapshot{};
    // Task 400: this runs exactly once per attempt, on both the interrupted and
    // the normal teardown path, so the full-table dump belongs here rather than
    // inside the snapshot function.
    if (context.single_step_hotspot_profile != nullptr)
    {
        attempt->single_step_hotspot_profile.dump_written =
            WriteSingleStepHotspotDumpIfEnabled(
                context.single_step_hotspot_profile.get(),
                &attempt->single_step_hotspot_profile.dump_entry_count,
                &attempt->single_step_hotspot_profile.dump_path);
    }
    // Task 411: same placement and the same reason as the hotspot dump above --
    // this runs once per attempt on both teardown paths, and writing the file
    // here keeps it ahead of a stalled Glide close.
    attempt->guest_position_census =
        context.guest_position_census != nullptr
            ? SnapshotGuestPositionCensus(*context.guest_position_census)
            : GuestPositionCensusSnapshot{};
    if (context.guest_position_census != nullptr)
    {
        attempt->guest_position_census.dump_written =
            WriteGuestPositionCensusDumpIfEnabled(
                context.guest_position_census.get(),
                &attempt->guest_position_census.dump_entry_count,
                &attempt->guest_position_census.dump_path);
        // Task 412: module and symbol lookup runs here, after the guest thread
        // has stopped, so nothing on the sampling path pays for it.
        ResolveGuestPositionCensusSymbols(&attempt->guest_position_census);
    }
    // Task 421: written here for the same reason -- ahead of a Glide close that
    // may stall, so the series survives a teardown that does not finish.
    if (context.cd_audio_position_census != nullptr)
    {
        WriteCdAudioPositionCensusDump(
            *context.cd_audio_position_census,
            &attempt->cd_audio_position_dump_entry_count,
            &attempt->cd_audio_position_regression_count);
    }
    // Task 422: same placement, same reason.
    if (context.mscdex_command_trace != nullptr)
    {
        WriteMscdexCommandTraceDump(
            *context.mscdex_command_trace,
            &attempt->mscdex_command_trace_entry_count);
        attempt->mscdex_command_trace_total =
            context.mscdex_command_trace->total_commands;
    }
    attempt->execution_time_profile =
        context.execution_time_profile != nullptr
            ? SnapshotExecutionTimeProfile(*context.execution_time_profile)
            : ExecutionTimeProfileSnapshot{};
    attempt->aot_worker_timing =
        context.aot_worker_timing != nullptr
            ? SnapshotAotWorkerTiming(*context.aot_worker_timing)
            : AotWorkerTimingSnapshot{};
    attempt->aot_return_stage_profile =
        SnapshotAotReturnStageProfile(context.aot_return_stage_profile);
    // Task 333: read after the guest thread has stopped, so the backend's
    // counters are quiescent and no lock is needed here.
    attempt->glide_gate_timing = context.glide_backend.glide_gate_timing();
    attempt->glide_rendezvous_spin =
        context.glide_backend.rendezvous_spin_counts();
    attempt->glide_ordinal_timing =
        SnapshotGlideOrdinalTiming(context.glide_ordinal_timing);
    attempt->glide_buffer_swap_timing =
        context.glide_backend.glide_buffer_swap_timing();
    attempt->glide_lfb_timing =
        SnapshotGlideLfbTiming(context.glide_lfb_timing);
    attempt->glide_lfb_write_footprint =
        SnapshotGlideLfbWriteFootprint(context.glide_lfb_write_footprint);
    attempt->glide_lfb_native_store_census =
        SnapshotGlideLfbNativeStoreCensus(context.glide_lfb_native_store_census);
    attempt->glide_lfb_staging_shadow =
        SnapshotGlideLfbStagingShadow(context.glide_lfb_staging_shadow);
    attempt->glide_lfb_lock_interval =
        SnapshotGlideLfbLockIntervalCensus(
            context.glide_lfb_lock_interval_census);
    attempt->glide_setter_census =
        SnapshotGlideSetterCensus(context.glide_setter_census);
    attempt->glide_setter_phase_timing =
        context.glide_backend.glide_setter_phase_timing();
    attempt->glide_setter_state_cache =
        SnapshotGlideSetterStateCache(context.glide_setter_state_cache);
    attempt->glide_draw_batch =
        SnapshotGlideDrawBatch(context.glide_draw_batch);
    attempt->glide_async_present =
        context.glide_backend.glide_async_present();
    attempt->glide_gl_error_policy =
        context.glide_backend.glide_gl_error_policy();
    attempt->glide_swap_interval_policy =
        context.glide_backend.glide_swap_interval_policy();
    attempt->glide_texture_census =
        context.glide_backend.glide_texture_census();
    attempt->out_of_arena_step_census =
        SnapshotOutOfArenaStepCensus(context.out_of_arena_step_census);
    attempt->timer_tick_delivery =
        SnapshotTimerTickDelivery(context.timer_tick_delivery);
    attempt->piu10_mp3_stats = context.piu10_mp3_audio.stats();
    attempt->piu10_song_clock = context.piu10_song_clock.Summary();
    attempt->pic_timer_in_service = context.pic_timer_in_service;
    attempt->timer_return_pad = context.timer_return_pad;
    attempt->native_fast_path_entry_count =
        context.native_fast_path.entry_count.load(std::memory_order_relaxed);
    attempt->native_fast_path_return_count =
        context.native_fast_path.return_count.load(std::memory_order_relaxed);
    attempt->native_fast_path_cancel_count =
        context.native_fast_path.cancel_count.load(std::memory_order_relaxed);
    attempt->native_fast_path_last_entry =
        context.native_fast_path.last_entry;
    attempt->native_fast_path_last_return =
        context.native_fast_path.last_return;
    attempt->native_linear_span_entry_count =
        context.native_fast_path.linear_span_entry_count.load(
            std::memory_order_relaxed);
    attempt->native_linear_span_boundary_count =
        context.native_fast_path.linear_span_boundary_count.load(
            std::memory_order_relaxed);
    attempt->native_linear_span_cancel_count =
        context.native_fast_path.linear_span_cancel_count.load(
            std::memory_order_relaxed);
    attempt->native_linear_span_cancel_tf_count =
        context.native_fast_path.linear_span_cancel_tf_count.load(
            std::memory_order_relaxed);
    attempt->native_linear_span_cancel_dr0_count =
        context.native_fast_path.linear_span_cancel_dr0_count.load(
            std::memory_order_relaxed);
    attempt->native_linear_span_cancel_dr1_count =
        context.native_fast_path.linear_span_cancel_dr1_count.load(
            std::memory_order_relaxed);
    attempt->native_linear_span_cancel_dr2_count =
        context.native_fast_path.linear_span_cancel_dr2_count.load(
            std::memory_order_relaxed);
    attempt->native_linear_span_cancel_dr3_count =
        context.native_fast_path.linear_span_cancel_dr3_count.load(
            std::memory_order_relaxed);
    attempt->native_linear_span_cancel_other_db_count =
        context.native_fast_path.linear_span_cancel_other_db_count.load(
            std::memory_order_relaxed);
    attempt->native_linear_span_cancel_tf_first_eip =
        context.native_fast_path.linear_span_cancel_tf_first_eip.load(
            std::memory_order_relaxed);
    attempt->native_linear_span_cancel_dr0_first_eip =
        context.native_fast_path.linear_span_cancel_dr0_first_eip.load(
            std::memory_order_relaxed);
    attempt->native_linear_span_cancel_dr1_first_eip =
        context.native_fast_path.linear_span_cancel_dr1_first_eip.load(
            std::memory_order_relaxed);
    attempt->native_linear_span_cancel_dr2_first_eip =
        context.native_fast_path.linear_span_cancel_dr2_first_eip.load(
            std::memory_order_relaxed);
    attempt->native_linear_span_cancel_dr3_first_eip =
        context.native_fast_path.linear_span_cancel_dr3_first_eip.load(
            std::memory_order_relaxed);
    attempt->native_linear_span_cancel_other_db_first_eip =
        context.native_fast_path.linear_span_cancel_other_db_first_eip.load(
            std::memory_order_relaxed);
    attempt->native_linear_span_instruction_total =
        context.native_fast_path.linear_span_instruction_total.load(
            std::memory_order_relaxed);
    attempt->native_linear_span_reject_count =
        context.native_fast_path.linear_span_reject_count.load(
            std::memory_order_relaxed);
    attempt->native_linear_span_reject_cache_hit_count =
        context.native_fast_path.linear_span_reject_cache_hit_count.load(
            std::memory_order_relaxed);
    attempt->native_linear_span_reject_cache_miss_count =
        context.native_fast_path.linear_span_reject_cache_miss_count.load(
            std::memory_order_relaxed);
    attempt->native_linear_span_reject_cache_stale_count =
        context.native_fast_path.linear_span_reject_cache_stale_count.load(
            std::memory_order_relaxed);
    attempt->native_linear_span_reject_cache_store_count =
        context.native_fast_path.linear_span_reject_cache_store_count.load(
            std::memory_order_relaxed);
    attempt->native_linear_span_reject_cache_capacity_skip_count =
        context.native_fast_path
            .linear_span_reject_cache_capacity_skip_count.load(
                std::memory_order_relaxed);
    attempt->native_linear_span_write_fault_cancel_count =
        context.native_fast_path.linear_span_write_fault_cancel_count.load(
            std::memory_order_relaxed);
    attempt->native_linear_span_last_cancel_code =
        context.native_fast_path.linear_span_last_cancel_code.load(
            std::memory_order_relaxed);
    attempt->native_linear_span_last_cancel_eip =
        context.native_fast_path.linear_span_last_cancel_eip.load(
            std::memory_order_relaxed);
    attempt->execution_backend = context.execution_backend;
    attempt->aot_backend_active = context.aot_placement != nullptr;
    attempt->aot_cache_entry_count = context.aot_cache_entry_count.load(
        std::memory_order_relaxed);
    attempt->aot_inline_cache_direct_patch_count =
        context.aot_inline_cache_direct_patch_count;
    attempt->aot_boundary_count = context.aot_boundary_count.load(
        std::memory_order_relaxed);
    attempt->aot_boundary_return_count =
        context.aot_boundary_return_count.load(std::memory_order_relaxed);
    attempt->aot_boundary_indirect_count =
        context.aot_boundary_indirect_count.load(std::memory_order_relaxed);
    attempt->aot_boundary_direct_count =
        context.aot_boundary_direct_count.load(std::memory_order_relaxed);
    attempt->aot_boundary_conditional_count =
        context.aot_boundary_conditional_count.load(std::memory_order_relaxed);
    attempt->aot_boundary_other_count =
        context.aot_boundary_other_count.load(std::memory_order_relaxed);
    for (std::uint32_t index = 0;
         index < kAotCacheBreakpointProvenanceCount; ++index)
    {
        attempt->aot_breakpoint_provenance_counts[index] =
            context.aot_breakpoint_provenance_counts[index].load(
                std::memory_order_relaxed);
    }
    // Task 263(a): top-8 lead opcodes of the `other` boundary bucket.
    {
        std::uint32_t histogram[256];
        for (int i = 0; i < 256; ++i)
        {
            histogram[i] = context.aot_other_opcode_histogram[i];
        }
        for (int slot = 0; slot < 8; ++slot)
        {
            int best = 0;
            for (int i = 1; i < 256; ++i)
            {
                if (histogram[i] > histogram[best])
                {
                    best = i;
                }
            }
            attempt->aot_other_top_opcodes[slot] =
                static_cast<std::uint32_t>(best);
            attempt->aot_other_top_counts[slot] = histogram[best];
            histogram[best] = 0;
        }
    }
    // Task 367: the same samples ranked by real instruction rather than by lead
    // byte. Sorting happens here, at exit, never on the hot path.
    {
        const AotBoundaryOpcodeCensus& census =
            context.aot_boundary_opcode_census;
        RankAotOpcodeHistogram(
            census.effective_opcode_counts,
            attempt->aot_effective_opcode_ranks,
            MinimalExecutionAttempt::kAotOpcodeRankCount);
        RankAotOpcodeHistogram(
            census.escape_opcode_counts,
            attempt->aot_escape_opcode_ranks,
            MinimalExecutionAttempt::kAotOpcodeRankCount);
        attempt->aot_opcode_census_samples = census.sample_count;
        attempt->aot_opcode_census_escapes = census.escape_count;
        attempt->aot_opcode_census_prefixed = census.prefixed_count;
        attempt->aot_opcode_census_segment_prefixed =
            census.segment_prefixed_count;
        attempt->aot_opcode_census_operand_size_prefixed =
            census.operand_size_prefixed_count;
        attempt->aot_opcode_census_truncated = census.escape_truncated_count;
        attempt->aot_opcode_census_prefix_overflow =
            census.prefix_overflow_count;
        attempt->aot_opcode_census_empty = census.empty_sample_count;
    }
    attempt->aot_last_other_eip =
        context.aot_last_other_boundary_eip.load(std::memory_order_relaxed);
    attempt->aot_last_other_bytes =
        context.aot_last_other_boundary_bytes.load(std::memory_order_relaxed);
    attempt->aot_residency_total =
        context.aot_residency_instruction_total.load(std::memory_order_relaxed);
    attempt->aot_residency_samples =
        context.aot_residency_sample_count.load(std::memory_order_relaxed);
    attempt->aot_residency_max =
        context.aot_residency_max.load(std::memory_order_relaxed);
    attempt->aot_reentry_count = context.aot_reentry_count.load(
        std::memory_order_relaxed);
    attempt->aot_legacy_fallback_count =
        context.aot_legacy_fallback_count.load(std::memory_order_relaxed);
    attempt->aot_last_fallback_address =
        context.aot_last_fallback_address.load(std::memory_order_relaxed);
    attempt->aot_dynamic_attempt_count =
        context.aot_dynamic_attempt_count.load(std::memory_order_relaxed);
    attempt->aot_dynamic_success_count =
        context.aot_dynamic_success_count.load(std::memory_order_relaxed);
    attempt->aot_dynamic_added_bytes =
        context.aot_dynamic_added_bytes.load(std::memory_order_relaxed);
    attempt->aot_dbt_hle_reentry_attempt_count =
        context.aot_dbt_hle_reentry_attempt_count.load(
            std::memory_order_relaxed);
    attempt->aot_dbt_hle_reentry_success_count =
        context.aot_dbt_hle_reentry_success_count.load(
            std::memory_order_relaxed);
    attempt->aot_dbt_hle_translation_attempt_count =
        context.aot_dbt_hle_translation_attempt_count.load(
            std::memory_order_relaxed);
    attempt->aot_dbt_hle_translation_success_count =
        context.aot_dbt_hle_translation_success_count.load(
            std::memory_order_relaxed);
    attempt->aot_dbt_hle_dispatch_entry_count =
        context.aot_dbt_hle_dispatch_entry_count.load(
            std::memory_order_relaxed);
    attempt->aot_dbt_hle_dispatch_success_count =
        context.aot_dbt_hle_dispatch_success_count.load(
            std::memory_order_relaxed);
    attempt->aot_dbt_hle_dispatch_fallback_count =
        context.aot_dbt_hle_dispatch_fallback_count.load(
            std::memory_order_relaxed);
    attempt->aot_dbt_hle_dispatch_attempt_count =
        attempt->aot_dbt_hle_dispatch_success_count +
        attempt->aot_dbt_hle_dispatch_fallback_count;
    for (std::uint32_t index = 0;
         index < kAotDbtHleFallbackReasonCount; ++index)
    {
        attempt->aot_dbt_hle_dispatch_fallback_reason_counts[index] =
            context.aot_dbt_hle_dispatch_fallback_reason_counts[index].load(
                std::memory_order_relaxed);
    }
    attempt->aot_selector_guard_native_site_count =
        context.aot_selector_guard_native_site_count.load(
            std::memory_order_relaxed);
    attempt->aot_selector_guard_hle_site_count =
        context.aot_selector_guard_hle_site_count.load(
            std::memory_order_relaxed);
    attempt->aot_dbt_hle_dispatch_last_source =
        context.aot_dbt_hle_dispatch_last_source.load(
            std::memory_order_relaxed);
    attempt->aot_dbt_hle_dispatch_last_next =
        context.aot_dbt_hle_dispatch_last_next.load(
            std::memory_order_relaxed);
    attempt->aot_dbt_hle_dispatch_last_bytes =
        context.aot_dbt_hle_dispatch_last_bytes.load(
            std::memory_order_relaxed);
    attempt->aot_selector_guard_unresolved_site_count =
        context.aot_selector_guard_unresolved_site_count.load(
            std::memory_order_relaxed);
    attempt->aot_selector_guard_hle_exit_count =
        context.aot_selector_guard_hle_exit_count.load(
            std::memory_order_relaxed);
    attempt->aot_selector_guard_mismatch_count =
        context.aot_selector_guard_mismatch_count.load(
            std::memory_order_relaxed);
    if (context.aot_placement != nullptr)
    {
        attempt->aot_guarded_segment_pop_success_count =
            context.aot_placement->guarded_segment_pop_success_count;
        attempt->aot_guarded_segment_pop_fallback_count =
            context.aot_placement->guarded_segment_pop_fallback_count;
        attempt->aot_guarded_segment_load_success_count =
            context.aot_placement->guarded_segment_load_success_count;
        attempt->aot_guarded_segment_load_fallback_count =
            context.aot_placement->guarded_segment_load_fallback_count;
        attempt->aot_timer_safe_point_trap_count =
            context.aot_placement->timer_safe_point_trap_count;
        attempt->aot_timer_safe_point_injected_count =
            context.aot_placement->timer_safe_point_injected_count;
        attempt->aot_timer_safe_point_deferred_count =
            context.aot_placement->timer_safe_point_deferred_count;
        attempt->timer_handler_entry_cache_count =
            context.timer_handler_entry_cache_count;
        attempt->timer_handler_entry_translated_count =
            context.timer_handler_entry_translated_count;
        attempt->timer_handler_entry_native_count =
            context.timer_handler_entry_native_count;
        attempt->aot_timer_source_profile =
            context.aot_placement->timer_source_profile;
    }
    attempt->aot_dbt_return_entry_count =
        context.aot_dbt_return_entry_count.load(std::memory_order_relaxed);
    attempt->aot_dbt_return_success_count =
        context.aot_dbt_return_success_count.load(std::memory_order_relaxed);
    attempt->aot_dbt_return_fallback_count =
        context.aot_dbt_return_fallback_count.load(std::memory_order_relaxed);
    // Derive the reported attempt as success + fallback so the accounting
    // invariant also holds for a sample whose graceful timeout landed inside the
    // resolver (Task 281 open item, corrected in Task 282).
    attempt->aot_dbt_return_attempt_count =
        attempt->aot_dbt_return_success_count +
        attempt->aot_dbt_return_fallback_count;
    for (std::uint32_t index = 0;
         index < kAotDbtDispatchFallbackReasonCount; ++index)
    {
        attempt->aot_dbt_return_fallback_reason_counts[index] =
            context.aot_dbt_return_fallback_reason_counts[index].load(
                std::memory_order_relaxed);
    }
    attempt->aot_indirect_dispatch_count =
        context.aot_indirect_dispatch_count.load(std::memory_order_relaxed);
    attempt->aot_inline_cache_patch_attempt_count =
        context.aot_inline_cache_patch_attempt_count.load(
            std::memory_order_relaxed);
    attempt->aot_inline_cache_patch_success_count =
        context.aot_inline_cache_patch_success_count.load(
            std::memory_order_relaxed);
    attempt->aot_inline_cache_site_count = context.aot_placement != nullptr
        ? static_cast<std::uint32_t>(
              context.aot_placement->indirect_inline_cache_sites.size())
        : 0U;
    if (context.aot_placement != nullptr)
    {
        const AotInlineCacheSiteIndex& site_index =
            context.aot_placement->inline_cache_site_index;
        attempt->aot_inline_cache_site_index_lookup_count =
            site_index.lookup_count;
        attempt->aot_inline_cache_site_index_scan_count =
            site_index.fallback_scan_count;
        attempt->aot_inline_cache_site_index_rebuild_count =
            site_index.rebuild_count;
        const AotReturnDispatchSiteIndex& return_index =
            context.aot_placement->return_dispatch_site_index;
        attempt->aot_return_dispatch_site_count =
            static_cast<std::uint32_t>(
                context.aot_placement->dbt_return_dispatch_sites.size());
        attempt->aot_return_dispatch_site_index_lookup_count =
            return_index.lookup_count;
        attempt->aot_return_dispatch_site_index_scan_count =
            return_index.fallback_scan_count;
        attempt->aot_return_dispatch_site_index_rebuild_count =
            return_index.rebuild_count;
        const AotReturnPatchPolicy& return_patch_policy =
            context.aot_placement->return_patch_policy;
        attempt->aot_return_patch_policy_observation_count =
            return_patch_policy.observation_count;
        attempt->aot_return_patch_policy_megamorphic_site_count =
            return_patch_policy.megamorphic_site_count;
        attempt->aot_return_patch_policy_bypass_count =
            return_patch_policy.bypass_count;
        const runtime::AotDirectReturnTable& direct_return_table =
            context.aot_placement->direct_return_table;
        attempt->aot_direct_return_table_enabled =
            context.aot_placement->direct_return_table_enabled;
        attempt->aot_direct_return_table_entry_count =
            static_cast<std::uint32_t>(direct_return_table.entries.size());
        attempt->aot_direct_return_table_hit_count =
            direct_return_table.hit_count;
        attempt->aot_direct_return_table_insert_count =
            direct_return_table.insert_count;
        attempt->aot_direct_return_table_overwrite_count =
            direct_return_table.overwrite_count;
        attempt->aot_direct_return_table_clear_count =
            direct_return_table.clear_count;
        attempt->aot_direct_return_probe_site_count =
            static_cast<std::uint32_t>(
                context.aot_placement->direct_return_probe_sites.size());
        // Task 482: rank the policy sites once, after the guest thread has
        // stopped. The hot path only increments the per-site counters this
        // reads, so nothing here runs while the game is running.
        if (AotReturnStageProfileEnabled())
        {
            RankAotReturnStageSites(
                return_patch_policy,
                context.aot_placement->dbt_return_dispatch_sites,
                &attempt->aot_return_stage_sites);
        }
    }
    attempt->aot_last_reentry_cache_address =
        context.aot_reentry_cache_address;
    attempt->aot_code_write_count =
        context.aot_code_write_count.load(std::memory_order_relaxed);
    attempt->aot_page_retire_attempt_count =
        context.aot_page_retire_attempt_count.load(std::memory_order_relaxed);
    attempt->aot_page_retire_success_count =
        context.aot_page_retire_success_count.load(std::memory_order_relaxed);
    attempt->aot_generation_publish_count =
        context.aot_generation_publish_count.load(std::memory_order_relaxed);
    attempt->aot_generation_failure_count =
        context.aot_generation_failure_count.load(std::memory_order_relaxed);
    attempt->aot_generation_relinked_entry_count =
        context.aot_generation_relinked_entry_count.load(
            std::memory_order_relaxed);
    attempt->aot_retired_entry_trap_count =
        context.aot_retired_entry_trap_count.load(std::memory_order_relaxed);
    attempt->aot_retired_trap_profile =
        SnapshotAotRetiredTrapProfile(context.aot_retired_trap_profile);
    attempt->aot_quarantine_count =
        context.aot_quarantine_count.load(std::memory_order_relaxed);
    attempt->aot_last_code_write_source =
        context.aot_last_code_write_source.load(std::memory_order_relaxed);
    attempt->aot_last_code_write_destination =
        context.aot_last_code_write_destination.load(
            std::memory_order_relaxed);
    attempt->aot_last_retired_page =
        context.aot_last_retired_page.load(std::memory_order_relaxed);
    attempt->aot_last_published_generation =
        context.aot_last_published_generation.load(
            std::memory_order_relaxed);
    attempt->aot_exception_mapping_valid =
        context.aot_exception_mapping_valid;
    attempt->aot_exception_cache_address =
        context.aot_exception_cache_address;
    attempt->aot_exception_guest_address =
        context.aot_exception_guest_address;
    std::memcpy(attempt->aot_exception_cache_bytes,
                context.aot_exception_cache_bytes,
                sizeof(attempt->aot_exception_cache_bytes));
    std::memcpy(attempt->aot_exception_guest_bytes,
                context.aot_exception_guest_bytes,
                sizeof(attempt->aot_exception_guest_bytes));
    attempt->aot_last_indirect_source =
        context.aot_last_indirect_source.load(std::memory_order_relaxed);
    attempt->aot_last_indirect_target =
        context.aot_last_indirect_target.load(std::memory_order_relaxed);
    attempt->aot_return_dispatch_count =
        context.aot_return_dispatch_count.load(std::memory_order_relaxed);
    attempt->aot_last_return_target =
        context.aot_last_return_target.load(std::memory_order_relaxed);
    attempt->aot_last_return_source =
        context.aot_last_return_source.load(std::memory_order_relaxed);
    std::memcpy(attempt->aot_last_return_stack,
                context.aot_last_return_stack,
                sizeof(attempt->aot_last_return_stack));
    attempt->execution_probe_configured = context.execution_probe_configured;
    attempt->execution_probe_hit = context.execution_probe_hit;
    attempt->execution_probe_offset = context.execution_probe_offset;
    attempt->execution_probe_memory_offset =
        context.execution_probe_memory_offset;
    attempt->execution_probe_snapshot = context.execution_probe_snapshot;
    std::memcpy(attempt->execution_probe_stack,
                context.execution_probe_stack,
                sizeof(attempt->execution_probe_stack));
    std::memcpy(attempt->execution_probe_memory,
                context.execution_probe_memory,
                sizeof(attempt->execution_probe_memory));
    attempt->execution_probe_dump_configured =
        context.execution_probe_dump_request.configured;
    attempt->execution_probe_dump_captured =
        context.execution_probe_dump_result.captured;
    attempt->execution_probe_dump_written =
        context.execution_probe_dump_result.written;
    attempt->execution_probe_dump_base_address =
        context.execution_probe_dump_result.base_address;
    attempt->execution_probe_dump_source_address =
        context.execution_probe_dump_result.source_address;
    attempt->execution_probe_dump_byte_count =
        context.execution_probe_dump_result.byte_count;
    attempt->execution_probe_dump_path =
        context.execution_probe_dump_request.path;
    attempt->execution_trace_configured = context.execution_trace_configured;
    attempt->execution_trace_start_offset = context.execution_trace_start_offset;
    attempt->execution_trace_end_offset = context.execution_trace_end_offset;
    attempt->execution_trace_esp_offset = context.execution_trace_esp_offset;
    attempt->execution_trace_hit_count = context.execution_trace_hit_count;
    attempt->execution_trace_sentinel2_configured =
        context.execution_trace_sentinel2_configured;
    attempt->execution_trace_sentinel2_offset =
        context.execution_trace_sentinel2_offset;
    attempt->execution_trace_sentinel_rearm_count =
        context.execution_trace_sentinel_rearm_count;
    std::memcpy(attempt->execution_trace, context.execution_trace,
                sizeof(attempt->execution_trace));
    attempt->aot_call_depth = context.aot_call_depth;
    attempt->aot_last_return_matches_call =
        context.aot_last_return_matches_call;
    attempt->aot_last_expected_return = context.aot_last_expected_return;
    attempt->aot_last_call_source = context.aot_last_call_source;
    attempt->aot_last_call_target = context.aot_last_call_target;
    attempt->aot_last_expected_call_source =
        context.aot_last_expected_call_source;
    attempt->aot_last_expected_call_target =
        context.aot_last_expected_call_target;
    attempt->aot_return_trace_count = context.aot_return_trace_count;
    std::memcpy(attempt->aot_return_trace, context.aot_return_trace,
                sizeof(attempt->aot_return_trace));
    attempt->aot_transfer_trace_count = context.aot_transfer_trace_count;
    std::memcpy(attempt->aot_transfer_trace, context.aot_transfer_trace,
                sizeof(attempt->aot_transfer_trace));
    attempt->aot_dbt_call_return_trace_configured =
        context.aot_dbt_call_return_trace_configured;
    attempt->aot_dbt_call_return_trace_count =
        context.aot_dbt_call_return_trace_count;
    attempt->aot_dbt_call_return_call_count =
        context.aot_dbt_call_return_call_count;
    attempt->aot_dbt_call_return_return_count =
        context.aot_dbt_call_return_return_count;
    attempt->aot_dbt_call_return_match_count =
        context.aot_dbt_call_return_match_count;
    attempt->aot_dbt_call_return_mismatch_count =
        context.aot_dbt_call_return_mismatch_count;
    attempt->aot_dbt_call_return_overwrite_count =
        context.aot_dbt_call_return_overwrite_count;
    attempt->aot_dbt_call_return_first_divergence_valid =
        context.aot_dbt_call_return_first_divergence_valid;
    attempt->aot_dbt_call_return_first_divergence =
        context.aot_dbt_call_return_first_divergence;
    std::memcpy(attempt->aot_dbt_call_return_trace,
                context.aot_dbt_call_return_trace,
                sizeof(attempt->aot_dbt_call_return_trace));
    attempt->diagnostic_poll_iteration_count =
        context.diagnostic_poll_iteration_count;
    attempt->diagnostic_progress_count =
        context.diagnostic_progress_count.load(std::memory_order_relaxed);
    attempt->diagnostic_quiet_iteration_count =
        context.diagnostic_quiet_iteration_count;
    attempt->exception_dispatch_entry_count =
        context.exception_dispatch_entry_count.load(
            std::memory_order_relaxed);
    attempt->exception_dispatch_exit_count =
        context.exception_dispatch_exit_count.load(
            std::memory_order_relaxed);
    attempt->exception_dispatch_last_eip =
        context.exception_dispatch_last_eip.load(
            std::memory_order_relaxed);
    attempt->exception_dispatch_malformed_count =
        context.exception_dispatch_malformed_count.load(
            std::memory_order_relaxed);
    attempt->exception_dispatch_last_bad_context =
        context.exception_dispatch_last_bad_context.load(
            std::memory_order_relaxed);
    attempt->exception_dispatch_last_bad_record =
        context.exception_dispatch_last_bad_record.load(
            std::memory_order_relaxed);
    attempt->selector_table_valid = context.selector_table.valid;
    attempt->selector_descriptor_count =
        static_cast<std::uint32_t>(
            context.selector_table.descriptors.size());
    attempt->linexe_environment_active = context.linexe_environment_active;
    attempt->linexe_gs_byte_load_count = context.linexe_gs_byte_load_count;
    attempt->linexe_first_gs_byte_offset = context.linexe_first_gs_byte_offset;
    attempt->linexe_first_gs_byte_value = context.linexe_first_gs_byte_value;
    const repiu::runtime::GuestDescriptor* client_descriptor =
        repiu::runtime::FindDescriptor(context.selector_table,
                                       kDos4gwClientDataSelector);
    if (client_descriptor != nullptr && client_descriptor->present)
    {
        attempt->linexe_client_descriptor_valid = true;
        attempt->linexe_client_descriptor_base = client_descriptor->base;
        attempt->linexe_client_descriptor_limit = client_descriptor->limit;
        if (client_descriptor->limit >= kDos4gwPrivateRootOffset + 3U &&
            static_cast<std::uint64_t>(client_descriptor->base) +
                    kDos4gwPrivateRootOffset + 4U <=
                static_cast<std::uint64_t>(context.runtime_base) +
                    context.runtime_size)
        {
            const auto* root = reinterpret_cast<const std::uint16_t*>(
                static_cast<std::uintptr_t>(client_descriptor->base +
                                            kDos4gwPrivateRootOffset));
            attempt->linexe_root_offset = root[0];
            attempt->linexe_root_selector = root[1];
        }
    }
    const repiu::runtime::GuestDescriptor* data_descriptor =
        repiu::runtime::FindDescriptor(context.selector_table,
                                       kDos4gwLinexeDataSelector);
    if (data_descriptor != nullptr && data_descriptor->present)
    {
        attempt->linexe_data_descriptor_valid = true;
        attempt->linexe_data_descriptor_base = data_descriptor->base;
        const auto* module = reinterpret_cast<const std::uint16_t*>(
            static_cast<std::uintptr_t>(data_descriptor->base +
                                        kDos4gwLinexeLoaderOffset));
        attempt->linexe_module_name_offset = module[2];
        attempt->linexe_module_name_selector = module[3];
        attempt->linexe_direct_export_count = module[8];
        attempt->linexe_direct_export_table_offset = module[9];
        attempt->linexe_direct_export_table_selector = module[10];
        const auto* first_export = reinterpret_cast<const std::uint16_t*>(
            static_cast<std::uintptr_t>(data_descriptor->base + module[9]));
        attempt->linexe_direct_first_export_name_offset = first_export[0];
        attempt->linexe_direct_first_export_name_selector = first_export[1];
        const char* name = reinterpret_cast<const char*>(
            static_cast<std::uintptr_t>(data_descriptor->base + module[2]));
        for (std::uint32_t index = 0; index < 64 && name[index] != '\0'; ++index)
        {
            attempt->linexe_direct_module_name.push_back(name[index]);
        }
    }
    attempt->linexe_scan_entry_count = context.linexe_scan_entry_count;
    attempt->linexe_module_candidate_count =
        context.linexe_module_candidate_count;
    attempt->linexe_module_match_count = context.linexe_module_match_count;
    attempt->linexe_name_pointer_valid_count =
        context.linexe_name_pointer_valid_count;
    attempt->linexe_name_byte_instruction_count =
        context.linexe_name_byte_instruction_count;
    attempt->linexe_data_gs_load_count = context.linexe_data_gs_load_count;
    attempt->linexe_module_selector_stack_value =
        context.linexe_module_selector_stack_value;
    attempt->linexe_module_offset_stack_value =
        context.linexe_module_offset_stack_value;
    attempt->linexe_export_offset_stack_value =
        context.linexe_export_offset_stack_value;
    attempt->linexe_export_selector_stack_value =
        context.linexe_export_selector_stack_value;
    attempt->linexe_export_jump_source_esp =
        context.linexe_export_jump_source_esp;
    attempt->linexe_export_jump_source_module_offset =
        context.linexe_export_jump_source_module_offset;
    attempt->linexe_export_jump_source_module_selector =
        context.linexe_export_jump_source_module_selector;
    attempt->linexe_export_jump_target_esp =
        context.linexe_export_jump_target_esp;
    attempt->linexe_export_jump_target_module_offset =
        context.linexe_export_jump_target_module_offset;
    attempt->linexe_export_jump_target_module_selector =
        context.linexe_export_jump_target_module_selector;
    attempt->linexe_export_name_compare_count =
        context.linexe_export_name_compare_count;
    attempt->linexe_export_name_compare_gs =
        context.linexe_export_name_compare_gs;
    attempt->linexe_export_name_compare_edi =
        context.linexe_export_name_compare_edi;
    attempt->linexe_export_name_compare_esi =
        context.linexe_export_name_compare_esi;
    attempt->linexe_export_name_actual_byte =
        context.linexe_export_name_actual_byte;
    attempt->linexe_export_name_expected_byte =
        context.linexe_export_name_expected_byte;
    attempt->linexe_export_name_stage_mask =
        context.linexe_export_name_stage_mask;
    attempt->linexe_export_entry_name_offset_value =
        context.linexe_export_entry_name_offset_value;
    attempt->linexe_export_entry_name_selector_value =
        context.linexe_export_entry_name_selector_value;
    attempt->linexe_export_result_store_destination =
        context.linexe_export_result_store_destination;
    attempt->linexe_export_result_store_value =
        context.linexe_export_result_store_value;
    attempt->linexe_export_result_store_count =
        context.linexe_export_result_store_count;
    attempt->linexe_export_value_load_selector =
        context.linexe_export_value_load_selector;
    attempt->linexe_export_value_load_offset =
        context.linexe_export_value_load_offset;
    attempt->linexe_export_value_load_value =
        context.linexe_export_value_load_value;
    attempt->linexe_root_selector_eax = context.linexe_root_selector_eax;
    attempt->linexe_root_read_gs = context.linexe_root_read_gs;
    attempt->linexe_shared_load_entry_count =
        context.linexe_shared_load_entry_count;
    attempt->linexe_shared_load_read_count =
        context.linexe_shared_load_read_count;
    attempt->linexe_shared_load_selector = context.linexe_shared_load_selector;
    attempt->linexe_shared_load_offset = context.linexe_shared_load_offset;
    attempt->linexe_shared_load_value = context.linexe_shared_load_value;
    attempt->linexe_root_offset_load_value =
        context.linexe_root_offset_load_value;
    attempt->linexe_root_selector_load_value =
        context.linexe_root_selector_load_value;
    attempt->linexe_root_offset_load_success =
        context.linexe_root_offset_load_success;
    attempt->linexe_root_selector_load_success =
        context.linexe_root_selector_load_success;
    attempt->linexe_export_match_count = context.linexe_export_match_count;
    attempt->linexe_export_entry_loop_count =
        context.linexe_export_entry_loop_count;
    attempt->linexe_export_compare_count = context.linexe_export_compare_count;
    attempt->linexe_export_compare_eax = context.linexe_export_compare_eax;
    attempt->linexe_export_compare_ecx = context.linexe_export_compare_ecx;
    attempt->linexe_export_compare_eflags =
        context.linexe_export_compare_eflags;
    attempt->linexe_export_count_load_edx =
        context.linexe_export_count_load_edx;
    attempt->linexe_export_count_load_gs =
        context.linexe_export_count_load_gs;
    attempt->linexe_scan_return_count = context.linexe_scan_return_count;
    attempt->linexe_indirect_far_call_count = context.linexe_indirect_far_call_count;
    attempt->linexe_indirect_far_call_source = context.linexe_indirect_far_call_source;
    attempt->linexe_indirect_far_call_pointer = context.linexe_indirect_far_call_pointer;
    attempt->linexe_indirect_far_call_offset = context.linexe_indirect_far_call_offset;
    attempt->linexe_indirect_far_call_selector = context.linexe_indirect_far_call_selector;
    attempt->linexe_indirect_far_call_known_export = context.linexe_indirect_far_call_known_export;
    attempt->timer_interrupt_chain_hle_count = context.timer_interrupt_chain_hle_count;
    attempt->timer_interrupt_chain_hle_source = context.timer_interrupt_chain_hle_source;
    attempt->timer_interrupt_chain_hle_pointer = context.timer_interrupt_chain_hle_pointer;
    attempt->timer_interrupt_chain_hle_offset = context.timer_interrupt_chain_hle_offset;
    attempt->timer_interrupt_chain_hle_selector = context.timer_interrupt_chain_hle_selector;
    attempt->linexe_bridge_entry_count = context.linexe_bridge_entry_count;
    attempt->linexe_bridge_gate_valid = context.linexe_bridge_gate_valid;
    attempt->linexe_bridge_selector = context.linexe_bridge_selector;
    attempt->linexe_bridge_offset = context.linexe_bridge_offset;
    attempt->linexe_bridge_service = context.linexe_bridge_service;
    attempt->linexe_bridge_esp = context.linexe_bridge_esp;
    attempt->linexe_bridge_ebp = context.linexe_bridge_ebp;
    std::memcpy(attempt->linexe_bridge_stack,
                context.linexe_bridge_stack,
                sizeof(attempt->linexe_bridge_stack));
    std::memcpy(attempt->linexe_bridge_argument_text,
                context.linexe_bridge_argument_text,
                sizeof(attempt->linexe_bridge_argument_text));
    std::memcpy(attempt->linexe_bridge_stack_text,
                context.linexe_bridge_stack_text,
                sizeof(attempt->linexe_bridge_stack_text));
    attempt->linexe_virtual_module_load_count =
        context.linexe_virtual_module_load_count;
    attempt->linexe_virtual_module_handle =
        context.linexe_virtual_module_handle;
    attempt->linexe_get_proc_count = context.linexe_get_proc_count;
    attempt->linexe_get_proc_result_pointer =
        context.linexe_get_proc_result_pointer;
    std::memcpy(attempt->linexe_get_proc_name,
                context.linexe_get_proc_name,
                sizeof(attempt->linexe_get_proc_name));
    attempt->glide_gate_entry_count = context.glide_gate_entry_count;
    attempt->glide_gate_handled_count = context.glide_gate_handled_count;
    attempt->glide_gate_esp = context.glide_gate_esp;
    std::memcpy(attempt->glide_gate_stack,
                context.glide_gate_stack,
                sizeof(attempt->glide_gate_stack));
    attempt->glide_gate_ordinal = context.glide_gate_ordinal;
    attempt->glide_gate_argument_bytes = context.glide_gate_argument_bytes;
    std::memcpy(attempt->glide_gate_name,
                context.glide_gate_name,
                sizeof(attempt->glide_gate_name));
    attempt->glide_implementation_issues =
        context.glide_implementation_issues;
    attempt->glide_texture_gate_trace_count = context.glide_texture_gate_trace_count;
    attempt->glide_texture_gate_trace_wrapped = context.glide_texture_gate_trace_wrapped;
    std::memcpy(attempt->glide_texture_gate_trace,
                context.glide_texture_gate_trace,
                sizeof(attempt->glide_texture_gate_trace));
    attempt->glide_first_triangle = context.glide_first_triangle;
    attempt->glide_triangle_trace_count = context.glide_triangle_trace_count;
    attempt->glide_triangle_trace_wrapped = context.glide_triangle_trace_wrapped;
    std::memcpy(attempt->glide_triangle_trace, context.glide_triangle_trace,
                sizeof(attempt->glide_triangle_trace));
    for (std::size_t ordinal = 0;
         ordinal < context.glide_call_counts.size(); ++ordinal)
    {
        if (context.glide_call_counts[ordinal] == 0U)
        {
            continue;
        }
        MinimalExecutionAttempt::GlideCallObservation observation;
        observation.ordinal = static_cast<std::uint16_t>(ordinal);
        observation.count = context.glide_call_counts[ordinal];
        observation.name = context.glide_call_names[ordinal];
        std::copy(context.glide_first_stacks[ordinal].begin(),
                  context.glide_first_stacks[ordinal].end(),
                  observation.first_stack);
        attempt->glide_calls.push_back(std::move(observation));
    }
    for (std::size_t ordinal = 0;
         ordinal < attempt->glide_ordinal_timing.entries.size(); ++ordinal)
    {
        const GlideOrdinalTimingEntry& timing =
            attempt->glide_ordinal_timing.entries[ordinal];
        if (timing.count == 0U)
        {
            continue;
        }
        MinimalExecutionAttempt::GlideOrdinalTimingObservation
            observation;
        observation.ordinal = static_cast<std::uint16_t>(ordinal);
        observation.name = context.glide_call_names[ordinal];
        observation.timing = timing;
        attempt->glide_ordinal_timings.push_back(std::move(observation));
    }
    std::sort(
        attempt->glide_ordinal_timings.begin(),
        attempt->glide_ordinal_timings.end(),
        [](const auto& left, const auto& right) {
            if (left.timing.gate_cycles != right.timing.gate_cycles)
            {
                return left.timing.gate_cycles >
                    right.timing.gate_cycles;
            }
            return left.ordinal < right.ordinal;
        });
    // Read from the profile rather than the snapshot: the snapshot carries only
    // aggregates so a copy does not move the whole 256-entry array.
    for (std::size_t ordinal = 0;
         ordinal < context.glide_setter_census.entries.size(); ++ordinal)
    {
        const GlideSetterCensusEntry& census =
            context.glide_setter_census.entries[ordinal];
        const GlideSetterStateCacheEntry& cache =
            context.glide_setter_state_cache.entries[ordinal];
        if (census.call_count == 0U && census.key_overflow_count == 0U &&
            cache.elided_count == 0U && cache.applied_count == 0U)
        {
            continue;
        }
        MinimalExecutionAttempt::GlideSetterCensusObservation observation;
        observation.ordinal = static_cast<std::uint16_t>(ordinal);
        observation.name = context.glide_call_names[ordinal];
        observation.census = census;
        observation.elided_count = cache.elided_count;
        observation.applied_count = cache.applied_count;
        attempt->glide_setter_censuses.push_back(std::move(observation));
    }
    // Ranked by call volume: the leading setters are the ones whose repetition
    // rate decides whether Task 365 is worth implementing.
    std::sort(
        attempt->glide_setter_censuses.begin(),
        attempt->glide_setter_censuses.end(),
        [](const auto& left, const auto& right) {
            if (left.census.call_count != right.census.call_count)
            {
                return left.census.call_count > right.census.call_count;
            }
            return left.ordinal < right.ordinal;
        });
    attempt->mscdex_available = context.mscdex_available;
    attempt->cd_audio_available = context.cd_audio_available;
    attempt->mscdex_track_count = static_cast<std::uint32_t>(
        context.cd_image.tracks().size());
    attempt->mscdex_request_count = context.mscdex_request_count;
    attempt->mscdex_frame_es = context.mscdex_frame_es;
    attempt->mscdex_decline_count = context.mscdex_decline_count;
    attempt->mscdex_last_decline_reason = context.mscdex_last_decline_reason;
    attempt->mscdex_last_resolve_kind = context.mscdex_last_resolve_kind;
    attempt->mscdex_last_header_bytes = context.mscdex_last_header_bytes;
    attempt->mscdex_last_ioctl_subfunction =
        context.mscdex_last_ioctl_subfunction;
    attempt->mscdex_last_ioctl_handled = context.mscdex_last_ioctl_handled;
    attempt->mscdex_last_ioctl_length = context.mscdex_last_ioctl_length;
    attempt->mscdex_ioctl_reject_mask = context.mscdex_ioctl_reject_mask;
    attempt->mscdex_last_play_mode = context.mscdex_last_play_mode;
    attempt->mscdex_last_play_start = context.mscdex_last_play_start;
    attempt->mscdex_last_play_length = context.mscdex_last_play_length;
    attempt->mscdex_last_seek_target = context.mscdex_last_seek_target;
    attempt->cd_audio_current_lba = context.cd_audio.current_lba();
    attempt->glide_window_open_count = context.glide_window_open_count;
    attempt->glide_logical_width = context.glide_logical_width;
    attempt->glide_logical_height = context.glide_logical_height;
    attempt->glide_backend_message = context.glide_backend_message;
    attempt->glide_texture_memory_bytes =
        context.glide_state.texture_memory_bytes;
    repiu::hle::CalculateGlideTextureMaxAddress(
        context.glide_state.texture_memory_bytes,
        &attempt->glide_texture_max_address);
    attempt->linexe_scan_return_eax = context.linexe_scan_return_eax;
    attempt->linexe_scan_return_ebp = context.linexe_scan_return_ebp;
    attempt->linexe_scan_caller_eax = context.linexe_scan_caller_eax;
    std::memcpy(attempt->linexe_selector_init_results,
                context.linexe_selector_init_results,
                sizeof(attempt->linexe_selector_init_results));
    attempt->dpmi_allocate_call_count = context.dpmi_allocate_call_count;
    attempt->dpmi_last_allocate_requested_count =
        context.dpmi_last_allocate_requested_count;
    attempt->dpmi_last_allocated_selector =
        context.dpmi_last_allocated_selector;
    constexpr std::uint32_t kSelectorWordsOffset = 0x000C68C0U;
    constexpr std::uint32_t kResolvedExportsOffset = 0x001A62C4U;
    constexpr std::uint32_t kSavedClientGsOffset = 0x001A6354U;
    const std::uint64_t runtime_end =
        static_cast<std::uint64_t>(context.runtime_base) +
        context.runtime_size;
    if (context.linexe_environment_active &&
        static_cast<std::uint64_t>(context.runtime_base) +
                kResolvedExportsOffset + 8U * 8U <= runtime_end)
    {
        const auto* saved_gs = reinterpret_cast<const std::uint16_t*>(
            static_cast<std::uintptr_t>(context.runtime_base +
                                        kSavedClientGsOffset));
        attempt->linexe_saved_client_gs = *saved_gs;
        const auto* selector_words = reinterpret_cast<const std::uint16_t*>(
            static_cast<std::uintptr_t>(context.runtime_base +
                                        kSelectorWordsOffset));
        std::memcpy(attempt->linexe_selector_words,
                    selector_words,
                    sizeof(attempt->linexe_selector_words));
        for (std::uint32_t index = 0; index < 8; ++index)
        {
            const auto* value = reinterpret_cast<const std::uint32_t*>(
                static_cast<std::uintptr_t>(
                    context.runtime_base + kResolvedExportsOffset +
                    index * 8U));
            attempt->linexe_resolved_exports[index] = *value;
            if (*value != 0)
            {
                ++attempt->linexe_resolved_export_count;
            }
        }
    }
    attempt->dos_low_memory_valid = context.dos_low_memory.valid;
    attempt->dos_low_memory_size =
        repiu::runtime::kDosLowMemorySize;
    BuildSingleStepSnapshot(context, &attempt->last_single_step_snapshot);
    attempt->dos_environment_block_size =
        static_cast<std::uint32_t>(context.dos_environment_block.size());
    attempt->last_dos_environment_access_valid =
        context.last_dos_environment_access_valid;
    attempt->last_dos_environment_access_offset =
        context.last_dos_environment_access_offset;
    attempt->last_dos_environment_entry_offset =
        context.last_dos_environment_entry_offset;
    attempt->last_dos_environment_value_length =
        context.last_dos_environment_value_length;
    attempt->last_dos_environment_entry_name =
        context.last_dos_environment_entry_name;
    attempt->handled_hle_trap_count = context.handled_hle_trap_count;
    attempt->last_hle_trap_address = context.last_hle_trap_address;
    attempt->last_hle_trap_opcode = context.last_hle_trap_opcode;
    attempt->port_io = context.port_io;
    const JammaInputTimelineSnapshot jamma_timeline =
        context.jamma_input_timeline.Snapshot();
    attempt->port_io.jamma_timeline_edge_count = jamma_timeline.edge_count;
    attempt->port_io.jamma_timeline_history_pruned_count =
        jamma_timeline.history_pruned_count;
    attempt->port_io.jamma_timeline_history_overflow_count =
        jamma_timeline.history_overflow_count;
    attempt->port_io.jamma_timeline_history_coverage_miss_count =
        jamma_timeline.history_coverage_miss_count;
    attempt->port_io.jamma_timeline_history_peak_size =
        jamma_timeline.history_peak_size;
    attempt->port_io.jamma_timeline_due_enqueued_count =
        jamma_timeline.due_enqueued_count;
    attempt->port_io.jamma_timeline_due_overflow_count =
        jamma_timeline.due_overflow_count;
    attempt->port_io.jamma_timeline_replay_begin_count =
        jamma_timeline.replay_begin_count;
    attempt->port_io.jamma_timeline_replay_read_count =
        jamma_timeline.replay_read_count;
    attempt->port_io.jamma_timeline_missing_due_count =
        jamma_timeline.replay_missing_due_count;
    attempt->port_io.jamma_timeline_frame_retire_count =
        jamma_timeline.replay_frame_retire_count;
    attempt->port_io.jamma_timeline_frame_overflow_count =
        jamma_timeline.replay_frame_overflow_count;
    attempt->port_io.jamma_timeline_active_frame_depth =
        jamma_timeline.replay_frame_depth;
    attempt->port_io.jamma_timeline_frame_end_count =
        jamma_timeline.replay_frame_end_count;
    attempt->port_io.jamma_timeline_frame_stale_count =
        jamma_timeline.replay_frame_stale_count;
    attempt->port_io.jamma_timeline_latest_state_read_count =
        jamma_timeline.latest_state_read_count;
    attempt->dos_path = context.dos_path;
    attempt->dos_file_io = context.dos_file_io;
    attempt->dos_file_io.read_count = context.dos_file_system.file_read_count;
    attempt->dos_file_io.host_open_count =
        context.dos_file_system.host_file_open_count;
    attempt->allocator_probe = context.allocator_probe;
    attempt->allocator_control_flow = context.allocator_control_flow;
    attempt->handled_dos_interrupt_count =
        context.handled_dos_interrupt_count;
    attempt->last_dos_interrupt_vector = context.last_dos_interrupt_vector;
    attempt->last_dos_interrupt_ah = context.last_dos_interrupt_ah;
    attempt->last_dos_interrupt_ax = context.last_dos_interrupt_ax;
    std::memcpy(attempt->handled_dos_interrupt_ah_counts, context.handled_dos_interrupt_ah_counts, sizeof(attempt->handled_dos_interrupt_ah_counts));
    attempt->handled_dos_chdir_count = context.handled_dos_chdir_count;
    attempt->last_dos_chdir_guest_path =
        context.last_dos_chdir_guest_path;
    attempt->last_dos_chdir_host_path = context.last_dos_chdir_host_path;
    attempt->last_dos_chdir_virtual_path =
        context.last_dos_chdir_virtual_path;
    attempt->last_dos_chdir_success = context.last_dos_chdir_success;
    attempt->last_dos_chdir_error = context.last_dos_chdir_error;
    attempt->handled_dos_getcwd_count = context.handled_dos_getcwd_count;
    attempt->last_dos_getcwd_drive = context.last_dos_getcwd_drive;
    attempt->last_dos_getcwd_path = context.last_dos_getcwd_path;
    attempt->last_dos_getcwd_success = context.last_dos_getcwd_success;
    attempt->last_dos_getcwd_error = context.last_dos_getcwd_error;
    attempt->handled_dos_getdrive_count =
        context.handled_dos_getdrive_count;
    attempt->last_dos_getdrive_value = context.last_dos_getdrive_value;
    attempt->handled_dos_open_count = context.handled_dos_open_count;
    attempt->last_dos_open_guest_path = context.last_dos_open_guest_path;
    attempt->last_dos_open_host_path = context.last_dos_open_host_path;
    attempt->last_dos_open_virtual_path =
        context.last_dos_open_virtual_path;
    attempt->last_dos_open_success = context.last_dos_open_success;
    attempt->last_dos_open_error = context.last_dos_open_error;
    attempt->last_dos_open_handle = context.last_dos_open_handle;
    attempt->last_dos_open_access_mode = context.last_dos_open_access_mode;
    attempt->handled_dos_read_count = context.handled_dos_read_count;
    attempt->last_dos_read_handle = context.last_dos_read_handle;
    attempt->last_dos_read_requested_bytes =
        context.last_dos_read_requested_bytes;
    attempt->last_dos_read_actual_bytes =
        context.last_dos_read_actual_bytes;
    attempt->last_dos_read_buffer = context.last_dos_read_buffer;
    attempt->last_dos_read_success = context.last_dos_read_success;
    attempt->last_dos_read_error = context.last_dos_read_error;
    attempt->handled_dos_create_count = context.handled_dos_create_count;
    attempt->last_dos_create_guest_path = context.last_dos_create_guest_path;
    attempt->last_dos_create_host_path = context.last_dos_create_host_path;
    attempt->last_dos_create_virtual_path =
        context.last_dos_create_virtual_path;
    attempt->last_dos_create_success = context.last_dos_create_success;
    attempt->last_dos_create_error = context.last_dos_create_error;
    attempt->last_dos_create_handle = context.last_dos_create_handle;
    attempt->last_dos_create_attributes = context.last_dos_create_attributes;
    attempt->handled_dos_write_count = context.handled_dos_write_count;
    attempt->last_dos_write_handle = context.last_dos_write_handle;
    attempt->last_dos_write_requested_bytes =
        context.last_dos_write_requested_bytes;
    attempt->last_dos_write_actual_bytes =
        context.last_dos_write_actual_bytes;
    attempt->last_dos_write_buffer = context.last_dos_write_buffer;
    attempt->last_dos_write_success = context.last_dos_write_success;
    attempt->last_dos_write_error = context.last_dos_write_error;
    attempt->handled_dos_seek_count = context.handled_dos_seek_count;
    attempt->dos_console_input_count = context.dos_console_input_count;
    attempt->dos_console_input_wait_count =
        context.dos_console_input_wait_count;
    attempt->last_dos_seek_handle = context.last_dos_seek_handle;
    attempt->last_dos_seek_origin = context.last_dos_seek_origin;
    attempt->last_dos_seek_offset = context.last_dos_seek_offset;
    attempt->last_dos_seek_position = context.last_dos_seek_position;
    attempt->last_dos_seek_success = context.last_dos_seek_success;
    attempt->last_dos_seek_error = context.last_dos_seek_error;
    attempt->handled_dos_close_count = context.handled_dos_close_count;
    attempt->last_dos_close_handle = context.last_dos_close_handle;
    attempt->last_dos_close_success = context.last_dos_close_success;
    attempt->last_dos_close_error = context.last_dos_close_error;
    attempt->handled_dos_ioctl_count = context.handled_dos_ioctl_count;
    attempt->last_dos_ioctl_subfunction =
        context.last_dos_ioctl_subfunction;
    attempt->last_dos_ioctl_handle = context.last_dos_ioctl_handle;
    attempt->last_dos_ioctl_success = context.last_dos_ioctl_success;
    attempt->last_dos_ioctl_error = context.last_dos_ioctl_error;
    attempt->last_dos_ioctl_device_info =
        context.last_dos_ioctl_device_info;
    attempt->handled_dos_resize_count = context.handled_dos_resize_count;
    attempt->last_dos_resize_selector = context.last_dos_resize_selector;
    attempt->last_dos_resize_paragraphs =
        context.last_dos_resize_paragraphs;
    attempt->last_dos_resize_success = context.last_dos_resize_success;
    attempt->last_dos_resize_error = context.last_dos_resize_error;
    attempt->last_dos_resize_requested_end =
        context.last_dos_resize_requested_end;
    attempt->last_dos_resize_allocator_end =
        context.last_dos_resize_allocator_end;
    attempt->handled_segment_load_count =
        context.handled_segment_load_count;
    attempt->last_segment_load_address = context.last_segment_load_address;
    attempt->last_segment_load_opcode = context.last_segment_load_opcode;
    attempt->last_segment_load_register =
        context.last_segment_load_register;
    attempt->last_segment_load_selector =
        context.last_segment_load_selector;
    attempt->last_segment_load_source = context.last_segment_load_source;
    std::memcpy(attempt->handled_segment_load_register_counts, context.handled_segment_load_register_counts, sizeof(attempt->handled_segment_load_register_counts));
    attempt->segment_load = context.segment_load;
    attempt->handled_segment_store_count =
        context.handled_segment_store_count;
    attempt->last_segment_store_address = context.last_segment_store_address;
    attempt->last_segment_store_opcode = context.last_segment_store_opcode;
    attempt->last_segment_store_register =
        context.last_segment_store_register;
    attempt->last_segment_store_selector =
        context.last_segment_store_selector;
    attempt->last_segment_store_destination =
        context.last_segment_store_destination;
    std::memcpy(attempt->handled_segment_store_register_counts, context.handled_segment_store_register_counts, sizeof(attempt->handled_segment_store_register_counts));
    attempt->handled_segment_memory_load_count =
        context.handled_segment_memory_load_count;
    attempt->last_segment_memory_load_address =
        context.last_segment_memory_load_address;
    attempt->last_segment_memory_load_opcode =
        context.last_segment_memory_load_opcode;
    attempt->last_segment_memory_load_register =
        context.last_segment_memory_load_register;
    attempt->last_segment_memory_load_selector =
        context.last_segment_memory_load_selector;
    attempt->last_segment_memory_load_offset =
        context.last_segment_memory_load_offset;
    attempt->last_segment_memory_load_width =
        context.last_segment_memory_load_width;
    attempt->last_segment_memory_load_value =
        context.last_segment_memory_load_value;
    attempt->handled_low_memory_access_count =
        context.handled_low_memory_access_count;
    attempt->last_low_memory_access_address =
        context.last_low_memory_access_address;
    attempt->last_low_memory_access_opcode =
        context.last_low_memory_access_opcode;
    attempt->last_low_memory_access_esi =
        context.last_low_memory_access_esi;
    attempt->last_low_memory_access_edi =
        context.last_low_memory_access_edi;
    attempt->last_low_memory_access_destination =
        context.last_low_memory_access_destination;
    attempt->last_low_memory_access_value =
        context.last_low_memory_access_value;
    attempt->low_memory_read_emulate_count =
        context.low_memory_read_emulate_count;
    attempt->last_low_memory_read_emulate_address =
        context.last_low_memory_read_emulate_address;
    attempt->last_low_memory_read_emulate_eip =
        context.last_low_memory_read_emulate_eip;
    attempt->last_low_memory_read_emulate_value =
        context.last_low_memory_read_emulate_value;
    attempt->last_low_memory_read_emulate_reg =
        context.last_low_memory_read_emulate_reg;
    attempt->low_memory_string_service_count =
        context.low_memory_string_service_count;
    attempt->low_memory_string_iteration_count =
        context.low_memory_string_iteration_count;
    attempt->last_low_memory_string_eip =
        context.last_low_memory_string_eip;
    attempt->last_low_memory_string_address =
        context.last_low_memory_string_address;
    attempt->last_low_memory_string_mnemonic =
        context.last_low_memory_string_mnemonic;
    attempt->last_low_memory_string_iterations =
        context.last_low_memory_string_iterations;
    attempt->debug_emulate_stage =
        context.debug_emulate_stage;
    attempt->debug_emulate_decode_result =
        context.debug_emulate_decode_result;
    attempt->debug_emulate_calculated_address =
        context.debug_emulate_calculated_address;
    attempt->rep_movs_copy_failure_count =
        context.rep_movs_copy_failure_count;
    attempt->last_rep_movs_copy_failure_stage =
        context.last_rep_movs_copy_failure_stage;
    attempt->last_rep_movs_copy_error =
        context.last_rep_movs_copy_error;
    attempt->last_rep_movs_copy_source =
        context.last_rep_movs_copy_source;
    attempt->last_rep_movs_copy_destination =
        context.last_rep_movs_copy_destination;
    attempt->last_rep_movs_copy_bytes =
        context.last_rep_movs_copy_bytes;
    attempt->handled_memory_store_count = context.handled_memory_store_count;
    attempt->last_memory_store_address = context.last_memory_store_address;
    attempt->last_memory_store_opcode = context.last_memory_store_opcode;
    attempt->last_memory_store_destination =
        context.last_memory_store_destination;
    attempt->last_memory_store_value = context.last_memory_store_value;
    attempt->last_memory_store_width = context.last_memory_store_width;
    attempt->last_memory_store_source_kind =
        context.last_memory_store_source_kind;
    attempt->last_memory_store_applied = context.last_memory_store_applied;
    attempt->shadow_memory_write_count = context.shadow_memory_write_count;
    attempt->shadow_memory_read_hit_count =
        context.shadow_memory_read_hit_count;
    attempt->shadow_memory_byte_count =
        static_cast<std::uint32_t>(context.shadow_memory.size());
    attempt->shadow_memory_range_valid = context.shadow_memory_range_valid;
    attempt->shadow_memory_min_address = context.shadow_memory_min_address;
    attempt->shadow_memory_max_address = context.shadow_memory_max_address;
    attempt->handled_fatal_breakpoint_count =
        context.handled_fatal_breakpoint_count;
    attempt->last_fatal_breakpoint_address =
        context.last_fatal_breakpoint_address;
    attempt->last_fatal_message_address =
        context.last_fatal_message_address;
    attempt->last_fatal_message = context.last_fatal_message;
    attempt->fatal_halt_reached = context.fatal_halt_reached;
}

} // namespace repiu::engine
