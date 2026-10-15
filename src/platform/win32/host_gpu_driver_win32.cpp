#include "repiu/platform/host_gpu_driver.h"

// Task 759. Windows draws on its own GPU driver; there is no WSL driver to pick.

namespace repiu::platform
{

bool SelectWslD3d12Driver()
{
    return false;
}

GlContextRequest RequestedGlContext()
{
    return GlContextRequest{false, 3, 3};
}

}  // namespace repiu::platform
