#include "execution_trampoline_model.h"

// Issue #61. The execution trampoline's half for the emulated execution model:
// the guest runs on the rex86 CPU core, and this process executes none of its
// bytes. See docs/design/20261014-i061-emulated-execution-model.md.
//
// The core is configured as an x86 inside a Win32 user-mode process
// (Features::user_mode, rex86 #69): it stops before CLI, STI, HLT, port I/O
// and INT n, and on any selector or page the host has not granted, exactly
// where the guest faults natively. Each stop becomes the FaultEvent the
// platform layer would have built and goes to DispatchGuestFault, so the HLE
// that serves the direct model serves this one unchanged.

#include "aot_dbt_hle_dispatch.h"
#include "aot_runtime_dispatch.h"
#include "execution_internal.h"
#include "host_turn.h"

#include "repiu/engine/exception_rescue_win32.h"
#include "repiu/engine/pic_timer_in_service.h"
#include "repiu/platform/guest_stack_switch.h"
#include "repiu/platform/host_cpu_entry_state.h"
#include "repiu/platform/host_time.h"
#include "repiu/platform/thunk_calling_convention.h"
#include "repiu/platform/virtual_memory.h"
#include "repiu/runtime/aot_segment_patch.h"
#include "repiu/runtime/aot_translation_plan.h"

#include "rex86/cpu.h"

#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

// The direct model's recovery entry, in the platform layer. HLE exits point
// the guest's EIP at it (RecoverToHost); the emulated CPU must never run it,
// because it rewrites the TEB and the host stack, so it is a gate here.
extern "C" void REPIU_THUNK_RESOLVER_CALL RecoverGuestStackException();

namespace repiu::engine
{
namespace trampoline_model
{
namespace
{

// Steps per Run between slice boundaries, where owed timer ticks are offered
// and memory changes applied.
constexpr std::uint64_t kSliceSteps = 10000U;

// The Windows exception codes a native fault carries in FaultEvent::host_code.
// The dispatcher and the crash report read them, so the emulated faults carry
// the same numbers on every host.
constexpr std::uint32_t kStatusBreakpoint = 0x80000003U;
constexpr std::uint32_t kStatusSingleStep = 0x80000004U;
constexpr std::uint32_t kStatusAccessViolation = 0xC0000005U;
constexpr std::uint32_t kStatusIllegalInstruction = 0xC000001DU;
constexpr std::uint32_t kStatusArrayBoundsExceeded = 0xC000008CU;
constexpr std::uint32_t kStatusFloatInvalidOperation = 0xC0000090U;
constexpr std::uint32_t kStatusIntegerDivideByZero = 0xC0000094U;
constexpr std::uint32_t kStatusIntegerOverflow = 0xC0000095U;
constexpr std::uint32_t kStatusPrivilegedInstruction = 0xC0000096U;

constexpr std::uint32_t kEflagsTrap = 0x00000100U;
// What a thread context may set when it is resumed: Windows keeps IF on and
// IOPL at zero for a user-mode thread whatever the context says.
constexpr std::uint32_t kEflagsResumeWritable = 0x00247FD5U & ~0x00003200U;
constexpr std::uint32_t kEflagsUserFixed = 0x00000202U;
// DR6.BS: the single-step trap's status bit.
constexpr std::uint32_t kDr6SingleStep = 0x00004000U;
// EFLAGS.RF: the next instruction's breakpoint is not raised.
constexpr std::uint32_t kEflagsResume = 0x00010000U;

constexpr std::uint32_t kPageMask = ~(rex86::kGuestPageSize - 1U);

// ---------------------------------------------------------------------------
// The host's view of the guest's selectors.

class EmulatedEnvironment final : public rex86::Environment
{
public:
    explicit EmulatedEnvironment(const repiu::platform::HostCpuEntryState& host)
        : host_(host)
    {
    }

    // Only the selectors a native guest thread holds are valid: the host's
    // flat code and data selectors and the TEB's FS. Any other load -- a
    // guest selector, or the null selector -- is the #GP the HLE emulates
    // natively, which keeps its shadow of the guest's virtual selectors the
    // one record of them (design decision 9).
    bool LoadDescriptor(const std::uint16_t selector,
                        rex86::Descriptor* const descriptor) override
    {
        *descriptor = rex86::Descriptor{};
        if ((selector & 0xFFFCU) == 0U)
        {
            return false;
        }
        const std::uint16_t index = static_cast<std::uint16_t>(selector & 0xFFFCU);
        if (index == (host_.cs & 0xFFFCU))
        {
            descriptor->executable = true;
            descriptor->writable = false;
            return true;
        }
        if (index == (host_.fs & 0xFFFCU) && host_.fs_limit != 0U)
        {
            descriptor->base = host_.fs_base;
            descriptor->limit = host_.fs_limit;
            return true;
        }
        return IsHostSelector(selector);
    }

    // Design decision 9: the instructions the dynamic backend's AOT plan
    // makes HLE boundaries -- segment override prefixes, segment register
    // operands, LAR/LSL, INT, port I/O, privileged instructions -- stop the
    // emulated CPU before they run, so the HLE serves them as it does there.
    bool IsHostBoundary(const std::uint8_t* const bytes, const std::uint32_t length) override
    {
        return repiu::runtime::IsAotHleBoundaryBytes(bytes, length);
    }

