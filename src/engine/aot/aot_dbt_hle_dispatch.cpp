#include "aot_dbt_hle_dispatch.h"

#include "aot_runtime_dispatch.h"
#include "aot_residency_sample.h"
#include "execution_internal.h"
#include "guest_memory_access.h"
#include "piu10_mp3_frame_batch.h"

#include <Zydis.h>

#include <cstddef>
#include <cstdio>
#include <cstring>
#include "repiu/platform/guest_cpu_context.h"
#include "repiu/platform/thunk_calling_convention.h"

namespace repiu::engine
{
namespace
{

constexpr std::size_t kSavedEspIndex = 3U;
constexpr std::size_t kSavedEflagsIndex = 8U;
constexpr std::size_t kGuestSourceIndex = 9U;
constexpr std::size_t kDispatchAddressIndex = 10U;
constexpr std::uint32_t kGuestMetadataBytes = 8U;
constexpr std::uint32_t kFallbackFromDispatchBytes = 15U;

bool FindDispatchSite(
    const ThreadContext* context,
    std::uint32_t dispatch_address,
    runtime::AotDbtHleDispatchSite* result)
{
    if (context == nullptr || context->aot_placement == nullptr ||
        result == nullptr ||
        dispatch_address < context->aot_placement->base_address)
    {
        return false;
    }
    const std::uint32_t offset =
        dispatch_address - context->aot_placement->base_address;
    for (const runtime::AotDbtHleDispatchSite& site :
         context->aot_placement->dbt_hle_dispatch_sites)
    {
        if (site.dispatch_cache_offset == offset)
        {
            *result = site;
            return true;
        }
    }
    return false;
}

bool RequiresVehMediatedHle(ThreadContext* context, std::uint32_t guest_eip,
                            std::uint32_t eax)
{
    const auto* code = reinterpret_cast<const std::uint8_t*>(
        static_cast<std::uintptr_t>(guest_eip));
    if (context == nullptr || !IsGuestRangeReadable(context, code, 15U))
    {
        return true;
    }
    ZydisDecoder decoder;
    if (!ZYAN_SUCCESS(ZydisDecoderInit(
            &decoder, ZYDIS_MACHINE_MODE_LEGACY_32, ZYDIS_STACK_WIDTH_32)))
    {
        return true;
    }
    ZydisDecodedInstruction instruction{};
    ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT] = {};
    if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(
            &decoder, code, 15U, &instruction, operands)))
    {
        return true;
    }
    switch (instruction.mnemonic)
    {
        case ZYDIS_MNEMONIC_INT:
        {
            // Task i018. INT 21h services that only read and write general
            // registers and flags run through the host dispatch: AH=2Ch
            // (get time -- pumpitea's calibrate/delay loops, ~100k calls per
            // 30 s of loading) and AH=42h (lseek -- the second stall's
            // ~45k-call burst). Both leave ESP, the segment registers and
            // guest memory untouched and advance EIP by two, which the
            // dispatcher resolves as the block's own continuation; the ESP
            // state-mismatch guard below the call still applies.
            //
            // AH=3Fh (read) was in this list and broke pumpitc and seven
            // other titles whose loaders it served: a read lands in guest
            // memory, and resuming through the dispatcher's direct cache
            // jump skips the reentry funnel's retirement and quarantine
            // checks that the VEH path applies after guest memory changes.
            // Every memory-writing service and every other vector keeps its
            // VEH mediation.
            const std::uint8_t ah =
                static_cast<std::uint8_t>((eax >> 8) & 0xFFU);
            const bool int21 = instruction.operand_count > 0 &&
                operands[0].type == ZYDIS_OPERAND_TYPE_IMMEDIATE &&
                operands[0].imm.value.u == 0x21U;
            if (int21 && (ah == 0x2CU || ah == 0x42U))
            {
                return false;
            }
            return true;
        }
        case ZYDIS_MNEMONIC_INT1:
        case ZYDIS_MNEMONIC_INT3:
        case ZYDIS_MNEMONIC_INTO:
        case ZYDIS_MNEMONIC_IRET:
        case ZYDIS_MNEMONIC_IRETD:
        case ZYDIS_MNEMONIC_IRETQ:
            return true;
        default:
            break;
    }
    // Task 345: a far transfer loads CS, which the operand scan below misses
    // because Zydis does not report CS as a written register operand for
    // `call far` and `jmp far`. Executing one natively took the guest through
    // the INT 8 chain pointer at 0x03042EBE and faulted on a null target,
    // reproducibly, in every SUPERBLOCK run.
    if (instruction.meta.branch_type == ZYDIS_BRANCH_TYPE_FAR)
    {
        return true;
    }
    for (std::uint8_t index = 0; index < instruction.operand_count; ++index)
    {
        if ((operands[index].actions & ZYDIS_OPERAND_ACTION_MASK_WRITE) == 0 ||
            operands[index].type != ZYDIS_OPERAND_TYPE_REGISTER)
        {
            continue;
        }
        const ZydisRegister reg = operands[index].reg.value;
        if (ZydisRegisterGetClass(reg) == ZYDIS_REGCLASS_SEGMENT ||
            reg == ZYDIS_REGISTER_ESP || reg == ZYDIS_REGISTER_SP)
        {
            return true;
        }
    }
    return false;
}

