#include "hle_boundary_trace.h"

#include "repiu/platform/fault_handler.h"
#include "repiu/platform/guest_cpu_context.h"
#include "repiu/platform/virtual_memory.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace repiu::engine
{
namespace
{

constexpr std::uint64_t kDefaultLineLimit = 1000000U;
// The flags both models must agree on: the arithmetic flags, TF and DF. IF
// and the rest are the host's.
constexpr std::uint32_t kComparedFlags = 0x00000DD5U;

struct TraceFile
{
    std::FILE* file = nullptr;
    std::uint64_t limit = kDefaultLineLimit;
    std::uint64_t lines = 0U;

    TraceFile()
    {
        const char* const path = std::getenv("REPIU_HLE_BOUNDARY_TRACE");
        if (path == nullptr || path[0] == '\0')
        {
            return;
        }
        const char* const limit_setting = std::getenv("REPIU_HLE_BOUNDARY_TRACE_LIMIT");
        if (limit_setting != nullptr && limit_setting[0] != '\0')
        {
            limit = std::strtoull(limit_setting, nullptr, 10);
        }
        file = std::fopen(path, "w");
    }

    ~TraceFile()
    {
        if (file != nullptr)
        {
            std::fclose(file);
        }
    }

    // False once the file is closed to further lines.
    bool Take()
    {
        if (file == nullptr)
        {
            return false;
        }
        if (lines >= limit)
        {
            std::fflush(file);
            return false;
        }
        ++lines;
        // A run that ends in a crash keeps what was written up to its last
        // few thousand lines.
        if ((lines & 0xFFFU) == 0U)
        {
            std::fflush(file);
        }
        return true;
    }
};

TraceFile& Trace()
{
    static TraceFile trace;
    return trace;
}

const char* KindName(const repiu::platform::FaultKind kind)
{
    switch (kind)
    {
        case repiu::platform::FaultKind::kAccessViolation: return "av";
        case repiu::platform::FaultKind::kSingleStep: return "ss";
        case repiu::platform::FaultKind::kBreakpoint: return "bp";
        case repiu::platform::FaultKind::kIllegalInstruction: return "ud";
        case repiu::platform::FaultKind::kIntegerDivideByZero: return "de";
        case repiu::platform::FaultKind::kPrivilegedInstruction: return "priv";
        default: return "other";
    }
}

}  // namespace

bool HleBoundaryTraceEnabled()
{
    return Trace().file != nullptr;
}

void RecordHleBoundaryFault(const repiu::platform::FaultEvent& fault,
                            const char* const previous_exit)
{
    TraceFile& trace = Trace();
    if (fault.registers == nullptr || !trace.Take())
    {
        return;
    }
    const auto& r = *fault.registers;
    const std::uint32_t eip = static_cast<std::uint32_t>(r.Eip);
    std::uint8_t bytes[8] = {};
    const void* const code = reinterpret_cast<const void*>(static_cast<std::uintptr_t>(eip));
    if (repiu::platform::IsRangeReadable(code, sizeof(bytes)))
    {
        std::memcpy(bytes, code, sizeof(bytes));
    }
    std::fprintf(trace.file,
                 "f %llu %s %08X eip=%08X b=%02X%02X%02X%02X%02X%02X%02X%02X "
                 "a=%08X/%c eax=%08X ecx=%08X edx=%08X ebx=%08X esp=%08X ebp=%08X "
                 "esi=%08X edi=%08X fl=%08X sg=%04X/%04X/%04X/%04X/%04X/%04X prev=%s\n",
                 static_cast<unsigned long long>(trace.lines), KindName(fault.kind),
                 static_cast<unsigned>(fault.host_code), eip, bytes[0], bytes[1], bytes[2],
                 bytes[3], bytes[4], bytes[5], bytes[6], bytes[7],
                 fault.access.valid ? static_cast<unsigned>(fault.access.fault_address) : 0U,
                 !fault.access.valid ? '-'
                     : fault.access.write_access ? 'w'
                     : fault.access.execute_access ? 'x'
                                                   : 'r',
                 static_cast<unsigned>(r.Eax), static_cast<unsigned>(r.Ecx),
                 static_cast<unsigned>(r.Edx), static_cast<unsigned>(r.Ebx),
                 static_cast<unsigned>(r.Esp), static_cast<unsigned>(r.Ebp),
                 static_cast<unsigned>(r.Esi), static_cast<unsigned>(r.Edi),
                 static_cast<unsigned>(r.EFlags) & kComparedFlags,
                 static_cast<unsigned>(r.SegCs) & 0xFFFFU, static_cast<unsigned>(r.SegSs) & 0xFFFFU,
                 static_cast<unsigned>(r.SegDs) & 0xFFFFU, static_cast<unsigned>(r.SegEs) & 0xFFFFU,
                 static_cast<unsigned>(r.SegFs) & 0xFFFFU, static_cast<unsigned>(r.SegGs) & 0xFFFFU,
                 previous_exit != nullptr ? previous_exit : "-");
}

void RecordHleBoundaryInjection(const std::uint32_t interrupted_eip,
                                const std::uint32_t handler_eip)
{
    TraceFile& trace = Trace();
    if (!trace.Take())
    {
        return;
    }
    std::fprintf(trace.file, "i %llu eip=%08X handler=%08X\n",
                 static_cast<unsigned long long>(trace.lines), interrupted_eip, handler_eip);
}

}  // namespace repiu::engine
