#include "repiu/runtime/execution_model.h"

// Task 759. The cache model: the translated long-mode code cache runs.
// Linux x64.
#if defined(_WIN32) || !defined(__x86_64__)
#error "the cache execution model needs a Linux x86-64 host"
#endif

#include "repiu/platform/linux/x64/linux_x64_aot_dispatch.h"

namespace repiu::runtime::execution_model
{

bool RunsGuestBytesDirectly()
{
    return false;
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
    return true;
}

std::uintptr_t LongModeReturnThunkAddress()
{
    return repiu::platform::LinuxX64ReturnThunkAddress();
}

bool InjectsTicksDuringSwapWait()
{
    return true;
}

}  // namespace repiu::runtime::execution_model
