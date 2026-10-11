#include "repiu/engine/jamma_input_timeline.h"

#include "repiu/engine/active_jamma_bindings.h"
#include <SDL3/SDL_keyboard.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <thread>

namespace repiu::engine {
namespace {

class TimelineLock {
public:
  explicit TimelineLock(std::atomic_flag *lock) : lock_(lock) {
    while (lock_->test_and_set(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
  }

  ~TimelineLock() { lock_->clear(std::memory_order_release); }

private:
  std::atomic_flag *lock_;
};

// Task 753: `REPIU_JAMMA_TIMELINE_TRACE=1` names each replay frame the age
// rule retires, which is how a frame the stack test cannot reach is found.
bool TimelineTraceEnabled() {
  static const bool enabled = []() {
    const char *const value = std::getenv("REPIU_JAMMA_TIMELINE_TRACE");
    return value != nullptr && value[0] != '0' && value[0] != 0;
  }();
  return enabled;
}

} // namespace

bool ResolveJammaReplayFrameEnd(const char *value) {
  return value == nullptr || value[0] != '0';
}

bool JammaReplayFrameEndEnabled() {
  static const bool enabled =
      ResolveJammaReplayFrameEnd(std::getenv("REPIU_JAMMA_REPLAY_FRAME_END"));
  return enabled;
}

void JammaInputTimeline::Reset(std::uint64_t timestamp_nanoseconds,
                                    std::uint16_t pressed_mask) {
  TimelineLock lock(&lock_);
  history_size_ = 0;
  history_floor_timestamp_ = timestamp_nanoseconds;
  history_floor_pressed_mask_ = pressed_mask;
  latest_pressed_mask_ = pressed_mask;
  history_peak_size_ = 0;
  due_head_ = 0;
  due_size_ = 0;
  last_due_timestamp_ = 0;
  has_due_timestamp_ = false;
  last_begun_timestamp_ = 0;
  replay_frame_depth_ = 0;
  edge_count_ = 0;
  history_pruned_count_ = 0;
  history_overflow_count_ = 0;
  history_coverage_miss_count_ = 0;
  due_enqueued_count_ = 0;
  due_overflow_count_ = 0;
  replay_begin_count_ = 0;
  replay_read_count_ = 0;
  replay_missing_due_count_ = 0;
  replay_frame_retire_count_ = 0;
  replay_frame_overflow_count_ = 0;
  replay_frame_end_count_ = 0;
  replay_frame_stale_count_ = 0;
  latest_state_read_count_ = 0;
}

void JammaInputTimeline::RecordStateLocked(
    std::uint64_t timestamp_nanoseconds, std::uint16_t pressed_mask) {
  if (pressed_mask == latest_pressed_mask_) {
    return;
  }
  if (history_size_ != 0U) {
    timestamp_nanoseconds =
        std::max(timestamp_nanoseconds,
                 history_[history_size_ - 1U].timestamp_nanoseconds);
  }
  PruneHistoryLocked();
  if (history_size_ == kHistoryCapacity) {
    history_floor_timestamp_ = history_[0].timestamp_nanoseconds;
    history_floor_pressed_mask_ = history_[0].pressed_mask;
    for (std::uint32_t index = 1U; index < history_size_; ++index) {
      history_[index - 1U] = history_[index];
    }
    --history_size_;
    ++history_overflow_count_;
  }
  history_[history_size_++] = {timestamp_nanoseconds, pressed_mask};
  history_peak_size_ = std::max(history_peak_size_, history_size_);
  latest_pressed_mask_ = pressed_mask;
  ++edge_count_;
}

void JammaInputTimeline::RecordKeyEdge(std::uint64_t timestamp_nanoseconds,
                                            JammaInputKey key, bool pressed) {
  if (key >= JammaInputKey::kCount) {
    return;
  }
  TimelineLock lock(&lock_);
  const std::uint16_t bit = JammaInputKeyMask(key);
  const std::uint16_t next =
      pressed ? static_cast<std::uint16_t>(latest_pressed_mask_ | bit)
              : static_cast<std::uint16_t>(latest_pressed_mask_ & ~bit);
  RecordStateLocked(timestamp_nanoseconds, next);
}

void JammaInputTimeline::RecordAllReleased(
    std::uint64_t timestamp_nanoseconds) {
  TimelineLock lock(&lock_);
  RecordStateLocked(timestamp_nanoseconds, 0U);
}

bool JammaInputTimeline::EnqueueTimerTick(
    std::uint64_t due_timestamp_nanoseconds) {
  TimelineLock lock(&lock_);
  if (due_size_ == kDueCapacity) {
    ++due_overflow_count_;
    return false;
  }
  const std::uint32_t tail = (due_head_ + due_size_) % kDueCapacity;
  due_timestamps_[tail] = due_timestamp_nanoseconds;
  ++due_size_;
  ++due_enqueued_count_;
  last_due_timestamp_ = due_timestamp_nanoseconds;
  has_due_timestamp_ = true;
  PruneHistoryLocked();
  return true;
}

void JammaInputTimeline::ClearTimerTicks() {
  TimelineLock lock(&lock_);
  due_head_ = 0;
  due_size_ = 0;
  replay_frame_depth_ = 0;
  PruneHistoryLocked();
}

bool JammaInputTimeline::BeginTimerInterrupt(
    std::uint32_t pre_interrupt_esp, std::uint32_t interrupt_frame_esp,
    std::uint32_t interrupted_eip, std::uint64_t* due_timestamp) {
  TimelineLock lock(&lock_);
  RetireReplayFramesLocked(pre_interrupt_esp);
  RetireStaleReplayFramesLocked();
  if (due_timestamp != nullptr) {
    *due_timestamp = 0U;
  }
  if (due_size_ == 0U) {
    ++replay_missing_due_count_;
    return false;
  }
  const std::uint64_t replay_timestamp = due_timestamps_[due_head_];
  if (due_timestamp != nullptr) {
    *due_timestamp = replay_timestamp;
  }
  last_begun_timestamp_ = replay_timestamp;
  due_head_ = (due_head_ + 1U) % kDueCapacity;
  --due_size_;
  if (replay_frame_depth_ == kDueCapacity) {
    for (std::uint32_t index = 1U; index < replay_frame_depth_; ++index) {
      replay_frames_[index - 1U] = replay_frames_[index];
    }
    --replay_frame_depth_;
    ++replay_frame_overflow_count_;
  }
  ++replay_begin_count_;
  replay_frames_[replay_frame_depth_++] = {replay_timestamp,
                                           interrupt_frame_esp,
                                           interrupted_eip,
                                           replay_begin_count_};
  PruneHistoryLocked();
  return true;
}

bool JammaInputTimeline::EndTimerInterrupt(
    std::uint32_t interrupt_frame_esp) {
  TimelineLock lock(&lock_);
  // From the top: the frame this return pops, and with it whatever nested
  // above it and never came to a stack test.
  for (std::uint32_t index = replay_frame_depth_; index != 0U; --index) {
    if (replay_frames_[index - 1U].interrupt_frame_esp !=
        interrupt_frame_esp) {
      continue;
    }
    replay_frame_end_count_ += replay_frame_depth_ - (index - 1U);
    replay_frame_depth_ = index - 1U;
    PruneHistoryLocked();
    return true;
  }
  return false;
}

void JammaInputTimeline::RetireReplayFramesLocked(
    std::uint32_t current_esp) {
  while (replay_frame_depth_ != 0U &&
         current_esp >
             replay_frames_[replay_frame_depth_ - 1U].interrupt_frame_esp) {
    --replay_frame_depth_;
    ++replay_frame_retire_count_;
  }
}

// Task 753. The stack test retires a frame only when the guest is seen above
// it. A tick delivered at a stack level the guest does not come back to --
// while a load ran in a function the game's loop is not called from, say --
// left its frame at the bottom for the rest of the run, and every read
// outside a handler was then answered with the keys as they were at that
// tick: pumpit8 read a SERVICE press 60 times a second for 35 seconds.
void JammaInputTimeline::RetireStaleReplayFramesLocked() {
  std::uint32_t kept = 0U;
  for (std::uint32_t index = 0; index < replay_frame_depth_; ++index) {
    const ReplayFrame frame = replay_frames_[index];
    if (replay_begin_count_ - frame.begin_index > kReplayFrameStaleBegins) {
      ++replay_frame_stale_count_;
      if (TimelineTraceEnabled()) {
        std::fprintf(stderr,
                     "[repiu-jamma-timeline] stale frame retired "
                     "frame_esp=0x%08X interrupted_eip=0x%08X begin=%llu "
                     "begins_since=%llu depth=%u\n",
                     static_cast<unsigned>(frame.interrupt_frame_esp),
                     static_cast<unsigned>(frame.interrupted_eip),
                     static_cast<unsigned long long>(frame.begin_index),
                     static_cast<unsigned long long>(replay_begin_count_ -
                                                     frame.begin_index),
                     static_cast<unsigned>(replay_frame_depth_));
      }
      continue;
    }
    replay_frames_[kept++] = frame;
  }
  replay_frame_depth_ = kept;
}

void JammaInputTimeline::PruneHistoryLocked() {
  if (history_size_ == 0U) {
    return;
  }

  std::uint64_t cutoff = 0U;
  bool has_cutoff = false;
  if (due_size_ != 0U) {
    cutoff = due_timestamps_[due_head_];
    has_cutoff = true;
  }
  for (std::uint32_t index = 0; index < replay_frame_depth_; ++index) {
    const std::uint64_t timestamp = replay_frames_[index].timestamp_nanoseconds;
    if (!has_cutoff || timestamp < cutoff) {
      cutoff = timestamp;
      has_cutoff = true;
    }
  }
  if (!has_cutoff && has_due_timestamp_) {
    cutoff = last_due_timestamp_;
    has_cutoff = true;
  }
  if (!has_cutoff) {
    return;
  }

  std::uint32_t remove_count = 0U;
  while (remove_count < history_size_ &&
         history_[remove_count].timestamp_nanoseconds <= cutoff) {
    ++remove_count;
  }
  if (remove_count == 0U) {
    return;
  }

  const HistoryEntry &floor = history_[remove_count - 1U];
  history_floor_timestamp_ = floor.timestamp_nanoseconds;
  history_floor_pressed_mask_ = floor.pressed_mask;
  for (std::uint32_t index = remove_count; index < history_size_; ++index) {
    history_[index - remove_count] = history_[index];
  }
  history_size_ -= remove_count;
  history_pruned_count_ += remove_count;
}

std::uint16_t
JammaInputTimeline::StateAtLocked(std::uint64_t timestamp_nanoseconds) {
  if (timestamp_nanoseconds < history_floor_timestamp_) {
    ++history_coverage_miss_count_;
  }
  std::uint16_t state = history_floor_pressed_mask_;
  for (std::uint32_t index = 0; index < history_size_; ++index) {
    if (history_[index].timestamp_nanoseconds > timestamp_nanoseconds) {
      break;
    }
    state = history_[index].pressed_mask;
  }
  return state;
}

bool JammaInputTimeline::TryReplayPressedMask(
    std::uint32_t current_esp, std::uint16_t *pressed_mask) {
  if (pressed_mask == nullptr) {
    return false;
  }
  TimelineLock lock(&lock_);
  RetireReplayFramesLocked(current_esp);
  RetireStaleReplayFramesLocked();
  PruneHistoryLocked();
  if (replay_frame_depth_ == 0U) {
    return false;
  }
  *pressed_mask = StateAtLocked(
      replay_frames_[replay_frame_depth_ - 1U].timestamp_nanoseconds);
  ++replay_read_count_;
  return true;
}

bool JammaInputTimeline::TryReplayTimestamp(
    std::uint32_t current_esp, std::uint64_t *timestamp_nanoseconds) {
  if (timestamp_nanoseconds == nullptr) {
    return false;
  }
  TimelineLock lock(&lock_);
  RetireReplayFramesLocked(current_esp);
  RetireStaleReplayFramesLocked();
  if (replay_frame_depth_ == 0U) {
    *timestamp_nanoseconds = last_begun_timestamp_;
    return false;
  }
  *timestamp_nanoseconds =
      replay_frames_[replay_frame_depth_ - 1U].timestamp_nanoseconds;
  return true;
}

void JammaInputTimeline::ServeLiveReadsFromLatestState(bool enabled) {
  serve_live_reads_.store(enabled, std::memory_order_relaxed);
}

bool JammaInputTimeline::TryLatestPressedMask(std::uint16_t *pressed_mask) {
  if (pressed_mask == nullptr ||
      !serve_live_reads_.load(std::memory_order_relaxed)) {
    return false;
  }
  TimelineLock lock(&lock_);
  *pressed_mask = latest_pressed_mask_;
  ++latest_state_read_count_;
  return true;
}

JammaInputTimelineSnapshot JammaInputTimeline::Snapshot() const {
  // Called only after the guest thread has stopped. It deliberately takes no
  // lock: timeout teardown may terminate the guest while it owns the spin
  // guard, and diagnostics must remain readable in that case.
  JammaInputTimelineSnapshot result;
  result.edge_count = edge_count_;
  result.history_pruned_count = history_pruned_count_;
  result.history_overflow_count = history_overflow_count_;
  result.history_coverage_miss_count = history_coverage_miss_count_;
  result.due_enqueued_count = due_enqueued_count_;
  result.due_overflow_count = due_overflow_count_;
  result.replay_begin_count = replay_begin_count_;
  result.replay_read_count = replay_read_count_;
  result.replay_missing_due_count = replay_missing_due_count_;
  result.replay_frame_retire_count = replay_frame_retire_count_;
  result.replay_frame_overflow_count = replay_frame_overflow_count_;
  result.replay_frame_end_count = replay_frame_end_count_;
  result.replay_frame_stale_count = replay_frame_stale_count_;
  result.latest_state_read_count = latest_state_read_count_;
  result.history_size = history_size_;
  result.history_peak_size = history_peak_size_;
  result.due_size = due_size_;
  result.replay_frame_depth = replay_frame_depth_;
  return result;
}

std::uint16_t CaptureKeyboardJammaPressedMask() {
  const repiu::input::ResolvedJammaBindings &bindings = ActiveJammaBindings();

  // Hoisted out of the loop below, and skipped entirely unless some alias
  // actually needs it. With the default configuration nothing does, so this
  // costs exactly what it did before configuration existed.
  const SDL_Keymod modifier_state = bindings.any_binding_uses_modifiers
                                        ? SDL_GetModState()
                                        : SDL_KMOD_NONE;

  // As on the port I/O path: SDL owns the array for the life of the process
  // and documents reading it from any thread.
  static const bool *const key_state = SDL_GetKeyboardState(nullptr);

  std::uint16_t pressed_mask = 0U;
  for (std::uint32_t index = 0; index < repiu::input::kJammaInputKeyCount;
       ++index) {
    const repiu::input::JammaInputBinding &binding = bindings.inputs[index];
    for (std::uint32_t slot = 0; slot < binding.alias_count; ++slot) {
      const repiu::input::HostKeyAlias &alias = binding.aliases[slot];
      if (alias.scancode == SDL_SCANCODE_UNKNOWN) {
        continue;
      }
      if (!key_state[alias.scancode] ||
          !alias.ModifiersMatch(modifier_state)) {
        continue;
      }
      pressed_mask = static_cast<std::uint16_t>(
          pressed_mask |
          JammaInputKeyMask(static_cast<JammaInputKey>(index)));
      break;
    }
  }
  return pressed_mask;
}

std::uint16_t CaptureCurrentJammaPressedMask() {
  return static_cast<std::uint16_t>(CaptureKeyboardJammaPressedMask() |
                                    PublishedJammaPadMask());
}

} // namespace repiu::engine
