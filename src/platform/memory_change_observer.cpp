#include "repiu/platform/virtual_memory.h"

// Issue #61. The observer of virtual_memory.h, shared by every host: each
// platform's ProtectMemory, ReleaseMemory and FlushInstructionCacheRange
// report through NotifyMemoryChange after they succeed.

#include <atomic>

namespace repiu::platform
{
namespace
{

// One pair, replaced whole: the observer and its data are published together
// so a caller on another thread never pairs one with the other's predecessor.
struct ObserverSlot
{
    MemoryChangeObserver observer = nullptr;
    void* user_data = nullptr;
};

std::atomic<const ObserverSlot*> g_slot{nullptr};

}  // namespace

void SetMemoryChangeObserver(const MemoryChangeObserver observer, void* const user_data)
{
    // Set once per guest run and never under contention, so the previous slot
    // is simply kept: a notification in flight on another thread may still be
    // reading it.
    const ObserverSlot* const slot =
        observer != nullptr ? new ObserverSlot{observer, user_data} : nullptr;
    g_slot.store(slot, std::memory_order_release);
}

void NotifyMemoryChange(const MemoryChange change, const void* const address,
                        const std::size_t bytes)
{
    const ObserverSlot* const slot = g_slot.load(std::memory_order_acquire);
    if (slot == nullptr || bytes == 0U)
    {
        return;
    }
    slot->observer(change, reinterpret_cast<std::uintptr_t>(address), bytes,
                   slot->user_data);
}

}  // namespace repiu::platform
