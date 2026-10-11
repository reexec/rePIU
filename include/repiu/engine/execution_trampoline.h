#ifndef REPIU_ENGINE_EXECUTION_TRAMPOLINE_H_
#define REPIU_ENGINE_EXECUTION_TRAMPOLINE_H_

#include "repiu/engine/runtime_memory_policy.h"
#include "repiu/runtime/guest_context.h"
#include "repiu/runtime/execution_backend.h"
#include "repiu/engine/aot_code_cache.h"
#include "repiu/engine/aot_boundary_provenance.h"
#include "repiu/engine/aot_retired_trap_profile.h"
#include "repiu/engine/single_step_hotspot_profile.h"
#include "repiu/engine/guest_position_census.h"
#include "repiu/engine/execution_time_profile.h"
#include "repiu/engine/aot_worker_timing.h"
#include "repiu/engine/glide_buffer_swap_timing.h"
#include "repiu/engine/glide_lfb_timing.h"
#include "repiu/engine/glide_lfb_write_footprint.h"
#include "repiu/engine/glide_lfb_native_store_census.h"
#include "repiu/engine/glide_lfb_lock_interval_census.h"
#include "repiu/engine/glide_lfb_staging_shadow.h"
#include "repiu/engine/glide_gate_timing.h"
#include "repiu/engine/aot_return_stage_profile.h"
#include "repiu/engine/glide_ordinal_timing.h"
#include "repiu/engine/glide_gl_error_policy.h"
#include "repiu/engine/glide_setter_phase_timing.h"
#include "repiu/engine/glide_swap_interval_policy.h"
#include "repiu/engine/glide_texture_census.h"
#include "repiu/engine/out_of_arena_step_census.h"
#include "repiu/engine/glide_setter_state_census.h"
#include "repiu/engine/glide_async_present.h"
#include "repiu/engine/glide_draw_batch.h"
#include "repiu/engine/glide_setter_state_cache.h"
#include "repiu/engine/pic_timer_in_service.h"
#include "repiu/engine/timer_return_pad.h"
#include "repiu/engine/piu10_song_clock_census.h"
#include "repiu/engine/timer_tick_delivery.h"
#include "repiu/engine/piu10_mp3_audio_out.h"
#include "repiu/engine/aot_boundary_opcode_census.h"
#include "repiu/hle/dos_file_system.h"
#include "repiu/hle/glide_implementation_issue.h"
#include "repiu/exe/dos16m_bound_module.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace repiu::engine
{

constexpr std::uint32_t kPortIoTraceCapacity = 16;
constexpr std::uint32_t kDosPathTraceCapacity = 16;
constexpr std::uint32_t kDosFileIoTraceCapacity = 64;
constexpr std::uint32_t kDosFileIoPrefixCapacity = 16;
constexpr std::uint32_t kDosTerminationStackCapacity = 128;
constexpr std::uint32_t kExceptionStackDwordCapacity = 96;
constexpr std::uint32_t kBreakpointByteWindowCapacity = 32;
constexpr std::uint32_t kAllocatorProbeTraceCapacity = 16;
constexpr std::uint32_t kAllocatorControlFlowTraceCapacity = 32;
constexpr std::uint32_t kSegmentLoadTraceCapacity = 16;
constexpr std::uint32_t kGlideTextureGateTraceCapacity = 16;
constexpr std::uint32_t kGlideVertexDwordCount = 18;
constexpr std::uint32_t kGlideTriangleTraceCapacity = 16;
constexpr std::uint32_t kWin32GlideProducerVertexDwordCount = 15;
constexpr std::uint32_t kDeferredPortIoLimit = 65536;
constexpr std::uint32_t kAotDbtHleFallbackReasonCount = 6;
constexpr std::uint32_t kExecutionProbeRegisterCount = 7;
constexpr std::uint32_t kExecutionProbeMemoryByteCount = 32;

// Task 281 introduced this exclusive cause model for RET miss dispatch; Task 282
// shares it with indirect call/jump miss dispatch, which fails for the same
// reasons. `kUnreadableSource` is the return target on the guest stack for RET
// and the register or ModRM memory operand for indirect transfers. Counters stay
// per path so the two dispatchers remain separately comparable.
enum class AotDbtDispatchFallbackReason : std::uint32_t
{
    kInvalidSite = 0,
    kInvalidState,
    kInvalidInstruction,
    kUnreadableSource,
    kZeroTarget,
    kHleTarget,
    kQuarantinedTarget,
    kNonGuestTarget,
    kTranslationFailure,
    kUnknown,
    kCount,
};

constexpr std::uint32_t kAotDbtDispatchFallbackReasonCount =
    static_cast<std::uint32_t>(AotDbtDispatchFallbackReason::kCount);

enum BreakpointStateFlag : std::uint32_t
{
    kBreakpointAotReentryPending = 1U << 0U,
    kBreakpointSingleStepTrace = 1U << 1U,
    kBreakpointNativeFastPath = 1U << 2U,
    kBreakpointNativeLinearSpan = 1U << 3U,
};

struct UnhandledBreakpointEvidence
{
    bool valid = false;
    std::uint32_t code = 0;
    std::uint32_t exception_address = 0;
    std::uint32_t entry_eip = 0;
    std::uint32_t final_eip = 0;
    std::uint32_t entry_esp = 0;
    std::uint32_t final_esp = 0;
    std::uint32_t entry_eflags = 0;
    std::uint32_t entry_dr6 = 0;
    std::uint32_t entry_dr7 = 0;
    std::uint32_t entry_state_flags = 0;
    std::uint32_t final_state_flags = 0;
    std::uint32_t entry_aot_reentry_cache_address = 0;
    std::uint32_t entry_aot_return_dispatch_count = 0;
    std::uint32_t final_aot_return_dispatch_count = 0;
    std::uint32_t entry_aot_last_return_source = 0;
    std::uint32_t entry_aot_last_return_target = 0;
    std::uint32_t final_aot_last_return_source = 0;
    std::uint32_t final_aot_last_return_target = 0;
    bool exception_address_in_aot_cache = false;
    bool entry_eip_in_aot_cache = false;
    bool exception_exact_mapping_valid = false;
    bool exception_previous_mapping_valid = false;
    bool eip_exact_mapping_valid = false;
    bool eip_previous_mapping_valid = false;
    std::uint32_t exception_exact_guest = 0;
    std::uint32_t exception_previous_guest = 0;
    std::uint32_t eip_exact_guest = 0;
    std::uint32_t eip_previous_guest = 0;
    bool exception_exact_provenance_valid = false;
    bool exception_previous_provenance_valid = false;
    bool eip_exact_provenance_valid = false;
    bool eip_previous_provenance_valid = false;
    std::uint32_t exception_exact_provenance = 0;
    std::uint32_t exception_previous_provenance = 0;
    std::uint32_t eip_exact_provenance = 0;
    std::uint32_t eip_previous_provenance = 0;
    std::uint32_t exception_window_base = 0;
    std::uint32_t exception_window_count = 0;
    std::uint8_t exception_window[kBreakpointByteWindowCapacity] = {};
    std::uint32_t eip_window_base = 0;
    std::uint32_t eip_window_count = 0;
    std::uint8_t eip_window[kBreakpointByteWindowCapacity] = {};
    std::uint32_t stack_dwords[4] = {};
    std::uint32_t stack_valid_mask = 0;
};

struct GlideTriangleObservation
{
    bool valid = false;
    std::uint32_t pointers[3] = {};
    bool pointer_readable[3] = {};
    std::uint32_t dwords[3][kGlideVertexDwordCount] = {};
};

struct GlideTriangleTraceEntry
{
    bool valid = false;
    std::uint32_t sequence = 0;
    std::uint32_t pointers[3] = {};
    bool pointer_readable[3] = {};
    std::uint32_t dwords[3][kWin32GlideProducerVertexDwordCount] = {};
};

struct GlideTextureGateTraceEntry
{
    bool valid = false;
    std::uint32_t sequence = 0;
    std::uint16_t ordinal = 0;
    bool is_max_address = false;
    std::uint32_t entry_eip = 0;
    std::uint32_t entry_esp = 0;
    std::uint32_t return_address = 0;
    std::uint32_t tmu = 0;
    std::uint32_t entry_eax = 0;
    std::uint32_t return_eax = 0;
    std::uint32_t planned_return_esp = 0;
};
constexpr std::uint32_t kExecutionTraceCapacity = 64;

// One capture per single-stepped instruction inside a guest code range
// (see RecordExecutionTrace). `value_at_esp_offset` is read relative to the
// live ESP at capture time, not a hardcoded absolute address, so it stays
// correct across stack reuse between calls to the traced function.
struct ExecutionTraceEntry
{
    std::uint32_t sequence = 0;
    std::uint32_t eip = 0;
    std::uint32_t esp = 0;
    std::uint32_t value_at_esp_offset = 0;
};

struct X86ExecutionSnapshot
{
    bool captured = false;
    std::uint32_t eip = 0;
    std::uint32_t eax = 0;
    std::uint32_t ebx = 0;
    std::uint32_t ecx = 0;
    std::uint32_t edx = 0;
    std::uint32_t esi = 0;
    std::uint32_t edi = 0;
    std::uint32_t esp = 0;
    std::uint32_t ebp = 0;
    std::uint32_t eflags = 0;
    std::uint16_t cs = 0;
    std::uint16_t ds = 0;
    std::uint16_t es = 0;
    std::uint16_t ss = 0;
    std::uint16_t fs = 0;
    std::uint16_t gs = 0;
};

struct ExecutionProbeMemoryWindow
{
    bool valid = false;
    std::uint32_t address = 0;
    std::uint8_t bytes[kExecutionProbeMemoryByteCount] = {};
};

struct PortIoTraceEntry
{
    bool valid = false;
    std::uint32_t sequence = 0;
    std::uint32_t address = 0;
    std::uint32_t opcode = 0;
    std::uint32_t port = 0;
    std::uint32_t width = 0;
    std::uint32_t value = 0;
    bool is_input = false;
    bool handled = false;
};

struct PortIoObservation
{
    std::uint32_t observed_count = 0;
    std::uint32_t last_address = 0;
    std::uint32_t last_opcode = 0;
    std::uint32_t last_port = 0;
    std::uint32_t last_width = 0;
    std::uint32_t last_value = 0;
    bool last_is_input = false;
    bool last_handled = false;
    std::string last_result;
    std::uint32_t trace_stored_count = 0;
    bool trace_limit_reached = false;
    std::uint32_t opcode_counts[256] = {};
    std::uint32_t input_count = 0;
    std::uint32_t output_count = 0;
    std::uint32_t handled_count = 0;
    std::uint32_t unhandled_count = 0;
    // Task 403: decomposition of the JAMMA input read. Task 402 measured
    // 16,000-29,000 cycles per port I/O call but left "how much of that is
    // GetAsyncKeyState" unresolved, which is the premise any fix depends on.
    // `jamma_scan_cycles` covers only the ReadJammaPort8 loop; `key_query_count`
    // counts individual GetAsyncKeyState calls.
    std::uint64_t jamma_scan_cycles = 0;
    std::uint32_t jamma_scan_count = 0;
    std::uint32_t key_query_count = 0;
    std::uint64_t jamma_timeline_edge_count = 0;
    std::uint64_t jamma_timeline_history_pruned_count = 0;
    std::uint64_t jamma_timeline_history_overflow_count = 0;
    std::uint64_t jamma_timeline_history_coverage_miss_count = 0;
    std::uint32_t jamma_timeline_history_peak_size = 0;
    std::uint64_t jamma_timeline_due_enqueued_count = 0;
    std::uint64_t jamma_timeline_due_overflow_count = 0;
    std::uint64_t jamma_timeline_replay_begin_count = 0;
    std::uint64_t jamma_timeline_replay_read_count = 0;
    std::uint64_t jamma_timeline_missing_due_count = 0;
    std::uint64_t jamma_timeline_frame_retire_count = 0;
    std::uint64_t jamma_timeline_frame_overflow_count = 0;
    std::uint32_t jamma_timeline_active_frame_depth = 0;
    std::uint64_t jamma_timeline_frame_end_count = 0;
    std::uint64_t jamma_timeline_frame_stale_count = 0;
    std::uint64_t jamma_timeline_latest_state_read_count = 0;
    PortIoTraceEntry trace[kPortIoTraceCapacity];
};

struct DosPathTraceEntry
{
    bool valid = false;
    std::uint32_t sequence = 0;
    std::string service;
    std::string guest_path;
    std::string virtual_path;
    std::string host_path;
    std::string result;
    std::uint16_t dos_error = 0;
    std::uint8_t drive = 0;
    std::uint8_t access_mode = 0;
};

struct DosPathObservation
{
    std::uint32_t observed_count = 0;
    std::uint32_t trace_stored_count = 0;
    bool trace_limit_reached = false;
    std::uint32_t opcode_counts[256] = {};
    std::uint32_t input_count = 0;
    std::uint32_t output_count = 0;
    std::uint32_t handled_count = 0;
    std::uint32_t unhandled_count = 0;
    DosPathTraceEntry trace[kDosPathTraceCapacity];
};

struct DosFileIoTraceEntry
{
    bool valid = false;
    std::uint32_t sequence = 0;
    std::string operation;
    std::string host_path;
    std::uint16_t handle = 0;
    std::uint8_t origin = 0;
    std::int32_t seek_offset = 0;
    std::uint32_t position_before = 0;
    std::uint32_t position_after = 0;
    std::uint32_t requested_bytes = 0;
    std::uint32_t actual_bytes = 0;
    std::uint16_t dos_error = 0;
    std::uint8_t prefix[kDosFileIoPrefixCapacity] = {};
    std::uint32_t prefix_size = 0;
    std::uint32_t guest_eip = 0;
    std::uint32_t guest_esp = 0;
    // Task 711. Twenty-four words rather than eight. Eight reached the C
    // runtime's own frames -- the read wrapper, the buffer fill, and fread's
    // saved registers -- and stopped six words short of fread's return into the
    // game, which is the frame a file-reading loop actually lives in. The fill
    // and the report both take the array's size, so this is the only change.
    std::uint32_t guest_stack[24] = {};
};

struct DosFileIoObservation
{
    std::uint32_t observed_count = 0;
    std::uint32_t trace_stored_count = 0;
    bool trace_wrapped = false;
    // Task 374: reads against host opens. These were one-to-one before the
    // handle cache, which is what made a 4 KB read cost milliseconds.
    std::uint32_t read_count = 0;
    std::uint32_t host_open_count = 0;
    DosFileIoTraceEntry trace[kDosFileIoTraceCapacity];
};

struct AllocatorProbeTraceEntry
{
    bool valid = false;
    std::uint32_t sequence = 0;
    std::uint32_t eax = 0;
    std::uint32_t esi = 0;
    std::uint32_t source = 0;
    std::uint16_t ds = 0;
    bool pending_before = false;
    std::uint32_t pending_size_before = 0;
    bool pending_after = false;
    std::uint32_t pending_size_after = 0;
    std::string result;
};

struct AllocatorProbeObservation
{
    std::uint32_t observed_count = 0;
    std::uint32_t trace_stored_count = 0;
    bool trace_wrapped = false;
    AllocatorProbeTraceEntry
        trace[kAllocatorProbeTraceCapacity];
};

struct AllocatorControlFlowTraceEntry
{
    bool valid = false;
    std::uint32_t sequence = 0;
    std::uint32_t eip_offset = 0;
    std::uint32_t seh_code = 0;
    std::uint8_t opcode[4] = {};
    std::uint32_t eax = 0;
    std::uint32_t ebx = 0;
    std::uint32_t edx = 0;
    std::uint32_t esi = 0;
    std::uint32_t edi = 0;
    std::uint32_t eflags = 0;
    bool pending_valid = false;
    std::uint32_t pending_size = 0;
    bool read_valid = false;
    std::uint32_t read_address = 0;
    std::uint32_t read_value = 0;
    bool read_explicit_shadow = false;
    bool read_zero_backed = false;
    bool writer_valid = false;
    std::uint32_t writer_sequence = 0;
    std::uint32_t writer_eip_offset = 0;
    std::uint32_t writer_opcode = 0;
    std::uint32_t writer_destination = 0;
    std::uint32_t writer_value = 0;
    std::uint32_t writer_width = 0;
};

struct AllocatorControlFlowObservation
{
    std::uint32_t observed_count = 0;
    std::uint32_t trace_stored_count = 0;
    bool trace_wrapped = false;
    bool null_link_transition_valid = false;
    AllocatorControlFlowTraceEntry null_link_transition;
    bool poison_link_transition_valid = false;
    AllocatorControlFlowTraceEntry poison_link_transition;
    bool root_transition_valid = false;
    AllocatorControlFlowTraceEntry root_transition;
    AllocatorControlFlowTraceEntry
        trace[kAllocatorControlFlowTraceCapacity];
};

struct SegmentLoadTraceEntry
{
    bool valid = false;
    std::uint32_t sequence = 0;
    std::uint32_t eip_offset = 0;
    std::uint8_t segment_register = 0;
    std::uint16_t selector = 0;
    std::uint32_t source = 0;
};

struct SegmentLoadObservation
{
    std::uint32_t observed_count = 0;
    std::uint32_t trace_stored_count = 0;
    bool trace_wrapped = false;
    SegmentLoadTraceEntry trace[kSegmentLoadTraceCapacity];
};

struct AotReturnTraceEntry
{
    std::uint32_t source = 0;
    std::uint32_t actual_target = 0;
    std::uint32_t expected_target = 0;
    std::uint32_t esp = 0;
    bool matches = false;
};

constexpr std::uint32_t kAotReturnTraceCapacity = 16;

struct AotTransferTraceEntry
{
    std::uint32_t source = 0;
    std::uint32_t target = 0;
    bool is_call = false;
};

constexpr std::uint32_t kAotTransferTraceCapacity = 32;

enum class AotTransferOrigin : std::uint32_t
{
    kVeh = 0,
    kHost = 1,
};

enum class AotCallReturnTraceEventKind : std::uint32_t
{
    kCall = 0,
    kReturn = 1,
};

struct AotCallReturnTraceEntry
{
    std::uint32_t sequence = 0;
    AotCallReturnTraceEventKind kind =
        AotCallReturnTraceEventKind::kCall;
    AotTransferOrigin origin = AotTransferOrigin::kVeh;
    std::uint32_t call_sequence = 0;
    std::uint32_t source = 0;
    std::uint32_t target = 0;
    std::uint32_t return_address = 0;
    std::uint32_t esp = 0;
    std::uint32_t expected_source = 0;
    std::uint32_t expected_target = 0;
    std::uint32_t expected_return_address = 0;
    std::uint32_t expected_esp = 0;
    bool correlated = false;
    bool target_matches = false;
    bool esp_matches = false;
};

constexpr std::uint32_t kAotCallReturnTraceCapacity = 256;

struct MinimalExecutionAttempt
{
    bool valid = false;
    bool supported = false;
    bool attempted = false;
    bool returned = false;
    bool exception_caught = false;
    bool timed_out = false;
    bool stall_timed_out = false;
    bool quit_requested = false;
    // Task 507. True except on the one path that can leave the guest thread
    // running: a timeout or quit request whose recovery attempt was refused or
    // went unanswered. `timed_out` alone cannot stand in for this -- it is true
    // for every interruption on this path, including the ones that did recover
    // the thread. A caller freeing memory the guest might still touch (the AOT
    // cache, the relocated image) has to gate on this rather than on
    // `timed_out`.
    bool guest_thread_stopped = true;
    bool guest_stack_switch_supported = false;
    bool guest_stack_switch_attempted = false;
    std::uint32_t entry_address = 0;
    std::uint32_t guest_stack_initial_esp = 0;
    std::uint32_t guest_stack_return_esp = 0;
    std::uint32_t seh_exception_code = 0;
    std::uint32_t seh_exception_address = 0;
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
    UnhandledBreakpointEvidence unhandled_breakpoint_evidence;
    std::uint32_t handled_fatal_breakpoint_count = 0;
    std::uint32_t last_fatal_breakpoint_address = 0;
    std::uint32_t last_fatal_message_address = 0;
    std::string last_fatal_message;
    bool fatal_halt_reached = false;
    X86ExecutionSnapshot timeout_snapshot;
    X86ExecutionSnapshot last_single_step_snapshot;
    std::uint32_t single_step_trace_count = 0;
    // Task 337: exclusive census of the guest thread's exceptions plus the
    // length distribution of consecutive single-step runs.
    std::uint32_t veh_single_step_exception_count = 0;
    std::uint32_t veh_breakpoint_exception_count = 0;
    std::uint32_t veh_access_violation_exception_count = 0;
    std::uint32_t veh_other_exception_count = 0;
    // Task 343: the distinct codes behind "other".
    std::uint32_t veh_other_exception_codes[4] = {};
    std::uint32_t veh_other_exception_code_counts[4] = {};
    std::uint32_t veh_other_exception_code_overflow = 0;
    std::uint32_t veh_single_step_run_total = 0;
    std::uint32_t veh_single_step_run_max = 0;
    std::uint32_t veh_single_step_run_buckets[8] = {};
    // Task 340: why the post-HLE return to the cache fails, by reason.
    std::uint32_t hle_reentry_reject_not_pending = 0;
    std::uint32_t hle_reentry_reject_segment_write = 0;
    std::uint32_t hle_reentry_reject_outside_arena = 0;
    std::uint32_t hle_reentry_reject_quarantined = 0;
    std::uint32_t hle_reentry_reject_cache_miss = 0;
    std::uint32_t hle_reentry_reject_span_unsafe = 0;
    std::uint32_t hle_reentry_success = 0;
    // Task 346: returns that proceeded after a segment write.
    std::uint32_t hle_reentry_segment_write_resumed = 0;
    // Task 341: what quarantined each of the first few pages.
    struct QuarantineTraceEntry
    {
        std::uint32_t page = 0;
        std::uint32_t source = 0;
        std::uint32_t destination = 0;
        std::uint32_t byte_count = 0;
    };
    QuarantineTraceEntry quarantine_trace[16] = {};
    std::uint32_t quarantine_trace_count = 0;
    std::uint32_t quarantine_unknown_source_count = 0;
    // Task 342: same-page writes that retired without quarantining.
    std::uint32_t quarantine_deferred_count = 0;
    std::uint32_t guest_page_write_history_overflow = 0;
    // Task 404: why the re-translation that would have published the next
    // generation failed, and whether that failure quarantined the page.
    struct GenerationFailureTraceEntry
    {
        std::uint32_t target = 0;
        std::uint32_t page = 0;
        bool quarantined = false;
        bool terminal = false;
        char message[96] = {};
    };
    GenerationFailureTraceEntry generation_failure_trace[8] = {};
    std::uint32_t generation_failure_trace_count = 0;
    std::uint32_t generation_failure_trace_overflow = 0;
    // Task 405: which guest addresses issue port I/O, and how many of those
    // executions came from the AOT cache rather than the arena.
    struct PortIoAddressCensusEntry
    {
        std::uint32_t guest_address = 0;
        std::uint32_t count = 0;
        std::uint32_t cache_count = 0;
        // Task 406: opt-in under `REPIU_PORT_IO_CENSUS_MAPPING`; zero when off.
        std::uint32_t mapped_count = 0;
        std::uint32_t reentry_pending_count = 0;
        // Task 408: this address's first arena-entry transition, plus how many
        // it had. Flags: bit 0 prev-in-cache, 1 trap flag, 2 re-entry pending,
        // 3 legacy fallback, 4 single-step trace.
        std::uint32_t entry_transition_count = 0;
        std::uint32_t entry_previous_code = 0;
        std::uint32_t entry_previous_eip = 0;
        std::uint8_t entry_flags = 0;
        // Task 409: predecessor class of every transition, not just the first.
        std::uint32_t entry_prev_single_step = 0;
        std::uint32_t entry_prev_breakpoint = 0;
        std::uint32_t entry_prev_access_violation = 0;
        std::uint32_t entry_prev_other = 0;
        // Task 410: which VEH exit resumed the guest after that predecessor,
        // and at which EIP.
        std::uint8_t entry_previous_exit_site = 0;
        std::uint32_t entry_previous_exit_eip = 0;
    };
    PortIoAddressCensusEntry port_io_address_census[32] = {};
    std::uint32_t port_io_address_census_size = 0;
    std::uint32_t port_io_address_census_overflow = 0;
    // Task 741: where breakpoint exceptions come from. See
    // ThreadContext::BreakpointSiteCensusEntry.
    struct BreakpointSiteCensusEntry
    {
        std::uint32_t host_address = 0;
        std::uint32_t guest_address = 0;
        bool in_cache = false;
        bool guest_mapped = false;
        std::uint32_t count = 0;
        std::uint8_t exit_sites[4] = {};
        std::uint32_t exit_counts[4] = {};
        std::uint32_t exit_overflow = 0;
    };
    // The 32 busiest, in count order; `distinct` is how many addresses the
    // whole table held.
    BreakpointSiteCensusEntry breakpoint_site_census[32] = {};
    std::uint32_t breakpoint_site_census_size = 0;
    std::uint32_t breakpoint_site_census_distinct = 0;
    std::uint32_t breakpoint_site_census_overflow = 0;
    // Task 741: the HLE reentry memo, by kind (segment probe, span safety,
    // long-mode identity).
    std::uint32_t aot_reentry_memo_hits[3] = {};
    std::uint32_t aot_reentry_memo_misses[3] = {};
    std::uint32_t aot_reentry_memo_generation = 0;
    // Task 407: how free-running arena execution is entered, recorded once per
    // transition rather than once per steady-state fault.
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
    ArenaPortIoEntryTraceEntry arena_port_io_entry_trace[16] = {};
    std::uint32_t arena_port_io_entry_trace_count = 0;
    // Task 410: every single step taken at an arena EIP, by the VEH exit that
    // consumed it, with the population total beside it so the sum is checkable.
    // Sized from the enumeration in
    // src/platform/win32/execution/veh_exit_site.h, which the platform layer
    // static-asserts against when it fills this in.
    static constexpr std::uint32_t kVehExitSiteSnapshotCapacity = 64U;
    std::uint32_t veh_arena_single_step_count = 0;
    std::uint32_t
        veh_arena_single_step_exit_site_counts[kVehExitSiteSnapshotCapacity] =
            {};
    SingleStepHotspotProfileSnapshot
        single_step_hotspot_profile;
    // Task 411: where the guest thread actually was, sampled on wall-clock
    // intervals rather than at exception boundaries.
    GuestPositionCensusSnapshot guest_position_census;
    ExecutionTimeProfileSnapshot execution_time_profile;
    AotWorkerTimingSnapshot aot_worker_timing;
    // Task 482: the return handler bucket split into five stages, the residual
    // of the same window, and the Task 481 policy sites that produced the most
    // observations. Filled only while REPIU_AOT_RETURN_STAGE_PROFILE is set.
    AotReturnStageSnapshot aot_return_stage_profile;
    std::vector<AotReturnStageSiteObservation> aot_return_stage_sites;
    // Task 499: the memo table generated code probes on the return miss path.
    bool aot_direct_return_table_enabled = false;
    std::uint32_t aot_direct_return_table_entry_count = 0;
    std::uint32_t aot_direct_return_table_hit_count = 0;
    std::uint64_t aot_direct_return_table_insert_count = 0;
    std::uint64_t aot_direct_return_table_overwrite_count = 0;
    std::uint64_t aot_direct_return_table_clear_count = 0;
    std::uint32_t aot_direct_return_probe_site_count = 0;
    // Task 333: the Glide host-thread rendezvous split into waiting and work.
    GlideGateTimingSnapshot glide_gate_timing;
    // Task 419: how often a spin resolved that rendezvous before the condition
    // variable had to, with the budget that produced the counts.
    GlideRendezvousSpinSnapshot glide_rendezvous_spin;
    // Task 353: decoded gate and rendezvous time attributed by Glide ordinal.
    GlideOrdinalTimingSnapshot glide_ordinal_timing;
    // Task 354: guest grBufferSwap host work split around SDL presentation.
    GlideBufferSwapTimingSnapshot glide_buffer_swap_timing;
    // Task 724: grLfbLock staging seed split into readback and 565 encoding.
    GlideLfbTimingSnapshot glide_lfb_timing;
    // Task 725: byte-difference footprint of a write lock's staging bytes.
    GlideLfbWriteFootprintSnapshot glide_lfb_write_footprint;
    // Task 726: decoded explicit AOT stores overlapping a write-lock range.
    GlideLfbNativeStoreCensusSnapshot glide_lfb_native_store_census;
    // Task 728: whether the staging surface already held the frame buffer at
    // each write lock, and why the shadow was lost when it did not.
    GlideLfbStagingShadowSnapshot glide_lfb_staging_shadow;
    // Task 729: how the window before each write lock was filled.
    GlideLfbLockIntervalCensusSnapshot glide_lfb_lock_interval;
    // Task 364: repeated-versus-changing state-setter arguments, and the
    // OpenGL interval of the two leading setters split by phase.
    GlideSetterCensusSnapshot glide_setter_census;
    GlideSetterPhaseSnapshot glide_setter_phase_timing;
    // Task 365: how much of that repetition was actually elided.
    GlideSetterStateCacheSnapshot glide_setter_state_cache;
    GlideDrawBatchSnapshot glide_draw_batch;
    GlideAsyncPresentSnapshot glide_async_present;
    // Task 369: whether the per-call setter error check ran, and what the
    // once-per-frame replacement found.
    GlideGlErrorPolicySnapshot glide_gl_error_policy;
    // Task 371: swap interval override request and the driver's answer.
    GlideSwapIntervalPolicySnapshot glide_swap_interval_policy;
    // Task 375: texture upload attributes and dump accounting.
    GlideTextureCensusSnapshot glide_texture_census;
    // Task 376: single steps discarded outside the guest arena.
    OutOfArenaStepCensusSnapshot out_of_arena_step_census;
    // Task 366: timer ticks owed against timer ticks the guest received.
    TimerTickDeliverySnapshot timer_tick_delivery;
    // Task 740. Read after the guest thread stops.
    Piu10Mp3AudioStats piu10_mp3_stats;
    // Issue #58: the arrow jumps at each song clock overwrite.
    Piu10SongClockSummary piu10_song_clock;
    // Task 735. Read after the guest thread stops, so a plain copy.
    PicTimerInService pic_timer_in_service;
    // Task 762. Likewise.
    TimerReturnPad timer_return_pad;
    std::uint32_t native_fast_path_entry_count = 0;
    std::uint32_t native_fast_path_return_count = 0;
    std::uint32_t native_fast_path_cancel_count = 0;
    std::uint32_t native_fast_path_last_entry = 0;
    std::uint32_t native_fast_path_last_return = 0;
    std::uint32_t native_linear_span_entry_count = 0;
    std::uint32_t native_linear_span_boundary_count = 0;
    std::uint32_t native_linear_span_cancel_count = 0;
    std::uint32_t native_linear_span_instruction_total = 0;
    std::uint32_t native_linear_span_reject_count = 0;
    std::uint32_t native_linear_span_reject_cache_hit_count = 0;
    std::uint32_t native_linear_span_cancel_tf_count = 0;
    std::uint32_t native_linear_span_cancel_dr0_count = 0;
    std::uint32_t native_linear_span_cancel_dr1_count = 0;
    std::uint32_t native_linear_span_cancel_dr2_count = 0;
    std::uint32_t native_linear_span_cancel_dr3_count = 0;
    std::uint32_t native_linear_span_cancel_other_db_count = 0;
    std::uint32_t native_linear_span_cancel_tf_first_eip = 0;
    std::uint32_t native_linear_span_cancel_dr0_first_eip = 0;
    std::uint32_t native_linear_span_cancel_dr1_first_eip = 0;
    std::uint32_t native_linear_span_cancel_dr2_first_eip = 0;
    std::uint32_t native_linear_span_cancel_dr3_first_eip = 0;
    std::uint32_t native_linear_span_cancel_other_db_first_eip = 0;
    std::uint32_t native_linear_span_reject_cache_miss_count = 0;
    std::uint32_t native_linear_span_reject_cache_stale_count = 0;
    std::uint32_t native_linear_span_reject_cache_store_count = 0;
    std::uint32_t native_linear_span_reject_cache_capacity_skip_count = 0;
    std::uint32_t native_linear_span_write_fault_cancel_count = 0;
    std::uint32_t native_linear_span_last_cancel_code = 0;
    std::uint32_t native_linear_span_last_cancel_eip = 0;
    runtime::ExecutionBackend execution_backend =
        runtime::ExecutionBackend::kLegacy;
    bool aot_backend_active = false;
    std::uint32_t aot_cache_entry_count = 0;
    std::uint32_t aot_boundary_count = 0;
    // Task 445: inline-cache patches, all serviced on the guest thread.
    std::uint64_t aot_inline_cache_direct_patch_count = 0;
    // Per-reason breakdown of aot_boundary_count (Task 262); the five sum to it.
    std::uint32_t aot_boundary_return_count = 0;
    std::uint32_t aot_boundary_indirect_count = 0;
    std::uint32_t aot_boundary_direct_count = 0;
    std::uint32_t aot_boundary_conditional_count = 0;
    std::uint32_t aot_boundary_other_count = 0;
    std::uint32_t aot_breakpoint_provenance_counts[
        kAotCacheBreakpointProvenanceCount] = {};
    // Task 263(a): top-8 lead opcodes of the `other` boundary bucket (by count)
    // and the most recent `other` boundary sample.
    std::uint32_t aot_other_top_opcodes[8] = {};
    std::uint32_t aot_other_top_counts[8] = {};
    std::uint32_t aot_last_other_eip = 0;
    std::uint32_t aot_last_other_bytes = 0;
    // Task 367: the same samples resolved to real instructions. `effective` skips
    // legacy prefixes; `escape` is the second byte behind a `0F`.
    static constexpr std::size_t kAotOpcodeRankCount = 8U;
    AotOpcodeRank aot_effective_opcode_ranks[kAotOpcodeRankCount] = {};
    AotOpcodeRank aot_escape_opcode_ranks[kAotOpcodeRankCount] = {};
    std::uint32_t aot_opcode_census_samples = 0;
    std::uint32_t aot_opcode_census_escapes = 0;
    std::uint32_t aot_opcode_census_prefixed = 0;
    std::uint32_t aot_opcode_census_segment_prefixed = 0;
    std::uint32_t aot_opcode_census_operand_size_prefixed = 0;
    std::uint32_t aot_opcode_census_truncated = 0;
    std::uint32_t aot_opcode_census_prefix_overflow = 0;
    std::uint32_t aot_opcode_census_empty = 0;
    // Task 263(b): AOT residency proxy.
    std::uint32_t aot_residency_total = 0;
    std::uint32_t aot_residency_samples = 0;
    std::uint32_t aot_residency_max = 0;
    std::uint32_t aot_reentry_count = 0;
    std::uint32_t aot_legacy_fallback_count = 0;
    std::uint32_t aot_last_fallback_address = 0;
    std::uint32_t aot_dynamic_attempt_count = 0;
    std::uint32_t aot_dynamic_success_count = 0;
    std::uint32_t aot_dynamic_added_bytes = 0;
    std::uint32_t aot_dbt_hle_reentry_attempt_count = 0;
    std::uint32_t aot_dbt_hle_reentry_success_count = 0;
    std::uint32_t aot_dbt_hle_translation_attempt_count = 0;
    std::uint32_t aot_dbt_hle_translation_success_count = 0;
    std::uint32_t aot_dbt_hle_dispatch_entry_count = 0;
    std::uint32_t aot_dbt_hle_dispatch_attempt_count = 0;
    std::uint32_t aot_dbt_hle_dispatch_success_count = 0;
    std::uint32_t aot_dbt_hle_dispatch_fallback_count = 0;
    std::uint32_t aot_dbt_hle_dispatch_fallback_reason_counts[
        kAotDbtHleFallbackReasonCount] = {};
    std::uint32_t aot_dbt_hle_dispatch_last_source = 0;
    std::uint32_t aot_dbt_hle_dispatch_last_next = 0;
    std::uint32_t aot_dbt_hle_dispatch_last_bytes = 0;
    std::uint32_t aot_selector_guard_native_site_count = 0;
    std::uint32_t aot_selector_guard_hle_site_count = 0;
    std::uint32_t aot_selector_guard_unresolved_site_count = 0;
    std::uint32_t aot_selector_guard_hle_exit_count = 0;
    std::uint32_t aot_selector_guard_mismatch_count = 0;
    std::uint32_t aot_guarded_segment_pop_success_count = 0;
    std::uint32_t aot_guarded_segment_pop_fallback_count = 0;
    std::uint32_t aot_guarded_segment_load_success_count = 0;
    std::uint32_t aot_guarded_segment_load_fallback_count = 0;
    std::uint32_t aot_timer_safe_point_trap_count = 0;
    std::uint32_t aot_timer_safe_point_injected_count = 0;
    std::uint32_t aot_timer_safe_point_deferred_count = 0;
    // Task i018. INT8 handler entries by kind (see ThreadContext).
    std::uint32_t timer_handler_entry_cache_count = 0;
    std::uint32_t timer_handler_entry_translated_count = 0;
    std::uint32_t timer_handler_entry_native_count = 0;
    AotTimerSourceProfile aot_timer_source_profile;
    // `entry` counts C++ resolver entries; `attempt` is derived as
    // success + fallback so the accounting invariant also holds for a sample
    // whose graceful timeout landed inside the resolver (Task 281 open item).
    // `entry - attempt` is then the in-flight count at the observation point.
    std::uint32_t aot_dbt_return_entry_count = 0;
    std::uint32_t aot_dbt_return_attempt_count = 0;
    std::uint32_t aot_dbt_return_success_count = 0;
    std::uint32_t aot_dbt_return_fallback_count = 0;
    std::uint32_t aot_dbt_return_fallback_reason_counts[
        kAotDbtDispatchFallbackReasonCount] = {};
    std::uint32_t aot_indirect_dispatch_count = 0;
    std::uint32_t aot_inline_cache_patch_attempt_count = 0;
    std::uint32_t aot_inline_cache_patch_success_count = 0;
    std::uint32_t aot_inline_cache_site_count = 0;
    // Task 479: a non-zero scan count means the site index did not answer and
    // the patch fell back to the linear scan it replaced.
    std::uint32_t aot_inline_cache_site_index_lookup_count = 0;
    std::uint32_t aot_inline_cache_site_index_scan_count = 0;
    std::uint32_t aot_inline_cache_site_index_rebuild_count = 0;
    std::uint32_t aot_return_dispatch_site_count = 0;
    std::uint32_t aot_return_dispatch_site_index_lookup_count = 0;
    std::uint32_t aot_return_dispatch_site_index_scan_count = 0;
    std::uint32_t aot_return_dispatch_site_index_rebuild_count = 0;
    std::uint32_t aot_return_patch_policy_observation_count = 0;
    std::uint32_t aot_return_patch_policy_megamorphic_site_count = 0;
    std::uint32_t aot_return_patch_policy_bypass_count = 0;
    std::uint32_t aot_last_reentry_cache_address = 0;
    std::uint32_t aot_code_write_count = 0;
    std::uint32_t aot_page_retire_attempt_count = 0;
    std::uint32_t aot_page_retire_success_count = 0;
    std::uint32_t aot_generation_publish_count = 0;
    std::uint32_t aot_generation_failure_count = 0;
    std::uint32_t aot_generation_relinked_entry_count = 0;
    std::uint32_t aot_retired_entry_trap_count = 0;
    AotRetiredTrapProfileSnapshot aot_retired_trap_profile;
    std::uint32_t aot_quarantine_count = 0;
    std::uint32_t aot_last_code_write_source = 0;
    std::uint32_t aot_last_code_write_destination = 0;
    std::uint32_t aot_last_retired_page = 0;
    std::uint32_t aot_last_published_generation = 0;
    bool aot_exception_mapping_valid = false;
    std::uint32_t aot_exception_cache_address = 0;
    std::uint32_t aot_exception_guest_address = 0;
    std::uint8_t aot_exception_cache_bytes[16] = {};
    std::uint8_t aot_exception_guest_bytes[16] = {};
    std::uint32_t aot_last_indirect_source = 0;
    std::uint32_t aot_last_indirect_target = 0;
    std::uint32_t aot_return_dispatch_count = 0;
    std::uint32_t aot_last_return_target = 0;
    std::uint32_t aot_last_return_source = 0;
    std::uint32_t aot_last_return_stack[4] = {};
    bool execution_probe_configured = false;
    bool execution_probe_hit = false;
    std::uint32_t execution_probe_offset = 0;
    std::uint32_t execution_probe_memory_offset = 0;
    X86ExecutionSnapshot execution_probe_snapshot;
    std::uint32_t execution_probe_stack[8] = {};
    ExecutionProbeMemoryWindow execution_probe_memory[
        kExecutionProbeRegisterCount] = {};
    // Status only: the dumped bytes go to the file, never through the snapshot,
    // which is copied on every telemetry poll.
    bool execution_probe_dump_configured = false;
    bool execution_probe_dump_captured = false;
    bool execution_probe_dump_written = false;
    std::uint32_t execution_probe_dump_base_address = 0;
    std::uint32_t execution_probe_dump_source_address = 0;
    std::uint32_t execution_probe_dump_byte_count = 0;
    std::string execution_probe_dump_path;
    bool execution_trace_configured = false;
    std::uint32_t execution_trace_start_offset = 0;
    std::uint32_t execution_trace_end_offset = 0;
    std::uint32_t execution_trace_esp_offset = 0;
    std::uint32_t execution_trace_hit_count = 0;
    bool execution_trace_sentinel2_configured = false;
    std::uint32_t execution_trace_sentinel2_offset = 0;
    std::uint32_t execution_trace_sentinel_rearm_count = 0;
    ExecutionTraceEntry execution_trace[kExecutionTraceCapacity];
    std::uint32_t aot_call_depth = 0;
    bool aot_last_return_matches_call = false;
    std::uint32_t aot_last_expected_return = 0;
    std::uint32_t aot_last_call_source = 0;
    std::uint32_t aot_last_call_target = 0;
    std::uint32_t aot_last_expected_call_source = 0;
    std::uint32_t aot_last_expected_call_target = 0;
    std::uint32_t aot_return_trace_count = 0;
    AotReturnTraceEntry
        aot_return_trace[kAotReturnTraceCapacity];
    std::uint32_t aot_transfer_trace_count = 0;
    AotTransferTraceEntry
        aot_transfer_trace[kAotTransferTraceCapacity];
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
        aot_dbt_call_return_trace[kAotCallReturnTraceCapacity];
    std::uint32_t diagnostic_poll_iteration_count = 0;
    std::uint32_t diagnostic_progress_count = 0;
    std::uint32_t diagnostic_quiet_iteration_count = 0;
    std::uint32_t exception_dispatch_entry_count = 0;
    std::uint32_t exception_dispatch_exit_count = 0;
    std::uint32_t exception_dispatch_last_eip = 0;
    std::uint32_t exception_dispatch_malformed_count = 0;
    std::uint32_t exception_dispatch_last_bad_context = 0;
    std::uint32_t exception_dispatch_last_bad_record = 0;
    bool selector_table_valid = false;
    std::uint32_t selector_descriptor_count = 0;
    bool linexe_environment_active = false;
    std::uint16_t linexe_saved_client_gs = 0;
    bool linexe_client_descriptor_valid = false;
    std::uint32_t linexe_client_descriptor_base = 0;
    std::uint32_t linexe_client_descriptor_limit = 0;
    std::uint16_t linexe_root_offset = 0;
    std::uint16_t linexe_root_selector = 0;
    bool linexe_data_descriptor_valid = false;
    std::uint32_t linexe_data_descriptor_base = 0;
    std::uint16_t linexe_module_name_offset = 0;
    std::uint16_t linexe_module_name_selector = 0;
    std::uint16_t linexe_direct_export_count = 0;
    std::uint16_t linexe_direct_export_table_offset = 0;
    std::uint16_t linexe_direct_export_table_selector = 0;
    std::string linexe_direct_module_name;
    std::uint32_t linexe_gs_byte_load_count = 0;
    std::uint32_t linexe_first_gs_byte_offset = 0;
    std::uint32_t linexe_first_gs_byte_value = 0;
    std::uint16_t linexe_selector_words[4] = {};
    std::uint32_t linexe_resolved_exports[8] = {};
    std::uint32_t linexe_resolved_export_count = 0;
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
    std::uint16_t linexe_direct_first_export_name_offset = 0;
    std::uint16_t linexe_direct_first_export_name_selector = 0;
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
    std::uint32_t timer_interrupt_chain_hle_count = 0;
    std::uint32_t timer_interrupt_chain_hle_source = 0;
    std::uint32_t timer_interrupt_chain_hle_pointer = 0;
    std::uint32_t timer_interrupt_chain_hle_offset = 0;
    std::uint16_t timer_interrupt_chain_hle_selector = 0;
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
    struct GlideCallObservation
    {
        std::uint16_t ordinal = 0;
        std::uint32_t count = 0;
        std::uint32_t first_stack[8] = {};
        std::string name;
    };
    std::vector<GlideCallObservation> glide_calls;
    struct GlideOrdinalTimingObservation
    {
        std::uint16_t ordinal = 0;
        std::string name;
        GlideOrdinalTimingEntry timing;
    };
    std::vector<GlideOrdinalTimingObservation> glide_ordinal_timings;
    struct GlideSetterCensusObservation
    {
        std::uint16_t ordinal = 0;
        std::string name;
        GlideSetterCensusEntry census;
        // Task 365: reported next to the census so the per-ordinal cross-check
        // "observed duplicates == actually elided" is readable from one line.
        std::uint32_t elided_count = 0;
        std::uint32_t applied_count = 0;
    };
    std::vector<GlideSetterCensusObservation> glide_setter_censuses;
    bool mscdex_available = false;
    bool cd_audio_available = false;
    std::uint32_t mscdex_track_count = 0;
    std::uint32_t mscdex_request_count = 0;
    std::uint16_t mscdex_frame_es = 0;
    std::uint32_t mscdex_decline_count = 0;
    std::uint32_t mscdex_last_decline_reason = 0;
    std::uint32_t mscdex_last_resolve_kind = 0;
    std::uint32_t mscdex_last_header_bytes = 0;
    std::uint32_t mscdex_last_ioctl_subfunction = 0xFFFFFFFFU;
    bool mscdex_last_ioctl_handled = false;
    std::uint32_t mscdex_last_ioctl_length = 0;
    std::uint32_t mscdex_ioctl_reject_mask = 0;
    std::uint8_t mscdex_last_play_mode = 0xFFU;
    std::uint32_t mscdex_last_play_start = 0;
    std::uint32_t mscdex_last_play_length = 0;
    std::uint32_t mscdex_last_seek_target = 0;
    std::uint32_t cd_audio_current_lba = 0;
    // Task 421: how much of the music-position series was written, and how
    // often that position moved backwards while playing.
    std::uint32_t cd_audio_position_dump_entry_count = 0;
    std::uint32_t cd_audio_position_regression_count = 0;
    // Task 422: how many CD commands were traced, and how many were issued.
    std::uint32_t mscdex_command_trace_entry_count = 0;
    std::uint32_t mscdex_command_trace_total = 0;
    std::uint32_t glide_window_open_count = 0;
    std::uint32_t glide_logical_width = 0;
    std::uint32_t glide_logical_height = 0;
    std::string glide_backend_message;
    std::uint32_t glide_texture_memory_bytes = 0;
    std::uint32_t glide_texture_max_address = 0;
    std::uint32_t linexe_scan_return_eax = 0;
    std::uint32_t linexe_scan_return_ebp = 0;
    std::uint32_t linexe_scan_caller_eax = 0;
    std::uint32_t linexe_selector_init_results[3] = {};
    std::uint32_t dpmi_allocate_call_count = 0;
    std::uint16_t dpmi_last_allocate_requested_count = 0;
    std::uint16_t dpmi_last_allocated_selector = 0;
    bool dos_low_memory_valid = false;
    std::uint32_t dos_low_memory_size = 0;
    std::uint32_t dos_environment_block_size = 0;
    bool last_dos_environment_access_valid = false;
    std::uint32_t last_dos_environment_access_offset = 0;
    std::uint32_t last_dos_environment_entry_offset = 0;
    std::uint32_t last_dos_environment_value_length = 0;
    std::string last_dos_environment_entry_name;
    std::uint32_t handled_hle_trap_count = 0;
    std::uint32_t last_hle_trap_address = 0;
    std::uint32_t last_hle_trap_opcode = 0;
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
    // Task 764: INT 21h AH=07h/08h characters returned and empty-buffer waits.
    std::uint32_t dos_console_input_count = 0;
    std::uint32_t dos_console_input_wait_count = 0;
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
    // Task 475: SCAS/LODS/CMPS servicing, counted separately from the
    // MOV-family path because one fault covers a whole REP run.
    std::uint32_t low_memory_string_service_count = 0;
    std::uint32_t low_memory_string_iteration_count = 0;
    std::uint32_t last_low_memory_string_eip = 0;
    std::uint32_t last_low_memory_string_address = 0;
    std::uint32_t last_low_memory_string_mnemonic = 0;
    std::uint32_t last_low_memory_string_iterations = 0;
    std::uint32_t debug_emulate_stage = 0;
    std::uint32_t debug_emulate_decode_result = 0;
    std::uint32_t debug_emulate_calculated_address = 0;
    std::uint32_t rep_movs_copy_failure_count = 0;
    std::uint32_t last_rep_movs_copy_failure_stage = 0;
    std::uint32_t last_rep_movs_copy_error = 0;
    std::uint32_t last_rep_movs_copy_source = 0;
    std::uint32_t last_rep_movs_copy_destination = 0;
    std::uint32_t last_rep_movs_copy_bytes = 0;
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
    std::uint32_t shadow_memory_byte_count = 0;
    bool shadow_memory_range_valid = false;
    std::uint32_t shadow_memory_min_address = 0;
    std::uint32_t shadow_memory_max_address = 0;
    std::uint32_t thread_exit_code = 0;
    bool dos_termination_captured = false;
    std::uint32_t dos_termination_ax = 0;
    std::uint32_t dos_termination_eip = 0;
    std::uint32_t dos_termination_esp = 0;
    std::uint32_t dos_termination_stack[kDosTerminationStackCapacity] = {};
    std::string hle_stdout_output;
    std::string hle_stderr_output;
    std::string message;
};

bool AttemptGuestStackTrapExecution(
    const RelocatedImagePlacement& placement,
    const runtime::GuestStackSwitchPlan& stack_plan,
    const hle::DosVirtualFileSystemState& dos_file_system,
    const exe::Dos16mBoundModule* linexe_module,
    const std::vector<exe::LeResidentName>* glide_exports,
    const std::filesystem::path* cd_chd_path,
    const std::filesystem::path* sound_rom_zip_path,
    bool enable_piu_jamma_board,
    bool enable_piu10_isa_board,
    bool enable_cat702,
    std::string_view parent_rom_set_id,
    std::uint32_t piu10_mp3_latency_ms,
    std::uint32_t timeout_milliseconds,
    std::uint32_t stall_timeout_milliseconds,
    MinimalExecutionAttempt* attempt);

bool AttemptGuestStackAotExecution(
    const RelocatedImagePlacement& placement,
    AotCodeCachePlacement& aot_placement,
    const runtime::GuestStackSwitchPlan& stack_plan,
    const hle::DosVirtualFileSystemState& dos_file_system,
    const exe::Dos16mBoundModule* linexe_module,
    const std::vector<exe::LeResidentName>* glide_exports,
    const std::filesystem::path* cd_chd_path,
    const std::filesystem::path* sound_rom_zip_path,
    bool enable_piu_jamma_board,
    bool enable_piu10_isa_board,
    bool enable_cat702,
    std::string_view parent_rom_set_id,
    std::uint32_t piu10_mp3_latency_ms,
    runtime::ExecutionBackend execution_backend,
    std::uint32_t timeout_milliseconds,
    std::uint32_t stall_timeout_milliseconds,
    MinimalExecutionAttempt* attempt);

bool AttemptGuestStackHleExecution(
    const RelocatedImagePlacement& placement,
    const runtime::GuestStackSwitchPlan& stack_plan,
    const hle::DosVirtualFileSystemState& dos_file_system,
    std::uint32_t timeout_milliseconds,
    std::uint32_t stall_timeout_milliseconds,
    MinimalExecutionAttempt* attempt);

}  // namespace repiu::engine

#endif  // REPIU_ENGINE_EXECUTION_TRAMPOLINE_H_