    // Whether `selector` is one the host thread itself holds.
    [[nodiscard]] bool IsHostSelector(const std::uint16_t selector) const
    {
        const std::uint16_t index = static_cast<std::uint16_t>(selector & 0xFFFCU);
        return index != 0U &&
            (index == (host_.cs & 0xFFFCU) || index == (host_.ds & 0xFFFCU) ||
             index == (host_.es & 0xFFFCU) || index == (host_.ss & 0xFFFCU) ||
             index == (host_.gs & 0xFFFCU) || index == (host_.fs & 0xFFFCU));
    }

    // Under user_mode the core never asks: port I/O and INT n fault first.
    bool PortRead(std::uint16_t, std::uint8_t, std::uint32_t*) override
    {
        return false;
    }

    bool PortWrite(std::uint16_t, std::uint8_t, std::uint32_t) override
    {
        return false;
    }

    bool InterruptTarget(std::uint8_t, std::uint16_t*, std::uint32_t*) override
    {
        return false;
    }

    std::uint64_t ReadTimeStampCounter() override
    {
        return repiu::platform::ReadCycleCounter();
    }

    // The host's answers, as a native guest gets them, less what the core
    // does not implement: SSE2 and everything leaf 1 reports in ECX.
    void Cpuid(const std::uint32_t leaf, const std::uint32_t subleaf,
               std::uint32_t registers[4]) override
    {
        repiu::platform::ReadHostCpuid(leaf, subleaf, registers);
        if (leaf == 1U)
        {
            registers[2] = 0U;
            registers[3] &= ~(1U << 26U);
        }
    }

