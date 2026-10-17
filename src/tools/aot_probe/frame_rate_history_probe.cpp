#include "frame_rate_history_probe.h"

#include "repiu/engine/frame_rate_history.h"

#include <cmath>
#include <iostream>

namespace repiu::tools
{
namespace
{

bool Near(float actual, float expected)
{
    return std::fabs(actual - expected) < 1.0e-3F;
}

}  // namespace

// Issue #88. The OSD graph's record: one slot per one-second measurement, a
// load's long measurement spread over its seconds, the oldest dropped past
// 60, and the minimum, mean and maximum over what is kept.
bool RunFrameRateHistoryProbe()
{
    using engine::FrameRateHistory;

    FrameRateHistory empty;
    const bool empty_ok = empty.count() == 0U && Near(empty.latest(), 0.0F) &&
        Near(empty.minimum(), 0.0F) && Near(empty.mean(), 0.0F);

    FrameRateHistory history;
    history.Record(60.0F, 1.0);
    history.Record(30.0F, 1.02);
    float values[FrameRateHistory::kSlots] = {};
    const bool order_ok = history.count() == 2U &&
        history.CopyOldestFirst(values) == 2U && Near(values[0], 60.0F) &&
        Near(values[1], 30.0F) && Near(history.latest(), 30.0F) &&
        Near(history.minimum(), 30.0F) && Near(history.maximum(), 60.0F) &&
        Near(history.mean(), 45.0F);

    // A 10-second load with one swap: ten slots of 0.1.
    history.Record(0.1F, 10.0);
    const bool gap_ok = history.count() == 12U && Near(history.latest(), 0.1F) &&
        Near(history.minimum(), 0.1F);

    // Past 60 the oldest go; a gap longer than the graph fills it once.
    for (int i = 0; i < 70; ++i)
    {
        history.Record(59.0F + static_cast<float>(i % 3), 1.0);
    }
    history.CopyOldestFirst(values);
    const bool wrap_ok = history.count() == FrameRateHistory::kSlots &&
        Near(history.minimum(), 59.0F) && Near(history.maximum(), 61.0F) &&
        Near(values[FrameRateHistory::kSlots - 1U], history.latest());
    FrameRateHistory long_gap;
    long_gap.Record(0.0F, 500.0);
    long_gap.Record(NAN, NAN);
    const bool long_ok = long_gap.count() == FrameRateHistory::kSlots &&
        Near(long_gap.maximum(), 0.0F);

    const bool all = empty_ok && order_ok && gap_ok && wrap_ok && long_ok;
    std::cout << "frame_rate_history_empty=" << (empty_ok ? "true" : "false")
              << "\nframe_rate_history_order=" << (order_ok ? "true" : "false")
              << "\nframe_rate_history_gap=" << (gap_ok ? "true" : "false")
              << "\nframe_rate_history_wrap=" << (wrap_ok ? "true" : "false")
              << "\nframe_rate_history_long_gap=" << (long_ok ? "true" : "false")
              << "\nframe_rate_history_all=" << (all ? "true" : "false") << '\n';
    return all;
}

}  // namespace repiu::tools
