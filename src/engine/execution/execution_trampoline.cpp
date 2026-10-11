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
#include "repiu/platform/host_fault_report.h"
#include "execution_trampoline_model.h"
#include "repiu/runtime/execution_model.h"
#include "repiu/runtime/env_toggle.h"
#include "repiu/engine/timer_latency_trace.h"
#include "repiu/engine/event_clock.h"
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
#include <chrono>
#include <cmath>
#include <cstdio>
#include <optional>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <sstream>
#include <unordered_map>
#include <utility>
#include <vector>

#if defined(_WIN32)
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>
#endif

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

// Task 743. Defined beside NoteVehExitSite, used from the anonymous namespace.
void ReportLinuxX64UntranslatableContinuation(
    ThreadContext* context, std::uint32_t eip, const char* stage);


void RecordFaultRecoveryProvenance(ThreadContext* context,
                                   std::uint32_t source_eip,
                                   FaultRecoveryPath path);

namespace
{
std::uint32_t AotFaultTraceAddressFilter()
{
    static const std::uint32_t address = [] {
        const char* setting = std::getenv("REPIU_AOT_FAULT_TRACE_ADDRESS");
        if (setting == nullptr || *setting == '\0')
        {
            return 0U;
        }
        char* end = nullptr;
        const unsigned long parsed = std::strtoul(setting, &end, 0);
        if (end == setting || *end != '\0' || parsed > UINT32_MAX)
        {
            return 0U;
        }
        return static_cast<std::uint32_t>(parsed);
    }();
    return address;
}

std::uint32_t AotFaultTraceGuestAddressFilter()
{
    static const std::uint32_t address = [] {
        const char* setting =
            std::getenv("REPIU_AOT_FAULT_TRACE_GUEST_ADDRESS");
        if (setting == nullptr || *setting == '\0')
        {
            return 0U;
        }
        char* end = nullptr;
        const unsigned long parsed = std::strtoul(setting, &end, 0);
        if (end == setting || *end != '\0' || parsed > UINT32_MAX)
        {
            return 0U;
        }
        return static_cast<std::uint32_t>(parsed);
    }();
    return address;
}

// Task 503d-6 wrote this thunk because CreateThread dictated its signature.
// Task 503d-18: the thread layer takes `std::uint32_t(void*)` instead, so the
// shape is the engine's own and the fence around it is gone. What is left is a
// cast from the worker's own return type.
std::uint32_t AotTranslationWorkerThunk(void* parameter)
{
    return static_cast<std::uint32_t>(AotTranslationWorkerProc(parameter));
}
}  // namespace
extern "C" ThreadContext* g_repiu_active_thread_context = nullptr;
namespace
{



bool IsDirectX86ExecutionSupported()
{
// Task 503d-19: the `_WIN32` half is gone. What running the guest's code in
// this process requires is a 32-bit x86 host, and every Win32 API the driver
// behind this used to need is in the platform layer now.
// Task 759: the execution model answers.
    return runtime::execution_model::RunsGuestBytesDirectly();
}

// Task 578. Whether this host enters the guest through the emitted cache.
//
// A different question from the two around it, and deliberately not a
// relaxation of either. `IsDirectX86ExecutionSupported` asks whether the guest's
// own bytes may be jumped at, and on x64 the answer stays no -- long mode reads
// several of those encodings differently (Task 550). What an x64 host can enter
// is the *emitted* long-mode cache, which is this.
//
// `IsGuestStackSwitchSupported` also stays false on x64, and for a reason
// rather than by omission: there is no switch to make. Host RSP stays the SysV
// stack and guest ESP lives in R15D (Tasks 546 and 558), so the two are separate
// to begin with.
bool IsCodeCacheEntrySupported()
{
    return runtime::execution_model::RunsLongModeCodeCache();
}

const char* AotFixupKindName(const runtime::AotFixupKind kind)
{
    switch (kind)
    {
        case runtime::AotFixupKind::kDirectCall: return "direct-call";
        case runtime::AotFixupKind::kDirectJump: return "direct-jump";
        case runtime::AotFixupKind::kBlockFallthrough:
            return "block-fallthrough";
        case runtime::AotFixupKind::kConditionalBranch:
            return "conditional-branch";
        case runtime::AotFixupKind::kHleBoundary: return "hle-boundary";
        case runtime::AotFixupKind::kIndirectExit: return "indirect-exit";
    }
    return "unknown";
}

bool ParseAotGuestMapTraceOffset(const std::string_view token,
                                 std::uint32_t* const offset)
{
    if (offset == nullptr)
    {
        return false;
    }
    std::size_t begin = 0U;
    while (begin < token.size() &&
           std::isspace(static_cast<unsigned char>(token[begin])) != 0)
    {
        ++begin;
    }
    std::size_t end_index = token.size();
    while (end_index > begin &&
           std::isspace(static_cast<unsigned char>(token[end_index - 1U])) != 0)
    {
        --end_index;
    }
    if (begin == end_index || end_index - begin >= 32U)
    {
        return false;
    }
    char text[32] = {};
    std::memcpy(text, token.data() + begin, end_index - begin);
    char* parse_end = nullptr;
    const unsigned long value = std::strtoul(text, &parse_end, 0);
    if (parse_end == text || *parse_end != '\0' || value > UINT32_MAX)
    {
        return false;
    }
    *offset = static_cast<std::uint32_t>(value);
    return true;
}

std::uint32_t AotGuestMapContextRadius()
{
    static const std::uint32_t radius = [] {
        const auto setting = repiu::platform::ReadEnvironmentSetting(
            "REPIU_AOT_GUEST_MAP_CONTEXT", 31U);
        if (!setting.present || setting.too_long || setting.value.empty())
        {
            return 0U;
        }
        char text[32] = {};
        std::memcpy(text, setting.value.data(), setting.value.size());
        char* parse_end = nullptr;
        const unsigned long parsed = std::strtoul(text, &parse_end, 0);
        if (parse_end == text || *parse_end != '\0')
        {
            return 0U;
        }
        return static_cast<std::uint32_t>(std::min(parsed, 64UL));
    }();
    return radius;
}

void TraceAotGuestMapContextEntry(
    const AotCodeCachePlacement& placement,
    const std::size_t index,
    const std::size_t match_index)
{
    const runtime::AotAddressMapEntry& entry = placement.address_map[index];
    const bool emitted_readable =
        placement.placed && placement.base_address != 0U &&
        entry.cache_offset <= placement.size &&
        entry.emitted_length <= placement.size - entry.cache_offset;
    const bool inactive = std::find(
        placement.inactive_map_indices.begin(),
        placement.inactive_map_indices.end(),
        static_cast<std::uint32_t>(index)) !=
        placement.inactive_map_indices.end();
    const std::size_t guest_byte_count = std::min<std::size_t>(
        entry.guest_length, 16U);
    std::uint8_t guest_bytes[16] = {};
    const void* const guest_pointer = reinterpret_cast<const void*>(
        static_cast<std::uintptr_t>(entry.guest_address));
    const bool guest_readable = guest_byte_count != 0U &&
        repiu::platform::CopyMemoryWithoutFaulting(
            guest_bytes, guest_pointer, guest_byte_count).complete;
    std::fprintf(
        stderr,
        "[repiu-aot-map-context] index=%zu relative=%lld guest=0x%08X "
        "cache=0x%08X guest_len=%u emitted_len=%u inactive=%u guest_bytes=",
        index,
        static_cast<long long>(index) - static_cast<long long>(match_index),
        entry.guest_address, placement.base_address + entry.cache_offset,
        static_cast<unsigned>(entry.guest_length),
        static_cast<unsigned>(entry.emitted_length), inactive ? 1U : 0U);
    if (guest_readable)
    {
        for (std::size_t byte_index = 0U;
             byte_index < guest_byte_count; ++byte_index)
        {
            std::fprintf(stderr, "%02X", guest_bytes[byte_index]);
        }
    }
    else
    {
        std::fprintf(stderr, "unreadable");
    }
    std::fprintf(stderr, " emitted_bytes=");
    if (emitted_readable)
    {
        const std::size_t byte_count = std::min<std::size_t>(
            entry.emitted_length, 16U);
        const auto* const emitted_bytes =
            reinterpret_cast<const std::uint8_t*>(
                static_cast<std::uintptr_t>(placement.base_address) +
                entry.cache_offset);
        for (std::size_t byte_index = 0U;
             byte_index < byte_count; ++byte_index)
        {
            std::fprintf(stderr, "%02X", emitted_bytes[byte_index]);
        }
    }
    else
    {
        std::fprintf(stderr, "unreadable");
    }
    std::fprintf(stderr, "\n");
}

void TraceAotGuestMap(const AotCodeCachePlacement& placement,
                      const std::uint32_t runtime_base,
                      const char* const phase)
{
    constexpr std::size_t kEnvironmentCapacity = 1024U;
    constexpr std::size_t kIncomingFixupTraceCapacity = 32U;
    std::size_t incoming_fixups_printed = 0U;
    const auto setting = repiu::platform::ReadEnvironmentSetting(
        "REPIU_AOT_GUEST_MAP_TRACE", kEnvironmentCapacity);
    if (!setting.present)
    {
        return;
    }
    if (setting.too_long)
    {
        std::fprintf(stderr,
                     "[repiu-aot-map] invalid=environment-too-long\n");
        return;
    }

    std::size_t token_begin = 0U;
    while (token_begin <= setting.value.size())
    {
        const std::size_t separator = setting.value.find(',', token_begin);
        const std::size_t token_end = separator == std::string_view::npos
            ? setting.value.size() : separator;
        const std::string_view token = setting.value.substr(
            token_begin, token_end - token_begin);
        std::uint32_t offset = 0U;
        if (!ParseAotGuestMapTraceOffset(token, &offset))
        {
            std::fprintf(stderr,
                         "[repiu-aot-map] token-invalid value='%.*s'\n",
                         static_cast<int>(token.size()), token.data());
        }
        else if (offset > UINT32_MAX - runtime_base)
        {
            std::fprintf(stderr,
                         "[repiu-aot-map] offset-invalid offset=0x%08X "
                         "reason=guest-address-overflow\n",
                         offset);
        }
        else
        {
            const std::uint32_t guest_address = runtime_base + offset;
            std::vector<std::size_t> exact_indices;
            std::vector<std::size_t> covering_indices;
            const runtime::AotAddressMapEntry* previous = nullptr;
            const runtime::AotAddressMapEntry* next = nullptr;
            std::size_t previous_index = 0U;
            std::size_t next_index = 0U;
            for (std::size_t index = 0U;
                 index < placement.address_map.size(); ++index)
            {
                const runtime::AotAddressMapEntry& entry =
                    placement.address_map[index];
                if (entry.guest_address == guest_address)
                {
                    exact_indices.push_back(index);
                }
                const std::uint64_t entry_end =
                    static_cast<std::uint64_t>(entry.guest_address) +
                    entry.guest_length;
                if (static_cast<std::uint64_t>(guest_address) >=
                        entry.guest_address &&
                    static_cast<std::uint64_t>(guest_address) < entry_end)
                {
                    covering_indices.push_back(index);
                }
                if (entry.guest_address < guest_address &&
                    (previous == nullptr ||
                     entry.guest_address > previous->guest_address))
                {
                    previous = &entry;
                    previous_index = index;
                }
                if (entry.guest_address > guest_address &&
                    (next == nullptr ||
                     entry.guest_address < next->guest_address))
                {
                    next = &entry;
                    next_index = index;
                }
            }

            const std::vector<std::size_t>& matches =
                exact_indices.empty() ? covering_indices : exact_indices;
            std::fprintf(stderr,
                         "[repiu-aot-map] offset=0x%08X guest=0x%08X "
                         "phase=%s match=%s matches=%zu map_entries=%zu\n",
                         offset, guest_address,
                         phase == nullptr ? "unknown" : phase,
                         matches.empty() ? "none" :
                             (exact_indices.empty() ? "covering" : "exact"),
                         matches.size(),
                         placement.address_map.size());
            for (const std::size_t match_index : matches)
            {
                const runtime::AotAddressMapEntry& match =
                    placement.address_map[match_index];
                const bool bytes_readable =
                    placement.placed && placement.base_address != 0U &&
                    match.cache_offset <= placement.size &&
                    match.emitted_length <=
                        placement.size - match.cache_offset;
                const bool inactive = std::find(
                    placement.inactive_map_indices.begin(),
                    placement.inactive_map_indices.end(),
                    static_cast<std::uint32_t>(match_index)) !=
                    placement.inactive_map_indices.end();
                std::fprintf(stderr,
                             "[repiu-aot-map-entry] target=0x%08X index=%zu "
                             "guest=0x%08X cache=0x%08X guest_len=%u "
                             "emitted_len=%u inactive=%u bytes=",
                             guest_address, match_index, match.guest_address,
                             placement.base_address + match.cache_offset,
                             static_cast<unsigned>(match.guest_length),
                             static_cast<unsigned>(match.emitted_length),
                             inactive ? 1U : 0U);
                if (bytes_readable)
                {
                    const std::size_t byte_count = std::min<std::size_t>(
                        match.emitted_length, 16U);
                    const auto* bytes = reinterpret_cast<const std::uint8_t*>(
                        static_cast<std::uintptr_t>(placement.base_address) +
                        match.cache_offset);
                    for (std::size_t byte_index = 0U;
                         byte_index < byte_count; ++byte_index)
                    {
                        std::fprintf(stderr, "%02X", bytes[byte_index]);
                    }
                }
                else
                {
                    std::fprintf(stderr, "unreadable");
                }
                std::fprintf(stderr, "\n");
                for (const runtime::AotCodeCacheFixup& fixup :
                     placement.fixups)
                {
                    if (fixup.guest_source == match.guest_address)
                    {
                        std::fprintf(
                            stderr,
                            "[repiu-aot-map-fixup] source=0x%08X "
                            "kind=%s target=0x%08X patch=0x%08X resolved=%u\n",
                            fixup.guest_source, AotFixupKindName(fixup.kind),
                            fixup.guest_target, fixup.cache_patch_offset,
                            fixup.resolved ? 1U : 0U);
                    }
                    if (fixup.guest_target == match.guest_address &&
                        incoming_fixups_printed <
                            kIncomingFixupTraceCapacity)
                    {
                        std::fprintf(
                            stderr,
                            "[repiu-aot-map-incoming-fixup] "
                            "filter=0x%08X source=0x%08X "
                            "target=0x%08X kind=%s patch=0x%08X "
                            "resolved=%u\n",
                            guest_address, fixup.guest_source,
                            fixup.guest_target,
                            AotFixupKindName(fixup.kind),
                            fixup.cache_patch_offset,
                            fixup.resolved ? 1U : 0U);
                        ++incoming_fixups_printed;
                    }
                }
                const std::size_t context_radius =
                    AotGuestMapContextRadius();
                if (context_radius != 0U)
                {
                    const std::size_t first_index =
                        match_index > context_radius
                            ? match_index - context_radius : 0U;
                    const std::size_t last_index = std::min(
                        placement.address_map.size() - 1U,
                        match_index + context_radius);
                    for (std::size_t context_index = first_index;
                         context_index <= last_index; ++context_index)
                    {
                        TraceAotGuestMapContextEntry(
                            placement, context_index, match_index);
                    }
                }
            }
            if (matches.empty())
            {
                if (previous != nullptr)
                {
                    std::fprintf(stderr,
                                 "[repiu-aot-map-neighbor] side=previous "
                                 "index=%zu guest=0x%08X guest_len=%u\n",
                                 previous_index, previous->guest_address,
                                 static_cast<unsigned>(previous->guest_length));
                }
                if (next != nullptr)
                {
                    std::fprintf(stderr,
                                 "[repiu-aot-map-neighbor] side=next "
                                 "index=%zu guest=0x%08X guest_len=%u\n",
                                 next_index, next->guest_address,
                                 static_cast<unsigned>(next->guest_length));
                }
            }
        }
        if (separator == std::string_view::npos)
        {
            break;
        }
        token_begin = separator + 1U;
    }
}

void TraceAotCacheMap(const AotCodeCachePlacement& placement,
                      const char* const phase)
{
    const auto setting = repiu::platform::ReadEnvironmentSetting(
        "REPIU_AOT_CACHE_MAP_TRACE", 31U);
    if (!setting.present)
    {
        return;
    }
    std::uint32_t cache_address = 0U;
    if (setting.too_long ||
        !ParseAotGuestMapTraceOffset(setting.value, &cache_address))
    {
        std::fprintf(stderr, "[repiu-aot-cache-map] invalid=address\n");
        return;
    }
    if (!placement.placed || cache_address < placement.base_address ||
        cache_address - placement.base_address >= placement.size)
    {
        std::fprintf(stderr,
                     "[repiu-aot-cache-map] cache=0x%08X phase=%s "
                     "in_range=0\n",
                     cache_address,
                     phase == nullptr ? "unknown" : phase);
        return;
    }

    const std::uint32_t previous_address =
        cache_address == placement.base_address
            ? cache_address : cache_address - 1U;
    std::uint32_t guest_address = 0U;
    std::uint32_t previous_guest_address = 0U;
    const bool mapped = FindAotGuestAddress(
        placement, cache_address, &guest_address);
    const bool previous_mapped = FindAotGuestAddress(
        placement, previous_address, &previous_guest_address);
    const AotCacheBreakpointProvenance provenance =
        ClassifyAotCacheBreakpointProvenance(
            placement, cache_address, false);
    const AotCacheBreakpointProvenance previous_provenance =
        ClassifyAotCacheBreakpointProvenance(
            placement, previous_address, false);
    const auto* const bytes = reinterpret_cast<const std::uint8_t*>(
        static_cast<std::uintptr_t>(placement.base_address));
    const std::uint32_t offset = cache_address - placement.base_address;
    const std::uint32_t previous_offset =
        previous_address - placement.base_address;
    const std::uint32_t first_offset = offset > 8U ? offset - 8U : 0U;
    const std::uint32_t byte_count = std::min<std::uint32_t>(
        17U, placement.size - first_offset);

    std::fprintf(
        stderr,
        "[repiu-aot-cache-map] cache=0x%08X phase=%s in_range=1 "
        "byte=0x%02X previous=0x%08X previous_byte=0x%02X "
        "mapped=%u guest=0x%08X previous_mapped=%u previous_guest=0x%08X "
        "provenance=%u previous_provenance=%u bytes_start=0x%08X bytes=",
        cache_address, phase == nullptr ? "unknown" : phase,
        static_cast<unsigned>(bytes[offset]), previous_address,
        static_cast<unsigned>(bytes[previous_offset]), mapped ? 1U : 0U,
        guest_address, previous_mapped ? 1U : 0U, previous_guest_address,
        static_cast<unsigned>(provenance),
        static_cast<unsigned>(previous_provenance),
        placement.base_address + first_offset);
    for (std::uint32_t index = 0U; index < byte_count; ++index)
    {
        std::fprintf(stderr, "%02X", bytes[first_offset + index]);
    }
    std::fprintf(stderr, "\n");
}

bool IsGuestStackSwitchSupported()
{
// Task 503d-19: what this asks is whether the stack switch exists, and since
// 3d-16 wrote it in GAS the answer no longer depends on the compiler. It
// depends on the architecture, because the switch is 32-bit x86 assembly.
// Task 759: which is the direct execution model.
    return runtime::execution_model::RunsGuestBytesDirectly();
}

// Task 503d-15. What used to be one `#if defined(_WIN32)` over the next two
// thousand lines came from Task 233's file decomposition rather than from this
// port. Measuring behind it found thirteen errors, so the fence was far wider
// than what it protected. It is gone; the pieces that are genuinely Windows
// carry their own fence and say why.
// Task 503d-16: the recovery globals moved to
// `repiu/platform/guest_stack_switch.h`. They had internal linkage here, which
// was enough while the only reader was MSVC inline assembly in this same file;
// the GAS counterparts are a separate object and need a symbol to reach.


// Task 503d-16. One host entry appended in the shape DOS keeps its block in:
// the name upper-cased, the value untouched, NUL-terminated.
//
// The name ends at the first `=`, and an entry that begins with one therefore
// has an empty name and is copied verbatim. That is not a degenerate case to
// guard against -- `cmd.exe` records each drive's current directory as `=C:`,
// and this block has always carried them across.
void AppendDosEnvironmentEntry(const char* entry, void* user_data)
{
    auto& block = *static_cast<std::vector<std::uint8_t>*>(user_data);
    bool before_equals = true;
    for (const char* current = entry; *current != 0; ++current)
    {
        unsigned char byte = static_cast<unsigned char>(*current);
        if (before_equals && byte == '=')
        {
            before_equals = false;
        }
        else if (before_equals)
        {
            byte = static_cast<unsigned char>(std::toupper(byte));
        }
        block.push_back(static_cast<std::uint8_t>(byte));
    }
    block.push_back(0);
}

// Task 503d-16: the enumeration moved to the platform layer, so what stays here
// is only the DOS shape. 3d-15 fenced this whole body to Windows, which left
// Linux building an empty block -- invisible while nothing linked this file,
// and wrong the moment something did.
std::vector<std::uint8_t> BuildDosEnvironmentBlock()
{
    std::vector<std::uint8_t> block;
    repiu::platform::ForEachEnvironmentEntry(&AppendDosEnvironmentEntry,
                                             &block);

    // A DOS environment block ends with an empty entry, so the terminator is
    // two NULs -- one closing the last entry and one standing for the empty
    // one. An environment with no entries at all still needs both.
    if (block.empty() || block.back() != 0)
    {
        block.push_back(0);
    }
    block.push_back(0);
    return block;
}

int CaptureException(const repiu::platform::FaultEvent& fault,
                     ThreadContext* context)
{
    if (fault.registers != nullptr && context != nullptr)
    {
        // Task 503d-15, Task 759. A language exception of the host's own (an
        // MSVC C++ throw) is reported by the platform layer; a guest fault
        // never is one.
        repiu::platform::ReportHostLanguageException(
            fault.host_code,
            reinterpret_cast<const void*>(
                static_cast<std::uintptr_t>(fault.instruction_address)));
        context->exception_caught = true;
        context->exception_code =
            fault.host_code;
        context->exception_address = static_cast<std::uint32_t>(
            static_cast<std::uintptr_t>(fault.instruction_address));
        const void* instruction_bytes = reinterpret_cast<const void*>(
            static_cast<std::uintptr_t>(fault.instruction_address));
        const repiu::platform::MemoryRegion instruction_page =
            repiu::platform::QueryMemory(instruction_bytes);
        if (instruction_page.valid)
        {
            std::uint8_t bytes[16] = {};
            std::memcpy(bytes, instruction_bytes, sizeof(bytes));
            // Task 503d-15: the protection prints as what it permits rather
            // than as a host protection number. What a reader of this line
            // wants is whether the faulting instruction sat on a page it could
            // execute, and that is a question either host can answer.
            fprintf(stderr,
                    "[repiu-live-debug] exception instruction region "
                    "base=0x%p alloc=0x%p size=0x%zX access=%c%c%c bytes=",
                    instruction_page.base,
                    instruction_page.allocation_base,
                    instruction_page.size,
                    instruction_page.readable ? 'r' : '-',
                    instruction_page.writable ? 'w' : '-',
                    instruction_page.executable ? 'x' : '-');
            for (std::uint8_t byte : bytes)
            {
                fprintf(stderr, "%02X", byte);
            }
            fprintf(stderr, "\n");
        }
        if (fault.access.valid)
        {
            // The recorded access kind keeps the host's own numbering, since
            // this is crash-report detail rather than something decided on:
            // 1 for a write, 8 for an instruction fetch, 0 otherwise.
            context->exception_access_kind = fault.access.write_access ? 1U
                : fault.access.execute_access ? 8U
                                              : 0U;
            context->exception_fault_va = fault.access.fault_address;
            const void* fault_address = reinterpret_cast<const void*>(
                static_cast<std::uintptr_t>(context->exception_fault_va));
            const repiu::platform::MemoryRegion fault_page =
                repiu::platform::QueryMemory(fault_address);
            if (fault_page.valid)
            {
                context->exception_fault_region_base =
                    static_cast<std::uint32_t>(
                        reinterpret_cast<std::uintptr_t>(fault_page.base));
                context->exception_fault_alloc_base =
                    static_cast<std::uint32_t>(
                        reinterpret_cast<std::uintptr_t>(
                            fault_page.allocation_base));
                context->exception_fault_region_size =
                    static_cast<std::uint32_t>(fault_page.size);
                // Task 503d-15. These two keep the host numbering, because the
                // crash report prints them as hex for a person who reads them
                // as Windows constants. Rounding them to the neutral triple
                // would change what an existing report means, so Linux leaves
                // them zero rather than putting different numbers in the same
                // fields.
                std::uint32_t host_state = 0U;
                std::uint32_t host_protect = 0U;
                if (repiu::platform::QueryHostMemoryNumbers(
                        fault_address, &host_state, &host_protect))
                {
                    context->exception_fault_state = host_state;
                    context->exception_fault_protect = host_protect;
                }
            }
        }
        // Task 759. The registers and the memory they point at, where the
        // host's report carries them (Windows; Task 503d-15).
        if (repiu::platform::FillsFaultReportDetail())
        {
            CopySnapshotFromContextRecord(*fault.registers,
                                          &context->exception_snapshot);
            context->exception_eax = fault.registers->Eax;
            context->exception_ebx = fault.registers->Ebx;
            context->exception_ecx = fault.registers->Ecx;
            context->exception_edx = fault.registers->Edx;
            context->exception_esi = fault.registers->Esi;
            context->exception_edi = fault.registers->Edi;
            for (std::uint32_t index = 0; index < 8U; ++index)
            {
                const std::uintptr_t source =
                    static_cast<std::uintptr_t>(
                        fault.registers->Esi) +
                    0x20U + index * 4U;
                std::size_t copied = 0;
                if (repiu::platform::ReadMemoryForFaultReport(
                        reinterpret_cast<const void*>(source),
                        &context->exception_esi_dwords[index],
                        sizeof(std::uint32_t), &copied) &&
                    copied == sizeof(std::uint32_t))
                {
                    context->exception_esi_dword_valid_mask |= 1U << index;
                }
            }
            // Capture the ASCII string that each GPR points at (up to 32 bytes).
            // Null-pointer and string frontiers (e.g. a stricmp fed a filename with
            // no extension) are diagnosed by seeing the actual string a register
            // holds; ESI in particular keeps the callee-saved source pointer.
            const std::uint32_t exception_register_values[6] = {
                fault.registers->Eax,
                fault.registers->Ebx,
                fault.registers->Ecx,
                fault.registers->Edx,
                fault.registers->Esi,
                fault.registers->Edi,
            };
            for (std::uint32_t reg = 0; reg < 6U; ++reg)
            {
                std::size_t copied = 0;
                if (exception_register_values[reg] != 0 &&
                    repiu::platform::ReadMemoryForFaultReport(
                        reinterpret_cast<const void*>(
                            static_cast<std::uintptr_t>(
                                exception_register_values[reg])),
                        context->exception_register_strings[reg],
                        sizeof(context->exception_register_strings[reg]),
                        &copied) &&
                    copied != 0)
                {
                    context->exception_register_string_valid_mask |= 1U << reg;
                }
            }
            // Capture a window of the guest stack starting at the fault-time ESP.
            // Arguments passed to the faulting function sit above ESP; the caller
            // return address lives just below the lowest argument slot. Reading
            // this window is what lets a terminal fault's wild-pointer argument be
            // traced back to the caller that supplied it.
            context->exception_stack_base =
                static_cast<std::uint32_t>(fault.registers->Esp);
            context->exception_stack_dword_count = 0;
            for (std::uint32_t index = 0;
                 index < kExceptionStackDwordCapacity; ++index)
            {
                const std::uintptr_t source =
                    static_cast<std::uintptr_t>(context->exception_stack_base) +
                    index * 4U;
                std::size_t copied = 0;
                if (!repiu::platform::ReadMemoryForFaultReport(
                        reinterpret_cast<const void*>(source),
                        &context->exception_stack_dwords[index],
                        sizeof(std::uint32_t), &copied) ||
                    copied != sizeof(std::uint32_t))
                {
                    break;
                }
                context->exception_stack_dword_count = index + 1U;
            }
        }
    }

    // Task 503d-15: the return value is an SEH filter code, which only
    // means anything to the __except that calls this. Elsewhere the
    // handler decides with a FaultDisposition and never reads it.
    return repiu::platform::kFaultFilterRunsHandler;
}

// Task 503d-15. The three hand-written assembly entries, declared out here so
// the C++ that calls them compiles on both hosts. Their definitions below are
// MSVC inline assembly and stay fenced; the GAS counterparts are the next
// assembly step, exactly as the five dispatch thunks were declared before
// 3d-12 wrote them. Nothing links this file on Linux yet, so an undefined
// symbol here is a marker of what is owed rather than a cost.
extern "C" std::uint32_t REPIU_THUNK_RESOLVER_CALL CallGuestEntryWithStack(
    StackSwitchCallState* state);
extern "C" void REPIU_THUNK_RESOLVER_CALL RecoverGuestStackException();
extern "C" void RecoverHostStackException();

// Task 503d-16. The offsets are checked on both hosts, not just where the MSVC
// assembly reads them, because the GAS entries read the same header and this is
// what ties its numbers to the structure. The last four were never asserted
// even though the assembly has always used them.
static_assert(offsetof(StackSwitchCallState, entry_address) ==
              REPIU_STACK_SWITCH_ENTRY_ADDRESS);
static_assert(offsetof(StackSwitchCallState, initial_esp) ==
              REPIU_STACK_SWITCH_INITIAL_ESP);
static_assert(offsetof(StackSwitchCallState, host_esp) ==
              REPIU_STACK_SWITCH_HOST_ESP);
static_assert(offsetof(StackSwitchCallState, guest_return_esp) ==
              REPIU_STACK_SWITCH_GUEST_RETURN_ESP);
static_assert(offsetof(StackSwitchCallState, result_code) ==
              REPIU_STACK_SWITCH_RESULT_CODE);
static_assert(offsetof(StackSwitchCallState, enable_single_step_trace) ==
              REPIU_STACK_SWITCH_SINGLE_STEP);
static_assert(offsetof(StackSwitchCallState, host_fs) ==
              REPIU_STACK_SWITCH_HOST_FS);
static_assert(offsetof(StackSwitchCallState, host_ds) ==
              REPIU_STACK_SWITCH_HOST_DS);
static_assert(offsetof(StackSwitchCallState, host_es) ==
              REPIU_STACK_SWITCH_HOST_ES);
static_assert(offsetof(StackSwitchCallState, host_gs) ==
              REPIU_STACK_SWITCH_HOST_GS);
static_assert(offsetof(StackSwitchCallState, host_ss) ==
              REPIU_STACK_SWITCH_HOST_SS);
static_assert(offsetof(StackSwitchCallState, guest_stack_base) ==
              REPIU_STACK_SWITCH_GUEST_STACK_BASE);
static_assert(offsetof(StackSwitchCallState, guest_stack_limit) ==
              REPIU_STACK_SWITCH_GUEST_STACK_LIMIT);
static_assert(offsetof(StackSwitchCallState, host_stack_base) ==
              REPIU_STACK_SWITCH_HOST_STACK_BASE);
static_assert(offsetof(StackSwitchCallState, host_stack_limit) ==
              REPIU_STACK_SWITCH_HOST_STACK_LIMIT);

// Task 759. The stack switch and its recovery points are the platform layer's:
// src/platform/win32/guest_stack_switch_win32.cpp on Win32, and
// src/platform/linux/x86/guest_stack_switch.S on Linux i386.

void RecoverToHost(repiu::platform::GuestCpuContext* context, ThreadContext* thread_context)
{
    // Task 503d-15: the casts below ask the fields what they are, because
    // DWORD is `unsigned long` on Windows and the neutral structure says
    // `std::uint32_t`, which are the same width and not the same type.
    using RegisterField = decltype(context->Eip);
    context->Eip = static_cast<RegisterField>(
        reinterpret_cast<std::uintptr_t>(&RecoverGuestStackException));
    context->EFlags &= ~0x00000100U;
    context->EFlags &= ~0x00000400U;
    if (thread_context->active_call_state != nullptr)
    {
        context->Ecx = static_cast<RegisterField>(
            reinterpret_cast<std::uintptr_t>(thread_context->active_call_state));
        context->SegFs = static_cast<RegisterField>(
            thread_context->active_call_state->host_fs);
        context->SegDs = static_cast<RegisterField>(
            thread_context->active_call_state->host_ds);
        context->SegEs = static_cast<RegisterField>(
            thread_context->active_call_state->host_es);
        context->SegGs = static_cast<RegisterField>(
            thread_context->active_call_state->host_gs);
        context->SegSs = static_cast<RegisterField>(
            thread_context->active_call_state->host_ss);
    }
    std::uint32_t host_esp = thread_context->host_esp;
    if (host_esp == 0 && thread_context->active_call_state != nullptr)
    {
        host_esp = thread_context->active_call_state->host_esp;
    }
    thread_context->host_esp = host_esp;
    context->Esp = host_esp;
}


bool HandlePrivilegedTrapInstruction(repiu::platform::GuestCpuContext* win32_context,
                                     ThreadContext* context);
bool HandleAsciiAdjustInstruction(repiu::platform::GuestCpuContext* win32_context,
                                  ThreadContext* context);
bool HandleSelectorLimitInstruction(repiu::platform::GuestCpuContext* win32_context,
                                    ThreadContext* context);
struct AotPlacementPlan;
bool FindAotGuestAddress(const AotPlacementPlan& placement,
                         std::uint32_t host_address,
                         std::uint32_t* guest_address);

bool HandleSelectorLimitInstruction(repiu::platform::GuestCpuContext* win32_context,
                                    ThreadContext* context)
{
    if (win32_context == nullptr || context == nullptr ||
        !IsGuestRangeReadable(context, reinterpret_cast<const void*>(static_cast<std::uintptr_t>(win32_context->Eip)), 3U))
    {
        return false;
    }
    const std::uint8_t* instruction = reinterpret_cast<const std::uint8_t*>(
        win32_context->Eip);
    if (instruction[0] != 0x0F ||
        (instruction[1] != 0x02 && instruction[1] != 0x03))
    {
        return false;
    }
    const std::uint8_t modrm = instruction[2];
    const std::uint8_t mod = (modrm >> 6) & 0x03U;
    const std::uint8_t destination_register = (modrm >> 3) & 0x07U;
    std::uint16_t selector = 0;
    std::uint32_t instruction_size = 3;
    if (mod == 0x03U)
    {
        selector = ReadRegister16(*win32_context, modrm & 0x07U);
    }
    else
    {
        std::uint32_t source = 0;
        std::uint32_t unprefixed_size = 0;
        if (!DecodeModRmMemoryAddress(win32_context,
                                      instruction + 1,
                                      &source,
                                      &unprefixed_size))
        {
            return false;
        }
        const void* source_pointer = reinterpret_cast<const void*>(
            static_cast<std::uintptr_t>(source));
        if (!IsGuestRangeReadable(context, source_pointer, sizeof(selector)))
        {
            return false;
        }
        std::memcpy(&selector, source_pointer, sizeof(selector));
        instruction_size = 1U + unprefixed_size;
    }

    constexpr std::uint32_t kZeroFlag = 0x00000040U;
    const repiu::runtime::GuestDescriptor* descriptor =
        repiu::runtime::FindDescriptor(context->selector_table, selector);
    if (descriptor != nullptr && descriptor->present)
    {
        const std::uint32_t value = instruction[1] == 0x03
            ? descriptor->limit
            : (descriptor->flags & 0xFFFFU) << 8;
        WriteGeneralRegister32(win32_context, destination_register, value);
        win32_context->EFlags |= kZeroFlag;
    }
    else
    {
        win32_context->EFlags &= ~kZeroFlag;
    }
    win32_context->Eip += instruction_size;
    return true;
}



bool HandleGuestLowMemoryReadFault(repiu::platform::GuestCpuContext* win32_context,
                                   ThreadContext* context,
                                   std::uint32_t fault_va,
                                   std::uint32_t decode_eip);
bool HandleDosMemoryAccess(repiu::platform::GuestCpuContext* win32_context,
                           ThreadContext* context);

std::uint32_t DrainFollowingLegacyStackInstructions(
    repiu::platform::GuestCpuContext* win32_context,
    ThreadContext* context)
{
    // Task 759. The cache model's alone: original PUSH/POP bytes would use
    // the host's stack pointer in long mode.
    constexpr std::uint32_t kMaximumFollowingStackInstructions = 16U;
    if (runtime::execution_model::RunsLongModeCodeCache() && context != nullptr)
    {
        return HandleConsecutiveLegacyStackInstructions(
            win32_context, context, kMaximumFollowingStackInstructions);
    }
    return 0U;
}



// Shared guest-instruction HLE dispatch (Task 266). Runs the same handler chain
// the single-step path uses to emulate one sensitive guest instruction at the
// current EIP, advancing EIP past it, WITHOUT touching the trap flag. Callers
// decide whether to re-arm single-step (HandleSingleStepTrace) or stay native
// (the region executor). Mirrors the former inline chain in HandleSingleStepTrace.
bool DispatchGuestHleHandlers(repiu::platform::GuestCpuContext* win32_context, ThreadContext* context)
{
    constexpr std::uint32_t kMaximumX86InstructionBytes = 15U;
    if (win32_context == nullptr || context == nullptr ||
        !IsGuestRangeReadable(
            context,
            reinterpret_cast<const void*>(
                static_cast<std::uintptr_t>(win32_context->Eip)),
            kMaximumX86InstructionBytes))
    {
        return false;
    }

    if (context->enable_segment_load_hle)
    {
        const std::optional<bool> mode16_push =
            HandleMode16StackPush(win32_context, context);
        if (mode16_push.has_value())
        {
            return *mode16_push;
        }
    }

    const std::uint8_t* ptr = reinterpret_cast<const std::uint8_t*>(
        static_cast<std::uintptr_t>(win32_context->Eip));
    std::uint32_t offset = 0U;
    if (ptr[0] == 0x66U || ptr[0] == 0x67U)
    {
        offset = 1U;
    }
    const std::uint8_t opcode = ptr[offset];
    const std::uint8_t second_opcode = offset == 0U ? ptr[1] : 0U;
    if (runtime::execution_model::RunsLongModeCodeCache())
    {
        if (context->aot_legacy_fallback && offset == 0U && opcode == 0xE8U &&
            HandleAotLegacyDirectCall(win32_context, context))
        {
            return true;
        }
        // Original 32-bit PUSH/POP bytes would silently use host RSP in long mode.
        // Only legacy fallback reaches original bytes; emitted cache code already
        // lowers these operations to the R15D guest stack pointer.
        if (context->aot_legacy_fallback && offset == 0U &&
            opcode >= 0x50U && opcode <= 0x5FU &&
            HandleGeneralRegisterStackInstruction(win32_context, context))
        {
            DrainFollowingLegacyStackInstructions(win32_context, context);
            return true;
        }
    }
    const bool segment_stack_candidate =
        (opcode == 0x06U || opcode == 0x0EU || opcode == 0x16U ||
         opcode == 0x1EU || opcode == 0x07U || opcode == 0x1FU ||
         (opcode == 0x0FU &&
          (second_opcode == 0xA0U || second_opcode == 0xA1U ||
           second_opcode == 0xA8U || second_opcode == 0xA9U)));
    const char* const segment_hle_trace_value =
        std::getenv("REPIU_SEGMENT_HLE_TRACE");
    const bool segment_hle_trace_enabled =
        segment_hle_trace_value != nullptr &&
        std::strcmp(segment_hle_trace_value, "0") != 0;
    static std::atomic<std::uint32_t> segment_hle_trace_count{0U};
    const std::uint32_t segment_hle_trace_index =
        segment_hle_trace_enabled && segment_stack_candidate
            ? segment_hle_trace_count.fetch_add(
                  1U, std::memory_order_relaxed) + 1U
            : 0U;
    if (segment_hle_trace_index != 0U && segment_hle_trace_index <= 16U)
    {
        std::fprintf(
            stderr,
            "[repiu-segment-hle] stage=shared-dispatch n=%u eip=0x%08X "
            "opcode=%02X second=%02X offset=%u enabled=%u cache_active=%u "
            "call_state=%u pending=%u legacy=%u esp=0x%08X es=0x%04X\n",
            segment_hle_trace_index,
            static_cast<std::uint32_t>(win32_context->Eip),
            static_cast<unsigned>(opcode),
            static_cast<unsigned>(second_opcode),
            static_cast<unsigned>(offset),
            context->enable_segment_load_hle ? 1U : 0U,
            context->cache_entry_active ? 1U : 0U,
            context->active_call_state != nullptr ? 1U : 0U,
            context->aot_reentry_pending ? 1U : 0U,
            context->aot_legacy_fallback ? 1U : 0U,
            static_cast<std::uint32_t>(win32_context->Esp),
            static_cast<unsigned>(context->guest_es));
    }

    // Opcode-directed Fast Dispatcher (Task 312)
    switch (opcode)
    {
        case 0xC8U:
            if (offset == 0U && HandleEnterInstruction(win32_context, context))
            {
                return true;
            }
            break;
        case 0xECU: case 0xEDU: case 0xEEU: case 0xEFU:
            if (context->enable_privileged_trap_hle && HandlePortIoInstruction(win32_context, context)) return true;
            break;
        case 0x8EU:
            if (context->enable_segment_load_hle && HandleSegmentLoadInstruction(win32_context, context)) return true;
            break;
        case 0x8CU:
            if (context->enable_segment_load_hle && HandleSegmentStoreInstruction(win32_context, context)) return true;
            break;
        case 0x06U: case 0x16U: case 0x1EU:
            if (context->enable_segment_load_hle)
            {
                const bool handled =
                    HandleSegmentPushInstruction(win32_context, context);
                const std::uint32_t drained = handled
                    ? DrainFollowingLegacyStackInstructions(
                          win32_context, context)
                    : 0U;
                if (segment_hle_trace_index != 0U &&
                    segment_hle_trace_index <= 16U)
                {
                    const auto* const next =
                        reinterpret_cast<const std::uint8_t*>(
                            static_cast<std::uintptr_t>(win32_context->Eip));
                    const bool next_readable = handled &&
                        IsGuestRangeReadable(context, next, 6U);
                    std::fprintf(
                        stderr,
                        "[repiu-segment-hle] stage=shared-handler n=%u "
                        "handled=%u drained=%u eip_after=0x%08X esp_after=0x%08X "
                        "next=%02X%02X%02X%02X%02X%02X\n",
                        segment_hle_trace_index,
                        handled ? 1U : 0U,
                        drained,
                        static_cast<std::uint32_t>(win32_context->Eip),
                        static_cast<std::uint32_t>(win32_context->Esp),
                        next_readable ? next[0] : 0U,
                        next_readable ? next[1] : 0U,
                        next_readable ? next[2] : 0U,
                        next_readable ? next[3] : 0U,
                        next_readable ? next[4] : 0U,
                        next_readable ? next[5] : 0U);
                }
                if (handled) return true;
            }
            break;
        case 0x07U: case 0x1FU:
            if (context->enable_segment_load_hle &&
                HandleSegmentPopInstruction(win32_context, context))
            {
                DrainFollowingLegacyStackInstructions(win32_context, context);
                return true;
            }
            break;
        case 0x0FU:
            if (context->enable_segment_load_hle)
            {
                const bool handled =
                    HandleSegmentPushInstruction(win32_context, context) ||
                    HandleSegmentPopInstruction(win32_context, context);
                const std::uint32_t drained = handled
                    ? DrainFollowingLegacyStackInstructions(
                          win32_context, context)
                    : 0U;
                if (segment_hle_trace_index != 0U &&
                    segment_hle_trace_index <= 16U)
                {
                    std::fprintf(
                        stderr,
                        "[repiu-segment-hle] stage=shared-handler n=%u "
                        "handled=%u drained=%u eip_after=0x%08X esp_after=0x%08X\n",
                        segment_hle_trace_index,
                        handled ? 1U : 0U,
                        drained,
                        static_cast<std::uint32_t>(win32_context->Eip),
                        static_cast<std::uint32_t>(win32_context->Esp));
                }
                if (handled) return true;
            }
            break;
        case 0xEAU:
            if (HandleLinexeFarTransferBoundary(win32_context, context))
            {
                return true;
            }
            if (context->enable_segment_load_hle && HandleFarJumpInstruction(win32_context, context)) return true;
            break;
        case 0xCBU:
            if (context->enable_segment_load_hle)
            {
                const std::optional<bool> mode16_return =
                    HandleMode16FarReturn(win32_context, context);
                if (mode16_return.has_value()) return *mode16_return;
                if (HandleFarReturnInstruction(win32_context, context)) return true;
            }
            break;
        case 0xCDU:
            if (context->enable_traced_dos_hle &&
                (HandleTracedDosInterrupt21(win32_context, context) ||
                 HandleTracedDosInterrupt2F(win32_context, context) ||
                 HandleTracedDpmiInterrupt31(win32_context, context) ||
                 HandleTracedMouseInterrupt33(win32_context, context) ||
                 HandleTracedBiosInterrupt16(win32_context, context))) return true;
            break;
        case 0xCFU:
        {
            const std::optional<bool> iretd =
                HandleIretdInstruction(win32_context, context);
            if (iretd.has_value()) return *iretd;
            break;
        }
        case 0xFAU: case 0xFBU:
            if (context->enable_privileged_trap_hle && HandlePrivilegedTrapInstruction(win32_context, context)) return true;
            break;
        case 0xD4U: case 0xD5U:
            if (HandleAsciiAdjustInstruction(win32_context, context)) return true;
            break;
        case 0xABU:
            if (context->enable_segment_load_hle && HandleRepStosdInstruction(win32_context, context)) return true;
            break;
        case 0xA4U: case 0xA5U:
            if (context->enable_segment_load_hle && HandleRepMovsInstruction(win32_context, context)) return true;
            break;
        case 0x64U: case 0x65U: case 0x26U: case 0x2EU: case 0x36U: case 0x3EU:
            if (context->enable_segment_load_hle &&
                (HandleSegmentOverrideMemoryLoadInstruction(win32_context, context) ||
                 HandleSegmentOverrideByteLoadInstruction(win32_context, context) ||
                 HandleFsSegmentWordLoadInstruction(win32_context, context))) return true;
            break;
        default:
            break;
    }

    if (context->enable_privileged_trap_hle &&
        (HandleSelectorLimitInstruction(win32_context, context) ||
         HandlePrivilegedTrapInstruction(win32_context, context) ||
         HandlePortIoInstruction(win32_context, context)))
    {
        return true;
    }
    if (context->enable_traced_dos_hle &&
        (HandleTracedDosInterrupt21(win32_context, context) ||
         HandleTracedDosInterrupt2F(win32_context, context) ||
         HandleTracedDpmiInterrupt31(win32_context, context) ||
         HandleTracedMouseInterrupt33(win32_context, context) ||
         HandleTracedBiosInterrupt16(win32_context, context)))
    {
        return true;
    }
    if (context->enable_segment_load_hle &&
        (HandleSegmentLoadInstruction(win32_context, context) ||
         HandleSegmentPushInstruction(win32_context, context) ||
         HandleFarJumpInstruction(win32_context, context) ||
         (HandleMode16FarReturn(win32_context, context).value_or(false)) ||
         HandleFarReturnInstruction(win32_context, context) ||
         HandleIretdInstruction(win32_context, context).value_or(false) ||
         HandleSegmentPopInstruction(win32_context, context) ||
         HandleRepStosdInstruction(win32_context, context) ||
         HandleRepMovsInstruction(win32_context, context) ||
         HandleRepCmpsbInstruction(win32_context, context) ||
         HandleLodsbInstruction(win32_context, context) ||
         HandleSegmentStoreInstruction(win32_context, context) ||
         HandleSegmentOverrideMemoryLoadInstruction(win32_context, context) ||
         HandleSegmentOverrideByteLoadInstruction(win32_context, context) ||
         HandleFsSegmentWordLoadInstruction(win32_context, context) ||
         HandleSegmentMemoryCompareInstruction(win32_context, context) ||
         HandleSegmentMemoryLoadInstruction(win32_context, context) ||
         HandleTracedMemoryLoadInstruction(win32_context, context) ||
         HandleTracedMemoryAddInstruction(win32_context, context) ||
         HandleTracedMemoryOrInstruction(win32_context, context) ||
         HandleTracedMemoryCompareByteInstruction(win32_context, context) ||
         HandleTracedMemoryStoreInstruction(win32_context, context) ||
         HandleTracedMemoryTestInstruction(win32_context, context) ||
         HandleTracedFpuMemoryInstruction(win32_context, context) ||
         HandleDosMemoryAccess(win32_context, context)))
    {
        HandleSegmentStoreInstruction(win32_context, context);
        HandleSegmentOverrideMemoryLoadInstruction(win32_context, context);
        return true;
    }
    return false;
}

// Route A sizing instrumentation. Decodes the instruction at `eip` and reports
// whether it is HLE-sensitive under selective-breakpoint region execution: a
// segment-override memory access, a segment-register move/push/pop, an FS/GS
// read-write, an INT/IO/string/privileged/system instruction. Everything else
// (ALU, plain mov/lea, register push/pop, direct branch/call/ret) can run
// natively between traps. Mirrors detail::IsSensitive in the verified region
// analyzer so the measured ceiling matches what the native fast path enforces.
bool ClassifyRouteASensitive(std::uint32_t eip, bool* is_segment)
{
    if (is_segment != nullptr)
    {
        *is_segment = false;
    }
    static thread_local ZydisDecoder decoder;
    static thread_local bool decoder_ready = false;
    if (!decoder_ready)
    {
        if (!ZYAN_SUCCESS(ZydisDecoderInit(&decoder,
                                           ZYDIS_MACHINE_MODE_LEGACY_32,
                                           ZYDIS_STACK_WIDTH_32)))
        {
            return false;
        }
        decoder_ready = true;
    }
    ZydisDecodedInstruction instruction{};
    const auto* bytes = reinterpret_cast<const void*>(
        static_cast<std::uintptr_t>(eip));
    if (!ZYAN_SUCCESS(ZydisDecoderDecodeInstruction(
            &decoder, nullptr, bytes, ZYDIS_MAX_INSTRUCTION_LENGTH,
            &instruction)))
    {
        return false;
    }
    const bool segment =
        (instruction.attributes & ZYDIS_ATTRIB_HAS_SEGMENT) != 0 ||
        instruction.meta.category == ZYDIS_CATEGORY_SEGOP ||
        instruction.meta.category == ZYDIS_CATEGORY_RDWRFSGS;
    if (is_segment != nullptr)
    {
        *is_segment = segment;
    }
    if (segment ||
        (instruction.attributes & ZYDIS_ATTRIB_IS_PRIVILEGED) != 0)
    {
        return true;
    }
    switch (instruction.meta.category)
    {
    case ZYDIS_CATEGORY_INTERRUPT:
    case ZYDIS_CATEGORY_IO:
    case ZYDIS_CATEGORY_IOSTRINGOP:
    case ZYDIS_CATEGORY_STRINGOP:
    case ZYDIS_CATEGORY_SYSCALL:
    case ZYDIS_CATEGORY_SYSRET:
    case ZYDIS_CATEGORY_SYSTEM:
    case ZYDIS_CATEGORY_UINTR:
        return true;
    default:
        return false;
    }
}

// Extends the same int3-sentinel single-step probe as RecordExecutionProbe,
// but logs every single-stepped instruction inside a guest code RANGE
// (rather than one exact-match address, and without a single-shot gate) into
// a wrapping ring. `value_at_esp_offset` is read relative to the live ESP at
// each capture, not a hardcoded absolute address. See
// docs/design/20260717-223-guest-stack-watchpoint-veh-coexistence.md §8.

// Clears ThreadContext::active_hotspot_scope on every exit path of
// HandleSingleStepTrace so a stale pointer can never outlive its sample.
class ActiveHotspotScopeReset
{
public:
    explicit ActiveHotspotScopeReset(ThreadContext* context)
        : context_(context)
    {
    }

