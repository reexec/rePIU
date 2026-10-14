#include "repiu/platform/host_cpu_entry_state.h"

// Issue #61. The Linux host's half. The selectors are read with GCC inline
// assembly, which means the same on i386 and x86-64. FS carries nothing a
// guest would look at on Linux (glibc's thread pointer is GS on i386 and the
// FS base register on x86-64, neither reachable through a selector a 32-bit
// guest loads), so its base is left at zero.

#if defined(__i386__) || defined(__x86_64__)
#include <cpuid.h>
#include <xmmintrin.h>
#endif

namespace repiu::platform
{

HostCpuEntryState ReadHostCpuEntryState()
{
    HostCpuEntryState state;
#if defined(__i386__) || defined(__x86_64__)
    std::uint16_t cs = 0;
    std::uint16_t ds = 0;
    std::uint16_t es = 0;
    std::uint16_t fs = 0;
    std::uint16_t gs = 0;
    std::uint16_t ss = 0;
    std::uint16_t control_word = 0;
    __asm__ volatile("movw %%cs, %0" : "=r"(cs));
    __asm__ volatile("movw %%ds, %0" : "=r"(ds));
    __asm__ volatile("movw %%es, %0" : "=r"(es));
    __asm__ volatile("movw %%fs, %0" : "=r"(fs));
    __asm__ volatile("movw %%gs, %0" : "=r"(gs));
    __asm__ volatile("movw %%ss, %0" : "=r"(ss));
    __asm__ volatile("fnstcw %0" : "=m"(control_word));
    state.cs = cs;
    state.ds = ds;
    state.es = es;
    state.fs = fs;
    state.gs = gs;
    state.ss = ss;
    state.x87_control_word = control_word;
    state.mxcsr = _mm_getcsr();
    state.valid = true;
#endif
    return state;
}

void ReadHostCpuid(const std::uint32_t leaf, const std::uint32_t subleaf,
                   std::uint32_t registers[4])
{
    registers[0] = registers[1] = registers[2] = registers[3] = 0U;
#if defined(__i386__) || defined(__x86_64__)
    unsigned int a = 0;
    unsigned int b = 0;
    unsigned int c = 0;
    unsigned int d = 0;
    __cpuid_count(leaf, subleaf, a, b, c, d);
    registers[0] = a;
    registers[1] = b;
    registers[2] = c;
    registers[3] = d;
#else
    (void)leaf;
    (void)subleaf;
#endif
}

}  // namespace repiu::platform
