// Task 759. The execution trampoline's half for the cache execution model
// (Linux x64): the resolver the emitted code's return thunk asks, the entry
// into the cache, and the diagnostics of both. Moved whole from
// execution_trampoline.cpp, where it sat under `#if defined(__x86_64__)`.
#if defined(_WIN32) || !defined(__x86_64__)
#error "the cache execution model needs a Linux x86-64 host"
#endif

#include "execution_trampoline_model.h"

// Task 578. The x64 entry bridge and the dispatch frame its resolver fills.
#include "repiu/platform/linux/x64/linux_x64_aot_dispatch.h"
#include "repiu/platform/linux/x64/linux_x64_guest_entry.h"

#include "repiu/engine/shutdown_recovery_policy.h"
#include "repiu/engine/execution_trampoline.h"
#include "native_fast_path.h"
#include "aot_reentry_memo.h"
#include "repiu/platform/fault_handler.h"
#include "native_linear_span.h"
#include "verified_region_analyzer.h"
#include "native_phase_sampler.h"
#include "repiu/engine/live_telemetry.h"
#include "repiu/platform/virtual_memory.h"
#include "repiu/platform/guest_stack_switch.h"
#include "repiu/platform/host_environment.h"
#include "repiu/platform/host_error_stream.h"
#include "repiu/platform/host_thread.h"
#include "repiu/runtime/execution_timeout.h"
#include "repiu/runtime/timer_safe_point_injection.h"
#include "repiu/runtime/aot_long_mode_compatibility.h"
#include "repiu/platform/thunk_calling_convention.h"
#include "repiu/hle/linexe_call_gate.h"
#include "repiu/hle/glide_hle.h"
#include "repiu/assets/rom_zip_archive.h"
#include "repiu/engine/glide_opengl_backend.h"
#include "repiu/engine/cd_audio_wave_out.h"
#include "repiu/engine/aot_page_coherence.h"
#include "repiu/engine/aot_boundary_provenance.h"
#include "repiu/engine/guest_write_trace.h"
#include "repiu/engine/fault_recovery_provenance.h"
#include "repiu/engine/linux_x64_transfer_failure_provenance.h"
#include "repiu/engine/aot_ff_target_timing.h"
#include "repiu/media/chd_cd_image.h"
#include "repiu/runtime/dos_low_memory.h"
#include "repiu/runtime/selector_table.h"

#include <Zydis.h>

#include <cstddef>
#include <cstring>
#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <optional>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <sstream>
#include <unordered_map>
#include <utility>
#include <vector>

#include "thread_context.h"
#include "linexe_glide_boundary.h"
#include "timer_interrupt_boundary.h"
#include "aot_dbt_dispatch.h"
#include "aot_dbt_glide_gate_dispatch.h"
#include "aot_guard_compare_fault.h"
#include "aot_runtime_dispatch.h"
#include "guest_address_watch.h"
#include "fault_exit_trace.h"
#include "instruction_emulation.h"
#include "interrupt_return.h"
#include "mode16_far_return.h"
#include "mode16_stack_push.h"
#include "dpmi_mscdex_services.h"
#include "bios_keyboard_services.h"
#include "dos_int21_services.h"
#include "guest_memory_access.h"
#include "low_memory_string_access.h"
#include "repiu/platform/win32/win32_thread_api.h"
#include "execution_internal.h"
#include "port_io_emulator.h"
#include "breakpoint_evidence.h"
#include "repiu/engine/exception_rescue_win32.h"
#include "guest_owned_breakpoint.h"
#include "live_telemetry_snapshot.h"
#include "repiu/engine/final_execution_report.h"
#include "repiu/platform/guest_cpu_context.h"
#include "repiu/platform/atomic_ops.h"
#include "repiu/platform/host_time.h"
#include "repiu/platform/safe_memory_copy.h"
#include "repiu/platform/fault_handler.h"
#include "repiu/platform/worker_signal.h"