    ~ActiveHotspotScopeReset()
    {
        if (context_ != nullptr)
        {
            context_->active_hotspot_scope = nullptr;
        }
    }

    ActiveHotspotScopeReset(const ActiveHotspotScopeReset&) = delete;
    ActiveHotspotScopeReset& operator=(const ActiveHotspotScopeReset&) = delete;

private:
    ThreadContext* context_ = nullptr;
};

// Diagnostic instrumentation that runs unconditionally on every single step:
// execution probe/trace capture, the LINEXE resolution EIP ladder, the shadow
// register mirror read by the telemetry supervisor, and Route A classification.
// Extracted from HandleSingleStepTrace in Task 322 so its cost can be attributed
// to SingleStepProfileStage::kPrologueTrace without re-indenting the body. The
// body is unchanged; only the enclosing function boundary moved.
void RecordSingleStepDiagnostics(repiu::platform::GuestCpuContext* win32_context, ThreadContext* context)
{
    RecordExecutionProbe(win32_context, context);
    RecordExecutionTrace(win32_context, context);
    const std::uint32_t eip_offset =
        static_cast<std::uint32_t>(win32_context->Eip) -
        context->runtime_base;
    if (eip_offset >= 0x000F38F6U && eip_offset <= 0x000F3902U)
    {
        constexpr std::uint32_t stages[] = {
            0x000F38F6U, 0x000F38FAU, 0x000F38FEU,
            0x000F3900U, 0x000F3902U};
        for (std::uint32_t index = 0; index < 5; ++index)
        {
            if (eip_offset == stages[index])
            {
                context->linexe_export_name_stage_mask |= 1U << index;
            }
        }
    }
    if (eip_offset == 0x000F37E8U)
    {
        ++context->linexe_scan_entry_count;
    }
    else if (eip_offset == 0x000F382DU)
    {
        ++context->linexe_module_candidate_count;
        const auto* selector = reinterpret_cast<const std::uint16_t*>(
            static_cast<std::uintptr_t>(win32_context->Esp + 0x10U));
        if (IsGuestRangeReadable(context, selector, sizeof(*selector)))
        {
            context->linexe_module_selector_stack_value = *selector;
        }
        const auto* offset = reinterpret_cast<const std::uint32_t*>(
            static_cast<std::uintptr_t>(win32_context->Esp + 0x08U));
        if (IsGuestRangeReadable(context, offset, sizeof(*offset)))
        {
            context->linexe_module_offset_stack_value = *offset;
        }
    }
    else if (eip_offset == 0x000F3818U)
    {
        context->linexe_root_selector_eax = win32_context->Eax;
        context->linexe_root_read_gs = context->guest_gs;
    }
    else if (eip_offset == 0x000F3889U)
    {
        ++context->linexe_module_match_count;
    }
    else if (eip_offset == 0x000F384CU)
    {
        ++context->linexe_name_pointer_valid_count;
    }
    else if (eip_offset == 0x000F3853U)
    {
        ++context->linexe_name_byte_instruction_count;
    }
    else if (eip_offset == 0x000F393FU)
    {
        ++context->linexe_export_match_count;
    }
    else if (eip_offset == 0x000F38BEU)
    {
        ++context->linexe_export_entry_loop_count;
    }
    else if (eip_offset == 0x000F3974U)
    {
        ++context->linexe_export_compare_count;
        context->linexe_export_compare_eax = win32_context->Eax;
        context->linexe_export_compare_ecx = win32_context->Ecx;
        context->linexe_export_compare_eflags = win32_context->EFlags;
    }
    else if (eip_offset == 0x000F396DU)
    {
        context->linexe_export_count_load_edx = win32_context->Edx;
        context->linexe_export_count_load_gs = context->guest_gs;
    }
    else if (eip_offset == 0x000F3963U)
    {
        const auto* offset = reinterpret_cast<const std::uint32_t*>(
            static_cast<std::uintptr_t>(win32_context->Esp + 0x08U));
        const auto* selector = reinterpret_cast<const std::uint16_t*>(
            static_cast<std::uintptr_t>(win32_context->Esp + 0x10U));
        if (IsGuestRangeReadable(context, offset, sizeof(*offset)))
        {
            context->linexe_export_offset_stack_value = *offset;
        }
        if (IsGuestRangeReadable(context, selector, sizeof(*selector)))
        {
            context->linexe_export_selector_stack_value = *selector;
        }
    }
    else if (eip_offset == 0x000F38B9U ||
             eip_offset == 0x000F395FU)
    {
        const auto* offset = reinterpret_cast<const std::uint32_t*>(
            static_cast<std::uintptr_t>(win32_context->Esp + 0x08U));
        const auto* selector = reinterpret_cast<const std::uint16_t*>(
            static_cast<std::uintptr_t>(win32_context->Esp + 0x10U));
        const std::uint32_t module_offset =
            IsGuestRangeReadable(context, offset, sizeof(*offset)) ?
                *offset : 0;
        const std::uint16_t module_selector =
            IsGuestRangeReadable(context, selector, sizeof(*selector)) ?
                *selector : 0;
        if (eip_offset == 0x000F38B9U)
        {
            context->linexe_export_jump_source_esp = win32_context->Esp;
            context->linexe_export_jump_source_module_offset = module_offset;
            context->linexe_export_jump_source_module_selector =
                module_selector;
        }
        else
        {
            context->linexe_export_jump_target_esp = win32_context->Esp;
            context->linexe_export_jump_target_module_offset = module_offset;
            context->linexe_export_jump_target_module_selector =
                module_selector;
        }
    }
    else if (eip_offset == 0x000F3900U)
    {
        ++context->linexe_export_name_compare_count;
        context->linexe_export_name_compare_gs = context->guest_gs;
        context->linexe_export_name_compare_edi = win32_context->Edi;
        context->linexe_export_name_compare_esi = win32_context->Esi;
        std::uint8_t actual = 0;
        if (ReadSegmentByte(context, 5, context->guest_gs,
                            win32_context->Edi, &actual))
        {
            context->linexe_export_name_actual_byte = actual;
        }
        const auto* expected = reinterpret_cast<const std::uint8_t*>(
            static_cast<std::uintptr_t>(win32_context->Esi));
        if (IsGuestRangeReadable(context, expected, sizeof(*expected)))
        {
            context->linexe_export_name_expected_byte = *expected;
        }
    }
    else if (eip_offset == 0x000F37A5U)
    {
        ++context->linexe_bridge_entry_count;
        context->linexe_bridge_selector = static_cast<std::uint16_t>(
            win32_context->Ebx & 0xFFFFU);
        context->linexe_bridge_offset = win32_context->Edi;
        context->linexe_bridge_esp = win32_context->Esp;
        context->linexe_bridge_ebp = win32_context->Ebp;
        repiu::hle::LinexeService service{};
        context->linexe_bridge_gate_valid =
            context->linexe_bridge_selector ==
                context->linexe_gate_plan.linexe_code_selector &&
            repiu::hle::DecodeLinexeCallGate(
                context->linexe_gate_plan,
                context->linexe_bridge_offset,
                &service);
        if (context->linexe_bridge_gate_valid)
        {
            context->linexe_bridge_service =
                static_cast<std::uint32_t>(service);
        }
        const auto* stack = reinterpret_cast<const std::uint32_t*>(
            static_cast<std::uintptr_t>(win32_context->Esp));
        if (IsGuestRangeReadable(context,
                                 stack,
                                 sizeof(context->linexe_bridge_stack)))
        {
            std::memcpy(context->linexe_bridge_stack,
                        stack,
                        sizeof(context->linexe_bridge_stack));
        }
    }
    else if (eip_offset == 0x000F39A6U)
    {
        ++context->linexe_scan_return_count;
        context->linexe_scan_return_eax = win32_context->Eax;
        context->linexe_scan_return_ebp = win32_context->Ebp;
    }
    else if (eip_offset == 0x000F3F9BU)
    {
        context->linexe_scan_caller_eax = win32_context->Eax;
    }
    else if (eip_offset == 0x000F3FD2U)
    {
        context->linexe_selector_init_results[0] = win32_context->Eax;
    }
    else if (eip_offset == 0x000F3FE1U)
    {
        context->linexe_selector_init_results[1] = win32_context->Eax;
    }
    else if (eip_offset == 0x000F3FF0U)
    {
        context->linexe_selector_init_results[2] = win32_context->Eax;
    }
    const std::uint32_t eip =
        static_cast<std::uint32_t>(win32_context->Eip);
    if (IsGuestInstructionPointer(context, eip))
    {
        context->single_step_eip.store(eip, std::memory_order_relaxed);
        context->single_step_eax.store(win32_context->Eax,
                                       std::memory_order_relaxed);
        context->single_step_ebx.store(win32_context->Ebx,
                                       std::memory_order_relaxed);
        context->single_step_ecx.store(win32_context->Ecx,
                                       std::memory_order_relaxed);
        context->single_step_edx.store(win32_context->Edx,
                                       std::memory_order_relaxed);
        context->single_step_esi.store(win32_context->Esi,
                                       std::memory_order_relaxed);
        context->single_step_edi.store(win32_context->Edi,
                                       std::memory_order_relaxed);
        context->single_step_esp.store(win32_context->Esp,
                                       std::memory_order_relaxed);
        context->single_step_ebp.store(win32_context->Ebp,
                                       std::memory_order_relaxed);
        context->single_step_eflags.store(win32_context->EFlags,
                                          std::memory_order_relaxed);
        context->single_step_cs.store(win32_context->SegCs,
                                      std::memory_order_relaxed);
        context->single_step_ds.store(win32_context->SegDs,
                                      std::memory_order_relaxed);
        context->single_step_es.store(win32_context->SegEs,
                                      std::memory_order_relaxed);
        context->single_step_ss.store(win32_context->SegSs,
                                      std::memory_order_relaxed);
        context->single_step_fs.store(win32_context->SegFs,
                                      std::memory_order_relaxed);
        context->single_step_gs.store(win32_context->SegGs,
                                      std::memory_order_relaxed);
        context->single_step_trace_count.fetch_add(
            1,
            std::memory_order_relaxed);
        // Task 581: inside the guest-EIP branch, so the watch sees the address
        // the interpreter is actually about to step rather than every fault.
        std::optional<std::uint64_t> le_bytes;
        if (GuestAddressWatchAddress() == eip)
        {
            const auto* guest_eip = reinterpret_cast<const std::uint8_t*>(
                static_cast<std::uintptr_t>(eip));
            if (IsGuestRangeReadable(context, guest_eip, sizeof(std::uint64_t)))
            {
                std::uint64_t captured = 0U;
                std::memcpy(&captured, guest_eip, sizeof(captured));
                le_bytes = captured;
            }
        }
        RecordGuestAddressWatch(
            GuestAddressWatchEvent::kSingleStep,
            eip,
            eip,
            win32_context,
            le_bytes);
        bool routea_segment = false;
        if (ClassifyRouteASensitive(eip, &routea_segment))
        {
            context->routea_sensitive_count.fetch_add(
                1, std::memory_order_relaxed);
            if (routea_segment)
            {
                context->routea_segment_sensitive_count.fetch_add(
                    1, std::memory_order_relaxed);
            }
        }
    }
}

bool HandleSingleStepTrace(repiu::platform::GuestCpuContext* win32_context, ThreadContext* context)
{
    if (win32_context == nullptr || context == nullptr ||
        !context->enable_single_step_trace)
    {
        return false;
    }
    const std::uint32_t profile_eip =
        static_cast<std::uint32_t>(win32_context->Eip);
    SingleStepHotspotProfile* hotspot_profile =
        context->single_step_hotspot_profile != nullptr &&
        IsGuestInstructionPointer(context, profile_eip)
            ? context->single_step_hotspot_profile.get()
            : nullptr;
    SingleStepHotspotCycleScope hotspot_scope(
        hotspot_profile, profile_eip);
    // Task 323: publish the open sample so TryResumeAotAfterHandledHle can
    // attribute its sub-stages into it from another translation unit.
    context->active_hotspot_scope = &hotspot_scope;
    const ActiveHotspotScopeReset active_scope_reset(context);
    {
        SingleStepHotspotStageScope stage_scope(
            hotspot_scope, SingleStepProfileStage::kPrologueTrace);
        RecordSingleStepDiagnostics(win32_context, context);
    }

    const std::uint32_t hle_entry_eip =
        static_cast<std::uint32_t>(win32_context->Eip);
    TraceAotHleReentryState(
        "hle-before", context, win32_context, hle_entry_eip, hle_entry_eip);
    bool handled_hle = false;
    {
        SingleStepHotspotStageScope stage_scope(
            hotspot_scope, SingleStepProfileStage::kHleDispatch);
        handled_hle = DispatchGuestHleHandlers(win32_context, context);
    }
    TraceAotHleReentryState(
        "hle-after", context, win32_context, hle_entry_eip,
        static_cast<std::uint32_t>(win32_context->Eip));
    if (handled_hle)
    {
        hotspot_scope.SetOutcome(
            SingleStepProfileOutcome::kHandledHle);
        if (static_cast<std::uint32_t>(win32_context->Eip) != hle_entry_eip)
        {
            bool resumed = false;
            {
                SingleStepHotspotStageScope stage_scope(
                    hotspot_scope, SingleStepProfileStage::kAotResume);
                resumed = TryResumeAotAfterHandledHle(
                    win32_context, context, hle_entry_eip);
            }
            TraceAotHleReentryState(
                "reentry-after", context, win32_context, hle_entry_eip,
                static_cast<std::uint32_t>(win32_context->Eip));
            if (resumed)
            {
                NoteVehExitSite(context,
                                VehExitSite::kSingleStepTraceHleResumed);
                return true;
            }
            // A failed HLE-to-AOT lookup may not fall through to original guest
            // bytes when the continuation has different long-mode semantics.
            // The cache path is the common lowering for stack, width,
            // addressing, and privileged-instruction differences; if it is
            // unavailable, decline the event instead of corrupting host RSP.
            if (runtime::execution_model::RunsLongModeCodeCache() &&
                !CanResumeLinuxX64LegacyTarget(
                    context,
                    static_cast<std::uint32_t>(win32_context->Eip)))
            {
                ReportLinuxX64UntranslatableContinuation(
                    context, static_cast<std::uint32_t>(win32_context->Eip),
                    "single-step-trace");
                context->aot_reentry_pending = false;
                context->aot_legacy_fallback = false;
                context->enable_single_step_trace = false;
                win32_context->EFlags &= ~0x00000100U;
                return false;
            }
        }
        NoteVehExitSite(context, VehExitSite::kSingleStepTraceHleStepped);
        win32_context->EFlags |= 0x00000100U;
        return true;
    }

    // Task 301: deliver a coalesced timer request only after the existing VEH
    // path has reconciled AOT/HLE state to a guest instruction boundary.
    // PollThreadUntilExit never forces TF or changes the guest thread context.
    const std::uint32_t interrupt_boundary_eip =
        static_cast<std::uint32_t>(win32_context->Eip);
    {
        SingleStepHotspotStageScope stage_scope(
            hotspot_scope, SingleStepProfileStage::kInterruptInjection);
        InjectPendingInterrupts(win32_context, context);
    }
    if (static_cast<std::uint32_t>(win32_context->Eip) != interrupt_boundary_eip)
    {
        hotspot_scope.SetOutcome(
            SingleStepProfileOutcome::kTimerInterrupt);
        NoteVehExitSite(context, VehExitSite::kSingleStepTraceTimerInjected);
        return true;
    }
    bool entered_native = false;
    {
        SingleStepHotspotStageScope stage_scope(
            hotspot_scope, SingleStepProfileStage::kNativeEntry);
        entered_native = detail::TryEnterNativeFastPath(
            win32_context,
            &context->native_fast_path,
            context->runtime_base,
            context->runtime_size);
        if (!entered_native &&
            !context->enable_single_step_trace &&
            NativeLinearSpanEnabled(context->execution_backend))
        {
            entered_native = TryEnterNativeLinearSpan(win32_context, context);
        }
    }
    if (entered_native)
    {
        hotspot_scope.SetOutcome(
            SingleStepProfileOutcome::kNativeExecution);
        NoteVehExitSite(context, VehExitSite::kSingleStepTraceNativeEntry);
        return true;
    }
    NoteVehExitSite(context, VehExitSite::kSingleStepTraceStepped);
    win32_context->EFlags |= 0x00000100U;
    return true;
}






void RecordHandledHleTrap(repiu::platform::GuestCpuContext* win32_context,
                          ThreadContext* context,
                          std::uint8_t opcode)
{
    if (win32_context == nullptr || context == nullptr)
    {
        return;
    }

    ++context->handled_hle_trap_count;
    context->last_hle_trap_address =
        static_cast<std::uint32_t>(win32_context->Eip);
    context->last_hle_trap_opcode = opcode;
    // Task 581. Placed here rather than at the three call sites in
    // HandlePrivilegedTrapInstruction, which are its only callers: this
    // function is reached exactly when the privileged instruction was
    // serviced, which is the event the watch names.
    RecordGuestAddressWatch(GuestAddressWatchEvent::kPrivilegedService,
                            context->last_hle_trap_address,
                            context->last_hle_trap_address);
}



bool HandleOriginalFatalBreakpoint(const repiu::platform::FaultEvent& fault,
                                   ThreadContext* context)
{
    repiu::platform::GuestCpuContext* win32_context = fault.registers;
    if (fault.kind != repiu::platform::FaultKind::kBreakpoint ||
        win32_context == nullptr || context == nullptr ||
        win32_context->Eip == 0)
    {
        return false;
    }

    const std::uint32_t context_eip =
        static_cast<std::uint32_t>(win32_context->Eip);
    std::uint32_t breakpoint = context_eip;
    bool advance_to_continuation = false;
    if (IsGuestRangeReadable(
            context,
            reinterpret_cast<const void*>(
                static_cast<std::uintptr_t>(context_eip)),
            1U) &&
        *reinterpret_cast<const std::uint8_t*>(
            static_cast<std::uintptr_t>(context_eip)) == 0xCC)
    {
        advance_to_continuation = true;
    }
    else
    {
        breakpoint = context_eip - 1U;
    }
    if (!IsGuestRangeReadable(
            context,
            reinterpret_cast<const void*>(
                static_cast<std::uintptr_t>(breakpoint)),
            8U))
    {
        return false;
    }

    const auto* instruction = reinterpret_cast<const std::uint8_t*>(
        static_cast<std::uintptr_t>(breakpoint));
    if (instruction[0] != 0xCC || instruction[1] != 0x52 ||
        instruction[2] != 0xE8 || instruction[7] != 0xF4)
    {
        return false;
    }

    context->handled_fatal_breakpoint_count += 1U;
    context->last_fatal_breakpoint_address = breakpoint;
    context->last_fatal_message_address =
        static_cast<std::uint32_t>(win32_context->Edx);
    if (context->shared_live_telemetry != nullptr)
    {
        repiu::platform::AtomicExchange(
            &context->shared_live_telemetry->fatal_breakpoint_count,
            static_cast<long>(context->handled_fatal_breakpoint_count));
        repiu::platform::AtomicExchange(
            &context->shared_live_telemetry->fatal_message_address,
            static_cast<long>(context->last_fatal_message_address));
    }
    context->last_fatal_message.clear();
    ReadGuestAsciz(context,
                   context->last_fatal_message_address,
                   512U,
                   &context->last_fatal_message);
    context->fatal_breakpoint_continued = true;
    if (advance_to_continuation)
    {
        win32_context->Eip += 1U;
    }
    return true;
}



// Task 751. `AAM imm8` and `AAD imm8`, which long mode does not have. The
// cache leaves them as a boundary, and nothing answered it: Watcom's number
// formatting divides by ten with `aam`, and pumpitpc died there at its first
// score. AAM: AH = AL / imm8, AL = AL % imm8. AAD: AL = AL + AH * imm8, AH = 0.
// Both set SF, ZF and PF from AL and leave OF, AF and CF undefined, which here
// means untouched. A zero divisor is a divide error on the real machine and is
// declined rather than invented.
bool HandleAsciiAdjustInstruction(
    repiu::platform::GuestCpuContext* const win32_context,
    ThreadContext* const context)
{
    if (win32_context == nullptr || context == nullptr)
    {
        return false;
    }
    const auto* const instruction = reinterpret_cast<const std::uint8_t*>(
        static_cast<std::uintptr_t>(win32_context->Eip));
    if (!IsGuestRangeReadable(context, instruction, 2U) ||
        (instruction[0] != 0xD4U && instruction[0] != 0xD5U))
    {
        return false;
    }
    const std::uint32_t base = instruction[1];
    const std::uint32_t al = win32_context->Eax & 0xFFU;
    const std::uint32_t ah = (win32_context->Eax >> 8U) & 0xFFU;
    std::uint32_t new_al = 0U;
    std::uint32_t new_ah = 0U;
    if (instruction[0] == 0xD4U)
    {
        if (base == 0U)
        {
            return false;
        }
        new_ah = al / base;
        new_al = al % base;
    }
    else
    {
        new_al = (al + ah * base) & 0xFFU;
    }
    win32_context->Eax =
        (win32_context->Eax & 0xFFFF0000U) | (new_ah << 8U) | new_al;
    std::uint32_t parity = new_al;
    parity ^= parity >> 4U;
    parity ^= parity >> 2U;
    parity ^= parity >> 1U;
    constexpr std::uint32_t kParity = 0x00000004U;
    constexpr std::uint32_t kZero = 0x00000040U;
    constexpr std::uint32_t kSign = 0x00000080U;
    std::uint32_t eflags =
        win32_context->EFlags & ~(kParity | kZero | kSign);
    if ((parity & 1U) == 0U)
    {
        eflags |= kParity;
    }
    if (new_al == 0U)
    {
        eflags |= kZero;
    }
    if ((new_al & 0x80U) != 0U)
    {
        eflags |= kSign;
    }
    win32_context->EFlags = eflags;
    RecordHandledHleTrap(win32_context, context, instruction[0]);
    win32_context->Eip += 2U;
    return true;
}

bool HandlePrivilegedTrapInstruction(repiu::platform::GuestCpuContext* win32_context,
                                     ThreadContext* context)
{
    if (win32_context == nullptr || context == nullptr ||
        !IsGuestRangeReadable(context, reinterpret_cast<const void*>(static_cast<std::uintptr_t>(win32_context->Eip)), 1U))
    {
        return false;
    }
    const std::uint8_t* instruction = reinterpret_cast<const std::uint8_t*>(
        win32_context->Eip);
    if (*instruction == 0xFA)
    {
        RecordHandledHleTrap(win32_context, context, *instruction);
        win32_context->EFlags &= ~kEFlagsInterruptEnable;
        ++win32_context->Eip;
        // Task 751: the edit above does not survive the return to user mode.
        if (GuestCliHoldEnabled())
        {
            NoteGuestCli(&context->pic_timer_in_service,
                         GuestCliHoldClockNanoseconds());
        }
        return true;
    }
    if (*instruction == 0xFB)
    {
        RecordHandledHleTrap(win32_context, context, *instruction);
        win32_context->EFlags |= kEFlagsInterruptEnable;
        ++win32_context->Eip;
        if (GuestCliHoldEnabled())
        {
            NoteGuestSti(&context->pic_timer_in_service,
                         GuestCliHoldClockNanoseconds());
        }
        // Task 735. A pending IRQ0 is taken as soon as IF is set again, which
        // for a timer handler is its closing `sti` after the EOI. This is where
        // a tick owed during a long host call is caught up, now that the
        // in-service bit keeps it out of the handler's body. Only requested
        // here; the dispatcher's next injection attempt delivers it.
        if (PicTimerInServiceEnabled())
        {
            context->pic_timer_in_service.sti_request = true;
        }
        return true;
    }
    if (*instruction == 0xF4 && context->fatal_breakpoint_continued)
    {
        RecordHandledHleTrap(win32_context, context, *instruction);
        context->fatal_halt_reached = true;
        RecoverFromHleExit(win32_context, context);
        return true;
    }

    return false;
}


bool HandleDosHleInstruction(repiu::platform::GuestCpuContext* win32_context,
                             ThreadContext* context)
{
    const std::uint8_t* instruction = reinterpret_cast<const std::uint8_t*>(
        win32_context->Eip);
    if (instruction[0] == 0xCD && instruction[1] == 0x21)
    {
        return HandleDosInterrupt21(win32_context, context);
    }
    if (instruction[0] == 0xCD && instruction[1] == 0x2F)
    {
        return HandleDosInterrupt2F(win32_context, context);
    }
    if (instruction[0] == 0xCD && instruction[1] == 0x31)
    {
        return HandleDpmiInterrupt31(win32_context, context);
    }
    if (instruction[0] == 0xCD && instruction[1] == 0x33)
    {
        return HandleMouseInterrupt33(win32_context, context);
    }
    if (instruction[0] == 0xCD && instruction[1] == 0x16)
    {
        return HandleBiosInterrupt16(win32_context, context);
    }

    if (instruction[0] == 0xCD)
    {
        std::ostringstream stream;
        stream << "unsupported DOS interrupt 0x"
               << std::hex << static_cast<unsigned>(instruction[1]);
        context->hle_message = stream.str();
    }
    return false;
}

std::uint32_t ReadRegisterValueForAddress(const repiu::platform::GuestCpuContext* win32_context, ZydisRegister reg)
{
    if (reg == ZYDIS_REGISTER_NONE)
    {
        return 0;
    }
    switch (reg)
    {
        case ZYDIS_REGISTER_EAX: return win32_context->Eax;
        case ZYDIS_REGISTER_ECX: return win32_context->Ecx;
        case ZYDIS_REGISTER_EDX: return win32_context->Edx;
        case ZYDIS_REGISTER_EBX: return win32_context->Ebx;
        case ZYDIS_REGISTER_ESP: return win32_context->Esp;
        case ZYDIS_REGISTER_EBP: return win32_context->Ebp;
        case ZYDIS_REGISTER_ESI: return win32_context->Esi;
        case ZYDIS_REGISTER_EDI: return win32_context->Edi;
        default: return 0;
    }
}

void WriteRegisterFromZydis(repiu::platform::GuestCpuContext* win32_context, ZydisRegister reg, std::uint32_t value)
{
    switch (reg)
    {
        case ZYDIS_REGISTER_EAX: win32_context->Eax = value; break;
        case ZYDIS_REGISTER_ECX: win32_context->Ecx = value; break;
        case ZYDIS_REGISTER_EDX: win32_context->Edx = value; break;
        case ZYDIS_REGISTER_EBX: win32_context->Ebx = value; break;
        case ZYDIS_REGISTER_ESP: win32_context->Esp = value; break;
        case ZYDIS_REGISTER_EBP: win32_context->Ebp = value; break;
        case ZYDIS_REGISTER_ESI: win32_context->Esi = value; break;
        case ZYDIS_REGISTER_EDI: win32_context->Edi = value; break;

        case ZYDIS_REGISTER_AX: win32_context->Eax = (win32_context->Eax & 0xFFFF0000U) | (value & 0xFFFFU); break;
        case ZYDIS_REGISTER_CX: win32_context->Ecx = (win32_context->Ecx & 0xFFFF0000U) | (value & 0xFFFFU); break;
        case ZYDIS_REGISTER_DX: win32_context->Edx = (win32_context->Edx & 0xFFFF0000U) | (value & 0xFFFFU); break;
        case ZYDIS_REGISTER_BX: win32_context->Ebx = (win32_context->Ebx & 0xFFFF0000U) | (value & 0xFFFFU); break;
        case ZYDIS_REGISTER_SP: win32_context->Esp = (win32_context->Esp & 0xFFFF0000U) | (value & 0xFFFFU); break;
        case ZYDIS_REGISTER_BP: win32_context->Ebp = (win32_context->Ebp & 0xFFFF0000U) | (value & 0xFFFFU); break;
        case ZYDIS_REGISTER_SI: win32_context->Esi = (win32_context->Esi & 0xFFFF0000U) | (value & 0xFFFFU); break;
        case ZYDIS_REGISTER_DI: win32_context->Edi = (win32_context->Edi & 0xFFFF0000U) | (value & 0xFFFFU); break;

        case ZYDIS_REGISTER_AL: win32_context->Eax = (win32_context->Eax & 0xFFFFFF00U) | (value & 0xFFU); break;
        case ZYDIS_REGISTER_CL: win32_context->Ecx = (win32_context->Ecx & 0xFFFFFF00U) | (value & 0xFFU); break;
        case ZYDIS_REGISTER_DL: win32_context->Edx = (win32_context->Edx & 0xFFFFFF00U) | (value & 0xFFU); break;
        case ZYDIS_REGISTER_BL: win32_context->Ebx = (win32_context->Ebx & 0xFFFFFF00U) | (value & 0xFFU); break;

        case ZYDIS_REGISTER_AH: win32_context->Eax = (win32_context->Eax & 0xFFFF00FFU) | ((value & 0xFFU) << 8); break;
        case ZYDIS_REGISTER_CH: win32_context->Ecx = (win32_context->Ecx & 0xFFFF00FFU) | ((value & 0xFFU) << 8); break;
        case ZYDIS_REGISTER_DH: win32_context->Edx = (win32_context->Edx & 0xFFFF00FFU) | ((value & 0xFFU) << 8); break;
        case ZYDIS_REGISTER_BH: win32_context->Ebx = (win32_context->Ebx & 0xFFFF00FFU) | ((value & 0xFFU) << 8); break;

        default: break;
    }
}

bool HandleGuestLowMemoryReadFault(repiu::platform::GuestCpuContext* win32_context, ThreadContext* context, std::uint32_t fault_va, std::uint32_t decode_eip)
{
    if (win32_context == nullptr || context == nullptr)
    {
        return false;
    }

    context->debug_emulate_stage = 1; // Started

    // Task 475: string instructions reading low memory are serviced by their
    // own module first. The MOV/MOVZX/MOVSX path below cannot express them --
    // they carry repetition state in ECX/ESI/EDI and must not always step EIP.
    if (ServiceGuestLowMemoryStringInstruction(win32_context, context))
    {
        context->debug_emulate_stage = 101; // Serviced as a string operation
        return true;
    }

    // Decode the instruction that is actually about to execute, not the guest
    // address it maps back to. On a fault EIP points at the faulting
    // instruction, so this is also what we must step over afterwards; under AOT
    // the two addresses differ and only this one describes the running code.
    const std::uint32_t execute_eip =
        static_cast<std::uint32_t>(win32_context->Eip);
    const std::uint8_t* instruction_ptr = reinterpret_cast<const std::uint8_t*>(
        static_cast<std::uintptr_t>(execute_eip));

    ZydisDecoder decoder;
    if (!ZYAN_SUCCESS(ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LEGACY_32, ZYDIS_STACK_WIDTH_32)))
    {
        context->debug_emulate_stage = 2; // Decoder init failed
        return false;
    }

