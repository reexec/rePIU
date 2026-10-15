#include "repiu/platform/host_thread.h"

#include "../host_thread_platform.h"

// Issue #67. The web host thread layer: there are no threads. The web build is
// linked without pthreads, because the single Worker it runs in is served
// without COOP/COEP (design #64), and the emulated model runs everything on one
// thread since #64, so nothing on the run's path starts one. Starting a thread
// therefore fails and says so, once, rather than appearing to work (see this
// directory's README). See docs/design/20261015-i067-emscripten-headless.md,
// decision 4.

#if defined(__EMSCRIPTEN__)

#include "web_unsupported.h"

namespace repiu::platform
{
namespace
{

bool g_reported = false;

}  // namespace

// The one thread there is.
std::uint32_t CurrentThreadId()
{
    return 1U;
}

namespace host_thread_platform
{

bool StartThread(HostThreadEntry /*entry*/,
                 void* /*parameter*/,
                 HostThread* /*thread*/,
                 std::uint32_t* const host_error)
{
    web::ReportUnsupportedOnce(
        &g_reported,
        "[repiu-web] threads are unavailable: the web build runs on one thread "
        "(design #64), and nothing on its path should start one.\n");
    if (host_error != nullptr)
    {
        *host_error = 38U;  // ENOSYS
    }
    return false;
}

HostThreadStatus QueryThread(const HostThread& /*thread*/)
{
    HostThreadStatus status;
    status.running = false;
    return status;
}

bool JoinThread(const HostThread& /*thread*/,
                std::uint32_t /*timeout_milliseconds*/,
                std::uint32_t* /*exit_code*/)
{
    return false;
}

ThreadInterruptFailure InterruptThread(const HostThread& /*thread*/,
                                       ThreadInterruptCallback /*callback*/,
                                       ThreadInterruptContextCallback /*context_callback*/,
                                       void* /*user_data*/,
                                       std::uint32_t /*timeout_milliseconds*/)
{
    return ThreadInterruptFailure::kNotDelivered;
}

void CloseThread(HostThread* const thread)
{
    *thread = HostThread{};
}

void DetachThread(HostThread* const thread)
{
    *thread = HostThread{};
}

}  // namespace host_thread_platform

bool TerminateHostThread(const HostThread& /*thread*/, std::uint32_t /*exit_code*/)
{
    return false;
}

}  // namespace repiu::platform

#endif  // __EMSCRIPTEN__
