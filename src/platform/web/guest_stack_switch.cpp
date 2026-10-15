// Issue #67. The web's two recovery entries.
//
// On a native host these are machine-code thunks (guest_stack_switch_win32.cpp,
// linux/x86/guest_stack_switch.S): an HLE exit points the guest's EIP at one,
// and running it unwinds back to the host. On the web the guest runs on the
// rex86 CPU core only (the emulated model), which registers the address as a
// gate and stops there instead of running it, so these are never called.
// They exist so the address can be taken.
//
// In wasm a function's "address" is its index in the function table, a small
// number rather than a location in linear memory. That is still a unique value
// the guest cannot otherwise reach -- page 0 is never mapped -- which is all a
// gate needs.

#if defined(__EMSCRIPTEN__)

#include "repiu/platform/thunk_calling_convention.h"

#include <cstdio>
#include <cstdlib>

namespace
{

[[noreturn]] void ReachedRecoveryThunk(const char* name)
{
    std::fprintf(stderr, "[repiu-web] %s was called; on the web it is only a gate address\n", name);
    std::abort();
}

}  // namespace

extern "C" void REPIU_THUNK_RESOLVER_CALL RecoverGuestStackException()
{
    ReachedRecoveryThunk("RecoverGuestStackException");
}

extern "C" void RecoverHostStackException()
{
    ReachedRecoveryThunk("RecoverHostStackException");
}

#endif  // __EMSCRIPTEN__