    ZydisDecodedInstruction instruction{};
    ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT] = {};

    if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(&decoder, instruction_ptr, 15, &instruction, operands)))
    {
        context->debug_emulate_stage = 3; // Decode failed
        return false;
    }

    if (instruction.mnemonic != ZYDIS_MNEMONIC_MOV &&
        instruction.mnemonic != ZYDIS_MNEMONIC_MOVZX &&
        instruction.mnemonic != ZYDIS_MNEMONIC_MOVSX)
    {
        context->debug_emulate_stage = 4; // Wrong mnemonic
        context->debug_emulate_decode_result = instruction.mnemonic;
        return false;
    }

    if (instruction.operand_count_visible < 2)
    {
        context->debug_emulate_stage = 5; // Invisible operands too few
        return false;
    }

    if (operands[0].type != ZYDIS_OPERAND_TYPE_REGISTER ||
        operands[1].type != ZYDIS_OPERAND_TYPE_MEMORY)
    {
        context->debug_emulate_stage = 6; // Operand types mismatch
        return false;
    }

    const auto& mem = operands[1].mem;
    std::uint32_t base_val = ReadRegisterValueForAddress(win32_context, mem.base);
    std::uint32_t index_val = ReadRegisterValueForAddress(win32_context, mem.index);
    std::uint32_t scale = mem.scale;
    std::uint32_t displacement = mem.disp.has_displacement ? static_cast<std::uint32_t>(mem.disp.value) : 0;
    
    std::uint32_t calculated_address = base_val + index_val * scale + displacement;

    if (calculated_address >= repiu::runtime::kDosLowMemorySize)
    {
        context->debug_emulate_stage = 7; // Out of range calculated address
        context->debug_emulate_calculated_address = calculated_address;
        return false;
    }

    // Runaway guard. This must not fire on *legitimate* repetition: the known
    // caller is a byte load inside a shared stricmp, and a single filename check
    // calls it once per candidate extension ("tga", "pcx", "ptx", "rgb"), so the
    // same EIP and address recur in a tight burst by design. While this handler
    // rewrote the instruction as NOPs the case could not arise -- the first hit
    // destroyed the instruction -- so the guard only became reachable once the
    // instruction was preserved, and it then aborted the run as a false
    // positive. Count only repeats that made no forward progress, which is what
    // "runaway" actually means now that we step EIP over the load.
    std::uint32_t current_tick = repiu::platform::MillisecondTicks();
    std::uint32_t elapsed_ticks = current_tick - context->last_low_memory_fault_tick;
    context->last_low_memory_fault_tick = current_tick;

    // Stepping EIP past the load makes forward progress structural: the guest
    // cannot be pinned on this instruction by us, only by its own control flow.
    // So the guard no longer needs a tight time-based trip. Keep a high absolute
    // cap purely as a pathology backstop, and reset it whenever the site moves.
    constexpr std::uint32_t kLowMemoryRunawayCap = 100000U;
    (void)elapsed_ticks;

    if (execute_eip == context->last_low_memory_fault_eip &&
        calculated_address == context->last_low_memory_fault_address)
    {
        context->low_memory_fault_repeat_count++;
        if (context->low_memory_fault_repeat_count >= kLowMemoryRunawayCap)
        {
            context->debug_emulate_stage = 99; // Runaway abort
            return false;
        }
    }
    else
    {
        context->last_low_memory_fault_eip = execute_eip;
        context->last_low_memory_fault_address = calculated_address;
        context->low_memory_fault_repeat_count = 1;
    }

    std::uint32_t read_width_bits = operands[1].size;
    std::uint32_t read_width_bytes = read_width_bits / 8;
    if (read_width_bytes == 0)
    {
        read_width_bytes = 1;
    }

    std::uint32_t raw_val = 0;
    if (read_width_bytes == 1)
    {
        std::uint8_t byte_val = 0;
        repiu::runtime::ReadDosLowMemoryUInt8(context->dos_low_memory, calculated_address, &byte_val);
        raw_val = byte_val;
    }
    else if (read_width_bytes == 2)
    {
        std::uint16_t word_val = 0;
        repiu::runtime::ReadDosLowMemoryUInt16(context->dos_low_memory, calculated_address, &word_val);
        raw_val = word_val;
    }
    else if (read_width_bytes == 4)
    {
        std::uint32_t dword_val = 0;
        repiu::runtime::ReadDosLowMemoryUInt32(context->dos_low_memory, calculated_address, &dword_val);
        raw_val = dword_val;
    }

    std::uint32_t final_val = raw_val;
    if (instruction.mnemonic == ZYDIS_MNEMONIC_MOVSX)
    {
        if (read_width_bytes == 1)
        {
            final_val = static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int8_t>(raw_val)));
        }
        else if (read_width_bytes == 2)
        {
            final_val = static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int16_t>(raw_val)));
        }
    }

    WriteRegisterFromZydis(win32_context, operands[0].reg.value, final_val);

    // Step over the emulated load instead of rewriting it. The previous version
    // overwrote the instruction with NOPs, which resumed execution but did so by
    // destroying guest code permanently. The only observed site is
    // `mov al,[ebx]` inside a *shared* stricmp, so one null-string comparison
    // removed the byte load for every later call -- silently corrupting every
    // filename-extension comparison in the game rather than crashing.
    win32_context->Eip = execute_eip + instruction.length;

    context->debug_emulate_stage = 100; // Success
    context->debug_emulate_decode_result = instruction.length;
    context->debug_emulate_calculated_address = operands[0].reg.value;

    context->low_memory_read_emulate_count++;
    context->last_low_memory_read_emulate_address = calculated_address;
    context->last_low_memory_read_emulate_eip = decode_eip;
    context->last_low_memory_read_emulate_value = final_val;
    context->last_low_memory_read_emulate_reg = operands[0].reg.value;

    RecordLowMemoryAccess(win32_context, context, instruction_ptr[0], calculated_address, final_val);

    return true;
}

bool HandleDosMemoryAccess(repiu::platform::GuestCpuContext* win32_context, ThreadContext* context)
{
    const std::uint8_t* instruction = reinterpret_cast<const std::uint8_t*>(
        win32_context->Eip);
    if (instruction[0] == 0x66 && instruction[1] == 0x26 &&
        instruction[2] == 0x8B && instruction[3] == 0x0D)
    {
        const std::uint32_t offset =
            static_cast<std::uint32_t>(instruction[4]) |
            (static_cast<std::uint32_t>(instruction[5]) << 8) |
            (static_cast<std::uint32_t>(instruction[6]) << 16) |
            (static_cast<std::uint32_t>(instruction[7]) << 24);
        std::uint16_t value = 0;
        if (offset == 0x2C)
        {
            value = context->linexe_environment_active ? 0x002CU : 0;
        }
        win32_context->Ecx =
            (win32_context->Ecx & 0xFFFF0000U) | value;
        RecordLowMemoryAccess(win32_context,
                              context,
                              instruction[2],
                              offset,
                              value);
        win32_context->Eip += 8;
        return true;
    }
    if (instruction[0] == 0x66 && instruction[1] == 0x26 &&
        instruction[2] == 0x8C && instruction[3] == 0x1D)
    {
        const std::uint32_t offset =
            static_cast<std::uint32_t>(instruction[4]) |
            (static_cast<std::uint32_t>(instruction[5]) << 8) |
            (static_cast<std::uint32_t>(instruction[6]) << 16) |
            (static_cast<std::uint32_t>(instruction[7]) << 24);
        RecordLowMemoryAccess(win32_context,
                              context,
                              instruction[2],
                              offset,
                              0);
        win32_context->Eip += 8;
        return true;
    }
    if (instruction[0] == 0x26 && instruction[1] == 0x8A &&
        instruction[2] == 0x4F && instruction[3] == 0xFF)
    {
        win32_context->Ecx &= 0xFFFFFF00U;
        RecordLowMemoryAccess(win32_context,
                              context,
                              instruction[1],
                              win32_context->Edi - 1,
                              0);
        win32_context->Eip += 4;
        return true;
    }
    if (instruction[0] == 0x8B && instruction[1] == 0x06 &&
        win32_context->Esi < 0x10000)
    {
        win32_context->Eax = 0;
        RecordLowMemoryAccess(win32_context,
                              context,
                              instruction[0],
                              win32_context->Esi,
                              0);
        win32_context->Eip += 2;
        return true;
    }
    if (instruction[0] == 0x80 && instruction[1] == 0x3E &&
        instruction[2] == 0x00 && win32_context->Esi < 0x10000)
    {
        win32_context->EFlags |= 0x40U;
        win32_context->EFlags &= ~1U;
        RecordLowMemoryAccess(win32_context,
                              context,
                              instruction[0],
                              win32_context->Esi,
                              0);
        win32_context->Eip += 3;
        return true;
    }
    if (instruction[0] == 0xAC && win32_context->Esi < 0x10000)
    {
        win32_context->Eax &= 0xFFFFFF00U;
        RecordLowMemoryAccess(win32_context,
                              context,
                              instruction[0],
                              win32_context->Esi,
                              0);
        ++win32_context->Esi;
        ++win32_context->Eip;
        return true;
    }
    if (instruction[0] == 0xA4 && win32_context->Esi < 0x10000)
    {
        char* destination = reinterpret_cast<char*>(
            static_cast<std::uintptr_t>(win32_context->Edi));
        if (destination != nullptr &&
            IsGuestRangeReadable(context, destination, 1))
        {
            *destination = '\0';
        }
        RecordLowMemoryAccess(win32_context,
                              context,
                              instruction[0],
                              win32_context->Edi,
                              0);
        ++win32_context->Esi;
        ++win32_context->Edi;
        ++win32_context->Eip;
        return true;
    }

    return false;
}


// Task 759. What differed by execution model here -- the direct model's two
// timed entries, and the cache model's resolver, entry and return traces -- is
// in execution_trampoline_direct.cpp and execution_trampoline_cache.cpp, behind
// execution_trampoline_model.h.

// Task 503d-19. What both hosts' thread procedures do around the switch,
// extracted rather than transcribed twice. Six fields and a memory query is
// exactly the amount of code that drifts when it is copied.
void FillGuestStackCallState(ThreadContext* context,
                             StackSwitchCallState* state)
{
    state->entry_address = context->entry_address;
    state->initial_esp = context->guest_initial_esp;
    state->enable_single_step_trace =
        context->enable_single_step_trace ? 1U : 0U;

    const repiu::platform::MemoryRegion stack_region =
        repiu::platform::QueryMemory(
            reinterpret_cast<void*>(context->guest_initial_esp - 4));
    if (stack_region.valid)
    {
        // allocation_base, because the limit names the whole reservation
        // rather than the run of pages sharing one protection -- the
        // distinction 3b tracks separately for exactly this caller.
        state->guest_stack_limit = static_cast<std::uint32_t>(
            reinterpret_cast<std::uintptr_t>(stack_region.allocation_base));
        state->guest_stack_base = context->guest_initial_esp;
    }
    context->active_call_state = state;
}

// The other half: what the guest left behind, and whether the procedure is
// finished. Returns true when `*thread_exit_code` is the answer.
bool FinishGuestStackCall(ThreadContext* context,
                          const StackSwitchCallState& state,
                          std::uint32_t* thread_exit_code)
{
    g_repiu_active_thread_context = nullptr;
    context->active_call_state = nullptr;
    context->host_esp = state.host_esp;
    if (context->guest_return_esp == 0)
    {
        context->guest_return_esp = state.guest_return_esp;
    }
    if (context->exception_caught)
    {
        *thread_exit_code = 2;
        return true;
    }
    if (context->process_exit)
    {
        *thread_exit_code = 0;
        return true;
    }
    return false;
}

// Task 503d-15 fenced this whole, because CreateThread named the signature and
// the body is an SEH __try around the guest call. Task 503d-18 takes the first
// of those away -- the signature is the thread layer's now -- and 503d-19 the
// second: what remains fenced is the SEH, and Linux gets a procedure of its own
// below that answers the same question without it.
#if defined(_WIN32)
std::uint32_t GuestEntryThreadProc(void* parameter)
{
    ThreadContext* context = static_cast<ThreadContext*>(parameter);
    if (context == nullptr)
    {
        return 1;
    }
    context->guest_thread_id = repiu::platform::CurrentThreadId();

    __try
    {
        if (context->use_guest_stack)
        {
            StackSwitchCallState state;
            FillGuestStackCallState(context, &state);
            g_repiu_active_thread_context = context;
            context->vectored_handler = AddVectoredExceptionHandler(
                1, GuestStackVectoredExceptionHandler);
            if (context->vectored_handler == nullptr)
            {
                g_repiu_active_thread_context = nullptr;
                context->active_call_state = nullptr;
                return 5;
            }

            trampoline_model::EnterGuestWithStack(&state, context);

            std::uint32_t finished_exit_code = 0;
            if (FinishGuestStackCall(context, state, &finished_exit_code))
            {
                return finished_exit_code;
            }
        }
        else
        {
            g_repiu_active_thread_context = context;
            context->vectored_handler = AddVectoredExceptionHandler(
                1, GuestStackVectoredExceptionHandler);
            if (context->vectored_handler == nullptr)
            {
                g_repiu_active_thread_context = nullptr;
                return 5;
            }
            trampoline_model::EnterGuestDirect(context);
            g_repiu_active_thread_context = nullptr;
            if (context->process_exit)
            {
                return 0;
            }
        }
        context->returned = true;
        return 0;
    }
    __except (CaptureException(
        repiu::platform::MakeFaultEventFromWin32(GetExceptionInformation()),
        context))
    {
        return 2;
    }
}

// Task 759. The Win32 host runs the direct model only.
repiu::platform::HostThreadEntry SelectGuestThreadEntry()
{
    return &GuestEntryThreadProc;
}
#else   // !defined(_WIN32)

// Task 503d-19. What `__except` does above, done where Linux can do it.
//
// A fault reaches here through the 3c handler. `DispatchGuestFault` gets first
// refusal, and everything it resumes -- guest INTs, self-modifying writes, the
// single-step machinery -- never reaches the rest of this function.
//
// What it declines is what SEH unwinds out of on Windows, and Linux has nothing
// to unwind: returning from a signal handler resumes the faulting instruction,
// which would fault again forever. So the fault is recorded and the guest's
// context is pointed at the recovery entry instead, which makes
// `CallGuestEntryWithStack` return as though the switch had completed. 3d-16's
// probe exercised exactly this round trip before the engine used it.
repiu::platform::FaultDisposition GuestThreadFaultCallback(
    repiu::platform::FaultEvent* fault, void* user_data)
{
    if (fault == nullptr || fault->registers == nullptr)
    {
        return repiu::platform::FaultDisposition::kNotHandled;
    }
    const repiu::platform::FaultDisposition disposition =
        DispatchGuestFault(*fault);
    if (disposition == repiu::platform::FaultDisposition::kResume)
    {
        // Task 759. On the cache model a guest that has exited leaves through
        // the cache's exit.
        auto* context = static_cast<ThreadContext*>(user_data);
        if (runtime::execution_model::RunsLongModeCodeCache() && context != nullptr &&
            context->cache_entry_active && context->process_exit)
        {
            fault->registers->EFlags &= ~0x00000100U;
            fault->registers->EFlags &= ~0x00000400U;
            fault->host_resume_address = trampoline_model::CacheExitAddress();
        }
        return disposition;
    }

    auto* context = static_cast<ThreadContext*>(user_data);
    // Task 582. The only place that sees DispatchGuestFault's answer, and it
    // holds the context, which is why the line is printed from here rather than
    // from the platform's unhandled-fault reporter -- that one knows nothing of
    // ThreadContext, and teaching it would make platform depend on engine.
    //
    // Before the recovery below, which rewrites the state this reports.
    RecordFaultExit(context, *fault);
    if (context == nullptr || context->active_call_state == nullptr)
    {
        // No switch to return from. The direct-entry path has no host frame to
        // land on, so this is where Linux is weaker than the SEH above: the
        // fault goes unhandled and the process takes the default action.
        return disposition;
    }
    CaptureException(*fault, context);
    RecordFaultRecoveryProvenance(
        context, fault->instruction_address,
        FaultRecoveryPath::kFaultCallback);
    RecoverToHost(fault->registers, context);
    return repiu::platform::FaultDisposition::kResume;
}

// The Linux guest thread procedure. Same shape as the Windows one, minus the
// SEH and with 3c where the vectored handler was.
std::uint32_t GuestEntryThreadProc(void* parameter)
{
    // The current Linux guest entry is a 32-bit native ABI. Do not let an x64
    // host reach it through a truncated guest function pointer or stack state.
    // Task 759: which is to say, it is the direct model's.
    if (!runtime::execution_model::RunsGuestBytesDirectly())
    {
        return 4;
    }
    ThreadContext* context = static_cast<ThreadContext*>(parameter);
    if (context == nullptr)
    {
        return 1;
    }
    context->guest_thread_id = repiu::platform::CurrentThreadId();

    if (context->use_guest_stack)
    {
        StackSwitchCallState state;
        FillGuestStackCallState(context, &state);
        g_repiu_active_thread_context = context;
        if (!repiu::platform::InstallFaultHandler(&GuestThreadFaultCallback,
                                                  context))
        {
            g_repiu_active_thread_context = nullptr;
            context->active_call_state = nullptr;
            return 5;
        }
        // 3c owns the registration itself; this field is only the flag that
        // says one is installed, which is what the teardown reads.
        context->vectored_handler = context;

        trampoline_model::EnterGuestWithStack(&state, context);

        std::uint32_t finished_exit_code = 0;
        if (FinishGuestStackCall(context, state, &finished_exit_code))
        {
            return finished_exit_code;
        }
    }
    else
    {
        g_repiu_active_thread_context = context;
        if (!repiu::platform::InstallFaultHandler(&GuestThreadFaultCallback,
                                                  context))
        {
            g_repiu_active_thread_context = nullptr;
            return 5;
        }
        context->vectored_handler = context;
        trampoline_model::EnterGuestDirect(context);
        g_repiu_active_thread_context = nullptr;
        if (context->process_exit)
        {
            return 0;
        }
    }
    context->returned = true;
    return 0;
}

// Task 578. The x64 thread procedure.
//
// The same shape as the i386 direct path above -- install the fault handler,
// run, clear it -- with the one difference this port is about: what is entered
// is the emitted cache rather than the guest's own bytes.
std::uint32_t GuestCacheEntryThreadProc(void* parameter)
{
    ThreadContext* context = static_cast<ThreadContext*>(parameter);
    if (context == nullptr)
    {
        return 1;
    }
    context->guest_thread_id = repiu::platform::CurrentThreadId();
    g_repiu_active_thread_context = context;
    if (!repiu::platform::InstallFaultHandler(&GuestThreadFaultCallback,
                                              context))
    {
        g_repiu_active_thread_context = nullptr;
        return 5;
    }
    context->vectored_handler = context;
    repiu::platform::ArmLinuxDataWatchFromEnvironment();
    // Task 583: the window in which a guest is executing by this mechanism.
    // The fault guard reads it to answer "is a guest running", which on this
    // host cannot be answered by `active_call_state` -- there is no switch to
    // record. Cleared immediately after, so a fault taken outside the run is
    // declined here exactly as it was before.
    context->cache_entry_active = true;
    trampoline_model::EnterGuestCache(context);
    context->cache_entry_active = false;
    g_repiu_active_thread_context = nullptr;
    if (context->process_exit)
    {
        return 0;
    }
    context->returned = true;
    return 0;
}

// Task 759. The thread procedure of this host's execution model.
repiu::platform::HostThreadEntry SelectGuestThreadEntry()
{
    return runtime::execution_model::RunsLongModeCodeCache()
        ? &GuestCacheEntryThreadProc
        : &GuestEntryThreadProc;
}
#endif  // defined(_WIN32)

}  // namespace

void TraceAotGuestMapForModel(const AotCodeCachePlacement& placement,
                              const std::uint32_t runtime_base,
                              const char* const phase)
{
    TraceAotGuestMap(placement, runtime_base, phase);
}

bool DispatchGuestHleInstruction(repiu::platform::GuestCpuContext* win32_context,
                                 ThreadContext* context)
{
    return DispatchGuestHleHandlers(win32_context, context);
}

std::optional<runtime::GuestCodeDefaultOperandSize>
LegacyResumeCodeMode(const ThreadContext* context,
                     const std::uint32_t guest_target)
{
    if (context == nullptr)
    {
        return runtime::GuestCodeDefaultOperandSize::k32;
    }

    std::optional<runtime::GuestCodeDefaultOperandSize> placement_mode;
    bool placement_ambiguous = false;
    if (context->aot_placement != nullptr)
    {
        for (const runtime::RuntimeCodeModeRange& range :
             context->aot_placement->code_mode_ranges)
        {
            if (range.virtual_size == 0U ||
                guest_target < range.relocated_base_address)
            {
                continue;
            }
            const std::uint64_t offset =
                static_cast<std::uint64_t>(guest_target) -
                range.relocated_base_address;
            if (offset >= range.virtual_size)
            {
                continue;
            }
            const runtime::GuestCodeDefaultOperandSize candidate =
                (range.object_flags & runtime::kLeObjectBigDefault) != 0U
                    ? runtime::GuestCodeDefaultOperandSize::k32
                    : runtime::GuestCodeDefaultOperandSize::k16;
            if (placement_mode.has_value())
            {
                placement_ambiguous = true;
            }
            else
            {
                placement_mode = candidate;
            }
        }
    }

    std::optional<runtime::GuestCodeDefaultOperandSize> selector_mode;
    bool selector_ambiguous = false;
    for (const runtime::GuestDescriptor& descriptor :
         context->selector_table.descriptors)
    {
        if (!descriptor.present || !descriptor.executable ||
            descriptor.code_default_operand_size ==
                runtime::GuestCodeDefaultOperandSize::kUnknown ||
            guest_target < descriptor.base)
        {
            continue;
        }
        const std::uint64_t offset =
            static_cast<std::uint64_t>(guest_target) - descriptor.base;
        if (offset > descriptor.limit)
        {
            continue;
        }
        if (selector_mode.has_value())
        {
            selector_ambiguous = true;
        }
        else
        {
            selector_mode = descriptor.code_default_operand_size;
        }
    }

    if (placement_ambiguous || selector_ambiguous)
    {
        return runtime::GuestCodeDefaultOperandSize::kUnknown;
    }
    if (placement_mode.has_value() && selector_mode.has_value() &&
        *placement_mode != *selector_mode)
    {
        return runtime::GuestCodeDefaultOperandSize::kUnknown;
    }
    if (placement_mode.has_value())
    {
        return placement_mode;
    }
    if (selector_mode.has_value())
    {
        return selector_mode;
    }
    return runtime::GuestCodeDefaultOperandSize::k32;
}

bool CanResumeLinuxX64LegacyTargetUncached(ThreadContext* context,
                                           const std::uint32_t guest_target)
{
    // Task 759. A question of the cache model; as before, a host that does
    // not run the long-mode cache answers false.
    if (!runtime::execution_model::RunsLongModeCodeCache())
    {
        return false;
    }
    constexpr std::size_t kMaximumX86InstructionBytes = 15U;
    const auto* const instruction = reinterpret_cast<const std::uint8_t*>(
        static_cast<std::uintptr_t>(guest_target));
    if (context == nullptr || !IsGuestInstructionPointer(context, guest_target) ||
        !IsGuestRangeReadable(
            context, instruction, kMaximumX86InstructionBytes))
    {
        return false;
    }
    const std::optional<runtime::GuestCodeDefaultOperandSize> code_mode =
        LegacyResumeCodeMode(context, guest_target);
    if (!code_mode.has_value() ||
        *code_mode == runtime::GuestCodeDefaultOperandSize::kUnknown)
    {
        return false;
    }
    return runtime::ClassifyLongModeBytes(
               instruction, kMaximumX86InstructionBytes, *code_mode)
               .compatibility ==
        runtime::LongModeByteCompatibility::kIdenticalBytes;
}

// Task 741. The answer depends on the guest bytes and the code segment's
// default operand size, so it is remembered per address together with the
// code mode it was computed under, until a guest code write or a change of
// the bytes at the address invalidates the entry.
bool CanResumeLinuxX64LegacyTarget(ThreadContext* context,
                                   const std::uint32_t guest_target)
{
    if (context == nullptr || !AotReentryMemoEnabled())
    {
        return CanResumeLinuxX64LegacyTargetUncached(context, guest_target);
    }
    auto* const entry = LookupAotReentryMemo(context, guest_target);
    if (entry == nullptr)
    {
        return CanResumeLinuxX64LegacyTargetUncached(context, guest_target);
    }
    std::uint8_t code_mode = 0U;
    if (runtime::execution_model::RunsLongModeCodeCache())
    {
        if (const std::optional<runtime::GuestCodeDefaultOperandSize> mode =
                LegacyResumeCodeMode(context, guest_target);
            mode.has_value())
        {
            code_mode = static_cast<std::uint8_t>(
                static_cast<std::uint32_t>(*mode) + 1U);
        }
    }
    if ((entry->computed & 0x04U) != 0U &&
        entry->long_mode_code_mode == code_mode)
    {
        CountAotReentryMemo(
            context, AotReentryMemoKind::kLongModeIdentity, true);
        return entry->long_mode_identical;
    }
    const bool identical =
        CanResumeLinuxX64LegacyTargetUncached(context, guest_target);
    entry->long_mode_identical = identical;
    entry->long_mode_code_mode = code_mode;
    entry->computed |= 0x04U;
    CountAotReentryMemo(context, AotReentryMemoKind::kLongModeIdentity, false);
    return identical;
}

// Execution probe/trace + guest-IP helpers promoted to external linkage for
// aot_runtime_dispatch.cpp (relocated out of the anonymous namespace).
// Task 342. Counts how often a page has been written from itself and answers
// whether this write should quarantine. The first writes only retire, which
// already stops the cache executing stale bytes; quarantine is the churn
// defence and waits for evidence of repetition. A page evicted from the table
// quarantines on sight, so overflow degrades to the old policy.
bool ShouldQuarantineWrittenPage(ThreadContext* context,
                                 std::uint32_t page,
                                 std::uint32_t destination)
{
    // Task 344: quarantine when the same address is rewritten repeatedly, which
    // is real churn, or when a page accumulates far more writes than a handful
    // of one-shot patches would explain, which bounds the pathological case a
    // per-address rule alone would miss.
    constexpr std::uint32_t kRepeatWriteThreshold = 4U;
    constexpr std::uint32_t kPageWriteCeiling = 32U;
    if (context == nullptr)
    {
        return true;
    }
    for (std::uint32_t index = 0;
         index < context->guest_page_write_history_size; ++index)
    {
        ThreadContext::GuestPageWriteRecord& record =
            context->guest_page_write_history[index];
        if (record.page != page)
        {
            continue;
        }
        ++record.count;
        if (record.last_destination == destination)
        {
            ++record.repeat_count;
        }
        else
        {
            record.last_destination = destination;
            record.repeat_count = 1U;
        }
        if (record.repeat_count >= kRepeatWriteThreshold ||
            record.count >= kPageWriteCeiling)
        {
            return true;
        }
        ++context->quarantine_deferred_count;
        return false;
    }
    if (context->guest_page_write_history_size >=
        ThreadContext::kGuestPageWriteHistoryCapacity)
    {
        ++context->guest_page_write_history_overflow;
        return true;
    }
    context->guest_page_write_history[
        context->guest_page_write_history_size] = {page, 1U, destination, 1U};
    ++context->guest_page_write_history_size;
    ++context->quarantine_deferred_count;
    return false;
}

// Task 337. Buckets are 1, 2, 3, 4, 5-8, 9-16, 17-32, 33+, so a uniform cost
// and a long tail are told apart without storing every run.
std::uint32_t SingleStepRunBucket(std::uint32_t length)
{
    if (length <= 4U)
    {
        return length - 1U;
    }
    if (length <= 8U)
    {
        return 4U;
    }
    if (length <= 16U)
    {
        return 5U;
    }
    if (length <= 32U)
    {
        return 6U;
    }
    return 7U;
}

// Task 503d-15: takes the event rather than a raw code, so the classification
// reads the kind and only the "other" bucket, which names what it could not
// classify, still reads the host's own number.
// Task 741. Open addressing keyed by the host address: a hash probe of a few
// slots per breakpoint. The guest mapping runs once per new address, so the
// lookup cost never repeats on the hot path. Returns null when the table is
// full and the address is new.
ThreadContext::BreakpointSiteCensusEntry* FindBreakpointSiteSlot(
    ThreadContext* context, const std::uint32_t eip, const bool insert)
{
    constexpr std::uint32_t kCapacity =
        ThreadContext::kBreakpointSiteCensusCapacity;
    static_assert((kCapacity & (kCapacity - 1U)) == 0U,
                  "the probe mask needs a power-of-two capacity");
    std::uint32_t index = ((eip >> 1U) * 0x9E3779B1U) >> 23U;
    for (std::uint32_t probe = 0; probe < kCapacity; ++probe)
    {
        auto& entry = context->breakpoint_site_census[index & (kCapacity - 1U)];
        if (entry.count == 0U)
        {
            return insert ? &entry : nullptr;
        }
        if (entry.host_address == eip)
        {
            return &entry;
        }
        ++index;
    }
    return nullptr;
}

void RecordBreakpointSite(ThreadContext* context, const std::uint32_t eip)
{
    auto* const slot = FindBreakpointSiteSlot(context, eip, true);
    if (slot == nullptr)
    {
        ++context->breakpoint_site_census_overflow;
        return;
    }
    if (slot->count != 0U)
    {
        ++slot->count;
        return;
    }
    auto& entry = *slot;
    ++context->breakpoint_site_census_size;
    entry.host_address = eip;
    entry.count = 1U;
    entry.in_cache = IsAotCacheAddress(context, eip);
    if (entry.in_cache)
    {
        std::uint32_t guest_address = 0U;
        entry.guest_mapped = context->aot_placement != nullptr &&
            FindAotGuestAddress(*context->aot_placement, eip, &guest_address);
        entry.guest_address = entry.guest_mapped ? guest_address : 0U;
    }
    else
    {
        entry.guest_address = eip;
        entry.guest_mapped = true;
    }
}

void RecordBreakpointSiteExit(ThreadContext* context, const std::uint32_t eip,
                              const VehExitSite site)
{
    auto* const slot = FindBreakpointSiteSlot(context, eip, false);
    if (slot != nullptr)
    {
        auto& entry = *slot;
        const auto code = static_cast<std::uint8_t>(site);
        for (std::uint32_t slot = 0;
             slot < ThreadContext::kBreakpointSiteExitCapacity; ++slot)
        {
            if (entry.exit_counts[slot] == 0U)
            {
                entry.exit_sites[slot] = code;
                entry.exit_counts[slot] = 1U;
                return;
            }
            if (entry.exit_sites[slot] == code)
            {
                ++entry.exit_counts[slot];
                return;
            }
        }
        ++entry.exit_overflow;
        return;
    }
}

