#include "repiu/runtime/execution_backend.h"

#include "repiu/runtime/execution_model.h"

namespace repiu::runtime
{

bool ParseExecutionBackend(std::string_view value,
                           ExecutionBackend* backend)
{
    if (backend == nullptr)
    {
        return false;
    }
    if (value == "legacy")
    {
        *backend = ExecutionBackend::kLegacy;
    }
    else if (value == "dynamic")
    {
        *backend = ExecutionBackend::kDynamic;
    }
    else
    {
        return false;
    }
    return true;
}

bool ResolveExecutionBackend(const char* value, ExecutionBackend* backend)
{
    if (backend == nullptr)
    {
        return false;
    }
    *backend = kDefaultExecutionBackend;
    if (value == nullptr || *value == '\0')
    {
        return true;
    }
    return ParseExecutionBackend(value, backend);
}

std::string_view ExecutionBackendName(ExecutionBackend backend)
{
    switch (backend)
    {
        case ExecutionBackend::kLegacy:
            return "legacy";
        case ExecutionBackend::kDynamic:
            return "dynamic";
    }
    return "legacy";
}

std::string_view ExecutionBackendDisplayName(ExecutionBackend backend)
{
    return execution_model::RunsGuestOnEmulatedCpu() ? "emulated (rex86)"
                                                      : ExecutionBackendName(backend);
}

bool ExecutionBackendUsesDynamicTranslation(ExecutionBackend backend)
{
    return backend == ExecutionBackend::kDynamic;
}

}  // namespace repiu::runtime
