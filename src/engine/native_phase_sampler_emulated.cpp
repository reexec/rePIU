#include "native_phase_sampler_model.h"

// Issue #61. Native-phase capture on the emulated model: none. The guest
// thread's host instruction pointer is always inside the emulated CPU, so
// stopping it from outside says nothing about where the guest is.

namespace repiu::engine
{
namespace native_phase_sampler_model
{

bool CaptureEnabled()
{
    return false;
}

bool InterruptsWithHostContext()
{
    return false;
}

bool ScansHostStack()
{
    return false;
}

void RecordNativeInstructionPointer(NativePhaseSample* sample, void* host_context)
{
    (void)sample;
    (void)host_context;
}

}  // namespace native_phase_sampler_model

void WriteLinuxX64NativeSampleTraceLine(
    const NativePhaseSample& sample,
    const std::uint32_t elapsed_milliseconds)
{
    (void)sample;
    (void)elapsed_milliseconds;
}

}  // namespace repiu::engine
