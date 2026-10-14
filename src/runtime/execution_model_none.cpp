#include "repiu/runtime/execution_model.h"

// Task 759. No execution model: a host that runs no guest (the web build,
// Task 513 Stage 1). Every question is answered with "no".

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
    return false;
}

}  // namespace repiu::runtime::execution_model