void RecordVehExceptionCensus(ThreadContext* context,
                              const repiu::platform::FaultEvent& fault)
{
    if (context == nullptr)
    {
        return;
    }
    // Task 372: the exception code is validated by the time the census runs, so
    // this is where the gap the VEH scope banked gets its class. Single step is
    // the class that reads as a pure kernel round trip, since the guest executes
    // exactly one instruction between two consecutive single steps.
    RecordVehExceptionGap(
        context->execution_time_profile.get(),
        fault.kind == repiu::platform::FaultKind::kSingleStep
            ? VehGapClass::kSingleStep
            : fault.kind == repiu::platform::FaultKind::kBreakpoint
                ? VehGapClass::kBreakpoint
                : VehGapClass::kOther);
    if (fault.kind == repiu::platform::FaultKind::kSingleStep)
    {
        ++context->veh_single_step_exception_count;
        ++context->veh_single_step_run_length;
        return;
    }
    if (fault.kind == repiu::platform::FaultKind::kBreakpoint)
    {
        ++context->veh_breakpoint_exception_count;
        if (fault.registers != nullptr)
        {
            RecordBreakpointSite(
                context, static_cast<std::uint32_t>(fault.registers->Eip));
        }
    }
    else if (fault.kind == repiu::platform::FaultKind::kAccessViolation)
    {
        ++context->veh_access_violation_exception_count;
    }
    else
    {
        ++context->veh_other_exception_count;
        // Task 343: name the code rather than bucketing it as "other".
        bool recorded = false;
        for (std::uint32_t index = 0;
             index < ThreadContext::kOtherExceptionCodeCapacity; ++index)
        {
            if (context->veh_other_exception_code_counts[index] == 0U ||
                context->veh_other_exception_codes[index] ==
                    fault.host_code)
            {
                context->veh_other_exception_codes[index] =
                    fault.host_code;
                ++context->veh_other_exception_code_counts[index];
                recorded = true;
                break;
            }
        }
        if (!recorded)
        {
            ++context->veh_other_exception_code_overflow;
        }
    }
    if (context->veh_single_step_run_length != 0U)
    {
        const std::uint32_t length = context->veh_single_step_run_length;
        context->veh_single_step_run_buckets[SingleStepRunBucket(length)] += 1U;
        ++context->veh_single_step_run_total;
        context->veh_single_step_run_max =
            context->veh_single_step_run_max > length
                ? context->veh_single_step_run_max : length;
        context->veh_single_step_run_length = 0U;
    }
}

bool IsGuestInstructionPointer(const ThreadContext* context,
                               std::uint32_t eip)
{
    if (context == nullptr || context->runtime_size == 0)
    {
        return false;
    }

    const std::uint32_t runtime_end =
        context->runtime_base + context->runtime_size;
    return eip >= context->runtime_base &&
           runtime_end >= context->runtime_base &&
           eip < runtime_end;
}

// Copies the configured range into the buffer reserved before execution began.
// This runs inside the exception handler, so it allocates nothing and reads
// only ranges the guest-range check accepts in full.
void RecordExecutionProbeDump(repiu::platform::GuestCpuContext* win32_context, ThreadContext* context)
{
    const auto& request = context->execution_probe_dump_request;
    auto& result = context->execution_probe_dump_result;
    if (!request.configured || result.captured ||
        result.bytes.size() < request.byte_count)
    {
        return;
    }
    std::uint32_t base = request.absolute_base;
    switch (request.base)
    {
        case ExecutionProbeDumpBase::kEax:
            base = win32_context->Eax;
            break;
        case ExecutionProbeDumpBase::kEbx:
            base = win32_context->Ebx;
            break;
        case ExecutionProbeDumpBase::kEcx:
            base = win32_context->Ecx;
            break;
        case ExecutionProbeDumpBase::kEdx:
            base = win32_context->Edx;
            break;
        case ExecutionProbeDumpBase::kEsi:
            base = win32_context->Esi;
            break;
        case ExecutionProbeDumpBase::kEdi:
            base = win32_context->Edi;
            break;
        case ExecutionProbeDumpBase::kEbp:
            base = win32_context->Ebp;
            break;
        case ExecutionProbeDumpBase::kEsp:
            base = win32_context->Esp;
            break;
        case ExecutionProbeDumpBase::kAbsolute:
        case ExecutionProbeDumpBase::kCount:
            break;
    }
    if (base > UINT32_MAX - request.offset)
    {
        return;
    }
    result.base_address = base + request.offset;
    std::uint32_t source_address = result.base_address;
    if (request.indirect)
    {
        const void* pointer_site = reinterpret_cast<const void*>(
            static_cast<std::uintptr_t>(result.base_address));
        if (!IsGuestRangeReadable(context, pointer_site,
                                  sizeof(std::uint32_t)))
        {
            return;
        }
        std::memcpy(&source_address, pointer_site, sizeof(source_address));
    }
    result.source_address = source_address;
    const void* source = reinterpret_cast<const void*>(
        static_cast<std::uintptr_t>(source_address));
    if (!IsGuestRangeReadable(context, source, request.byte_count))
    {
        return;
    }
    std::memcpy(result.bytes.data(), source, request.byte_count);
    result.byte_count = request.byte_count;
    result.captured = true;
}

// Task 713. One line per arrival, for the first N. Registers, then the first
// sixteen bytes of each register's memory window when a memory offset is set.
// Written with snprintf and one unbuffered write, because this runs inside the
// fault handler.
void LogExecutionProbeArrival(const repiu::platform::GuestCpuContext& registers,
                              ThreadContext* context)
{
    const std::uint32_t arrival = ++context->execution_probe_arrival_count;
    if (arrival > context->execution_probe_log_arrivals)
    {
        return;
    }
    const std::uint32_t values[kExecutionProbeRegisterCount] = {
        registers.Eax, registers.Ebx, registers.Ecx, registers.Edx,
        registers.Esi, registers.Edi, registers.Ebp,
    };
    static const char* const kNames[kExecutionProbeRegisterCount] = {
        "eax", "ebx", "ecx", "edx", "esi", "edi", "ebp",
    };
    char line[1024] = {};
    int length = std::snprintf(
        line, sizeof(line),
        "[repiu-probe-arrival] n=%u eip=0x%08X esp=0x%08X eflags=0x%08X",
        arrival, static_cast<std::uint32_t>(registers.Eip),
        static_cast<std::uint32_t>(registers.Esp),
        static_cast<std::uint32_t>(registers.EFlags));
    for (std::uint32_t index = 0; index < kExecutionProbeRegisterCount &&
         length > 0 && static_cast<std::size_t>(length) < sizeof(line);
         ++index)
    {
        length += std::snprintf(line + length, sizeof(line) - length,
                                " %s=0x%08X", kNames[index], values[index]);
    }
    if (context->execution_probe_memory_offset != 0U)
    {
        for (std::uint32_t index = 0; index < kExecutionProbeRegisterCount &&
             length > 0 && static_cast<std::size_t>(length) < sizeof(line);
             ++index)
        {
            if (values[index] >
                UINT32_MAX - context->execution_probe_memory_offset)
            {
                continue;
            }
            const std::uint32_t address =
                values[index] + context->execution_probe_memory_offset;
            const auto* bytes = reinterpret_cast<const std::uint8_t*>(
                static_cast<std::uintptr_t>(address));
            if (!IsGuestRangeReadable(context, bytes, 16U))
            {
                continue;
            }
            length += std::snprintf(line + length, sizeof(line) - length,
                                    " [%s+off]=", kNames[index]);
            for (std::uint32_t k = 0; k < 16U && length > 0 &&
                 static_cast<std::size_t>(length) + 3U < sizeof(line); ++k)
            {
                length += std::snprintf(line + length, sizeof(line) - length,
                                        "%02X", bytes[k]);
            }
        }
    }
    if (length > 0 && static_cast<std::size_t>(length) + 1U < sizeof(line))
    {
        line[length++] = '\n';
        repiu::platform::WriteHostErrorStream(
            line, static_cast<std::size_t>(length));
    }
}

void RecordExecutionProbe(repiu::platform::GuestCpuContext* win32_context, ThreadContext* context)
{
    if (win32_context == nullptr || context == nullptr ||
        !context->execution_probe_configured ||
        win32_context->Eip < context->runtime_base ||
        static_cast<std::uint32_t>(win32_context->Eip) -
                context->runtime_base != context->execution_probe_offset)
    {
        return;
    }
    if (context->execution_probe_log_arrivals != 0U)
    {
        LogExecutionProbeArrival(*win32_context, context);
    }
    if (context->execution_probe_hit)
    {
        return;
    }
    context->execution_probe_hit = true;
    CopySnapshotFromContextRecord(*win32_context,
                                  &context->execution_probe_snapshot);
    const void* stack = reinterpret_cast<const void*>(
        static_cast<std::uintptr_t>(win32_context->Esp));
    if (IsGuestRangeReadable(context, stack,
                             sizeof(context->execution_probe_stack)))
    {
        std::memcpy(context->execution_probe_stack, stack,
                    sizeof(context->execution_probe_stack));
    }
    const std::uint32_t register_values[
        kExecutionProbeRegisterCount] = {
            win32_context->Eax,
            win32_context->Ebx,
            win32_context->Ecx,
            win32_context->Edx,
            win32_context->Esi,
            win32_context->Edi,
            win32_context->Ebp,
        };
    for (std::uint32_t index = 0;
         index < kExecutionProbeRegisterCount; ++index)
    {
        auto& window = context->execution_probe_memory[index];
        if (register_values[index] >
            UINT32_MAX - context->execution_probe_memory_offset)
        {
            continue;
        }
        window.address = register_values[index] +
                         context->execution_probe_memory_offset;
        const void* source = reinterpret_cast<const void*>(
            static_cast<std::uintptr_t>(window.address));
        if (IsGuestRangeReadable(
                context, source, kExecutionProbeMemoryByteCount))
        {
            std::memcpy(window.bytes, source, sizeof(window.bytes));
            window.valid = true;
        }
    }
    RecordExecutionProbeDump(win32_context, context);
}

void RecordExecutionTrace(repiu::platform::GuestCpuContext* win32_context, ThreadContext* context)
{
    if (win32_context == nullptr || context == nullptr ||
        !context->execution_trace_configured ||
        win32_context->Eip < context->runtime_base)
    {
        return;
    }
    const std::uint32_t offset = static_cast<std::uint32_t>(win32_context->Eip) -
        context->runtime_base;
    if (offset < context->execution_trace_start_offset ||
        offset > context->execution_trace_end_offset)
    {
        return;
    }
    ExecutionTraceEntry entry{};
    entry.sequence = context->execution_trace_hit_count;
    entry.eip = static_cast<std::uint32_t>(win32_context->Eip);
    entry.esp = static_cast<std::uint32_t>(win32_context->Esp);
    const void* value_address = reinterpret_cast<const void*>(
        static_cast<std::uintptr_t>(entry.esp + context->execution_trace_esp_offset));
    if (IsGuestRangeReadable(context, value_address, sizeof(entry.value_at_esp_offset)))
    {
        std::memcpy(&entry.value_at_esp_offset, value_address,
                    sizeof(entry.value_at_esp_offset));
    }
    const std::uint32_t slot = context->execution_trace_hit_count %
        kExecutionTraceCapacity;
    context->execution_trace[slot] = entry;
    ++context->execution_trace_hit_count;
    // Task 598. The normal trace ring is reported after a normal attempt
    // return, but a Linux terminal fault exits before that summary. An explicit
    // opt-in line preserves the capture without changing trace/reentry state.
    if (context->execution_trace_log_enabled)
    {
        char line[256] = {};
        const int length = std::snprintf(
            line, sizeof(line),
            "[repiu-exec-trace] #%u eip=0x%08X esp=0x%08X stack=0x%08X "
            "eax=0x%08X ebx=0x%08X edx=0x%08X ebp=0x%08X "
            "eflags=0x%08X\n",
            static_cast<unsigned>(entry.sequence),
            static_cast<unsigned>(entry.eip),
            static_cast<unsigned>(entry.esp),
            static_cast<unsigned>(entry.value_at_esp_offset),
            static_cast<unsigned>(win32_context->Eax),
            static_cast<unsigned>(win32_context->Ebx),
            static_cast<unsigned>(win32_context->Edx),
            static_cast<unsigned>(win32_context->Ebp),
            static_cast<unsigned>(win32_context->EFlags));
        if (length > 0)
        {
            repiu::platform::WriteHostErrorStream(
                line,
                static_cast<std::size_t>(length) < sizeof(line)
                    ? static_cast<std::size_t>(length)
                    : sizeof(line) - 1U);
        }
    }
}

// ResolveSegmentLinearRange promoted to external linkage (relocated out of the
// anonymous namespace) for dpmi_mscdex_services.cpp and segment handlers.
bool ResolveSegmentLinearRange(ThreadContext* context,
                               std::uint16_t selector,
                               std::uint32_t offset,
                               std::uint32_t byte_count,
                               bool writable,
                               std::uint32_t* linear_address)
{
    if (context == nullptr || linear_address == nullptr)
    {
        return false;
    }

    std::uint32_t translated = 0;
    if (!repiu::runtime::TranslateSelectorOffset(
            context->selector_table,
            selector,
            offset,
            byte_count,
            &translated))
    {
        translated = offset;
    }
    void* pointer = reinterpret_cast<void*>(
        static_cast<std::uintptr_t>(translated));
    const bool valid = writable
        ? IsGuestRangeWritable(context, pointer, byte_count)
        : IsGuestRangeReadable(context, pointer, byte_count);
    if (!valid)
    {
        translated = offset;
        pointer = reinterpret_cast<void*>(
            static_cast<std::uintptr_t>(translated));
        const bool direct_valid = writable
            ? IsGuestRangeWritable(context, pointer, byte_count)
            : IsGuestRangeReadable(context, pointer, byte_count);
        if (!direct_valid)
        {
            return false;
        }
    }
    *linear_address = translated;
    return true;
}


// Shared trap/record substrate promoted to external linkage (relocated out of
// the anonymous namespace) for use by DOS/DPMI/MSCDEX/segment modules.
// Declared in execution_internal.h.

void RecoverFromHleExit(repiu::platform::GuestCpuContext* win32_context,
                        ThreadContext* thread_context)
{
    thread_context->process_exit = true;
    thread_context->returned = true;
    if (runtime::execution_model::RunsLongModeCodeCache())
    {
        // Linux x64 leaves host RSP on the cache call frame. The signal
        // callback installs the full-width host RIP for the cache's exit, so
        // writing the 32-bit guest EIP to a recovery symbol here would be
        // wrong.
        win32_context->EFlags &= ~0x00000100U;
        win32_context->EFlags &= ~0x00000400U;
        return;
    }
    if (thread_context->use_guest_stack)
    {
        RecoverToHost(win32_context, thread_context);
    }
    else
    {
        win32_context->Eip = static_cast<decltype(win32_context->Eip)>(
            reinterpret_cast<std::uintptr_t>(&RecoverHostStackException));
    }
}

void RecordHandledDosInterrupt(ThreadContext* context,
                               std::uint8_t vector,
                               std::uint16_t ax)
{
    if (context == nullptr)
    {
        return;
    }

    // Task 523 diagnostic (temporary): the sequence of DOS/DPMI services the
    // guest actually got, so two hosts can be diffed. The last one before a
    // divergence names the call that answered differently.
    if (([]() { static const char* const dos_int_trace = std::getenv("REPIU_DOS_INT_TRACE"); return dos_int_trace; }()) != nullptr)
    {
        char line[96] = {};
        const int length = std::snprintf(
            line, sizeof(line), "[repiu-dos-int] #%u int=%02X ax=%04X\n",
            static_cast<unsigned>(context->handled_dos_interrupt_count + 1U),
            static_cast<unsigned>(vector), static_cast<unsigned>(ax));
        if (length > 0)
        {
            repiu::platform::WriteHostErrorStream(
                line, static_cast<std::size_t>(length));
        }
    }
    ++context->handled_dos_interrupt_count;
    if (vector == 0x21U)
    {
        ++context->handled_dos_int21_count;
        ++context->handled_dos_int21_ah_counts[ax >> 8];
    }
    context->last_dos_interrupt_vector = vector;
    context->last_dos_interrupt_ah = static_cast<std::uint8_t>(ax >> 8);
    context->last_dos_interrupt_ax = ax;
    ++context->handled_dos_interrupt_ah_counts[ax >> 8];
}

void RecordLowMemoryAccess(repiu::platform::GuestCpuContext* win32_context,
                           ThreadContext* context,
                           std::uint8_t opcode,
                           std::uint32_t destination,
                           std::uint32_t value)
{
    if (win32_context == nullptr || context == nullptr)
    {
        return;
    }

    ++context->handled_low_memory_access_count;
    context->last_low_memory_access_address =
        static_cast<std::uint32_t>(win32_context->Eip);
    context->last_low_memory_access_opcode = opcode;
    context->last_low_memory_access_esi = win32_context->Esi;
    context->last_low_memory_access_edi = win32_context->Edi;
    context->last_low_memory_access_destination = destination;
    context->last_low_memory_access_value = value;
}

std::uint16_t ReadGuestSegmentSelector(const ThreadContext& context,
                                       std::uint8_t segment_register,
                                       const repiu::platform::GuestCpuContext* win32_context)
{
    if (segment_register == 1U)
    {
        std::uint16_t selector = 0U;
        if (win32_context != nullptr &&
            repiu::runtime::FindSelectorForLinearAddress(
                context.selector_table,
                static_cast<std::uint32_t>(win32_context->Eip),
                &selector))
        {
            return selector;
        }
        return 0U;
    }

    std::uint16_t shadow = 0;
    if (context.shadow_selectors != nullptr && segment_register < 6U)
    {
        // Task i018. The shadow block is the virtual segment state: the
        // guarded load slot writes it natively for accepted selector
        // switches, so the context mirror may be one switch behind.
        shadow = context.shadow_selectors->selectors[segment_register];
    }
    else
    {
        switch (segment_register)
        {
            case 0:
                shadow = context.guest_es;
                break;
            case 2:
                shadow = context.guest_ss;
                break;
            case 3:
                shadow = context.guest_ds;
                break;
            case 4:
                shadow = context.guest_fs;
                break;
            case 5:
                shadow = context.guest_gs;
                break;
            default:
                return 0;
        }
    }
    if (win32_context == nullptr)
    {
        return shadow;
    }

    std::uint16_t physical = shadow;
    std::uint32_t host_entry = 0;
    switch (segment_register)
    {
        case 0:
            physical = static_cast<std::uint16_t>(win32_context->SegEs);
            host_entry = g_recovery_host_es;
            break;
        case 2:
            // Linux x64 does not report SS in ucontext_t. The logical guest
            // selector is therefore authoritative once the execution setup
            // has seeded it from the LE stack object.
            if (runtime::execution_model::RunsLongModeCodeCache() && shadow != 0)
            {
                return shadow;
            }
            return static_cast<std::uint16_t>(win32_context->SegSs);
        case 3:
            physical = static_cast<std::uint16_t>(win32_context->SegDs);
            host_entry = g_recovery_host_ds;
            break;
        case 4:
            physical = static_cast<std::uint16_t>(win32_context->SegFs);
            host_entry = g_recovery_host_fs;
            break;
        case 5:
            physical = static_cast<std::uint16_t>(win32_context->SegGs);
            host_entry = g_recovery_host_gs;
            break;
        default:
            return shadow;
    }
    if (physical == shadow)
    {
        return physical;
    }
    if (context.shared_live_telemetry != nullptr)
    {
        repiu::platform::AtomicIncrement(
            &context.shared_live_telemetry->seg_divergence_count);
        repiu::platform::AtomicExchange(
            &context.shared_live_telemetry->seg_divergence_reg_physical,
            static_cast<long>((static_cast<std::uint32_t>(segment_register)
                               << 16) | physical));
        repiu::platform::AtomicExchange(
            &context.shared_live_telemetry->seg_divergence_shadow,
            static_cast<long>(shadow));
    }
    // A selector that only exists in the software SelectorTable (for example
    // the DOS/4G client-data selector installed by INT 21h AX=FF00h) can
    // never be loaded into the hardware register, so the physical value
    // still holding the host's entry-time selector means the HLE shadow is
    // authoritative. A physical value that moved away from the host entry
    // selector was loaded by the guest (natively or via write-through) and
    // wins over a possibly stale shadow.
    if (physical == static_cast<std::uint16_t>(host_entry & 0xFFFFU) &&
        shadow != 0)
    {
        const repiu::runtime::GuestDescriptor* descriptor =
            repiu::runtime::FindDescriptor(context.selector_table, shadow);
        if (descriptor != nullptr && descriptor->present)
        {
            return shadow;
        }
    }
    return physical;
}

// NoteSuccessfulAotGuestWrite (AOT write hook) promoted to external linkage
// for guest_memory_access.cpp; relocated out of the anonymous namespace.
bool NoteSuccessfulAotGuestWrite(ThreadContext* context,
                                 std::uint32_t destination,
                                 std::uint32_t byte_count)
{
    if (context == nullptr || context->aot_placement == nullptr ||
        !context->aot_translation_thread.valid || byte_count == 0U)
    {
        return true;
    }
    constexpr std::uint32_t kPageMask = 0xFFFFF000U;
    const std::uint32_t first_page = destination & kPageMask;
    const std::uint64_t write_end =
        static_cast<std::uint64_t>(destination) + byte_count;
    if (write_end == 0U ||
        write_end > static_cast<std::uint64_t>(
                        std::numeric_limits<std::uint32_t>::max()) + 1U)
    {
        return false;
    }
    const std::uint32_t last_page = static_cast<std::uint32_t>(
        (write_end - 1U) & kPageMask);
    bool relevant = AotGuestRangeHasActiveTranslation(
        *context->aot_placement, destination, byte_count);
    for (std::uint32_t page = first_page;
         !relevant; page += 0x1000U)
    {
        relevant = IsAotGuestPageRetired(
                       *context->aot_placement, page) ||
            IsAotGuestPageQuarantined(
                *context->aot_placement, page);
        if (page == last_page || page > 0xFFFFEFFFU)
        {
            break;
        }
    }
    if (!relevant)
    {
        return true;
    }
    const std::uint32_t source = AotGuestAddressForExecutionAddress(
        context,
        context->exception_dispatch_last_eip.load(
            std::memory_order_relaxed));
    bool observed = false;
    bool retired_provenance = false;
    for (std::uint32_t page = first_page;; page += 0x1000U)
    {
        const std::uint32_t range_begin = std::max(destination, page);
        const std::uint64_t page_end =
            static_cast<std::uint64_t>(page) + 0x1000U;
        const std::uint32_t range_size = static_cast<std::uint32_t>(
            std::min(write_end, page_end) - range_begin);
        const bool active = AotGuestRangeHasActiveTranslation(
            *context->aot_placement, range_begin, range_size);
        retired_provenance = retired_provenance ||
            IsAotGuestPageRetired(*context->aot_placement, page) ||
            IsAotGuestPageQuarantined(
                *context->aot_placement, page);
        if (active)
        {
            const bool writes_own_page = source == 0U ||
                (source & kPageMask) == page;
            // Task 342: quarantine on repetition, not on the first write. See
            // docs/design/20260728-342-quarantine-on-repeat-write.md section 2
            // for why this does not weaken correctness.
            const bool same_page = writes_own_page &&
                ShouldQuarantineWrittenPage(context, page, destination);
            // Task 341: record what quarantines a page, since four of them
            // block 80.24% of post-HLE returns. An unknown source is counted
            // separately because it quarantines by default rather than by
            // evidence of self-modification.
            if (same_page)
            {
                if (source == 0U)
                {
                    ++context->quarantine_unknown_source_count;
                }
                if (context->quarantine_trace_count <
                    ThreadContext::kQuarantineTraceCapacity)
                {
                    context->quarantine_trace[
                        context->quarantine_trace_count] = {
                        page, source, destination, byte_count};
                }
                ++context->quarantine_trace_count;
            }
            BumpAotPageRetireAttemptCount(context);
            if (!RequestAotGuestPageRetirement(
                    context, page, same_page))
            {
                context->aot_terminal_failure.store(
                    true, std::memory_order_release);
                return false;
            }
            BumpAotPageRetireSuccessCount(context);
            context->aot_last_retired_page.store(
                page, std::memory_order_relaxed);
            if (context->shared_live_telemetry != nullptr)
            {
                repiu::platform::AtomicExchange(
                    &context->shared_live_telemetry->aot_last_retired_page,
                    static_cast<long>(page));
            }
            if (same_page)
            {
                BumpAotQuarantineCount(context);
            }
            observed = true;
        }
        if (page == last_page || page > 0xFFFFEFFFU)
        {
            break;
        }
    }
    // Task 741: any completed guest code write may change what the reentry
    // memo remembered about an address.
    ++context->aot_reentry_memo_generation;
    if (observed || retired_provenance)
    {
        context->aot_code_write_count.fetch_add(
            1, std::memory_order_relaxed);
        context->aot_last_code_write_source.store(
            source, std::memory_order_relaxed);
        context->aot_last_code_write_destination.store(
            destination, std::memory_order_relaxed);
        if (context->shared_live_telemetry != nullptr)
        {
            repiu::platform::AtomicExchange(
                &context->shared_live_telemetry->aot_last_code_write_source,
                static_cast<long>(source));
            repiu::platform::AtomicExchange(
                &context->shared_live_telemetry
                     ->aot_last_code_write_destination,
                static_cast<long>(destination));
        }
    }
    return true;
}

