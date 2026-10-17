#include "repiu/engine/frame_rate_history.h"

#include <algorithm>
#include <cmath>

namespace repiu::engine
{

void FrameRateHistory::Record(const float frames_per_second, const double seconds)
{
    const float value = std::isfinite(frames_per_second) && frames_per_second > 0.0F
        ? frames_per_second : 0.0F;
    // At least one slot; a long gap fills the whole graph and no more.
    double rounded = std::isfinite(seconds) ? std::floor(seconds + 0.5) : 1.0;
    rounded = std::clamp(rounded, 1.0, static_cast<double>(kSlots));
    const std::size_t slots = static_cast<std::size_t>(rounded);
    for (std::size_t i = 0U; i < slots; ++i)
    {
        slots_[next_] = value;
        next_ = (next_ + 1U) % kSlots;
        count_ = std::min(count_ + 1U, kSlots);
    }
}

float FrameRateHistory::latest() const
{
    return count_ == 0U ? 0.0F : slots_[(next_ + kSlots - 1U) % kSlots];
}

std::size_t FrameRateHistory::CopyOldestFirst(float* const out) const
{
    if (out == nullptr)
    {
        return 0U;
    }
    const std::size_t first = (next_ + kSlots - count_) % kSlots;
    for (std::size_t i = 0U; i < count_; ++i)
    {
        out[i] = slots_[(first + i) % kSlots];
    }
    return count_;
}

float FrameRateHistory::minimum() const
{
    float values[kSlots];
    const std::size_t n = CopyOldestFirst(values);
    return n == 0U ? 0.0F : *std::min_element(values, values + n);
}

float FrameRateHistory::maximum() const
{
    float values[kSlots];
    const std::size_t n = CopyOldestFirst(values);
    return n == 0U ? 0.0F : *std::max_element(values, values + n);
}

float FrameRateHistory::mean() const
{
    float values[kSlots];
    const std::size_t n = CopyOldestFirst(values);
    if (n == 0U)
    {
        return 0.0F;
    }
    double sum = 0.0;
    for (std::size_t i = 0U; i < n; ++i)
    {
        sum += values[i];
    }
    return static_cast<float>(sum / static_cast<double>(n));
}

}  // namespace repiu::engine
