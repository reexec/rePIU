#include "repiu/runtime/execution_model.h"

// Task 759. The direct model: the guest's bytes run in this process as they
// are. Win32 and Linux i386.
#if !defined(_M_IX86) && !defined(__i386__)
#error "the direct execution model needs a 32-bit x86 host"
#endif

namespace repiu::runtime::execution_model
{

bool RunsGuestBytesDirectly()
{
    return true;
}

bool RunsGuestOnEmulatedCpu()
{
    return false;
}

bool RunsHostOnOneThread()
{
    return false;
}

bool RunsLongModeCodeCache()
{
    return false;
}

std::uintptr_t LongModeReturnThunkAddress()
{
    return 0U;
}

bool InjectsTicksDuringSwapWait()
{
    return true;
}

}  // namespace repiu::runtime::execution_model
