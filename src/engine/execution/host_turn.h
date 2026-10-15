#pragma once

// Issue #64. One turn of the host's work: the Glide command pump and the
// window's events, the PIT tick schedule, the progress and stall watch, the
// timeout, and the census and telemetry cadences. It used to be the body of
// PollThreadUntilExit's loop.
//
// Two loops take these turns. PollThreadUntilExit, on a model with a guest
// thread, repeats "turn, then wait". The one-thread loop of the emulated model
// (RunsHostOnOneThread) repeats "guest slice, then turn", because the browser's
// single Worker has no other thread to run the host on. Both call the same
// turn, so the tick schedule and the watch rules are the same in both shapes.
// See docs/design/20261015-i064-browser-host-single-thread-loop.md.

#include "telemetry/live_telemetry_snapshot.h"
#include "thread_context.h"

#include "repiu/platform/host_thread.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>

namespace repiu::engine
{

class HostTurn
{
public:
    // `guest_thread` is the thread the guest runs on, or null when it runs on
    // this one. Without a thread there is nothing to ask whether it exited and
    // nothing to sample from outside, so those parts of the turn are skipped.
    HostTurn(const repiu::platform::HostThread* guest_thread,
             std::uint32_t timeout_milliseconds,
             std::uint32_t stall_timeout_milliseconds,
             ThreadContext* progress_context,
             ThreadContext* host_context,
             bool* stall_timed_out);
    ~HostTurn();

    HostTurn(const HostTurn&) = delete;
    HostTurn& operator=(const HostTurn&) = delete;

    // One turn. Empty to go on, otherwise what the run came to; `exit_code` is
    // filled in when the guest thread exited.
    [[nodiscard]] std::optional<HostPollOutcome> Take(std::uint32_t* exit_code);

    // Whether a tick is owed or the next one is within a millisecond, which is
    // when the threaded loop does not wait between turns.
    [[nodiscard]] bool TimerDeadlineNear() const;

    // The time to the next PIT tick, as of the last turn.
    [[nodiscard]] std::uint64_t NanosecondsUntilNextTick() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Issue #64. The one-thread loop as the emulated CPU sees it: between stops it
// asks whether the host's turn has come, and takes it.
class OneThreadHostLoop
{
public:
    explicit OneThreadHostLoop(HostTurn* turn);

    // Whether the host's turn has come: the next tick's due time passed, the
    // gap cap between turns reached, or a yield asked for (YieldToHost).
    [[nodiscard]] bool TurnDue(const ThreadContext& context) const;

    // Takes the turn. False when the run must stop; outcome() says why.
    [[nodiscard]] bool TakeTurn(ThreadContext* context);

    [[nodiscard]] std::optional<HostPollOutcome> outcome() const
    {
        return outcome_;
    }

    [[nodiscard]] std::uint64_t turns() const
    {
        return turns_;
    }

    [[nodiscard]] std::uint64_t yields() const
    {
        return yields_;
    }

    // Issue #72: how many times the thread went back to the browser page's
    // event loop (none outside a browser).
    [[nodiscard]] std::uint64_t page_yields() const
    {
        return page_yields_;
    }

private:
    void YieldToPageIfDue(const ThreadContext& context);

    HostTurn* turn_;
    std::chrono::steady_clock::time_point next_turn_;
    std::optional<HostPollOutcome> outcome_;
    std::uint64_t turns_ = 0U;
    std::uint64_t yields_ = 0U;
    std::chrono::steady_clock::time_point last_page_yield_;
    std::uint64_t frames_at_page_yield_ = 0U;
    std::uint64_t page_yields_ = 0U;
};

// Issue #64. Runs the guest on this thread through `entry` (the guest thread
// procedure the threaded model would start), taking host turns between its
// slices. What PollThreadUntilExit answers for a guest thread, answered for
// this one: kGuestThreadExited with the procedure's exit code when the guest
// ended by itself, otherwise the turn's outcome.
[[nodiscard]] HostPollOutcome RunGuestOnThisThread(
    repiu::platform::HostThreadEntry entry,
    ThreadContext* context,
    std::uint32_t timeout_milliseconds,
    std::uint32_t stall_timeout_milliseconds,
    ThreadContext* progress_context,
    std::uint32_t* exit_code,
    bool* stall_timed_out);

// Issue #64. What guest-side code does when it has to wait for the host: on a
// model with a host thread it sleeps for `milliseconds`, as it did before; on
// one thread it asks for the host's turn instead, because the host only runs
// once the guest's slice ends.
void YieldToHost(ThreadContext* context, std::chrono::milliseconds milliseconds);

}  // namespace repiu::engine