// Task 751. One clock for every reading of the guest's `cli` hold.
std::uint64_t GuestCliHoldClockNanoseconds()
{
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

namespace
{

// Task 759. The continuation after a boundary the engine handled has moved
// EIP off `boundary_eip`. On the direct model the guest resumes at the new
// address as it is. On the cache model a raw guest address cannot be run, so
// the continuation enters the cache (Tasks 702 and 703), or the original bytes
// where they read the same in long mode; false when neither is possible, and
// the event has to be declined.
bool ResumeHandledBoundary(repiu::platform::GuestCpuContext* const win32_context,
                           ThreadContext* const context,
                           const std::uint32_t boundary_eip)
{
    if (!runtime::execution_model::RunsLongModeCodeCache())
    {
        return true;
    }
    if (context->aot_placement != nullptr &&
        static_cast<std::uint32_t>(win32_context->Eip) != boundary_eip)
    {
        const bool resumed = TryResumeAotAfterHandledHle(
            win32_context,
            context,
            boundary_eip,
            AotHleResumeOrigin::kHandledGuestBoundary);
        if (!resumed &&
            !CanResumeLinuxX64LegacyTarget(
                context,
                static_cast<std::uint32_t>(win32_context->Eip)))
        {
            win32_context->EFlags &= ~0x00000100U;
            return false;
        }
    }
    return true;
}

}  // namespace

bool CanEnterTimerInterruptHandler(ThreadContext* const context)
{
    // Task 759. On the direct model the handler runs as it is.
    if (!runtime::execution_model::RunsLongModeCodeCache())
    {
        return true;
    }
    if (context == nullptr || context->aot_placement == nullptr)
    {
        return true;
    }
    const DpmiInterruptVectorShadow& shadow =
        context->dpmi_interrupt_vectors[0x08];
    if (!shadow.valid)
    {
        // Nothing to enter; the injection itself accounts for it.
        return true;
    }
    std::uint32_t cache_address = 0U;
    return FindAotCacheAddress(*context->aot_placement, shadow.offset,
                               &cache_address) ||
        CanResumeLinuxX64LegacyTarget(context, shadow.offset);
}

namespace
{

// Task 762. The return pad: one reserved page that is never committed, so
// executing its first byte faults with EIP naming it. No bytes are placed
// there, which is why the two hosts' different EIP after an `int3` does not
// come into it. Reserved at first use, one per process, never released.
// Zero when the host refused or the address does not fit a guest register.
std::uint32_t TimerReturnPadAddress()
{
    static const std::uint32_t address = [] {
        const repiu::platform::MemoryReservation reservation =
            repiu::platform::ReserveMemory(
                nullptr, repiu::platform::SystemPageSize(), false,
                repiu::platform::MemoryProtection::kNoAccess);
        if (!reservation.valid || reservation.base == nullptr)
        {
            return std::uint32_t{0U};
        }
        const std::uintptr_t base =
            reinterpret_cast<std::uintptr_t>(reservation.base);
        return base <= std::numeric_limits<std::uint32_t>::max()
            ? static_cast<std::uint32_t>(base)
            : std::uint32_t{0U};
    }();
    return address;
}

// Task 762. A timer handler has returned: its `iret` went to the return pad
// and faulted there. Puts the interrupted code back where it was, records the
// return as the cache model's `iret` HLE does, and leaves code that runs in
// place under the trace, so that the next tick has somewhere to go in.
bool HandleTimerReturnPad(repiu::platform::GuestCpuContext* const win32_context,
                          ThreadContext* const context)
{
    if (context->timer_return_pad.pushed_total == 0U ||
        static_cast<std::uint32_t>(win32_context->Eip) !=
            TimerReturnPadAddress())
    {
        return false;
    }
    TimerReturnRecord record;
    if (!PopTimerReturn(&context->timer_return_pad,
                        static_cast<std::uint32_t>(win32_context->Esp),
                        &record))
    {
        return false;
    }
    win32_context->Eip = record.return_eip;
    context->enable_single_step_trace = record.single_step_trace;
    context->aot_legacy_fallback = record.legacy_fallback;
    context->aot_reentry_pending = record.reentry_pending;
    NoteGuestIret(&context->pic_timer_in_service, record.frame_esp,
                  GuestCliHoldClockNanoseconds());
    if (JammaReplayFrameEndEnabled())
    {
        context->jamma_input_timeline.EndTimerInterrupt(record.frame_esp);
    }
    const bool in_place =
        IsGuestInstructionPointer(context, record.return_eip) &&
        !IsAotCacheAddress(context, record.return_eip);
    if (in_place && context->aot_placement != nullptr)
    {
        // In place there is no safe point, so the code goes back to where
        // there are some: the next step asks for its cache entry, and only
        // what has none goes on under the trace.
        context->aot_reentry_pending = true;
        context->aot_legacy_fallback = false;
        context->enable_single_step_trace = true;
        win32_context->EFlags |= 0x00000100U;
        ++context->timer_return_pad.supervised_total;
    }
    else if (in_place && context->enable_single_step_trace)
    {
        // No cache on this backend: the trace the code was under goes on.
        win32_context->EFlags |= 0x00000100U;
        ++context->timer_return_pad.supervised_total;
    }
    else
    {
        win32_context->EFlags &= ~0x00000100U;
    }
    // The next owed tick may go in, as after an `iret` the engine emulated
    // (Tasks 747 and 751); the chain is bounded there.
    InjectPendingInterrupts(win32_context, context,
                            TimerInjectionSite::kAfterHandlerReturn);
    return true;
}

// Task i018. Whether an INT8 injection on the direct model enters the
// handler's cache block (default) or its raw guest address as before.
// `REPIU_TIMER_HANDLER_CACHE_ENTRY=0|off|false` keeps the old entry.
bool TimerHandlerCacheEntryEnabled()
{
    static const bool enabled = [] {
        const char* const value = std::getenv("REPIU_TIMER_HANDLER_CACHE_ENTRY");
        if (value == nullptr || *value == '\0')
        {
            return true;
        }
        return std::strcmp(value, "0") != 0 && std::strcmp(value, "off") != 0 &&
            std::strcmp(value, "false") != 0;
    }();
    return enabled;
}

// Task 366 counts the deferral; issue #58 also records which check made it.
void DeferTimerTick(ThreadContext* context, TimerDeferReason reason)
{
    RecordTimerTickDeferred(&context->timer_tick_delivery);
    if (TimerLatencyTraceEnabled())
    {
        RecordTimerTrace(TimerTraceEvent::kDefer, TimerTraceNowNanoseconds(),
                         static_cast<std::uint32_t>(reason));
    }
}

}  // namespace

std::uint32_t InjectPendingInterrupts(repiu::platform::GuestCpuContext* win32_context,
                                      ThreadContext* context,
                                      const TimerInjectionSite site)
{
    // Task 735. An emulated `sti` asks for the next attempt, wherever the
    // dispatcher makes it; an attempt with nothing owed still consumes it.
    const bool at_sti = std::exchange(
        context->pic_timer_in_service.sti_request, false);
    if (!context->timer_interrupt_pending.load(std::memory_order_acquire))
    {
        return 0U;
    }

    TimerTickDeliveryGuard timer_guard(
        &context->timer_tick_delivery_lock);
    if (!context->timer_interrupt_pending.load(std::memory_order_acquire))
    {
        return 0U;
    }

    const DpmiInterruptVectorShadow& shadow = context->dpmi_interrupt_vectors[0x08];
    if (!shadow.valid)
    {
        context->timer_interrupt_pending.store(false, std::memory_order_relaxed);
        ClearAotTimerSafePointRequest(context);
        context->timer_interrupt_due_ticks.store(
            0U, std::memory_order_relaxed);
        // Task 366: delivery is abandoned with no vector to reach, so the owed
        // ticks are accounted rather than silently vanishing from the identity.
        RecordTimerTickBacklogCleared(&context->timer_tick_delivery);
        context->jamma_input_timeline.ClearTimerTicks();
        return 0U;
    }

    // Task 366: these two are delays, not losses -- the pending state survives
    // and the tick lands at the next safe moment. Counting them separately keeps
    // them out of the loss figure.
    if (!IsGuestInstructionPointer(context, static_cast<std::uint32_t>(win32_context->Eip)) &&
        !IsAotCacheAddress(context, static_cast<std::uint32_t>(win32_context->Eip)))
    {
        DeferTimerTick(context, TimerDeferReason::kNotGuestCode);
        return 0U;
    }

    if ((win32_context->EFlags & kEFlagsInterruptEnable) == 0U)
    {
        DeferTimerTick(context, TimerDeferReason::kInterruptFlag);
        return 0U;
    }

    // Task 751. The test above sees only what the host context carries; the
    // guest's `cli` is kept by the engine.
    if (GuestCliHoldEnabled() &&
        GuestCliBlocksInjection(&context->pic_timer_in_service,
                                GuestCliHoldClockNanoseconds()))
    {
        DeferTimerTick(context, TimerDeferReason::kGuestCli);
        return 0U;
    }

    // Task 735. The IF test above cannot see a guest `cli`: user mode keeps IF
    // set in the host context whatever the guest asked for. The PIC's
    // in-service bit is what keeps IRQ0 out of its own handler on the real
    // machine, and without it pumpitea's 51.9 kHz phase nested injected
    // frames inside the ISR until the guest stack overflowed. The policy also
    // bounds delivery at `sti` so a closing `sti` cannot chain handlers.
    if (PicTimerInServiceEnabled() &&
        PicTimerBlocksInjection(&context->pic_timer_in_service,
                                static_cast<std::uint32_t>(win32_context->Esp),
                                at_sti))
    {
        DeferTimerTick(context, TimerDeferReason::kInService);
        return 0U;
    }

    // Task 751. Last of the waits, because a false answer counts a delivery.
    // The checks that remain below only fail for an address the guest's code
    // selectors do not cover, which a handler's return target is not.
    // 838.0965 ns is one count of the PIT's 1,193,182 Hz input.
    const std::uint64_t tick_period_ns =
        static_cast<std::uint64_t>(context->pit_channel0.snapshot().divisor) *
        838096ULL / 1000ULL;
    if (PostReturnChainBlocksInjection(&context->pic_timer_in_service, site,
                                       GuestCliHoldClockNanoseconds(),
                                       tick_period_ns))
    {
        DeferTimerTick(context, TimerDeferReason::kPostReturnChain);
        return 0U;
    }

    // Task 735. TF in this context is the engine's trace flag, set by the
    // dispatcher before HLE handling, never the guest's. Pushed into the frame
    // it came back with the handler's `iret` -- natively on Win32 -- and a
    // stray single step escaped from ordinary guest code.
    std::uint32_t eflags = win32_context->EFlags & ~0x00000100U;
    std::uint32_t segcs = win32_context->SegCs;
    std::uint32_t eip = win32_context->Eip;
    if (runtime::execution_model::RunsLongModeCodeCache())
    {
        // Task 714. On this host the interrupt frame holds a guest address, always.
        //
        // At a handled boundary EIP already is one. At an AOT timer safe point it is
        // not: EIP is the cache byte after the int3. That broke delivery twice over.
        // Looking a cache address up in the guest selector table found nothing, so
        // every safe-point tick was deferred and then dropped -- 0 of 3,859 injected
        // in a 30-second pumpit2a run, against 5,667 of 5,719 on Win32, which left
        // the guest's clock about 3.3 times slow. And once the lookup was made to
        // succeed, the frame still carried the cache address, which Task 704's IRETD
        // HLE rightly refuses as a return target outside every guest code
        // descriptor, so the ISR died on its IRET.
        //
        // So the cache address is mapped to the guest instruction it executes -- at
        // a safe point, the backward edge it guards -- and that is used for both the
        // selector and the frame. Returning there re-runs the safe point, finds the
        // request already cleared, and falls into the branch with the guest's flags
        // intact, which is what resuming after the int3 would have done. On i386 the
        // guest's own IRET runs natively and returns to the cache address directly,
        // so that host is left as it was.
        //
        // Task 718: on by default. It was opt-in while runs with it on died at
        // about 28 seconds -- first on the CS-override boundary Task 716 removed,
        // then on the ES fold Task 717 fixed. `=0` restores the old behavior, in
        // which safe-point ticks are deferred and dropped.
        static const bool safe_point_injection =
            runtime::ResolveTimerSafePointInjection(
                std::getenv("REPIU_LINUX_X64_SAFE_POINT_INJECTION"));
        if (safe_point_injection && context->aot_placement != nullptr &&
            IsAotCacheAddress(context, eip))
        {
            std::uint32_t guest_address = 0U;
            if (!FindAotGuestAddress(*context->aot_placement, eip,
                                     &guest_address))
            {
                DeferTimerTick(context, TimerDeferReason::kCacheAddress);
                return 0U;
            }
            eip = guest_address;
        }
        std::uint16_t logical_cs = 0U;
        if (!runtime::FindSelectorForLinearAddress(
                context->selector_table, eip, &logical_cs))
        {
            DeferTimerTick(context, TimerDeferReason::kSelector);
            return 0U;
        }
        const runtime::GuestDescriptor* const code_descriptor =
            runtime::FindDescriptor(context->selector_table, logical_cs);
        if (code_descriptor == nullptr || !code_descriptor->executable)
        {
            DeferTimerTick(context, TimerDeferReason::kSelector);
            return 0U;
        }
        segcs = logical_cs;
    }

    // Task 366: a still-owed tick keeps delivery armed so the backlog drains
    // one interrupt per safe point rather than bursting into the guest stack.
    const bool keep_armed = RecordTimerTickInjected(
        &context->timer_tick_delivery);
    const std::uint32_t interrupt_frame_esp = win32_context->Esp - 12U;
    // Task 762. On the direct model the frame returns to the return pad, and
    // the handler runs without the interrupted code's trace state.
    std::uint32_t frame_eip = eip;
    if (runtime::execution_model::RunsGuestBytesDirectly() &&
        TimerReturnPadEnabled())
    {
        const std::uint32_t pad = TimerReturnPadAddress();
        const TimerReturnRecord record{
            eip, interrupt_frame_esp, context->enable_single_step_trace,
            context->aot_legacy_fallback, context->aot_reentry_pending};
        if (pad != 0U &&
            PushTimerReturn(&context->timer_return_pad, record,
                            static_cast<std::uint32_t>(win32_context->Esp)))
        {
            frame_eip = pad;
            context->enable_single_step_trace = false;
            context->aot_legacy_fallback = false;
            context->aot_reentry_pending = false;
        }
    }
    NotePicTimerInjected(&context->pic_timer_in_service, interrupt_frame_esp,
                         at_sti);
    NoteTimerInjectionTime(&context->pic_timer_in_service,
                           GuestCliHoldClockNanoseconds());
    std::uint64_t tick_due_ns = 0U;
    context->jamma_input_timeline.BeginTimerInterrupt(
        win32_context->Esp, interrupt_frame_esp, eip, &tick_due_ns);
    if (TimerLatencyTraceEnabled())
    {
        RecordTimerTrace(TimerTraceEvent::kInject, TimerTraceNowNanoseconds(),
                         eip, at_sti ? 1U : 0U, tick_due_ns);
    }
    context->timer_interrupt_pending.store(keep_armed,
                                           std::memory_order_relaxed);
    timer_guard.Release();
    if (keep_armed)
    {
        ArmAotTimerSafePoint(context);
    }
    else
    {
        ClearAotTimerSafePointRequest(context);
    }
    const std::uint32_t consumed_ticks =
        context->timer_interrupt_due_ticks.exchange(
            0U, std::memory_order_acq_rel);

    std::uint32_t esp = win32_context->Esp;
    esp -= 4;
    WriteGuestBytes(context, reinterpret_cast<void*>(static_cast<std::uintptr_t>(esp)), &eflags, 4);
    esp -= 4;
    WriteGuestBytes(context, reinterpret_cast<void*>(static_cast<std::uintptr_t>(esp)), &segcs, 4);
    esp -= 4;
    WriteGuestBytes(context, reinterpret_cast<void*>(static_cast<std::uintptr_t>(esp)), &frame_eip, 4);

    win32_context->Esp = esp;
    win32_context->SegCs = shadow.selector;
    win32_context->Eip = shadow.offset;
    // Task i018. On the direct model the handler used to start at its guest
    // address and run natively until its first fault -- pumpitea's INT8 entry
    // helper `mov ds, cs:[abs]` faulted that way on every tick, outside the
    // slot the cache holds for it. Enter the handler's cache block instead
    // when one exists, translating it on first use; the frame above is the
    // same either way, and the return pad takes the IRETD as before.
    if (runtime::execution_model::RunsGuestBytesDirectly() &&
        context->aot_placement != nullptr && TimerHandlerCacheEntryEnabled())
    {
        std::uint32_t handler_cache_address = 0U;
        if (FindAotCacheAddress(*context->aot_placement, shadow.offset,
                                &handler_cache_address) &&
            handler_cache_address != 0U)
        {
            win32_context->Eip = handler_cache_address;
            ++context->timer_handler_entry_cache_count;
        }
        else
        {
            std::uint32_t translated_entry = 0U;
            std::uint32_t added_bytes = 0U;
            if (RequestAotDynamicTranslation(context, shadow.offset,
                                             &translated_entry, &added_bytes) &&
                translated_entry != 0U)
            {
                win32_context->Eip = translated_entry;
                ++context->timer_handler_entry_translated_count;
            }
            else
            {
                ++context->timer_handler_entry_native_count;
            }
        }
    }
    else
    {
        ++context->timer_handler_entry_native_count;
    }
    win32_context->EFlags &= ~(kEFlagsInterruptEnable | 0x100U);

    // The timer tick is injected continuously while the guest runs, so this
    // line floods the console and drowns out everything else. Keep it as an
    // opt-in diagnostic (REPIU_TIMER_INJECT_LOG=1) instead of default output.
    static const bool timer_injection_log_enabled =
        std::getenv("REPIU_TIMER_INJECT_LOG") != nullptr;
    if (timer_injection_log_enabled)
    {
        fprintf(stderr, "[repiu-live] Injected INT 8, jump to %04X:%08X, return frame %08X, ticks=%u\n",
                shadow.selector, shadow.offset, esp,
                context->last_timer_injection_ticks.load(
                    std::memory_order_relaxed));
    }
    return consumed_ticks;
}

// Task 743. Linux x64 has no interpreter: a guest instruction runs either
// from the cache or, under the single-step bridge, as its own bytes when they
// mean the same thing in long mode. When neither holds and the block could
// not be translated, execution cannot continue, and until now the run died
// as an anonymous SIGTRAP. This names the address and its bytes first.
void ReportLinuxX64UntranslatableContinuation(
    ThreadContext* context, const std::uint32_t eip, const char* const stage)
{
    static std::atomic<std::uint32_t> reported{0U};
    if (reported.fetch_add(1U, std::memory_order_relaxed) >= 8U)
    {
        return;
    }
    const auto* const bytes = reinterpret_cast<const std::uint8_t*>(
        static_cast<std::uintptr_t>(eip));
    const bool readable = IsGuestRangeReadable(context, bytes, 8U);
    std::fprintf(
        stderr,
        "[repiu-x64-untranslatable] stage=%s eip=0x%08X bytes=%02X %02X %02X "
        "%02X %02X %02X %02X %02X legacy_fallback=%u reentry_pending=%u "
        "dynamic_attempts=%llu: the block has no translation and its first "
        "instruction is not byte-identical in long mode\n",
        stage, eip,
        readable ? bytes[0] : 0U, readable ? bytes[1] : 0U,
        readable ? bytes[2] : 0U, readable ? bytes[3] : 0U,
        readable ? bytes[4] : 0U, readable ? bytes[5] : 0U,
        readable ? bytes[6] : 0U, readable ? bytes[7] : 0U,
        context->aot_legacy_fallback ? 1U : 0U,
        context->aot_reentry_pending ? 1U : 0U,
        static_cast<unsigned long long>(
            context->aot_dynamic_attempt_count.load(std::memory_order_relaxed)));
}

void NoteVehExitSite(ThreadContext* context, VehExitSite site)
{
    if (context == nullptr)
    {
        return;
    }
    context->last_veh_exit_site = static_cast<std::uint8_t>(site);
    // Task 743: published for the unhandled fault report.
    repiu::platform::repiu_last_veh_exit_site = static_cast<std::uint32_t>(site);
    repiu::platform::repiu_last_veh_exit_eip = context->last_veh_eip;
    // Task 741: `last_veh_eip` is this exception's own EIP, set at the choke
    // point before any handler runs; only breakpoint addresses are in the
    // table, so a miss costs the scan and nothing else.
    if (context->breakpoint_site_census_size != 0U)
    {
        RecordBreakpointSiteExit(context, context->last_veh_eip, site);
    }
}

// Task 410. Constructed at the VEH choke point, before AotHleTranslationScope,
// so it is destroyed after that scope has finished rewriting EIP -- the address
// recorded here is therefore the one the guest actually resumes at. See
// docs/design/20260803-410-veh-exit-site-attribution.md.
struct VehExitRecorder
{
    repiu::platform::GuestCpuContext* win32_context;
    ThreadContext* context;
    bool arena_single_step;
    std::uint32_t fault_eip;
    repiu::platform::FaultKind fault_kind;
    std::uint32_t entry_eip;
    std::uint32_t entry_esp;

    VehExitRecorder(repiu::platform::GuestCpuContext* wc,
                    ThreadContext* ctx,
                    const repiu::platform::FaultEvent& fault,
                    bool arena_step)
        : win32_context(wc),
          context(ctx),
          arena_single_step(arena_step),
          fault_eip(fault.instruction_address),
          fault_kind(fault.kind),
          entry_eip(static_cast<std::uint32_t>(wc->Eip)),
          entry_esp(static_cast<std::uint32_t>(wc->Esp))
    {
    }

    VehExitRecorder(const VehExitRecorder&) = delete;
    VehExitRecorder& operator=(const VehExitRecorder&) = delete;

    ~VehExitRecorder()
    {
        const std::uint32_t exit_eip =
            static_cast<std::uint32_t>(win32_context->Eip);
        context->last_veh_exit_eip = exit_eip;
        // Same bit order as the port I/O entry sample, so the two can be read
        // against each other without a conversion table.
        context->last_veh_exit_flags = static_cast<std::uint8_t>(
            (IsAotCacheAddress(context, exit_eip) ? 0x01U : 0U) |
            (((win32_context->EFlags & 0x00000100U) != 0U) ? 0x02U : 0U) |
            (context->aot_reentry_pending ? 0x04U : 0U) |
            (context->aot_legacy_fallback ? 0x08U : 0U) |
            (context->enable_single_step_trace ? 0x10U : 0U));
        trampoline_model::TraceGuestEntry(
            context, fault_eip, fault_kind, entry_eip, exit_eip,
            entry_esp,
            static_cast<std::uint32_t>(win32_context->Esp),
            static_cast<std::uint32_t>(win32_context->EFlags));
        if (!arena_single_step)
        {
            return;
        }
        // Task 409's lesson: count the population, not just the first sample.
        // The total is kept separately so `sum(counts) == total` can be checked
        // rather than assumed.
        ++context->veh_arena_single_step_count;
        const std::uint32_t site = context->last_veh_exit_site;
        if (site < kVehExitSiteCount)
        {
            ++context->veh_arena_single_step_exit_site_counts[site];
        }
    }
};

struct AotHleTranslationScope
{
    repiu::platform::GuestCpuContext* win32_context;
    ThreadContext* context;
    std::uint32_t original_aot_eip;
    std::uint32_t guest_eip;
    bool is_aot_exception;

    AotHleTranslationScope(repiu::platform::GuestCpuContext* wc, ThreadContext* ctx)
        : win32_context(wc), context(ctx), original_aot_eip(0U), guest_eip(0U), is_aot_exception(false)
    {
        if (context != nullptr && context->aot_placement != nullptr &&
            IsAotCacheAddress(context, static_cast<std::uint32_t>(win32_context->Eip)))
        {
            original_aot_eip = static_cast<std::uint32_t>(win32_context->Eip);
            std::uint32_t resolved_guest_eip = 0U;
            if (FindAotGuestAddress(*context->aot_placement, original_aot_eip, &resolved_guest_eip))
            {
                guest_eip = resolved_guest_eip;
                win32_context->Eip = guest_eip;
                is_aot_exception = true;
            }
        }
    }

    ~AotHleTranslationScope()
    {
        if (is_aot_exception)
        {
            if (static_cast<std::uint32_t>(win32_context->Eip) != guest_eip)
            {
                std::uint32_t new_cache_address = 0U;
                if (FindAotCacheAddress(*context->aot_placement,
                                        static_cast<std::uint32_t>(win32_context->Eip),
                                        &new_cache_address))
                {
                    win32_context->Eip = new_cache_address;
                }
            }
            else
            {
                win32_context->Eip = original_aot_eip;
            }
        }
    }
};

// Task 296: syscall-free plausibility gate for the exception-dispatch hot path.
// A real Windows EXCEPTION_POINTERS/CONTEXT/EXCEPTION_RECORD pointer is always
// well above the 64KB NULL-reserve region and pointer-aligned; the observed
// corruption (ContextRecord=0x23, a selector value used as a pointer) fails
// both tests. This runs on every VEH dispatch (up to ~166K/s during
// single-step-heavy phases), so it must stay branch-only -- the authoritative
// VirtualQuery check below is reserved for the rare implausible case.
static inline bool IsPlausibleHostPointer(const void* pointer)
{
    const std::uintptr_t value = reinterpret_cast<std::uintptr_t>(pointer);
    return value >= 0x10000U && (value & 0x3U) == 0U;
}

// Task 503d-15: what used to be a hand-written readability walk here is now
// repiu::platform::IsRangeReadable, the third of the five classifiers 3b was
// built to collect. It answers the same question -- committed, readable, not a
// guard page, walking region by region -- and the callers below are unchanged
// except in what they call. Still only reached when IsPlausibleHostPointer has
// already flagged a pointer, so its kernel transition stays off the hot path.

// Task 503d-15. From here to the end of DispatchGuestException is the Win32
// delivery path: the structure the kernel fills in, the Task 296 checks that
// it is well formed, and the entry a vectored handler calls. 3d-5 split the
// dispatcher so that everything after this hands control to DispatchGuestFault,
// which is where a Linux fault arrives directly from the 3c handler. There is
// nothing here for the other host to become.
#if defined(_WIN32)
// Task 296: record a malformed EXCEPTION_POINTERS and emit a one-line
// diagnostic. Safe to call even when the embedded pointers are unreadable.
static void RecordMalformedExceptionPointers(ThreadContext* context,
                                             EXCEPTION_POINTERS* exception_info)
{
    const std::uint32_t count =
        context->exception_dispatch_malformed_count.fetch_add(
            1U, std::memory_order_relaxed) +
        1U;
    std::uint32_t bad_context = 0U;
    std::uint32_t bad_record = 0U;
    if (repiu::platform::IsRangeReadable(exception_info, sizeof(EXCEPTION_POINTERS)))
    {
        bad_context = static_cast<std::uint32_t>(
            reinterpret_cast<std::uintptr_t>(exception_info->ContextRecord));
        bad_record = static_cast<std::uint32_t>(
            reinterpret_cast<std::uintptr_t>(exception_info->ExceptionRecord));
    }
    context->exception_dispatch_last_bad_context.store(
        bad_context, std::memory_order_relaxed);
    context->exception_dispatch_last_bad_record.store(
        bad_record, std::memory_order_relaxed);
    fprintf(stderr,
            "[repiu-live] Malformed EXCEPTION_POINTERS at VEH: info=%p "
            "ExceptionRecord=0x%08X ContextRecord=0x%08X (count=%u) -- failing "
            "closed to surface primary exception\n",
            static_cast<void*>(exception_info), bad_record, bad_context, count);
}

// VEH dispatch logic relocated out of the anonymous namespace (external
// linkage) so the thin GuestStackVectoredExceptionHandler entry in
// exception_rescue_win32.cpp can forward to it. Declared in that header.
// Task 503d-5. The Win32-shaped half, and all that is left of it: validate the
// structure Windows handed over, then build the event and let the neutral body
// have it. The validation cannot move down, because once the event exists there
// is nothing left to validate -- and it cannot move into the 3c backend either,
// since it is about pointers only a vectored handler ever sees.
LONG DispatchGuestException(EXCEPTION_POINTERS* exception_info)
{
    ThreadContext* context = g_repiu_active_thread_context;
    if (context == nullptr || exception_info == nullptr)
    {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    // Task 296: Windows can hand the VEH a malformed EXCEPTION_POINTERS (a
    // non-null but unreadable ContextRecord/ExceptionRecord) when a fault is
    // dispatched while the thread runs on the guest stack (TIB stack bounds
    // still point at the host stack). Dereferencing it caused a secondary
    // access violation inside this dispatcher (win32_context->Eip, CONTEXT
    // offset 0xB8) that masked the original guest exception. Validate before
    // any dereference and fail closed so the primary exception propagates to
    // the outer handler and is recorded.
    //
    // The common (valid) case is settled with branch-only arithmetic so the hot
    // exception path pays no kernel transition; only an implausible pointer
    // falls through to the authoritative VirtualQuery check.
    if (!IsPlausibleHostPointer(exception_info) ||
        !IsPlausibleHostPointer(exception_info->ContextRecord) ||
        !IsPlausibleHostPointer(exception_info->ExceptionRecord))
    {
        if (!repiu::platform::IsRangeReadable(exception_info, sizeof(EXCEPTION_POINTERS)) ||
            !repiu::platform::IsRangeReadable(exception_info->ContextRecord,
                                   sizeof(repiu::platform::GuestCpuContext)) ||
            !repiu::platform::IsRangeReadable(exception_info->ExceptionRecord,
                                   sizeof(EXCEPTION_RECORD)))
        {
            RecordMalformedExceptionPointers(context, exception_info);
            return EXCEPTION_CONTINUE_SEARCH;
        }
    }

    repiu::platform::FaultEvent fault =
        repiu::platform::MakeFaultEventFromWin32(exception_info);
    return DispatchGuestFault(fault) ==
            repiu::platform::FaultDisposition::kResume
        ? EXCEPTION_CONTINUE_EXECUTION
        : EXCEPTION_CONTINUE_SEARCH;
}
#endif  // defined(_WIN32)

// Everything below names no Windows type. What reaches it is the fault as 3c
// reports it, whichever host produced it.
repiu::platform::FaultDisposition DispatchGuestFault(
    repiu::platform::FaultEvent& fault)
{
    ThreadContext* context = g_repiu_active_thread_context;
    if (context == nullptr || fault.registers == nullptr)
    {
        return repiu::platform::FaultDisposition::kNotHandled;
    }
    if (AotFfTargetTimingEnabled())
    {
        CompleteAotFfTargetTiming(
            &context->aot_ff_boundary_attribution.target_timing,
            repiu::platform::ReadCycleCounter());
    }
    // Task 581. Hooked at this choke point rather than in any one handler,
    // because a fault raised inside the cache is precisely what no handler
    // claims -- that is the gap Task 580 found. The reverse address-map lookup
    // sits behind the watch's own gate.
    if (context->aot_placement != nullptr)
    {
        RecordGuestAddressWatchCacheFault(
            *context->aot_placement,
            static_cast<std::uint32_t>(fault.registers->Eip),
            fault.registers);
        // Task 597. The byte reporter identifies the host instruction; this
        // opt-in line identifies whether that address belongs to a registered
        // guest instruction range. Keep it outside the normal path because a
        // reverse lookup and formatted output are diagnostic-only costs.
        const bool trace_aot_fault =
            fault.kind == repiu::platform::FaultKind::kAccessViolation ||
            (fault.kind == repiu::platform::FaultKind::kBreakpoint &&
             IsAotCacheAddress(
                 context,
                 static_cast<std::uint32_t>(fault.registers->Eip)));
        const std::uint32_t trace_cache_address =
            static_cast<std::uint32_t>(fault.registers->Eip);
        const std::uint32_t trace_address_filter =
            AotFaultTraceAddressFilter();
        if (trace_aot_fault &&
            (trace_address_filter == 0U ||
             trace_address_filter == trace_cache_address) &&
            std::getenv("REPIU_AOT_FAULT_TRACE") != nullptr)
        {
            const std::uint32_t cache_address = trace_cache_address;
            std::uint32_t guest_address = 0U;
            const bool mapped = FindAotGuestAddress(
                *context->aot_placement, cache_address, &guest_address);
            const std::uint32_t previous_cache_address =
                cache_address != 0U ? cache_address - 1U : 0U;
            std::uint32_t previous_guest_address = 0U;
            const bool previous_mapped = cache_address != 0U &&
                FindAotGuestAddress(
                    *context->aot_placement, previous_cache_address,
                    &previous_guest_address);
            std::uint32_t fallthrough_guest_address = 0U;
            const bool fallthrough_mapped =
                runtime::FindAotBlockFallthroughTarget(
                    context->aot_placement->fixups,
                    context->aot_placement->base_address,
                    context->aot_placement->size,
                    cache_address, &fallthrough_guest_address);
            const std::uint32_t guest_address_filter =
                AotFaultTraceGuestAddressFilter();
            const bool guest_filter_matches = guest_address_filter == 0U ||
                (mapped && guest_address == guest_address_filter) ||
                (previous_mapped &&
                 previous_guest_address == guest_address_filter) ||
                (fallthrough_mapped &&
                 fallthrough_guest_address == guest_address_filter);
            if (guest_filter_matches)
            {
                static std::atomic<std::uint32_t> aot_fault_trace_count{0U};
                const std::uint32_t occurrence =
                    aot_fault_trace_count.fetch_add(
                        1U, std::memory_order_relaxed) + 1U;
                if (occurrence <= 16U)
                {
                    const AotCacheBreakpointProvenance provenance =
                        ClassifyAotCacheBreakpointProvenance(
                            *context->aot_placement, cache_address, false);
                    const AotCacheBreakpointProvenance previous_provenance =
                        ClassifyAotCacheBreakpointProvenance(
                            *context->aot_placement, previous_cache_address,
                            false);
                    std::fprintf(
                        stderr,
                        "[repiu-aot-fault] kind=%s cache=0x%08X "
                        "exact=%u/0x%08X/%u previous=%u/0x%08X/%u "
                        "fallthrough=%u/0x%08X size=%u tail=%u maps=%zu "
                        "n=%u\n",
                        fault.kind == repiu::platform::FaultKind::kBreakpoint
                            ? "breakpoint" : "access",
                        cache_address,
                        mapped ? 1U : 0U,
                        guest_address,
                        static_cast<std::uint32_t>(provenance),
                        previous_mapped ? 1U : 0U,
                        previous_guest_address,
                        static_cast<std::uint32_t>(previous_provenance),
                        fallthrough_mapped ? 1U : 0U,
                        fallthrough_guest_address,
                        context->aot_placement->size,
                        context->aot_placement->base_address +
                                context->aot_placement->size >= cache_address
                            ? context->aot_placement->base_address +
                                  context->aot_placement->size - cache_address
                            : 0U,
                        context->aot_placement->address_map.size(),
                        occurrence);
                }
            }
        }
        // Task 586. Hooked beside the watch for the same reason: a fault inside
        // a guard slot's compare is the one no handler claims, and it is also
        // the one that silently strands EFLAGS on the guest stack. Reported
        // once per occurrence and never repaired -- see the header for why.
        AotGuardCompareFault guard_fault;
        if (fault.kind == repiu::platform::FaultKind::kAccessViolation &&
            FindAotGuardCompareFault(
                *context->aot_placement,
                static_cast<std::uint32_t>(fault.registers->Eip),
                &guard_fault))
        {
            context->aot_guard_compare_fault_count.fetch_add(
                1U, std::memory_order_relaxed);
            std::fprintf(
                stderr,
                "[repiu-guard-compare-fault] slot=%s guest=0x%08X "
                "cache=0x%08X seg=%u shadow=0x%08X access=0x%08X "
                "esp=0x%08X\n",
                AotGuardSlotKindName(guard_fault.kind),
                guard_fault.guest_source,
                guard_fault.cache_offset,
                static_cast<unsigned>(guard_fault.segment_register),
                guard_fault.shadow_address,
                fault.access.fault_address,
                static_cast<std::uint32_t>(fault.registers->Esp));
        }
    }

    // Task 323: the single choke point for every exception the guest thread
    // takes -- single steps, INT3 boundaries, and access violations alike.
    // Measured here rather than per handler so no exception path escapes.
    const ExecutionTimeScope veh_time_scope(
        context->execution_time_profile.get(),
        ExecutionTimeBucket::kVehTotal);
    // Task 325: validation, call-step probe, and span/region teardown. Reset
    // explicitly once the prologue ends rather than relying on block scope,
    // because the prologue is not a single lexical block.
    std::optional<ExecutionTimeScope> prologue_time_scope;
    prologue_time_scope.emplace(
        context->execution_time_profile.get(),
        ExecutionTimeBucket::kVehPrologue);

    // Task 582: both exits below are tagged explicitly rather than left to the
    // rotation that resets the tag, because that rotation runs *after* them --
    // reading the field here without writing it would report the previous
    // fault's exit. Moving the rotation up instead would admit other threads'
    // faults into `last_veh_eip` and `last_veh_code`, changing what existing
    // instrumentation means.
    if (repiu::platform::CurrentThreadId() != context->guest_thread_id)
    {
        NoteVehExitSite(context, VehExitSite::kForeignThread);
        return repiu::platform::FaultDisposition::kNotHandled;
    }

    // Task 583. This guard used to ask two questions at once, and they are not
    // the same question:
    //
    //   A. is a guest executing right now?
    //   B. if this fault cannot be serviced, can we unwind to the host?
    //
    // On i386 one fact answers both -- the switch having happened means a guest
    // is running *and* that `host_esp` holds somewhere to return to -- so the
    // fusion never showed. On x64 they diverge: a guest is running and there is
    // no switch to undo, and the fused guard let the "no" to B drag A down with
    // it, refusing the fault before any handler could see it (Task 582).
    //
    // So B moved to where it is actually asked: the two give-up sites below
    // that unwind. Here only A is asked.
    //
    // `host_esp` is what makes `guest_stack_entered` mean what it says: the
    // switch assembly is the only thing that writes it, so a zero means the
    // switch has been set up but has not run yet.
    const bool guest_stack_entered =
        context->active_call_state != nullptr &&
        context->active_call_state->host_esp != 0;
    if (context->use_guest_stack && !guest_stack_entered &&
        !context->cache_entry_active)
    {
        NoteVehExitSite(context, VehExitSite::kGuestStackNotEntered);
        return repiu::platform::FaultDisposition::kNotHandled;
    }

    repiu::platform::GuestCpuContext* win32_context = fault.registers;
    // Task 337: classify every exception exactly once, before any handler can
    // consume it, so the census is exclusive by construction. A single-step run
    // is closed by the next non-single-step exception, which is what makes the
    // bucket "instructions walked under TF between two boundaries".
    RecordVehExceptionCensus(context, fault);
    // Task 407: one slot of history, so a handler can ask what preceded it.
    // Six assignments and one range check on the choke point.
    context->prev_veh_code = context->last_veh_code;
    context->prev_veh_eip = context->last_veh_eip;
    context->prev_veh_in_cache = context->last_veh_in_cache;
    context->last_veh_code =
        static_cast<std::uint32_t>(
            fault.host_code);
    context->last_veh_eip = static_cast<std::uint32_t>(win32_context->Eip);
    context->last_veh_in_cache =
        IsAotCacheAddress(context, context->last_veh_eip);
    // Task 410: the same rotation for who consumed the exception. The site is
    // reset to `kUnknown` here, so a path that resumes without tagging itself
    // reports the omission instead of inheriting the previous answer.
    context->prev_veh_exit_site = context->last_veh_exit_site;
    context->prev_veh_exit_eip = context->last_veh_exit_eip;
    context->prev_veh_exit_flags = context->last_veh_exit_flags;
    context->last_veh_exit_site =
        static_cast<std::uint8_t>(VehExitSite::kUnknown);
    // The population is single steps taken at an arena EIP, which is exactly
    // the class the `0x0301F7CE` question is about. Fixed here rather than in
    // the destructor because handlers rewrite EIP on the way out.
    const VehExitRecorder veh_exit_recorder(
        win32_context, context, fault,
        fault.kind == repiu::platform::FaultKind::kSingleStep &&
            IsGuestInstructionPointer(context, context->last_veh_eip));
    // Task 526: classify what followed the last trap-free reentry.
    //
    // Done at the top of the dispatcher, before any handler can rewrite Eip,
    // because the address the fault arrived at is the whole question. Split by
    // kind because "a fault followed" and "a breakpoint followed" are different
    // claims, and only the second one says the cache still holds an INT3.
    if (context->direct_dispatch_trap_pending)
    {
        context->direct_dispatch_trap_pending = false;
        if (fault.kind == repiu::platform::FaultKind::kSingleStep)
        {
            context->direct_dispatch_step_after.fetch_add(
                1U, std::memory_order_relaxed);
        }
        else if (fault.instruction_address ==
                 context->last_direct_dispatch_target)
        {
            context->direct_dispatch_trap_at_target.fetch_add(
                1U, std::memory_order_relaxed);
        }
        else if (fault.kind == repiu::platform::FaultKind::kBreakpoint)
        {
            context->direct_dispatch_trap_elsewhere.fetch_add(
                1U, std::memory_order_relaxed);
            // Evict the coldest slot when full. Filling eight slots and
            // freezing captures whichever sites the first eight faults hit,
            // which are startup sites, and then reports nothing about the
            // steady state -- the first version of this did exactly that and
            // showed eight addresses at one hit each.
            const std::uint32_t site = fault.instruction_address;
            std::size_t victim = 0;
            bool placed = false;
            for (std::size_t slot = 0;
                 slot < ThreadContext::kDirectTrapSiteCapacity; ++slot)
            {
                if (context->direct_trap_site_address[slot] == site &&
                    context->direct_trap_site_count[slot] != 0U)
                {
                    ++context->direct_trap_site_count[slot];
                    placed = true;
                    break;
                }
                if (context->direct_trap_site_count[slot] <
                    context->direct_trap_site_count[victim])
                {
                    victim = slot;
                }
            }
            if (!placed)
            {
                context->direct_trap_site_address[victim] = site;
                context->direct_trap_site_count[victim] = 1U;
            }
        }
        else
        {
            context->direct_dispatch_other_elsewhere.fetch_add(
                1U, std::memory_order_relaxed);
        }
    }
    UnhandledBreakpointEvidence breakpoint_evidence;
    if (fault.kind == repiu::platform::FaultKind::kBreakpoint)
    {
        breakpoint_evidence =
            CaptureBreakpointEvidence(fault, context);
    }

    // Task 762. Before anything else reads the address: the return pad is
    // neither guest code nor the cache, and every handler below would decline
    // it.
    if (HandleTimerReturnPad(win32_context, context))
    {
        NoteVehExitSite(context, VehExitSite::kTimerReturnPad);
        return repiu::platform::FaultDisposition::kResume;
    }

    if (win32_context->Eip == 0U)
    {
        NoteVehExitSite(context, VehExitSite::kZeroEip);
        win32_context->EFlags &= ~0x00000100U;
        DumpZeroReturnEvidence(
            win32_context, context, "zero-eip-fail-closed",
            context->exception_dispatch_last_eip.load(
                std::memory_order_relaxed));
        CaptureException(fault, context);
        context->guest_return_esp =
            static_cast<std::uint32_t>(win32_context->Esp);
        if (context->use_guest_stack)
        {
            RecordFaultRecoveryProvenance(
                context, fault.instruction_address,
                FaultRecoveryPath::kFaultCallback);
            RecoverToHost(win32_context, context);
        }
        else
        {
            win32_context->Eip = static_cast<decltype(win32_context->Eip)>(
                reinterpret_cast<std::uintptr_t>(&RecoverHostStackException));
        }
        return repiu::platform::FaultDisposition::kResume;
    }

    // Task 275 linear spans use Dr0 only. On the expected boundary, restore
    // debug state and deliberately continue through the normal #DB chain so the
    // boundary instruction receives the exact existing single-step/HLE policy.
    // Any other exception cancels the span and follows the same fail-closed path.
    if (NativeLinearSpanEnabled(context->execution_backend) &&
        context->native_fast_path.linear_span_active)
    {
        const bool reached_boundary =
            fault.kind == repiu::platform::FaultKind::kSingleStep &&
            (static_cast<std::uint32_t>(win32_context->Dr6) & 0x1U) != 0U &&
            static_cast<std::uint32_t>(win32_context->Eip) ==
                context->native_fast_path.linear_span_boundary;
        const bool write_fault_cancel =
            fault.kind == repiu::platform::FaultKind::kAccessViolation &&
            fault.access.valid && fault.access.write_access &&
            IsAotGuestPageWriteWatched(
                context->aot_page_write_watch,
                fault.access.fault_address);
        LeaveNativeLinearSpan(
            win32_context, context, reached_boundary, write_fault_cancel,
            fault.kind, fault.host_code);
    }
    const auto stop_for_aot_terminal_failure = [context, win32_context]() {
        if (!context->aot_terminal_failure.load(std::memory_order_acquire))
        {
            return false;
        }
        win32_context->EFlags &= ~0x00000100U;
        NoteVehExitSite(context, VehExitSite::kTerminalFailureSearch);
        return true;
    };
    prologue_time_scope.reset();
    // Task 325: AOT transfer resolution. The scope closes on every path,
    // including the early returns, because it is an ordinary block-scoped
    // object. Handler order and the interleaved terminal-failure checks are
    // unchanged.
    {
        const ExecutionTimeScope aot_transfer_time_scope(
            context->execution_time_profile.get(),
            ExecutionTimeBucket::kVehAotTransfer);
        if (stop_for_aot_terminal_failure())
        {
            return repiu::platform::FaultDisposition::kNotHandled;
        }
        // One event for the whole AOT handler chain. Every handler below now
        // takes the fault as 3c reports it; this is the last place that still
        // reads Windows' structure, and it goes when the dispatcher's own
        // signature follows them over.
        const repiu::platform::FaultEvent aot_fault =
            fault;
        if (HandleAotGuestCodeWriteCompletion(aot_fault, context))
        {
            NoteVehExitSite(context, VehExitSite::kAotWriteCompletion);
            return repiu::platform::FaultDisposition::kResume;
        }
        if (stop_for_aot_terminal_failure())
        {
            return repiu::platform::FaultDisposition::kNotHandled;
        }
        if (HandleAotGuestCodeWriteFault(aot_fault, context))
        {
            NoteVehExitSite(context, VehExitSite::kAotWriteFault);
            return repiu::platform::FaultDisposition::kResume;
        }
        if (stop_for_aot_terminal_failure())
        {
            return repiu::platform::FaultDisposition::kNotHandled;
        }
        std::uint32_t safe_point_guest_source = 0U;
        if (HandleAotTimerSafePoint(aot_fault, context,
                                    &safe_point_guest_source))
        {
            NoteVehExitSite(context, VehExitSite::kAotTimerSafePoint);
            if (runtime::execution_model::RunsLongModeCodeCache())
            {
                // Task 710. When no tick was due, EIP is the cache byte after the
                // int3 and resuming there is ordinary. When INT 8 was injected, EIP
                // is now the guest's handler -- a raw guest address that long mode
                // cannot run -- so it has to enter the cache the way Tasks 702 and
                // 703 made the Glide gate and the timer chain enter it. The IRET
                // frame's return address is the cache byte after the int3, which
                // the IRETD HLE from Task 704 already knows how to resume.
                const std::uint32_t resumed_eip =
                    static_cast<std::uint32_t>(win32_context->Eip);
                AotCodeCachePlacement* const safe_point_placement =
                    context->aot_placement;
                const bool eip_in_cache =
                    safe_point_placement != nullptr &&
                    resumed_eip >= safe_point_placement->base_address &&
                    resumed_eip < safe_point_placement->base_address +
                        safe_point_placement->size;
                if (safe_point_placement != nullptr && !eip_in_cache)
                {
                    const bool resumed = TryResumeAotAfterHandledHle(
                        win32_context,
                        context,
                        safe_point_guest_source,
                        AotHleResumeOrigin::kHandledGuestBoundary);
                    if (!resumed &&
                        !CanResumeLinuxX64LegacyTarget(context, resumed_eip))
                    {
                        win32_context->EFlags &= ~0x00000100U;
                        return repiu::platform::FaultDisposition::kNotHandled;
                    }
                }
            }
            return repiu::platform::FaultDisposition::kResume;
        }
        if (stop_for_aot_terminal_failure())
        {
            return repiu::platform::FaultDisposition::kNotHandled;
        }
        if (HandleAotReentry(aot_fault, context))
        {
            return repiu::platform::FaultDisposition::kResume;
        }
        if (stop_for_aot_terminal_failure())
        {
            return repiu::platform::FaultDisposition::kNotHandled;
        }
        if (HandleAotIndirectTransfer(aot_fault, context))
        {
            NoteVehExitSite(context, VehExitSite::kAotIndirectTransfer);
            return repiu::platform::FaultDisposition::kResume;
        }
        if (stop_for_aot_terminal_failure())
        {
            return repiu::platform::FaultDisposition::kNotHandled;
        }
        if (HandleAotConditionalTransfer(aot_fault, context))
        {
            NoteVehExitSite(context, VehExitSite::kAotConditionalTransfer);
            return repiu::platform::FaultDisposition::kResume;
        }
        if (stop_for_aot_terminal_failure())
        {
            return repiu::platform::FaultDisposition::kNotHandled;
        }
        if (HandleAotReturnTransfer(aot_fault, context))
        {
            NoteVehExitSite(context, VehExitSite::kAotReturnTransfer);
            return repiu::platform::FaultDisposition::kResume;
        }
        if (stop_for_aot_terminal_failure())
        {
            return repiu::platform::FaultDisposition::kNotHandled;
        }
    }
    if (context->native_fast_path.active)
    {
        const bool returned =
            true &&
            fault.kind == repiu::platform::FaultKind::kSingleStep &&
            win32_context->Eip ==
                context->native_fast_path.return_address &&
            (win32_context->Dr6 & 0x1U) != 0;
        detail::LeaveNativeFastPath(win32_context,
                                    &context->native_fast_path,
                                    returned);
    }
    if (context->shared_live_telemetry != nullptr)
    {
        repiu::platform::AtomicExchange(
            &context->shared_live_telemetry->recovery_host_fs,
            static_cast<long>(g_recovery_host_fs));
        repiu::platform::AtomicExchange(
            &context->shared_live_telemetry->recovery_host_ds,
            static_cast<long>(g_recovery_host_ds));
        repiu::platform::AtomicExchange(
            &context->shared_live_telemetry->recovery_host_es,
            static_cast<long>(g_recovery_host_es));
        repiu::platform::AtomicExchange(
            &context->shared_live_telemetry->recovery_host_gs,
            static_cast<long>(g_recovery_host_gs));
    }
    // Task 503d-15: three codes only Windows raises -- a debugger print and the
    // thread-name exception Visual C++ throws. They stay as numbers rather than
    // becoming a FaultKind, because nothing decides control flow on them beyond
    // passing them through, and there is no counterpart to invent.
    constexpr std::uint32_t kVisualCppThreadNameException = 0x406D1388U;
    constexpr std::uint32_t kDebugPrintExceptionAnsi = 0x40010006U;
    constexpr std::uint32_t kDebugPrintExceptionWide = 0x4001000AU;
    if (true &&
        fault.kind == repiu::platform::FaultKind::kSingleStep &&
        !IsGuestInstructionPointer(
            context, static_cast<std::uint32_t>(win32_context->Eip)))
    {
        // Task 376: this discard is 70.1% of all single-step exceptions and had
        // no counter, because both existing instruments gate on
        // IsGuestInstructionPointer and so never saw it. Behaviour is unchanged;
        // the population is merely named. Counter increments only -- this is the
        // exception path.
        {
            const std::uint32_t discarded_eip =
                static_cast<std::uint32_t>(win32_context->Eip);
            OutOfArenaStepLocation location = OutOfArenaStepLocation::kOther;
            if (context->aot_placement != nullptr &&
                context->aot_placement->placed &&
                discarded_eip >= context->aot_placement->base_address &&
                discarded_eip < context->aot_placement->base_address +
                    context->aot_placement->capacity)
            {
                location = OutOfArenaStepLocation::kAotCodeCache;
            }
            RecordOutOfArenaStep(&context->out_of_arena_step_census,
                                 discarded_eip, location,
                                 context->enable_single_step_trace,
                                 context->aot_reentry_pending);
        }
        NoteVehExitSite(context, VehExitSite::kOutOfArenaStepDiscard);
        win32_context->EFlags &= ~0x00000100U;
        return repiu::platform::FaultDisposition::kResume;
    }
    if (true &&
        (fault.host_code ==
             kDebugPrintExceptionAnsi ||
         fault.host_code ==
             kDebugPrintExceptionWide) &&
        !IsGuestInstructionPointer(
            context, static_cast<std::uint32_t>(win32_context->Eip)))
    {
        NoteVehExitSite(context, VehExitSite::kDebugPrintDiscard);
        return repiu::platform::FaultDisposition::kResume;
    }
    if (true &&
        fault.host_code ==
            kVisualCppThreadNameException &&
        !IsGuestInstructionPointer(
            context, static_cast<std::uint32_t>(win32_context->Eip)))
    {
        NoteVehExitSite(context, VehExitSite::kContinueSearch);
        return repiu::platform::FaultDisposition::kNotHandled;
    }
    // Task 325: nine InterlockedExchange writes plus allocator recording run on
    // every exception regardless of kind, so they are measured as fixed cost.
    std::optional<ExecutionTimeScope> telemetry_time_scope;
    telemetry_time_scope.emplace(
        context->execution_time_profile.get(),
        ExecutionTimeBucket::kVehTelemetry);
    if (context->shared_live_telemetry != nullptr &&
        true)
    {
        repiu::platform::AtomicExchange(
            &context->shared_live_telemetry->last_exception_code,
            static_cast<long>(
                fault.host_code));
        if (IsGuestInstructionPointer(
                context,
                static_cast<std::uint32_t>(win32_context->Eip)))
        {
            repiu::platform::AtomicExchange(
                &context->shared_live_telemetry->last_guest_eip,
                static_cast<long>(win32_context->Eip));
            repiu::platform::AtomicExchange(
                &context->shared_live_telemetry->last_guest_eax,
                static_cast<long>(win32_context->Eax));
            repiu::platform::AtomicExchange(
                &context->shared_live_telemetry->last_guest_ebx,
                static_cast<long>(win32_context->Ebx));
            repiu::platform::AtomicExchange(
                &context->shared_live_telemetry->last_guest_ecx,
                static_cast<long>(win32_context->Ecx));
            repiu::platform::AtomicExchange(
                &context->shared_live_telemetry->last_guest_edx,
                static_cast<long>(win32_context->Edx));
            repiu::platform::AtomicExchange(
                &context->shared_live_telemetry->last_guest_esi,
                static_cast<long>(win32_context->Esi));
            repiu::platform::AtomicExchange(
                &context->shared_live_telemetry->last_guest_edi,
                static_cast<long>(win32_context->Edi));
            repiu::platform::AtomicExchange(
                &context->shared_live_telemetry->last_guest_esp,
                static_cast<long>(win32_context->Esp));
            repiu::platform::AtomicExchange(
                &context->shared_live_telemetry->guest_handler_phase,
                1);
        }
    }
    if (context->enable_single_step_trace)
    {
        win32_context->EFlags |= 0x00000100U;
    }
    ExceptionDispatchScope dispatch_scope(
        context,
        static_cast<std::uint32_t>(win32_context->Eip));
    RecordAllocatorControlFlowException(fault, context);
    telemetry_time_scope.reset();
    const std::uint32_t glide_gate_eip =
        static_cast<std::uint32_t>(win32_context->Eip);
    if (HandleGlideGateBoundary(win32_context, context))
    {
        InjectPendingInterrupts(win32_context, context);
        NoteVehExitSite(context, VehExitSite::kGlideGateBoundary);
        if (!ResumeHandledBoundary(win32_context, context, glide_gate_eip))
        {
            return repiu::platform::FaultDisposition::kNotHandled;
        }
        return repiu::platform::FaultDisposition::kResume;
    }
    // Task 325: the non-Glide boundary gates. Glide keeps its own bucket from
    // Task 323 so the render path stays separable.
    {
        const ExecutionTimeScope boundary_gate_time_scope(
            context->execution_time_profile.get(),
            ExecutionTimeBucket::kVehBoundaryGates);
        const std::uint32_t timer_chain_eip =
            static_cast<std::uint32_t>(win32_context->Eip);
        if (HandleTimerInterruptChainBoundary(win32_context, context))
        {
            NoteVehExitSite(context, VehExitSite::kTimerChainBoundary);
            if (!ResumeHandledBoundary(win32_context, context, timer_chain_eip))
            {
                return repiu::platform::FaultDisposition::kNotHandled;
            }
            return repiu::platform::FaultDisposition::kResume;
        }
        if (HandleLinexeFarTransferBoundary(win32_context, context))
        {
            NoteVehExitSite(context, VehExitSite::kLinexeFarTransferBoundary);
            return repiu::platform::FaultDisposition::kResume;
        }
    }
    // Task 525/596: a breakpoint that reaches here is the guest's own after
    // the AOT transfer and boundary gates have declined it. This check must
    // precede HandleSingleStepTrace: a cache boundary can resume at a guest
    // INT3 with `aot_reentry_pending` still set, and the trace path would then
    // re-arm TF without advancing the guest breakpoint.
    if (HandleGuestOwnedBreakpoint(fault, context))
    {
        return repiu::platform::FaultDisposition::kResume;
    }
    // Task 376: `single_step_trace_count` only advances inside
    // HandleSingleStepTrace, which returns immediately when trace mode is off.
    // Comparing it against the exception census therefore measured two different
    // things, not a discarded population. This records the split.
    if (true &&
        fault.kind == repiu::platform::FaultKind::kSingleStep)
    {
        RecordSingleStepTraceDisposition(&context->out_of_arena_step_census,
                                         context->enable_single_step_trace);
    }
    if (true &&
        (fault.kind == repiu::platform::FaultKind::kSingleStep ||
         (context->aot_reentry_pending &&
          fault.kind == repiu::platform::FaultKind::kBreakpoint)) &&
        HandleSingleStepTrace(win32_context, context))
    {
        return repiu::platform::FaultDisposition::kResume;
    }
    // Task 325: everything past this point is the sequential HLE handler chain
    // reached by exceptions the single-step path did not take -- principally
    // INT3 AOT boundaries. Held open to the end of the function so every exit
    // path is attributed.
    const ExecutionTimeScope hle_chain_time_scope(
        context->execution_time_profile.get(),
        ExecutionTimeBucket::kVehHleChain);
    AotHleTranslationScope aot_hle_translation_scope(win32_context, context);
    constexpr std::uint32_t kMaximumX86InstructionBytes = 15U;
    const char* const segment_hle_trace_value =
        std::getenv("REPIU_SEGMENT_HLE_TRACE");
    const bool segment_hle_trace_enabled =
        segment_hle_trace_value != nullptr &&
        std::strcmp(segment_hle_trace_value, "0") != 0;
    static std::atomic<std::uint32_t> segment_hle_trace_count{0U};
    const bool guest_instruction_readable = IsGuestRangeReadable(
        context,
        reinterpret_cast<const void*>(
            static_cast<std::uintptr_t>(win32_context->Eip)),
        1U);
    const std::uint8_t guest_opcode = guest_instruction_readable
        ? *reinterpret_cast<const std::uint8_t*>(
            static_cast<std::uintptr_t>(win32_context->Eip))
        : 0U;
    const bool guest_two_byte_readable = IsGuestRangeReadable(
        context,
        reinterpret_cast<const void*>(
            static_cast<std::uintptr_t>(win32_context->Eip)),
        2U);
    const std::uint8_t guest_second_opcode = guest_two_byte_readable
        ? reinterpret_cast<const std::uint8_t*>(
            static_cast<std::uintptr_t>(win32_context->Eip))[1]
        : 0U;
    const bool segment_push_candidate =
        guest_instruction_readable &&
        (guest_opcode == 0x06U || guest_opcode == 0x0EU ||
         guest_opcode == 0x16U || guest_opcode == 0x1EU ||
         (guest_opcode == 0x0FU &&
          (guest_second_opcode == 0xA0U ||
           guest_second_opcode == 0xA8U)));
    const std::uint32_t segment_hle_trace_index =
        segment_hle_trace_enabled && segment_push_candidate
            ? segment_hle_trace_count.fetch_add(
                  1U, std::memory_order_relaxed) + 1U
            : 0U;
    const bool guest_decode_window_readable = IsGuestRangeReadable(
        context,
        reinterpret_cast<const void*>(
            static_cast<std::uintptr_t>(win32_context->Eip)),
        kMaximumX86InstructionBytes);
    if (segment_hle_trace_index != 0U && segment_hle_trace_index <= 16U)
    {
        std::fprintf(
            stderr,
            "[repiu-segment-hle] stage=dispatch n=%u kind=%u eip=0x%08X "
            "opcode=%02X instruction_readable=%u decode_window=%u "
            "enabled=%u cache_active=%u call_state=%u esp=0x%08X "
            "es=0x%04X\n",
            segment_hle_trace_index,
            static_cast<unsigned>(fault.kind),
            static_cast<std::uint32_t>(win32_context->Eip),
            static_cast<unsigned>(guest_opcode),
            guest_instruction_readable ? 1U : 0U,
            guest_decode_window_readable ? 1U : 0U,
            context->enable_segment_load_hle ? 1U : 0U,
            context->cache_entry_active ? 1U : 0U,
            context->active_call_state != nullptr ? 1U : 0U,
            static_cast<std::uint32_t>(win32_context->Esp),
            static_cast<unsigned>(context->guest_es));
    }
    if (context->use_guest_stack && !guest_decode_window_readable)
    {
        // Task 583: question B, asked where it is actually needed. The entry
        // guard used to answer it for this site, which is why the dereference
        // below was unconditional. With only "is a guest running" asked up
        // front, a host that cannot unwind reaches here, and unwinding it would
        // mean jumping at a `ud2` through a truncated 32-bit Eip. Declining is
        // what such a host already did, so this preserves its behavior exactly.
        if (context->active_call_state == nullptr)
        {
            NoteVehExitSite(context, VehExitSite::kNoHostFrameToUnwind);
            return repiu::platform::FaultDisposition::kNotHandled;
        }
        NoteVehExitSite(context, VehExitSite::kUnreadableDecodeWindow);
        // Task 300: preserve the primary guest exception. Calling opcode
        // probes with an invalid EIP creates a host AV that masks the original
        // code/fault VA, as observed at HandleTracedDosInterrupt21.
        win32_context->EFlags &= ~0x00000100U;
        CommitUnhandledBreakpointEvidence(
            breakpoint_evidence, win32_context, context);
        CaptureException(fault, context);
        context->guest_return_esp =
            static_cast<std::uint32_t>(win32_context->Esp);
        context->host_esp = context->active_call_state->host_esp;
        RecordFaultRecoveryProvenance(
            context, fault.instruction_address,
            FaultRecoveryPath::kFaultCallback);
        RecoverToHost(win32_context, context);
        return repiu::platform::FaultDisposition::kResume;
    }

    const std::uint32_t iretd_eip =
        static_cast<std::uint32_t>(win32_context->Eip);
    const std::optional<bool> iretd =
        HandleIretdInstruction(win32_context, context);
    if (iretd.has_value() && *iretd)
    {
        NoteVehExitSite(context, VehExitSite::kHleChainPrivileged);
        // Task 751. The handler has returned, so the next owed tick may go
        // in, as it does after an `iret` handled from the cache (Task 747).
        // This is what drains the ticks a `cli` section held back: each
        // handler's return takes the next one, without nesting, and no
        // more than `kPostReturnChainLimit` in a row.
        if (CanEnterTimerInterruptHandler(context))
        {
            InjectPendingInterrupts(win32_context, context,
                                    TimerInjectionSite::kAfterHandlerReturn);
        }
        if (!ResumeHandledBoundary(win32_context, context, iretd_eip))
        {
            return repiu::platform::FaultDisposition::kNotHandled;
        }
        return repiu::platform::FaultDisposition::kResume;
    }

    if (context->enable_privileged_trap_hle &&
        HandlePrivilegedTrapInstruction(win32_context, context))
    {
        // Task 735. An emulated `sti` may make an owed tick deliverable; the
        // DOS HLE chain below injects at the same point.
        //
        // Task 751. On Linux x64 this returned to the handler's guest address
        // whatever it was, and a handler beginning with `pusha` died as an
        // illegal instruction. It was rare while ticks went in elsewhere
        // first; once the guest's `cli` holds them back, the closing `sti` is
        // where they arrive. So the tick goes in only when the handler can be
        // entered, and the entry goes through the cache as the `iret` above.
        const std::uint32_t privileged_resume_eip =
            static_cast<std::uint32_t>(win32_context->Eip);
        if (CanEnterTimerInterruptHandler(context))
        {
            InjectPendingInterrupts(win32_context, context);
        }
        NoteVehExitSite(context, VehExitSite::kHleChainPrivileged);
        if (!ResumeHandledBoundary(win32_context, context, privileged_resume_eip))
        {
            return repiu::platform::FaultDisposition::kNotHandled;
        }
        return repiu::platform::FaultDisposition::kResume;
    }
    if (context->enable_privileged_trap_hle &&
        HandlePortIoInstruction(win32_context, context))
    {
        NoteVehExitSite(context, VehExitSite::kHleChainPortIo);
        return repiu::platform::FaultDisposition::kResume;
    }
    if (context->enable_traced_dos_hle &&
        (HandleTracedDosInterrupt21(win32_context, context) ||
         HandleTracedDosInterrupt2F(win32_context, context) ||
         HandleTracedDpmiInterrupt31(win32_context, context) ||
         HandleTracedMouseInterrupt33(win32_context, context) ||
         HandleTracedBiosInterrupt16(win32_context, context)))
    {
        NoteVehExitSite(context, VehExitSite::kHleChainTracedDos);
        return repiu::platform::FaultDisposition::kResume;
    }
    // Task 410: the segment family is eighteen handlers with one answer, so it
    // is tagged once here rather than eighteen times. Every exit below this
    // point sets its own site, and the last call before the return wins, so a
    // fall-through past the family cannot be misread as one of its handlers.
    NoteVehExitSite(context, VehExitSite::kHleChainSegment);
    if (context->enable_segment_load_hle && segment_push_candidate)
    {
        const bool handled =
            HandleSegmentPushInstruction(win32_context, context);
        const std::uint32_t drained = handled
            ? DrainFollowingLegacyStackInstructions(win32_context, context)
            : 0U;
        if (segment_hle_trace_index != 0U && segment_hle_trace_index <= 16U)
        {
            const auto* const next = reinterpret_cast<const std::uint8_t*>(
                static_cast<std::uintptr_t>(win32_context->Eip));
            const bool next_readable = handled &&
                IsGuestRangeReadable(context, next, 6U);
            std::fprintf(
                stderr,
                        "[repiu-segment-hle] stage=handler n=%u handled=%u "
                        "drained=%u eip_after=0x%08X esp_after=0x%08X "
                        "next=%02X%02X%02X%02X%02X%02X\n",
                segment_hle_trace_index,
                handled ? 1U : 0U,
                drained,
                static_cast<std::uint32_t>(win32_context->Eip),
                static_cast<std::uint32_t>(win32_context->Esp),
                next_readable ? next[0] : 0U,
                next_readable ? next[1] : 0U,
                next_readable ? next[2] : 0U,
                next_readable ? next[3] : 0U,
                next_readable ? next[4] : 0U,
                next_readable ? next[5] : 0U);
        }
        if (handled)
        {
            return repiu::platform::FaultDisposition::kResume;
        }
    }
    if (context->enable_segment_load_hle &&
        HandleFarJumpInstruction(win32_context, context))
    {
        return repiu::platform::FaultDisposition::kResume;
    }
    if (context->enable_segment_load_hle)
    {
        const std::optional<bool> mode16_return =
            HandleMode16FarReturn(win32_context, context);
        if (mode16_return.has_value())
        {
            if (*mode16_return)
            {
                return repiu::platform::FaultDisposition::kResume;
            }
        }
        else if (HandleFarReturnInstruction(win32_context, context))
        {
            return repiu::platform::FaultDisposition::kResume;
        }
    }
    if (context->enable_segment_load_hle &&
        HandleSegmentLoadInstruction(win32_context, context))
    {
        return repiu::platform::FaultDisposition::kResume;
    }
    if (context->enable_segment_load_hle &&
        HandleSegmentPopInstruction(win32_context, context))
    {
        DrainFollowingLegacyStackInstructions(win32_context, context);
        return repiu::platform::FaultDisposition::kResume;
    }
    if (context->enable_segment_load_hle &&
        HandleRepStosdInstruction(win32_context, context))
    {
        return repiu::platform::FaultDisposition::kResume;
    }
    if (context->enable_segment_load_hle &&
        HandleSegmentStoreInstruction(win32_context, context))
    {
        return repiu::platform::FaultDisposition::kResume;
    }
    if (context->enable_segment_load_hle &&
        HandleSegmentOverrideMemoryLoadInstruction(win32_context, context))
    {
        return repiu::platform::FaultDisposition::kResume;
    }
    if (context->enable_segment_load_hle &&
        HandleSegmentOverrideByteLoadInstruction(win32_context, context))
    {
        return repiu::platform::FaultDisposition::kResume;
    }
    if (context->enable_segment_load_hle &&
        HandleFsSegmentWordLoadInstruction(win32_context, context))
    {
        return repiu::platform::FaultDisposition::kResume;
    }
    if (context->enable_segment_load_hle &&
        HandleSegmentMemoryCompareInstruction(win32_context, context))
    {
        return repiu::platform::FaultDisposition::kResume;
    }
    if (context->enable_segment_load_hle &&
        HandleSegmentMemoryLoadInstruction(win32_context, context))
    {
        return repiu::platform::FaultDisposition::kResume;
    }
    if (context->enable_segment_load_hle &&
        HandleTracedMemoryLoadInstruction(win32_context, context))
    {
        return repiu::platform::FaultDisposition::kResume;
    }
    if (context->enable_segment_load_hle &&
        HandleTracedMemoryAddInstruction(win32_context, context))
    {
        return repiu::platform::FaultDisposition::kResume;
    }
    if (context->enable_segment_load_hle &&
        HandleTracedMemoryOrInstruction(win32_context, context))
    {
        return repiu::platform::FaultDisposition::kResume;
    }
    if (context->enable_segment_load_hle &&
        HandleTracedMemoryCompareByteInstruction(win32_context, context))
    {
        return repiu::platform::FaultDisposition::kResume;
    }
    if (context->enable_segment_load_hle &&
        HandleTracedMemoryStoreInstruction(win32_context, context))
    {
        return repiu::platform::FaultDisposition::kResume;
    }
    if (context->enable_segment_load_hle &&
        HandleTracedMemoryTestInstruction(win32_context, context))
    {
        return repiu::platform::FaultDisposition::kResume;
    }
    if (context->enable_segment_load_hle &&
        HandleTracedFpuMemoryInstruction(win32_context, context))
    {
        return repiu::platform::FaultDisposition::kResume;
    }
    if (context->enable_dos_hle &&
        HandleDosHleInstruction(win32_context, context))
    {
        InjectPendingInterrupts(win32_context, context);
        NoteVehExitSite(context, VehExitSite::kHleChainDosHle);
        return repiu::platform::FaultDisposition::kResume;
    }
    // Task 401: nothing below handles a software interrupt, so an INT that
    // reaches here is unsupported. Name it, since the DOS HLE fallback above --
    // the only place that used to do so -- is off in the aot-dbt backend.
    RecordUnsupportedTracedSoftwareInterrupt(win32_context, context);
    const bool hle_active = context->enable_dos_hle || context->enable_traced_dos_hle;
    if (hle_active &&
        fault.kind == repiu::platform::FaultKind::kAccessViolation)
    {
        bool handled = false;
        if (fault.access.valid)
        {
            const std::uint32_t fault_va = fault.access.fault_address;

            bool is_aot_address = ((fault_va >= 0x0A000000U) && (fault_va < 0x0E000000U));

            // An instruction fetch, not a read or a write: the guest branched
            // into the AOT cache's range through a path the engine did not
            // translate.
            if (fault.access.execute_access && is_aot_address)
            {
                std::uint32_t decode_eip = 0;
                if (context->aot_placement != nullptr)
                {
                    const std::uint32_t offset = fault_va - context->aot_placement->base_address;
                    for (const runtime::AotAddressMapEntry& entry : context->aot_placement->address_map)
                    {
                        if (offset >= entry.cache_offset &&
                            offset < entry.cache_offset + entry.emitted_length)
                        {
                            decode_eip = entry.guest_address;
                            break;
                        }
                    }
                }
                if (decode_eip == 0)
                {
                    const std::uint32_t* esp_ptr = reinterpret_cast<const std::uint32_t*>(
                        static_cast<std::uintptr_t>(win32_context->Esp));
                    const repiu::platform::MemoryRegion stack_page =
                        repiu::platform::QueryMemory(esp_ptr);
                    if (stack_page.valid && stack_page.committed &&
                        stack_page.readable)
                    {
                        for (int i = 0; i < 32; ++i)
                        {
                            const std::uint32_t* cur_ptr = &esp_ptr[i];
                            if (reinterpret_cast<std::uintptr_t>(cur_ptr) < reinterpret_cast<std::uintptr_t>(stack_page.base) + stack_page.size)
                            {
                                std::uint32_t stack_val = *cur_ptr;
                                if (IsGuestInstructionPointer(context, stack_val))
                                {
                                    decode_eip = stack_val;
                                    break;
                                }
                            }
                        }
                    }
                }
                if (decode_eip == 0)
                {
                    const std::uint32_t* esp_ptr = reinterpret_cast<const std::uint32_t*>(
                        static_cast<std::uintptr_t>(win32_context->Esp));
                    if (IsGuestInstructionPointer(context, esp_ptr[0]))
                    {
                        decode_eip = esp_ptr[0];
                    }
                }
                if (decode_eip != 0)
                {
                    win32_context->Eip = decode_eip;
                    context->aot_legacy_fallback = true;
                    context->enable_single_step_trace = true;
                    context->aot_reentry_pending = false;
                    handled = true;
                }
            }
            else if (!fault.access.write_access &&
                     !fault.access.execute_access)
            {
                std::uint32_t decode_eip = win32_context->Eip;
                if (IsAotCacheAddress(context, win32_context->Eip))
                {
                    if (context->aot_placement != nullptr)
                    {
                        FindAotGuestAddress(*context->aot_placement, win32_context->Eip, &decode_eip);
                    }
                }

                if (decode_eip == 0x0304DD7DU)
                {
                    win32_context->EFlags |= 0x00000040U;
                    win32_context->Eip = decode_eip + 7;
                    context->aot_legacy_fallback = true;
                    context->enable_single_step_trace = true;
                    context->aot_reentry_pending = false;
                    handled = true;
                }
                else if (fault_va < 0x10000)
                {
                    std::uint32_t decode_eip = win32_context->Eip;
                    bool is_aot = false;
                    if (IsAotCacheAddress(context, win32_context->Eip))
                    {
                        is_aot = true;
                        if (context->aot_placement != nullptr)
                        {
                            FindAotGuestAddress(*context->aot_placement, win32_context->Eip, &decode_eip);
                        }
                    }

                    if (is_aot || IsGuestInstructionPointer(context, win32_context->Eip))
                    {
                        handled = HandleGuestLowMemoryReadFault(win32_context, context, fault_va, decode_eip);
                    }
                }
            }
        }
        
        if (!handled)
        {
            handled = HandleDosMemoryAccess(win32_context, context);
        }

        if (handled)
        {
            NoteVehExitSite(context, VehExitSite::kHleChainAccessViolation);
            return repiu::platform::FaultDisposition::kResume;
        }
    }
    NoteVehExitSite(context, VehExitSite::kHleChainStringOp);
    if (context->enable_segment_load_hle &&
        HandleRepMovsInstruction(win32_context, context))
    {
        return repiu::platform::FaultDisposition::kResume;
    }
    if (context->enable_segment_load_hle &&
        HandleRepCmpsbInstruction(win32_context, context))
    {
        return repiu::platform::FaultDisposition::kResume;
    }
    if (context->enable_segment_load_hle &&
        HandleLodsbInstruction(win32_context, context))
    {
        return repiu::platform::FaultDisposition::kResume;
    }
    if (context->aot_terminal_failure.load(std::memory_order_acquire))
    {
        NoteVehExitSite(context, VehExitSite::kTerminalFailureSearch);
        win32_context->EFlags &= ~0x00000100U;
        return repiu::platform::FaultDisposition::kNotHandled;
    }
    const std::uint32_t fatal_breakpoint_eip =
        static_cast<std::uint32_t>(win32_context->Eip);
    if (HandleOriginalFatalBreakpoint(fault, context))
    {
        NoteVehExitSite(context, VehExitSite::kFatalBreakpoint);
        if (!ResumeHandledBoundary(win32_context, context, fatal_breakpoint_eip))
        {
            return repiu::platform::FaultDisposition::kNotHandled;
        }
        return repiu::platform::FaultDisposition::kResume;
    }

    if (context->aot_reentry_pending &&
        true &&
        fault.kind == repiu::platform::FaultKind::kBreakpoint)
    {
        // Indirect transfers, returns, and LOOP-family instructions execute
        // once from the original image under TF, then re-enter the cache.
        NoteVehExitSite(context,
                        VehExitSite::kReentryBreakpointPassThrough);
        return repiu::platform::FaultDisposition::kResume;
    }

    const std::uint8_t* instruction = reinterpret_cast<const std::uint8_t*>(
        win32_context->Eip);
    if (!context->use_guest_stack &&
        (*instruction == 0xCC || instruction[-1] == 0xCC))
    {
        const std::uint32_t byte_count = fault.registers->Ecx;
        const void* source = reinterpret_cast<const void*>(
            static_cast<std::uintptr_t>(fault.registers->Edx));
        AppendConsoleOutput(context, source, byte_count);
        RecoverFromHleExit(fault.registers, context);
        NoteVehExitSite(context, VehExitSite::kConsoleOutputExit);
        return repiu::platform::FaultDisposition::kResume;
    }

    const std::uint32_t exception_address =
        static_cast<std::uint32_t>(win32_context->Eip);
    if (context->aot_placement != nullptr &&
        IsAotCacheAddress(context, exception_address) &&
        FindAotGuestAddress(*context->aot_placement,
                            exception_address,
                            &context->aot_exception_guest_address))
    {
        context->aot_exception_mapping_valid = true;
        context->aot_exception_cache_address = exception_address;
        const std::uint32_t cache_offset = exception_address -
            context->aot_placement->base_address;
        const std::uint32_t cache_bytes = std::min<std::uint32_t>(
            sizeof(context->aot_exception_cache_bytes),
            context->aot_placement->size - cache_offset);
        std::memcpy(
            context->aot_exception_cache_bytes,
            reinterpret_cast<const void*>(
                static_cast<std::uintptr_t>(exception_address)),
            cache_bytes);
        const std::uint64_t guest_end =
            static_cast<std::uint64_t>(
                context->aot_exception_guest_address) +
            sizeof(context->aot_exception_guest_bytes);
        const std::uint64_t runtime_end =
            static_cast<std::uint64_t>(context->runtime_base) +
            context->runtime_size;
        if (context->aot_exception_guest_address >=
                context->runtime_base &&
            guest_end <= runtime_end)
        {
            std::memcpy(
                context->aot_exception_guest_bytes,
                reinterpret_cast<const void*>(static_cast<std::uintptr_t>(
                    context->aot_exception_guest_address)),
                sizeof(context->aot_exception_guest_bytes));
        }
    }

    // Task 583: question B again, at the chain's last exit. Tagged before the
    // work below so the trace names this refusal rather than the recovery that
    // does not happen.
    if (context->use_guest_stack && context->active_call_state == nullptr)
    {
        if (runtime::execution_model::RunsLongModeCodeCache() &&
            fault.kind == repiu::platform::FaultKind::kSingleStep &&
            IsGuestInstructionPointer(
                context, static_cast<std::uint32_t>(win32_context->Eip)))
        {
            ReportLinuxX64UntranslatableContinuation(
                context, static_cast<std::uint32_t>(win32_context->Eip),
                "unhandled-single-step");
        }
        NoteVehExitSite(context, VehExitSite::kNoHostFrameToUnwind);
        return repiu::platform::FaultDisposition::kNotHandled;
    }
    NoteVehExitSite(context, VehExitSite::kUnhandledRecover);
    win32_context->EFlags &= ~0x00000100U;
    CommitUnhandledBreakpointEvidence(
        breakpoint_evidence, win32_context, context);
    CaptureException(fault, context);
    context->guest_return_esp =
        static_cast<std::uint32_t>(fault.registers->Esp);

    if (context->use_guest_stack)
    {
        context->host_esp = context->active_call_state->host_esp;
        RecordFaultRecoveryProvenance(
            context, fault.instruction_address,
            FaultRecoveryPath::kFaultCallback);
        RecoverToHost(fault.registers, context);
    }
    else
    {
        fault.registers->Eip =
            static_cast<decltype(fault.registers->Eip)>(
                reinterpret_cast<std::uintptr_t>(&RecoverHostStackException));
    }
    return repiu::platform::FaultDisposition::kResume;
}

// Boundary definitions promoted to external linkage (out of the anonymous
// namespace) so extracted win32 execution modules can call them across
// translation units. Declared in execution_internal.h.
bool WriteGuestBytes(ThreadContext* context,
                     void* destination,
                     const void* source,
                     std::size_t byte_count)
{
    if (source == nullptr ||
        !IsGuestRangeWritable(context, destination, byte_count))
    {
        return false;
    }

    // Task 503d-15: the same shape guest_memory_access.cpp moved to in 3d-3 --
    // unprotect, write, put back exactly what was there, which is why the call
    // below reports the previous protection at all.
    repiu::platform::MemoryProtection previous_protect =
        repiu::platform::MemoryProtection::kOther;
    if (!repiu::platform::ProtectMemory(
            destination,
            byte_count,
            repiu::platform::MemoryProtection::kExecuteReadWrite,
            &previous_protect))
    {
        std::ostringstream stream;
        // The host error number is no longer carried, so the address takes its
        // place; it is the more useful of the two when a guest store into
        // protected code is what went wrong.
        stream << "protecting guest memory failed for guest byte store at 0x"
               << std::hex
               << reinterpret_cast<std::uintptr_t>(destination);
        context->hle_message = stream.str();
        return false;
    }

    std::memcpy(destination, source, byte_count);

    if (!repiu::platform::ProtectMemory(destination,
                                        byte_count,
                                        previous_protect,
                                        nullptr))
    {
        return false;
    }
    if (byte_count > std::numeric_limits<std::uint32_t>::max())
    {
        return false;
    }
    const bool noted = NoteSuccessfulAotGuestWrite(
        context,
        static_cast<std::uint32_t>(
            reinterpret_cast<std::uintptr_t>(destination)),
        static_cast<std::uint32_t>(byte_count));
    RecordGuestWriteTrace(
        GuestWriteTraceEvent::kHle,
        0U,
        0U,
        static_cast<std::uint32_t>(
            reinterpret_cast<std::uintptr_t>(destination)),
        static_cast<std::uint32_t>(byte_count),
        source);
    return noted;
}

bool IsAotCacheAddress(const ThreadContext* context, std::uint32_t address)
{
    if (context == nullptr || context->aot_placement == nullptr ||
        !context->aot_placement->placed)
    {
        return false;
    }
    const std::uint64_t end =
        static_cast<std::uint64_t>(context->aot_placement->base_address) +
        context->aot_placement->size;
    return address >= context->aot_placement->base_address && address < end;
}

void RecordFaultRecoveryProvenance(ThreadContext* context,
                                   const std::uint32_t source_eip,
                                   const FaultRecoveryPath path)
{
    if (context == nullptr)
    {
        return;
    }
    const FaultRecoveryProvenance provenance = MakeFaultRecoveryProvenance(
        source_eip,
        IsGuestInstructionPointer(context, source_eip),
        IsAotCacheAddress(context, source_eip),
        path);
    if (provenance.valid)
    {
        context->fault_recovery_provenance = provenance;
    }
}

// Task 507. What the shutdown path asks of the guest thread, and what it gets
// back. One structure rather than captures, because this crosses a callback the
// platform layer defines with a `void*`.
struct GuestShutdownRecoveryRequest
{
    ThreadContext* context = nullptr;
    // Filled only for a timeout: an exit the operator asked for is not a
    // position worth reporting, and the field is read as "where it stopped".
    X86ExecutionSnapshot* snapshot = nullptr;
    // Set by the callback when it redirected the thread. False means the guest
    // was somewhere this path will not recover from, which is a different
    // outcome from the interrupt itself failing.
    bool recovered = false;
    // Where the last look found the thread. Kept whether or not the callback
    // acted, because "not in recoverable code" is only half an answer without
    // the address that says which code it was in.
    std::uint32_t last_eip = 0;
    // Task 730: the full native address behind `last_eip`, and the decision
    // taken on it. `last_eip` alone is the low half on an x64 host.
    std::uintptr_t last_host_ip = 0;
    ShutdownRecoveryDecision last_decision =
        ShutdownRecoveryDecision::kOutsideGuestCode;
    // Looks whose low half was in guest code but whose full address was a host
    // address -- each one a thread the 32-bit check would have recovered from a
    // host frame. Nonzero is direct evidence for the teardown segfault's cause.
    std::uint32_t aliased_count = 0;
    // Task 733: diagnostic-only state copied before the Windows guest resumes.
    // Linux executes this callback in a signal handler and does not format it.
    bool trace_enabled = false;
    std::uint32_t attempt = 0;
};

#if defined(_WIN32)
// How many exceptions after the redirect are still to be recorded. The first
// one alone showed a single step arriving at the recovery entry; the access
// violation that ends the run comes after it, so several are kept.
constexpr int kShutdownRecoveryExceptionTraceDepth = 4;
std::atomic<int> g_shutdown_recovery_exception_trace_remaining{0};

LONG CALLBACK ShutdownRecoveryExceptionTraceHandler(
    EXCEPTION_POINTERS* pointers)
{
    const int remaining = g_shutdown_recovery_exception_trace_remaining.fetch_sub(
        1, std::memory_order_acq_rel);
    if (remaining <= 0 || pointers == nullptr || pointers->ExceptionRecord == nullptr ||
        pointers->ContextRecord == nullptr)
    {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    const EXCEPTION_RECORD& record = *pointers->ExceptionRecord;
    const CONTEXT& registers = *pointers->ContextRecord;
    const ULONG_PTR access_kind = record.NumberParameters >= 1U
        ? record.ExceptionInformation[0] : 0U;
    const ULONG_PTR access_target = record.NumberParameters >= 2U
        ? record.ExceptionInformation[1] : 0U;
    char line[256] = {};
    const int length = std::snprintf(
        line, sizeof(line),
        "[repiu-shutdown-recovery-exception] index=%d code=0x%08lX "
        "address=0x%08X eip=0x%08X esp=0x%08X eflags=0x%08X access=%llu "
        "target=0x%08llX thread=%lu\n",
        kShutdownRecoveryExceptionTraceDepth - remaining + 1,
        record.ExceptionCode,
        static_cast<unsigned>(
            reinterpret_cast<std::uintptr_t>(record.ExceptionAddress)),
        static_cast<unsigned>(registers.Eip),
        static_cast<unsigned>(registers.Esp),
        static_cast<unsigned>(registers.EFlags),
        static_cast<unsigned long long>(access_kind),
        static_cast<unsigned long long>(access_target),
        GetCurrentThreadId());
    if (length > 0)
    {
        repiu::platform::WriteHostErrorStream(
            line,
            static_cast<std::size_t>(length) < sizeof(line)
                ? static_cast<std::size_t>(length)
                : sizeof(line) - 1U);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

// Task 733. Completes a shutdown redirect that an in-flight exception would
// otherwise undo. The decision is `DecideShutdownRedirectGuard`; see its
// header for the race. Registered after the engine's own vectored handler so
// that it is called before it -- in the case that crashed, the engine
// handler's frame is what overflowed the host stack.
struct ShutdownRedirectGuardState
{
    std::atomic<bool> redirect_applied{false};
    std::atomic<std::uint32_t> guest_thread_id{0};
    std::atomic<ThreadContext*> context{nullptr};
    std::atomic<std::uint32_t> reapplied_at_entry{0};
    std::atomic<std::uint32_t> reapplied_from_guest{0};
};

ShutdownRedirectGuardState g_shutdown_redirect_guard;

LONG CALLBACK ShutdownRedirectGuardHandler(EXCEPTION_POINTERS* pointers)
{
    ThreadContext* const context =
        g_shutdown_redirect_guard.context.load(std::memory_order_acquire);
    if (context == nullptr || pointers == nullptr ||
        pointers->ExceptionRecord == nullptr ||
        pointers->ContextRecord == nullptr)
    {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    CONTEXT* const registers = pointers->ContextRecord;
    const std::uint32_t eip = static_cast<std::uint32_t>(registers->Eip);
    ShutdownRedirectGuardInput input;
    input.redirect_applied =
        g_shutdown_redirect_guard.redirect_applied.load(std::memory_order_acquire);
    input.on_guest_thread = GetCurrentThreadId() ==
        g_shutdown_redirect_guard.guest_thread_id.load(std::memory_order_acquire);
    input.entry_reapplies =
        g_shutdown_redirect_guard.reapplied_at_entry.load(std::memory_order_relaxed);
    input.at_recovery_entry = eip == static_cast<std::uint32_t>(
        reinterpret_cast<std::uintptr_t>(&RecoverGuestStackException));
    input.in_guest_code = IsGuestInstructionPointer(context, eip) ||
        IsAotCacheAddress(context, eip);
    const ShutdownRedirectGuardAction action = DecideShutdownRedirectGuard(input);
    if (action == ShutdownRedirectGuardAction::kPass)
    {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    // `RecoverToHost` is idempotent for the entry case -- the context already
    // holds the recovery entry and host stack -- and restores the redirect for
    // the guest case. Either way the trap flag leaves with it.
    RecoverToHost(registers, context);
    if (action == ShutdownRedirectGuardAction::kReapplyAtEntry)
    {
        g_shutdown_redirect_guard.reapplied_at_entry.fetch_add(
            1U, std::memory_order_relaxed);
    }
    else
    {
        g_shutdown_redirect_guard.reapplied_from_guest.fetch_add(
            1U, std::memory_order_relaxed);
    }
    return EXCEPTION_CONTINUE_EXECUTION;
}
#endif

// **This runs on a different thread on each host.** Windows freezes the guest
// thread and runs this on the caller's; Linux runs it on the guest thread
// itself, inside a signal handler. So it carries the fault callback's
// constraints -- it allocates nothing, takes no lock, and blocks on nothing. The
// message that used to be written here now belongs to the requesting thread,
// because assigning a std::string is an allocation.
bool RecoverGuestThreadForShutdownCommon(
    repiu::platform::GuestCpuContext* registers,
    void* user_data,
    void* host_context)
{
    auto* request = static_cast<GuestShutdownRecoveryRequest*>(user_data);
    if (request == nullptr || registers == nullptr ||
        request->context == nullptr)
    {
        return false;
    }
    if (request->snapshot != nullptr)
    {
        CopySnapshotFromContextRecord(*registers, request->snapshot);
    }

    // The one defence against recovering from the wrong place. Guest code and
    // the AOT cache are the two regions whose frames the recovery entry knows
    // how to unwind; anywhere else -- host code, or a fault handler of the
    // engine's own -- it would return through a frame that was never set up.
    const std::uint32_t eip = static_cast<std::uint32_t>(registers->Eip);
    request->last_eip = eip;
    // Task 730: on an x64 host `eip` is only the low half of RIP, and a host
    // library address can alias guest code in it. The full address decides.
    ShutdownRecoveryPosition position;
    position.low_half_in_guest_code =
        IsGuestInstructionPointer(request->context, eip) ||
        IsAotCacheAddress(request->context, eip);
    trampoline_model::ReadShutdownRecoveryPosition(eip, host_context,
                                                   &position);
    request->last_host_ip = position.host_address;
    const ShutdownRecoveryDecision decision = DecideShutdownRecovery(position);
    request->last_decision = decision;
#if defined(_WIN32)
    if (request->trace_enabled)
    {
        char line[320] = {};
        const StackSwitchCallState* call_state =
            request->context->active_call_state;
        const int length = std::snprintf(
            line, sizeof(line),
            "[repiu-shutdown-recovery] attempt=%u phase=observed "
            "decision=%s eip=0x%08X esp=0x%08X eflags=0x%08X "
            "call_state=0x%08X call_host_esp=0x%08X stored_host_esp=0x%08X\n",
            static_cast<unsigned>(request->attempt),
            ShutdownRecoveryDecisionName(decision),
            static_cast<unsigned>(registers->Eip),
            static_cast<unsigned>(registers->Esp),
            static_cast<unsigned>(registers->EFlags),
            static_cast<unsigned>(reinterpret_cast<std::uintptr_t>(call_state)),
            call_state != nullptr ? static_cast<unsigned>(call_state->host_esp) : 0U,
            static_cast<unsigned>(request->context->host_esp));
        if (length > 0)
        {
            repiu::platform::WriteHostErrorStream(
                line,
                static_cast<std::size_t>(length) < sizeof(line)
                    ? static_cast<std::size_t>(length)
                    : sizeof(line) - 1U);
        }
    }
#endif
    if (decision == ShutdownRecoveryDecision::kAliasedHostAddress)
    {
        ++request->aliased_count;
    }
    if (decision != ShutdownRecoveryDecision::kRecover)
    {
        return false;
    }
    RecordFaultRecoveryProvenance(
        request->context, eip, FaultRecoveryPath::kShutdownInterrupt);
    if (runtime::execution_model::RunsLongModeCodeCache())
    {
        // The x64 cache runs with the host stack in RSP. Returning through the
        // cache's exit is therefore the only valid shutdown unwind.
        if (!trampoline_model::RedirectToCacheExit(registers, host_context))
        {
            return false;
        }
        request->recovered = true;
        return true;
    }
#if defined(_WIN32)
    if (request->trace_enabled)
    {
        g_shutdown_recovery_exception_trace_remaining.store(
            kShutdownRecoveryExceptionTraceDepth, std::memory_order_release);
    }
#endif
    RecoverToHost(registers, request->context);
#if defined(_WIN32)
    // Task 733: from here an exception the kernel was already delivering may
    // still arrive; the guard completes the redirect when it does.
    g_shutdown_redirect_guard.redirect_applied.store(
        true, std::memory_order_release);
#endif
#if defined(_WIN32)
    if (request->trace_enabled)
    {
        char line[160] = {};
        const int length = std::snprintf(
            line, sizeof(line),
            "[repiu-shutdown-recovery] attempt=%u phase=redirected "
            "eip=0x%08X esp=0x%08X\n",
            static_cast<unsigned>(request->attempt),
            static_cast<unsigned>(registers->Eip),
            static_cast<unsigned>(registers->Esp));
        if (length > 0)
        {
            repiu::platform::WriteHostErrorStream(
                line,
                static_cast<std::size_t>(length) < sizeof(line)
                    ? static_cast<std::size_t>(length)
                    : sizeof(line) - 1U);
        }
    }
#endif
    request->recovered = true;
    return true;
}

bool RecoverGuestThreadForShutdown(repiu::platform::GuestCpuContext* registers,
                                   void* user_data)
{
    return RecoverGuestThreadForShutdownCommon(registers, user_data, nullptr);
}

bool RecoverGuestThreadForShutdownWithContext(
    repiu::platform::GuestCpuContext* registers,
    void* user_data,
    void* host_context)
{
    return RecoverGuestThreadForShutdownCommon(
        registers, user_data, host_context);
}

std::uint16_t FindGuestStackSelector(
    const RelocatedImagePlacement& placement,
    const runtime::GuestStackSwitchPlan& stack_plan)
{
    if (!stack_plan.valid)
    {
        return 0;
    }
    for (const runtime::RelocatedSelectorBinding& binding :
         placement.selector_bindings)
    {
        if (binding.relocated_base_address == stack_plan.stack_base &&
            binding.selector != 0)
        {
            return binding.selector;
        }
    }
    return 0;
}

std::uint16_t FindGuestSelectorForLinearAddress(
    const RelocatedImagePlacement& placement,
    std::uint32_t linear_address)
{
    for (const runtime::RelocatedSelectorBinding& binding :
         placement.selector_bindings)
    {
        if (binding.selector == 0 || linear_address < binding.relocated_base_address)
        {
            continue;
        }
        const std::uint64_t offset =
            static_cast<std::uint64_t>(linear_address) -
            binding.relocated_base_address;
        if (offset <= binding.limit)
        {
            return binding.selector;
        }
    }
    return 0;
}

bool RunExecutionThread(
    const RelocatedImagePlacement& placement,
    std::uint32_t entry_address,
    std::uint32_t guest_initial_esp,
    std::uint16_t guest_initial_ss,
    bool use_guest_stack,
    bool enable_privileged_trap_hle,
    bool enable_traced_dos_hle,
    bool enable_segment_load_hle,
    bool enable_dos_hle,
    bool enable_single_step_trace,
    const hle::DosVirtualFileSystemState* dos_file_system,
    const exe::Dos16mBoundModule* linexe_module,
    const std::vector<exe::LeResidentName>* glide_exports,
    const std::filesystem::path* cd_chd_path,
    const std::filesystem::path* sound_rom_zip_path,
    bool enable_piu_jamma_board,
    bool enable_piu10_isa_board,
    bool enable_cat702,
    std::string_view parent_rom_set_id,
    std::uint32_t piu10_mp3_latency_ms,
    AotCodeCachePlacement* aot_placement,
    runtime::ExecutionBackend execution_backend,
    std::uint32_t timeout_milliseconds,
    std::uint32_t stall_timeout_milliseconds,
    MinimalExecutionAttempt* attempt)
{
    if (attempt == nullptr)
    {
        return false;
    }

    *attempt = MinimalExecutionAttempt{};
    attempt->entry_address = entry_address;
    // Task 578. Either way of entering the guest counts as supported, and they
    // are different ways: i386 jumps at the guest's own bytes, x64 enters the
    // emitted cache. `IsDirectX86ExecutionSupported` keeps meaning only the
    // first, so the routing is a disjunction rather than a loosened predicate.
    attempt->supported =
        IsDirectX86ExecutionSupported() || IsCodeCacheEntrySupported();
    attempt->guest_stack_switch_supported = IsGuestStackSwitchSupported();
    attempt->guest_stack_initial_esp = guest_initial_esp;

    if (!attempt->supported)
    {
        attempt->valid = true;
        attempt->message =
            "minimal original entry execution requires a 32-bit host";
        return true;
    }

    // Task 578. What the caller is asking for is that the guest run on its own
    // stack, and there are two ways to provide that. i386 switches host ESP
    // onto it. x64 never has to: the entry seeds guest ESP into R15D and host
    // RSP stays the SysV stack, so the guest is on its own stack from the first
    // instruction and there is no switch to support.
    //
    // `IsGuestStackSwitchSupported` therefore keeps meaning "is there a 32-bit
    // switch" and keeps answering no on x64, exactly as
    // `IsDirectX86ExecutionSupported` does. The refusal below is the one that
    // has to know about both mechanisms.
    if (use_guest_stack && !attempt->guest_stack_switch_supported &&
        !IsCodeCacheEntrySupported())
    {
        attempt->valid = true;
        attempt->message =
            "guest stack execution requires a 32-bit x86 host";
        return true;
    }

    if (!placement.valid || !placement.placed)
    {
        attempt->message = "relocated image is not placed";
        return false;
    }

    ThreadContext context;
    if (SingleStepHotspotProfileEnabled())
    {
        context.single_step_hotspot_profile =
            std::make_unique<SingleStepHotspotProfile>();
    }
    if (ExecutionTimeProfileEnabled())
    {
        context.execution_time_profile =
            std::make_unique<ExecutionTimeProfile>();
        context.aot_worker_timing =
            std::make_unique<AotWorkerTimingProfile>();
    }
    // Task 411: sampled by the poll thread, not this one, so the allocation is
    // the only thing the guest thread's setup owes it.
    if (GuestPositionCensusEnabled())
    {
        context.guest_position_census =
            std::make_unique<GuestPositionCensus>();
        context.guest_position_census->interval_milliseconds =
            GuestPositionCensusIntervalMilliseconds();
    }
    // Task 421: same arrangement for the music position.
    if (CdAudioPositionCensusEnabled())
    {
        context.cd_audio_position_census =
            std::make_unique<CdAudioPositionCensus>();
        context.cd_audio_position_census->enabled = true;
        context.cd_audio_position_census->interval_milliseconds =
            CdAudioPositionCensusIntervalMilliseconds();
    }
    // Task 422: recorded on the guest thread as each command is served, so no
    // separate sampler is involved.
    if (MscdexCommandTraceEnabled())
    {
        context.mscdex_command_trace =
            std::make_unique<MscdexCommandTrace>();
        context.mscdex_command_trace->enabled = true;
        context.mscdex_command_trace->base_tick =
            static_cast<std::uint32_t>(repiu::platform::MillisecondTicks());
    }
    // Task 503d-19: the shared section exists on Windows only. Everything
    // downstream already tests the pointer for null, so Linux simply runs with
    // no external observer attached.
    // Task 759: the platform layer answers, and answers "none" on Linux.
    SharedTelemetryMapping shared_telemetry =
        OpenSharedTelemetryMapping();
    context.shared_live_telemetry = shared_telemetry.telemetry;
    if (context.shared_live_telemetry != nullptr)
    {
        repiu::platform::AtomicExchange(&context.shared_live_telemetry->host_phase, 1);
    }
    context.entry_address = aot_placement != nullptr
        ? aot_placement->entry_address : entry_address;
    context.runtime_base = placement.placed_base;
    context.runtime_size = placement.placed_size;
    context.guest_initial_esp = guest_initial_esp;
    context.guest_ss = guest_initial_ss;
    context.flat_stack_selector = guest_initial_ss;
    context.guest_ds = FindGuestSelectorForLinearAddress(
        placement, entry_address);
    context.flat_data_selector = context.guest_ds;
    context.use_guest_stack = use_guest_stack;
    context.enable_privileged_trap_hle = enable_privileged_trap_hle;
    context.enable_traced_dos_hle = enable_traced_dos_hle;
    context.enable_segment_load_hle = enable_segment_load_hle;
    context.enable_dos_hle = enable_dos_hle;
    context.enable_single_step_trace = enable_single_step_trace;
    context.piu_jamma_board_enabled = enable_piu_jamma_board;
    context.piu10_isa_board_enabled = enable_piu10_isa_board;
    context.aot_placement = aot_placement;
    context.execution_backend = execution_backend;
    // Task 503d-19: onto 3d-9's layer, which was built for exactly this idiom
    // -- a small buffer, a length, and the three outcomes absent / too long /
    // present that the length was being decoded into.
    const auto call_return_trace = repiu::platform::ReadEnvironmentSetting(
        "REPIU_AOT_DBT_CALL_TRACE", 8U);
    context.aot_dbt_call_return_trace_configured =
        call_return_trace.present && !call_return_trace.too_long &&
        call_return_trace.value == "1";
    context.glide_backend.SetExecutionBackend(execution_backend);
    char probe_offset_text[32] = {};
    const auto probe_offset = repiu::platform::ReadEnvironmentSetting(
        "REPIU_EXECUTION_PROBE_OFFSET", sizeof(probe_offset_text));
    if (probe_offset.present && !probe_offset.too_long)
    {
        probe_offset.value.copy(probe_offset_text, probe_offset.value.size());
        char* end = nullptr;
        const unsigned long value = std::strtoul(
            probe_offset_text, &end, 0);
        if (end != probe_offset_text && *end == '\0' && value <= UINT32_MAX)
        {
            context.execution_probe_configured = true;
            context.execution_probe_offset =
                static_cast<std::uint32_t>(value);
        }
    }
    if (context.execution_probe_configured && aot_placement != nullptr)
    {
        // A miss is expected for a dynamic-only target. Keep the probe armed;
        // the append path installs it after publishing the matching entry.
        (void)InstallAotProbeSentinel(
            aot_placement,
            context.runtime_base + context.execution_probe_offset);
    }
    const auto read_hex_env = [](const char* name, std::uint32_t* out) {
        char text[32] = {};
        const auto setting =
            repiu::platform::ReadEnvironmentSetting(name, sizeof(text));
        if (!setting.present || setting.too_long)
        {
            return false;
        }
        setting.value.copy(text, setting.value.size());
        char* end = nullptr;
        const unsigned long value = std::strtoul(text, &end, 0);
        if (end == text || *end != '\0' || value > UINT32_MAX)
        {
            return false;
        }
        *out = static_cast<std::uint32_t>(value);
        return true;
    };
    read_hex_env("REPIU_EXECUTION_PROBE_MEMORY_OFFSET",
                 &context.execution_probe_memory_offset);
    read_hex_env("REPIU_EXECUTION_PROBE_LOG_ARRIVALS",
                 &context.execution_probe_log_arrivals);
    if (aot_placement != nullptr)
    {
        TraceAotGuestMap(*aot_placement, context.runtime_base, "initial");
        TraceAotCacheMap(*aot_placement, "initial");
    }
    // The capture buffer is reserved here, before the guest thread starts, so
    // the first-hit recorder never allocates inside the exception handler.
    context.execution_probe_dump_request =
        ReadExecutionProbeDumpRequest();
    if (context.execution_probe_dump_request.configured)
    {
        context.execution_probe_dump_result.bytes.assign(
            context.execution_probe_dump_request.byte_count, 0U);
    }
    if (read_hex_env("REPIU_EXECUTION_TRACE_START",
                     &context.execution_trace_start_offset) &&
        read_hex_env("REPIU_EXECUTION_TRACE_END",
                     &context.execution_trace_end_offset) &&
        read_hex_env("REPIU_EXECUTION_TRACE_ESP_OFFSET",
                     &context.execution_trace_esp_offset) &&
        context.execution_trace_start_offset <=
            context.execution_trace_end_offset)
    {
        context.execution_trace_configured = true;
    }
    const auto trace_log = repiu::platform::ReadEnvironmentSetting(
        "REPIU_EXECUTION_TRACE_LOG", 2U);
    const bool trace_log_requested =
        trace_log.present && !trace_log.too_long && trace_log.value == "1";
    if (context.execution_trace_configured && aot_placement != nullptr &&
        !InstallAotProbeSentinel(
            aot_placement,
            context.runtime_base + context.execution_trace_start_offset))
    {
        context.execution_trace_configured = false;
    }
    context.execution_trace_log_enabled =
        context.execution_trace_configured && trace_log_requested;
    // A single sentinel only forces single-stepping until the AOT dispatcher
    // (HandleAotReentry) finds a resolvable cached target for the NEXT guest
    // address and jumps straight back into fast cached execution — typically
    // after just one instruction, since the rest of the traced range usually
    // already has a normal cache entry. A second, independent sentinel forces
    // a fresh breakpoint (and a fresh capture) at that later point too.
    if (context.execution_trace_configured && aot_placement != nullptr &&
        read_hex_env("REPIU_EXECUTION_TRACE_SENTINEL2",
                     &context.execution_trace_sentinel2_offset))
    {
        context.execution_trace_sentinel2_configured =
            InstallAotProbeSentinel(
                aot_placement,
                context.runtime_base + context.execution_trace_sentinel2_offset);
    }
    if (aot_placement != nullptr)
    {
        context.aot_cache_entry_count.store(1, std::memory_order_relaxed);
    }
    context.glide_state.texture_memory_bytes =
        repiu::hle::kPiuBansheeVirtualTextureMemoryBytes;
    if (glide_exports != nullptr)
    {
        context.glide_exports = *glide_exports;
    }
    if (cd_chd_path != nullptr && context.cd_image.Open(*cd_chd_path))
    {
        context.mscdex_available = true;
        context.cd_audio_available = context.cd_audio.Open(*cd_chd_path);
    }
    // Sound is optional: a missing or unreadable ROM set leaves the PIU10 sound
    // window inert instead of failing the run, exactly like a cabinet with a dead
    // sound board still booting.
    if (enable_piu_jamma_board && sound_rom_zip_path != nullptr)
    {
        context.ymz_audio_available =
            context.ymz_audio.Open(*sound_rom_zip_path);
    }

    if (enable_piu10_isa_board && sound_rom_zip_path != nullptr)
    {
        constexpr std::uint32_t kMaximumMp3LatencyMs = 500U;
        char mp3_latency_text[16] = {};
        const auto mp3_latency = repiu::platform::ReadEnvironmentSetting(
            "REPIU_PIU10_MP3_LATENCY_MS", std::size(mp3_latency_text));
        if (mp3_latency.present && !mp3_latency.too_long)
        {
            mp3_latency.value.copy(mp3_latency_text, mp3_latency.value.size());
            char* end = nullptr;
            const unsigned long parsed = std::strtoul(
                mp3_latency_text, &end, 10);
            if (end != mp3_latency_text && *end == '\0' &&
                parsed <= kMaximumMp3LatencyMs)
            {
                piu10_mp3_latency_ms = static_cast<std::uint32_t>(parsed);
            }
            else
            {
                std::fprintf(
                    stderr,
                    "[repiu-piu10-mp3] invalid latency '%s'; using %u ms\n",
                    mp3_latency_text, piu10_mp3_latency_ms);
            }
        }
        context.piu10_mp3_audio.SetStartupLatencyMs(piu10_mp3_latency_ms);
        std::fprintf(stderr,
                     "[repiu-piu10-mp3] startup latency=%u ms\n",
                     piu10_mp3_latency_ms);
        context.piu10_mp3_frame_batch_enabled = true;
        const auto mp3_batch_audit = repiu::platform::ReadEnvironmentSetting(
            "REPIU_PIU10_MP3_BATCH_AUDIT", 2U);
        context.piu10_mp3_frame_batch_audit_enabled =
            context.piu10_mp3_frame_batch_enabled &&
            mp3_batch_audit.present && !mp3_batch_audit.too_long &&
            mp3_batch_audit.value == "1";
        if (context.piu10_mp3_frame_batch_audit_enabled)
        {
            std::fprintf(stderr,
                         "[repiu-piu10-mp3] frame-tail audit enabled\n");
        }
        const auto mp3_stream_audit = repiu::platform::ReadEnvironmentSetting(
            "REPIU_PIU10_MP3_STREAM_AUDIT", 2U);
        const bool mp3_stream_audit_enabled =
            mp3_stream_audit.present && !mp3_stream_audit.too_long &&
            mp3_stream_audit.value == "1";
        context.piu10_mp3_audio.SetStreamAuditEnabled(
            mp3_stream_audit_enabled);
        if (mp3_stream_audit_enabled)
        {
            std::fprintf(stderr,
                         "[repiu-piu10-mp3-audit] stream audit enabled\n");
        }
        repiu::assets::RomZipEntry flash =
            repiu::assets::ExtractRomZipEntry(
                *sound_rom_zip_path, "piu10.u8");
        std::optional<repiu::hle::Piu10IsaBoard::Cat702Transform>
            cat702_transform;
        bool cat702_ready = !enable_cat702;
        std::string cat702_message = "disabled by target profile";
        if (enable_cat702)
        {
            const std::string cat702_name =
                sound_rom_zip_path->stem().string() + ".cat702";
            const std::string parent_cat702_name =
                parent_rom_set_id.empty()
                    ? std::string{}
                    : std::string(parent_rom_set_id) + ".cat702";
            repiu::assets::RomZipEntry cat702 =
                repiu::assets::ExtractRomZipEntryWithParentFallback(
                    *sound_rom_zip_path, cat702_name,
                    parent_rom_set_id, parent_cat702_name);
            cat702_message = cat702.message;
            cat702_ready = cat702.valid &&
                cat702.data.size() ==
                    repiu::hle::Piu10IsaBoard::kCat702TransformBytes;
            if (cat702_ready)
            {
                cat702_transform.emplace();
                std::copy(cat702.data.begin(), cat702.data.end(),
                          cat702_transform->begin());
            }
            else if (cat702.valid)
            {
                cat702_message =
                    "CAT702 transform must contain exactly 8 bytes";
            }
        }
        if (flash.valid && cat702_ready)
        {
            std::string piu10_message;
            context.piu10_isa_board.Initialize(
                std::move(flash.data), cat702_transform, &piu10_message);
            const auto dac_audit = repiu::platform::ReadEnvironmentSetting(
                "REPIU_PIU10_DAC_AUDIT", 2U);
            const bool dac_audit_enabled = dac_audit.present &&
                !dac_audit.too_long && dac_audit.value == "1";
            context.piu10_isa_board.SetDacControlSink(
                [&context, dac_audit_enabled](
                    const repiu::sound::Dac3350aControlEvent& event) {
                    Piu10Mp3AudioSnapshot snapshot;
                    if (dac_audit_enabled)
                    {
                        snapshot = context.piu10_mp3_audio.Snapshot();
                    }
                    float applied_gain = 0.0F;
                    bool gain_applied = false;
                    if (event.analog_volume)
                    {
                        applied_gain =
                            (event.left_gain + event.right_gain) * 0.5F;
                        gain_applied =
                            context.piu10_mp3_audio.SetGain(applied_gain);
                    }
                    if (dac_audit_enabled)
                    {
                        std::fprintf(
                            stderr,
                            "[repiu-piu10-dac] subaddress=0x%02X "
                            "data=0x%04X bytes=%zu analog-volume=%s "
                            "left=%u right=%u left-gain=%.6f "
                            "right-gain=%.6f applied-gain=%.6f "
                            "gain-applied=%s muted=%s "
                            "audio-ready=%s pcm-queued-bytes=%d "
                            "pcm-queued-ms=%.3f pcm-format=%d/%d/S16 "
                            "device-buffer-frames=%d device-buffer-ms=%.3f "
                            "device-rate=%d compressed-ring=%zu "
                            "decoder-pending=%zu compressed-inflight=%zu "
                            "received=%llu decoded=%llu "
                            "frame-sync=%u\n",
                            event.subaddress, event.data, event.data_bytes,
                            event.analog_volume ? "true" : "false",
                            event.left_volume, event.right_volume,
                            event.left_gain, event.right_gain, applied_gain,
                            gain_applied ? "true" : "false",
                            event.stereo_muted ? "true" : "false",
                            snapshot.available ? "true" : "false",
                            snapshot.pcm_queued_bytes,
                            snapshot.pcm_queued_ms,
                            snapshot.pcm_sample_rate,
                            snapshot.pcm_channels,
                            snapshot.device_buffer_frames,
                            snapshot.device_buffer_ms,
                            snapshot.device_sample_rate,
                            snapshot.compressed_ring_bytes,
                            snapshot.decoder_pending_bytes,
                            snapshot.compressed_inflight_bytes,
                            static_cast<unsigned long long>(
                                snapshot.received_bytes),
                            static_cast<unsigned long long>(
                                snapshot.decoded_frames),
                            snapshot.frame_sync);
                    }
                });
            context.piu10_mp3_audio.Open();
            context.piu10_isa_board.SetMp3DataSink(
                [&context](std::uint8_t value) {
                    context.piu10_mp3_audio.WriteByte(value);
                });
            // Issue #58. Every status read also feeds the song clock
            // census, which replays the game's overwrite of its song clock
            // and reports how far the arrows jump at each one.
            // `REPIU_PIU10_SONG_CLOCK_LOG=1` prints every overwrite; jumps
            // of `kPiu10SongClockReportJumpMs` or more are printed anyway.
            static const bool song_clock_log = repiu::runtime::
                ResolveOptInToggle(std::getenv("REPIU_PIU10_SONG_CLOCK_LOG"));
            // Issue #58. A read inside an injected timer handler gets the
            // frame-sync bit as it was at that tick's due time, as the JAMMA
            // inputs do (see piu10_frame_sync_history.h). The flips are
            // stamped on the steady clock, so the replay needs the tick
            // schedule on it too. `REPIU_PIU10_FRAME_SYNC_REPLAY=0` answers
            // every read with the bit as it is now, as before.
            static const bool frame_sync_replay =
                repiu::runtime::ResolvePromotedToggle(
                    std::getenv("REPIU_PIU10_FRAME_SYNC_REPLAY")) &&
                ResolveEventClockUsesSteady(std::getenv("REPIU_EVENT_CLOCK"));
            context.piu10_isa_board.SetMp3StatusSource([&context]() {
                const std::uint64_t now_ns = static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count());
                std::uint8_t frame_sync = 0U;
                std::uint64_t answer_ns = now_ns;
                bool in_handler = false;
                if (frame_sync_replay)
                {
                    std::uint64_t guest_ns = 0U;
                    in_handler =
                        context.jamma_input_timeline.TryReplayTimestamp(
                            context.piu10_read_guest_esp, &guest_ns);
                    answer_ns = context.piu10_frame_sync_answer_clock.Answer(
                        guest_ns, now_ns);
                    frame_sync = context.piu10_mp3_audio.FrameSyncAt(answer_ns);
                }
                else
                {
                    frame_sync = context.piu10_mp3_audio.frame_sync();
                }
                if (TimerLatencyTraceEnabled())
                {
                    RecordTimerTrace(TimerTraceEvent::kStatusRead, now_ns,
                                     frame_sync, in_handler ? 1U : 0U,
                                     answer_ns);
                }
                Piu10SongClockRead read;
                read.now_ns = now_ns;
                read.frame_sync = frame_sync;
                read.ticks_injected =
                    context.timer_tick_delivery.injected_total.load(
                        std::memory_order_relaxed);
                read.ticks_dropped =
                    context.timer_tick_delivery.dropped_total.load(
                        std::memory_order_relaxed);
                read.pit_hz = repiu::hle::PitFrequencyHz(
                    context.pit_channel0.snapshot().divisor);
                read.frame_seconds = context.piu10_mp3_audio.frame_seconds();
                Piu10SongClockResync resync;
                if (context.piu10_song_clock.Observe(read, &resync) &&
                    (song_clock_log ||
                     std::fabs(resync.jump_ms) >=
                         kPiu10SongClockReportJumpMs))
                {
                    std::fprintf(
                        stderr,
                        "[repiu-song-clock] song=%u frame=%u jump_ms=%+.1f "
                        "ticks=%u dropped=%u window_ms=%.1f audio_ms=%.1f "
                        "tick_gap_max_ms=%.1f tick_burst=%u "
                        "toggle_gap_max_ms=%.1f frames=%u uneven=%u\n",
                        resync.song, resync.frame, resync.jump_ms,
                        resync.ticks, resync.dropped, resync.window_ms,
                        resync.audio_ms, resync.tick_gap_max_ms,
                        resync.tick_burst_max, resync.toggle_gap_max_ms,
                        resync.frames, resync.uneven_frames);
                }
                return static_cast<std::uint8_t>(
                    (frame_sync << 2U) |
                    (context.piu10_mp3_audio.demand() ? 1U : 0U));
            });
            std::fprintf(stderr, "[repiu-piu10] %s; %s; %s\n",
                         piu10_message.c_str(), flash.message.c_str(),
                         cat702_message.c_str());
        }
        else
        {
            std::fprintf(stderr,
                         "[repiu-piu10] unavailable; flash=%s; cat702=%s\n",
                         flash.message.c_str(), cat702_message.c_str());
        }
    }
    context.dos_environment_block = BuildDosEnvironmentBlock();
    repiu::runtime::InitializeSelectorTable(&context.selector_table);
    repiu::runtime::InitializeSelectorAllocator(
        &context.dpmi_selector_allocator, 0x00A4U);
    repiu::runtime::InitializeDosLowMemory(&context.dos_low_memory);
    context.shadow_selector_reservation =
        repiu::runtime::ReserveAotShadowSelectorBlock();
    context.shadow_selectors = context.shadow_selector_reservation.block;
    if (context.shadow_selectors != nullptr)
    {
        // Task i018. The block is authoritative from here on, so it inherits
        // every selector the context holds, and the accepted pairs get their
        // flat members. ES/FS/GS are usually still zero at this point, which
        // the resolution builder reads as unresolved, exactly as before.
        context.shadow_selectors->selectors[0] = context.guest_es;
        context.shadow_selectors->selectors[2] = context.guest_ss;
        context.shadow_selectors->selectors[3] = context.guest_ds;
        context.shadow_selectors->selectors[4] = context.guest_fs;
        context.shadow_selectors->selectors[5] = context.guest_gs;
        repiu::runtime::SeedAotShadowAcceptedPairs(
            context.shadow_selectors,
            context.flat_data_selector,
            context.flat_stack_selector);
    }
    if (std::getenv("REPIU_LINEXE_INIT_TRACE") != nullptr)
    {
        std::fprintf(
            stderr,
            "[repiu-guest-segments] initial_ds=%s selector=0x%04X "
            "initial_ss=%s selector=0x%04X\n",
            context.guest_ds != 0 ? "true" : "false",
            static_cast<unsigned>(context.guest_ds),
            context.guest_ss != 0 ? "true" : "false",
            static_cast<unsigned>(context.guest_ss));
    }
    // Task 586. Reported rather than assumed. A failure here is not fatal --
    // `BuildAotSegmentTable` closes every guard slot when there is no shadow --
    // but it silently converts a natively-folded run into an all-boundary one,
    // and every other reservation this loader makes prints a line.
    //
    // The ES and SS operand addresses are printed rather than only the base,
    // because they are what actually distinguishes this block from the
    // `&context->guest_xx` fallback the guards used before Task 585: the
    // block's words are four bytes apart, `ThreadContext`'s adjacent
    // `guest_es`/`guest_ss` are two.
    const auto* const shadow_block = context.shadow_selectors;
    std::fprintf(
        stderr,
        "[repiu-shadow-selector] valid=%s es=0x%08X ss=0x%08X %s\n",
        context.shadow_selector_reservation.valid ? "true" : "false",
        shadow_block == nullptr ? 0U : static_cast<std::uint32_t>(
            reinterpret_cast<std::uintptr_t>(&shadow_block->selectors[0])),
        shadow_block == nullptr ? 0U : static_cast<std::uint32_t>(
            reinterpret_cast<std::uintptr_t>(&shadow_block->selectors[2])),
        context.shadow_selector_reservation.message);
    for (const repiu::runtime::RelocatedSelectorBinding& binding :
         placement.selector_bindings)
    {
        const bool executable =
            (binding.object_flags & repiu::runtime::kLeObjectExecutable) != 0U;
        const repiu::runtime::GuestCodeDefaultOperandSize code_default =
            executable
                ? ((binding.object_flags &
                    repiu::runtime::kLeObjectBigDefault) != 0U
                      ? repiu::runtime::GuestCodeDefaultOperandSize::k32
                      : repiu::runtime::GuestCodeDefaultOperandSize::k16)
                : repiu::runtime::GuestCodeDefaultOperandSize::kUnknown;
        repiu::runtime::RegisterDescriptor(
            &context.selector_table,
            repiu::runtime::GuestDescriptor{
                binding.selector,
                binding.relocated_base_address,
                binding.limit,
                0,
                true,
                binding.object_flags,
                executable,
                code_default,
        });
    }
    const auto find_linexe_segment =
        [linexe_module](std::uint16_t selector)
            -> const exe::Dos16mBoundSegment* {
        if (linexe_module == nullptr)
        {
            return nullptr;
        }
        for (const exe::Dos16mBoundSegment& segment :
             linexe_module->segments)
        {
            if (segment.selector == selector)
            {
                return &segment;
            }
        }
        return nullptr;
    };
    const exe::Dos16mBoundSegment* extracted_code =
        find_linexe_segment(kDos4gwLinexeCodeSelector);
    const exe::Dos16mBoundSegment* extracted_bss =
        find_linexe_segment(0x0088U);
    const exe::Dos16mBoundSegment* extracted_data =
        find_linexe_segment(kDos4gwLinexeDataSelector);
    const bool extracted_linexe_valid = linexe_module != nullptr &&
        extracted_code != nullptr && extracted_bss != nullptr &&
        extracted_data != nullptr;
    bool linexe_plan_built = repiu::hle::BuildLinexeCallGatePlan(
        &context.linexe_gate_plan);
    bool linexe_layout_built = linexe_plan_built &&
        repiu::hle::BuildLinexeArenaLayout(
            placement.hle_reserve_base,
            placement.arena_end_address,
            extracted_linexe_valid
                ? static_cast<std::uint32_t>(extracted_code->image.size())
                : static_cast<std::uint32_t>(
                      context.linexe_gate_plan.gate_image.size()),
            extracted_linexe_valid
                ? static_cast<std::uint32_t>(extracted_bss->image.size())
                : 0U,
            extracted_linexe_valid
                ? static_cast<std::uint32_t>(extracted_data->image.size())
                : static_cast<std::uint32_t>(
                      context.linexe_gate_plan.private_data_image.size()),
            &context.linexe_arena_layout);
    bool glide_gate_fits = false;
    bool direct_glide_dispatch_requested = false;
    bool direct_glide_dispatch_capable = false;
    bool direct_glide_dispatch = false;
    bool client_image_written = false;
    bool gate_code_written = false;
    bool glide_gate_written = false;
    bool bss_image_written = false;
    bool images_written = false;
    bool direct_glide_image_verified = false;
    bool descriptors_registered = false;
    bool client_protected = false;
    bool private_protected = false;
    bool gates_protected = false;
    bool bss_protected = false;
    bool gates_flushed = false;
    if (linexe_layout_built)
    {
        const auto address = [](std::uint32_t value) {
            return reinterpret_cast<void*>(static_cast<std::uintptr_t>(value));
        };
        glide_gate_fits =
            context.linexe_arena_layout.gate_code_size >
                kGlideFirstGateOffset &&
            repiu::hle::BuildGlideGatePlan(
                context.glide_exports,
                kGlideFirstGateOffset,
                kGlideGateStride,
                context.linexe_arena_layout.gate_code_size -
                    kGlideFirstGateOffset,
                &context.glide_gate_plan);
        direct_glide_dispatch_requested =
            repiu::runtime::ExecutionBackendUsesDynamicTranslation(
                context.execution_backend) &&
            ResolveGlideGateDirectDispatchEnabled(
                std::getenv("REPIU_AOT_DBT_GLIDE_GATE_DISPATCH"));
        direct_glide_dispatch_capable =
            GetGlideGateDirectDispatchThunkAddress() != nullptr;
        direct_glide_dispatch = direct_glide_dispatch_requested &&
            direct_glide_dispatch_capable;
        context.aot_dbt_glide_direct_dispatch = direct_glide_dispatch;
        if (glide_gate_fits && direct_glide_dispatch)
        {
            glide_gate_fits = PatchGlideGatePlanForDirectDispatch(
                context.linexe_arena_layout.gate_code_base,
                &context.glide_gate_plan);
        }
        // Task 524: the five writes are named so a failure says which one.
        //
        // Folded into one expression they were indistinguishable, and the
        // whole DOS/4G environment -- and with it INT 21h AX=FF00h -- hangs
        // off their conjunction.
        client_image_written =
            WriteGuestBytes(&context,
                            address(context.linexe_arena_layout.client_data_base),
                            context.linexe_gate_plan.client_data_image.data(),
                            context.linexe_gate_plan.client_data_image.size());
        gate_code_written =
            client_image_written &&
            WriteGuestBytes(&context,
                            address(context.linexe_arena_layout.gate_code_base),
                            extracted_linexe_valid
                                ? extracted_code->image.data()
                                : context.linexe_gate_plan.gate_image.data(),
                            extracted_linexe_valid
                                ? extracted_code->image.size()
                                : context.linexe_gate_plan.gate_image.size());
        glide_gate_written =
            gate_code_written && glide_gate_fits &&
            WriteGuestBytes(
                &context,
                address(context.linexe_arena_layout.gate_code_base +
                        kGlideFirstGateOffset),
                context.glide_gate_plan.image.data(),
                context.glide_gate_plan.image.size());
        bss_image_written =
            glide_gate_written &&
            (!extracted_linexe_valid || WriteGuestBytes(
                &context,
                address(context.linexe_arena_layout.bss_base),
                extracted_bss->image.data(),
                extracted_bss->image.size()));
        images_written =
            bss_image_written &&
            WriteGuestBytes(&context,
                            address(context.linexe_arena_layout.private_data_base),
                            extracted_linexe_valid
                                ? extracted_data->image.data()
                                : context.linexe_gate_plan.private_data_image.data(),
                            extracted_linexe_valid
                                ? extracted_data->image.size()
                                : context.linexe_gate_plan.private_data_image.size());
        direct_glide_image_verified =
            !direct_glide_dispatch ||
            (images_written && VerifyGlideGateDirectDispatchImage(
                context.linexe_arena_layout.gate_code_base,
                context.glide_gate_plan));
        descriptors_registered =
            direct_glide_image_verified && images_written &&
            repiu::runtime::RegisterDescriptor(
                &context.selector_table,
                {kDos4gwClientDataSelector,
                 context.linexe_arena_layout.client_data_base,
                 0x0FFFU, 0, true}) &&
            repiu::runtime::RegisterDescriptor(
                &context.selector_table,
                {kDos4gwLinexeDataSelector,
                 context.linexe_arena_layout.private_data_base,
                 extracted_linexe_valid ? extracted_data->limit : 0x0FFFU,
                 0, true}) &&
            repiu::runtime::RegisterDescriptor(
                &context.selector_table,
                {kDos4gwLinexeCodeSelector,
                 context.linexe_arena_layout.gate_code_base,
                 context.linexe_arena_layout.gate_code_size - 1U,
                 0, true}) &&
            (!extracted_linexe_valid || repiu::runtime::RegisterDescriptor(
                &context.selector_table,
                {0x0088U,
                 context.linexe_arena_layout.bss_base,
                 extracted_bss->limit,
                 0, true}));
        if (descriptors_registered)
        {
            using repiu::platform::MemoryProtection;
            client_protected = repiu::platform::ProtectMemory(
                address(context.linexe_arena_layout.client_data_base),
                0x1000U, MemoryProtection::kReadOnly, nullptr);
            private_protected = repiu::platform::ProtectMemory(
                address(context.linexe_arena_layout.private_data_base),
                context.linexe_arena_layout.private_data_size,
                extracted_linexe_valid ? MemoryProtection::kReadWrite
                                       : MemoryProtection::kReadOnly,
                nullptr);
            gates_protected = repiu::platform::ProtectMemory(
                address(context.linexe_arena_layout.gate_code_base),
                context.linexe_arena_layout.gate_code_size,
                direct_glide_dispatch
                    ? MemoryProtection::kExecuteRead
                    : (extracted_linexe_valid
                           ? MemoryProtection::kReadWrite
                           : MemoryProtection::kExecuteRead),
                nullptr);
            bss_protected = !extracted_linexe_valid ||
                repiu::platform::ProtectMemory(
                    address(context.linexe_arena_layout.bss_base),
                    context.linexe_arena_layout.bss_size,
                    MemoryProtection::kReadWrite, nullptr);
            gates_flushed = !direct_glide_dispatch ||
                (gates_protected && repiu::platform::FlushInstructionCacheRange(
                    address(context.linexe_arena_layout.gate_code_base),
                    context.linexe_arena_layout.gate_code_size));
            context.linexe_environment_active =
                client_protected && private_protected && gates_protected &&
                bss_protected && gates_flushed;
        }
    }
    if (std::getenv("REPIU_LINEXE_INIT_TRACE") != nullptr)
    {
        std::fprintf(
            stderr,
            "[repiu-linexe-init] extracted=%u plan=%u layout=%u "
            "glide_fits=%u requested=%u capable=%u direct=%u "
            "writes=%u/%u/%u/%u/%u "
            "verify=%u descriptors=%u protect=%u/%u/%u/%u/%u active=%u "
            "bases=0x%08X/0x%08X/0x%08X/0x%08X sizes=0x%08X/0x%08X/0x%08X/0x%08X\n",
            extracted_linexe_valid ? 1U : 0U,
            linexe_plan_built ? 1U : 0U,
            linexe_layout_built ? 1U : 0U,
            glide_gate_fits ? 1U : 0U,
            direct_glide_dispatch_requested ? 1U : 0U,
            direct_glide_dispatch_capable ? 1U : 0U,
            direct_glide_dispatch ? 1U : 0U,
            client_image_written ? 1U : 0U,
            gate_code_written ? 1U : 0U,
            glide_gate_written ? 1U : 0U,
            bss_image_written ? 1U : 0U,
            images_written ? 1U : 0U,
            direct_glide_image_verified ? 1U : 0U,
            descriptors_registered ? 1U : 0U,
            client_protected ? 1U : 0U,
            private_protected ? 1U : 0U,
            gates_protected ? 1U : 0U,
            bss_protected ? 1U : 0U,
            gates_flushed ? 1U : 0U,
            context.linexe_environment_active ? 1U : 0U,
            context.linexe_arena_layout.client_data_base,
            context.linexe_arena_layout.gate_code_base,
            context.linexe_arena_layout.bss_base,
            context.linexe_arena_layout.private_data_base,
            context.linexe_arena_layout.client_data_size,
            context.linexe_arena_layout.gate_code_size,
            context.linexe_arena_layout.bss_size,
            context.linexe_arena_layout.private_data_size);
    }
    if (context.linexe_environment_active)
    {
        context.aot_excluded_guest_ranges.push_back({
            context.linexe_arena_layout.gate_code_base,
            context.linexe_arena_layout.gate_code_size});
    }
    if (dos_file_system != nullptr)
    {
        context.dos_file_system = *dos_file_system;
    }

    const auto stop_translation_worker = [&context]() {
        if (context.aot_translation_thread.valid)
        {
            context.aot_translation_shutdown.store(
                true, std::memory_order_release);
            if (context.aot_translation_request_event == nullptr ||
                !repiu::platform::SignalWorker(
                    context.aot_translation_request_event) ||
                !repiu::platform::JoinHostThread(
                    context.aot_translation_thread,
                    repiu::runtime::kWaitForeverMilliseconds, nullptr))
            {
                // Context ownership cannot be released while the worker could
                // still reference it. Treat an impossible join failure as a
                // process-local terminal failure rather than creating UAF.
                std::abort();
            }
            repiu::platform::CloseHostThread(&context.aot_translation_thread);
        }
        if (context.aot_translation_request_event != nullptr)
        {
            repiu::platform::DestroyWorkerSignal(
                context.aot_translation_request_event);
            context.aot_translation_request_event = nullptr;
        }
        if (context.aot_translation_complete_event != nullptr)
        {
            repiu::platform::DestroyWorkerSignal(
                context.aot_translation_complete_event);
            context.aot_translation_complete_event = nullptr;
        }
    };
    ReResolveAotSegmentOverrides(&context);
    if (context.aot_placement != nullptr)
    {
        context.aot_translation_request_event =
            repiu::platform::CreateWorkerSignal();
        context.aot_translation_complete_event =
            repiu::platform::CreateWorkerSignal();
        if (context.aot_translation_request_event == nullptr ||
            context.aot_translation_complete_event == nullptr)
        {
            stop_translation_worker();
            attempt->message = "failed to create AOT translation events";
            return false;
        }
        if (!repiu::platform::CreateHostThread(&AotTranslationWorkerThunk,
                                               &context,
                                               &context.aot_translation_thread,
                                               nullptr))
        {
            stop_translation_worker();
            attempt->message = "failed to create AOT translation worker";
            return false;
        }
        const std::uint32_t trace_address = GuestWriteTraceAddress();
        const std::uint64_t runtime_end =
            static_cast<std::uint64_t>(context.runtime_base) +
            context.runtime_size;
        const bool trace_address_in_runtime =
            trace_address != 0U &&
            trace_address >= context.runtime_base &&
            static_cast<std::uint64_t>(trace_address) < runtime_end;
        const std::uint32_t trace_page = trace_address & 0xFFFFF000U;
        const std::vector<std::uint32_t> trace_pages =
            trace_address_in_runtime
                ? std::vector<std::uint32_t>{trace_page}
                : std::vector<std::uint32_t>{};
        if (!InstallAotGuestPageWriteWatches(
                *context.aot_placement, nullptr,
                &context.aot_page_write_watch) ||
            (trace_address_in_runtime &&
             !InstallAotGuestPageWriteWatches(
                 *context.aot_placement, &trace_pages,
                 &context.aot_page_write_watch)))
        {
            RestoreAotGuestPageWriteWatches(
                &context.aot_page_write_watch);
            stop_translation_worker();
            attempt->message =
                "failed to install AOT guest code write watches";
            return false;
        }
        if (trace_address_in_runtime &&
            GuestWriteTraceNativeObserverEnabled())
        {
            // The native observer sees the watched slot after each emitted
            // explicit write. Disarm only this diagnostic page so the
            // observer does not turn the guest stack into a write fault; all
            // other AOT coherence watches remain active.
            RemoveAotPageWriteWatch(
                &context.aot_page_write_watch, trace_page);
        }
    }

    context.glide_backend.SetJammaInputTimeline(&context.jamma_input_timeline);
    context.glide_backend.SetBiosKeyboard(&context.bios_keyboard);
    context.glide_backend.BindHostThread();
    repiu::platform::HostThread thread;
    std::uint32_t create_error = 0;
    const repiu::platform::HostThreadEntry guest_thread_proc =
        SelectGuestThreadEntry();
    if (!repiu::platform::CreateHostThread(guest_thread_proc, &context,
                                           &thread, &create_error))
    {
        std::ostringstream stream;
        stream << "guest thread creation failed with host error "
               << create_error;
        attempt->message = stream.str();
        RestoreAotGuestPageWriteWatches(&context.aot_page_write_watch);
        stop_translation_worker();
        return false;
    }

    attempt->attempted = true;
    if (context.shared_live_telemetry != nullptr)
    {
        repiu::platform::AtomicExchange(&context.shared_live_telemetry->host_phase, 2);
        repiu::platform::AtomicExchange(
            &context.shared_live_telemetry->guest_thread_id,
            static_cast<long>(thread.id));
        repiu::platform::AtomicExchange(
            &context.shared_live_telemetry->host_main_thread_id,
            static_cast<long>(repiu::platform::CurrentThreadId()));
        if (context.aot_placement != nullptr &&
            context.aot_placement->placed)
        {
            repiu::platform::AtomicExchange(
                &context.shared_live_telemetry->aot_cache_base,
                static_cast<long>(context.aot_placement->base_address));
            repiu::platform::AtomicExchange(
                &context.shared_live_telemetry->aot_cache_size,
                static_cast<long>(context.aot_placement->capacity));
        }
    }
    attempt->guest_stack_switch_attempted = use_guest_stack;
    std::uint32_t exit_code = 0;
    const HostPollOutcome wait_result = PollThreadUntilExit(
        thread,
        timeout_milliseconds,
        stall_timeout_milliseconds,
        (enable_single_step_trace || aot_placement != nullptr ||
         stall_timeout_milliseconds !=
             repiu::runtime::kWaitForeverMilliseconds)
            ? &context : nullptr,
        &context,
        &exit_code,
        &attempt->stall_timed_out);

    // Task 503d-19: 3c owns installing and removing the fault handler on both
    // hosts. `vectored_handler` is the flag saying one is installed; what it
    // used to hold -- the cookie Windows hands back -- is the backend's
    // business now.
    const auto remove_vectored_handler = [&context]() {
        if (context.vectored_handler != nullptr)
        {
            repiu::platform::RemoveFaultHandler();
            context.vectored_handler = nullptr;
        }
    };

    const bool host_exit_requested =
        wait_result == HostPollOutcome::kHostExitRequested;
    if (wait_result == HostPollOutcome::kTimedOut || host_exit_requested)
    {
        attempt->timed_out = !host_exit_requested;
        attempt->quit_requested = host_exit_requested;
        const std::uint32_t interruption_exit_code =
            host_exit_requested ? 0U : 3U;
        attempt->thread_exit_code = interruption_exit_code;

        // Task 507: the graceful path runs on both hosts now.
        //
        // What stood here was four Win32 calls -- suspend, read the context,
        // write it back, resume -- which is exactly what `InterruptHostThread`
        // performs on Windows, and what a single signal performs on Linux.
        // 3d-18 named that equivalence and 3d-20 built it; this is the caller
        // that was waiting for it.
        //
        // **The callback runs on a different thread on each host**, so nothing
        // that allocates may be inside it. The messages below are therefore
        // written here, on the thread that asked, rather than in the callback
        // where they used to be.
        //
        // A short deadline: this is a shutdown, and a guest thread that does not
        // answer within it is a finding rather than something to wait out.
        constexpr std::uint32_t kShutdownInterruptTimeoutMilliseconds = 200U;
        // Task 507: asked more than once, because one look lands wherever the
        // thread happens to be.
        //
        // The guest thread spends much of a frame inside the engine rather than
        // inside guest code -- an HLE call, the Glide gate, a dispatch handler
        // -- and the callback refuses all of those, correctly: they are frames
        // the recovery entry cannot unwind. A single sample therefore reports
        // "not in recoverable code" most of the time even though the thread
        // returns to guest code microseconds later. Measured on `pumpit1`, one
        // attempt found it there in none of the runs tried.
        //
        // Bounded, and short: this is the shutdown path, and a guest thread that
        // never comes back to its own code -- one waiting on a host that has
        // stopped pumping, for instance -- has to be given up on rather than
        // waited for.
        constexpr std::uint32_t kShutdownRecoveryAttempts = 40U;
        constexpr std::uint32_t kShutdownRecoveryRetryMilliseconds = 5U;
        GuestShutdownRecoveryRequest recovery_request;
        recovery_request.context = &context;
        const char* shutdown_recovery_trace =
            std::getenv("REPIU_SHUTDOWN_RECOVERY_TRACE");
        recovery_request.trace_enabled = shutdown_recovery_trace != nullptr &&
            shutdown_recovery_trace[0] != '\0' &&
            std::strcmp(shutdown_recovery_trace, "0") != 0;
#if defined(_WIN32)
        // Task 733: installed before the diagnostic trace handler, so the trace
        // (registered later, called first) still sees what the guard handles.
        g_shutdown_redirect_guard.redirect_applied.store(
            false, std::memory_order_release);
        g_shutdown_redirect_guard.reapplied_at_entry.store(
            0U, std::memory_order_relaxed);
        g_shutdown_redirect_guard.reapplied_from_guest.store(
            0U, std::memory_order_relaxed);
        g_shutdown_redirect_guard.guest_thread_id.store(
            thread.id, std::memory_order_release);
        g_shutdown_redirect_guard.context.store(
            &context, std::memory_order_release);
        PVOID shutdown_redirect_guard_handler =
            AddVectoredExceptionHandler(1, &ShutdownRedirectGuardHandler);
        PVOID shutdown_recovery_exception_handler = nullptr;
        if (recovery_request.trace_enabled)
        {
            g_shutdown_recovery_exception_trace_remaining.store(
                0, std::memory_order_release);
            shutdown_recovery_exception_handler = AddVectoredExceptionHandler(
                1, &ShutdownRecoveryExceptionTraceHandler);
        }
#endif
        recovery_request.snapshot =
            host_exit_requested ? nullptr : &attempt->timeout_snapshot;
        repiu::platform::ThreadInterruptFailure interrupt_failure =
            repiu::platform::ThreadInterruptFailure::kNone;
        bool interrupt_answered = false;
        std::uint32_t recovery_attempts = 0;
        while (recovery_attempts < kShutdownRecoveryAttempts)
        {
            ++recovery_attempts;
            recovery_request.attempt = recovery_attempts;
            interrupt_answered = repiu::platform::InterruptHostThreadWithContext(
                thread, &RecoverGuestThreadForShutdownWithContext,
                &recovery_request, kShutdownInterruptTimeoutMilliseconds,
                &interrupt_failure);
            if (!interrupt_answered || recovery_request.recovered)
            {
                break;
            }
            // The first sample is the one that answers "where was the guest when
            // the run ended"; the ones after it are only looking for a moment to
            // act on, and would otherwise overwrite that answer with a later
            // position.
            recovery_request.snapshot = nullptr;
            // Task 507: the gate has to keep being served while this waits.
            //
            // A guest thread inside the Glide gate is waiting for the host to
            // execute the command it handed over, and the host is here. Stop
            // pumping and it never returns to guest code, so every attempt
            // refuses and the loop spends its whole budget watching a thread
            // that cannot move. Measured on `pumpit1`: a run whose guest was in
            // the gate refused all forty attempts, and the same run's teardown
            // then blocked behind the same thread.
            context.glide_backend.PumpHostCommands();
            repiu::platform::YieldMilliseconds(
                kShutdownRecoveryRetryMilliseconds);
        }

        bool gracefully_interrupted =
            interrupt_answered && recovery_request.recovered;
        if (!interrupt_answered &&
            interrupt_failure ==
                repiu::platform::ThreadInterruptFailure::kNotDelivered)
        {
            // Undelivered means there was no thread to deliver to, which on this
            // path usually means it ended on its own between the poll loop
            // giving up and this asking. A thread that has exited is one that
            // stopped, so it is joined rather than detached.
            gracefully_interrupted =
                repiu::platform::JoinHostThread(thread, 100U, nullptr);
        }
        if (gracefully_interrupted && recovery_request.recovered)
        {
            // Task 503d-18: one question instead of two. What stood here waited
            // and then read GetExitCodeThread, comparing against STILL_ACTIVE
            // -- which is what that call also reports for a thread that is
            // still running. The code it collected was never read, and what
            // this establishes is only that the thread stopped, so the join
            // says it on its own.
            if (!repiu::platform::JoinHostThread(thread, 3000U, nullptr))
            {
                gracefully_interrupted = false;
            }
        }

#if defined(_WIN32)
        g_shutdown_recovery_exception_trace_remaining.store(
            0, std::memory_order_release);
        if (shutdown_recovery_exception_handler != nullptr)
        {
            RemoveVectoredExceptionHandler(
                shutdown_recovery_exception_handler);
        }
#endif

        if (gracefully_interrupted && recovery_request.recovered)
        {
            context.hle_message = host_exit_requested
                ? "SDL exit requested; hijacked guest thread for clean teardown"
                : "timeout reached during guest execution; hijacked thread for clean teardown";
        }
        else if (gracefully_interrupted)
        {
            // The thread had ended by itself before this asked.
            context.hle_message = host_exit_requested
                ? "exit requested; guest thread had already exited"
                : "timeout reached; guest thread had already exited";
        }
        else if (!interrupt_answered)
        {
            // Which of the three it was decides what to look at next, so it is
            // named rather than folded into "failed": a refusal is this caller's
            // bug, an undelivered signal means the thread is already gone, and a
            // timeout means it is there and not answering -- the shape the
            // stalled interrupt handler in the frontier's section 6 leaves.
            const char* reason = "refused";
            switch (interrupt_failure)
            {
                case repiu::platform::ThreadInterruptFailure::kNotDelivered:
                    reason = "not delivered";
                    break;
                case repiu::platform::ThreadInterruptFailure::kTimedOut:
                    reason = "no answer within the deadline";
                    break;
                default:
                    break;
            }
            context.hle_message =
                (host_exit_requested
                     ? std::string("exit requested; guest thread not stopped (")
                     : std::string("timeout reached; guest thread not stopped (")) +
                reason + ")";
        }
        else
        {
            // The interrupt worked and the callback declined: the guest thread
            // was outside its own image and outside the AOT cache, so there was
            // no frame to unwind it through.
            context.hle_message = host_exit_requested
                ? "exit requested; guest thread was not in recoverable code"
                : "timeout reached; guest thread was not in recoverable code";
        }

        // Task 507: Windows only, and deliberately so. Nothing in POSIX stops a
        // thread that is not asking to be stopped -- pthread_cancel acts at
        // cancellation points and guest code has none -- so on Linux a guest
        // thread that refused the interrupt keeps running, and what changes
        // below is only that the loader stops waiting for it.
        // Task 759: the platform layer says whether it asked the thread to stop.
        if (!gracefully_interrupted &&
            repiu::platform::TerminateHostThread(
                thread, static_cast<std::uint32_t>(interruption_exit_code)))
        {
            gracefully_interrupted =
                repiu::platform::JoinHostThread(thread, 5000U, nullptr);
        }

        // Task 507: written here rather than left to the loader's summary,
        // because that summary is printed after teardown, and a teardown that
        // dumps core takes it with it -- which is how this path came to be
        // investigated with no record of what it had done. One unbuffered line,
        // on the stream and through the call the live telemetry already uses.
        {
            // Task 509: `frames` and `span_ms` ride along here because this is
            // the one line both arms print, and because a Linux run that
            // reaches rendering takes the arm that prints nothing else -- so
            // this is the only place its speed can be recorded at all.
            //
            // `span_ms` starts at the first presented frame rather than at
            // process start: `pumpit1` spends roughly forty-five seconds
            // decoding assets with nothing on screen, and dividing by the whole
            // budget would understate the rate several times over.
            //
            // Read without a lock on purpose -- both counters are written on the
            // host thread, which is the thread running this block.
            char shutdown_line[448] = {};
            const int length = std::snprintf(
                shutdown_line, sizeof(shutdown_line),
                "[repiu-shutdown] reason=%s attempts=%u answered=%d "
                "recovered=%d stopped=%d failure=%u eip=0x%08X gate=%d "
                "frames=%llu span_ms=%llu host_ip=0x%llX decision=%s "
                "aliased=%u\n",
                host_exit_requested ? "exit-requested" : "timeout",
                static_cast<unsigned>(recovery_attempts),
                interrupt_answered ? 1 : 0,
                recovery_request.recovered ? 1 : 0,
                gracefully_interrupted ? 1 : 0,
                static_cast<unsigned>(interrupt_failure),
                static_cast<unsigned>(recovery_request.last_eip),
                context.glide_backend.guest_in_glide_gate() ? 1 : 0,
                static_cast<unsigned long long>(
                    context.glide_backend.presented_frame_total()),
                static_cast<unsigned long long>(
                    context.glide_backend.presented_frame_span_milliseconds()),
                static_cast<unsigned long long>(recovery_request.last_host_ip),
                ShutdownRecoveryDecisionName(recovery_request.last_decision),
                static_cast<unsigned>(recovery_request.aliased_count));
            if (length > 0)
            {
                repiu::platform::WriteHostErrorStream(
                    shutdown_line,
                    static_cast<std::size_t>(length) < sizeof(shutdown_line)
                        ? static_cast<std::size_t>(length)
                        : sizeof(shutdown_line) - 1U);
            }
        }

        // Task 507: the same milestones the uninterrupted path publishes through
        // `host_phase`, on the stream instead.
        //
        // The comment below already knew this sequence can block; what it could
        // not say was where, because nothing on this path names the step it is
        // in and the loader's summary comes after all of it. A run whose guest
        // thread was left in the Glide gate blocked here for ten minutes with
        // nothing between the decision above and silence.
        const auto mark_shutdown_step = [](const char* step) {
            char line[96] = {};
            const int length = std::snprintf(
                line, sizeof(line), "[repiu-shutdown] step=%s\n", step);
            if (length > 0)
            {
                repiu::platform::WriteHostErrorStream(
                    line,
                    static_cast<std::size_t>(length) < sizeof(line)
                        ? static_cast<std::size_t>(length)
                        : sizeof(line) - 1U);
            }
        };

        // Task 401: write the census before Glide close, handler removal, and
        // worker shutdown -- a 45-second interrupted pumpit3 run was observed
        // hanging in that sequence, which would otherwise discard the whole
        // thing. That reason still holds; what 507 changed is the sentence this
        // comment opened with, which said the guest thread has stopped by now.
        // It has not necessarily, so this is first for the same reason it was:
        // it is the last point at which the census is certain to be written.
        if (context.single_step_hotspot_profile != nullptr)
        {
            mark_shutdown_step("hotspot-dump");
            WriteSingleStepHotspotDumpIfEnabled(
                context.single_step_hotspot_profile.get(), nullptr, nullptr);
        }

        // Task 508: a guest thread that refused recovery is still running, and
        // everything below this point was written for one that stopped.
        //
        // The step that matters is `remove_vectored_handler`. It restores the
        // dispositions of SIGSEGV, SIGBUS, SIGTRAP, SIGILL and SIGFPE to what
        // they were before 3c installed itself, which is the default -- nothing
        // else in this repository touches those five. A running guest thread
        // plants an INT3 or sets the trap flag in the ordinary course of
        // dispatching, so the next one it hits after this call has nothing left
        // to receive it, and the kernel's default disposition dumps core. Two of
        // six measured runs died that way, both with `translation-worker` -- the
        // step right after the removal -- as the last marker they printed.
        // **This path was removing a handler a live thread still needs.**
        //
        // So it is not removed, and neither is anything else done that only a
        // stopped thread makes safe. What is skipped is not cleanup that got
        // lost: `_Exit` hands the Glide and SDL resources back to the kernel,
        // the worker join can block, restoring page protections under a running
        // thread is the more dangerous of the two options, and `attempt` never
        // reaches the caller from here. Both dumps that write a file survive --
        // the hotspot census above, and the probe dump below, which only writes
        // bytes captured earlier and so races nothing.
        if (!gracefully_interrupted)
        {
            attempt->guest_thread_stopped = false;
            mark_shutdown_step("probe-dump");
            WriteExecutionProbeDump(context.execution_probe_dump_request,
                                         &context.execution_probe_dump_result);
            // Kept even though `_Exit` follows, because the call is this
            // caller's statement that it cannot say the thread stopped. It is
            // the half of the pair 507 built against `CloseHostThread`'s join,
            // and dropping it would leave that function with no caller.
            repiu::platform::DetachHostThread(&thread);
            // Task 709. The loader never sees this arm's `attempt`, because
            // `_Exit` below does not return -- so the report goes out from
            // here instead. Without it a run that refused recovery leaves no
            // summary at all, and Linux x64 refuses on every run: the DOS
            // path and file-I/O traces, the Glide ordinal counts and the rest
            // of what a run is read by afterwards have never once been printed
            // there.
            //
            // Only what is safe to read beside a live guest thread is filled
            // in. `CopyThreadObservationToAttempt` copies scalars and POD
            // arrays and touches no `std::string`, so the worst it can produce
            // is a counter read mid-update -- a stale or torn diagnostic value,
            // which is plainly better than printing nothing. The string members
            // are deliberately left alone: the guest thread writes those while
            // servicing HLE, and copying one concurrently would let a
            // diagnostic kill the process.
            //
            // Nothing else about this arm changes. The fault handler, the
            // Glide close, the worker join and the page protections are still
            // not touched, for the reason recorded above.
            CopyThreadObservationToAttempt(context, attempt);
            attempt->valid = true;
            attempt->message =
                "guest thread was not recovered; summary reported from the "
                "immediate-exit path";
            mark_shutdown_step("final-report");
            EmitFinalExecutionReport(*attempt);
            mark_shutdown_step("immediate-exit");
            // Task 507's reasoning, unchanged: once recovery has been refused,
            // nothing this process does afterwards can be trusted not to race
            // the thread that is still running.
            std::fflush(nullptr);
            std::_Exit(static_cast<int>(interruption_exit_code));
        }

        mark_shutdown_step("glide-close");
        context.glide_backend.Close();
        mark_shutdown_step("fault-handler");
        remove_vectored_handler();
        mark_shutdown_step("translation-worker");
        stop_translation_worker();
        mark_shutdown_step("write-watches");
        RestoreAotGuestPageWriteWatches(&context.aot_page_write_watch);
        mark_shutdown_step("probe-dump");
        WriteExecutionProbeDump(context.execution_probe_dump_request,
                                     &context.execution_probe_dump_result);
        mark_shutdown_step("thread-release");
        CopyThreadObservationToAttempt(context, attempt);
        attempt->valid = true;
        attempt->message = context.hle_message.empty()
            ? (host_exit_requested
                ? "minimal execution stopped by SDL exit request"
                : "minimal execution attempt timed out")
            : context.hle_message;
        // Task 507: `CloseHostThread` joins to reclaim the thread's stack, so on
        // a guest thread still running it never returns -- which is what a Linux
        // run asked to quit did, after SDL turned the signal into a quit event
        // and brought it here.
        //
        // Task 508: the guard that used to stand here moved to the top of this
        // sequence. Everything from here down runs only for a thread that
        // stopped, so the join is once again the correct call rather than a
        // choice between two.
        repiu::platform::CloseHostThread(&thread);
#if defined(_WIN32)
        // Task 733: the guest thread has stopped, so no exception is left in
        // flight for the guard to complete. The counts are printed here rather
        // than on the shutdown line, which can be written before the guest
        // receives the exception the guard handles.
        g_shutdown_redirect_guard.context.store(nullptr, std::memory_order_release);
        if (shutdown_redirect_guard_handler != nullptr)
        {
            RemoveVectoredExceptionHandler(shutdown_redirect_guard_handler);
        }
        {
            char guard_line[128] = {};
            const int guard_length = std::snprintf(
                guard_line, sizeof(guard_line),
                "[repiu-shutdown] redirect-guard reapplied_entry=%u "
                "reapplied_guest=%u\n",
                static_cast<unsigned>(
                    g_shutdown_redirect_guard.reapplied_at_entry.load(
                        std::memory_order_relaxed)),
                static_cast<unsigned>(
                    g_shutdown_redirect_guard.reapplied_from_guest.load(
                        std::memory_order_relaxed)));
            if (guard_length > 0)
            {
                repiu::platform::WriteHostErrorStream(
                    guard_line,
                    static_cast<std::size_t>(guard_length) < sizeof(guard_line)
                        ? static_cast<std::size_t>(guard_length)
                        : sizeof(guard_line) - 1U);
            }
        }
#endif
        if (aot_placement != nullptr)
        {
            TraceAotGuestMap(*aot_placement, context.runtime_base, "final");
            TraceAotCacheMap(*aot_placement, "final");
        }
        mark_shutdown_step("done");
        return true;
    }

    // Teardown milestones are published through host_phase so an external
    // supervisor can locate the exact step if this path blocks.
    context.glide_backend.Close();
    const auto set_teardown_phase = [&context](long phase) {
        if (context.shared_live_telemetry != nullptr)
        {
            repiu::platform::AtomicExchange(
                &context.shared_live_telemetry->host_phase, phase);
        }
    };
    set_teardown_phase(10);
    remove_vectored_handler();
    set_teardown_phase(11);
    stop_translation_worker();
    set_teardown_phase(12);
    RestoreAotGuestPageWriteWatches(&context.aot_page_write_watch);
    set_teardown_phase(13);
    repiu::platform::CloseHostThread(&thread);
    set_teardown_phase(14);
    if (aot_placement != nullptr)
    {
        TraceAotGuestMap(*aot_placement, context.runtime_base, "final");
        TraceAotCacheMap(*aot_placement, "final");
    }

    attempt->returned = context.returned;
    attempt->exception_caught = context.exception_caught;
    attempt->guest_stack_return_esp = context.guest_return_esp;
    attempt->seh_exception_code = context.exception_code;
    attempt->seh_exception_address = context.exception_address;
    attempt->exception_eax = context.exception_eax;
    attempt->exception_ebx = context.exception_ebx;
    attempt->exception_ecx = context.exception_ecx;
    attempt->exception_edx = context.exception_edx;
    attempt->exception_esi = context.exception_esi;
    attempt->exception_edi = context.exception_edi;
    attempt->exception_snapshot = context.exception_snapshot;
    attempt->exception_access_kind = context.exception_access_kind;
    attempt->exception_fault_va = context.exception_fault_va;
    attempt->exception_fault_region_base =
        context.exception_fault_region_base;
    attempt->exception_fault_alloc_base = context.exception_fault_alloc_base;
    attempt->exception_fault_state = context.exception_fault_state;
    attempt->exception_fault_protect = context.exception_fault_protect;
    attempt->exception_fault_region_size =
        context.exception_fault_region_size;
    std::memcpy(attempt->exception_esi_dwords,
                context.exception_esi_dwords,
                sizeof(attempt->exception_esi_dwords));
    attempt->exception_esi_dword_valid_mask =
        context.exception_esi_dword_valid_mask;
    std::memcpy(attempt->exception_register_strings,
                context.exception_register_strings,
                sizeof(attempt->exception_register_strings));
    attempt->exception_register_string_valid_mask =
        context.exception_register_string_valid_mask;
    attempt->exception_stack_base = context.exception_stack_base;
    std::memcpy(attempt->exception_stack_dwords,
                context.exception_stack_dwords,
                sizeof(attempt->exception_stack_dwords));
    attempt->exception_stack_dword_count =
        context.exception_stack_dword_count;
    attempt->unhandled_breakpoint_evidence =
        context.unhandled_breakpoint_evidence;
    WriteExecutionProbeDump(context.execution_probe_dump_request,
                                 &context.execution_probe_dump_result);
    CopyThreadObservationToAttempt(context, attempt);
    attempt->thread_exit_code = exit_code;
    attempt->hle_stdout_output.assign(
        context.hle_stdout_output,
        context.hle_stdout_output + context.hle_stdout_output_size);
    attempt->hle_stderr_output.assign(
        context.hle_stderr_output,
        context.hle_stderr_output + context.hle_stderr_output_size);
    attempt->valid = true;

    if (attempt->returned)
    {
        attempt->message = context.hle_message.empty()
                               ? "original entry returned to host trampoline"
                               : context.hle_message;
    }
    else if (attempt->exception_caught)
    {
        attempt->message = context.hle_message.empty()
                               ? "original entry raised a caught exception"
                               : context.hle_message;
    }
    else
    {
        attempt->message =
            "minimal execution attempt ended without return or exception";
    }

    return true;
}

bool AttemptGuestStackTrapExecution(
    const RelocatedImagePlacement& placement,
    const runtime::GuestStackSwitchPlan& stack_plan,
    const hle::DosVirtualFileSystemState& dos_file_system,
    const exe::Dos16mBoundModule* linexe_module,
    const std::vector<exe::LeResidentName>* glide_exports,
    const std::filesystem::path* cd_chd_path,
    const std::filesystem::path* sound_rom_zip_path,
    bool enable_piu_jamma_board,
    bool enable_piu10_isa_board,
    bool enable_cat702,
    std::string_view parent_rom_set_id,
    std::uint32_t piu10_mp3_latency_ms,
    std::uint32_t timeout_milliseconds,
    std::uint32_t stall_timeout_milliseconds,
    MinimalExecutionAttempt* attempt)
{
    if (attempt == nullptr)
    {
        return false;
    }

    if (!stack_plan.valid)
    {
        *attempt = MinimalExecutionAttempt{};
        attempt->entry_address = stack_plan.entry_eip;
        attempt->guest_stack_initial_esp = stack_plan.initial_esp;
        attempt->message = "guest stack switch plan is not valid";
        return false;
    }

    return RunExecutionThread(
        placement,
        stack_plan.entry_eip,
        stack_plan.initial_esp,
        FindGuestStackSelector(placement, stack_plan),
        true,
        true,
        true,
        true,
        false,
        true,
        &dos_file_system,
        linexe_module,
        glide_exports,
        cd_chd_path,
        sound_rom_zip_path,
        enable_piu_jamma_board,
        enable_piu10_isa_board,
        enable_cat702,
        parent_rom_set_id,
        piu10_mp3_latency_ms,
        nullptr,
        runtime::ExecutionBackend::kLegacy,
        timeout_milliseconds,
        stall_timeout_milliseconds,
        attempt);
}

bool AttemptGuestStackHleExecution(
    const RelocatedImagePlacement& placement,
    const runtime::GuestStackSwitchPlan& stack_plan,
    const hle::DosVirtualFileSystemState& dos_file_system,
    std::uint32_t timeout_milliseconds,
    std::uint32_t stall_timeout_milliseconds,
    MinimalExecutionAttempt* attempt)
{
    if (attempt == nullptr)
    {
        return false;
    }

    if (!stack_plan.valid)
    {
        *attempt = MinimalExecutionAttempt{};
        attempt->entry_address = stack_plan.entry_eip;
        attempt->guest_stack_initial_esp = stack_plan.initial_esp;
        attempt->message = "guest stack switch plan is not valid";
        return false;
    }

    return RunExecutionThread(
        placement,
        stack_plan.entry_eip,
        stack_plan.initial_esp,
        FindGuestStackSelector(placement, stack_plan),
        true,
        true,
        true,
        true,
        true,
        false,
        &dos_file_system,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        false,
        false,
        false,
        {},
        0U,
        nullptr,
        runtime::ExecutionBackend::kLegacy,
        timeout_milliseconds,
        stall_timeout_milliseconds,
        attempt);
}

bool AttemptGuestStackAotExecution(
    const RelocatedImagePlacement& placement,
    AotCodeCachePlacement& aot_placement,
    const runtime::GuestStackSwitchPlan& stack_plan,
    const hle::DosVirtualFileSystemState& dos_file_system,
    const exe::Dos16mBoundModule* linexe_module,
    const std::vector<exe::LeResidentName>* glide_exports,
    const std::filesystem::path* cd_chd_path,
    const std::filesystem::path* sound_rom_zip_path,
    bool enable_piu_jamma_board,
    bool enable_piu10_isa_board,
    bool enable_cat702,
    std::string_view parent_rom_set_id,
    std::uint32_t piu10_mp3_latency_ms,
    runtime::ExecutionBackend execution_backend,
    std::uint32_t timeout_milliseconds,
    std::uint32_t stall_timeout_milliseconds,
    MinimalExecutionAttempt* attempt)
{
    if (attempt == nullptr || !stack_plan.valid || !aot_placement.placed)
    {
        if (attempt != nullptr)
        {
            *attempt = MinimalExecutionAttempt{};
            attempt->message = !stack_plan.valid
                ? "guest stack switch plan is not valid"
                : "AOT code cache placement is not valid";
        }
        return false;
    }
    return RunExecutionThread(
        placement, stack_plan.entry_eip, stack_plan.initial_esp,
        FindGuestStackSelector(placement, stack_plan),
        true, true, true, true, false, false, &dos_file_system,
        linexe_module, glide_exports, cd_chd_path, sound_rom_zip_path,
        enable_piu_jamma_board,
        enable_piu10_isa_board,
        enable_cat702,
        parent_rom_set_id,
        piu10_mp3_latency_ms,
        &aot_placement,
        execution_backend,
        timeout_milliseconds,
        stall_timeout_milliseconds,
        attempt);
}

}  // namespace repiu::engine
