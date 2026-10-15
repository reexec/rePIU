#include "repiu/platform/host_gpu_driver.h"

#include "host_gpu_driver_arch.h"

// Task 759. The WSL driver choice, which is the host's to make.

#include <cstddef>
#include <cstdlib>
#include <unistd.h>

namespace repiu::platform {

// Task 752. WSL hands the GPU to Linux only through Mesa's D3D12 driver, and
// Mesa picks llvmpipe unless told otherwise, so a machine with a GPU drew in
// software. Chosen when the paravirtual GPU device and the driver are both
// there and the user has not chosen for themselves; `REPIU_WSL_D3D12=0` keeps
// Mesa's own choice.
bool SelectWslD3d12Driver() {
  const char *const choice = std::getenv("REPIU_WSL_D3D12");
  if (choice != nullptr && choice[0] == '0') {
    return false;
  }
  if (std::getenv("GALLIUM_DRIVER") != nullptr ||
      std::getenv("MESA_LOADER_DRIVER_OVERRIDE") != nullptr ||
      std::getenv("LIBGL_ALWAYS_SOFTWARE") != nullptr) {
    return false;
  }
  if (access("/dev/dxg", F_OK) != 0) {
    return false;
  }
  // Task 760: the driver of this process's own architecture.
  std::size_t driver_count = 0U;
  const char *const *const drivers =
      host_gpu_driver_arch::WslD3d12DriverPaths(&driver_count);
  for (std::size_t index = 0; index < driver_count; ++index) {
    if (access(drivers[index], R_OK) == 0) {
      return setenv("GALLIUM_DRIVER", "d3d12", 0) == 0;
    }
  }
  return false;
}

GlContextRequest RequestedGlContext() { return GlContextRequest{false, 3, 3}; }

} // namespace repiu::platform
