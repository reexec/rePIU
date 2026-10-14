#include "repiu/engine/pic_timer_in_service.h"
#include "repiu/platform/guest_cpu_context.h"
#pragma once

// Declarations for functions that are defined in execution_trampoline.cpp but
// must be visible across the win32 execution translation units extracted from
// it (Phase 1 decomposition). These carry external linkage; their definitions
// were moved out of the anonymous namespace accordingly. As dedicated module
// headers are formalized, declarations here migrate to their owning module.

#include "thread_context.h"

#include "repiu/platform/fault_handler.h"

#include <cstddef>
#include <cstdint>

namespace repiu::engine
{

inline constexpr std::uint32_t kEFlagsInterruptEnable = 0x00000200U;

// Task 410: tag the VEH exit that is about to resume the guest. Called from the
// handlers that consume an exception, including the ones extracted into other
// translation units. The last call before the handler returns wins, so a path
// that first marks its state and is then finished by a later site reports the
// site that actually resumed execution.
void NoteVehExitSite(ThreadContext* context, VehExitSite site);

bool IsAotCacheAddress(const ThreadContext* context, std::uint32_t address);

bool WriteGuestBytes(ThreadContext* context,
                     void* destination,
                     const void* source,
                     std::size_t byte_count);

bool NoteSuccessfulAotGuestWrite(ThreadContext* context,
                                 std::uint32_t destination,
                                 std::uint32_t byte_count);

void RecoverFromHleExit(repiu::platform::GuestCpuContext* win32_context, ThreadContext* thread_context);

void RecordHandledDosInterrupt(ThreadContext* context, std::uint8_t vector, std::uint16_t ax);

void RecordLowMemoryAccess(repiu::platform::GuestCpuContext* win32_context, ThreadContext* context,
                           std::uint8_t opcode, std::uint32_t destination, std::uint32_t value);

std::uint16_t ReadGuestSegmentSelector(const ThreadContext& context,
                                       std::uint8_t segment_register, const repiu::platform::GuestCpuContext* win32_context);

bool ResolveSegmentLinearRange(ThreadContext* context, std::uint16_t selector,
                               std::uint32_t offset, std::uint32_t byte_count,
                               bool writable, std::uint32_t* linear_address);

bool IsGuestInstructionPointer(const ThreadContext* context, std::uint32_t eip);
// Returns the number of expired PIT ticks consumed by a successful injection.
// Zero means no interrupt was injected.
// Task 751.
std::uint64_t GuestCliHoldClockNanoseconds();
// Task 751. Whether an IRQ0 injected now could be entered. On Linux x64 the
// handler runs only from the cache or, under the single-step bridge, as bytes
// that mean the same in long mode; a handler that is neither (a `pusha` first,
// before its translation exists) has to wait for a later attempt. Elsewhere
// the handler runs as it is, so the answer is always yes.
bool CanEnterTimerInterruptHandler(ThreadContext* context);
std::uint32_t InjectPendingInterrupts(
    repiu::platform::GuestCpuContext* win32_context,
    ThreadContext* context,
    TimerInjectionSite site = TimerInjectionSite::kOrdinary);

bool DispatchGuestHleInstruction(repiu::platform::GuestCpuContext* win32_context,
                                 ThreadContext* context);

// Issue #61. Records a fault DispatchGuestFault declined as the end of the
// guest's run, as the direct model's SEH filter does: the emulated model has
// no host exception to unwind and calls this instead.
void CaptureUnhandledGuestFault(const repiu::platform::FaultEvent& fault,
                                ThreadContext* context);

bool CanResumeLinuxX64LegacyTarget(ThreadContext* context,
                                   std::uint32_t guest_target);

void RecordExecutionProbe(repiu::platform::GuestCpuContext* win32_context, ThreadContext* context);
void RecordExecutionTrace(repiu::platform::GuestCpuContext* win32_context, ThreadContext* context);

} // namespace repiu::engine