    [[nodiscard]] const repiu::platform::HostCpuEntryState& host() const
    {
        return host_;
    }

private:
    repiu::platform::HostCpuEntryState host_;
};

// ---------------------------------------------------------------------------
// Memory changes reported by the platform layer, from any thread, applied by
// the guest thread between stops.

struct PendingMemoryChange
{
    repiu::platform::MemoryChange change;
    std::uintptr_t address;
    std::size_t bytes;
};

struct PendingMemoryChanges
{
    std::mutex mutex;
    std::vector<PendingMemoryChange> changes;
    std::atomic<bool> any{false};
};

void QueueMemoryChange(const repiu::platform::MemoryChange change,
                       const std::uintptr_t address, const std::size_t bytes,
                       void* const user_data)
{
    auto* const pending = static_cast<PendingMemoryChanges*>(user_data);
    const std::lock_guard<std::mutex> guard(pending->mutex);
    pending->changes.push_back(PendingMemoryChange{change, address, bytes});
    pending->any.store(true, std::memory_order_release);
}

// The part of [address, address + bytes) the guest can name.
bool ClipToGuest(const std::uintptr_t address, const std::size_t bytes,
                 std::uint32_t* const start, std::uint32_t* const size)
{
    constexpr std::uintptr_t kGuestEnd = 0xFFFFF000U;
    if (address >= kGuestEnd || bytes == 0U)
    {
        return false;
    }
    const std::uintptr_t first = address & kPageMask;
    std::uintptr_t end = address + bytes;
    if (end < address || end > kGuestEnd)
    {
        end = kGuestEnd;
    }
    *start = static_cast<std::uint32_t>(first);
    *size = static_cast<std::uint32_t>(end - first);
    return true;
}

// ---------------------------------------------------------------------------
// The page attribute table mirrors the host's protection, page run by page
// run, the first time the guest touches it.

rex86::PageFlag FlagsFromRegion(const repiu::platform::MemoryRegion& region)
{
    if (!region.valid || !region.committed || !region.readable)
    {
        return rex86::PageFlag::kNone;
    }
    rex86::PageFlag flags = rex86::PageFlag::kMapped | rex86::PageFlag::kRead;
    if (region.writable)
    {
        flags = flags | rex86::PageFlag::kWrite;
    }
    if (region.executable)
    {
        flags = flags | rex86::PageFlag::kExecute;
    }
    return flags;
}

// Mirrors the run of host pages around `address` if the table has nothing
// for it yet. True when something was mirrored, so the access is worth
// running again.
bool MirrorHostPages(rex86::GuestMemory* const memory, const std::uint32_t address)
{
    const std::uint32_t page = address & kPageMask;
    if (page == 0U || memory->pages().Get(page) != rex86::PageFlag::kNone)
    {
        return false;
    }
    const repiu::platform::MemoryRegion region = repiu::platform::QueryMemory(
        reinterpret_cast<const void*>(static_cast<std::uintptr_t>(page)));
    const rex86::PageFlag flags = FlagsFromRegion(region);
    if (flags == rex86::PageFlag::kNone)
    {
        return false;
    }
    std::uint32_t start = 0U;
    std::uint32_t size = 0U;
    if (!ClipToGuest(reinterpret_cast<std::uintptr_t>(region.base), region.size,
                     &start, &size))
    {
        return false;
    }
    return memory->pages().Set(start, size, flags);
}

// ---------------------------------------------------------------------------
// CpuState <-> GuestCpuContext.

template <typename Field>
Field AsField(const std::uint32_t value)
{
    return static_cast<Field>(value);
}

// The segment registers are reported as the host's, which is what a WOW64
// CONTEXT shows; the guest's virtual selectors are the HLE's shadow.
void StateToContext(const rex86::CpuState& state,
                    const repiu::platform::HostCpuEntryState& host,
                    repiu::platform::GuestCpuContext* const registers)
{
    std::memset(registers, 0, sizeof(*registers));
    using Field = decltype(registers->Eip);
    registers->ContextFlags = repiu::platform::kGuestCpuContextIntegerControlSegments;
    registers->Eax = AsField<Field>(state.Get(rex86::Gpr::kEax));
    registers->Ecx = AsField<Field>(state.Get(rex86::Gpr::kEcx));
    registers->Edx = AsField<Field>(state.Get(rex86::Gpr::kEdx));
    registers->Ebx = AsField<Field>(state.Get(rex86::Gpr::kEbx));
    registers->Esp = AsField<Field>(state.Get(rex86::Gpr::kEsp));
    registers->Ebp = AsField<Field>(state.Get(rex86::Gpr::kEbp));
    registers->Esi = AsField<Field>(state.Get(rex86::Gpr::kEsi));
    registers->Edi = AsField<Field>(state.Get(rex86::Gpr::kEdi));
    registers->Eip = AsField<Field>(state.eip);
    registers->EFlags = AsField<Field>(state.eflags);
    registers->SegCs = AsField<Field>(host.cs);
    registers->SegDs = AsField<Field>(host.ds);
    registers->SegEs = AsField<Field>(host.es);
    registers->SegFs = AsField<Field>(host.fs);
    registers->SegGs = AsField<Field>(host.gs);
    registers->SegSs = AsField<Field>(host.ss);

    // FNSAVE layout: the area holds ST(0) to ST(7), and rex86 keeps physical
    // registers, so the area is the physical file rotated by TOP.
    const rex86::X87State& x87 = state.x87;
    auto& save = registers->FloatSave;
    save.ControlWord = x87.control_word;
    save.StatusWord = x87.status_word;
    save.TagWord = x87.tag_word;
    save.ErrorOffset = x87.last_instruction_pointer;
    save.ErrorSelector = static_cast<std::uint32_t>(x87.last_instruction_selector) |
        (static_cast<std::uint32_t>(x87.last_opcode & 0x07FFU) << 16U);
    save.DataOffset = x87.last_operand_pointer;
    save.DataSelector = x87.last_operand_selector;
    const unsigned top = (x87.status_word >> 11U) & 7U;
    for (unsigned i = 0; i < 8U; ++i)
    {
        std::memcpy(save.RegisterArea + i * 10U, x87.registers[(top + i) & 7U].data(), 10U);
    }
    registers->ContextFlags |= 0x00010008U;  // CONTEXT_FLOATING_POINT
}

// Loads a selector into a segment register as the CPU would, or leaves the
// register unusable when the environment declines it.
void LoadSegment(EmulatedEnvironment* const environment, rex86::CpuState* const state,
                 const rex86::Segment segment, const std::uint16_t selector)
{
    rex86::SegmentRegister& target = state->Seg(segment);
    rex86::Descriptor descriptor;
    target.selector = selector;
    if (!environment->LoadDescriptor(selector, &descriptor))
    {
        descriptor.present = false;
        descriptor.limit = 0U;
    }
    target.base = descriptor.base;
    target.limit = descriptor.limit;
    target.present = descriptor.present;
    target.executable = descriptor.executable;
    target.writable = descriptor.writable;
    target.default_32bit = descriptor.default_32bit;
}

// The segment registers are not taken from the context. A resumed WOW64
// thread keeps the selectors it holds whatever the context says (the direct
// model's traces show 0023/002B/002B/002B/0053/002B after every HLE write of
// a guest selector into SegDs or SegCs, the timer injection's included), and
// the HLE keeps the guest's virtual selectors in its own shadow. Only the
// guest's own segment loads change them, and those run on the emulated CPU.
void ContextToState(const repiu::platform::GuestCpuContext& registers,
                    rex86::CpuState* const state)
{
    state->Set(rex86::Gpr::kEax, static_cast<std::uint32_t>(registers.Eax));
    state->Set(rex86::Gpr::kEcx, static_cast<std::uint32_t>(registers.Ecx));
    state->Set(rex86::Gpr::kEdx, static_cast<std::uint32_t>(registers.Edx));
    state->Set(rex86::Gpr::kEbx, static_cast<std::uint32_t>(registers.Ebx));
    state->Set(rex86::Gpr::kEsp, static_cast<std::uint32_t>(registers.Esp));
    state->Set(rex86::Gpr::kEbp, static_cast<std::uint32_t>(registers.Ebp));
    state->Set(rex86::Gpr::kEsi, static_cast<std::uint32_t>(registers.Esi));
    state->Set(rex86::Gpr::kEdi, static_cast<std::uint32_t>(registers.Edi));
    state->eip = static_cast<std::uint32_t>(registers.Eip);
    state->eflags = (static_cast<std::uint32_t>(registers.EFlags) & kEflagsResumeWritable) |
        kEflagsUserFixed;

    rex86::X87State& x87 = state->x87;
    const auto& save = registers.FloatSave;
    x87.control_word = static_cast<std::uint16_t>(save.ControlWord);
    x87.status_word = static_cast<std::uint16_t>(save.StatusWord);
    x87.tag_word = static_cast<std::uint16_t>(save.TagWord);
    x87.last_instruction_pointer = static_cast<std::uint32_t>(save.ErrorOffset);
    x87.last_instruction_selector = static_cast<std::uint16_t>(save.ErrorSelector);
    x87.last_opcode = static_cast<std::uint16_t>((save.ErrorSelector >> 16U) & 0x07FFU);
    x87.last_operand_pointer = static_cast<std::uint32_t>(save.DataOffset);
    x87.last_operand_selector = static_cast<std::uint16_t>(save.DataSelector);
    const unsigned top = (x87.status_word >> 11U) & 7U;
    for (unsigned i = 0; i < 8U; ++i)
    {
        std::memcpy(x87.registers[(top + i) & 7U].data(), save.RegisterArea + i * 10U, 10U);
    }
}

// ---------------------------------------------------------------------------
// The running CPU, for a stop request from another thread.

struct RunningCpu
{
    std::mutex mutex;
    rex86::Cpu* cpu = nullptr;
    bool stop_requested = false;
};

RunningCpu g_running;

class RunningCpuScope
{
public:
    explicit RunningCpuScope(rex86::Cpu* const cpu)
    {
        const std::lock_guard<std::mutex> guard(g_running.mutex);
        g_running.cpu = cpu;
        g_running.stop_requested = false;
    }

