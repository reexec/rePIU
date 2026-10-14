#include "repiu/platform/virtual_memory.h"

#if !defined(_WIN32)

#include <cerrno>
#include <cstring>
#include <limits>
#include <map>
#include <mutex>

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

namespace repiu::platform
{
namespace
{

// Why a shadow table exists at all.
//
// mprotect does not report the protection it replaced, and the guest store path
// depends on getting it back: protect a page writable, write, restore. The only
// way to ask the kernel is /proc/self/maps, which means opening and parsing a
// file for every guest store -- unusable on a path that runs millions of times.
//
// So this backend records what it maps and what it protects. The record is
// authoritative for memory rePIU created, which is every address the engine
// protects. Anything else -- the host stack, a shared library, memory a driver
// mapped -- falls through to /proc/self/maps, which only the cold pointer
// validation path reaches.

struct ProtectionInterval
{
    std::uintptr_t end = 0;
    MemoryProtection protection = MemoryProtection::kNoAccess;
};

struct ShadowState
{
    std::mutex mutex;
    // Keyed by interval start. Non-overlapping and kept in address order, so a
    // lookup is the last entry starting at or below the address.
    std::map<std::uintptr_t, ProtectionInterval> protections;
    // Keyed by reservation start, valued by its end. Protection changes split
    // the map above but must not change what a query reports as the enclosing
    // allocation, so reservations are tracked separately.
    std::map<std::uintptr_t, std::uintptr_t> reservations;
};

ShadowState& Shadow()
{
    static ShadowState state;
    return state;
}

int ToPosixProtection(const MemoryProtection protection)
{
    switch (protection)
    {
        case MemoryProtection::kNoAccess:
            return PROT_NONE;
        case MemoryProtection::kReadOnly:
            return PROT_READ;
        case MemoryProtection::kReadWrite:
            return PROT_READ | PROT_WRITE;
        case MemoryProtection::kExecuteRead:
            return PROT_READ | PROT_EXEC;
        case MemoryProtection::kExecuteReadWrite:
            return PROT_READ | PROT_WRITE | PROT_EXEC;
        case MemoryProtection::kOther:
            break;
    }
    // kOther describes what was found, never what is wanted. -1 is not a valid
    // protection, so callers reject it rather than mapping something arbitrary.
    return -1;
}

MemoryProtection FromAccessFlags(const bool readable,
                                 const bool writable,
                                 const bool executable)
{
    if (!readable && !writable && !executable)
    {
        return MemoryProtection::kNoAccess;
    }
    if (readable && !writable && !executable)
    {
        return MemoryProtection::kReadOnly;
    }
    if (readable && writable && !executable)
    {
        return MemoryProtection::kReadWrite;
    }
    if (readable && !writable && executable)
    {
        return MemoryProtection::kExecuteRead;
    }
    if (readable && writable && executable)
    {
        return MemoryProtection::kExecuteReadWrite;
    }
    // Write-only or execute-only: legal to ask the kernel for, never asked for
    // here, and not one of the named protections.
    return MemoryProtection::kOther;
}

void ApplyAccessFlags(const MemoryProtection protection, MemoryRegion* region)
{
    switch (protection)
    {
        case MemoryProtection::kReadOnly:
            region->readable = true;
            break;
        case MemoryProtection::kReadWrite:
            region->readable = true;
            region->writable = true;
            break;
        case MemoryProtection::kExecuteRead:
            region->readable = true;
            region->executable = true;
            break;
        case MemoryProtection::kExecuteReadWrite:
            region->readable = true;
            region->writable = true;
            region->executable = true;
            break;
        case MemoryProtection::kNoAccess:
        case MemoryProtection::kOther:
            break;
    }
}

std::size_t PageSize()
{
    static const std::size_t size =
        static_cast<std::size_t>(::sysconf(_SC_PAGESIZE));
    return size;
}

std::uintptr_t AlignDown(const std::uintptr_t value)
{
    return value & ~(static_cast<std::uintptr_t>(PageSize()) - 1U);
}

std::uintptr_t AlignUp(const std::uintptr_t value)
{
    const std::uintptr_t mask = static_cast<std::uintptr_t>(PageSize()) - 1U;
    return (value + mask) & ~mask;
}

// Records [start, end) as `protection`, trimming and splitting whatever it
// overlaps. The caller holds the lock.
void RecordProtection(ShadowState* shadow,
                      const std::uintptr_t start,
                      const std::uintptr_t end,
                      const MemoryProtection protection)
{
    if (end <= start)
    {
        return;
    }
    auto cursor = shadow->protections.upper_bound(start);
    if (cursor != shadow->protections.begin())
    {
        --cursor;
    }
    while (cursor != shadow->protections.end() && cursor->first < end)
    {
        const std::uintptr_t existing_start = cursor->first;
        const std::uintptr_t existing_end = cursor->second.end;
        if (existing_end <= start)
        {
            ++cursor;
            continue;
        }
        const MemoryProtection existing_protection = cursor->second.protection;
        cursor = shadow->protections.erase(cursor);
        // Whatever of the old interval sticks out on either side survives with
        // its own protection.
        if (existing_start < start)
        {
            shadow->protections.emplace(
                existing_start,
                ProtectionInterval{start, existing_protection});
        }
        if (existing_end > end)
        {
            cursor = shadow->protections.emplace(
                end,
                ProtectionInterval{existing_end, existing_protection}).first;
            ++cursor;
        }
    }
    shadow->protections.emplace(start, ProtectionInterval{end, protection});
}

void ForgetRange(ShadowState* shadow,
                 const std::uintptr_t start,
                 const std::uintptr_t end)
{
    auto cursor = shadow->protections.upper_bound(start);
    if (cursor != shadow->protections.begin())
    {
        --cursor;
    }
    while (cursor != shadow->protections.end() && cursor->first < end)
    {
        const std::uintptr_t existing_start = cursor->first;
        const std::uintptr_t existing_end = cursor->second.end;
        if (existing_end <= start)
        {
            ++cursor;
            continue;
        }
        const MemoryProtection existing_protection = cursor->second.protection;
        cursor = shadow->protections.erase(cursor);
        if (existing_start < start)
        {
            shadow->protections.emplace(
                existing_start,
                ProtectionInterval{start, existing_protection});
        }
        if (existing_end > end)
        {
            cursor = shadow->protections.emplace(
                end,
                ProtectionInterval{existing_end, existing_protection}).first;
            ++cursor;
        }
    }
}

// The caller holds the lock. Returns false when the address is not in anything
// this backend mapped.
bool LookupShadow(ShadowState* shadow,
                  const std::uintptr_t address,
                  MemoryRegion* region)
{
    auto cursor = shadow->protections.upper_bound(address);
    if (cursor == shadow->protections.begin())
    {
        return false;
    }
    --cursor;
    if (address >= cursor->second.end)
    {
        return false;
    }
    region->valid = true;
    // Task 503d-19: the shadow only holds what this backend mapped, so a hit
    // is by definition claimed.
    region->claimed = true;
    // A reservation made without committing is mapped PROT_NONE, and that is
    // exactly what Windows reports as reserved-but-not-committed. Since nothing
    // in this project ever asks to protect a page kNoAccess, the mapping from
    // "no access" to "not committed" is unambiguous here.
    region->committed =
        cursor->second.protection != MemoryProtection::kNoAccess;
    region->base = reinterpret_cast<const void*>(cursor->first);
    region->size = static_cast<std::size_t>(cursor->second.end - cursor->first);
    region->protection = cursor->second.protection;
    ApplyAccessFlags(region->protection, region);

    region->allocation_base = region->base;
    auto reservation = shadow->reservations.upper_bound(address);
    if (reservation != shadow->reservations.begin())
    {
        --reservation;
        if (address < reservation->second)
        {
            region->allocation_base =
                reinterpret_cast<const void*>(reservation->first);
        }
    }
    return true;
}

std::uintptr_t ParseHex(const char* text,
                        const std::size_t length,
                        std::size_t* consumed)
{
    std::uintptr_t value = 0;
    std::size_t index = 0;
    while (index < length)
    {
        const char character = text[index];
        std::uintptr_t digit = 0;
        if (character >= '0' && character <= '9')
        {
            digit = static_cast<std::uintptr_t>(character - '0');
        }
        else if (character >= 'a' && character <= 'f')
        {
            digit = static_cast<std::uintptr_t>(character - 'a') + 10U;
        }
        else if (character >= 'A' && character <= 'F')
        {
            digit = static_cast<std::uintptr_t>(character - 'A') + 10U;
        }
        else
        {
            break;
        }
        value = (value << 4U) | digit;
        ++index;
    }
    *consumed = index;
    return value;
}

// One line of /proc/self/maps: "start-end perms offset dev inode path".
bool ParseMapsLine(const char* line,
                   const std::size_t length,
                   MemoryRegion* region)
{
    std::size_t consumed = 0;
    const std::uintptr_t start = ParseHex(line, length, &consumed);
    if (consumed == 0U || consumed >= length || line[consumed] != '-')
    {
        return false;
    }
    std::size_t offset = consumed + 1U;
    std::size_t end_consumed = 0;
    const std::uintptr_t end =
        ParseHex(line + offset, length - offset, &end_consumed);
    if (end_consumed == 0U)
    {
        return false;
    }
    offset += end_consumed;
    if (offset >= length || line[offset] != ' ')
    {
        return false;
    }
    ++offset;
    if (offset + 3U >= length)
    {
        return false;
    }
    const bool readable = line[offset] == 'r';
    const bool writable = line[offset + 1U] == 'w';
    const bool executable = line[offset + 2U] == 'x';

    region->valid = true;
    region->claimed = true;
    region->committed = true;
    region->base = reinterpret_cast<const void*>(start);
    region->size = static_cast<std::size_t>(end - start);
    region->allocation_base = region->base;
    region->readable = readable;
    region->writable = writable;
    region->executable = executable;
    region->protection = FromAccessFlags(readable, writable, executable);
    return true;
}

// The fallback, for addresses this backend did not map. It opens a file, so it
// is far slower than the shadow lookup and is deliberately reached only when
// the shadow misses -- which on the engine's paths means a host pointer, not a
// guest one.
// Task 503d-19: `next_mapping_start` receives the lowest mapping that begins
// above `address` when nothing covers it, which is what turns a miss into the
// extent of the free run rather than a failure.
bool QueryProcMaps(const std::uintptr_t address,
                   MemoryRegion* region,
                   std::uintptr_t* next_mapping_start)
{
    if (next_mapping_start != nullptr)
    {
        *next_mapping_start = 0;
    }
    const int file = ::open("/proc/self/maps", O_RDONLY | O_CLOEXEC);
    if (file < 0)
    {
        return false;
    }
    // Read in chunks, carrying any partial line forward. No allocation, so this
    // stays usable from a fault handler.
    char buffer[8192];
    std::size_t carried = 0;
    bool found = false;
    while (!found)
    {
        const ssize_t read_bytes =
            ::read(file, buffer + carried, sizeof(buffer) - carried);
        if (read_bytes <= 0)
        {
            break;
        }
        const std::size_t filled = carried + static_cast<std::size_t>(read_bytes);
        std::size_t line_start = 0;
        for (std::size_t index = 0; index < filled; ++index)
        {
            if (buffer[index] != '\n')
            {
                continue;
            }
            MemoryRegion candidate;
            if (ParseMapsLine(buffer + line_start,
                              index - line_start,
                              &candidate))
            {
                const auto base =
                    reinterpret_cast<std::uintptr_t>(candidate.base);
                if (address >= base && address < base + candidate.size)
                {
                    *region = candidate;
                    found = true;
                    break;
                }
                // The file is ordered by address, so the first mapping that
                // starts above the address is the end of the free run.
                if (next_mapping_start != nullptr &&
                    *next_mapping_start == 0 && base > address)
                {
                    *next_mapping_start = base;
                }
            }
            line_start = index + 1U;
        }
        if (found)
        {
            break;
        }
        carried = filled - line_start;
        if (carried >= sizeof(buffer))
        {
            // A line longer than the buffer cannot be parsed; giving up beats
            // looping without progress.
            break;
        }
        std::memmove(buffer, buffer + line_start, carried);
    }
    ::close(file);
    return found;
}

}  // namespace

MemoryReservation ReserveMemory(void* preferred_base,
                                const std::size_t bytes,
                                const bool commit,
                                const MemoryProtection protection)
{
    MemoryReservation reservation;
    const int posix_protection = ToPosixProtection(protection);
    if (bytes == 0U || posix_protection < 0)
    {
        reservation.error = EINVAL;
        return reservation;
    }

    // Linux has no reserve-versus-commit distinction of the Win32 kind. A
    // reservation is an anonymous mapping either way; what `commit` controls is
    // whether pages may be backed on first touch, so an uncommitted reservation
    // is mapped PROT_NONE and protected later.
    const int mapped_protection = commit ? posix_protection : PROT_NONE;
    int flags = MAP_PRIVATE | MAP_ANONYMOUS;
    if (preferred_base != nullptr)
    {
#if defined(MAP_FIXED_NOREPLACE)
        // Not MAP_FIXED: that would unmap whatever already lives there. This
        // fails instead, which is what the callers want -- they all check the
        // address they got back.
        flags |= MAP_FIXED_NOREPLACE;
#endif
    }

    const std::size_t rounded = static_cast<std::size_t>(
        AlignUp(static_cast<std::uintptr_t>(bytes)));
    void* base = ::mmap(preferred_base, rounded, mapped_protection, flags, -1, 0);
    if (base == MAP_FAILED)
    {
        reservation.error = static_cast<std::uint32_t>(errno);
        return reservation;
    }

    const auto start = reinterpret_cast<std::uintptr_t>(base);
    {
        ShadowState& shadow = Shadow();
        const std::lock_guard<std::mutex> guard(shadow.mutex);
        shadow.reservations.emplace(start, start + rounded);
        RecordProtection(&shadow,
                         start,
                         start + rounded,
                         commit ? protection : MemoryProtection::kNoAccess);
    }

    reservation.valid = true;
    reservation.base = base;
    reservation.size = rounded;
    return reservation;
}

bool CommitMemory(void* base,
                  const std::size_t bytes,
                  const MemoryProtection protection)
{
    // Linux has nothing to commit: the reservation is already a mapping, just
    // an inaccessible one. Granting it a protection is all that is needed, and
    // ProtectMemory already records the change.
    return ProtectMemory(base, bytes, protection, nullptr);
}

bool ReleaseMemory(void* base, const std::size_t bytes)
{
    if (base == nullptr || bytes == 0U)
    {
        return false;
    }
    const auto start = reinterpret_cast<std::uintptr_t>(base);
    std::size_t length = static_cast<std::size_t>(
        AlignUp(static_cast<std::uintptr_t>(bytes)));
    {
        ShadowState& shadow = Shadow();
        const std::lock_guard<std::mutex> guard(shadow.mutex);
        const auto reservation = shadow.reservations.find(start);
        if (reservation != shadow.reservations.end())
        {
            // Releasing by base alone is what the Win32 callers do, so the
            // recorded length wins over whatever the caller passed.
            length = static_cast<std::size_t>(reservation->second - start);
            shadow.reservations.erase(reservation);
        }
        ForgetRange(&shadow, start, start + length);
    }
    if (::munmap(base, length) != 0)
    {
        return false;
    }
    NotifyMemoryChange(MemoryChange::kProtection, base, length);
    return true;
}

bool ProtectMemory(void* address,
                   const std::size_t bytes,
                   const MemoryProtection protection,
                   MemoryProtection* previous)
{
    const int posix_protection = ToPosixProtection(protection);
    if (address == nullptr || bytes == 0U || posix_protection < 0)
    {
        return false;
    }
    // mprotect requires a page-aligned start and covers whole pages. Win32
    // rounds outward for the caller, and several call sites rely on that --
    // they protect sizeof(value) bytes around a guest store.
    const std::uintptr_t requested = reinterpret_cast<std::uintptr_t>(address);
    const std::uintptr_t start = AlignDown(requested);
    const std::uintptr_t end = AlignUp(requested + bytes);

    ShadowState& shadow = Shadow();
    const std::lock_guard<std::mutex> guard(shadow.mutex);
    if (previous != nullptr)
    {
        // Windows reports the protection of the first page in the range, so
        // this does too, and reports kOther when nothing is on record rather
        // than inventing a value the caller might restore.
        MemoryRegion existing;
        *previous = LookupShadow(&shadow, requested, &existing)
            ? existing.protection
            : MemoryProtection::kOther;
    }
    if (::mprotect(reinterpret_cast<void*>(start),
                   static_cast<std::size_t>(end - start),
                   posix_protection) != 0)
    {
        return false;
    }
    RecordProtection(&shadow, start, end, protection);
    // Issue #61. Under the shadow lock: the observer only queues the range.
    NotifyMemoryChange(MemoryChange::kProtection, reinterpret_cast<void*>(start),
                       static_cast<std::size_t>(end - start));
    return true;
}

bool FlushInstructionCacheRange(const void* address, const std::size_t bytes)
{
    if (address == nullptr || bytes == 0U)
    {
        return false;
    }
    // The builtin takes a mutable range even though it only invalidates.
    char* begin = const_cast<char*>(static_cast<const char*>(address));
    __builtin___clear_cache(begin, begin + bytes);
    NotifyMemoryChange(MemoryChange::kCode, address, bytes);
    return true;
}

std::size_t SystemPageSize()
{
    return PageSize();
}

MemoryRegion QueryMemory(const void* address)
{
    MemoryRegion region;
    if (address == nullptr)
    {
        return region;
    }
    const auto value = reinterpret_cast<std::uintptr_t>(address);
    {
        ShadowState& shadow = Shadow();
        const std::lock_guard<std::mutex> guard(shadow.mutex);
        if (LookupShadow(&shadow, value, &region))
        {
            return region;
        }
    }
    std::uintptr_t next_mapping_start = 0;
    if (!QueryProcMaps(value, &region, &next_mapping_start))
    {
        // Task 503d-19: nothing maps this address, which is an answer rather
        // than a failure -- it is what Windows calls MEM_FREE. The run reaches
        // the next mapping, or the end of the address space when there is none
        // above.
        region = MemoryRegion{};
        region.valid = true;
        region.claimed = false;
        const std::uintptr_t page = static_cast<std::uintptr_t>(PageSize());
        const std::uintptr_t start = value & ~(page - 1U);
        region.base = reinterpret_cast<const void*>(start);
        region.allocation_base = region.base;
        const std::uintptr_t end = next_mapping_start > start
            ? next_mapping_start
            : std::numeric_limits<std::uintptr_t>::max();
        region.size = static_cast<std::size_t>(end - start);
        return region;
    }
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
        const auto region_base = reinterpret_cast<std::uintptr_t>(region.base);
        const std::uintptr_t region_end = region_base + region.size;
        if (region_end <= cursor)
        {
            return false;
        }
        cursor = region_end;
    }
    return true;
}

}  // namespace repiu::platform

#endif
