#include "repiu/runtime/execution_model.h"

// Issue #61. The emulated model: the guest runs on the rex86 CPU core, and
// this process executes none of its bytes. The web build and ARM hosts need
// it; a 32-bit x86 host can build it to compare against `direct`.

namespace repiu::runtime::execution_model
{

bool RunsGuestBytesDirectly()
{
    return false;
}

bool RunsGuestOnEmulatedCpu()
{
    return true;
}

bool RunsHostOnOneThread()
{
    return true;
}

bool RunsLongModeCodeCache()
{
    return false;
}

std::uintptr_t LongModeReturnThunkAddress()
{
    return 0U;
}

// As on the direct model: the gate leaves into the timer handler through the
// interrupt exit, which the emulated CPU runs like any guest code.
bool InjectsTicksDuringSwapWait()
{
    return true;
}

}  // namespace repiu::runtime::execution_model