    ~RunningCpuScope()
    {
        const std::lock_guard<std::mutex> guard(g_running.mutex);
        g_running.cpu = nullptr;
    }

    RunningCpuScope(const RunningCpuScope&) = delete;
    RunningCpuScope& operator=(const RunningCpuScope&) = delete;
};

bool StopRequested()
{
    const std::lock_guard<std::mutex> guard(g_running.mutex);
    return g_running.stop_requested;
}

// ---------------------------------------------------------------------------
// One guest run.

enum class RunEnd : std::uint8_t
{
    kContinue,
    kReturned,
    kRecovered,
    kUnhandled,
    kStopped,
};

class EmulatedRun
{
public:
    EmulatedRun(StackSwitchCallState* const state, ThreadContext* const context,
                const repiu::platform::HostCpuEntryState& host)
        : state_(state),
          context_(context),
          environment_(host),
          memory_(nullptr, 0xFFFFFFFFU),
          cpu_(&memory_, &environment_, Features())
    {
    }

    void Run()
    {
        if (!Enter())
        {
            return;
        }
        const RunningCpuScope running(&cpu_);
        repiu::platform::SetMemoryChangeObserver(&QueueMemoryChange, &pending_);
        RunEnd end = RunEnd::kContinue;
        while (end == RunEnd::kContinue)
        {
            ApplyMemoryChanges();
            if (StopRequested())
            {
                end = RunEnd::kStopped;
                break;
            }
            const bool trap = (cpu_.state().eflags & kEflagsTrap) != 0U;
            const rex86::Event event = Resume(trap);
            steps_ += event.steps;
            if (event.steps != 0U)
            {
                // The bound on mirroring is per faulting access, not per run.
                mirror_retries_ = 0U;
            }
            end = Handle(event, trap);
            // Issue #64: on one thread the host's turn comes between stops,
            // when it is due (host_turn.h).
            OneThreadHostLoop* const host = context_->one_thread_host;
            if (end == RunEnd::kContinue && host != nullptr && host->TurnDue(*context_) &&
                !host->TakeTurn(context_))
            {
                end = RunEnd::kStopped;
            }
        }
        repiu::platform::SetMemoryChangeObserver(nullptr, nullptr);
        std::fprintf(stderr,
                     "[repiu-emulated] guest run ended: %s steps=%llu "
                     "dispatches=%llu boundaries=%llu/%llu/%llu mirrored=%llu\n",
                     EndName(end), static_cast<unsigned long long>(steps_),
                     static_cast<unsigned long long>(dispatches_),
                     static_cast<unsigned long long>(boundary_dispatches_),
                     static_cast<unsigned long long>(boundary_passes_),
                     static_cast<unsigned long long>(boundary_folds_),
                     static_cast<unsigned long long>(mirrored_));
    }

private:
    static rex86::Features Features()
    {
        rex86::Features features;
        features.user_mode = true;
        features.segments_16bit = true;
        features.host_boundaries = true;
        return features;
    }

    static const char* EndName(const RunEnd end)
    {
        switch (end)
        {
            case RunEnd::kReturned: return "returned";
            case RunEnd::kRecovered: return "recovered to host";
            case RunEnd::kUnhandled: return "unhandled fault";
            case RunEnd::kStopped: return "stop requested";
            default: return "continue";
        }
    }

