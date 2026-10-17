#ifndef REPIU_ENGINE_FRAME_RATE_HISTORY_H_
#define REPIU_ENGINE_FRAME_RATE_HISTORY_H_

#include <array>
#include <cstddef>

namespace repiu::engine
{

// Issue #88. The frame rate of the last 60 seconds, one slot a second, for
// the OSD's graph (docs/design/20261017-i088-osd-frame-rate-graph.md). Fed
// with the backend's one-second measurement; a measurement that took several
// seconds (a load with no swaps) fills that many slots, so the slots always
// span real seconds.
class FrameRateHistory
{
public:
    static constexpr std::size_t kSlots = 60U;

    // `frames_per_second` measured over `seconds` (about 1 normally).
    void Record(float frames_per_second, double seconds);

    [[nodiscard]] std::size_t count() const
    {
        return count_;
    }
    // The newest value; 0 before the first.
    [[nodiscard]] float latest() const;
    // The values oldest first into `out` (kSlots long); returns how many.
    std::size_t CopyOldestFirst(float* out) const;
    [[nodiscard]] float minimum() const;
    [[nodiscard]] float maximum() const;
    [[nodiscard]] float mean() const;

private:
    std::array<float, kSlots> slots_{};
    std::size_t next_ = 0U;
    std::size_t count_ = 0U;
};

}  // namespace repiu::engine

#endif  // REPIU_ENGINE_FRAME_RATE_HISTORY_H_
