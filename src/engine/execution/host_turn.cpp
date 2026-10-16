#include "host_turn.h"

#include "boundary/timer_interrupt_boundary.h"
#include "execution/execution_internal.h"
#include "repiu/engine/timer_latency_trace.h"
#include "repiu/runtime/execution_model.h"
#include "repiu/runtime/execution_timeout.h"

#include "repiu/engine/browser_audio_mixer.h"
#include "repiu/platform/browser_host.h"
#include "repiu/platform/host_error_stream.h"
#include "repiu/platform/host_telemetry.h"
#include "repiu/platform/host_time.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <thread>

namespace repiu::engine
{
namespace
{

// Task 503d-19: derived from what the clock returns rather than spelled
// DWORD. Every variable below that holds a tick follows it, which is what
// took thirty-odd errors off the Linux measurement at once.
using TickCount = decltype(repiu::platform::MillisecondTicks());

// Sample the guest thread only after full exception dispatches have been
// silent for a full second: dispatch-active phases already report their
// location, and the observation target is the zero-dispatch native state.
// The composite `progressed` tracker is unsuitable as this gate because
// lightweight AOT VEH paths (inline-cache misses, reentries) increment
// aot_boundary/aot_reentry continuously without any dispatch.
constexpr TickCount kNativePhaseSampleQuietMilliseconds = 1000U;
constexpr TickCount kNativePhaseSampleIntervalMilliseconds = 500U;

// Issue #64. The longest the guest runs on one thread before the host takes a
// turn, whatever the tick schedule says, so that the window's events, the
// Glide window and the audio pumps are served at least this often. Design
// decision 3 starts it at 2 ms; the cost of a turn is measured against it.
constexpr std::chrono::microseconds kOneThreadTurnGapCap{2000};

void SaturatingAtomicAdd(std::atomic<std::uint32_t>* value,
                         std::uint64_t increment)
{
    std::uint32_t current = value->load(std::memory_order_relaxed);
    while (current != std::numeric_limits<std::uint32_t>::max())
    {
        const std::uint64_t room =
            std::numeric_limits<std::uint32_t>::max() - current;
        const std::uint32_t added = static_cast<std::uint32_t>(
            std::min(increment, room));
        const std::uint32_t next = current + added;
        if (value->compare_exchange_weak(
                current, next,
                std::memory_order_release,
                std::memory_order_relaxed))
        {
            return;
        }
    }
}

}  // namespace

// What PollThreadUntilExit kept in locals across its iterations, kept here
// across turns. The names are the locals' own, so the turn below reads as the
// loop body it was.
struct HostTurn::Impl
{
    const repiu::platform::HostThread* guest_thread = nullptr;
    std::uint32_t timeout_milliseconds = 0U;
    std::uint32_t stall_timeout_milliseconds = 0U;
    ThreadContext* progress_context = nullptr;
    ThreadContext* host_context = nullptr;
    bool* stall_timed_out = nullptr;

    TickCount start_tick = 0;
    std::uint64_t event_clock_start = 0U;
    repiu::hle::PitIrqSchedule pit_irq_schedule;
    std::uint64_t next_timer_wait_nanoseconds = 0U;
    std::uint64_t bios_tick_count = 0;
    TickCount quiet_start_tick = 0;
    runtime::ExecutionProgressSnapshot last_progress_snapshot;
    bool native_sampling_enabled = true;
    NativePhaseSamplerState native_sampler_state;
    TickCount position_census_interval = 0;
    TickCount last_position_census_tick = 0;
    TickCount cd_audio_census_interval = 0;
    TickCount last_cd_audio_census_tick = 0;
    std::uint32_t last_cd_audio_worker_iterations = 0;
    // Task 430: the same differencing for timer tick delivery, so each sample
    // carries the loss over its own interval instead of a run-long average that
    // cannot say whether the loss was in gameplay or in the attract demo.
    std::uint32_t last_cd_audio_ticks_due = 0;
    std::uint32_t last_cd_audio_ticks_injected = 0;
    std::uint32_t last_cd_audio_safe_point_traps = 0;
    TickCount piu10_mp3_census_interval = 0;
    TickCount last_piu10_mp3_census_tick = 0;
    std::uint32_t loader_module_base = 0;
    std::uint32_t loader_module_size = 0;
    TickCount dispatch_quiet_start_tick = 0;
    std::uint32_t last_dispatch_total = 0;
    TickCount quiet_iterations = 0;
    TickCount last_live_snapshot_tick = 0;
    TickCount iteration = 0;