    // The state CallGuestEntryWithStack enters the guest with: the host's
    // selectors and FPU control, ESP at the guest stack with the call-state
    // pointer and a return address pushed, EAX the entry, ECX the call state,
    // EDX the initial ESP and EBX the host SS (what the stub leaves there).
    bool Enter()
    {
        const repiu::platform::HostCpuEntryState host =
            repiu::platform::ReadHostCpuEntryState();
        if (!host.valid)
        {
            std::fprintf(stderr, "[repiu-emulated] host entry state unavailable\n");
            return false;
        }
        state_->host_fs = host.fs;
        state_->host_ds = host.ds;
        state_->host_es = host.es;
        state_->host_gs = host.gs;
        state_->host_ss = host.ss;
        // What the stack switch records for the HLE: the selectors the
        // thread held at entry tell a guest-loaded selector from one only
        // the HLE's shadow knows (ReadGuestSegmentSelector).
        g_recovery_host_fs = host.fs;
        g_recovery_host_ds = host.ds;
        g_recovery_host_es = host.es;
        g_recovery_host_gs = host.gs;

        // The guest's return address: a page reserved and never committed, so
        // nothing else can live there. Reached, it is a gate.
        const repiu::platform::MemoryReservation sentinel = repiu::platform::ReserveMemory(
            nullptr, repiu::platform::SystemPageSize(), false,
            repiu::platform::MemoryProtection::kNoAccess);
        const std::uintptr_t sentinel_address =
            reinterpret_cast<std::uintptr_t>(sentinel.base);
        const std::uintptr_t recovery_address =
            reinterpret_cast<std::uintptr_t>(&RecoverGuestStackException);
        const std::uintptr_t state_address = reinterpret_cast<std::uintptr_t>(state_);
        if (!sentinel.valid || sentinel_address > 0xFFFFFFFFU ||
            recovery_address > 0xFFFFFFFFU || state_address > 0xFFFFFFFFU)
        {
            std::fprintf(stderr, "[repiu-emulated] entry addresses do not fit the guest\n");
            return false;
        }
        return_gate_ = static_cast<std::uint32_t>(sentinel_address);
        recovery_gate_ = static_cast<std::uint32_t>(recovery_address);
        cpu_.RegisterGate(return_gate_);
        cpu_.RegisterGate(recovery_gate_);

        rex86::CpuState& cpu_state = cpu_.state();
        const std::uint16_t selectors[6] = {host.es, host.cs, host.ss, host.ds, host.fs, host.gs};
        for (unsigned i = 0; i < 6U; ++i)
        {
            LoadSegment(&environment_, &cpu_state, static_cast<rex86::Segment>(i), selectors[i]);
        }
        cpu_state.x87.control_word = host.x87_control_word;
        cpu_state.sse.mxcsr = host.mxcsr;

        std::uint32_t esp = state_->initial_esp;
        esp -= 4U;
        std::memcpy(reinterpret_cast<void*>(static_cast<std::uintptr_t>(esp)), &state_address, 4U);
        esp -= 4U;
        std::memcpy(reinterpret_cast<void*>(static_cast<std::uintptr_t>(esp)), &return_gate_, 4U);
        cpu_state.Set(rex86::Gpr::kEsp, esp);
        cpu_state.Set(rex86::Gpr::kEax, state_->entry_address);
        cpu_state.Set(rex86::Gpr::kEcx, static_cast<std::uint32_t>(state_address));
        cpu_state.Set(rex86::Gpr::kEdx, state_->initial_esp);
        cpu_state.Set(rex86::Gpr::kEbx, host.ss);
        cpu_state.eip = state_->entry_address;
        cpu_state.eflags = kEflagsUserFixed |
            (state_->enable_single_step_trace != 0U ? kEflagsTrap : 0U);
        return true;
    }

    void ApplyMemoryChanges()
    {
        if (!pending_.any.load(std::memory_order_acquire))
        {
            return;
        }
        std::vector<PendingMemoryChange> changes;
        {
            const std::lock_guard<std::mutex> guard(pending_.mutex);
            changes.swap(pending_.changes);
            pending_.any.store(false, std::memory_order_release);
        }
        for (const PendingMemoryChange& change : changes)
        {
            std::uint32_t start = 0U;
            std::uint32_t size = 0U;
            if (!ClipToGuest(change.address, change.bytes, &start, &size))
            {
                continue;
            }
            if (change.change == repiu::platform::MemoryChange::kCode)
            {
                cpu_.InvalidateCode(start, size);
            }
            else
            {
                memory_.pages().Set(start, size, rex86::PageFlag::kNone);
            }
        }
    }

    RunEnd Handle(const rex86::Event& event, const bool trap)
    {
        switch (event.reason)
        {
            case rex86::StopReason::kBudgetExhausted:
                if (trap)
                {
                    // The instruction retired under TF: the single-step trap,
                    // reported after it with DR6.BS set.
                    return Dispatch(repiu::platform::FaultKind::kSingleStep, kStatusSingleStep,
                                    repiu::platform::GuestFaultInfo{}, cpu_.state().eip, kDr6SingleStep);
                }
                OfferTimerTick();
                return RunEnd::kContinue;
            case rex86::StopReason::kGate:
                if (event.gate_address == return_gate_)
                {
                    // The stub's `pop ecx` after the call, then where it
                    // records the guest's ESP.
                    rex86::CpuState& cpu_state = cpu_.state();
                    const std::uint32_t esp = cpu_state.Get(rex86::Gpr::kEsp) + 4U;
                    cpu_state.Set(rex86::Gpr::kEsp, esp);
                    state_->guest_return_esp = esp;
                    state_->result_code = 0U;
                    return RunEnd::kReturned;
                }
                if (event.gate_address == recovery_gate_)
                {
                    return RunEnd::kRecovered;
                }
                {
                    // An execute breakpoint in DR0-DR3: Windows reports it as
                    // a single step at the breakpoint, before the instruction,
                    // with the slot's DR6 bit set.
                    const std::uint32_t hits = BreakpointHits(event.gate_address);
                    if (hits != 0U)
                    {
                        return Dispatch(repiu::platform::FaultKind::kSingleStep, kStatusSingleStep,
                                        repiu::platform::GuestFaultInfo{}, event.gate_address,
                                        hits);
                    }
                }
                std::fprintf(stderr, "[repiu-emulated] unexpected gate 0x%08X\n",
                             event.gate_address);
                return RunEnd::kUnhandled;
            case rex86::StopReason::kFault:
                return HandleFault(event);
            case rex86::StopReason::kSoftwareInterrupt:
                return HandleTrapInterrupt(event);
            case rex86::StopReason::kHostBoundary:
                return HandleBoundary();
            case rex86::StopReason::kStopRequested:
                return RunEnd::kStopped;
            default:
                std::fprintf(stderr, "[repiu-emulated] unexpected stop %u at 0x%08X\n",
                             static_cast<unsigned>(event.reason), cpu_.state().eip);
                return RunEnd::kUnhandled;
        }
    }

