#ifndef REPIU_PLATFORM_HOST_GPU_DRIVER_H_
#define REPIU_PLATFORM_HOST_GPU_DRIVER_H_

namespace repiu::platform
{

// Task 752. Picks Mesa's D3D12 driver under WSL, before the window opens; true
// when it did. WSL hands the GPU to Linux only through that driver, and Mesa
// picks llvmpipe unless told otherwise. Windows draws on its own GPU driver
// and has nothing to pick.
bool SelectWslD3d12Driver();

// Issue #69. The OpenGL context the Glide renderer asks for: desktop 3.3 core
// on Windows and Linux, ES 3.0 (WebGL2) on the web. The renderer uses only the
// subset the two share; the context, not the platform, then picks the GLSL
// dialect.
struct GlContextRequest
{
    bool es = false;
    int major = 3;
    int minor = 3;
};
GlContextRequest RequestedGlContext();

}  // namespace repiu::platform

#endif  // REPIU_PLATFORM_HOST_GPU_DRIVER_H_
