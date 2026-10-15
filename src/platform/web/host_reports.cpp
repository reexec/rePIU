// Issue #67. What the engine asks of the host beyond running the guest, on the
// web: the crash reporter, the fault report's host details, the WSL driver
// choice, module and symbol names, and the cross-process telemetry. A wasm
// module has none of them -- a trap ends in the JavaScript host, there are no
// modules or symbols to name and no other process to share a section with --
// so each answers "none", as the Linux host does for most of them. They
// report, not run the guest, so a "none" here loses a diagnostic and nothing
// else. See docs/design/20261015-i067-emscripten-headless.md.

#if defined(__EMSCRIPTEN__)

#include "repiu/platform/host_crash_report.h"
#include "repiu/platform/host_fault_report.h"
#include "repiu/platform/host_gpu_driver.h"
#include "repiu/platform/host_symbols.h"
#include "repiu/platform/host_telemetry.h"

#include <cstdio>

namespace repiu::platform
{

// A trap in wasm is reported by the JavaScript host that catches it.
void InstallHostCrashReporter()
{
}

void ReportHostLanguageException(const std::uint32_t host_code,
                                 const void* const instruction_address)
{
    std::fprintf(stderr, "[repiu-host-crash] host exception 0x%08X at %p\n",
                 static_cast<unsigned>(host_code), instruction_address);
}

bool QueryHostMemoryNumbers(const void* /*address*/,
                            std::uint32_t* /*state*/,
                            std::uint32_t* /*protect*/)
{
    return false;
}

bool FillsFaultReportDetail()
{
    return false;
}

bool ReadMemoryForFaultReport(const void* /*source*/,
                              void* /*destination*/,
                              std::size_t /*size*/,
                              std::size_t* const copied)
{
    if (copied != nullptr)
    {
        *copied = 0U;
    }
    return false;
}

bool SelectWslD3d12Driver()
{
    return false;
}

bool ResolveHostModule(std::uint32_t /*address*/,
                       std::string* /*module_name*/,
                       std::uint32_t* /*module_offset*/)
{
    return false;
}

HostSymbolSession::HostSymbolSession() = default;

HostSymbolSession::~HostSymbolSession() = default;

bool HostSymbolSession::Resolve(std::uint32_t /*address*/, std::string* /*name*/) const
{
    return false;
}

void ReadHostImageRange(std::uint32_t* const base, std::uint32_t* const size)
{
    *base = 0U;
    *size = 0U;
}

bool ReadHostThreadTimes(const HostThread& /*thread*/,
                         std::uint64_t* /*kernel_100ns*/,
                         std::uint64_t* /*user_100ns*/)
{
    return false;
}

HostSharedMapping OpenHostSharedMappingFromEnvironment(const char* /*variable_name*/)
{
    return HostSharedMapping{};
}

void CloseHostSharedMapping(HostSharedMapping* /*mapping*/)
{
}

bool ReadSuspendedHostThreadContext(void* /*thread_handle*/, GuestCpuContext* /*context*/)
{
    return false;
}

}  // namespace repiu::platform

#endif  // __EMSCRIPTEN__