    RunEnd HandleFault(const rex86::Event& event)
    {
        const std::uint32_t eip = cpu_.state().eip;
        repiu::platform::GuestFaultInfo access;
        switch (event.fault_kind)
        {
            case rex86::FaultKind::kAccessViolation:
            {
                // A page the table has not mirrored yet: mirror it, and the
                // next page for an access that may straddle, then run the
                // instruction again. Bounded, so a page the host keeps
                // refusing still reaches the HLE.
                if (mirror_retries_ < 4U)
                {
                    const bool mirrored = MirrorHostPages(&memory_, event.fault_address) |
                        MirrorHostPages(&memory_, (event.fault_address & kPageMask) +
                                                      rex86::kGuestPageSize);
                    if (mirrored)
                    {
                        ++mirrored_;
                        ++mirror_retries_;
                        return RunEnd::kContinue;
                    }
                }
                mirror_retries_ = 0U;
                access.valid = true;
                access.write_access = event.fault_on_write;
                access.execute_access = event.fault_on_fetch;
                access.fault_address = event.fault_address;
                return Dispatch(repiu::platform::FaultKind::kAccessViolation,
                                kStatusAccessViolation, access, eip, 0U);
            }
            case rex86::FaultKind::kGeneralProtection:
            case rex86::FaultKind::kStackFault:
                // Windows reports a #GP or #SS as an access violation at
                // address 0xFFFFFFFF.
                access.valid = true;
                access.fault_address = 0xFFFFFFFFU;
                return Dispatch(repiu::platform::FaultKind::kAccessViolation,
                                kStatusAccessViolation, access, eip, 0U);
            case rex86::FaultKind::kPrivilegedInstruction:
                return Dispatch(repiu::platform::FaultKind::kPrivilegedInstruction,
                                kStatusPrivilegedInstruction, access, eip, 0U);
            case rex86::FaultKind::kIllegalInstruction:
                return Dispatch(repiu::platform::FaultKind::kIllegalInstruction,
                                kStatusIllegalInstruction, access, eip, 0U);
            case rex86::FaultKind::kDivide:
                return Dispatch(repiu::platform::FaultKind::kIntegerDivideByZero,
                                kStatusIntegerDivideByZero, access, eip, 0U);
            case rex86::FaultKind::kBound:
                return Dispatch(repiu::platform::FaultKind::kOther,
                                kStatusArrayBoundsExceeded, access, eip, 0U);
            default:
                return Dispatch(repiu::platform::FaultKind::kOther,
                                kStatusFloatInvalidOperation, access, eip, 0U);
        }
    }

    // An HLE boundary (design decision 9), served as the dynamic backend's
    // dispatch thunk serves it (aot_dbt_hle_dispatch.cpp): directly through
    // DispatchGuestHleInstruction, with the HLE's virtual selectors in the
    // context, unless the boundary must be mediated by the fault handler or
    // the HLE leaves it alone. Then the instruction runs as it is, and if it
    // faults, as most mediated ones do under user_mode, the fault reaches
    // DispatchGuestFault as it would natively.
    RunEnd HandleBoundary()
    {
        rex86::CpuState& cpu_state = cpu_.state();
        const std::uint32_t eip = cpu_state.eip;
        // A segment-override memory access whose segment resolves to a
        // native fold runs as the dynamic backend's slot runs it: with the
        // resolved base added (aot_segment_patch.cpp). FS stays the HLE's,
        // as the slot refuses it.
        std::uint8_t override_segment = 0xFFU;
        const auto* const code =
            reinterpret_cast<const std::uint8_t*>(static_cast<std::uintptr_t>(eip));
        if (repiu::runtime::AotSegmentOverrideRegisterOfBytes(code, 15U, &override_segment) &&
            override_segment != 4U && override_segment < 6U)
        {
            runtime::AotSegmentTable table;
            BuildAotSegmentTable(context_, &table);
            const runtime::AotSegmentResolution& resolution = table.segments[override_segment];
            if (resolution.policy == runtime::AotSegmentAccessPolicy::kNativeFolded)
            {
                ++boundary_folds_;
                return RunFolded(static_cast<rex86::Segment>(override_segment), resolution.base);
            }
        }
        if (!AotDbtHleRequiresVehMediation(context_, eip, cpu_state.Get(rex86::Gpr::kEax)))
        {
            ++boundary_dispatches_;
            repiu::platform::GuestCpuContext registers;
            StateToContext(cpu_state, environment_.host(), &registers);
            WriteDebugRegisters(&registers);
            using Field = decltype(registers.SegDs);
            registers.SegEs = static_cast<Field>(context_->guest_es);
            registers.SegSs = static_cast<Field>(context_->guest_ss);
            registers.SegDs = static_cast<Field>(context_->guest_ds);
            registers.SegFs = static_cast<Field>(context_->guest_fs);
            registers.SegGs = static_cast<Field>(context_->guest_gs);
            const std::uint32_t esp = static_cast<std::uint32_t>(registers.Esp);
            if (DispatchGuestHleInstruction(&registers, context_) &&
                static_cast<std::uint32_t>(registers.Eip) != eip &&
                static_cast<std::uint32_t>(registers.Esp) == esp)
            {
                registers.EFlags &= ~kEflagsTrap;
                ContextToState(registers, &cpu_state);
                ReadDebugRegisters(registers);
                return cpu_state.eip == recovery_gate_ ? RunEnd::kRecovered : RunEnd::kContinue;
            }
        }
        ++boundary_passes_;
        cpu_.SkipHostBoundaryOnce();
        return RunEnd::kContinue;
    }