    std::uint64_t ReadEventClock() const
    {
        if (host_context != nullptr)
        {
            return host_context->glide_backend.EventClockNanoseconds();
        }
        return static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
    }

    // Task 750. What the loop did inline every iteration, as a callable the
    // guest thread can run too while it waits a swap out (see
    // `ThreadContext::timer_tick_arm`). The schedule is stateful, so both
    // callers hold `timer_tick_arm_mutex`.
    void ArmDueTimerTicks()
    {
        if (progress_context == nullptr)
        {
            return;
        }
        const std::uint64_t current_event_clock = ReadEventClock();
        const std::uint64_t elapsed_nanoseconds =
            current_event_clock >= event_clock_start
                ? current_event_clock - event_clock_start : 0U;
        bios_tick_count = repiu::hle::PitTickCountForElapsed(
            elapsed_nanoseconds,
            repiu::hle::PitChannel0::kDefaultDivisor);
        repiu::runtime::WriteDosLowMemory(
            &progress_context->dos_low_memory,
            0x046CU,
            static_cast<std::uint32_t>(bios_tick_count),
            4U);

        repiu::hle::PitIrqDueRange due_range;
        const std::uint64_t due_interrupts = pit_irq_schedule.Poll(
            progress_context->pit_channel0.snapshot(),
            elapsed_nanoseconds, &due_range);
        next_timer_wait_nanoseconds =
            pit_irq_schedule.NanosecondsUntilNextTick(
                progress_context->pit_channel0.snapshot(),
                elapsed_nanoseconds);
        if (due_interrupts != 0U)
        {
            // Task 366: recorded before arming, so the timestamp queue
            // below mirrors what the backlog accepted.
            // Task 431: whether the guest thread is inside the Glide gate
            // right now -- that window runs no guest code, so no safe point
            // is reachable until the gate returns.
            {
                TimerTickDeliveryGuard timer_guard(
                    &progress_context->timer_tick_delivery_lock);
                const std::uint32_t accepted = RecordTimerTicksDue(
                    &progress_context->timer_tick_delivery,
                    static_cast<std::uint32_t>(due_interrupts),
                    host_context != nullptr &&
                        host_context->glide_backend.guest_in_glide_gate());
                for (std::uint32_t index = 0; index < accepted; ++index)
                {
                    const std::uint64_t ordinal =
                        due_range.first_tick_ordinal + index;
                    const std::uint64_t due_ns =
                        event_clock_start + due_range.epoch_nanoseconds +
                        repiu::hle::PitElapsedNanosecondsForTick(
                            ordinal, due_range.divisor);
                    progress_context->jamma_input_timeline.EnqueueTimerTick(
                        due_ns);
                    if (TimerLatencyTraceEnabled())
                    {
                        RecordTimerTrace(
                            TimerTraceEvent::kDue, due_ns,
                            static_cast<std::uint32_t>(ordinal), 0U,
                            ReadEventClock());
                    }
                }
                progress_context->timer_interrupt_pending.store(
                    true, std::memory_order_release);
            }
            SaturatingAtomicAdd(
                &progress_context->last_timer_injection_ticks,
                due_interrupts);
            if (progress_context->aot_placement != nullptr &&
                progress_context->aot_placement
                    ->timer_source_profile.enabled)
            {
                SaturatingAtomicAdd(
                    &progress_context->timer_interrupt_due_ticks,
                    due_interrupts);
            }
            ArmAotTimerSafePoint(progress_context);
        }
    }

