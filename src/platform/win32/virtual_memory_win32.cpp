#include "repiu/platform/virtual_memory.h"

#if defined(_WIN32)

#include <windows.h>

namespace repiu::platform
{
namespace
{

DWORD ToProtection(const MemoryProtection protection)
{
    switch (protection)
    {
        case MemoryProtection::kNoAccess:
            return PAGE_NOACCESS;
        case MemoryProtection::kReadOnly:
            return PAGE_READONLY;
        case MemoryProtection::kReadWrite:
            return PAGE_READWRITE;
        case MemoryProtection::kExecuteRead:
            return PAGE_EXECUTE_READ;
        case MemoryProtection::kExecuteReadWrite:
            return PAGE_EXECUTE_READWRITE;
        case MemoryProtection::kOther:
            break;
    }
    // kOther cannot be requested: it exists to describe what a host reported,
    // not to ask for something. Refusing here rather than guessing keeps a
    // round-tripped query from silently changing a page's protection.
    return 0;
}

MemoryProtection FromProtection(const DWORD protection)
{
    // The access bits live in the low byte; PAGE_GUARD, PAGE_NOCACHE, and
    // PAGE_WRITECOMBINE are modifiers above it.
    switch (protection & 0xFFU)
    {
        case PAGE_NOACCESS:
            return MemoryProtection::kNoAccess;
        case PAGE_READONLY:
            return MemoryProtection::kReadOnly;
        case PAGE_READWRITE:
            return MemoryProtection::kReadWrite;
        case PAGE_EXECUTE_READ:
            return MemoryProtection::kExecuteRead;
        case PAGE_EXECUTE_READWRITE:
            return MemoryProtection::kExecuteReadWrite;
        default:
            return MemoryProtection::kOther;
    }
}

// The single classifier that replaces five hand-written ones. A guard page is
// not readable: touching it raises a guard violation first, which is precisely
// what the callers were trying to avoid.
void Classify(const DWORD protection, MemoryRegion* region)
{
    if ((protection & PAGE_GUARD) != 0U)
    {
        return;
    }
    switch (protection & 0xFFU)
    {
        case PAGE_READONLY:
            region->readable = true;
            break;
        case PAGE_READWRITE:
        case PAGE_WRITECOPY:
            region->readable = true;
            region->writable = true;
            break;
        case PAGE_EXECUTE:
            region->executable = true;
            break;
        case PAGE_EXECUTE_READ:
            region->readable = true;
            region->executable = true;
            break;
        case PAGE_EXECUTE_READWRITE:
        case PAGE_EXECUTE_WRITECOPY:
            region->readable = true;
            region->writable = true;
            region->executable = true;
            break;
        default:
            break;
    }
}

}  // namespace

MemoryReservation ReserveMemory(void* preferred_base,
                                const std::size_t bytes,
                                const bool commit,
                                const MemoryProtection protection)
{
    MemoryReservation reservation;
    const DWORD win32_protection = ToProtection(protection);
    if (bytes == 0U || win32_protection == 0U)
    {
        reservation.error = ERROR_INVALID_PARAMETER;
        return reservation;
    }
    const DWORD allocation_type =
        commit ? (MEM_RESERVE | MEM_COMMIT) : MEM_RESERVE;
    void* base = VirtualAlloc(preferred_base,
                              static_cast<SIZE_T>(bytes),
                              allocation_type,
                              win32_protection);
    if (base == nullptr)
    {
        reservation.error = static_cast<std::uint32_t>(GetLastError());
        return reservation;
    }
    reservation.valid = true;
    reservation.base = base;
    reservation.size = bytes;
    return reservation;
}

bool CommitMemory(void* base,
                  const std::size_t bytes,
                  const MemoryProtection protection)
{
    const DWORD win32_protection = ToProtection(protection);
    if (base == nullptr || bytes == 0U || win32_protection == 0U)
    {
        return false;
    }
    // MEM_COMMIT on an address inside an existing reservation commits just
    // those pages and leaves the rest of the reservation alone.
    return VirtualAlloc(base,
                        static_cast<SIZE_T>(bytes),
                        MEM_COMMIT,
                        win32_protection) != nullptr;
}

// Issue #61. How far the reservation starting at `base` reaches, walked
// region by region, because MEM_RELEASE frees all of it whatever a caller
// passed as its size.
std::size_t ReservationExtent(void* const base)
{
    std::size_t extent = 0U;
    auto* cursor = static_cast<std::uint8_t*>(base);
    MEMORY_BASIC_INFORMATION information{};
    while (VirtualQuery(cursor, &information, sizeof(information)) != 0U &&
           information.AllocationBase == base && information.RegionSize != 0U)
    {
        extent += information.RegionSize;
        cursor += information.RegionSize;
    }
    return extent;
}

bool ReleaseMemory(void* base, const std::size_t bytes)
{
    // MEM_RELEASE requires a size of zero and frees the whole reservation, so
    // the byte count is deliberately unused here. It is part of the signature
    // because Linux cannot release without it.
    static_cast<void>(bytes);
    if (base == nullptr)
    {
        return false;
    }
    const std::size_t extent = ReservationExtent(base);
    if (VirtualFree(base, 0, MEM_RELEASE) == 0)
    {
        return false;
    }
    NotifyMemoryChange(MemoryChange::kProtection, base, extent);
    return true;
}

bool ProtectMemory(void* address,
                   const std::size_t bytes,
                   const MemoryProtection protection,
                   MemoryProtection* previous)
{
    const DWORD win32_protection = ToProtection(protection);
    if (address == nullptr || bytes == 0U || win32_protection == 0U)
    {
        return false;
    }
    DWORD win32_previous = 0;
    if (VirtualProtect(address,
                       static_cast<SIZE_T>(bytes),
                       win32_protection,
                       &win32_previous) == 0)
    {
        return false;
    }
    if (previous != nullptr)
    {
        *previous = FromProtection(win32_previous);
    }
    NotifyMemoryChange(MemoryChange::kProtection, address, bytes);
    return true;
}

bool FlushInstructionCacheRange(const void* address, const std::size_t bytes)
{
    if (address == nullptr || bytes == 0U)
    {
        return false;
    }
    NotifyMemoryChange(MemoryChange::kCode, address, bytes);
    return FlushInstructionCache(GetCurrentProcess(), address,
                                 static_cast<SIZE_T>(bytes)) != 0;
}

std::size_t SystemPageSize()
{
    SYSTEM_INFO information{};
    GetSystemInfo(&information);
    return information.dwPageSize != 0U
        ? static_cast<std::size_t>(information.dwPageSize)
        : static_cast<std::size_t>(4096U);
}

MemoryRegion QueryMemory(const void* address)
{
    MemoryRegion region;
    MEMORY_BASIC_INFORMATION information{};
    if (VirtualQuery(address, &information, sizeof(information)) !=
        sizeof(information))
    {
        return region;
    }
    region.valid = true;
    region.base = information.BaseAddress;
    region.size = static_cast<std::size_t>(information.RegionSize);
    region.allocation_base = information.AllocationBase != nullptr
        ? information.AllocationBase
        : information.BaseAddress;
    // Task 503d-19: MEM_FREE is the one state where nothing owns the address.
    region.claimed = information.State != MEM_FREE;
    region.committed = information.State == MEM_COMMIT;
    if (!region.committed)
    {
        // Protect is meaningless for reserved-but-uncommitted memory, and
        // reading it there would report zero, which classifies as kOther.
        return region;
    }
    region.protection = FromProtection(information.Protect);
    Classify(information.Protect, &region);
    return region;
}

bool IsRangeReadable(const void* address, const std::size_t bytes)
{
    if (address == nullptr || bytes == 0U)
    {
        return false;
    }
    const auto start = reinterpret_cast<std::uintptr_t>(address);
    if (start + bytes < start)
    {
        return false;
    }
    const std::uintptr_t end = start + bytes;
    std::uintptr_t cursor = start;
    while (cursor < end)
    {
        const MemoryRegion region =
            QueryMemory(reinterpret_cast<const void*>(cursor));
        if (!region.valid || !region.committed || !region.readable)
        {
            return false;
        }
        const auto region_base =
            reinterpret_cast<std::uintptr_t>(region.base);
        const std::uintptr_t region_end = region_base + region.size;
        if (region_end <= cursor)
        {
            // No forward progress would loop forever. A host that reports such
            // a region is describing something this code cannot reason about.
            return false;
        }
        cursor = region_end;
    }
    return true;
}

}  // namespace repiu::platform

#endif