    // Runs the boundary at CS:EIP once with `segment` based at `base`, then
    // gives the segment back its host descriptor. A fault it takes is
    // handled like any other.
    RunEnd RunFolded(const rex86::Segment segment, const std::uint32_t base)
    {
        rex86::CpuState& cpu_state = cpu_.state();
        const bool trap = (cpu_state.eflags & kEflagsTrap) != 0U;
        rex86::SegmentRegister& target = cpu_state.Seg(segment);
        const rex86::SegmentRegister saved = target;
        target.base = base;
        target.limit = 0xFFFFFFFFU;
        target.present = true;
        cpu_.SkipHostBoundaryOnce();
        const rex86::Event event = cpu_.Step();
        cpu_state.Seg(segment) = saved;
        steps_ += event.steps;
        return Handle(event, trap);
    }

    // INT3, INT1 and INTO still retire and stop under user_mode, as traps
    // through DPL 3 gates do. Windows reports INT3 at the breakpoint itself.
    RunEnd HandleTrapInterrupt(const rex86::Event& event)
    {
        rex86::CpuState& cpu_state = cpu_.state();
        switch (event.vector)
        {
            case 3U:
                cpu_state.eip -= 1U;
                return Dispatch(repiu::platform::FaultKind::kBreakpoint, kStatusBreakpoint,
                                repiu::platform::GuestFaultInfo{}, cpu_state.eip, 0U);
            case 1U:
                return Dispatch(repiu::platform::FaultKind::kSingleStep, kStatusSingleStep,
                                repiu::platform::GuestFaultInfo{}, cpu_state.eip, kDr6SingleStep);
            case 4U:
                return Dispatch(repiu::platform::FaultKind::kOther, kStatusIntegerOverflow,
                                repiu::platform::GuestFaultInfo{}, cpu_state.eip, 0U);
            default:
                std::fprintf(stderr, "[repiu-emulated] unexpected interrupt 0x%02X\n",
                             event.vector);
                return RunEnd::kUnhandled;
        }
    }

    // `trap_dr6` is the DR6 a debug exception reports (zero for any other
    // fault): BS for a single step, a slot's bit for a breakpoint.
    RunEnd Dispatch(const repiu::platform::FaultKind kind, const std::uint32_t host_code,
                    const repiu::platform::GuestFaultInfo& access,
                    const std::uint32_t instruction_address, const std::uint32_t trap_dr6)
    {
        ++dispatches_;
        repiu::platform::GuestCpuContext registers;
        StateToContext(cpu_.state(), environment_.host(), &registers);
        WriteDebugRegisters(&registers);
        if (trap_dr6 != 0U)
        {
            registers.Dr6 = trap_dr6;
        }
        if ((trap_dr6 & kDr6SingleStep) != 0U)
        {
            // Windows reports a single-step trap with TF already cleared in
            // the context, as the trap's delivery clears it.
            registers.EFlags &= ~kEflagsTrap;
        }
        repiu::platform::FaultEvent fault;
        fault.kind = kind;
        fault.host_code = host_code;
        fault.access = access;
        fault.instruction_address = instruction_address;
        fault.registers = &registers;
        if (DispatchGuestFault(fault) != repiu::platform::FaultDisposition::kResume)
        {
            CaptureUnhandledGuestFault(fault, context_);
            return RunEnd::kUnhandled;
        }
        ContextToState(registers, &cpu_.state());
        ReadDebugRegisters(registers);
        resume_flag_ = (static_cast<std::uint32_t>(registers.EFlags) & kEflagsResume) != 0U;
        return cpu_.state().eip == recovery_gate_ ? RunEnd::kRecovered : RunEnd::kContinue;
    }

    // Runs on, one step under TF. A context resumed with RF set at an armed
    // breakpoint runs that instruction past the breakpoint, as RF does.
    rex86::Event Resume(const bool trap)
    {
        const std::uint32_t eip = cpu_.state().eip;
        if (std::exchange(resume_flag_, false) && BreakpointHits(eip) != 0U)
        {
            cpu_.UnregisterGate(eip);
            const rex86::Event event = cpu_.Step();
            cpu_.RegisterGate(eip);
            return event.reason == rex86::StopReason::kBudgetExhausted && !trap
                ? cpu_.Run(kSliceSteps)
                : event;
        }
        return trap ? cpu_.Step() : cpu_.Run(kSliceSteps);
    }

