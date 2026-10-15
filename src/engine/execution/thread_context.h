#pragma once

#include "repiu/engine/execution_trampoline.h"
#include "repiu/engine/fault_recovery_provenance.h"
#include "repiu/engine/linux_x64_transfer_failure_provenance.h"
#include "repiu/engine/execution_probe_memory_dump.h"
#include "repiu/engine/runtime_memory_policy.h"
#include "repiu/engine/aot_code_cache.h"
#include "repiu/engine/out_of_arena_step_census.h"
#include "repiu/engine/live_telemetry.h"
#include "repiu/engine/aot_retired_trap_profile.h"
#include "repiu/engine/aot_return_stage_profile.h"
#include "repiu/engine/cd_audio_wave_out.h"
#include "repiu/engine/mscdex_command_trace.h"
#include "repiu/engine/ymz280b_audio_out.h"
#include "repiu/engine/piu10_mp3_audio_out.h"
#include "repiu/engine/glide_lfb_guest_storage.h"
#include "repiu/engine/glide_opengl_backend.h"
#include "repiu/engine/glide_ordinal_timing.h"
#include "repiu/engine/glide_lfb_timing.h"
#include "repiu/engine/glide_lfb_write_footprint.h"
#include "repiu/engine/glide_lfb_native_store_census.h"
#include "repiu/engine/glide_lfb_lock_interval_census.h"
#include "repiu/engine/glide_lfb_staging_shadow.h"
#include "repiu/engine/glide_setter_state_census.h"
#include "repiu/engine/glide_draw_batch.h"
#include "repiu/engine/glide_setter_state_cache.h"
#include "repiu/engine/pic_timer_in_service.h"
#include "repiu/engine/timer_return_pad.h"
#include "repiu/engine/timer_tick_delivery.h"
#include "repiu/engine/piu10_song_clock_census.h"
#include "repiu/engine/piu10_frame_sync_history.h"
#include "repiu/engine/jamma_input_timeline.h"
#include "repiu/engine/aot_boundary_opcode_census.h"
#include "repiu/engine/aot_ff_boundary_attribution.h"
#include "repiu/hle/linexe_call_gate.h"
#include "repiu/hle/glide_hle.h"
#include "repiu/hle/glide_lfb.h"
#include "repiu/hle/pit_timer.h"
#include "repiu/hle/piu10_isa_board.h"
#include "repiu/hle/bios_keyboard.h"
#include "repiu/media/chd_cd_image.h"
#include "repiu/runtime/aot_shadow_selector_block.h"
#include "repiu/runtime/dos_low_memory.h"
#include "repiu/runtime/selector_table.h"
#include "repiu/engine/veh_exit_site.h"
#include "native_fast_path.h"

#include <chrono>
#include <functional>
#include <mutex>
#include <memory>
#include "repiu/platform/guest_stack_switch.h"
#include "repiu/platform/host_thread.h"

#include <cstdint>
#include <atomic>
#include <array>
#include <string>
#include <vector>
#include <array>
#include <unordered_map>

