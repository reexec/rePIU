#include "repiu/platform/host_cpu_entry_state.h"

// Issue #67. The web host's half. A wasm host has no x86 selectors, FPU or
// CPUID, so the entry state is what a guest thread inherits under WOW64 --
// the values the native traces show (0023/002B/002B/002B/0053/002B) -- which
// keeps the boundary traces comparable with native emulated runs. See
// docs/design/20261015-i067-emscripten-headless.md, decision 3.

#if defined(__EMSCRIPTEN__)

#include "repiu/platform/virtual_memory.h"

#include <cstdint>
#include <cstring>

namespace repiu::platform
{
namespace
{

constexpr std::uint16_t kWow64Code = 0x0023U;
constexpr std::uint16_t kWow64Data = 0x002BU;
constexpr std::uint16_t kWow64Teb = 0x0053U;
// What a Win32 thread starts with: 53-bit precision, all exceptions masked.
constexpr std::uint16_t kWin32X87ControlWord = 0x027FU;
constexpr std::uint32_t kTebBytes = 0x1000U;
// NT_TIB.Self: the TEB's own linear address, which code reads as FS:[18h].
constexpr std::uint32_t kTebSelfOffset = 0x18U;

// A zero-filled page from the guest range, standing in for the TEB that FS
// names natively. Made once; nothing in the guest is known to read it.
std::uint32_t StandInTeb()
{
    static std::uint32_t base = [] {
        const MemoryReservation page =
            ReserveMemory(nullptr, kTebBytes, true, MemoryProtection::kReadWrite);
        if (!page.valid)
        {
            return 0U;
        }
        const auto address = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(page.base));
        std::memcpy(static_cast<std::uint8_t*>(page.base) + kTebSelfOffset, &address,
                    sizeof(address));
        return address;
    }();
    return base;
}

}  // namespace

HostCpuEntryState ReadHostCpuEntryState()
{
    HostCpuEntryState state;
    state.cs = kWow64Code;
    state.ds = kWow64Data;
    state.es = kWow64Data;
    state.ss = kWow64Data;
    state.gs = kWow64Data;
    state.fs = kWow64Teb;
    state.fs_base = StandInTeb();
    state.fs_limit = state.fs_base != 0U ? kTebBytes - 1U : 0U;
    state.x87_control_word = kWin32X87ControlWord;
    state.mxcsr = 0x1F80U;
    state.valid = state.fs_base != 0U;
    return state;
}

// A fixed P6-class answer that names only what rex86 implements: the x87,
// TSC, CMPXCHG8B, CMOV, MMX, FXSAVE and SSE. Leaves past 1 answer zero.
void ReadHostCpuid(const std::uint32_t leaf, const std::uint32_t /*subleaf*/,
                   std::uint32_t registers[4])
{
    registers[0] = registers[1] = registers[2] = registers[3] = 0U;
    if (leaf == 0U)
    {
        registers[0] = 1U;
        // "GenuineIntel" in EBX, EDX, ECX order.
        registers[1] = 0x756E6547U;
        registers[3] = 0x49656E69U;
        registers[2] = 0x6C65746EU;
    }
    else if (leaf == 1U)
    {
        // Family 6, model 5 (Pentium II).
        registers[0] = 0x00000652U;
        registers[3] = (1U << 0U) | (1U << 4U) | (1U << 8U) | (1U << 15U) |
            (1U << 23U) | (1U << 24U) | (1U << 25U);
    }
}

}  // namespace repiu::platform

#endif  // __EMSCRIPTEN__