bool TryPiu10Mp3ByteFastPath(
    ThreadContext* context, std::uint32_t* frame,
    const runtime::AotDbtHleDispatchSite& site)
{
    const std::uint32_t guest_source = frame[kGuestSourceIndex];
    const auto* instruction = reinterpret_cast<const std::uint8_t*>(
        static_cast<std::uintptr_t>(guest_source));
    if (context == nullptr || !context->piu10_isa_board_enabled ||
        !context->piu10_isa_board.available() ||
        context->piu10_isa_board.destination() != 0x008U ||
        !context->piu10_mp3_audio.available() ||
        !IsGuestRangeReadable(context, instruction, 1U) ||
        instruction[0] != 0xEEU ||
        (frame[5] & 0xFFFFU) != 0x02DAU)
    {
        return false;
    }

    const std::uint8_t mp3_byte =
        static_cast<std::uint8_t>(frame[7] & 0xFFU);
    const bool mp3_byte_accepted =
        context->piu10_mp3_audio.WriteByte(mp3_byte);
    if (mp3_byte_accepted)
    {
        const std::uint64_t previous =
            context->piu10_mp3_fast_path_write_count.fetch_add(
                1U, std::memory_order_relaxed);
        if (previous == 0U)
        {
            std::fprintf(stderr,
                         "[repiu-piu10-mp3] AOT byte fast path active\n");
        }
    }
    if (mp3_byte_accepted || context->piu10_mp3_frame_batch_audit_enabled)
    {
        TransferPiu10Mp3FrameTail(
            context, guest_source,
            frame[kSavedEspIndex] + 4U + kGuestMetadataBytes,
            mp3_byte, &frame[6]);
    }

    const std::uint32_t next_guest = guest_source + 1U;
    context->aot_dbt_hle_dispatch_last_source.store(
        guest_source, std::memory_order_relaxed);
    context->aot_dbt_hle_dispatch_last_next.store(
        next_guest, std::memory_order_relaxed);
    context->aot_dbt_hle_dispatch_last_bytes.store(
        0x000000EEU, std::memory_order_relaxed);

    const std::uint32_t cache_base = context->aot_placement->base_address;
    frame[kGuestSourceIndex] = cache_base + site.success_cache_offset;
    context->aot_legacy_fallback = false;

    std::uint32_t cache_target = 0U;
    if (ResolveAotTransferTarget(context, next_guest, &cache_target))
    {
        frame[kSavedEflagsIndex] &= ~0x00000100U;
        frame[kDispatchAddressIndex] = cache_target;
        context->aot_reentry_pending = false;
        context->enable_single_step_trace = false;
        context->aot_dbt_hle_dispatch_success_count.fetch_add(
            1U, std::memory_order_relaxed);
        AccumulateAotResidency(context, next_guest);
        BumpAotReentryCount(context);
    }
    else
    {
        frame[kSavedEflagsIndex] |= 0x00000100U;
        frame[kDispatchAddressIndex] = next_guest;
        context->aot_reentry_pending = true;
        context->enable_single_step_trace = true;
        RecordAotDbtHleFallback(
            context, AotDbtHleFallbackReason::kTargetMiss);
    }
    return true;
}

