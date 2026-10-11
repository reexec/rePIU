#include "repiu/engine/piu10_frame_sync_history.h"

namespace repiu::engine
{
namespace
{

class HistoryLock
{
public:
    explicit HistoryLock(std::atomic_flag* flag) : flag_(flag)
    {
        while (flag_->test_and_set(std::memory_order_acquire))
        {
        }
    }
    ~HistoryLock() { flag_->clear(std::memory_order_release); }

    HistoryLock(const HistoryLock&) = delete;
    HistoryLock& operator=(const HistoryLock&) = delete;

private:
    std::atomic_flag* flag_;
};

}  // namespace

void Piu10FrameSyncHistory::Record(std::uint64_t time_ns, std::uint8_t value)
{
    HistoryLock lock(&lock_);
    edges_[next_] = Edge{time_ns, value};
    next_ = (next_ + 1U) % kCapacity;
    if (size_ < kCapacity)
    {
        ++size_;
    }
}

std::uint8_t Piu10FrameSyncHistory::ValueAt(std::uint64_t time_ns,
                                            std::uint8_t live) const
{
    HistoryLock lock(&lock_);
    if (size_ == 0U)
    {
        return live;
    }
    // The newest recorded flip, not `live`: the worker flips the live bit
    // before it records the flip, and a read between the two found no newer
    // flip here and answered with the already flipped bit -- one phantom
    // change each way, two frames on the game's count.
    const std::uint32_t newest = (next_ + kCapacity - 1U) % kCapacity;
    if (edges_[newest].time_ns <= time_ns)
    {
        return edges_[newest].value;
    }
    // Newest first: the latest flip at or before `time_ns` set the bit.
    for (std::uint32_t back = 1U; back < size_; ++back)
    {
        const Edge& edge = edges_[(newest + kCapacity - back) % kCapacity];
        if (edge.time_ns <= time_ns)
        {
            return edge.value;
        }
    }
    // Older than every recorded flip: the bit before the oldest one.
    const Edge& oldest = edges_[(newest + kCapacity - (size_ - 1U)) % kCapacity];
    return static_cast<std::uint8_t>(oldest.value ^ 1U);
}

std::uint64_t Piu10FrameSyncAnswerClock::Answer(std::uint64_t guest_ns,
                                                std::uint64_t now_ns)
{
    std::uint64_t answer = guest_ns != 0U && guest_ns < now_ns
        ? guest_ns : now_ns;
    if (answer < last_answer_ns_)
    {
        answer = last_answer_ns_;
    }
    last_answer_ns_ = answer;
    return answer;
}

}  // namespace repiu::engine
