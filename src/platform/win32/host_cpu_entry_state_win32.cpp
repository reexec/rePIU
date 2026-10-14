#include "repiu/platform/host_cpu_entry_state.h"

// Issue #61. The Win32 host's half: MSVC x86 inline assembly for the
// selectors, the TEB for FS, and the CRT's view of the FPU control state.

#include <windows.h>

#include <intrin.h>
#include <xmmintrin.h>

namespace repiu::platform
{

HostCpuEntryState ReadHostCpuEntryState()
{
    HostCpuEntryState state;
#if defined(_MSC_VER) && defined(_M_IX86)
    std::uint16_t cs_value = 0;
    std::uint16_t ds_value = 0;
    std::uint16_t es_value = 0;
    std::uint16_t fs_value = 0;
    std::uint16_t gs_value = 0;
    std::uint16_t ss_value = 0;
    std::uint16_t control_word = 0;
    __asm
    {
        mov ax, cs
        mov cs_value, ax
        mov ax, ds
        mov ds_value, ax
        mov ax, es
        mov es_value, ax
        mov ax, fs
        mov fs_value, ax
        mov ax, gs
        mov gs_value, ax
        mov ax, ss
        mov ss_value, ax
        fnstcw control_word
    }
    state.cs = cs_value;
    state.ds = ds_value;
    state.es = es_value;
    state.fs = fs_value;
    state.gs = gs_value;
    state.ss = ss_value;
    state.x87_control_word = control_word;
    state.mxcsr = _mm_getcsr();
    // FS addresses the 32-bit TEB, one page on WOW64 and on x86 Windows.
    state.fs_base = static_cast<std::uint32_t>(
        reinterpret_cast<std::uintptr_t>(NtCurrentTeb()));
    state.fs_limit = 0x00000FFFU;
    state.valid = true;
#endif
    return state;
}

void ReadHostCpuid(const std::uint32_t leaf, const std::uint32_t subleaf,
                   std::uint32_t registers[4])
{
    int values[4] = {};
    __cpuidex(values, static_cast<int>(leaf), static_cast<int>(subleaf));
    for (int i = 0; i < 4; ++i)
    {
        registers[i] = static_cast<std::uint32_t>(values[i]);
    }
}

}  // namespace repiu::platform