extern "C" void REPIU_THUNK_RESOLVER_CALL ResolveAotDbtHleFrame(
    ThreadContext* context, std::uint32_t* frame)
{
    if (frame == nullptr)
    {
        return;
    }
    if (context != nullptr)
    {
        context->aot_dbt_hle_dispatch_entry_count.fetch_add(
            1U, std::memory_order_relaxed);
    }

    const std::uint32_t guest_source = frame[kGuestSourceIndex];
    const std::uint32_t dispatch_address = frame[kDispatchAddressIndex];
    runtime::AotDbtHleDispatchSite site;
    if (!FindDispatchSite(context, dispatch_address, &site) ||
        site.guest_source != guest_source)
    {
        frame[kGuestSourceIndex] =
            dispatch_address + kFallbackFromDispatchBytes;
        RecordAotDbtHleFallback(
            context, AotDbtHleFallbackReason::kInvalidSite);
        return;
    }

    if (TryPiu10Mp3ByteFastPath(context, frame, site))
    {
        return;
    }

    const std::uint32_t cache_base = context->aot_placement->base_address;
    frame[kGuestSourceIndex] = cache_base + site.fallback_cache_offset;
    if (RequiresVehMediatedHle(context, guest_source, frame[7]))
    {
        RecordAotDbtHleFallback(
            context, AotDbtHleFallbackReason::kVehRequired);
        return;
    }

    repiu::platform::GuestCpuContext guest_context{};
    guest_context.ContextFlags =
        repiu::platform::kGuestCpuContextIntegerControlSegments;
    guest_context.Edi = frame[0];
    guest_context.Esi = frame[1];
    guest_context.Ebp = frame[2];
    guest_context.Esp =
        frame[kSavedEspIndex] + 4U + kGuestMetadataBytes;
    guest_context.Ebx = frame[4];
    guest_context.Edx = frame[5];
    guest_context.Ecx = frame[6];
    guest_context.Eax = frame[7];
    guest_context.EFlags = frame[kSavedEflagsIndex];
    guest_context.Eip = guest_source;
    guest_context.SegEs = context->guest_es;
    guest_context.SegSs = context->guest_ss;
    guest_context.SegDs = context->guest_ds;
    guest_context.SegFs = context->guest_fs;
    guest_context.SegGs = context->guest_gs;
    const std::uint32_t original_esp =
        static_cast<std::uint32_t>(guest_context.Esp);

    if (!DispatchGuestHleInstruction(&guest_context, context) ||
        static_cast<std::uint32_t>(guest_context.Eip) == guest_source)
    {
        RecordAotDbtHleFallback(
            context, AotDbtHleFallbackReason::kUnhandled);
        return;
    }
    if (static_cast<std::uint32_t>(guest_context.Esp) != original_esp)
    {
        context->aot_terminal_failure.store(true, std::memory_order_release);
        RecordAotDbtHleFallback(
            context, AotDbtHleFallbackReason::kStateMismatch);
        return;
    }
    std::uint32_t source_bytes = 0U;
    std::memcpy(
        &source_bytes,
        reinterpret_cast<const void*>(
            static_cast<std::uintptr_t>(guest_source)),
        sizeof(source_bytes));
    context->aot_dbt_hle_dispatch_last_source.store(
        guest_source, std::memory_order_relaxed);
    context->aot_dbt_hle_dispatch_last_next.store(
        static_cast<std::uint32_t>(guest_context.Eip),
        std::memory_order_relaxed);
    context->aot_dbt_hle_dispatch_last_bytes.store(
        source_bytes, std::memory_order_relaxed);

    std::uint32_t cache_target = 0U;
    const bool cache_target_resolved = ResolveAotTransferTarget(
        context, static_cast<std::uint32_t>(guest_context.Eip),
        &cache_target);

    frame[0] = guest_context.Edi;
    frame[1] = guest_context.Esi;
    frame[2] = guest_context.Ebp;
    frame[4] = guest_context.Ebx;
    frame[5] = guest_context.Edx;
    frame[6] = guest_context.Ecx;
    frame[7] = guest_context.Eax;
    frame[kGuestSourceIndex] = cache_base + site.success_cache_offset;
    context->aot_legacy_fallback = false;
    if (cache_target_resolved)
    {
        frame[kSavedEflagsIndex] =
            guest_context.EFlags & ~0x00000100U;
        frame[kDispatchAddressIndex] = cache_target;
        context->aot_reentry_pending = false;
        context->enable_single_step_trace = false;
        context->aot_dbt_hle_dispatch_success_count.fetch_add(
            1U, std::memory_order_relaxed);
        AccumulateAotResidency(
            context, static_cast<std::uint32_t>(guest_context.Eip));
        BumpAotReentryCount(context);
    }
    else
    {
        // The HLE side effect is already committed, so re-executing the source
        // through fallback INT3 would be incorrect. Resume the handled next
        // guest instruction under the established one-step bridge instead.
        frame[kSavedEflagsIndex] =
            guest_context.EFlags | 0x00000100U;
        frame[kDispatchAddressIndex] =
            static_cast<std::uint32_t>(guest_context.Eip);
        context->aot_reentry_pending = true;
        context->enable_single_step_trace = true;
        RecordAotDbtHleFallback(
            context, AotDbtHleFallbackReason::kTargetMiss);
    }
}


}  // namespace

bool AotDbtHleRequiresVehMediation(ThreadContext* const context, const std::uint32_t guest_eip,
                                   const std::uint32_t eax)
{
    return RequiresVehMediatedHle(context, guest_eip, eax);
}

void RecordAotDbtHleFallback(
    ThreadContext* context,
    AotDbtHleFallbackReason reason)
{
    if (context == nullptr)
    {
        return;
    }
    std::uint32_t index = static_cast<std::uint32_t>(reason);
    if (index >= kAotDbtHleFallbackReasonCount)
    {
        index = static_cast<std::uint32_t>(
            AotDbtHleFallbackReason::kUnknown);
    }
    context->aot_dbt_hle_dispatch_fallback_count.fetch_add(
        1U, std::memory_order_relaxed);
    context->aot_dbt_hle_dispatch_fallback_reason_counts[index].fetch_add(
        1U, std::memory_order_relaxed);
}

// Task 759. GetAotDbtHleDispatchThunkAddress is in
// aot_dbt_dispatch_thunks_direct.cpp and aot_dbt_dispatch_thunks_cache.cpp.

}  // namespace repiu::engine