    // The slots of DR0-DR3 armed for execution at `address`, as DR6 bits.
    [[nodiscard]] std::uint32_t BreakpointHits(const std::uint32_t address) const
    {
        std::uint32_t hits = 0U;
        for (unsigned i = 0; i < 4U; ++i)
        {
            if (armed_[i] && debug_[i] == address)
            {
                hits |= 1U << i;
            }
        }
        return hits;
    }

    void WriteDebugRegisters(repiu::platform::GuestCpuContext* const registers) const
    {
        registers->Dr0 = debug_[0];
        registers->Dr1 = debug_[1];
        registers->Dr2 = debug_[2];
        registers->Dr3 = debug_[3];
        registers->Dr6 = dr6_;
        registers->Dr7 = dr7_;
    }

    // Takes the context's debug registers and arms their execute
    // breakpoints as gates. Data breakpoints have no emulated form and are
    // reported once.
    void ReadDebugRegisters(const repiu::platform::GuestCpuContext& registers)
    {
        const std::uint32_t values[4] = {
            static_cast<std::uint32_t>(registers.Dr0), static_cast<std::uint32_t>(registers.Dr1),
            static_cast<std::uint32_t>(registers.Dr2), static_cast<std::uint32_t>(registers.Dr3)};
        dr6_ = static_cast<std::uint32_t>(registers.Dr6);
        dr7_ = static_cast<std::uint32_t>(registers.Dr7);
        for (unsigned i = 0; i < 4U; ++i)
        {
            const bool enabled = ((dr7_ >> (i * 2U)) & 3U) != 0U;
            const std::uint32_t kind = (dr7_ >> (16U + i * 4U)) & 3U;
            if (enabled && kind != 0U && !data_breakpoint_reported_)
            {
                data_breakpoint_reported_ = true;
                std::fprintf(stderr,
                             "[repiu-emulated] DR%u is a data breakpoint, which the "
                             "emulated CPU does not raise\n",
                             i);
            }
            const bool armed = enabled && kind == 0U;
            if (armed_[i] && (!armed || debug_[i] != values[i]))
            {
                armed_[i] = false;
                if (BreakpointHits(debug_[i]) == 0U && debug_[i] != return_gate_ &&
                    debug_[i] != recovery_gate_)
                {
                    cpu_.UnregisterGate(debug_[i]);
                }
            }
            debug_[i] = values[i];
            if (armed && !armed_[i])
            {
                armed_[i] = true;
                cpu_.RegisterGate(values[i]);
            }
        }
    }

    // A slice boundary is where a native guest that runs without faulting
    // would be when a tick came due; the dispatcher offers it there too.
    void OfferTimerTick()
    {
        repiu::platform::GuestCpuContext registers;
        StateToContext(cpu_.state(), environment_.host(), &registers);
        const std::uint32_t eip = static_cast<std::uint32_t>(registers.Eip);
        const std::uint32_t esp = static_cast<std::uint32_t>(registers.Esp);
        WriteDebugRegisters(&registers);
        InjectPendingInterrupts(&registers, context_);
        if (static_cast<std::uint32_t>(registers.Eip) != eip ||
            static_cast<std::uint32_t>(registers.Esp) != esp)
        {
            ContextToState(registers, &cpu_.state());
            ReadDebugRegisters(registers);
        }
    }

    StackSwitchCallState* state_;
    ThreadContext* context_;
    EmulatedEnvironment environment_;
    rex86::GuestMemory memory_;
    rex86::Cpu cpu_;
    PendingMemoryChanges pending_;
    std::uint32_t return_gate_ = 0U;
    std::uint32_t recovery_gate_ = 0U;
    std::uint32_t mirror_retries_ = 0U;
    std::uint64_t steps_ = 0U;
    std::uint64_t dispatches_ = 0U;
    std::uint64_t mirrored_ = 0U;
    // Boundaries the HLE served directly, and those run as they are.
    std::uint64_t boundary_dispatches_ = 0U;
    std::uint64_t boundary_passes_ = 0U;
    std::uint64_t boundary_folds_ = 0U;
    // DR0-DR3, DR6 and DR7 as the HLE last wrote them, and which slots are
    // armed as execute breakpoints (gates).
    std::array<std::uint32_t, 4> debug_{};
    std::array<bool, 4> armed_{};
    std::uint32_t dr6_ = 0U;
    std::uint32_t dr7_ = 0U;
    bool resume_flag_ = false;
    bool data_breakpoint_reported_ = false;
};

}  // namespace

void EnterGuestWithStack(StackSwitchCallState* state, ThreadContext* context)
{
    // The direct model's.
    (void)state;
    (void)context;
}

void EnterGuestDirect(ThreadContext* context)
{
    // The direct model's.
    (void)context;
}

void EnterGuestCache(ThreadContext* context)
{
    // The cache model's.
    (void)context;
}

void EnterGuestEmulated(StackSwitchCallState* state, ThreadContext* context)
{
    if (state == nullptr || context == nullptr)
    {
        return;
    }
    // Large (the page attribute table alone is a megabyte), so not on the
    // guest thread's stack.
    const auto run = std::make_unique<EmulatedRun>(
        state, context, repiu::platform::ReadHostCpuEntryState());
    run->Run();
}

bool RequestGuestStop(ThreadContext* context)
{
    (void)context;
    const std::lock_guard<std::mutex> guard(g_running.mutex);
    g_running.stop_requested = true;
    if (g_running.cpu != nullptr)
    {
        g_running.cpu->RequestStop();
    }
    return true;
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
