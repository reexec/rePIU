// Task 759. The execution trampoline's half for the direct execution model
// (Win32 and Linux i386): the two timed entries into the guest. Moved from
// execution_trampoline.cpp, where they sat under
// `#if defined(_M_IX86) || defined(__i386__)`.
#if !defined(_M_IX86) && !defined(__i386__)
#error "the direct execution model needs a 32-bit x86 host"
#endif

#include "execution_trampoline_model.h"

#include "repiu/engine/execution_time_profile.h"
#include "repiu/platform/thunk_calling_convention.h"

#include <cstdint>

// The stack switch is the platform layer's:
// src/platform/win32/guest_stack_switch_win32.cpp and
// src/platform/linux/x86/guest_stack_switch.S.
extern "C" std::uint32_t REPIU_THUNK_RESOLVER_CALL CallGuestEntryWithStack(
    repiu::engine::StackSwitchCallState* state);

namespace repiu::engine
{
namespace
{

// Task 323 denominator: the whole guest execution window on this thread. The
// scope lives here rather than in GuestEntryThreadProc because that function
// uses __try on Windows, and MSVC rejects objects requiring unwinding in the
// same function (C2712).
void CallGuestEntryWithStackTimed(StackSwitchCallState* state,
                                  ThreadContext* context)
{
    const ExecutionTimeScope guest_run_time_scope(
        context != nullptr ? context->execution_time_profile.get() : nullptr,
        ExecutionTimeBucket::kGuestRunTotal);
    CallGuestEntryWithStack(state);
}

// Same denominator for the non-stack-switching entry path. Both branches must
// be instrumented or the guest-run total silently stays zero.
void CallGuestEntryDirectTimed(ThreadContext* context)
{
    const ExecutionTimeScope guest_run_time_scope(
        context != nullptr ? context->execution_time_profile.get() : nullptr,
        ExecutionTimeBucket::kGuestRunTotal);
    using EntryFunction = void (*)();
    EntryFunction entry = reinterpret_cast<EntryFunction>(
        static_cast<std::uintptr_t>(context->entry_address));
    entry();
}

}  // namespace

namespace trampoline_model
{

void EnterGuestWithStack(StackSwitchCallState* state, ThreadContext* context)
{
    CallGuestEntryWithStackTimed(state, context);
}

void EnterGuestDirect(ThreadContext* context)
{
    CallGuestEntryDirectTimed(context);
}

void EnterGuestCache(ThreadContext* context)
{
    // The cache model's; this host enters the guest's own bytes.
    (void)context;
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
    return 0U;
}

void ReadShutdownRecoveryPosition(const std::uint32_t eip,
                                  void* const host_context,
                                  ShutdownRecoveryPosition* const position)
{
    (void)host_context;
    position->host_address = static_cast<std::uintptr_t>(eip);
    position->host_address_known = true;
}

bool RedirectToCacheExit(repiu::platform::GuestCpuContext* const registers,
                         void* const host_context)
{
    (void)registers;
    (void)host_context;
    return false;
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
    // A diagnostic of the cache model.
    (void)context;
    (void)fault_eip;
    (void)fault_kind;
    (void)entry_eip;
    (void)exit_eip;
    (void)entry_esp;
    (void)exit_esp;
    (void)exit_eflags;
}

}  // namespace trampoline_model
}  // namespace repiu::engine
