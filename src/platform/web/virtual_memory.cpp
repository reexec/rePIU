#include "repiu/platform/virtual_memory.h"

// Issue #67. The web virtual memory: a reserver over the guest range of wasm
// linear memory, and a record of each page's protection.
//
// The guest range sits at the same addresses as the guest's own (identity
// mapping, design #61): [kGuestRangeBegin, kGuestRangeEnd) lies below
// Emscripten's data, stack and heap, which the link moves up with
// -sGLOBAL_BASE. Nothing else in the process allocates there, so this file
// hands it out.
//
// wasm has no page protection. The record is still kept, and changes are
// still reported to the memory-change observer, because the emulated CPU's
// page attribute table mirrors it: rex86 is what enforces protection for the
// guest. Host code (the HLE) writes regardless, as it does natively.
//
// Single-threaded: the web build runs on one thread (design #64), so there is
// no lock. See docs/design/20261015-i067-emscripten-headless.md, decision 2.

#if defined(__EMSCRIPTEN__)

#include "web_unsupported.h"

#include <emscripten/heap.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <map>

namespace repiu::platform
{
namespace
{

constexpr std::uintptr_t kPageSize = 0x1000U;
// What Windows reserves at, which the callers' preferred addresses assume.
constexpr std::uintptr_t kReservationGranularity = 0x10000U;
// Page 0 to 15 stay unreserved, so a null pointer stays invalid.
constexpr std::uintptr_t kGuestRangeBegin = 0x00010000U;
// -sGLOBAL_BASE in CMakeLists.txt; the guest range ends where it begins.
constexpr std::uintptr_t kGuestRangeEnd = 0x10000000U;

struct Run
{
    std::uintptr_t end = 0;
    MemoryProtection protection = MemoryProtection::kNoAccess;
};

struct State
{
    // Reservation base -> end.
    std::map<std::uintptr_t, std::uintptr_t> reservations;
    // Run start -> run, non-overlapping, inside reservations.
    std::map<std::uintptr_t, Run> protections;
};

State& Shared()
{
    static State state;
    return state;
}

std::uintptr_t AlignDown(const std::uintptr_t value, const std::uintptr_t unit)
{
    return value & ~(unit - 1U);
}

std::uintptr_t AlignUp(const std::uintptr_t value, const std::uintptr_t unit)
{
    return (value + unit - 1U) & ~(unit - 1U);
}

void ApplyAccessFlags(const MemoryProtection protection, MemoryRegion* const region)
{
    region->readable = protection != MemoryProtection::kNoAccess;
    region->writable = protection == MemoryProtection::kReadWrite ||
        protection == MemoryProtection::kExecuteReadWrite;
    region->executable = protection == MemoryProtection::kExecuteRead ||
        protection == MemoryProtection::kExecuteReadWrite;
}

// Removes [start, end) from the protection record, splitting runs at the edges.
void ForgetRange(State* const state, const std::uintptr_t start, const std::uintptr_t end)
{
    auto cursor = state->protections.lower_bound(start);
    if (cursor != state->protections.begin())
    {
        auto previous = std::prev(cursor);
        if (previous->second.end > start)
        {
            const Run tail = previous->second;
            previous->second.end = start;
            if (tail.end > end)
            {
                state->protections[end] = Run{tail.end, tail.protection};
            }
        }
    }
    cursor = state->protections.lower_bound(start);
    while (cursor != state->protections.end() && cursor->first < end)
    {
        if (cursor->second.end > end)
        {
            const Run tail = cursor->second;
            state->protections.erase(cursor);
            state->protections[end] = tail;
            break;
        }
        cursor = state->protections.erase(cursor);
    }
}

void RecordProtection(State* const state, const std::uintptr_t start,
                      const std::uintptr_t end, const MemoryProtection protection)
{
    ForgetRange(state, start, end);
    state->protections[start] = Run{end, protection};
}

// The reservation holding `address`, or end().
std::map<std::uintptr_t, std::uintptr_t>::const_iterator FindReservation(
    const State& state, const std::uintptr_t address)
{
    auto cursor = state.reservations.upper_bound(address);
    if (cursor == state.reservations.begin())
    {
        return state.reservations.end();
    }
    --cursor;
    return address < cursor->second ? cursor : state.reservations.end();
}

bool RangeIsFree(const State& state, const std::uintptr_t start, const std::uintptr_t end)
{
    if (start < kGuestRangeBegin || end > kGuestRangeEnd || end <= start)
    {
        return false;
    }
    auto cursor = state.reservations.lower_bound(start);
    if (cursor != state.reservations.end() && cursor->first < end)
    {
        return false;
    }
    return FindReservation(state, start) == state.reservations.end();
}

// The first free, granularity-aligned place for `length` bytes.
std::uintptr_t FindFree(const State& state, const std::uintptr_t length)
{
    std::uintptr_t candidate = kGuestRangeBegin;
    for (const auto& [base, end] : state.reservations)
    {
        if (base >= candidate + length)
        {
            break;
        }
        candidate = AlignUp(end > candidate ? end : candidate, kReservationGranularity);
    }
    return candidate + length <= kGuestRangeEnd ? candidate : 0U;
}

// Where the link put Emscripten's data (-sGLOBAL_BASE). The guest range is
// only free when it lies wholly below.
extern "C" unsigned char __global_base;

bool g_layout_reported = false;

// Whether this binary was linked with the guest range kept free. A binary
// linked without it has Emscripten's data at 1024, and handing out the guest
// range there would overwrite it.
bool GuestRangeIsFree()
{
    if (reinterpret_cast<std::uintptr_t>(&__global_base) >= kGuestRangeEnd)
    {
        return true;
    }
    web::ReportUnsupportedOnce(
        &g_layout_reported,
        "[repiu-web] virtual memory is unavailable: this binary is not linked "
        "with -sGLOBAL_BASE=0x10000000, so the guest range is Emscripten's own "
        "memory.\n");
    return false;
}

// Whether `address` is in Emscripten's own part of linear memory: data, stack
// and heap, which are always there and always readable and writable.
bool InRuntimeMemory(const std::uintptr_t address)
{
    return address >= kGuestRangeEnd && address < emscripten_get_heap_size();
}

}  // namespace

MemoryReservation ReserveMemory(void* const preferred_base,
                                const std::size_t bytes,
                                const bool commit,
                                const MemoryProtection protection)
{
    MemoryReservation reservation;
    if (bytes == 0U)
    {
        reservation.error = 87U;  // ERROR_INVALID_PARAMETER, as Win32 reports.
        return reservation;
    }
    if (!GuestRangeIsFree())
    {
        reservation.error = 38U;  // ENOSYS
        return reservation;
    }
    State& state = Shared();
    const std::uintptr_t length = AlignUp(static_cast<std::uintptr_t>(bytes), kPageSize);
    std::uintptr_t base = 0U;
    if (preferred_base != nullptr)
    {
        const std::uintptr_t wanted =
            AlignDown(reinterpret_cast<std::uintptr_t>(preferred_base), kReservationGranularity);
        if (!RangeIsFree(state, wanted, wanted + length))
        {
            // A preferred address is a demand, as VirtualAlloc treats it.
            reservation.error = 487U;  // ERROR_INVALID_ADDRESS
            return reservation;
        }
        base = wanted;
    }
    else
    {
        base = FindFree(state, length);
        if (base == 0U)
        {
            reservation.error = 8U;  // ERROR_NOT_ENOUGH_MEMORY
            return reservation;
        }
    }
    state.reservations[base] = base + length;
    std::memset(reinterpret_cast<void*>(base), 0, static_cast<std::size_t>(length));
    RecordProtection(&state, base, base + length,
                     commit ? protection : MemoryProtection::kNoAccess);
    reservation.valid = true;
    reservation.base = reinterpret_cast<void*>(base);
    reservation.size = static_cast<std::size_t>(length);
    return reservation;
}

bool CommitMemory(void* const base, const std::size_t bytes, const MemoryProtection protection)
{
    return ProtectMemory(base, bytes, protection, nullptr);
}

bool ReleaseMemory(void* const base, const std::size_t bytes)
{
    if (base == nullptr)
    {
        return false;
    }
    State& state = Shared();
    const auto start = reinterpret_cast<std::uintptr_t>(base);
    const auto reservation = state.reservations.find(start);
    std::uintptr_t end = start + AlignUp(static_cast<std::uintptr_t>(bytes), kPageSize);
    if (reservation != state.reservations.end())
    {
        // Releasing by base alone is what the Win32 callers do, so the
        // recorded length wins over whatever the caller passed.
        end = reservation->second;
        state.reservations.erase(reservation);
    }
    else if (bytes == 0U)
    {
        return false;
    }
    ForgetRange(&state, start, end);
    // A later reservation here gets zeroed pages, as a fresh mapping would.
    std::memset(base, 0, static_cast<std::size_t>(end - start));
    NotifyMemoryChange(MemoryChange::kProtection, base, static_cast<std::size_t>(end - start));
    return true;
}

bool ProtectMemory(void* const address,
                   const std::size_t bytes,
                   const MemoryProtection protection,
                   MemoryProtection* const previous)
{
    if (address == nullptr || bytes == 0U || protection == MemoryProtection::kOther)
    {
        return false;
    }
    State& state = Shared();
    // Rounded outward to whole pages, as Win32 does for its callers.
    const auto requested = reinterpret_cast<std::uintptr_t>(address);
    const std::uintptr_t start = AlignDown(requested, kPageSize);
    const std::uintptr_t end = AlignUp(requested + bytes, kPageSize);
    if (FindReservation(state, start) == state.reservations.end() ||
        FindReservation(state, end - 1U) == state.reservations.end())
    {
        return false;
    }
    if (previous != nullptr)
    {
        const MemoryRegion existing = QueryMemory(address);
        *previous = existing.valid ? existing.protection : MemoryProtection::kOther;
    }
    RecordProtection(&state, start, end, protection);
    NotifyMemoryChange(MemoryChange::kProtection, reinterpret_cast<void*>(start),
                       static_cast<std::size_t>(end - start));
    return true;
}

MemoryRegion QueryMemory(const void* const address)
{
    MemoryRegion region;
    const auto where = reinterpret_cast<std::uintptr_t>(address);
    const State& state = Shared();
    const auto reservation = FindReservation(state, where);
    if (reservation != state.reservations.end())
    {
        auto run = state.protections.upper_bound(where);
        if (run != state.protections.begin())
        {
            --run;
            if (where < run->second.end)
            {
                region.valid = true;
                region.claimed = true;
                // A reservation without a commit is recorded kNoAccess, which
                // is what Windows reports as reserved but not committed.
                region.committed = run->second.protection != MemoryProtection::kNoAccess;
                region.base = reinterpret_cast<const void*>(run->first);
                region.size = static_cast<std::size_t>(run->second.end - run->first);
                region.allocation_base = reinterpret_cast<const void*>(reservation->first);
                region.protection = run->second.protection;
                ApplyAccessFlags(region.protection, &region);
                return region;
            }
        }
    }
    if (InRuntimeMemory(where))
    {
        const std::uintptr_t page = AlignDown(where, kPageSize);
        region.valid = true;
        region.claimed = true;
        region.committed = true;
        region.base = reinterpret_cast<const void*>(page);
        region.size = static_cast<std::size_t>(emscripten_get_heap_size() - page);
        region.allocation_base = reinterpret_cast<const void*>(kGuestRangeEnd);
        region.protection = MemoryProtection::kReadWrite;
        ApplyAccessFlags(region.protection, &region);
        return region;
    }
    // Free: valid and unclaimed, which is the answer that lets a caller
    // reserve here.
    region.valid = where >= kGuestRangeBegin && where < kGuestRangeEnd;
    region.base = reinterpret_cast<const void*>(AlignDown(where, kPageSize));
    region.size = region.valid ? kPageSize : 0U;
    region.protection = MemoryProtection::kNoAccess;
    return region;
}

// wasm has no instruction cache, but the emulated CPU keeps translated and
// decoded code, so a code write is still reported for it to drop.
bool FlushInstructionCacheRange(const void* const address, const std::size_t bytes)
{
    if (address == nullptr || bytes == 0U)
    {
        return false;
    }
    NotifyMemoryChange(MemoryChange::kCode, address, bytes);
    return true;
}

std::size_t SystemPageSize()
{
    return static_cast<std::size_t>(kPageSize);
}

bool IsRangeReadable(const void* const address, const std::size_t bytes)
{
    if (address == nullptr || bytes == 0U)
    {
        return false;
    }
    const auto start = reinterpret_cast<std::uintptr_t>(address);
    const std::uintptr_t end = start + bytes;
    if (end < start)
    {
        return false;
    }
    std::uintptr_t cursor = start;
    while (cursor < end)
    {
        const MemoryRegion region = QueryMemory(reinterpret_cast<const void*>(cursor));
        if (!region.valid || !region.committed || !region.readable || region.size == 0U)
        {
            return false;
        }
        cursor = reinterpret_cast<std::uintptr_t>(region.base) + region.size;
    }
    return true;
}

}  // namespace repiu::platform

#endif  // __EMSCRIPTEN__