namespace repiu::engine
{
namespace
{

bool LinuxX64ReturnTraceEnabled()
{
    static const bool enabled = std::getenv("REPIU_LINUX_X64_RETURN_TRACE") != nullptr;
    return enabled;
}

void TraceLinuxX64ReturnResolver(const char* const result,
                                 const std::uint32_t guest_target,
                                 const std::uint32_t cache_address,
                                 const char* const detail = "",
                                 const std::uint32_t producer_tag = 0U,
                                 const std::uint32_t guest_esp = 0U)
{
    if (!LinuxX64ReturnTraceEnabled())
    {
        return;
    }
    std::fprintf(stderr,
                 "[repiu-x64-return] result=%s source=0x%08X cache=0x%08X "
                 "producer=0x%08X guest_esp=0x%08X detail=%s\n",
                 result, static_cast<unsigned>(guest_target),
                 static_cast<unsigned>(cache_address),
                 static_cast<unsigned>(producer_tag),
                 static_cast<unsigned>(guest_esp), detail);
}

std::uint32_t LinuxX64GuestEntryTraceAddress()
{
    static const std::uint32_t address = [] {
        const char* const value =
            std::getenv("REPIU_LINUX_X64_GUEST_ENTRY_TRACE");
        if (value == nullptr || *value == '\0')
        {
            return 0U;
        }
        char* end = nullptr;
        const unsigned long parsed = std::strtoul(value, &end, 0);
        if (end == value || *end != '\0' ||
            parsed > std::numeric_limits<std::uint32_t>::max())
        {
            return 0U;
        }
        return static_cast<std::uint32_t>(parsed);
    }();
    return address;
}

std::uint32_t LinuxX64GuestEntryTraceEndAddress()
{
    static const std::uint32_t address = [] {
        const char* const value =
            std::getenv("REPIU_LINUX_X64_GUEST_ENTRY_TRACE_END");
        if (value == nullptr || *value == '\0')
        {
            return 0U;
        }
        char* end = nullptr;
        const unsigned long parsed = std::strtoul(value, &end, 0);
        if (end == value || *end != '\0' ||
            parsed > std::numeric_limits<std::uint32_t>::max())
        {
            return 0U;
        }
        return static_cast<std::uint32_t>(parsed);
    }();
    return address;
}

const char* LinuxX64FaultKindName(
    const repiu::platform::FaultKind kind)
{
    switch (kind)
    {
        case repiu::platform::FaultKind::kAccessViolation:
            return "access";
        case repiu::platform::FaultKind::kSingleStep:
            return "single-step";
        case repiu::platform::FaultKind::kBreakpoint:
            return "breakpoint";
        case repiu::platform::FaultKind::kIllegalInstruction:
            return "illegal";
        case repiu::platform::FaultKind::kIntegerDivideByZero:
            return "divide-by-zero";
        case repiu::platform::FaultKind::kPrivilegedInstruction:
            return "privileged";
        case repiu::platform::FaultKind::kOther:
            return "other";
    }
    return "unknown";
}

std::uint32_t ResolveLinuxX64GuestEntryAddress(
    ThreadContext* const context,
    const std::uint32_t eip)
{
    if (context == nullptr || context->aot_placement == nullptr ||
        !IsAotCacheAddress(context, eip))
    {
        return eip;
    }
    std::uint32_t guest_address = 0U;
    return FindAotGuestAddress(
               *context->aot_placement, eip, &guest_address)
        ? guest_address : eip;
}

void TraceLinuxX64GuestEntry(
    ThreadContext* const context,
    const std::uint32_t fault_eip,
    const repiu::platform::FaultKind fault_kind,
    const std::uint32_t entry_eip,
    const std::uint32_t exit_eip,
    const std::uint32_t entry_esp,
    const std::uint32_t exit_esp,
    const std::uint32_t exit_eflags)
{
    if (context == nullptr)
    {
        return;
    }
    const std::uint32_t target = LinuxX64GuestEntryTraceAddress();
    if (target == 0U)
    {
        return;
    }
    const std::uint32_t configured_end =
        LinuxX64GuestEntryTraceEndAddress();
    const std::uint32_t range_end = configured_end >= target
        ? configured_end : target;
    const std::uint32_t entry_guest_eip =
        ResolveLinuxX64GuestEntryAddress(context, entry_eip);
    const std::uint32_t exit_guest_eip =
        ResolveLinuxX64GuestEntryAddress(context, exit_eip);
    const bool entry_matched =
        entry_guest_eip >= target && entry_guest_eip <= range_end;
    const bool exit_matched =
        exit_guest_eip >= target && exit_guest_eip <= range_end;
    if (!entry_matched && !exit_matched)
    {
        return;
    }
    static std::atomic<std::uint32_t> trace_count{0U};
    const std::uint32_t sequence =
        trace_count.fetch_add(1U, std::memory_order_relaxed) + 1U;
    if (sequence > 32U)
    {
        return;
    }
    char line[512] = {};
    const int length = std::snprintf(
        line, sizeof(line),
        "[repiu-x64-guest-entry] n=%u target=0x%08X end=0x%08X fault_kind=%s "
        "fault_eip=0x%08X entry_eip=0x%08X entry_guest=0x%08X "
        "exit_eip=0x%08X exit_guest=0x%08X entry_esp=0x%08X "
        "exit_esp=0x%08X "
        "eflags=0x%08X pending=%u legacy=%u exit_site=%s\n",
        static_cast<unsigned>(sequence), static_cast<unsigned>(target),
        static_cast<unsigned>(range_end),
        LinuxX64FaultKindName(fault_kind), static_cast<unsigned>(fault_eip),
        static_cast<unsigned>(entry_eip),
        static_cast<unsigned>(entry_guest_eip),
        static_cast<unsigned>(exit_eip), static_cast<unsigned>(exit_guest_eip),
        static_cast<unsigned>(entry_esp), static_cast<unsigned>(exit_esp),
        static_cast<unsigned>(exit_eflags),
        context->aot_reentry_pending ? 1U : 0U,
        context->aot_legacy_fallback ? 1U : 0U,
        VehExitSiteName(context->last_veh_exit_site));
    if (length > 0)
    {
        repiu::platform::WriteHostErrorStream(
            line,
            static_cast<std::size_t>(length) < sizeof(line)
                ? static_cast<std::size_t>(length)
                : sizeof(line) - 1U);
    }
}

bool LinuxX64ReturnFrameTraceEnabled()
{
    static const bool enabled = [] {
        const char* const value =
            std::getenv("REPIU_LINUX_X64_RETURN_FRAME_TRACE");
        return value != nullptr && std::strcmp(value, "0") != 0;
    }();
    return enabled;
}

void TraceLinuxX64ReturnStackTail(
    const repiu::platform::LinuxX64AotDispatchFrame& frame,
    std::uint32_t sequence);

// Task 616. Capture the two stack layouts that share the x64 return thunk:
// ordinary RET leaves its caller at [ESP] after consuming [ESP-4], while an
// indirect call leaves its pushed fallthrough at [ESP] before resolving the
// loaded target in R14D. This is diagnostics only; it does not choose a path.
void TraceLinuxX64ZeroReturnFrame(
    ThreadContext* const context,
    const repiu::platform::LinuxX64AotDispatchFrame& frame)
{
    if (!LinuxX64ReturnFrameTraceEnabled() || context == nullptr)
    {
        return;
    }
    static std::atomic<std::uint32_t> trace_count{0U};
    const std::uint32_t sequence =
        trace_count.fetch_add(1U, std::memory_order_relaxed) + 1U;
    if (sequence > 8U)
    {
        return;
    }

    std::uint32_t stack_words[4] = {};
    std::uint32_t valid_mask = 0U;
    const std::uint32_t guest_esp = frame.guest.esp;
    const std::uint32_t stack_base = guest_esp >= 8U ? guest_esp - 8U : 0U;
    if (guest_esp >= 8U)
    {
        for (std::uint32_t index = 0U; index < 4U; ++index)
        {
            const std::uint32_t address = stack_base + index * 4U;
            const void* const source = reinterpret_cast<const void*>(
                static_cast<std::uintptr_t>(address));
            if (!IsGuestRangeReadable(context, source, sizeof(std::uint32_t)) ||
                !repiu::platform::CopyMemoryWithoutFaulting(
                    &stack_words[index], source, sizeof(stack_words[index]))
                     .complete)
            {
                continue;
            }
            valid_mask |= 1U << index;
        }
    }
    std::uint32_t source_match_mask = 0U;
    for (std::uint32_t index = 0U; index < 4U; ++index)
    {
        if ((valid_mask & (1U << index)) != 0U &&
            stack_words[index] == frame.guest_source)
        {
            source_match_mask |= 1U << index;
        }
    }
    const std::uint32_t consumed_slot =
        guest_esp >= 4U ? guest_esp - 4U : 0U;
    std::uint32_t stack_write_match_count = 0U;
    for (std::uint32_t index = 0U;
         index < REPIU_LINUX_X64_FRAME_STACK_TRACE_CAPACITY; ++index)
    {
        const auto& record = frame.stack_trace[index];
        if (record.site != 0U && record.guest_esp == consumed_slot)
        {
            ++stack_write_match_count;
        }
    }
    std::uint32_t top_call_source = 0U;
    std::uint32_t top_call_target = 0U;
    std::uint32_t top_call_fallthrough = 0U;
    std::uint32_t top_call_entry_esp = 0U;
    std::uint32_t top_call_origin = 0U;
    if (context->aot_call_depth != 0U)
    {
        const ThreadContext::AotCallFrame& top_call =
            context->aot_call_frames[context->aot_call_depth - 1U];
        top_call_source = top_call.source;
        top_call_target = top_call.target;
        top_call_fallthrough = top_call.fallthrough;
        top_call_entry_esp = top_call.entry_esp;
        top_call_origin = static_cast<std::uint32_t>(top_call.origin);
    }
    std::fprintf(
        stderr,
        "[repiu-x64-return-frame] n=%u source=0x%08X guest_eip=0x%08X "
        "guest_esp=0x%08X eflags=0x%08X continuation=0x%08X "
        "metadata_esp=0x%08X status=0x%08X stack_base=0x%08X valid=0x%X "
        "m8=0x%08X m4=0x%08X p0=0x%08X p4=0x%08X matches=0x%X "
        "producer=%s producer_site=0x%08X "
        "last_indirect=0x%08X/0x%08X last_return=0x%08X/0x%08X "
        "call_depth=%u top_call=0x%08X/0x%08X/0x%08X/0x%08X/%u\n",
        sequence,
        static_cast<unsigned>(frame.guest_source),
        static_cast<unsigned>(frame.guest.eip),
        static_cast<unsigned>(frame.guest.esp),
        static_cast<unsigned>(frame.guest.eflags),
        static_cast<unsigned>(frame.guest_continuation),
        static_cast<unsigned>(frame.guest_metadata_esp),
        static_cast<unsigned>(frame.status),
        static_cast<unsigned>(stack_base),
        static_cast<unsigned>(valid_mask),
        static_cast<unsigned>(stack_words[0]),
        static_cast<unsigned>(stack_words[1]),
        static_cast<unsigned>(stack_words[2]),
        static_cast<unsigned>(stack_words[3]),
        static_cast<unsigned>(source_match_mask),
        (frame.status & 0x80000000U) != 0U ? "indirect-call" : "ret",
        static_cast<unsigned>(frame.status & 0x7FFFFFFFU),
        static_cast<unsigned>(context->aot_last_indirect_source.load(
            std::memory_order_relaxed)),
        static_cast<unsigned>(context->aot_last_indirect_target.load(
            std::memory_order_relaxed)),
        static_cast<unsigned>(context->aot_last_return_source.load(
            std::memory_order_relaxed)),
        static_cast<unsigned>(context->aot_last_return_target.load(
            std::memory_order_relaxed)),
        static_cast<unsigned>(context->aot_call_depth),
        static_cast<unsigned>(top_call_source),
        static_cast<unsigned>(top_call_target),
        static_cast<unsigned>(top_call_fallthrough),
        static_cast<unsigned>(top_call_entry_esp),
        static_cast<unsigned>(top_call_origin));
    std::fprintf(stderr,
                 "[repiu-x64-stack-write] consumed=0x%08X sequence=%u "
                 "matches=%u\n",
                 static_cast<unsigned>(consumed_slot),
                 static_cast<unsigned>(frame.stack_trace_sequence),
                 static_cast<unsigned>(stack_write_match_count));
    for (std::uint32_t index = 0U;
         index < REPIU_LINUX_X64_FRAME_STACK_TRACE_CAPACITY; ++index)
    {
        const auto& record = frame.stack_trace[index];
        if (record.site == 0U || record.guest_esp != consumed_slot)
        {
            continue;
        }
        const char* const writer = record.fallthrough != 0U
            ? "direct-call" : "guest-push";
        std::fprintf(stderr,
                     "[repiu-x64-stack-write] slot=0x%08X index=%u "
                     "writer=%s "
                     "site=0x%08X fallthrough=0x%08X esp=0x%08X "
                     "value=0x%08X\n",
                     static_cast<unsigned>(consumed_slot),
                     static_cast<unsigned>(index),
                     writer,
                     static_cast<unsigned>(record.site),
                     static_cast<unsigned>(record.fallthrough),
                     static_cast<unsigned>(record.guest_esp),
                     static_cast<unsigned>(record.value));
    }
    TraceLinuxX64ReturnStackTail(frame, sequence);
}

// Task 626. Select one resolved return target for a read-only register/frame
// snapshot at the x64 resolver boundary.
std::uint32_t LinuxX64ReturnRegisterTraceAddress()
{
    static const std::uint32_t address = [] {
        const auto setting = repiu::platform::ReadEnvironmentSetting(
            "REPIU_LINUX_X64_RETURN_REG_TRACE", 32U);
        if (!setting.present || setting.too_long || setting.value.empty())
        {
            return 0U;
        }
        char text[33] = {};
        std::memcpy(text, setting.value.data(), setting.value.size());
        char* end = nullptr;
        const unsigned long parsed = std::strtoul(text, &end, 0);
        if (end == text || *end != '\0' ||
            parsed > std::numeric_limits<std::uint32_t>::max())
        {
            return 0U;
        }
        return static_cast<std::uint32_t>(parsed);
    }();
    return address;
}

// Task 628. How many of the most recent guest stack writes to print beside a
// selected return. The slot filter Task 619 added answers "who wrote this
// slot"; it cannot answer "what did the stack do just before this return",
// because the writes immediately before a failure usually land on other slots.
std::uint32_t LinuxX64ReturnStackTailCount()
{
    static const std::uint32_t count = [] {
        const auto setting = repiu::platform::ReadEnvironmentSetting(
            "REPIU_LINUX_X64_RETURN_STACK_TAIL", 32U);
        if (!setting.present || setting.too_long || setting.value.empty())
        {
            return 0U;
        }
        char text[33] = {};
        std::memcpy(text, setting.value.data(), setting.value.size());
        char* end = nullptr;
        const unsigned long parsed = std::strtoul(text, &end, 0);
        if (end == text || *end != '\0')
        {
            return 0U;
        }
        if (parsed > REPIU_LINUX_X64_FRAME_STACK_TRACE_CAPACITY)
        {
            return static_cast<std::uint32_t>(
                REPIU_LINUX_X64_FRAME_STACK_TRACE_CAPACITY);
        }
        return static_cast<std::uint32_t>(parsed);
    }();
    return count;
}

// Task 628. The ring in write order, oldest of the requested window first, so
// a correct return and the failing one that follows it read as one sequence.
void TraceLinuxX64ReturnStackTail(
    const repiu::platform::LinuxX64AotDispatchFrame& frame,
    const std::uint32_t sequence)
{
    const std::uint32_t requested = LinuxX64ReturnStackTailCount();
    if (requested == 0U)
    {
        return;
    }
    const std::uint32_t written = frame.stack_trace_sequence;
    const std::uint32_t printed = std::min(requested, written);
    std::fprintf(stderr,
                 "[repiu-x64-return-stack-tail] n=%u target=0x%08X "
                 "sequence=%u printed=%u\n",
                 static_cast<unsigned>(sequence),
                 static_cast<unsigned>(frame.guest_source),
                 static_cast<unsigned>(written),
                 static_cast<unsigned>(printed));
    for (std::uint32_t offset = printed; offset != 0U; --offset)
    {
        const std::uint32_t record_sequence = written - offset;
        const std::uint32_t index = record_sequence &
            (REPIU_LINUX_X64_FRAME_STACK_TRACE_CAPACITY - 1U);
        const auto& record = frame.stack_trace[index];
        if (record.site == 0U)
        {
            continue;
        }
        const char* const writer = record.fallthrough != 0U
            ? "direct-call" : "guest-push";
        std::fprintf(stderr,
                     "[repiu-x64-return-stack-tail] index=%u writer=%s "
                     "site=0x%08X fallthrough=0x%08X esp=0x%08X "
                     "value=0x%08X\n",
                     static_cast<unsigned>(index), writer,
                     static_cast<unsigned>(record.site),
                     static_cast<unsigned>(record.fallthrough),
                     static_cast<unsigned>(record.guest_esp),
                     static_cast<unsigned>(record.value));
    }
}

void TraceLinuxX64ReturnRegisters(
    ThreadContext* const context,
    const repiu::platform::LinuxX64AotDispatchFrame& frame)
{
    const std::uint32_t watched = LinuxX64ReturnRegisterTraceAddress();
    const bool legacy_match =
        watched != 0U && frame.guest_source == watched;
    const bool transfer_match =
        AotTransferTargetTraceMatches(frame.guest_source);
    if (!legacy_match && !transfer_match)
    {
        return;
    }
    static std::atomic<std::uint32_t> trace_count{0U};
    const std::uint32_t sequence =
        trace_count.fetch_add(1U, std::memory_order_relaxed) + 1U;
    if (sequence > 8U)
    {
        return;
    }

    std::uint32_t stack_words[4] = {};
    std::uint32_t valid_mask = 0U;
    const std::uint32_t guest_esp = frame.guest.esp;
    const std::uint32_t stack_base = guest_esp >= 4U ? guest_esp - 4U : 0U;
    if (context != nullptr && guest_esp >= 4U)
    {
        for (std::uint32_t index = 0U; index < 4U; ++index)
        {
            const std::uint32_t address = stack_base + index * 4U;
            const void* const source = reinterpret_cast<const void*>(
                static_cast<std::uintptr_t>(address));
            if (!IsGuestRangeReadable(context, source, sizeof(std::uint32_t)) ||
                !repiu::platform::CopyMemoryWithoutFaulting(
                    &stack_words[index], source, sizeof(stack_words[index]))
                     .complete)
            {
                continue;
            }
            valid_mask |= 1U << index;
        }
    }

    const bool indirect_call = (frame.status & 0x80000000U) != 0U;
    const std::uint32_t producer = frame.status & 0x7FFFFFFFU;
    std::uint8_t producer_bytes[8] = {};
    std::uint32_t producer_bytes_valid = 0U;
    const void* const producer_pointer = reinterpret_cast<const void*>(
        static_cast<std::uintptr_t>(producer));
    if (context != nullptr &&
        IsGuestRangeReadable(context, producer_pointer,
                             sizeof(producer_bytes)) &&
        repiu::platform::CopyMemoryWithoutFaulting(
            producer_bytes, producer_pointer, sizeof(producer_bytes)).complete)
    {
        producer_bytes_valid = 1U;
    }
    if (transfer_match)
    {
        std::fprintf(
            stderr,
            "[repiu-aot-transfer-target] kind=%s origin=x64-thunk "
            "source=0x%08X target=0x%08X "
            "bytes=%02X%02X%02X%02X%02X%02X%02X%02X valid=%u "
            "esp=0x%08X consumed=0x%08X stack_target=0x%08X "
            "eax=0x%08X ebx=0x%08X ecx=0x%08X edx=0x%08X "
            "esi=0x%08X edi=0x%08X ebp=0x%08X\n",
            indirect_call ? "call" : "return", producer,
            static_cast<unsigned>(frame.guest_source), producer_bytes[0],
            producer_bytes[1], producer_bytes[2], producer_bytes[3],
            producer_bytes[4], producer_bytes[5], producer_bytes[6],
            producer_bytes[7], producer_bytes_valid,
            static_cast<unsigned>(frame.guest.esp),
            static_cast<unsigned>(stack_base),
            static_cast<unsigned>(stack_words[0]),
            static_cast<unsigned>(frame.guest.eax),
            static_cast<unsigned>(frame.guest.ebx),
            static_cast<unsigned>(frame.guest.ecx),
            static_cast<unsigned>(frame.guest.edx),
            static_cast<unsigned>(frame.guest.esi),
            static_cast<unsigned>(frame.guest.edi),
            static_cast<unsigned>(frame.guest.ebp));
    }

    std::fprintf(
        stderr,
        "[repiu-x64-return-reg] n=%u target=0x%08X "
        "edi=0x%08X esi=0x%08X ebx=0x%08X edx=0x%08X "
        "ecx=0x%08X eax=0x%08X ebp=0x%08X eip=0x%08X "
        "esp=0x%08X eflags=0x%08X status=0x%08X "
        "continuation=0x%08X metadata_esp=0x%08X "
        "stack_base=0x%08X valid=0x%X m4=0x%08X m0=0x%08X "
        "p4=0x%08X p8=0x%08X\n",
        sequence,
        static_cast<unsigned>(frame.guest_source),
        static_cast<unsigned>(frame.guest.edi),
        static_cast<unsigned>(frame.guest.esi),
        static_cast<unsigned>(frame.guest.ebx),
        static_cast<unsigned>(frame.guest.edx),
        static_cast<unsigned>(frame.guest.ecx),
        static_cast<unsigned>(frame.guest.eax),
        static_cast<unsigned>(frame.guest.ebp),
        static_cast<unsigned>(frame.guest.eip),
        static_cast<unsigned>(frame.guest.esp),
        static_cast<unsigned>(frame.guest.eflags),
        static_cast<unsigned>(frame.status),
        static_cast<unsigned>(frame.guest_continuation),
        static_cast<unsigned>(frame.guest_metadata_esp),
        static_cast<unsigned>(stack_base),
        static_cast<unsigned>(valid_mask),
        static_cast<unsigned>(stack_words[0]),
        static_cast<unsigned>(stack_words[1]),
        static_cast<unsigned>(stack_words[2]),
        static_cast<unsigned>(stack_words[3]));
    TraceLinuxX64ReturnStackTail(frame, sequence);
    // Task 629. The same map dump the initial and final phases print, taken at
    // this return instead. The failing run dies on SIGSEGV, so the final phase
    // never arrives, and the block that fails has no initial entry to print.
    // Reading the map here is safe for the reason the worker handshake gives:
    // the translation worker only runs while the guest thread is parked, and
    // this is the guest thread.
    if (legacy_match && context != nullptr &&
        context->aot_placement != nullptr)
    {
        TraceAotGuestMapForModel(*context->aot_placement, context->runtime_base,
                         "return-trace");
    }
}

void TraceLinuxX64ReturnStackWriters(
    const repiu::platform::LinuxX64AotDispatchFrame& frame)
{
    if (!LinuxX64ReturnTraceEnabled())
    {
        return;
    }
    const std::uint32_t consumed_slot = frame.guest.esp >= 4U
        ? frame.guest.esp - 4U : 0U;
    std::uint32_t match_count = 0U;
    for (std::uint32_t index = 0U;
         index < REPIU_LINUX_X64_FRAME_STACK_TRACE_CAPACITY; ++index)
    {
        const auto& record = frame.stack_trace[index];
        if (record.site != 0U && record.guest_esp == consumed_slot)
        {
            ++match_count;
        }
    }
    std::fprintf(stderr,
                 "[repiu-x64-return-stack] source=0x%08X "
                 "producer=0x%08X consumed=0x%08X sequence=%u matches=%u\n",
                 static_cast<unsigned>(frame.guest_source),
                 static_cast<unsigned>(frame.status),
                 static_cast<unsigned>(consumed_slot),
                 static_cast<unsigned>(frame.stack_trace_sequence),
                 static_cast<unsigned>(match_count));
    for (std::uint32_t index = 0U;
         index < REPIU_LINUX_X64_FRAME_STACK_TRACE_CAPACITY; ++index)
    {
        const auto& record = frame.stack_trace[index];
        if (record.site == 0U || record.guest_esp != consumed_slot)
        {
            continue;
        }
        const char* const writer = record.fallthrough != 0U
            ? "direct-call" : "guest-push";
        std::fprintf(stderr,
                     "[repiu-x64-return-stack] index=%u writer=%s "
                     "site=0x%08X fallthrough=0x%08X esp=0x%08X "
                     "value=0x%08X\n",
                     static_cast<unsigned>(index), writer,
                     static_cast<unsigned>(record.site),
                     static_cast<unsigned>(record.fallthrough),
                     static_cast<unsigned>(record.guest_esp),
                     static_cast<unsigned>(record.value));
    }
}

// Task 578. Where an x64 host asks how to continue.
//
// The whole question is "where in the cache is this guest address", and the
// engine already answers it for other callers, so this is an adapter rather
// than a mechanism. Answering zero is not a failure path to avoid: Task 562's
// thunk turns it into an INT3, which is the fail-closed boundary that keeps a
// guest address the cache does not hold from continuing anywhere at all.
std::uintptr_t LinuxX64EngineResolver(
    void* resolver_context, repiu::platform::LinuxX64AotDispatchFrame* frame)
{
    auto* const context = static_cast<ThreadContext*>(resolver_context);
    if (context != nullptr)
    {
        context->linux_x64_transfer_failure_provenance = {};
    }
    if (context == nullptr || frame == nullptr ||
        context->aot_placement == nullptr)
    {
        TraceLinuxX64ReturnResolver("invalid-state",
                                   frame != nullptr ? frame->guest_source : 0U,
                                   0U);
        return 0U;
    }
    if (frame->guest_source == 0U)
    {
        TraceLinuxX64ZeroReturnFrame(context, *frame);
    }
    else
    {
        TraceLinuxX64ReturnRegisters(context, *frame);
        TraceLinuxX64ReturnStackWriters(*frame);
    }
    std::uint32_t cache_address = 0U;
    const std::uint64_t dynamic_attempts_before =
        context->aot_dynamic_attempt_count.load(std::memory_order_relaxed);
    if (!ResolveAotTransferTarget(context, frame->guest_source, &cache_address))
    {
        const bool identical_first_instruction =
            CanResumeLinuxX64LegacyTarget(context, frame->guest_source);
        // Task 743. The legacy resume thunk sets TF and jumps, so the #DB
        // names the target before anything there runs; the VEH's single-step
        // path then dispatches the HLE and stack-bridge handlers at that EIP
        // exactly as the indirect-transfer fallback does. Requiring a
        // byte-identical first instruction here left every other
        // untranslatable return target to the thunk's INT3, which nothing
        // handles.
        if (identical_first_instruction ||
            (IsGuestInstructionPointer(context, frame->guest_source) &&
             IsGuestRangeReadable(
                 context,
                 reinterpret_cast<const void*>(
                     static_cast<std::uintptr_t>(frame->guest_source)),
                 15U)))
        {
            frame->guest_continuation = frame->guest_source;
            frame->guest.eip = frame->guest_source;
            context->aot_reentry_pending = false;
            context->aot_legacy_fallback = true;
            context->enable_single_step_trace = true;
            TraceLinuxX64ReturnResolver(
                "legacy-fallback", frame->guest_source,
                static_cast<std::uint32_t>(
                    repiu::platform::LinuxX64LegacyResumeThunkAddress()),
                identical_first_instruction
                    ? "byte-identical first instruction"
                    : "single-step fallback at a non-identical target",
                frame->status, frame->guest.esp);
            return repiu::platform::LinuxX64LegacyResumeThunkAddress();
        }
        const bool attempted_dynamic_translation =
            context->aot_dynamic_attempt_count.load(std::memory_order_relaxed) !=
            dynamic_attempts_before;
        const auto failure_reason = attempted_dynamic_translation
            ? LinuxX64TransferFailureReason::kTranslationFailed
            : LinuxX64TransferFailureReason::kPolicyRefused;
        context->linux_x64_transfer_failure_provenance =
            MakeLinuxX64TransferFailureProvenance(
                frame->status, frame->guest_source, frame->guest.esp,
                failure_reason);
        TraceLinuxX64ReturnResolver(
            attempted_dynamic_translation ? "translation-failed"
                                          : "policy-refused",
            frame->guest_source, 0U,
            attempted_dynamic_translation
                ? context->aot_translation_result.message.c_str()
                : "",
            frame->status, frame->guest.esp);
        return 0U;
    }
    TraceLinuxX64ReturnResolver("resolved", frame->guest_source,
                                cache_address, "", frame->status,
                                frame->guest.esp);
    return static_cast<std::uintptr_t>(cache_address);
}

// Task 578. The third entry path: not the guest's bytes, but the placed cache.
//
// Guest ESP is seeded into R15D because there is no stack switch on x64 to put
// it anywhere else -- the guest's stack and the host's are separate registers
// from the start (Task 546 decision 3). The other guest registers start at zero,
// which is what the i386 direct path effectively gives them too: it calls the
// entry with whatever the ABI left behind and the guest's own prologue sets up
// what it needs.
void CallGuestCacheEntryTimed(ThreadContext* context)
{
    const ExecutionTimeScope guest_run_time_scope(
        context != nullptr ? context->execution_time_profile.get() : nullptr,
        ExecutionTimeBucket::kGuestRunTotal);
    if (context == nullptr || context->aot_placement == nullptr ||
        !context->aot_placement->placed)
    {
        return;
    }
    repiu::platform::LinuxX64AotDispatchFrame frame;
    repiu::platform::InstallLinuxX64Dispatch(&frame, context,
                                             &LinuxX64EngineResolver);
    InstallGlideGateDirectDispatchResolver();
    repiu::platform::LinuxX64GuestEntryState state;
    state.guest_esp = static_cast<std::uint64_t>(context->guest_initial_esp);
    void* const entry = reinterpret_cast<void*>(
        static_cast<std::uintptr_t>(context->aot_placement->entry_address));
    repiu::platform::RepiuLinuxX64GuestEntry(entry, &state);
    repiu::platform::ClearLinuxX64Dispatch();
}

}  // namespace

namespace trampoline_model
{

void EnterGuestWithStack(StackSwitchCallState* state, ThreadContext* context)
{
    // The direct model's; there is no stack switch to make here (Tasks 546
    // and 558).
    (void)state;
    (void)context;
}

void EnterGuestDirect(ThreadContext* context)
{
    // The direct model's; long mode cannot run the guest's own bytes.
    (void)context;
}

void EnterGuestCache(ThreadContext* context)
{
    CallGuestCacheEntryTimed(context);
}

void EnterGuestEmulated(StackSwitchCallState* state, ThreadContext* context)
{
    // The emulated model's.
    (void)state;
    (void)context;
}

bool RequestGuestStop(ThreadContext* context)
{
    // This model interrupts the guest thread instead.
    (void)context;
    return false;
}

std::uintptr_t CacheExitAddress()
{
    return reinterpret_cast<std::uintptr_t>(
        &repiu::platform::RepiuLinuxX64GuestExit);
}

void ReadShutdownRecoveryPosition(const std::uint32_t eip,
                                  void* const host_context,
                                  ShutdownRecoveryPosition* const position)
{
    (void)eip;
    position->host_address_required = true;
    position->host_address =
        repiu::platform::ReadHostInstructionPointer(host_context);
    position->host_address_known = host_context != nullptr;
}

bool RedirectToCacheExit(repiu::platform::GuestCpuContext* const registers,
                         void* const host_context)
{
    // Eip cannot carry the exit's full address, so prime native RIP first and
    // then leave the low half in GuestCpuContext for StoreGuestCpuContext's
    // merge.
    const std::uintptr_t resume_address = CacheExitAddress();
    if (host_context == nullptr ||
        !repiu::platform::StoreHostInstructionPointer(
            resume_address, host_context))
    {
        return false;
    }
    registers->Eip = static_cast<decltype(registers->Eip)>(resume_address);
    registers->EFlags &= ~0x00000100U;
    registers->EFlags &= ~0x00000400U;
    return true;
}

void TraceGuestEntry(ThreadContext* const context,
                     const std::uint32_t fault_eip,
                     const repiu::platform::FaultKind fault_kind,
                     const std::uint32_t entry_eip,
                     const std::uint32_t exit_eip,
                     const std::uint32_t entry_esp,
                     const std::uint32_t exit_esp,
                     const std::uint32_t exit_eflags)
{
    TraceLinuxX64GuestEntry(context, fault_eip, fault_kind, entry_eip,
                            exit_eip, entry_esp, exit_esp, exit_eflags);
}

}  // namespace trampoline_model
}  // namespace repiu::engine
