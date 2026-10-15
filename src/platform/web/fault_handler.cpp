#include "repiu/platform/fault_handler.h"

#if defined(__EMSCRIPTEN__)

#include "web_unsupported.h"

namespace repiu::platform
{
namespace
{

bool g_reported = false;

}  // namespace

// There is no wasm equivalent, and the gap is wider than "no signal API".
//
// The engine's fault handler does two things this host cannot offer. It runs
// with the faulting thread's register context and edits it, and it resumes that
// thread at an address it chose. A wasm trap delivers neither: it carries no
// register file to inspect, and it does not resume -- the module is finished.
//
// So this is not a missing implementation waiting for an Emscripten feature. It
// is the reason Stage 3 exists: on wasm the engine has to reach its HLE
// boundaries by translation rather than by faulting into them.
bool InstallFaultHandler(FaultCallback /*callback*/, void* /*user_data*/)
{
    web::ReportUnsupportedOnce(
        &g_reported,
        "[repiu-web] fault handling is unavailable: a wasm trap carries no "
        "register context and does not resume, so guest faults cannot reach a "
        "handler. Task 513 Stage 3 reaches HLE boundaries by translation.\n");
    return false;
}

bool RemoveFaultHandler()
{
    // False, not true. Nothing was installed, and reporting a successful
    // removal would let a teardown path record that it tidied up something that
    // never existed -- which is how Task 508's core dump stayed hidden behind a
    // sequence that looked correct.
    return false;
}

// Task 717's Linux x64 data watch. Nothing to arm here.
bool ArmLinuxDataWatchFromEnvironment()
{
    return false;
}

}  // namespace repiu::platform

#endif  // __EMSCRIPTEN__
