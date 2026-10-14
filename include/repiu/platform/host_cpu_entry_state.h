#ifndef REPIU_PLATFORM_HOST_CPU_ENTRY_STATE_H_
#define REPIU_PLATFORM_HOST_CPU_ENTRY_STATE_H_

#include <cstdint>

// Issue #61. What a guest entered on this host thread inherits from it: the
// segment selectors the direct model's stack switch leaves in place, the base
// of the thread environment block FS addresses on Windows, and the floating
// point control state. The emulated execution model starts its CPU from these
// so the guest sees what it would natively.

namespace repiu::platform
{

struct HostCpuEntryState
{
    bool valid = false;
    std::uint16_t cs = 0;
    std::uint16_t ds = 0;
    std::uint16_t es = 0;
    std::uint16_t fs = 0;
    std::uint16_t gs = 0;
    std::uint16_t ss = 0;
    // The linear address FS:0 names, and how far FS reaches. Zero where FS
    // means nothing to the host.
    std::uint32_t fs_base = 0;
    std::uint32_t fs_limit = 0;
    std::uint16_t x87_control_word = 0x037FU;
    std::uint32_t mxcsr = 0x1F80U;
};

// Read on the calling thread, which is the one about to run the guest.
[[nodiscard]] HostCpuEntryState ReadHostCpuEntryState();

// What CPUID answers on this host, as the guest would see it natively: EAX,
// EBX, ECX, EDX. Zeros where the host has no CPUID.
void ReadHostCpuid(std::uint32_t leaf, std::uint32_t subleaf, std::uint32_t registers[4]);

}  // namespace repiu::platform

#endif  // REPIU_PLATFORM_HOST_CPU_ENTRY_STATE_H_
