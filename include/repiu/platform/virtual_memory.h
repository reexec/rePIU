#ifndef REPIU_PLATFORM_VIRTUAL_MEMORY_H_
#define REPIU_PLATFORM_VIRTUAL_MEMORY_H_

#include <cstddef>
#include <cstdint>

// Task 503b. Reserving, protecting, and asking about virtual memory, without
// naming an operating system.
//
// The Win32 engine calls VirtualProtect at 50 sites, VirtualFree at 30,
// VirtualAlloc at 16, and VirtualQuery at 16. Those four do not port as a set:
// mprotect and mmap correspond closely, but VirtualQuery has no cheap Linux
// counterpart -- answering it there means parsing /proc/self/maps -- and
// mprotect, unlike VirtualProtect, does not report the protection it replaced.
//
// Rather than mirror the Win32 shapes and pay for them on Linux, this API asks
// what the call sites actually ask. Two findings from reading all 112 of them
// shaped it:
//
//   * No caller ever passes PAGE_GUARD, PAGE_WRITECOPY, or PAGE_NOACCESS to
//     VirtualProtect. They appear only inside five separate hand-written
//     "is this protection readable" classifiers, which disagree in their
//     details. MemoryRegion answers that question directly instead.
//
//   * The protection VirtualProtect reports as previous IS consumed -- the
//     guest store path protects a page writable, writes, and puts the old
//     protection back. Linux cannot report it, so the Linux backend records
//     what it set and hands that back.
//
// See docs/design/20260822-503-linux-execution-engine.md.

namespace repiu::platform
{

// The protections this project actually sets. Anything a host reports outside
// this set is `kOther`, which keeps a query honest rather than rounding an
// unfamiliar value to a familiar one; the classification flags on MemoryRegion
// stay correct in that case.
enum class MemoryProtection : std::uint8_t
{
    kNoAccess,
    kReadOnly,
    kReadWrite,
    kExecuteRead,
    kExecuteReadWrite,
    kOther,
};

struct MemoryRegion
{
    // False when the address could not be described at all. A valid region may
    // still be uncommitted, and may not even be claimed.
    bool valid = false;
    // Task 503d-19. Whether the address space belongs to anyone.
    //
    // This is a third state, not a synonym for `committed`. Windows reports
    // MEM_FREE, MEM_RESERVE and MEM_COMMIT, and only the first means an
    // allocation there would succeed -- reserved-but-uncommitted space is
    // claimed and would refuse a second reservation.
    //
    // The runtime memory policy asks exactly this before reserving the guest's
    // fixed range, and it is where the two hosts disagreed most sharply: a
    // Windows query describes free space, while a Linux query for an unmapped
    // address used to fail outright. Absorbing that here rather than at the
    // call site is what this layer is for -- an unclaimed region is now valid
    // on both, with `base` and `size` naming the free run.
    bool claimed = false;
    // Reserved address space that has not been committed reports false on both
    // hosts: Windows says MEM_RESERVE, and Linux maps such a reservation
    // PROT_NONE, which this API reads back the same way. The one case where the
    // two would disagree -- a committed page deliberately protected kNoAccess --
    // does not arise, because nothing here ever asks for that protection.
    bool committed = false;
    bool readable = false;
    bool writable = false;
    bool executable = false;
    // Start and length of the run of pages sharing this protection.
    const void* base = nullptr;
    std::size_t size = 0;
    // Start of the whole reservation this region belongs to, which is what the
    // guest stack limit is derived from. Equal to `base` when the host cannot
    // distinguish the two.
    const void* allocation_base = nullptr;
    MemoryProtection protection = MemoryProtection::kNoAccess;
};

struct MemoryReservation
{
    bool valid = false;
    void* base = nullptr;
    std::size_t size = 0;
    // GetLastError on Windows, errno on POSIX. Zero on success. Kept as a
    // number rather than a message because callers format it into their own
    // diagnostics.
    std::uint32_t error = 0;
};

// Reserves `bytes` of address space, committing it as well unless `commit` is
// false. `preferred_base` of nullptr means anywhere.
//
// A preferred base is a request, not a guarantee, and the two hosts fail it
// differently: Windows may hand back a different address, Linux refuses. Either
// way the caller must compare `base` against what it asked for -- every existing
// call site already does.
//
// Reservation granularity also differs: 64 KiB on Windows against one page on
// Linux, so a caller that reserves adjacent ranges must round up itself.
MemoryReservation ReserveMemory(void* preferred_base,
                                std::size_t bytes,
                                bool commit,
                                MemoryProtection protection);

// Backs part of an existing reservation with memory and gives it a protection.
//
// This is separate from ProtectMemory because the two hosts disagree about
// whether it is the same operation. On Linux it is: an uncommitted reservation
// is a PROT_NONE mapping and mprotect makes it usable. On Windows VirtualProtect
// fails outright on reserved-but-uncommitted pages, and only VirtualAlloc with
// MEM_COMMIT will do. Folding the two together would leave a call that works on
// one host and faults on the other -- which is exactly how the probe found this.
bool CommitMemory(void* base, std::size_t bytes, MemoryProtection protection);

// Releases a whole reservation. `bytes` is ignored on Windows, which releases
// by base alone, and required on Linux.
bool ReleaseMemory(void* base, std::size_t bytes);

// Changes protection over a range. When `previous` is not null it receives the
// protection that was in force, which is what lets a caller restore it.
bool ProtectMemory(void* address,
                   std::size_t bytes,
                   MemoryProtection protection,
                   MemoryProtection* previous);

// Describes the region containing `address`.
[[nodiscard]] MemoryRegion QueryMemory(const void* address);

// Task 503d-11. Tells the processor that code in this range has changed.
//
// The engine writes translated code and patches it in place, so it has to say
// so before the range is executed again. On x86 the instruction cache is
// coherent with data writes and this costs almost nothing, but saying it is
// still correct rather than relying on that.
//
// Windows spells it FlushInstructionCache against its own process; GCC and
// Clang provide __builtin___clear_cache. Returns false only when the host
// refuses.
bool FlushInstructionCacheRange(const void* address, std::size_t bytes);

// Returns the host virtual-memory page size. AOT patch windows use this to
// change protection only on the pages that contain modified code.
[[nodiscard]] std::size_t SystemPageSize();

// Whether every byte of the range can be read. Spans regions, so a range that
// crosses from a readable page into an unmapped one is correctly rejected.
[[nodiscard]] bool IsRangeReadable(const void* address, std::size_t bytes);

// Issue #61. What a successful call above changed, for an observer that keeps
// its own view of the host's memory -- the emulated execution model's CPU,
// which checks guest accesses against a page table mirrored from the host
// rather than taking hardware faults.
enum class MemoryChange : std::uint8_t
{
    // ProtectMemory or ReleaseMemory: the range's protection is no longer what
    // was last read.
    kProtection,
    // FlushInstructionCacheRange: code in the range was rewritten.
    kCode,
};

// Called after the change, on whichever thread made it, so it must only
// record the range. One observer at a time; nullptr removes it.
using MemoryChangeObserver = void (*)(MemoryChange change,
                                      std::uintptr_t address,
                                      std::size_t bytes,
                                      void* user_data);
void SetMemoryChangeObserver(MemoryChangeObserver observer, void* user_data);

// For the per-platform implementations above: reports a change to the
// observer, if any.
void NotifyMemoryChange(MemoryChange change, const void* address, std::size_t bytes);

}  // namespace repiu::platform

#endif  // REPIU_PLATFORM_VIRTUAL_MEMORY_H_