    std::optional<HostPollOutcome> Take(std::uint32_t* exit_code);
};

HostTurn::HostTurn(const repiu::platform::HostThread* guest_thread,
                   const std::uint32_t timeout_milliseconds,
                   const std::uint32_t stall_timeout_milliseconds,
                   ThreadContext* progress_context,
                   ThreadContext* host_context,
                   bool* stall_timed_out)
    : impl_(std::make_unique<Impl>())
{
    Impl& turn = *impl_;
    turn.guest_thread = guest_thread;
    turn.timeout_milliseconds = timeout_milliseconds;
    turn.stall_timeout_milliseconds = stall_timeout_milliseconds;
    turn.progress_context = progress_context;
    turn.host_context = host_context;
    turn.stall_timed_out = stall_timed_out;
    if (stall_timed_out != nullptr)
    {
        *stall_timed_out = false;
    }
    turn.start_tick = repiu::platform::MillisecondTicks();
    turn.event_clock_start = turn.ReadEventClock();
    turn.quiet_start_tick = turn.start_tick;
    if (progress_context != nullptr)
    {
        turn.last_progress_snapshot.diagnostic =
            progress_context->diagnostic_progress_count.load(
                std::memory_order_relaxed);
        turn.last_progress_snapshot.single_step =
            progress_context->single_step_trace_count.load(
                std::memory_order_relaxed);
        turn.last_progress_snapshot.aot =
            progress_context->aot_boundary_count.load(
                std::memory_order_relaxed) +
            progress_context->aot_reentry_count.load(
                std::memory_order_relaxed);
        turn.last_progress_snapshot.glide_direct =
            progress_context->aot_dbt_glide_dispatch_entry_count.load(
                std::memory_order_relaxed);
    }

    const char* sampling_environment =
        std::getenv("REPIU_NATIVE_SAMPLING");
    turn.native_sampling_enabled =
        sampling_environment == nullptr ||
        std::strcmp(sampling_environment, "0") != 0;
    // Task 411: independent of the native sampler's cadence and of its gate.
    turn.position_census_interval = static_cast<TickCount>(
        GuestPositionCensusIntervalMilliseconds());
    turn.last_position_census_tick = turn.start_tick;
    // Task 421: the music position on its own cadence, sampled here rather than
    // in the audio worker so that a worker which is not running still produces
    // a reading — its absence is the measurement.
    turn.cd_audio_census_interval = static_cast<TickCount>(
        CdAudioPositionCensusIntervalMilliseconds());
    turn.last_cd_audio_census_tick = turn.start_tick;
    // Task 740. The PIU10 MP3 pipeline on its own cadence, for the same reason
    // as the CD census: what the guest feeds, what the decoder has played and
    // how the frame-sync toggles arrive, on one time axis with the ticks.
    turn.piu10_mp3_census_interval = [] {
        const char* const value = std::getenv("REPIU_PIU10_MP3_CENSUS_MS");
        if (value == nullptr || *value == '\0')
        {
            return static_cast<TickCount>(0);
        }
        char* end = nullptr;
        const unsigned long parsed = std::strtoul(value, &end, 10);
        return static_cast<TickCount>(
            end != value && *end == '\0' && parsed != 0UL && parsed <= 1000UL
                ? parsed : 0UL);
    }();
    turn.last_piu10_mp3_census_tick = turn.start_tick;
    // Task 412: the loader's own image range, so a host sample can name the
    // call site that led into the kernel. Resolved once; the sampling path only
    // compares against it.
    repiu::platform::ReadHostImageRange(&turn.loader_module_base,
                                        &turn.loader_module_size);
    turn.dispatch_quiet_start_tick = turn.start_tick;
    if (progress_context != nullptr)
    {
        turn.last_dispatch_total =
            progress_context->exception_dispatch_entry_count.load(
                std::memory_order_relaxed) +
            progress_context->exception_dispatch_exit_count.load(
                std::memory_order_relaxed);
    }
    turn.last_live_snapshot_tick = turn.start_tick;
    if (progress_context != nullptr)
    {
        progress_context->live_telemetry_phase.store(
            1,
            std::memory_order_relaxed);
        WriteLiveTelemetrySnapshot(*progress_context, 0, 0);
        Impl* const arm = impl_.get();
        std::lock_guard<std::mutex> lock(progress_context->timer_tick_arm_mutex);
        progress_context->timer_tick_arm = [arm]() { arm->ArmDueTimerTicks(); };
    }
}

HostTurn::~HostTurn()
{
    if (impl_->progress_context != nullptr)
    {
        std::lock_guard<std::mutex> lock(
            impl_->progress_context->timer_tick_arm_mutex);
        impl_->progress_context->timer_tick_arm = nullptr;
    }
}

std::optional<HostPollOutcome> HostTurn::Take(std::uint32_t* exit_code)
{
    const std::optional<HostPollOutcome> outcome = impl_->Take(exit_code);
    ++impl_->iteration;
    return outcome;
}

bool HostTurn::TimerDeadlineNear() const
{
    // The normal 1ms wait is cheap when the next PIT edge is distant, but
    // it adds avoidable phase jitter near a 240Hz edge. Use a nonblocking
    // command pump in the final millisecond so the next loop observes the
    // edge at high resolution. A pending tick gets the same treatment so
    // its safe-point delivery is not delayed by the coarse wait.
    constexpr std::uint64_t kTimerDeadlineSpinWindowNanoseconds =
        1000000ULL;
    return impl_->progress_context != nullptr &&
        (impl_->progress_context->timer_interrupt_pending.load(
             std::memory_order_acquire) ||
         impl_->next_timer_wait_nanoseconds <=
             kTimerDeadlineSpinWindowNanoseconds);
}

std::uint64_t HostTurn::NanosecondsUntilNextTick() const
{
    return impl_->next_timer_wait_nanoseconds;
}

std::optional<HostPollOutcome> HostTurn::Impl::Take(std::uint32_t* exit_code)
{
    if (host_context != nullptr)
    {
        host_context->glide_backend.PumpHostCommands();
        host_context->glide_backend.PumpEvents();
        if (host_context->glide_backend.exit_requested())
        {
            if (progress_context != nullptr)
            {
                WriteLiveTelemetrySnapshot(
                    *progress_context,
                    repiu::platform::MillisecondTicks() - start_tick,
                    iteration + 1);
            }
            return HostPollOutcome::kHostExitRequested;
        }
    }
    if (progress_context != nullptr)
    {
        progress_context->diagnostic_poll_iteration_count =
            iteration + 1;

        {
            std::lock_guard<std::mutex> arm_lock(
                progress_context->timer_tick_arm_mutex);
            ArmDueTimerTicks();
        }
    }
    // Task 503d-18: one question, asked without waiting. What stood here
    // read GetExitCodeThread and compared against STILL_ACTIVE, which is
    // also a legal exit code -- a guest that returned 259 would have kept
    // this loop spinning until the timeout.
    // Issue #64: a guest on this thread ends by returning to its loop, which
    // answers this itself.
    if (guest_thread != nullptr)
    {
        const repiu::platform::HostThreadStatus status =
            repiu::platform::QueryHostThread(*guest_thread);
        if (!status.running)
        {
            if (exit_code != nullptr)
            {
                *exit_code = status.exit_code;
            }
            return HostPollOutcome::kGuestThreadExited;
        }
    }

    bool progressed = false;
    if (progress_context != nullptr)
    {
        runtime::ExecutionProgressSnapshot current_progress_snapshot;
        current_progress_snapshot.diagnostic =
            progress_context->diagnostic_progress_count.load(
                std::memory_order_relaxed);
        current_progress_snapshot.single_step =
            progress_context->single_step_trace_count.load(
                std::memory_order_relaxed);
        current_progress_snapshot.aot =
            progress_context->aot_boundary_count.load(
                std::memory_order_relaxed) +
            progress_context->aot_reentry_count.load(
                std::memory_order_relaxed);
        current_progress_snapshot.glide_direct =
            progress_context->aot_dbt_glide_dispatch_entry_count.load(
                std::memory_order_relaxed);
        progressed = runtime::HasExecutionProgress(
            last_progress_snapshot, current_progress_snapshot);
        last_progress_snapshot = current_progress_snapshot;
    }
    if (progressed)
    {
        quiet_iterations = 0;
        quiet_start_tick = repiu::platform::MillisecondTicks();
    }
    else
    {
        ++quiet_iterations;
    }
    if (progress_context != nullptr)
    {
        progress_context->diagnostic_quiet_iteration_count =
            quiet_iterations;
    }

    if (stall_timeout_milliseconds !=
            repiu::runtime::kWaitForeverMilliseconds &&
        progress_context != nullptr &&
        repiu::platform::MillisecondTicks() - quiet_start_tick >=
        stall_timeout_milliseconds)
    {
        if (progress_context != nullptr)
        {
            WriteLiveTelemetrySnapshot(
                *progress_context,
                repiu::platform::MillisecondTicks() - start_tick,
                iteration + 1);
        }
        if (stall_timed_out != nullptr)
        {
            *stall_timed_out = true;
        }
        return HostPollOutcome::kTimedOut;
    }

    if (timeout_milliseconds !=
            repiu::runtime::kWaitForeverMilliseconds &&
        repiu::platform::MillisecondTicks() - start_tick >= timeout_milliseconds)
    {
        if (progress_context != nullptr)
        {
            WriteLiveTelemetrySnapshot(
                *progress_context,
                repiu::platform::MillisecondTicks() - start_tick,
                iteration + 1);
        }
        return HostPollOutcome::kTimedOut;
    }

    const TickCount current_tick = repiu::platform::MillisecondTicks();
    if (progress_context != nullptr)
    {
        const std::uint32_t dispatch_total =
            progress_context->exception_dispatch_entry_count.load(
                std::memory_order_relaxed) +
            progress_context->exception_dispatch_exit_count.load(
                std::memory_order_relaxed);
        if (dispatch_total != last_dispatch_total)
        {
            last_dispatch_total = dispatch_total;
            dispatch_quiet_start_tick = current_tick;
        }
    }
    // Task 503d-21: the capture below runs on both hosts now, because
    // 3d-20 gave Linux a way to read another thread's registers. Only
    // `GetThreadTimes` stays fenced, further down -- the kernel's
    // busy/blocked split for one thread has no counterpart worth
    // inventing.
    // Task 411: the same capture, but on a plain wall-clock interval with
    // no dispatch-quiet gate. A stalled run faults continuously, so the
    // gate below never opens there, and the guest's position during the
    // stall is exactly what needs sampling.
    // Issue #64: both captures sample the guest thread from outside, and a
    // guest on this thread has none to sample.
    if (guest_thread != nullptr && progress_context != nullptr &&
        progress_context->guest_position_census != nullptr &&
        current_tick - last_position_census_tick >=
            position_census_interval)
    {
        std::uint32_t cache_base = 0;
        std::uint32_t cache_size = 0;
        if (progress_context->aot_placement != nullptr &&
            progress_context->aot_placement->placed)
        {
            cache_base = progress_context->aot_placement->base_address;
            cache_size = static_cast<std::uint32_t>(
                progress_context->aot_placement->size);
        }
        NativePhaseSample census_sample;
        // Null telemetry: the stage marker belongs to the native sampler,
        // and overwriting it would confuse a reader of that instrument.
        if (CaptureNativePhaseSample(
                *guest_thread, progress_context->aot_placement, nullptr,
                &census_sample, loader_module_base, loader_module_size))
        {
            const GuestPositionClassification classification =
                ClassifyGuestPosition(
                    census_sample.eip,
                    census_sample.mapped,
                    census_sample.guest_eip,
                    progress_context->runtime_base,
                    progress_context->runtime_size,
                    cache_base,
                    cache_size);
            RecordGuestPosition(
                progress_context->guest_position_census.get(),
                classification);
            // Task 412: the call-site axis exists only for host samples, so
            // exactly one outcome is recorded per host sample and the
            // reconciliation in the report stays meaningful.
            if (classification.origin == GuestPositionOrigin::kHost)
            {
                RecordGuestPositionHostSite(
                    progress_context->guest_position_census.get(),
                    census_sample.host_call_site,
                    census_sample.host_scan_failed);
            }
            WriteLinuxX64NativeSampleTraceLine(
                census_sample,
                static_cast<std::uint32_t>(current_tick - start_tick));
        }
        else
        {
            RecordGuestPositionCaptureFailure(
                progress_context->guest_position_census.get());
        }
        // Task 412: the busy-or-blocked split. One call per sample, on the
        // poll thread, and the last reading is the one reported.
        std::uint64_t kernel_100ns = 0;
        std::uint64_t user_100ns = 0;
        if (repiu::platform::ReadHostThreadTimes(
                *guest_thread, &kernel_100ns, &user_100ns))
        {
            RecordGuestPositionThreadTime(
                progress_context->guest_position_census.get(),
                kernel_100ns, user_100ns,
                static_cast<std::uint32_t>(current_tick - start_tick));
        }
        last_position_census_tick = current_tick;
    }
    // Task 421: the music position, on its own interval. `host_context` is
    // what owns the audio backend, while the census lives on the guest
    // context, so both have to be present.
    if (progress_context != nullptr && host_context != nullptr &&
        progress_context->cd_audio_position_census != nullptr &&
        current_tick - last_cd_audio_census_tick >=
            cd_audio_census_interval)
    {
        CdAudioPositionEntry entry;
        entry.wall_milliseconds =
            static_cast<std::uint32_t>(current_tick - start_tick);
        host_context->cd_audio.FillPositionSample(&entry);
        // Report iterations *since the previous sample*: a zero here is the
        // worker not having run across the whole interval.
        const std::uint32_t iterations = entry.worker_iterations;
        entry.worker_iterations =
            iterations - last_cd_audio_worker_iterations;
        last_cd_audio_worker_iterations = iterations;
        // Task 430: the guest's clock advances once per injected tick while
        // the music advances on real time, so this pair is what separates a
        // drifting guest clock from a healthy one — on the same time axis
        // as the position beside it.
        const TimerTickDeliverySnapshot ticks =
            SnapshotTimerTickDelivery(
                progress_context->timer_tick_delivery);
        entry.timer_ticks_due =
            ticks.due_total - last_cd_audio_ticks_due;
        entry.timer_ticks_injected =
            ticks.injected_total - last_cd_audio_ticks_injected;
        last_cd_audio_ticks_due = ticks.due_total;
        last_cd_audio_ticks_injected = ticks.injected_total;
        // Task 431: the opportunity side, differenced the same way. The
        // trap count is where the injections come from.
        const std::uint32_t safe_point_traps =
            progress_context->aot_placement != nullptr
                ? progress_context->aot_placement
                      ->timer_safe_point_trap_count
                : 0U;
        entry.safe_point_traps =
            safe_point_traps - last_cd_audio_safe_point_traps;
        last_cd_audio_safe_point_traps = safe_point_traps;
        RecordCdAudioPosition(
            progress_context->cd_audio_position_census.get(), entry);
        last_cd_audio_census_tick = current_tick;
    }

    if (piu10_mp3_census_interval != 0 && progress_context != nullptr &&
        current_tick - last_piu10_mp3_census_tick >=
            piu10_mp3_census_interval)
    {
        const Piu10Mp3AudioSnapshot mp3 =
            progress_context->piu10_mp3_audio.Snapshot();
        const TimerTickDeliverySnapshot ticks =
            SnapshotTimerTickDelivery(
                progress_context->timer_tick_delivery);
        char line[320] = {};
        const int length = std::snprintf(
            line, sizeof(line),
            "[repiu-piu10-mp3-census] elapsed_ms=%lu received=%llu "
            "decoded=%llu queued_ms=%.1f lag_ms=%.1f inflight=%zu "
            "ring=%zu demand=%u sync=%u toggles=%llu multi=%llu "
            "starvation=%llu pcm_empty=%llu ticks_injected=%u\n",
            static_cast<unsigned long>(current_tick - start_tick),
            static_cast<unsigned long long>(mp3.received_bytes),
            static_cast<unsigned long long>(mp3.decoded_frames),
            mp3.pcm_queued_ms, mp3.playback_lag_ms,
            mp3.compressed_inflight_bytes,
            mp3.compressed_ring_bytes, mp3.demand ? 1U : 0U,
            static_cast<unsigned>(mp3.frame_sync),
            static_cast<unsigned long long>(mp3.sync_toggles),
            static_cast<unsigned long long>(mp3.sync_multi_toggle_events),
            static_cast<unsigned long long>(mp3.starvation_events),
            static_cast<unsigned long long>(mp3.pcm_empty_events),
            ticks.injected_total);
        if (length > 0)
        {
            repiu::platform::WriteHostErrorStream(
                line,
                static_cast<std::size_t>(length) < sizeof(line)
                    ? static_cast<std::size_t>(length)
                    : sizeof(line) - 1U);
        }
        last_piu10_mp3_census_tick = current_tick;
    }

    if (guest_thread != nullptr && native_sampling_enabled &&
        progress_context != nullptr &&
        current_tick - dispatch_quiet_start_tick >=
            kNativePhaseSampleQuietMilliseconds &&
        current_tick - native_sampler_state.last_sample_tick >=
            kNativePhaseSampleIntervalMilliseconds)
    {
        NativePhaseSample sample;
        if (CaptureNativePhaseSample(
                *guest_thread, progress_context->aot_placement,
                progress_context->shared_live_telemetry, &sample))
        {
            sample.last_indirect_source =
                progress_context->aot_last_indirect_source.load(
                    std::memory_order_relaxed);
            sample.last_indirect_target =
                progress_context->aot_last_indirect_target.load(
                    std::memory_order_relaxed);
            RecordNativePhaseSample(
                sample,
                &native_sampler_state,
                progress_context->shared_live_telemetry);
        }
        WriteNativePhaseSampleLine(sample,
                                        native_sampler_state,
                                        current_tick - start_tick);
        native_sampler_state.last_sample_tick = current_tick;
    }
    if (progress_context != nullptr &&
        current_tick - last_live_snapshot_tick >= 1000U)
    {
        WriteLiveTelemetrySnapshot(*progress_context,
                                   current_tick - start_tick,
                                   iteration + 1);
        last_live_snapshot_tick = current_tick;
    }
    return std::nullopt;
}

// Task 503d-18: the thread is the layer's handle, and the loop asks it whether
// the guest is still running. Issue #64: what each iteration does is the host
// turn; what is left here is the waiting between turns.
HostPollOutcome PollThreadUntilExit(const repiu::platform::HostThread& thread,
                                    std::uint32_t timeout_milliseconds,
                                    std::uint32_t stall_timeout_milliseconds,
                                    ThreadContext* progress_context,
                                    ThreadContext* host_context,
                                    std::uint32_t* exit_code,
                                    bool* stall_timed_out)
{
    if (!thread.valid)
    {
        return HostPollOutcome::kFailed;
    }
    HostTurn turn(&thread, timeout_milliseconds, stall_timeout_milliseconds,
                  progress_context, host_context, stall_timed_out);
    for (;;)
    {
        const std::optional<HostPollOutcome> outcome = turn.Take(exit_code);
        if (outcome.has_value())
        {
            return *outcome;
        }
        // Task 333: the loop used to end in Sleep(1), so a Glide command
        // published just after the pump above waited out the whole sleep before
        // anyone looked at it — measured as the dominant part of a gate call.
        // Waiting on the command instead keeps the same 1ms poll cadence when
        // nothing is posted and wakes immediately when something is.
        const bool timer_deadline_near = turn.TimerDeadlineNear();
        if (host_context != nullptr)
        {
            host_context->glide_backend.WaitAndPumpHostCommands(
                timer_deadline_near ? 0U : 1U);
        }
        else
        {
            repiu::platform::YieldMilliseconds(
                timer_deadline_near ? 0U : 1U);
        }
    }
}

OneThreadHostLoop::OneThreadHostLoop(HostTurn* const turn)
    : turn_(turn),
      next_turn_(std::chrono::steady_clock::now()),
      last_page_yield_(next_turn_)
{
}

// Issue #72 (design decision 6): in a browser the page shows the canvas and
// handles messages only when the thread comes back to its event loop. Once a
// picture was swapped, or 16 ms after the last yield so the page never
// stalls, the thread goes back for one round. Elsewhere the platform does
// nothing and this only costs a clock read per turn.
void OneThreadHostLoop::YieldToPageIfDue(const ThreadContext& context)
{
    constexpr auto kPageYieldGap = std::chrono::milliseconds(16);
    const std::uint64_t frames = context.glide_backend.presented_frame_total();
    const auto now = std::chrono::steady_clock::now();
    if (frames == frames_at_page_yield_ && now - last_page_yield_ < kPageYieldGap)
    {
        return;
    }
    frames_at_page_yield_ = frames;
    last_page_yield_ = now;
    if (repiu::platform::YieldToHostEventLoop())
    {
        ++page_yields_;
    }
}

bool OneThreadHostLoop::TurnDue(const ThreadContext& context) const
{
    return context.host_turn_requested ||
        std::chrono::steady_clock::now() >= next_turn_;
}

bool OneThreadHostLoop::TakeTurn(ThreadContext* const context)
{
    if (context->host_turn_requested)
    {
        context->host_turn_requested = false;
        ++yields_;
    }
    ++turns_;
    // What the audio workers do on their own threads elsewhere (design
    // decision 5).
    context->ymz_audio.Pump();
    context->piu10_mp3_audio.Pump();
    context->cd_audio.Pump();
    // Issue #81: in a browser page's Worker, what they produced goes on to
    // the page's AudioWorklet; elsewhere this returns at once.
    PumpBrowserAudio();
    outcome_ = turn_->Take(nullptr);
    if (outcome_.has_value())
    {
        return false;
    }
    YieldToPageIfDue(*context);
    // The next turn comes with the next tick, or at the cap if that is later.
    const std::chrono::nanoseconds until_tick(
        static_cast<std::chrono::nanoseconds::rep>(std::min<std::uint64_t>(
            turn_->NanosecondsUntilNextTick(),
            static_cast<std::uint64_t>(
                std::chrono::nanoseconds(kOneThreadTurnGapCap).count()))));
    next_turn_ = std::chrono::steady_clock::now() + until_tick;
    return true;
}

HostPollOutcome RunGuestOnThisThread(const repiu::platform::HostThreadEntry entry,
                                     ThreadContext* const context,
                                     const std::uint32_t timeout_milliseconds,
                                     const std::uint32_t stall_timeout_milliseconds,
                                     ThreadContext* const progress_context,
                                     std::uint32_t* const exit_code,
                                     bool* const stall_timed_out)
{
    if (entry == nullptr || context == nullptr)
    {
        return HostPollOutcome::kFailed;
    }
    HostTurn turn(nullptr, timeout_milliseconds, stall_timeout_milliseconds,
                  progress_context, context, stall_timed_out);
    OneThreadHostLoop loop(&turn);
    context->one_thread_host = &loop;
    context->host_turn_requested = false;
    const std::uint32_t code = entry(context);
    context->one_thread_host = nullptr;
    std::fprintf(stderr,
                 "[repiu-host-loop] one thread: turns=%llu yields=%llu "
                 "page-yields=%llu end=%s\n",
                 static_cast<unsigned long long>(loop.turns()),
                 static_cast<unsigned long long>(loop.yields()),
                 static_cast<unsigned long long>(loop.page_yields()),
                 loop.outcome().has_value() ? "host" : "guest");
    if (loop.outcome().has_value())
    {
        return *loop.outcome();
    }
    if (exit_code != nullptr)
    {
        *exit_code = code;
    }
    return HostPollOutcome::kGuestThreadExited;
}

void YieldToHost(ThreadContext* const context, const std::chrono::milliseconds milliseconds)
{
    if (context != nullptr && context->one_thread_host != nullptr)
    {
        context->host_turn_requested = true;
        return;
    }
    std::this_thread::sleep_for(milliseconds);
}

}  // namespace repiu::engine