namespace repiu::engine
{

struct ThreadContext;
class OneThreadHostLoop;

extern "C" ThreadContext* g_repiu_active_thread_context;

// Task 503d-16: the five stack globals that stood here moved to
// `repiu/platform/guest_stack_switch.h`, beside the field offsets the same
// assembly reads. This header keeps only the one whose type it defines.

struct StackSwitchCallState
{
    std::uint32_t entry_address = 0;
    std::uint32_t initial_esp = 0;
    std::uint32_t host_esp = 0;
    std::uint32_t guest_return_esp = 0;
    std::uint32_t result_code = 0;
    std::uint32_t enable_single_step_trace = 0;
    std::uint32_t host_fs = 0;
    std::uint32_t host_ds = 0;
    std::uint32_t host_es = 0;
    std::uint32_t host_gs = 0;
    std::uint32_t host_ss = 0;
    std::uint32_t guest_stack_base = 0;
    std::uint32_t guest_stack_limit = 0;
    std::uint32_t host_stack_base = 0;
    std::uint32_t host_stack_limit = 0;
};

struct DosInterruptVectorShadow
{
    std::uint16_t segment = 0;
    std::uint16_t offset = 0;
    bool valid = false;
};

struct DpmiInterruptVectorShadow
{
    std::uint16_t selector = 0;
    std::uint32_t offset = 0;
    bool valid = false;
};

struct ShadowWriteProvenance
{
    std::uint32_t sequence = 0;
    std::uint32_t eip = 0;
    std::uint32_t opcode = 0;
    std::uint32_t destination = 0;
    std::uint32_t value = 0;
    std::uint32_t width = 0;
};

constexpr std::uint32_t kShadowWriteProvenanceCapacity = 256;

enum class AotWorkerOperation : std::uint32_t
{
    kTranslate = 0,
    kRetireGuestPage = 2,
};

// DOS/32A is used only as an independent behavioral reference for this
// DOS/4G-compatible identification call; no DOS/32A source code is included.
// Reference: https://github.com/amindlost/dos32a/blob/master/src/dos32a/text/client/int21h.asm
constexpr std::uint16_t kDos4gIdentificationAx = 0xFF00U;
constexpr std::uint16_t kDos4gIdentificationDx = 0x0078U;
constexpr std::uint32_t kDos32aIdentificationSignature = 0xFFFF3447U;

// Static replay of the original DOS4GW resident handler establishes a
// different implementation contract: low AX=FFFFh, carry set, and the
// already-installed client-data GS preserved. Keep this disabled until the
// corresponding GS:0042h private environment is available atomically.
constexpr std::uint16_t kDos4gwIdentificationAxResult = 0xFFFFU;
constexpr bool kDos4gwIdentificationCarry = true;

// Original DOS4GW/LINEXE private-environment topology recovered by static
// replay. These are evidence constants, not active descriptors: the 0080h
// exports are 16-bit code and require HLE call gates before exposure.
constexpr std::uint16_t kDos4gwClientDataSelector = 0x0020U;
constexpr std::uint16_t kDos4gwPrivateRootOffset = 0x0042U;
constexpr std::uint16_t kDos4gwLinexeDataSelector = 0x0090U;
constexpr std::uint16_t kDos4gwLinexeLoaderOffset = 0x059AU;
constexpr std::uint16_t kDos4gwLinexeCodeSelector = 0x0080U;
constexpr std::uint16_t kGlideFirstGateOffset = 0x9200U;
constexpr std::uint32_t kGlideGateStride = 8U;

struct ThreadContext
{
    struct AotCallFrame
    {
        std::uint32_t source = 0;
        std::uint32_t target = 0;
        std::uint32_t fallthrough = 0;
        std::uint32_t trace_sequence = 0;
        std::uint32_t entry_esp = 0;
        AotTransferOrigin origin = AotTransferOrigin::kVeh;
    };
    static constexpr std::uint32_t kAotCallFrameCapacity = 1024;
    std::uint32_t entry_address = 0;
    std::uint32_t runtime_base = 0;
    std::uint32_t runtime_size = 0;
    std::uint32_t guest_initial_esp = 0;
    std::uint32_t host_esp = 0;
    std::uint32_t guest_return_esp = 0;
    std::uint32_t guest_thread_id = 0;
    StackSwitchCallState* active_call_state = nullptr;
    bool use_guest_stack = false;
    // Task 583: a guest is executing through the x64 cache entry right now.
    //
    // The second way to satisfy `use_guest_stack`. i386 satisfies it by
    // switching host ESP onto the guest stack, which fills `active_call_state`;
    // x64 satisfies it by seeding guest ESP into R15D at entry, which fills
    // nothing, because there is no switch to record or undo.
    //
    // It exists because the fault guard has to tell "a guest is running" from
    // "there is a host frame to unwind to", and on i386 those were the same
    // fact. Set only by `GuestCacheEntryThreadProc`, so it is always false on
    // i386 and that host's decisions are unchanged.
    bool cache_entry_active = false;
    // Issue #61. The same answer on the emulated model: a guest is running on
    // the emulated CPU, which has no stack switch to record it.
    bool emulated_entry_active = false;
    // Issue #64. Set while the guest runs on the host's own thread
    // (RunsHostOnOneThread): the loop the emulated CPU hands the host's turns
    // to between its stops, and a yield the HLE asked for (YieldToHost).
    // Touched only by that one thread.
    OneThreadHostLoop* one_thread_host = nullptr;
    bool host_turn_requested = false;
    bool enable_privileged_trap_hle = false;
    bool enable_traced_dos_hle = false;
    bool enable_segment_load_hle = false;
    bool enable_dos_hle = false;
    bool enable_single_step_trace = false;
    AotCodeCachePlacement* aot_placement = nullptr;
    // Task 376: single steps discarded outside the guest arena, classified.
    OutOfArenaStepCensus out_of_arena_step_census;
    bool aot_reentry_pending = false;
    bool aot_legacy_fallback = false;
    runtime::ExecutionBackend execution_backend =
        runtime::ExecutionBackend::kLegacy;
    bool aot_dbt_glide_direct_dispatch = false;
    // Task 445: inline-cache patches, all serviced on the guest thread.
    std::uint64_t aot_inline_cache_direct_patch_count = 0;
    // Task 503d-2. These were HANDLE, which is void* on Windows -- the same
    // type, so nothing here changed except that this header no longer pulls in
    // <windows.h>. It was the only thing it needed from it, and 24 of the 32
    // sources that would not compile under GCC were stopped by exactly that.
    // The thread and event API itself is still Win32-only; this says nothing
    // about who creates these.
    // Task 503d-18: the thread layer's handle rather than a bare void*, so the
    // join and the release are the same two calls on both hosts.
    repiu::platform::HostThread aot_translation_thread;
    void* aot_translation_request_event = nullptr;
    void* aot_translation_complete_event = nullptr;
    std::atomic<bool> aot_translation_shutdown{false};
    std::atomic<std::uint32_t> aot_translation_target{0};
    std::atomic<std::uint32_t> aot_worker_operation{
        static_cast<std::uint32_t>(AotWorkerOperation::kTranslate)};
    // Raw C++ resolver entries. The reported attempt is derived as
    // success + fallback (Task 282), so a sample that ends inside the resolver
    // still satisfies the accounting invariant.
    std::atomic<std::uint32_t> aot_dbt_return_entry_count{0};
    std::atomic<std::uint32_t> aot_dbt_return_success_count{0};
    std::atomic<std::uint32_t> aot_dbt_return_fallback_count{0};
    std::atomic<std::uint32_t> aot_dbt_return_fallback_reason_counts[
        kAotDbtDispatchFallbackReasonCount] = {};
    std::atomic<std::uint32_t> aot_retire_guest_page{0};
    std::atomic<bool> aot_retire_quarantine{false};
    AotDynamicAppendResult aot_translation_result;
    std::vector<runtime::AotExcludedGuestRange> aot_excluded_guest_ranges;
    AotInlineCachePatchResult aot_inline_cache_patch_result;
    AotGuestPageRetireResult aot_guest_page_retire_result;
    AotPageWriteWatchSet aot_page_write_watch;
    std::atomic<bool> aot_terminal_failure{false};
    std::uint32_t aot_reentry_cache_address = 0;
    std::atomic<std::uint32_t> aot_cache_entry_count{0};
    std::atomic<std::uint32_t> aot_boundary_count{0};
    // Per-reason breakdown of aot_boundary_count (Task 262): the boundary guest
    // instruction that forced each single-step exit. The five sum to
    // aot_boundary_count.
    std::atomic<std::uint32_t> aot_boundary_return_count{0};
    std::atomic<std::uint32_t> aot_boundary_indirect_count{0};
    std::atomic<std::uint32_t> aot_boundary_direct_count{0};
    std::atomic<std::uint32_t> aot_boundary_conditional_count{0};
    std::atomic<std::uint32_t> aot_boundary_other_count{0};
    std::atomic<std::uint32_t> aot_breakpoint_provenance_counts[
        kAotCacheBreakpointProvenanceCount] = {};
    // Task 263(a): characterize the dominant `other` boundary bucket. Lead-opcode
    // histogram of the boundary guest instruction (guest-thread only) plus the
    // most recent `other` boundary EIP and its first four bytes.
    std::uint32_t aot_other_opcode_histogram[256] = {};
    // Task 367: the same samples resolved past prefixes and the two-byte escape,
    // so the dominant exception population can be named by instruction.
    AotBoundaryOpcodeCensus aot_boundary_opcode_census;
    AotFfBoundaryAttribution aot_ff_boundary_attribution;
    std::atomic<std::uint32_t> aot_last_other_boundary_eip{0};
    std::atomic<std::uint32_t> aot_last_other_boundary_bytes{0};
    // Task 263(b): AOT residency proxy. Straight-line guest instruction count from
    // each real cache entry to its first control transfer, accumulated. Coverage
    // estimate = total / (total + single_step_trace_count).
    std::atomic<std::uint32_t> aot_residency_instruction_total{0};
    std::atomic<std::uint32_t> aot_residency_sample_count{0};
    std::atomic<std::uint32_t> aot_residency_max{0};
    // Task 289 Stage 1: complete descriptor fingerprints last folded into
    // segment-override sites. Selector-only comparison misses same-selector
    // DPMI base/limit changes.
    std::array<AotSegmentResolution, 6> aot_resolved_segments = {};
    bool aot_segment_resolutions_initialized = false;
    std::atomic<std::uint32_t> aot_selector_guard_native_site_count{0};
    std::atomic<std::uint32_t> aot_selector_guard_hle_site_count{0};
    std::atomic<std::uint32_t> aot_selector_guard_unresolved_site_count{0};
    std::atomic<std::uint32_t> aot_selector_guard_hle_exit_count{0};
    std::atomic<std::uint32_t> aot_selector_guard_mismatch_count{0};
    // Task 523: breakpoints the engine did not plant -- the guest's own 0xCC.
    // Guest-thread only, like the trap that produces it.
    std::uint32_t guest_owned_breakpoint_count = 0;
    // Task 526: what follows a reentry that took the trap-free path.
    //
    // Windows resolves 95% of its reentries through Glide direct dispatch and
    // pays no trap for them; Linux takes that path for 87% and traps anyway.
    // Whether the trap lands on the very cache address the dispatch just
    // jumped to is the difference between "the jump did not take" and "the
    // target itself still holds a breakpoint", and no existing counter
    // separates those. Guest-thread only, like the faults they count.
    std::uint32_t last_direct_dispatch_target = 0;
    bool direct_dispatch_trap_pending = false;
    std::atomic<std::uint32_t> direct_dispatch_trap_at_target{0};
    std::atomic<std::uint32_t> direct_dispatch_trap_elsewhere{0};
    std::atomic<std::uint32_t> direct_dispatch_other_elsewhere{0};
    std::atomic<std::uint32_t> direct_dispatch_step_after{0};
    std::atomic<std::uint32_t> direct_dispatch_clean{0};
    // Where the follow-on breakpoint actually lands. A small fixed histogram
    // because the answer expected is a handful of sites, and anything larger
    // would allocate inside a fault handler.
    static constexpr std::size_t kDirectTrapSiteCapacity = 8;
    std::uint32_t direct_trap_site_address[kDirectTrapSiteCapacity] = {};
    std::uint32_t direct_trap_site_count[kDirectTrapSiteCapacity] = {};
    std::atomic<std::uint32_t> aot_reentry_count{0};
    std::atomic<std::uint32_t> aot_legacy_fallback_count{0};
    std::atomic<std::uint32_t> aot_last_fallback_address{0};
    std::atomic<std::uint32_t> aot_dynamic_attempt_count{0};
    std::atomic<std::uint32_t> aot_dynamic_success_count{0};
    std::atomic<std::uint32_t> aot_dynamic_added_bytes{0};
    std::atomic<std::uint32_t> aot_dbt_hle_reentry_attempt_count{0};
    std::atomic<std::uint32_t> aot_dbt_hle_reentry_success_count{0};
    std::atomic<std::uint32_t> aot_dbt_hle_translation_attempt_count{0};
    std::atomic<std::uint32_t> aot_dbt_hle_translation_success_count{0};
    // Task 340: why the return to the cache after an HLE fails. Task 339 showed
    // 88.7% of baseline attempts rejected at the combined quarantine and
    // guest-IP check and 98.7% of SUPERBLOCK attempts rejected by the first
    // guard, but neither check's two conditions were separated, and their
    // causes and fixes differ. Guest thread only, so plain counters.
    std::uint32_t hle_reentry_reject_not_pending = 0;
    std::uint32_t hle_reentry_reject_segment_write = 0;
    std::uint32_t hle_reentry_reject_outside_arena = 0;
    std::uint32_t hle_reentry_reject_quarantined = 0;
    std::uint32_t hle_reentry_reject_cache_miss = 0;
    std::uint32_t hle_reentry_reject_span_unsafe = 0;
    std::uint32_t hle_reentry_success = 0;
    // Task 346: returns that proceeded after a segment write instead of being
    // abandoned, each preceded by a re-fold of the cache's segment sites.
    std::uint32_t hle_reentry_segment_write_resumed = 0;
    // Task 341: Task 340 found four quarantined pages blocking 80.24% of
    // post-HLE returns, so the first few quarantine events are recorded whole.
    // `source` is the guest address that performed the write; it is zero when
    // the execution address could not be mapped back, which the policy treats
    // the same as "wrote its own page" and therefore quarantines.
    static constexpr std::uint32_t kQuarantineTraceCapacity = 16U;
    struct QuarantineTraceEntry
    {
        std::uint32_t page = 0;
        std::uint32_t source = 0;
        std::uint32_t destination = 0;
        std::uint32_t byte_count = 0;
    };
    QuarantineTraceEntry quarantine_trace[kQuarantineTraceCapacity] = {};
    std::uint32_t quarantine_trace_count = 0;
    std::uint32_t quarantine_unknown_source_count = 0;
    // Task 342: how many times the guest wrote each page from that same page.
    // Quarantine is a churn defence, not a correctness one -- retiring alone
    // already prevents the cache from executing stale bytes -- so it now waits
    // for repetition instead of firing on a one-shot self-patch. Guest thread
    // only; a small linear table, since the observed population is three pages.
    static constexpr std::uint32_t kGuestPageWriteHistoryCapacity = 64U;
    struct GuestPageWriteRecord
    {
        std::uint32_t page = 0;
        std::uint32_t count = 0;
        // Task 344: the churn signal is the SAME address being rewritten, not a
        // page collecting several one-shot patches at different addresses.
        // Page 0x03033000 quarantined under Task 342 because two distinct
        // patch sites on it both counted toward one per-page total.
        std::uint32_t last_destination = 0;
        std::uint32_t repeat_count = 0;
    };
    GuestPageWriteRecord
        guest_page_write_history[kGuestPageWriteHistoryCapacity] = {};
    std::uint32_t guest_page_write_history_size = 0;
    // Pages that fell out of the table; they quarantine on their next write so
    // an overflowing workload degrades to the old policy rather than to none.
    std::uint32_t guest_page_write_history_overflow = 0;
    std::uint32_t quarantine_deferred_count = 0;
    // Task 404: a re-translation that fails once quarantines its page forever,
    // and the page pumpit3 loses this way carries the 200-iteration I/O delay
    // loop -- 35-40% of wall clock in single steps. The append result already
    // carries the reason in `message`, but nothing recorded it, and the six
    // possible reasons are fixed in six different places. Guest thread only:
    // the branch that fills this runs on the VEH path.
    static constexpr std::uint32_t kGenerationFailureTraceCapacity = 8U;
    static constexpr std::uint32_t kGenerationFailureMessageCapacity = 96U;
    struct GenerationFailureTraceEntry
    {
        std::uint32_t target = 0;
        std::uint32_t page = 0;
        bool quarantined = false;
        bool terminal = false;
        char message[kGenerationFailureMessageCapacity] = {};
    };
    GenerationFailureTraceEntry
        generation_failure_trace[kGenerationFailureTraceCapacity] = {};
    std::uint32_t generation_failure_trace_count = 0;
    std::uint32_t generation_failure_trace_overflow = 0;
    // Task 405: 98.6% of port I/O takes a privileged-instruction fault instead
    // of the exception-free dispatch slot, and the planner cannot explain that
    // -- `EmitHleDispatchSlot` always succeeds for `kPortIo`. `cache_count`
    // separates the two remaining explanations: a raw `in` emitted into the
    // cache, or code executing natively in the arena because it was never
    // translated. Guest thread only, like the JAMMA snapshot: both callers of
    // `HandlePortIoInstruction` (the VEH path and the Task 311 thunk) run there.
    static constexpr std::uint32_t kPortIoAddressCensusCapacity = 32U;
    struct PortIoAddressCensusEntry
    {
        std::uint32_t guest_address = 0;
        std::uint32_t count = 0;
        std::uint32_t cache_count = 0;
        // Task 406: separates "no translation exists for this address" from
        // "one exists and execution stays in the arena anyway". Filled only
        // under `REPIU_PORT_IO_CENSUS_MAPPING`, because the lookup costs about
        // 6,866 ticks and this site runs roughly 23,000 times a second.
        std::uint32_t mapped_count = 0;
        std::uint32_t reentry_pending_count = 0;
        // Task 408: Task 407's global ring could not target one address --
        // whatever it retained, the noisiest address took every slot. A slot
        // per address cannot be displaced. Only the first transition is kept;
        // later ones raise the count. Flags: bit 0 prev-in-cache, 1 trap flag,
        // 2 re-entry pending, 3 legacy fallback, 4 single-step trace.
        std::uint32_t entry_transition_count = 0;
        std::uint32_t entry_previous_code = 0;
        std::uint32_t entry_previous_eip = 0;
        std::uint8_t entry_flags = 0;
        // Task 409: the first sample alone cannot speak for the population.
        // `0x0301DB22` had 2,018-3,124 entries in runs whose whole single-step
        // census was 260-283, so at most a tenth of them could match the first
        // sample's predecessor. A four-way histogram settles which class
        // dominates without keeping every sample.
        std::uint32_t entry_prev_single_step = 0;
        std::uint32_t entry_prev_breakpoint = 0;
        std::uint32_t entry_prev_access_violation = 0;
        std::uint32_t entry_prev_other = 0;
        // Task 410: the class of the previous exception says what it was, not
        // who finished it. These two say which VEH exit resumed the guest and
        // at which EIP, so "the consumer did not advance EIP" and "the consumer
        // returned to the cache" are different readings of the same entry.
        // First sample only, like the three fields above.
        std::uint8_t entry_previous_exit_site = 0;
        std::uint32_t entry_previous_exit_eip = 0;
    };
    PortIoAddressCensusEntry
        port_io_address_census[kPortIoAddressCensusCapacity] = {};
    std::uint32_t port_io_address_census_size = 0;
    std::uint32_t port_io_address_census_overflow = 0;
    // Task 741: which host addresses raise breakpoint exceptions, and which
    // VEH exit ended each. The per-kind census says how many; this says
    // where. A cache address is mapped to its guest address on first sight;
    // an arena address is its own guest address. Guest thread only, at the
    // VEH choke point.
    //
    // An open-addressing table rather than a 32-slot list: the first version
    // filled its 32 slots with start-up sites and put 98% of the run into
    // overflow. 512 slots keyed by address keep the lookup O(1); the report
    // copies the 32 busiest out.
    static constexpr std::uint32_t kBreakpointSiteCensusCapacity = 512U;
    static constexpr std::uint32_t kBreakpointSiteReportCapacity = 32U;
    static constexpr std::uint32_t kBreakpointSiteExitCapacity = 4U;
    struct BreakpointSiteCensusEntry
    {
        std::uint32_t host_address = 0;
        std::uint32_t guest_address = 0;
        bool in_cache = false;
        bool guest_mapped = false;
        std::uint32_t count = 0;
        std::uint8_t exit_sites[kBreakpointSiteExitCapacity] = {};
        std::uint32_t exit_counts[kBreakpointSiteExitCapacity] = {};
        std::uint32_t exit_overflow = 0;
    };
    BreakpointSiteCensusEntry
        breakpoint_site_census[kBreakpointSiteCensusCapacity] = {};
    std::uint32_t breakpoint_site_census_size = 0;
    std::uint32_t breakpoint_site_census_overflow = 0;
    // Task 741: what an HLE reentry asks about a guest address, remembered.
    // Every HLE reentry decoded the resumed instruction with Zydis three ways
    // (segment write? span safe up to 64 instructions ahead? identical in
    // long mode?), and the answers depend only on the guest bytes, the code
    // segment's default operand size and the HLE boundary list. Direct-mapped
    // by address. An entry is valid while `aot_reentry_memo_generation` still
    // matches (it moves on every completed guest code write) and while the
    // first eight code bytes still read the same; the long-mode answer also
    // keeps the code mode it was computed under.
    static constexpr std::uint32_t kAotReentryMemoCapacity = 1024U;
    struct AotReentryMemoEntry
    {
        std::uint32_t guest_eip = 0;
        std::uint32_t generation = 0;
        std::uint64_t fingerprint = 0;
        // bit 0 segment probe, bit 1 span safety, bit 2 long-mode identity.
        std::uint8_t computed = 0;
        std::uint8_t segment_probe = 0;
        // 0 = no mode, otherwise GuestCodeDefaultOperandSize + 1.
        std::uint8_t long_mode_code_mode = 0;
        bool span_safe = false;
        bool long_mode_identical = false;
        const char* span_reason = nullptr;
    };
    AotReentryMemoEntry aot_reentry_memo[kAotReentryMemoCapacity] = {};
    std::uint32_t aot_reentry_memo_generation = 1;
    std::uint32_t aot_reentry_memo_hits[3] = {};
    std::uint32_t aot_reentry_memo_misses[3] = {};
    // Task 407: the delay loop free-runs in the arena with no trap flag, and
    // that state is self-sustaining, so the question is how it is first
    // entered. In steady state the exception before an arena port I/O fault is
    // always another `0xC0000096`; on entry it is something else, which is the
    // whole filter. Updated at the VEH choke point, read in the port I/O
    // handler -- guest thread only, like the census above.
    std::uint32_t last_veh_code = 0;
    std::uint32_t last_veh_eip = 0;
    bool last_veh_in_cache = false;
    std::uint32_t prev_veh_code = 0;
    std::uint32_t prev_veh_eip = 0;
    bool prev_veh_in_cache = false;
    // Task 410: the same one-slot history, for who consumed the exception
    // rather than what it was. Written by VehExitRecorder's destructor, which
    // is constructed before AotHleTranslationScope and therefore destroyed
    // after it -- so the EIP recorded here is the one the guest resumes at,
    // not the one before that scope rewrote it. `exit_flags` uses the same bit
    // order as the port I/O entry sample: 0 in-cache, 1 trap flag, 2 re-entry
    // pending, 3 legacy fallback, 4 single-step trace.
    std::uint8_t last_veh_exit_site = 0;
    std::uint32_t last_veh_exit_eip = 0;
    std::uint8_t last_veh_exit_flags = 0;
    std::uint8_t prev_veh_exit_site = 0;
    std::uint32_t prev_veh_exit_eip = 0;
    std::uint8_t prev_veh_exit_flags = 0;
    // The population behind the per-address first sample. Task 409 showed a
    // first sample can describe a tenth of its population, so the exit site of
    // *every* single step taken at an arena EIP is counted here and the total
    // is kept beside it: `sum(counts) == total` or the instrument is not
    // trusted. Guest thread only, like the census above.
    std::uint32_t veh_arena_single_step_count = 0;
    std::uint32_t veh_arena_single_step_exit_site_counts[kVehExitSiteCount] = {};
    static constexpr std::uint32_t kArenaPortIoEntryTraceCapacity = 16U;
    struct ArenaPortIoEntryTraceEntry
    {
        std::uint32_t guest_address = 0;
        std::uint32_t previous_code = 0;
        std::uint32_t previous_eip = 0;
        bool previous_in_cache = false;
        bool trap_flag = false;
        bool reentry_pending = false;
        bool legacy_fallback = false;
        bool single_step_trace = false;
    };
    // A ring holding the newest entries; the count is the running total, so
    // `count % capacity` is both the write slot and the oldest live slot.
    ArenaPortIoEntryTraceEntry
        arena_port_io_entry_trace[kArenaPortIoEntryTraceCapacity] = {};
    std::uint32_t arena_port_io_entry_trace_count = 0;
    std::atomic<std::uint32_t> aot_dbt_hle_dispatch_entry_count{0};
    std::atomic<std::uint32_t> aot_dbt_hle_dispatch_success_count{0};
    std::atomic<std::uint32_t> aot_dbt_hle_dispatch_fallback_count{0};
    std::atomic<std::uint32_t> aot_dbt_hle_dispatch_fallback_reason_counts[
        kAotDbtHleFallbackReasonCount] = {};
    std::atomic<std::uint32_t> aot_dbt_hle_dispatch_last_source{0};
    std::atomic<std::uint32_t> aot_dbt_hle_dispatch_last_next{0};
    std::atomic<std::uint32_t> aot_dbt_hle_dispatch_last_bytes{0};
    // Per-run liveness signal for every valid Glide direct-dispatch entry,
    // including entries that later take the one-step fallback.
    std::atomic<std::uint32_t> aot_dbt_glide_dispatch_entry_count{0};
    std::atomic<std::uint32_t> aot_indirect_dispatch_count{0};
    std::atomic<std::uint32_t> aot_inline_cache_patch_attempt_count{0};
    std::atomic<std::uint32_t> aot_inline_cache_patch_success_count{0};
    std::atomic<std::uint32_t> aot_code_write_count{0};
    std::atomic<std::uint32_t> aot_page_retire_attempt_count{0};
    std::atomic<std::uint32_t> aot_page_retire_success_count{0};
    std::atomic<std::uint32_t> aot_generation_publish_count{0};
    std::atomic<std::uint32_t> aot_generation_failure_count{0};
    std::atomic<std::uint32_t> aot_generation_relinked_entry_count{0};
    std::atomic<std::uint32_t> aot_retired_entry_trap_count{0};
    AotRetiredTrapProfile aot_retired_trap_profile;
    std::atomic<std::uint32_t> aot_quarantine_count{0};
    std::atomic<std::uint32_t> aot_inline_cache_guard_reset_count{0};
    std::atomic<std::uint32_t> aot_last_code_write_source{0};
    std::atomic<std::uint32_t> aot_last_code_write_destination{0};
    std::atomic<std::uint32_t> aot_last_retired_page{0};
    std::atomic<std::uint32_t> aot_last_published_generation{0};
    bool aot_exception_mapping_valid = false;
    std::uint32_t aot_exception_cache_address = 0;
    std::uint32_t aot_exception_guest_address = 0;
    std::uint8_t aot_exception_cache_bytes[16] = {};
    std::uint8_t aot_exception_guest_bytes[16] = {};
    std::atomic<std::uint32_t> aot_last_indirect_source{0};
    std::atomic<std::uint32_t> aot_last_indirect_target{0};
    std::atomic<std::uint32_t> aot_return_dispatch_count{0};
    std::atomic<std::uint32_t> aot_last_return_target{0};
    std::atomic<std::uint32_t> aot_last_return_source{0};
    std::uint32_t aot_last_return_stack[4] = {};
    bool execution_probe_configured = false;
    bool execution_probe_hit = false;
    std::uint32_t execution_probe_offset = 0;
    std::uint32_t execution_probe_memory_offset = 0;
    // Task 713. Log the first N arrivals at the probe offset instead of
    // capturing only the first. A branch that goes one way on its first few
    // calls and another later -- a material selector called once per draw state
    // -- cannot be read from its first arrival alone. Zero means off.
    std::uint32_t execution_probe_log_arrivals = 0;
    std::uint32_t execution_probe_arrival_count = 0;
    X86ExecutionSnapshot execution_probe_snapshot;
    std::uint32_t execution_probe_stack[8] = {};
    ExecutionProbeMemoryWindow execution_probe_memory[
        kExecutionProbeRegisterCount] = {};
    ExecutionProbeDumpRequest execution_probe_dump_request;
    ExecutionProbeDumpResult execution_probe_dump_result;
    bool execution_trace_configured = false;
    bool execution_trace_log_enabled = false;
    std::uint32_t execution_trace_start_offset = 0;
    std::uint32_t execution_trace_end_offset = 0;
    std::uint32_t execution_trace_esp_offset = 0;
    std::uint32_t execution_trace_hit_count = 0;
    bool execution_trace_sentinel2_configured = false;
    std::uint32_t execution_trace_sentinel2_offset = 0;
    std::uint32_t execution_trace_sentinel_rearm_count = 0;
    ExecutionTraceEntry
        execution_trace[kExecutionTraceCapacity] = {};
    std::uint32_t aot_call_depth = 0;
    AotCallFrame aot_call_frames[kAotCallFrameCapacity] = {};
    bool aot_last_return_matches_call = false;
    std::uint32_t aot_last_expected_return = 0;
    std::uint32_t aot_last_call_source = 0;
    std::uint32_t aot_last_call_target = 0;
    std::uint32_t aot_last_expected_call_source = 0;
    std::uint32_t aot_last_expected_call_target = 0;
    std::uint32_t aot_return_trace_count = 0;
    AotReturnTraceEntry
        aot_return_trace[kAotReturnTraceCapacity] = {};
    std::uint32_t aot_transfer_trace_count = 0;
    AotTransferTraceEntry
        aot_transfer_trace[kAotTransferTraceCapacity] = {};
    bool aot_dbt_call_return_trace_configured = false;
    std::uint32_t aot_dbt_call_return_trace_count = 0;
    std::uint32_t aot_dbt_call_return_call_count = 0;
    std::uint32_t aot_dbt_call_return_return_count = 0;
    std::uint32_t aot_dbt_call_return_match_count = 0;
    std::uint32_t aot_dbt_call_return_mismatch_count = 0;
    std::uint32_t aot_dbt_call_return_overwrite_count = 0;
    bool aot_dbt_call_return_first_divergence_valid = false;
    AotCallReturnTraceEntry aot_dbt_call_return_first_divergence;
    AotCallReturnTraceEntry
        aot_dbt_call_return_trace[kAotCallReturnTraceCapacity] = {};
    detail::NativeFastPathState native_fast_path;
    bool returned = false;
    bool process_exit = false;
    bool dos_termination_captured = false;
    std::uint32_t dos_termination_ax = 0;
    std::uint32_t dos_termination_eip = 0;
    std::uint32_t dos_termination_esp = 0;
    std::uint32_t dos_termination_stack[kDosTerminationStackCapacity] = {};
    bool exception_caught = false;
    std::uint32_t exception_code = 0;
    std::uint32_t exception_address = 0;
    std::uint32_t exception_eax = 0;
    std::uint32_t exception_ebx = 0;
    std::uint32_t exception_ecx = 0;
    std::uint32_t exception_edx = 0;
    std::uint32_t exception_esi = 0;
    std::uint32_t exception_edi = 0;
    X86ExecutionSnapshot exception_snapshot;
    std::uint32_t exception_access_kind = 0xFFFFFFFFU;
    std::uint32_t exception_fault_va = 0;
    std::uint32_t exception_fault_region_base = 0;
    std::uint32_t exception_fault_alloc_base = 0;
    std::uint32_t exception_fault_state = 0;
    std::uint32_t exception_fault_protect = 0;
    std::uint32_t exception_fault_region_size = 0;
    std::uint32_t exception_esi_dwords[8] = {};
    std::uint32_t exception_esi_dword_valid_mask = 0;
    std::uint8_t exception_register_strings[6][32] = {};
    std::uint32_t exception_register_string_valid_mask = 0;
    std::uint32_t exception_stack_base = 0;
    std::uint32_t exception_stack_dwords[kExceptionStackDwordCapacity] = {};
    std::uint32_t exception_stack_dword_count = 0;
    // Task 647. A recovery callback can replace the source EIP with a host
    // recovery destination. Keep the source and the path so a later fault at
    // that destination can still be attributed.
    FaultRecoveryProvenance fault_recovery_provenance;
    // Task 648. Preserve the latest failed x64 transfer across the unresolved
    // thunk's INT3/UD2 boundary for the final fault trace.
    LinuxX64TransferFailureProvenance
        linux_x64_transfer_failure_provenance;
    UnhandledBreakpointEvidence unhandled_breakpoint_evidence;
    std::uint32_t handled_fatal_breakpoint_count = 0;
    std::uint32_t last_fatal_breakpoint_address = 0;
    std::uint32_t last_fatal_message_address = 0;
    std::string last_fatal_message;
    bool fatal_breakpoint_continued = false;
    bool fatal_halt_reached = false;
    std::uint32_t handled_hle_trap_count = 0;
    std::uint32_t last_hle_trap_address = 0;
    std::uint32_t last_hle_trap_opcode = 0;
    repiu::hle::LinexeCallGatePlan linexe_gate_plan;
    repiu::hle::LinexeArenaLayout linexe_arena_layout;
    bool linexe_environment_active = false;
    std::uint32_t linexe_gs_byte_load_count = 0;
    std::uint32_t linexe_first_gs_byte_offset = 0;
    std::uint32_t linexe_first_gs_byte_value = 0;
    std::uint32_t linexe_scan_entry_count = 0;
    std::uint32_t linexe_module_candidate_count = 0;
    std::uint32_t linexe_module_match_count = 0;
    std::uint32_t linexe_name_pointer_valid_count = 0;
    std::uint32_t linexe_name_byte_instruction_count = 0;
    std::uint32_t linexe_data_gs_load_count = 0;
    std::uint16_t linexe_module_selector_stack_value = 0;
    std::uint32_t linexe_module_offset_stack_value = 0;
    std::uint32_t linexe_export_offset_stack_value = 0;
    std::uint16_t linexe_export_selector_stack_value = 0;
    std::uint32_t linexe_export_jump_source_esp = 0;
    std::uint32_t linexe_export_jump_source_module_offset = 0;
    std::uint16_t linexe_export_jump_source_module_selector = 0;
    std::uint32_t linexe_export_jump_target_esp = 0;
    std::uint32_t linexe_export_jump_target_module_offset = 0;
    std::uint16_t linexe_export_jump_target_module_selector = 0;
    std::uint32_t linexe_export_name_compare_count = 0;
    std::uint16_t linexe_export_name_compare_gs = 0;
    std::uint32_t linexe_export_name_compare_edi = 0;
    std::uint32_t linexe_export_name_compare_esi = 0;
    std::uint8_t linexe_export_name_actual_byte = 0;
    std::uint8_t linexe_export_name_expected_byte = 0;
    std::uint32_t linexe_export_name_stage_mask = 0;
    std::uint32_t linexe_export_entry_name_offset_value = 0;
    std::uint32_t linexe_export_entry_name_selector_value = 0;
    std::uint32_t linexe_export_result_store_destination = 0;
    std::uint32_t linexe_export_result_store_value = 0;
    std::uint32_t linexe_export_result_store_count = 0;
    std::uint16_t linexe_export_value_load_selector = 0;
    std::uint32_t linexe_export_value_load_offset = 0;
    std::uint32_t linexe_export_value_load_value = 0;
    std::uint32_t linexe_root_selector_eax = 0;
    std::uint16_t linexe_root_read_gs = 0;
    std::uint32_t linexe_shared_load_entry_count = 0;
    std::uint32_t linexe_shared_load_read_count = 0;
    std::uint16_t linexe_shared_load_selector = 0;
    std::uint32_t linexe_shared_load_offset = 0;
    std::uint32_t linexe_shared_load_value = 0;
    std::uint32_t linexe_root_offset_load_value = 0;
    std::uint32_t linexe_root_selector_load_value = 0;
    std::uint32_t linexe_root_offset_load_success = 0;
    std::uint32_t linexe_root_selector_load_success = 0;
    std::uint32_t linexe_export_match_count = 0;
    std::uint32_t linexe_export_entry_loop_count = 0;
    std::uint32_t linexe_export_compare_count = 0;
    std::uint32_t linexe_export_compare_eax = 0;
    std::uint32_t linexe_export_compare_ecx = 0;
    std::uint32_t linexe_export_compare_eflags = 0;
    std::uint32_t linexe_export_count_load_edx = 0;
    std::uint16_t linexe_export_count_load_gs = 0;
    std::uint32_t linexe_scan_return_count = 0;
    std::uint32_t linexe_indirect_far_call_count = 0;
    std::uint32_t linexe_indirect_far_call_source = 0;
    std::uint32_t linexe_indirect_far_call_pointer = 0;
    std::uint32_t linexe_indirect_far_call_offset = 0;
    std::uint16_t linexe_indirect_far_call_selector = 0;
    bool linexe_indirect_far_call_known_export = false;
    std::uint32_t linexe_bridge_entry_count = 0;
    bool linexe_bridge_gate_valid = false;
    std::uint16_t linexe_bridge_selector = 0;
    std::uint32_t linexe_bridge_offset = 0;
    std::uint32_t linexe_bridge_service = 0;
    std::uint32_t linexe_bridge_esp = 0;
    std::uint32_t linexe_bridge_ebp = 0;
    std::uint32_t linexe_bridge_stack[20] = {};
    char linexe_bridge_argument_text[128] = {};
    char linexe_bridge_stack_text[20][64] = {};
    std::uint32_t linexe_virtual_module_load_count = 0;
    std::uint32_t linexe_virtual_module_handle = 0;
    std::uint32_t linexe_get_proc_count = 0;
    std::uint32_t linexe_get_proc_result_pointer = 0;
    char linexe_get_proc_name[64] = {};
    std::uint32_t glide_gate_entry_count = 0;
    std::uint32_t glide_gate_handled_count = 0;
    // Task 750. A swap posted and not yet presented, which the guest is
    // waiting out at the gate with the timer ISR running; and whether the
    // gate handler's last return left an injected interrupt frame rather
    // than the gate's own return.
    bool glide_swap_wait_active = false;
    // #6. When the current swap wait began. Kept across the gate re-entries
    // that injected ticks cause, so the hold before the first injection is
    // measured from the wait's start.
    std::chrono::steady_clock::time_point glide_swap_wait_begin{};
    // Task 750. The host poll loop's tick arming, callable from the guest
    // thread too: that loop is the host thread, which is inside the present
    // (a vblank wait, a pacing sleep) exactly when the guest waits the swap
    // out, so nothing was arming the ticks the wait is meant to deliver.
    // Both callers hold the mutex; the loop clears the callable on return.
    std::mutex timer_tick_arm_mutex;
    std::function<void()> timer_tick_arm;
    bool glide_gate_interrupt_injected = false;
    std::uint32_t glide_gate_esp = 0;
    std::uint32_t glide_gate_stack[8] = {};
    std::uint16_t glide_gate_ordinal = 0;
    std::uint32_t glide_gate_argument_bytes = 0;
    char glide_gate_name[64] = {};
    repiu::hle::GlideImplementationIssueTracker glide_implementation_issues;
    std::uint32_t glide_texture_gate_trace_count = 0;
    bool glide_texture_gate_trace_wrapped = false;
    GlideTextureGateTraceEntry glide_texture_gate_trace[kGlideTextureGateTraceCapacity] = {};
    GlideTriangleObservation glide_first_triangle;
    std::uint32_t glide_triangle_trace_count = 0;
    bool glide_triangle_trace_wrapped = false;
    GlideTriangleTraceEntry glide_triangle_trace[kGlideTriangleTraceCapacity] = {};
    std::array<std::uint32_t, 256> glide_call_counts = {};
    std::array<std::array<std::uint32_t, 8>, 256> glide_first_stacks = {};
    std::array<std::string, 256> glide_call_names = {};
    // Task 353: decoded gate and existing backend rendezvous time by ordinal.
    GlideOrdinalTimingProfile glide_ordinal_timing;
    // Task 724: guest-thread LFB seed timing; read after guest shutdown only.
    GlideLfbTimingProfile glide_lfb_timing;
    // Task 725: private baseline and aggregate guest LFB write footprint.
    GlideLfbWriteFootprintProfile glide_lfb_write_footprint;
    // Task 726: explicit AOT stores observed while a write LFB range is active.
    GlideLfbNativeStoreCensusProfile glide_lfb_native_store_census;
    // Task 364: exact repeated-versus-changing state-setter arguments. Guest
    // thread only, and observation only — it never changes a dispatch result.
    GlideSetterCensusProfile glide_setter_census;
    // Task 365: which state was last applied successfully on the host, so an exact
    // repeat can skip the rendezvous. Shares its rules with the census above.
    GlideSetterStateCache glide_setter_state_cache;
    // Task 438: primitives waiting for the next ordering boundary. Owned by
    // the guest thread; the host only ever sees it inside a flush, which is
    // itself a rendezvous, so no locking is needed.
    GlideDrawBatch glide_draw_batch;
    std::uint32_t glide_window_open_count = 0;
    std::uint32_t glide_logical_width = 0;
    std::uint32_t glide_logical_height = 0;
    std::string glide_backend_message;
    std::vector<exe::LeResidentName> glide_exports;
    repiu::hle::GlideGatePlan glide_gate_plan;
    repiu::media::ChdCdImage cd_image;
    CdAudioWaveOut cd_audio;
    // PIU10 board sound. Independent of the CD-DA path above: background music
    // comes from CD audio tracks while effects come from the YMZ280B sample ROM.
    Ymz280bAudioOut ymz_audio;
    bool ymz_audio_available = false;
    bool piu_jamma_board_enabled = false;
    JammaInputTimeline jamma_input_timeline;
    hle::BiosKeyboard bios_keyboard;
    // Separate ISA16 PIU10 flash/MP3/security board at 0x02D0..0x02DF.
    // This is not the JAMMA/YMZ280B board at 0x02A0..0x02AF.
    Piu10Mp3AudioOut piu10_mp3_audio;
    // Issue #58. Fed from the guest's MP3 status reads; guest thread only,
    // except `Summary()`.
    Piu10SongClockCensus piu10_song_clock;
    // Issue #58. ESP of the guest instruction reading the PIU10 board, so
    // the MP3 status source can tell a read inside an injected timer handler
    // and answer it at that tick's due time; and the policy that keeps the
    // answered time from running backwards.
    std::uint32_t piu10_read_guest_esp = 0;
    Piu10FrameSyncAnswerClock piu10_frame_sync_answer_clock;
    repiu::hle::Piu10IsaBoard piu10_isa_board;
    bool piu10_isa_board_enabled = false;
    std::atomic<std::uint64_t> piu10_mp3_fast_path_write_count{0};
    std::atomic<std::uint64_t> piu10_mp3_frame_batch_byte_count{0};
    bool piu10_mp3_frame_batch_enabled = false;
    std::uint32_t piu10_mp3_frame_batch_rejection_mask = 0U;
    bool piu10_mp3_frame_batch_audit_enabled = false;
    bool piu10_mp3_frame_batch_audit_active = false;
    std::array<std::uint8_t, 2048> piu10_mp3_frame_batch_audit_bytes = {};
    std::uint32_t piu10_mp3_frame_batch_audit_size = 0U;
    std::uint32_t piu10_mp3_frame_batch_audit_index = 0U;
    std::uint32_t piu10_mp3_frame_batch_audit_cursor = 0U;
    std::uint32_t piu10_mp3_frame_batch_audit_count = 0U;
    std::uint32_t piu10_mp3_frame_batch_audit_ecx = 0U;
    std::uint64_t piu10_mp3_frame_batch_audit_passed_frames = 0U;
    std::uint64_t piu10_mp3_frame_batch_audit_mismatches = 0U;
    bool mscdex_available = false;
    bool cd_audio_available = false;
    std::uint8_t mscdex_drive = 3;
    std::uint32_t mscdex_request_count = 0;
    std::uint16_t mscdex_frame_es = 0;
    std::uint32_t mscdex_decline_count = 0;
    std::uint32_t mscdex_last_decline_reason = 0;
    std::uint32_t mscdex_last_resolve_kind = 0;
    std::uint32_t mscdex_last_header_bytes = 0;
    // IOCTL control block code diagnostics: which position call the guest uses
    // and which ones we are still turning away with status 8103h.
    std::uint32_t mscdex_last_ioctl_subfunction = 0xFFFFFFFFU;
    bool mscdex_last_ioctl_handled = false;
    std::uint32_t mscdex_last_ioctl_length = 0;
    std::uint32_t mscdex_ioctl_reject_mask = 0;
    std::uint8_t mscdex_last_play_mode = 0xFFU;
    std::uint32_t mscdex_last_play_start = 0;
    std::uint32_t mscdex_last_play_length = 0;
    std::uint8_t mscdex_last_seek_mode = 0xFFU;
    std::uint32_t mscdex_last_seek_target = 0;
    repiu::hle::GlideLogicalState glide_state;
    GlideOpenGlBackend glide_backend;
    // Task 708. The memory behind the surface below, placed where a 32-bit
    // field can name it. Released in this object's destructor body, which runs
    // before any member is destroyed and so before the surface that points into
    // it -- the surface reads nothing on the way down. It is declared ahead of
    // the surface as well, so that the two are read in the order they depend on
    // each other.
    GlideLfbGuestStorage glide_lfb_guest_storage;
    // R4 LFB staging surface handed to the guest by grLfbLock. Host-owned (see
    // design 257 3.1): the guest writes it with native instructions under the
    // flat DS, so it does not need to live inside the runtime arena.
    //
    // Task 708 added the one condition that is not about the arena: grLfbLock
    // reports its address in a 32-bit `GrLfbInfo_t::lfbPtr`, so on a 64-bit
    // host the storage has to be placed below 4 GiB rather than left to the
    // heap. `glide_lfb_guest_storage` above is that placement.
    repiu::hle::GlideLfbSurface glide_lfb_surface;
    std::uint32_t glide_lfb_lock_count = 0;
    std::uint32_t glide_lfb_present_count = 0;
    // Task 476: grLfbReadRegion and grLfbWriteRegion share the staging surface
    // as a shadow of the frame buffer. `valid` says it mirrors the frame buffer,
    // `dirty` that region writes are still waiting to be presented. Every gate
    // that is not a region gate flushes and then invalidates, so a burst of
    // region calls costs one readback and one present instead of one per row.
    bool glide_lfb_region_shadow_valid = false;
    bool glide_lfb_region_shadow_dirty = false;
    // Which GrBuffer_t the shadow stands for. A region call naming a different
    // buffer flushes and re-seeds rather than mixing two buffers in one image.
    std::uint32_t glide_lfb_region_shadow_buffer = 0;
    std::uint32_t glide_lfb_region_seed_count = 0;
    std::uint32_t glide_lfb_region_flush_count = 0;
    // Task 761: the RGBA8 mirror of the staging surface, kept only while the
    // high-precision presentation toggle is on. The 565 staging stays the
    // guest-visible truth; this shadow only feeds PresentLfbSurface, so
    // >=24-bit region sources keep their precision to the screen. `valid`
    // clears whenever the toggle is off at a flush, so re-enabling starts from
    // a 565 decode rather than stale pixels.
    std::vector<std::uint8_t> glide_lfb_region_rgba8_shadow;
    bool glide_lfb_region_rgba8_valid = false;
    std::uint32_t glide_lfb_region_rgba8_present_count = 0;
    // Task 728: the second shadow of the same staging surface, this one for the
    // lock/unlock path. It survives the state setters between two locks, so a
    // write lock whose pixels the host still holds can skip its readback.
    GlideLfbStagingShadowState glide_lfb_staging_shadow;
    // Task 729: the gates between a write unlock and the next write lock.
    GlideLfbLockIntervalCensus glide_lfb_lock_interval_census;
    std::uint32_t glide_lfb_region_read_count = 0;
    std::uint32_t glide_lfb_region_write_count = 0;
    std::uint32_t linexe_scan_return_eax = 0;
    std::uint32_t linexe_scan_return_ebp = 0;
    std::uint32_t linexe_scan_caller_eax = 0;
    std::uint32_t linexe_selector_init_results[3] = {};
    std::uint32_t dpmi_allocate_call_count = 0;
    std::uint16_t dpmi_last_allocate_requested_count = 0;
    std::uint16_t dpmi_last_allocated_selector = 0;
    PortIoObservation port_io;
    DosPathObservation dos_path;
    DosFileIoObservation dos_file_io;
    AllocatorProbeObservation allocator_probe;
    AllocatorControlFlowObservation allocator_control_flow;
    std::uint32_t handled_dos_interrupt_count = 0;
    std::uint32_t last_dos_interrupt_vector = 0;
    std::uint32_t last_dos_interrupt_ah = 0;
    std::uint32_t last_dos_interrupt_ax = 0;
    std::uint32_t handled_dos_interrupt_ah_counts[256] = {};
    // Task 528: the aggregate AH histogram above includes every HLE interrupt
    // vector. Keep an INT 21h-specific histogram for trace-free live analysis.
    std::uint32_t handled_dos_int21_count = 0;
    std::uint32_t handled_dos_int21_ah_counts[256] = {};
    // Task 764: INT 21h AH=07h/08h. An extended key hands out its scan code on
    // the call after the zero, and a call with nothing queued is counted as a
    // wait (the guest is left on the `int 21h` to ask again).
    std::uint8_t dos_console_pending_scan_code = 0;
    bool dos_console_pending_scan_code_valid = false;
    std::uint32_t dos_console_input_count = 0;
    std::uint32_t dos_console_input_wait_count = 0;
    bool dos_date_offset_valid = false;
    std::int32_t dos_date_offset_days = 0;
    repiu::hle::DosVirtualFileSystemState dos_file_system;
    std::uint32_t handled_dos_chdir_count = 0;
    std::string last_dos_chdir_guest_path;
    std::string last_dos_chdir_host_path;
    std::string last_dos_chdir_virtual_path;
    bool last_dos_chdir_success = false;
    std::uint16_t last_dos_chdir_error = 0;
    std::uint32_t handled_dos_getcwd_count = 0;
    std::uint8_t last_dos_getcwd_drive = 0;
    std::string last_dos_getcwd_path;
    bool last_dos_getcwd_success = false;
    std::uint16_t last_dos_getcwd_error = 0;
    std::uint32_t handled_dos_getdrive_count = 0;
    std::uint8_t last_dos_getdrive_value = 0;
    std::uint32_t handled_dos_open_count = 0;
    std::string last_dos_open_guest_path;
    std::string last_dos_open_host_path;
    std::string last_dos_open_virtual_path;
    bool last_dos_open_success = false;
    std::uint16_t last_dos_open_error = 0;
    std::uint16_t last_dos_open_handle = 0;
    std::uint8_t last_dos_open_access_mode = 0;
    std::uint32_t handled_dos_read_count = 0;
    std::uint16_t last_dos_read_handle = 0;
    std::uint32_t last_dos_read_requested_bytes = 0;
    std::uint32_t last_dos_read_actual_bytes = 0;
    std::uint32_t last_dos_read_buffer = 0;
    bool last_dos_read_success = false;
    std::uint16_t last_dos_read_error = 0;
    // Task 477: INT 21h AH=3Ch / AH=40h. The guest creating a file is normally
    // it reporting on itself, so what it writes is worth keeping visible.
    std::uint32_t handled_dos_create_count = 0;
    std::string last_dos_create_guest_path;
    std::string last_dos_create_host_path;
    std::string last_dos_create_virtual_path;
    bool last_dos_create_success = false;
    std::uint16_t last_dos_create_error = 0;
    std::uint16_t last_dos_create_handle = 0;
    std::uint16_t last_dos_create_attributes = 0;
    std::uint32_t handled_dos_write_count = 0;
    std::uint16_t last_dos_write_handle = 0;
    std::uint32_t last_dos_write_requested_bytes = 0;
    std::uint32_t last_dos_write_actual_bytes = 0;
    std::uint32_t last_dos_write_buffer = 0;
    bool last_dos_write_success = false;
    std::uint16_t last_dos_write_error = 0;
    std::uint32_t handled_dos_seek_count = 0;
    std::uint16_t last_dos_seek_handle = 0;
    std::uint8_t last_dos_seek_origin = 0;
    std::int32_t last_dos_seek_offset = 0;
    std::uint32_t last_dos_seek_position = 0;
    bool last_dos_seek_success = false;
    std::uint16_t last_dos_seek_error = 0;
    std::uint32_t handled_dos_close_count = 0;
    std::uint16_t last_dos_close_handle = 0;
    bool last_dos_close_success = false;
    std::uint16_t last_dos_close_error = 0;
    std::uint32_t handled_dos_ioctl_count = 0;
    std::uint8_t last_dos_ioctl_subfunction = 0;
    std::uint16_t last_dos_ioctl_handle = 0;
    bool last_dos_ioctl_success = false;
    std::uint16_t last_dos_ioctl_error = 0;
    std::uint16_t last_dos_ioctl_device_info = 0;
    std::uint32_t handled_dos_resize_count = 0;
    std::uint16_t last_dos_resize_selector = 0;
    std::uint16_t last_dos_resize_paragraphs = 0;
    bool last_dos_resize_success = false;
    std::uint16_t last_dos_resize_error = 0;
    std::uint32_t last_dos_resize_requested_end = 0;
    std::uint32_t last_dos_resize_allocator_end = 0;
    std::uint32_t handled_segment_load_count = 0;
    std::uint32_t last_segment_load_address = 0;
    std::uint32_t last_segment_load_opcode = 0;
    std::uint32_t last_segment_load_register = 0;
    std::uint32_t last_segment_load_selector = 0;
    std::uint32_t last_segment_load_source = 0;
    std::uint32_t handled_segment_load_register_counts[6] = {};
    SegmentLoadObservation segment_load;
    std::uint32_t handled_segment_store_count = 0;
    std::uint32_t last_segment_store_address = 0;
    std::uint32_t last_segment_store_opcode = 0;
    std::uint32_t last_segment_store_register = 0;
    std::uint32_t last_segment_store_selector = 0;
    std::uint32_t last_segment_store_destination = 0;
    std::uint32_t handled_segment_store_register_counts[6] = {};
    std::uint32_t handled_segment_memory_load_count = 0;
    std::uint32_t last_segment_memory_load_address = 0;
    std::uint32_t last_segment_memory_load_opcode = 0;
    std::uint32_t last_segment_memory_load_register = 0;
    std::uint32_t last_segment_memory_load_selector = 0;
    std::uint32_t last_segment_memory_load_offset = 0;
    std::uint32_t last_segment_memory_load_width = 0;
    std::uint32_t last_segment_memory_load_value = 0;
    std::uint32_t handled_low_memory_access_count = 0;
    std::uint32_t last_low_memory_access_address = 0;
    std::uint32_t last_low_memory_access_opcode = 0;
    std::uint32_t last_low_memory_access_esi = 0;
    std::uint32_t last_low_memory_access_edi = 0;
    std::uint32_t last_low_memory_access_destination = 0;
    std::uint32_t last_low_memory_access_value = 0;
    std::uint32_t low_memory_read_emulate_count = 0;
    std::uint32_t last_low_memory_read_emulate_address = 0;
    std::uint32_t last_low_memory_read_emulate_eip = 0;
    std::uint32_t last_low_memory_read_emulate_value = 0;
    std::uint32_t last_low_memory_read_emulate_reg = 0;
    std::uint32_t last_low_memory_fault_eip = 0;
    std::uint32_t last_low_memory_fault_address = 0;
    std::uint32_t low_memory_fault_repeat_count = 0;
    std::uint32_t last_low_memory_fault_tick = 0;
    // Task 475: SCAS/LODS/CMPS reads of low memory, which the MOV-only path
    // above rejected. `iteration_count` exceeds `service_count` because one
    // fault services a whole run of a REP-prefixed instruction.
    std::uint32_t low_memory_string_service_count = 0;
    std::uint32_t low_memory_string_iteration_count = 0;
    std::uint32_t last_low_memory_string_eip = 0;
    std::uint32_t last_low_memory_string_address = 0;
    std::uint32_t last_low_memory_string_mnemonic = 0;
    std::uint32_t last_low_memory_string_iterations = 0;
    std::uint32_t rep_movs_copy_failure_count = 0;
    std::uint32_t last_rep_movs_copy_failure_stage = 0;
    std::uint32_t last_rep_movs_copy_error = 0;
    std::uint32_t last_rep_movs_copy_source = 0;
    std::uint32_t last_rep_movs_copy_destination = 0;
    std::uint32_t last_rep_movs_copy_bytes = 0;
    std::vector<std::uint8_t> dos_environment_block;
    bool last_dos_environment_access_valid = false;
    std::uint32_t last_dos_environment_access_offset = 0;
    std::uint32_t last_dos_environment_entry_offset = 0;
    std::uint32_t last_dos_environment_value_length = 0;
    std::string last_dos_environment_entry_name;
    std::uint32_t handled_memory_store_count = 0;
    std::uint32_t last_memory_store_address = 0;
    std::uint32_t last_memory_store_opcode = 0;
    std::uint32_t last_memory_store_destination = 0;
    std::uint32_t last_memory_store_value = 0;
    std::uint32_t last_memory_store_width = 0;
    std::string last_memory_store_source_kind;
    bool last_memory_store_applied = false;
    std::uint32_t shadow_memory_write_count = 0;
    std::uint32_t shadow_memory_read_hit_count = 0;
    bool shadow_memory_range_valid = false;
    std::uint32_t shadow_memory_min_address = 0;
    std::uint32_t shadow_memory_max_address = 0;
    std::uint32_t last_traced_fpu_m32_value = 0;
    bool has_last_traced_fpu_m32_value = false;
    std::atomic<std::uint32_t> single_step_trace_count{0};
    // Task 337: an exclusive census of what the guest thread's exceptions
    // actually are, because the remedy for kernel transition cost -- now
    // 27.7-30.4% of Release wall clock (Task 336) -- differs completely by
    // class. The existing counters overlap (a single-step can be seen by both
    // the reentry scope and HandleSingleStepTrace), so they can only sketch it.
    //
    // Guest thread only, so plain counters; the VEH prologue is a hot path.
    std::uint32_t veh_single_step_exception_count = 0;
    std::uint32_t veh_breakpoint_exception_count = 0;
    std::uint32_t veh_access_violation_exception_count = 0;
    std::uint32_t veh_other_exception_count = 0;
    // Task 343: which codes those are. Task 342's policy change took this from
    // 1 to 1,931 per minute and the census only said "other", which names
    // nothing. Four distinct codes are enough: the population is expected to be
    // one or two.
    static constexpr std::uint32_t kOtherExceptionCodeCapacity = 4U;
    std::uint32_t veh_other_exception_codes[kOtherExceptionCodeCapacity] = {};
    std::uint32_t veh_other_exception_code_counts[
        kOtherExceptionCodeCapacity] = {};
    std::uint32_t veh_other_exception_code_overflow = 0;
    // How many consecutive single-step exceptions separate two INT3 boundaries.
    // 265,272 boundaries against 942,160 single-steps implies about 3.55, which
    // says the guest walks several instructions under TF before it re-enters
    // translated code; the distribution says whether that is a long tail or a
    // uniform cost, and those need different fixes.
    static constexpr std::uint32_t kSingleStepRunBucketCount = 8U;
    std::uint32_t veh_single_step_run_length = 0;
    std::uint32_t veh_single_step_run_buckets[kSingleStepRunBucketCount] = {};
    std::uint32_t veh_single_step_run_total = 0;
    std::uint32_t veh_single_step_run_max = 0;
    std::unique_ptr<SingleStepHotspotProfile>
        single_step_hotspot_profile;
    // Task 411: filled by the poll thread around its SuspendThread of the guest
    // thread and read only after that thread has stopped, so it needs no
    // synchronisation of its own. Allocated only when the census is enabled.
    std::unique_ptr<GuestPositionCensus> guest_position_census;
    // Task 421: sampled by the poll thread so a starved audio worker cannot
    // hide its own starvation.
    std::unique_ptr<CdAudioPositionCensus> cd_audio_position_census;
    // Task 422: the sequence of CD commands the guest issues, since the
    // existing telemetry keeps only the last one and cannot show a storm.
    std::unique_ptr<MscdexCommandTrace> mscdex_command_trace;
    // Task 323: the cycle scope owned by the current HandleSingleStepTrace
    // invocation, so regions measured in other translation units (the
    // TryResumeAotAfterHandledHle sub-stages) can attribute into the same
    // sample. Set and cleared only by that handler on the guest thread; null
    // whenever no single-step sample is open or the profile is disabled.
    SingleStepHotspotCycleScope* active_hotspot_scope = nullptr;
    // Task 323: guest-thread wall-clock buckets. Allocated only when
    // REPIU_EXECUTION_TIME_PROFILE is set, so the normal path pays one branch.
    std::unique_ptr<ExecutionTimeProfile> execution_time_profile;
    // Task 327: rendezvous timing across the guest and worker threads. Shares
    // the REPIU_EXECUTION_TIME_PROFILE opt-in.
    std::unique_ptr<AotWorkerTimingProfile> aot_worker_timing;
    // Task 482: the return handler split into five mutually exclusive stages
    // and a residual. Own opt-in (REPIU_AOT_RETURN_STAGE_PROFILE) so this pass
    // and the Glide-ordinal pass never instrument the same run. Small enough
    // to hold by value; the disabled path never writes it.
    AotReturnStageProfile aot_return_stage_profile;
    // Route A sizing (native region execution). Of every single-stepped guest
    // instruction, how many are HLE-sensitive (segment op / INT / IO / string /
    // privileged) and would still require a trap under selective-breakpoint
    // region execution. Native-region speedup ceiling ~=
    // single_step_trace_count / routea_sensitive_count. The segment sub-count
    // isolates how much of that is segmentation specifically.
    std::atomic<std::uint32_t> routea_sensitive_count{0};
    std::atomic<std::uint32_t> routea_segment_sensitive_count{0};
    std::atomic<std::uint32_t> single_step_eip{0};
    std::atomic<std::uint32_t> single_step_eax{0};
    std::atomic<std::uint32_t> single_step_ebx{0};
    std::atomic<std::uint32_t> single_step_ecx{0};
    std::atomic<std::uint32_t> single_step_edx{0};
    std::atomic<std::uint32_t> single_step_esi{0};
    std::atomic<std::uint32_t> single_step_edi{0};
    std::atomic<std::uint32_t> single_step_esp{0};
    std::atomic<std::uint32_t> single_step_ebp{0};
    std::atomic<std::uint32_t> single_step_eflags{0};
    std::atomic<std::uint32_t> single_step_cs{0};
    std::atomic<std::uint32_t> single_step_ds{0};
    std::atomic<std::uint32_t> single_step_es{0};
    std::atomic<std::uint32_t> single_step_ss{0};
    std::atomic<std::uint32_t> single_step_fs{0};
    std::atomic<std::uint32_t> single_step_gs{0};
    std::atomic<std::uint32_t> diagnostic_progress_count{0};
    std::uint32_t diagnostic_poll_iteration_count = 0;
    std::uint32_t diagnostic_quiet_iteration_count = 0;
    std::atomic<std::uint32_t> exception_dispatch_entry_count{0};
    std::atomic<std::uint32_t> exception_dispatch_exit_count{0};
    std::atomic<std::uint32_t> exception_dispatch_last_eip{0};
    // Task 296: count and last-seen bit patterns of malformed EXCEPTION_POINTERS
    // handed to the VEH (non-null but unreadable ContextRecord/ExceptionRecord),
    // which would otherwise crash the dispatcher and mask the primary exception.
    std::atomic<std::uint32_t> exception_dispatch_malformed_count{0};
    std::atomic<std::uint32_t> exception_dispatch_last_bad_context{0};
    std::atomic<std::uint32_t> exception_dispatch_last_bad_record{0};
    std::atomic<std::uint32_t> live_telemetry_heartbeat{0};
    std::atomic<std::uint32_t> live_telemetry_phase{0};
    SharedLiveTelemetry* shared_live_telemetry = nullptr;
    void* vectored_handler = nullptr;
    std::unordered_map<std::uint32_t, std::uint8_t> shadow_memory;
    std::array<ShadowWriteProvenance, kShadowWriteProvenanceCapacity>
        shadow_write_provenance = {};
    std::uint32_t shadow_write_provenance_count = 0;
    bool pending_shadow_allocation_valid = false;
    std::uint32_t pending_shadow_allocation_size = 0;
    bool shadow_zero_payload_valid = false;
    std::uint32_t shadow_zero_payload_begin = 0;
    std::uint32_t shadow_zero_payload_end = 0;
    bool boundary_object_chain_valid = false;
    std::uint32_t boundary_object_chain_base = 0;
    std::uint32_t boundary_object_chain_frontier = 0;
    std::uint64_t boundary_object_chain_limit = 0;
    std::uint16_t guest_es = 0;
    std::uint16_t guest_ss = 0;
    // Task 712. The loader's initial stack selector, under which the loader's
    // own `ESP` is linear. An explicit `SS:` override folds base 0 while the
    // guest is on it, and the descriptor base on any stack it switches to.
    std::uint16_t flat_stack_selector = 0;
    // Task 717. The loader's initial data selector, flat in the same sense.
    std::uint16_t flat_data_selector = 0;
    std::uint16_t guest_ds = 0;
    std::uint16_t guest_fs = 0;
    std::uint16_t guest_gs = 0;
    repiu::runtime::SelectorTable selector_table;
    repiu::runtime::SelectorAllocator dpmi_selector_allocator;
    repiu::runtime::DosLowMemory dos_low_memory;
    std::array<DosInterruptVectorShadow, 256> dos_interrupt_vectors = {};
    std::array<DpmiInterruptVectorShadow, 256> dpmi_interrupt_vectors = {};
    repiu::hle::PitChannel0 pit_channel0;
    std::atomic<bool> timer_interrupt_pending{false};
    std::atomic_flag timer_tick_delivery_lock = ATOMIC_FLAG_INIT;
    // Task 366: how many ticks the schedule owed against how many the guest
    // actually received. Always on; the bounded backlog that preserves owed
    // ticks is opt-in.
    TimerTickDeliveryCounters timer_tick_delivery;
    // Task 735. IRQ0's in-service bit: set by an injection, cleared by the
    // ISR's EOI, so a tick owed during the ISR waits instead of nesting.
    PicTimerInService pic_timer_in_service;
    // Task 762. The injected frames that return through the return pad, and
    // what each of them interrupted.
    TimerReturnPad timer_return_pad;
    std::atomic<std::uint32_t> last_timer_injection_ticks{0};
    // Task 351: expired PIT ticks not yet attributed to the safe-point source
    // that successfully delivers them. Deferred traps leave this untouched.
    std::atomic<std::uint32_t> timer_interrupt_due_ticks{0};
    // Task i018. Where INT8 injections entered the handler on the direct
    // model: at its cache address, at a cache entry translated on demand, or
    // at the raw guest address (native execution until the first fault).
    std::uint32_t timer_handler_entry_cache_count = 0;
    std::uint32_t timer_handler_entry_translated_count = 0;
    std::uint32_t timer_handler_entry_native_count = 0;
    std::uint32_t timer_interrupt_chain_hle_count = 0;
    std::uint32_t timer_interrupt_chain_hle_source = 0;
    std::uint32_t timer_interrupt_chain_hle_pointer = 0;
    std::uint32_t timer_interrupt_chain_hle_offset = 0;
    std::uint16_t timer_interrupt_chain_hle_selector = 0;
    char hle_stdout_output[4096] = {};
    std::uint32_t hle_stdout_output_size = 0;
    char hle_stderr_output[4096] = {};
    std::uint32_t hle_stderr_output_size = 0;
    std::string hle_message;

