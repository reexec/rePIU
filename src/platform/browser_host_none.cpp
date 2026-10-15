// Issue #72. Win32 and Linux run in a window of their own: no browser canvas,
// no browser event loop to yield to.

#include "repiu/platform/browser_host.h"

namespace repiu::platform
{

bool BrowserCanvasAvailable()
{
    return false;
}

bool CreateBrowserGlContext(int* /*width*/, int* /*height*/)
{
    return false;
}

bool BrowserCanvasSize(int* /*width*/, int* /*height*/)
{
    return false;
}

void DestroyBrowserGlContext()
{
}

BrowserGlFunction BrowserGlProcAddress(const char* /*name*/)
{
    return nullptr;
}

bool YieldToHostEventLoop()
{
    return false;
}

}  // namespace repiu::platform
