#include "aot_dbt_direct_edge_dispatch.h"
#include "aot_dbt_glide_gate_dispatch.h"
#include "aot_dbt_hle_dispatch.h"
#include "aot_dbt_return_dispatch.h"

// Issue #61. The emulated model has no dispatch thunks: the thunks are host
// code that generated code calls on the guest stack, and the emulated model
// generates no code and runs the legacy backend. Null tells every caller the
// mechanism is unavailable.

namespace repiu::engine
{

void* GetAotDbtDirectEdgeDispatchThunkAddress()
{
    return nullptr;
}

void* GetAotDbtHleDispatchThunkAddress()
{
    return nullptr;
}

void* GetAotDbtReturnMissThunkAddress()
{
    return nullptr;
}

void* GetGlideGateDirectDispatchThunkAddress()
{
    return nullptr;
}

void InstallGlideGateDirectDispatchResolver()
{
}

}  // namespace repiu::engine