    struct RealModeBlock
    {
        std::uint16_t selector = 0;
        std::uint32_t offset = 0;
        std::uint32_t size = 0;
        bool active = false;
    };
    static constexpr std::size_t kRealModeBlockCapacity = 32;
    std::array<RealModeBlock, kRealModeBlockCapacity> dpmi_real_mode_blocks = {};
    std::uint32_t dpmi_low_memory_bump_offset = 0x1000U;
    std::uint32_t debug_emulate_stage = 0;
    std::uint32_t debug_emulate_decode_result = 0;
    std::uint32_t debug_emulate_calculated_address = 0;

    // Task 585. The selector words the emitted guard slots compare against,
    // in memory their 32-bit operand can name. `shadow_selectors` is null when
    // the reservation failed; every reader treats that as "no shadow", which
    // closes the guard slots rather than pointing them somewhere.
    repiu::runtime::AotShadowSelectorReservation shadow_selector_reservation{};
    repiu::runtime::AotShadowSelectorBlock* shadow_selectors = nullptr;

    // Task 586. How many faults landed inside a guard slot's compare. This
    // should stay zero: a compare that faults leaves EFLAGS on the guest stack
    // (see aot_guard_compare_fault.h), and Task 585 removed the only known way
    // to reach it.
    std::atomic<std::uint32_t> aot_guard_compare_fault_count{0};

    ~ThreadContext()
    {
        repiu::runtime::ReleaseAotShadowSelectorBlock(
            shadow_selector_reservation);
        shadow_selector_reservation = {};
        shadow_selectors = nullptr;
        ReleaseGlideLfbGuestStorage(&glide_lfb_guest_storage);
    }
};
} // namespace repiu::engine

