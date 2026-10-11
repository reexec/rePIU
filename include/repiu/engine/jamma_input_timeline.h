#ifndef REPIU_ENGINE_JAMMA_INPUT_TIMELINE_H_
#define REPIU_ENGINE_JAMMA_INPUT_TIMELINE_H_

#include "repiu/input/jamma_input_key.h"

#include <cstdint>
#include <atomic>

namespace repiu::engine
{

// The enumeration moved to repiu/input/jamma_input_key.h so the configuration
// layer, which is platform neutral, can name the same inputs. These aliases
// keep the win32 call sites reading as they did.
using repiu::input::JammaInputKey;
using repiu::input::JammaInputKeyMask;

struct JammaInputTimelineSnapshot
{
    std::uint64_t edge_count = 0;
    std::uint64_t history_pruned_count = 0;
    std::uint64_t history_overflow_count = 0;
    std::uint64_t history_coverage_miss_count = 0;
    std::uint64_t due_enqueued_count = 0;
    std::uint64_t due_overflow_count = 0;
    std::uint64_t replay_begin_count = 0;
    std::uint64_t replay_read_count = 0;
    std::uint64_t replay_missing_due_count = 0;
    std::uint64_t replay_frame_retire_count = 0;
    std::uint64_t replay_frame_overflow_count = 0;
    // Task 753. How a replay frame left other than by the stack test: by its
    // handler's return, or by age. And the reads the latest recorded state
    // answered because a scripted keyboard asked it to.
    std::uint64_t replay_frame_end_count = 0;
    std::uint64_t replay_frame_stale_count = 0;
    std::uint64_t latest_state_read_count = 0;
    std::uint32_t history_size = 0;
    std::uint32_t history_peak_size = 0;
    std::uint32_t due_size = 0;
    std::uint32_t replay_frame_depth = 0;
};

class JammaInputTimeline
{
public:
    static constexpr std::uint32_t kHistoryCapacity = 256U;
    static constexpr std::uint32_t kDueCapacity = 64U;
    // Task 753. A replay frame that this many later interrupts have begun
    // after is no handler still running; it is one the stack test never got
    // to retire. 64 is the tick backlog's bound, so a whole backlog draining
    // inside one handler would still not reach it.
    static constexpr std::uint64_t kReplayFrameStaleBegins = 64U;

    void Reset(std::uint64_t timestamp_nanoseconds,
               std::uint16_t pressed_mask);
    void RecordKeyEdge(std::uint64_t timestamp_nanoseconds,
                       JammaInputKey key,
                       bool pressed);
    void RecordAllReleased(std::uint64_t timestamp_nanoseconds);

    bool EnqueueTimerTick(std::uint64_t due_timestamp_nanoseconds);
    void ClearTimerTicks();
    // Issue #58: `due_timestamp`, when given, receives the due time of the
    // tick this interrupt carries (0 when none was queued).
    bool BeginTimerInterrupt(std::uint32_t pre_interrupt_esp,
                             std::uint32_t interrupt_frame_esp,
                             std::uint32_t interrupted_eip = 0U,
                             std::uint64_t* due_timestamp = nullptr);
    // Task 753. The handler's return, seen where the engine executes the
    // `iret` itself: `interrupt_frame_esp` is ESP at that `iret`. Only a
    // return popping a frame an interrupt pushed ends anything, so the `iret`
    // of a chained handler or of a software interrupt is ignored. Returns
    // whether a frame ended.
    bool EndTimerInterrupt(std::uint32_t interrupt_frame_esp);
    bool TryReplayPressedMask(std::uint32_t current_esp,
                              std::uint16_t* pressed_mask);
    // Issue #58. The due time of the tick whose handler is running at
    // `current_esp`, by the same frame rules as the read above, for other
    // devices the handler samples. Outside every handler it returns false
    // and gives the due time of the latest tick delivered (0 before any):
    // the guest has seen time up to there and no further.
    bool TryReplayTimestamp(std::uint32_t current_esp,
                            std::uint64_t* timestamp_nanoseconds);
    // Task 753. A scripted keyboard pushes events, which SDL's keyboard state
    // array does not follow, so with one running a read outside every handler
    // is answered from the latest recorded state instead.
    void ServeLiveReadsFromLatestState(bool enabled);
    bool TryLatestPressedMask(std::uint16_t* pressed_mask);

    JammaInputTimelineSnapshot Snapshot() const;

private:
    struct HistoryEntry
    {
        std::uint64_t timestamp_nanoseconds = 0;
        std::uint16_t pressed_mask = 0;
    };

    struct ReplayFrame
    {
        std::uint64_t timestamp_nanoseconds = 0;
        std::uint32_t interrupt_frame_esp = 0;
        std::uint32_t interrupted_eip = 0;
        std::uint64_t begin_index = 0;
    };

    std::uint16_t StateAtLocked(std::uint64_t timestamp_nanoseconds);
    void RecordStateLocked(std::uint64_t timestamp_nanoseconds,
                           std::uint16_t pressed_mask);
    void RetireReplayFramesLocked(std::uint32_t current_esp);
    void RetireStaleReplayFramesLocked();
    void PruneHistoryLocked();

    std::atomic_flag lock_ = ATOMIC_FLAG_INIT;
    HistoryEntry history_[kHistoryCapacity] = {};
    std::uint32_t history_size_ = 0;
    std::uint64_t history_floor_timestamp_ = 0;
    std::uint16_t history_floor_pressed_mask_ = 0;
    std::uint16_t latest_pressed_mask_ = 0;
    std::uint32_t history_peak_size_ = 0;

    std::uint64_t due_timestamps_[kDueCapacity] = {};
    std::uint32_t due_head_ = 0;
    std::uint32_t due_size_ = 0;
    std::uint64_t last_due_timestamp_ = 0;
    std::uint64_t last_begun_timestamp_ = 0;
    bool has_due_timestamp_ = false;

    ReplayFrame replay_frames_[kDueCapacity] = {};
    std::uint32_t replay_frame_depth_ = 0;

    std::uint64_t edge_count_ = 0;
    std::uint64_t history_pruned_count_ = 0;
    std::uint64_t history_overflow_count_ = 0;
    std::uint64_t history_coverage_miss_count_ = 0;
    std::uint64_t due_enqueued_count_ = 0;
    std::uint64_t due_overflow_count_ = 0;
    std::uint64_t replay_begin_count_ = 0;
    std::uint64_t replay_read_count_ = 0;
    std::uint64_t replay_missing_due_count_ = 0;
    std::uint64_t replay_frame_retire_count_ = 0;
    std::uint64_t replay_frame_overflow_count_ = 0;
    std::uint64_t replay_frame_end_count_ = 0;
    std::uint64_t replay_frame_stale_count_ = 0;
    std::uint64_t latest_state_read_count_ = 0;
    std::atomic<bool> serve_live_reads_{false};
};

// Task 753. `REPIU_JAMMA_REPLAY_FRAME_END=0` leaves replay frames to the
// stack test and the age rule, as before the handler's return was used.
bool ResolveJammaReplayFrameEnd(const char* value);
bool JammaReplayFrameEndEnabled();

// The inputs held right now: the keyboard part alone, and (issue #34) the
// keyboard together with the published gamepad and joystick mask.
std::uint16_t CaptureKeyboardJammaPressedMask();
std::uint16_t CaptureCurrentJammaPressedMask();

}  // namespace repiu::engine

#endif  // REPIU_ENGINE_JAMMA_INPUT_TIMELINE_H_
