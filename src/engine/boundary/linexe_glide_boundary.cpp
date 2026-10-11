#include "repiu/engine/glide_gate_interrupt_exit.h"
#include "repiu/engine/aot_boundary_opcode_census.h"
#include "repiu/runtime/execution_model.h"
#include "linexe_glide_boundary.h"
#include "aot/aot_runtime_dispatch.h"
#include "execution_internal.h"
#include "guest_memory_access.h"
#include "instruction_emulation.h"

#include "repiu/engine/glide_lfb_lock_interval_census.h"
#include "repiu/engine/glide_lfb_staging_shadow.h"
#include "repiu/hle/glide_texture_decode.h"
#include "repiu/hle/glide_implementation_issue.h"
#include "repiu/hle/glide_lfb.h"
#include "repiu/hle/glide_lfb_region.h"
#include "repiu/hle/glide_vertex.h"
#include "repiu/hle/mesa_fx_texture_source.h"
#include "repiu/runtime/selector_table.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>
#include <cctype>
#include "repiu/platform/guest_cpu_context.h"
#include "repiu/engine/live_execution_profile_report.h"
#include "../aot/aot_dbt_glide_gate_dispatch.h"
#include "repiu/platform/atomic_ops.h"
#include <chrono>
#include <mutex>

namespace repiu::engine
{

bool DecodeGlideTexDownloadTableCall(const std::uint32_t* guest_stack,
                                     const std::size_t word_count,
                                     GlideTexDownloadTableCall* call)
{
    constexpr std::size_t kFrameWordCount = 4U;
    if (guest_stack == nullptr || word_count < kFrameWordCount || call == nullptr)
    {
        return false;
    }
    call->tmu = guest_stack[1];
    call->type = guest_stack[2];
    call->data = guest_stack[3];
    call->stack_advance =
        static_cast<std::uint32_t>(kFrameWordCount * sizeof(std::uint32_t));
    return true;
}

namespace
{

constexpr std::uint32_t kLinexeFarTransferTraceCapacity = 64U;

const char* LinexeServiceName(const repiu::hle::LinexeService service)
{
    switch (service)
    {
        case repiu::hle::LinexeService::kLoadModule: return "load-module";
        case repiu::hle::LinexeService::kFreeModule: return "free-module";
        case repiu::hle::LinexeService::kGetLoadTable: return "get-load-table";
        case repiu::hle::LinexeService::kGetLoadName: return "get-load-name";
        case repiu::hle::LinexeService::kGetModuleHandle: return "get-module-handle";
        case repiu::hle::LinexeService::kGetProcedureAddress: return "get-proc";
        case repiu::hle::LinexeService::kRelocate: return "relocate";
        case repiu::hle::LinexeService::kUnrelocate: return "unrelocate";
    }
    return "unknown";
}

std::uint32_t BeginLinexeFarTransferTrace()
{
    const char* const value =
        std::getenv("REPIU_LINEXE_FAR_TRANSFER_TRACE");
    if (value == nullptr || std::strcmp(value, "0") == 0)
    {
        return 0U;
    }
    static std::atomic<std::uint32_t> trace_count{0U};
    const std::uint32_t sequence =
        trace_count.fetch_add(1U, std::memory_order_relaxed) + 1U;
    return sequence <= kLinexeFarTransferTraceCapacity ? sequence : 0U;
}

void TraceLinexeFarTransferStack(ThreadContext* context,
                                 const std::uint32_t sequence,
                                 const std::uint32_t esp)
{
    constexpr std::size_t kTraceWordCount = 16U;
    std::array<std::uint32_t, kTraceWordCount> words{};
    const auto* const stack = reinterpret_cast<const void*>(
        static_cast<std::uintptr_t>(esp));
    if (context != nullptr &&
        IsGuestRangeReadable(context,
                             stack,
                             words.size() * sizeof(words[0])))
    {
        std::memcpy(words.data(), stack, sizeof(words));
    }
    else
    {
        std::fprintf(stderr,
                     "[repiu-linexe-far] n=%u event=stack-unreadable "
                     "esp=0x%08X\n",
                     static_cast<unsigned>(sequence),
                     static_cast<unsigned>(esp));
        return;
    }
    std::fprintf(stderr,
                 "[repiu-linexe-far] n=%u event=stack esp=0x%08X "
                 "w0=0x%08X w1=0x%08X w2=0x%08X w3=0x%08X "
                 "w4=0x%08X w5=0x%08X w6=0x%08X w7=0x%08X "
                 "w8=0x%08X w9=0x%08X w10=0x%08X w11=0x%08X "
                 "w12=0x%08X w13=0x%08X w14=0x%08X w15=0x%08X\n",
                 static_cast<unsigned>(sequence),
                 static_cast<unsigned>(esp),
                 static_cast<unsigned>(words[0]),
                 static_cast<unsigned>(words[1]),
                 static_cast<unsigned>(words[2]),
                 static_cast<unsigned>(words[3]),
                 static_cast<unsigned>(words[4]),
                 static_cast<unsigned>(words[5]),
                 static_cast<unsigned>(words[6]),
                 static_cast<unsigned>(words[7]),
                 static_cast<unsigned>(words[8]),
                 static_cast<unsigned>(words[9]),
                 static_cast<unsigned>(words[10]),
                 static_cast<unsigned>(words[11]),
                 static_cast<unsigned>(words[12]),
                 static_cast<unsigned>(words[13]),
                 static_cast<unsigned>(words[14]),
                 static_cast<unsigned>(words[15]));
}

// DOS names arrive in whatever case the guest wrote them, so this comparison
// has to ignore case. `_stricmp` is MSVC's name for it and `strcasecmp` is
// POSIX's; comparing two short ASCII names directly avoids choosing.
bool EqualsIgnoringCase(const char* left, const char* right)
{
    if (left == nullptr || right == nullptr)
    {
        return left == right;
    }
    while (*left != 0 && *right != 0)
    {
        const unsigned char lower_left = static_cast<unsigned char>(
            std::tolower(static_cast<unsigned char>(*left)));
        const unsigned char lower_right = static_cast<unsigned char>(
            std::tolower(static_cast<unsigned char>(*right)));
        if (lower_left != lower_right)
        {
            return false;
        }
        ++left;
        ++right;
    }
    return *left == *right;
}

class GlideOrdinalTimingScope
{
  public:
    GlideOrdinalTimingScope(GlideOrdinalTimingProfile* profile,
                            GlideOpenGlBackend* backend,
                            const std::uint64_t* gate_cycles)
        : profile_(profile),
          backend_(backend),
          gate_cycles_(gate_cycles)
    {
    }

    ~GlideOrdinalTimingScope()
    {
        if (!active_)
        {
            return;
        }
        RecordGlideOrdinalGateTime(
            profile_, ordinal_, *gate_cycles_);
        backend_->EndGlideOrdinalTiming();
    }

    void Begin(std::uint16_t ordinal)
    {
        if (profile_ == nullptr)
        {
            return;
        }
        ordinal_ = ordinal;
        backend_->BeginGlideOrdinalTiming(profile_, ordinal_);
        active_ = true;
    }

    GlideOrdinalTimingScope(const GlideOrdinalTimingScope&) = delete;
    GlideOrdinalTimingScope& operator=(const GlideOrdinalTimingScope&) = delete;

  private:
    GlideOrdinalTimingProfile* profile_ = nullptr;
    std::uint16_t ordinal_ = 0;
    GlideOpenGlBackend* backend_ = nullptr;
    const std::uint64_t* gate_cycles_ = nullptr;
    bool active_ = false;
};

// Tasks 364 and 365. Hooked once here rather than in each setter case, so no
// dispatch path is edited. It serves two consumers that must agree exactly: the
// Task 364 census, which only observes, and the Task 365 cache, which skips the
// host rendezvous for a confirmed repeat. Building the key once and classifying
// the outcome once is what makes them unable to diverge — and what makes the
// cross-check "census `same` == cache `elided`" a real proof that only observed
// duplicates were skipped.
//
// The outcome is read from the handled counter and the implementation-issue
// totals across the call, so the census still cannot change a result.
class GlideSetterStateScope
{
  public:
    GlideSetterStateScope(GlideSetterCensusProfile* census,
                          GlideSetterStateCache* cache,
                          const ThreadContext* context)
        : census_(census),
          cache_(cache),
          context_(context)
    {
    }

    ~GlideSetterStateScope()
    {
        if (!active_)
        {
            return;
        }
        if (elided_)
        {
            // An elided call is still a call, and still an exact repeat, so the
            // observer counts it as `same` exactly as it would have without the
            // elision. That keeps the census comparable between the two
            // configurations, which gate E1 depends on.
            RecordGlideSetterCensusCall(
                census_, ordinal_, key_,
                GlideSetterCensusOutcome::kApplied);
            return;
        }
        const GlideSetterCensusOutcome outcome = Classify();
        RecordGlideSetterCensusCall(census_, ordinal_, key_, outcome);
        if (!elision_candidate_)
        {
            return;
        }
        if (outcome == GlideSetterCensusOutcome::kApplied)
        {
            RecordGlideSetterStateApplied(cache_, ordinal_, key_);
        }
        else
        {
            // A decline or a retained-but-unexpressed argument leaves the host
            // state unknown, so the record must go rather than be trusted.
            RecordGlideSetterStateVoided(cache_, ordinal_);
        }
    }

    // Called after the gate stack mirror is populated and before dispatch.
    void Begin(const repiu::hle::GlideExportGate& glide_export)
    {
        if (census_ == nullptr && cache_ == nullptr)
        {
            return;
        }
        const repiu::hle::GlideGateId gate_id = glide_export.gate_id;
        // Boundary bookkeeping runs for every gate, not only setter targets.
        if (IsGlideSetterStateInvalidatingGate(gate_id))
        {
            RecordGlideSetterCensusInvalidation(census_);
            InvalidateGlideSetterStateCache(cache_);
        }
        if (IsGlideSetterStateTextureGenerationGate(gate_id))
        {
            RecordGlideSetterCensusTextureGeneration(census_);
            BumpGlideSetterStateCacheTextureGeneration(cache_);
        }
        if (gate_id == repiu::hle::GlideGateId::kGrBufferSwap)
        {
            RecordGlideSetterCensusFrameBoundary(census_);
        }
        if (!IsGlideSetterStateGate(gate_id))
        {
            return;
        }

        ordinal_ = glide_export.ordinal;
        const std::uint32_t argument_words =
            glide_export.argument_byte_count / sizeof(std::uint32_t);
        if (argument_words > kGlideSetterStateKeyWords)
        {
            // Truncating would collide distinct states into one key, so the call
            // is counted and excluded instead. A nonzero total means the target
            // list needs revisiting.
            RecordGlideSetterCensusKeyOverflow(census_, ordinal_);
            return;
        }
        // One key, built from the one generation counter that is live. The two
        // counters advance together in lockstep above.
        const std::uint32_t generation =
            cache_ != nullptr ? cache_->texture_generation
                              : (census_ != nullptr ? census_->texture_generation
                                                    : 0U);
        key_ = BuildGlideSetterStateKey(
            context_->glide_gate_stack + 1U,
            argument_words,
            IsGlideSetterStateTextureDependentGate(gate_id) ? generation : 0U);
        // Task 437: batch two widens the list, never the rules. The key, the
        // texture generation and the invalidation set are the same ones the
        // census measured the ceiling with.
        elision_candidate_ =
            cache_ != nullptr &&
            (IsGlideSetterElisionGate(gate_id) ||
             IsGlideSetterTextureStateElisionGate(gate_id) ||
             IsGlideSetterBatchThreeElisionGate(gate_id) ||
             IsGlideSetterBatchFourElisionGate(gate_id));
        handled_before_ = context_->glide_gate_handled_count;
        issues_before_ = TotalIssues();
        backend_failures_before_ = context_->glide_implementation_issues.total(
            repiu::hle::GlideImplementationIssueKind::kBackendFailure);
        active_ = true;
    }

    // Queried only after the return address, signature, and argument size have all
    // been validated, so a gate that would have been rejected is never elided.
    bool ShouldElide() const
    {
        return active_ && elision_candidate_ &&
            ShouldElideGlideSetterState(cache_, ordinal_, key_);
    }

    void MarkElided()
    {
        elided_ = true;
        RecordGlideSetterStateElided(cache_, ordinal_);
    }

    GlideSetterStateScope(const GlideSetterStateScope&) = delete;
    GlideSetterStateScope& operator=(const GlideSetterStateScope&) = delete;

  private:
    std::uint64_t TotalIssues() const
    {
        using kind = repiu::hle::GlideImplementationIssueKind;
        const auto& issues = context_->glide_implementation_issues;
        return issues.total(kind::kUnimplementedFunction) +
            issues.total(kind::kUnsupportedArgument) +
            issues.total(kind::kBackendFailure) +
            issues.total(kind::kAbiReject);
    }

    GlideSetterCensusOutcome Classify() const
    {
        // A gate that never reached the handled path, and any backend failure,
        // leave the host state unknown. Anything else that recorded an issue was
        // retained for the ABI without the argument being expressed, so it is not
        // a successfully applied state either.
        if (context_->glide_gate_handled_count == handled_before_ ||
            context_->glide_implementation_issues.total(
                repiu::hle::GlideImplementationIssueKind::kBackendFailure) !=
                backend_failures_before_)
        {
            return GlideSetterCensusOutcome::kFailed;
        }
        if (TotalIssues() != issues_before_)
        {
            return GlideSetterCensusOutcome::kUnsupported;
        }
        return GlideSetterCensusOutcome::kApplied;
    }

    GlideSetterCensusProfile* census_ = nullptr;
    GlideSetterStateCache* cache_ = nullptr;
    const ThreadContext* context_ = nullptr;
    GlideSetterStateKey key_;
    std::uint16_t ordinal_ = 0;
    std::uint32_t handled_before_ = 0;
    std::uint64_t issues_before_ = 0;
    std::uint64_t backend_failures_before_ = 0;
    bool elision_candidate_ = false;
    bool elided_ = false;
    bool active_ = false;
};

// Task 438: hands the pending primitives to the backend in one rendezvous.
// Flushing an empty batch is a no-op success, which matters because the caller
// flushes unconditionally before every non-draw gate.
bool FlushGlideDrawBatchToBackend(ThreadContext* context,
                                  GlideDrawBatchFlushReason reason)
{
    // Task 440: with the asynchronous present on, the flush is posted rather
    // than waited on. It has to be: left synchronous it would block the guest at
    // the next frame's first `grTexSource`, and the swap's wait would simply
    // move there instead of being returned to the guest.
    const bool asynchronous = GlideAsyncPresentEnabled();
    return FlushGlideDrawBatch(
        &context->glide_draw_batch, reason,
        [context, asynchronous](const repiu::hle::GlideDrawVertex* vertices,
                                std::size_t vertex_count,
                                GlideBatchPrimitive primitive) {
            if (asynchronous)
            {
                return context->glide_backend.PostDrawPrimitiveBatch(
                    vertices, vertex_count, primitive);
            }
            return context->glide_backend.DrawPrimitiveBatch(
                vertices, vertex_count, primitive);
        });
}

// Task 476: grLfbReadRegion / grLfbWriteRegion are the only gates that use the
// staging surface as a live shadow of the frame buffer. `Ensure` fills it from
// the frame buffer once per burst, `Flush` presents accumulated region writes,
// and `Invalidate` forces the next burst to re-read. Doing this per row instead
// would mean 128 full-screen round trips per frame for the observed PIU pass.
bool IsGlideLfbRegionGate(repiu::hle::GlideGateId gate_id)
{
    return gate_id == repiu::hle::GlideGateId::kGrLfbReadRegion ||
        gate_id == repiu::hle::GlideGateId::kGrLfbWriteRegion;
}

// Bytes the guest side of a region transfer spans, or 0 when the arguments
// cannot describe a transfer this layer performs. The bound keeps a nonsense
// width or height from turning into an allocation.
std::size_t ResolveGlideLfbRegionSpan(std::uint32_t width,
                                      std::uint32_t height,
                                      std::uint32_t src_format,
                                      std::int32_t stride)
{
    constexpr std::uint64_t kMaximumRegionBytes = 64ULL * 1024ULL * 1024ULL;
    const std::uint32_t bytes_per_pixel =
        repiu::hle::GlideLfbSrcFormatBytesPerPixel(src_format);
    if (width == 0U || height == 0U || bytes_per_pixel == 0U || stride < 0)
    {
        return 0U;
    }
    const std::uint64_t row_pitch =
        static_cast<std::uint64_t>(repiu::hle::ResolveGlideLfbRegionStride(
            width, bytes_per_pixel, stride));
    const std::uint64_t span = (static_cast<std::uint64_t>(height) - 1ULL) *
            row_pitch +
        static_cast<std::uint64_t>(width) * bytes_per_pixel;
    if (span == 0ULL || span > kMaximumRegionBytes)
    {
        return 0U;
    }
    return static_cast<std::size_t>(span);
}

// Names the first reason a region transfer cannot be serviced, or null when the
// arguments are ones both region gates accept.
const char* ClassifyGlideLfbRegionArguments(const ThreadContext* context,
                                            std::uint32_t buffer,
                                            std::uint32_t width,
                                            std::uint32_t height,
                                            std::int32_t stride,
                                            std::uint32_t src_format)
{
    // The staging surface is the shadow and the lock buffer both. Servicing a
    // region transfer while the guest holds the lock pointer would rewrite
    // memory it is in the middle of using.
    if (context->glide_lfb_surface.locked())
    {
        return "a frame buffer lock is outstanding";
    }
    if (buffer != repiu::hle::kGlideBufferBackBuffer &&
        buffer != repiu::hle::kGlideBufferFrontBuffer)
    {
        return "only the front and back color buffers are implemented";
    }
    if (width == 0U || height == 0U)
    {
        return "region has no pixels";
    }
    if (stride < 0)
    {
        return "bottom-up images with a negative stride are not implemented";
    }
    if (!repiu::hle::GlideLfbSrcFormatSupported(src_format))
    {
        return "source pixel format is not implemented";
    }
    return nullptr;
}

bool FlushGlideLfbRegionShadow(ThreadContext* context);

bool EnsureGlideLfbRegionShadow(ThreadContext* context, std::uint32_t buffer)
{
    const std::uint32_t width = context->glide_state.width;
    const std::uint32_t height = context->glide_state.height;
    // Task 708. Idempotent, and ahead of every Resize rather than at an
    // initialization point of its own: the guest is handed this surface's
    // address as a 32-bit lfbPtr, so the storage has to be the placed one
    // before anything sizes it.
    if (!EnsureGlideLfbGuestStorage(&context->glide_lfb_guest_storage,
                                    &context->glide_lfb_surface))
    {
        return false;
    }
    if (!context->glide_lfb_surface.Resize(width, height))
    {
        return false;
    }
    if (context->glide_lfb_region_shadow_valid &&
        context->glide_lfb_region_shadow_buffer != buffer)
    {
        FlushGlideLfbRegionShadow(context);
        context->glide_lfb_region_shadow_valid = false;
    }
    context->glide_lfb_region_shadow_buffer = buffer;
    if (context->glide_lfb_region_shadow_valid)
    {
        return true;
    }
    std::vector<std::uint8_t> rgba8;
    if (!context->glide_backend.ReadbackFramebuffer(width, height, &rgba8) ||
        !repiu::hle::EncodeRgba8ToGlideLfb565(
            rgba8.data(), rgba8.size(), width, height,
            context->glide_state.color_format,
            context->glide_lfb_surface.pixels(),
            context->glide_lfb_surface.byte_count()))
    {
        context->glide_backend_message = context->glide_backend.message();
        return false;
    }
    // Task 761: the readback is the pre-quantization image, so when the
    // high-precision path is on it seeds the RGBA8 shadow for free.
    if (context->glide_backend.LfbHighPrecisionEnabled() &&
        rgba8.size() == static_cast<std::size_t>(width) * height * 4U)
    {
        context->glide_lfb_region_rgba8_shadow = std::move(rgba8);
        context->glide_lfb_region_rgba8_valid = true;
    }
    else
    {
        context->glide_lfb_region_rgba8_valid = false;
    }
    context->glide_lfb_region_shadow_valid = true;
    ++context->glide_lfb_region_seed_count;
    return true;
}

bool FlushGlideLfbRegionShadow(ThreadContext* context)
{
    if (!context->glide_lfb_region_shadow_dirty)
    {
        return true;
    }
    context->glide_lfb_region_shadow_dirty = false;
    const std::uint32_t width = context->glide_lfb_surface.width();
    const std::uint32_t height = context->glide_lfb_surface.height();
    // Task 761: when the high-precision shadow mirrors the staging surface it
    // is presented as-is, keeping the precision the 565 texels dropped. The
    // toggle is re-read here so the OSD takes effect at the next flush, and
    // turning it off invalidates the shadow so a later enable starts from a
    // fresh 565 decode instead of stale pixels.
    const bool high_precision =
        context->glide_backend.LfbHighPrecisionEnabled();
    if (!high_precision)
    {
        context->glide_lfb_region_rgba8_valid = false;
    }
    const bool present_high_precision = high_precision &&
        context->glide_lfb_region_rgba8_valid &&
        context->glide_lfb_region_rgba8_shadow.size() ==
            static_cast<std::size_t>(width) * height * 4U;
    std::vector<std::uint8_t> rgba8;
    if (!present_high_precision &&
        !repiu::hle::DecodeGlideLfb565ToRgba8(
            context->glide_lfb_surface.pixels(),
            context->glide_lfb_surface.byte_count(), width, height,
            context->glide_state.color_format, &rgba8))
    {
        return false;
    }
    // `flip_v` is false because the shadow is top-down and stays that way:
    // ReadbackFramebuffer returns top-down rows and region coordinates are
    // native frame buffer rows, not origin-relative ones. PresentLfbSurface
    // XORs this against the window projection, so false is what lands row 0 at
    // the top of the screen under either origin.
    const bool present_to_front = context->glide_lfb_region_shadow_buffer ==
        repiu::hle::kGlideBufferFrontBuffer;
    if (!context->glide_backend.PresentLfbSurface(
            present_high_precision
                ? context->glide_lfb_region_rgba8_shadow.data()
                : rgba8.data(),
            width, height, false, present_to_front))
    {
        context->glide_backend_message = context->glide_backend.message();
        return false;
    }
    if (present_high_precision)
    {
        ++context->glide_lfb_region_rgba8_present_count;
    }
    ++context->glide_lfb_region_flush_count;
    ++context->glide_lfb_present_count;
    return true;
}

// Appends one primitive, flushing first when the batch cannot take it. Returns
// false only when the primitive was not queued at all, in which case the caller
// draws it directly and ordering is still preserved -- the batch is empty by
// then either way.
bool QueueGlideDrawForBatch(ThreadContext* context,
                            GlideDrawBatch* batch,
                            const repiu::hle::GlideDrawVertex* vertices,
                            std::size_t vertex_count,
                            GlideBatchPrimitive primitive)
{
    if (batch == nullptr)
    {
        return false;
    }
    bool flush_required = false;
    if (QueueGlideDrawPrimitive(batch, vertices, vertex_count, primitive,
                                &flush_required))
    {
        return true;
    }
    if (!flush_required)
    {
        return false;
    }
    const GlideDrawBatchFlushReason reason =
        batch->primitive != primitive
            ? GlideDrawBatchFlushReason::kPrimitiveChange
            : GlideDrawBatchFlushReason::kCapacity;
    FlushGlideDrawBatchToBackend(context, reason);
    return QueueGlideDrawPrimitive(batch, vertices, vertex_count, primitive,
                                   &flush_required);
}

std::array<std::uint32_t,
           repiu::hle::kGlideImplementationIssueArgumentCapacity>
CaptureGlideImplementationIssueArguments(
    const repiu::platform::GuestCpuContext* win32_context,
    ThreadContext* context,
    const std::uint32_t argument_byte_count)
{
    std::array<std::uint32_t,
               repiu::hle::kGlideImplementationIssueArgumentCapacity>
        arguments{};
    const std::size_t argument_count = std::min<std::size_t>(
        argument_byte_count / sizeof(std::uint32_t), arguments.size());
    const auto* guest_arguments = reinterpret_cast<const std::uint32_t*>(
        static_cast<std::uintptr_t>(win32_context->Esp) +
        sizeof(std::uint32_t));
    if (argument_count != 0U &&
        IsGuestRangeReadable(context,
                             guest_arguments,
                             argument_count * sizeof(std::uint32_t)))
    {
        std::memcpy(arguments.data(),
                    guest_arguments,
                    argument_count * sizeof(std::uint32_t));
        return arguments;
    }

    const std::size_t mirrored_count =
        std::min<std::size_t>(argument_count, 7U);
    std::copy_n(context->glide_gate_stack + 1U,
                mirrored_count,
                arguments.begin());
    return arguments;
}

void RecordGlideImplementationIssue(
    const repiu::platform::GuestCpuContext* win32_context,
    ThreadContext* context,
    const repiu::hle::GlideExportGate& glide_export,
    const repiu::hle::GlideImplementationIssueKind kind,
    const std::string_view reason,
    const std::string_view detail,
    const char* action)
{
    const auto arguments = CaptureGlideImplementationIssueArguments(
        win32_context, context, glide_export.argument_byte_count);
    const auto result = context->glide_implementation_issues.Record(
        kind,
        glide_export.ordinal,
        glide_export.name,
        reason,
        detail,
        glide_export.argument_byte_count,
        arguments);
    if (result == repiu::hle::GlideImplementationIssueRecordResult::kRepeated)
    {
        return;
    }

    const bool fatal = repiu::hle::IsGlideImplementationIssueFatal(kind);
    if (result == repiu::hle::GlideImplementationIssueRecordResult::kOverflow)
    {
        if (context->glide_implementation_issues.overflow_count() == 1U)
        {
            fprintf(stderr,
                    "%s GLIDE_ISSUE_RECORD_OVERFLOW action=continue"
                    " capacity=%zu\n",
                    fatal ? "[repiu-fatal]" : "[repiu-error]",
                    repiu::hle::kGlideImplementationIssueRecordCapacity);
        }
        return;
    }

    const std::string line = repiu::hle::FormatGlideImplementationIssue(
        context->glide_implementation_issues.observations().back(),
        action);
    fprintf(stderr,
            "%s %s\n",
            fatal ? "[repiu-fatal]" : "[repiu-error]",
            line.c_str());
}

void RecordGlideTextureGateTrace(ThreadContext* context, const repiu::platform::GuestCpuContext* win32_context, const repiu::hle::GlideExportGate& glide_export, std::uint32_t return_address, std::uint32_t return_eax, bool is_max_address)
{
    const std::uint32_t sequence = context->glide_texture_gate_trace_count + 1U;
    GlideTextureGateTraceEntry& entry = context->glide_texture_gate_trace[(sequence - 1U) % kGlideTextureGateTraceCapacity];
    entry.valid = true;
    entry.sequence = sequence;
    entry.ordinal = glide_export.ordinal;
    entry.is_max_address = is_max_address;
    entry.entry_eip = static_cast<std::uint32_t>(win32_context->Eip);
    entry.entry_esp = static_cast<std::uint32_t>(win32_context->Esp);
    entry.return_address = return_address;
    entry.tmu = context->glide_gate_stack[1];
    entry.entry_eax = static_cast<std::uint32_t>(win32_context->Eax);
    entry.return_eax = return_eax;
    entry.planned_return_esp = entry.entry_esp + 2U * sizeof(std::uint32_t);
    context->glide_texture_gate_trace_count = sequence;
    if (sequence > kGlideTextureGateTraceCapacity)
    {
        context->glide_texture_gate_trace_wrapped = true;
    }
}

} // namespace

void RecordAllocatorControlFlowException(
    const repiu::platform::FaultEvent& fault,
    ThreadContext* context)
{
    if (context == nullptr || fault.registers == nullptr)
    {
        return;
    }

    const std::uint32_t eip = static_cast<std::uint32_t>(
        fault.registers->Eip);
    const std::uint64_t runtime_end =
        static_cast<std::uint64_t>(context->runtime_base) +
        context->runtime_size;
    if (eip < context->runtime_base ||
        static_cast<std::uint64_t>(eip) + 4U > runtime_end)
    {
        return;
    }

    const std::uint32_t eip_offset = eip - context->runtime_base;
    constexpr std::uint32_t kAllocatorTraceBegin = 0x000F7A60U;
    constexpr std::uint32_t kAllocatorTraceEnd = 0x000F7AD5U;
    if (eip_offset < kAllocatorTraceBegin ||
        eip_offset >= kAllocatorTraceEnd)
    {
        return;
    }

    AllocatorControlFlowObservation& observation =
        context->allocator_control_flow;
    const std::uint32_t sequence = observation.observed_count + 1;
    const std::uint32_t slot =
        (sequence - 1) % kAllocatorControlFlowTraceCapacity;
    AllocatorControlFlowTraceEntry& entry = observation.trace[slot];
    entry.valid = true;
    entry.sequence = sequence;
    entry.eip_offset = eip_offset;
    entry.seh_code = fault.host_code;
    const std::uint8_t* instruction =
        reinterpret_cast<const std::uint8_t*>(
            static_cast<std::uintptr_t>(eip));
    std::memcpy(entry.opcode, instruction, sizeof(entry.opcode));
    entry.eax = fault.registers->Eax;
    entry.ebx = fault.registers->Ebx;
    entry.edx = fault.registers->Edx;
    entry.esi = fault.registers->Esi;
    entry.edi = fault.registers->Edi;
    entry.eflags = fault.registers->EFlags;
    entry.pending_valid = context->pending_shadow_allocation_valid;
    entry.pending_size = context->pending_shadow_allocation_size;
    observation.observed_count = sequence;
    if (observation.trace_stored_count <
        kAllocatorControlFlowTraceCapacity)
    {
        ++observation.trace_stored_count;
    }
    else
    {
        observation.trace_wrapped = true;
    }
}

bool HandleLinexeFarTransferBoundary(repiu::platform::GuestCpuContext* win32_context,
                                     ThreadContext* context)
{
    if (win32_context == nullptr || context == nullptr ||
        !context->linexe_environment_active)
    {
        return false;
    }

    const auto* instruction = reinterpret_cast<const std::uint8_t*>(
        static_cast<std::uintptr_t>(win32_context->Eip));
    const bool instruction_readable =
        IsGuestRangeReadable(context, instruction, 6U);
    constexpr std::size_t kFarPointerSize = 6U;
    constexpr std::uint8_t kFarTransferPrefix[] =
        {0x66U, 0xEAU, 0x04U, 0x00U, 0x2CU, 0x00U};
    const bool indirect_candidate = instruction_readable &&
        instruction[0] == 0xFFU && instruction[1] == 0x1DU;
    const bool far_transfer_candidate = instruction_readable &&
        std::memcmp(instruction,
                    kFarTransferPrefix,
                    sizeof(kFarTransferPrefix)) == 0;
    const std::uint32_t trace_sequence =
        indirect_candidate || far_transfer_candidate
            ? BeginLinexeFarTransferTrace()
            : 0U;
    if (trace_sequence != 0U)
    {
        std::fprintf(stderr,
                     "[repiu-linexe-far] n=%u event=entry eip=0x%08X "
                     "bytes=%02X%02X%02X%02X%02X%02X esp=0x%08X "
                     "ebp=0x%08X eax=0x%08X edi=0x%08X es=0x%04X\n",
                     static_cast<unsigned>(trace_sequence),
                     static_cast<unsigned>(win32_context->Eip),
                     instruction_readable ? instruction[0] : 0U,
                     instruction_readable ? instruction[1] : 0U,
                     instruction_readable ? instruction[2] : 0U,
                     instruction_readable ? instruction[3] : 0U,
                     instruction_readable ? instruction[4] : 0U,
                     instruction_readable ? instruction[5] : 0U,
                     static_cast<unsigned>(win32_context->Esp),
                     static_cast<unsigned>(win32_context->Ebp),
                     static_cast<unsigned>(win32_context->Eax),
                     static_cast<unsigned>(win32_context->Edi),
                     static_cast<unsigned>(win32_context->SegEs));
        TraceLinexeFarTransferStack(
            context, trace_sequence, static_cast<std::uint32_t>(
                win32_context->Esp));
    }
    if (indirect_candidate)
    {
        const std::uint32_t pointer_address =
            static_cast<std::uint32_t>(instruction[2]) |
            (static_cast<std::uint32_t>(instruction[3]) << 8U) |
            (static_cast<std::uint32_t>(instruction[4]) << 16U) |
            (static_cast<std::uint32_t>(instruction[5]) << 24U);
        const auto* pointer = reinterpret_cast<const std::uint8_t*>(
            static_cast<std::uintptr_t>(pointer_address));
        if (IsGuestRangeReadable(context, pointer, kFarPointerSize))
        {
            const std::uint32_t target_offset =
                static_cast<std::uint32_t>(pointer[0]) |
                (static_cast<std::uint32_t>(pointer[1]) << 8U) |
                (static_cast<std::uint32_t>(pointer[2]) << 16U) |
                (static_cast<std::uint32_t>(pointer[3]) << 24U);
            const std::uint16_t target_selector =
                static_cast<std::uint16_t>(pointer[4]) |
                static_cast<std::uint16_t>(pointer[5] << 8U);
            repiu::hle::LinexeService service{};
            ++context->linexe_indirect_far_call_count;
            context->linexe_indirect_far_call_source =
                static_cast<std::uint32_t>(win32_context->Eip);
            context->linexe_indirect_far_call_pointer = pointer_address;
            context->linexe_indirect_far_call_offset = target_offset;
            context->linexe_indirect_far_call_selector = target_selector;
            context->linexe_indirect_far_call_known_export =
                repiu::hle::DecodeLinexeOriginalExport(
                    context->linexe_gate_plan,
                    target_selector,
                    static_cast<std::uint16_t>(target_offset),
                    &service);
            if (trace_sequence != 0U)
            {
                std::fprintf(stderr,
                             "[repiu-linexe-far] n=%u event=indirect "
                             "pointer=0x%08X target=0x%08X selector=0x%04X "
                             "service=%s known=%u\n",
                             static_cast<unsigned>(trace_sequence),
                             static_cast<unsigned>(pointer_address),
                             static_cast<unsigned>(target_offset),
                             static_cast<unsigned>(target_selector),
                             LinexeServiceName(service),
                             context->linexe_indirect_far_call_known_export
                                 ? 1U : 0U);
            }
        }
        return false;
    }
    if (!far_transfer_candidate)
    {
        return false;
    }

    const std::uint16_t target_selector = static_cast<std::uint16_t>(
        win32_context->Edi >> 16U);
    const std::uint16_t target_offset = static_cast<std::uint16_t>(
        win32_context->Edi & 0xFFFFU);
    repiu::hle::LinexeService service{};
    if (!repiu::hle::DecodeLinexeOriginalExport(
            context->linexe_gate_plan,
            target_selector,
            target_offset,
            &service))
    {
        if (trace_sequence != 0U)
        {
            std::fprintf(stderr,
                         "[repiu-linexe-far] n=%u event=decode-rejected "
                         "target=0x%04X selector=0x%04X\n",
                         static_cast<unsigned>(trace_sequence),
                         static_cast<unsigned>(target_offset),
                         static_cast<unsigned>(target_selector));
        }
        return false;
    }

    if (trace_sequence != 0U)
    {
        std::fprintf(stderr,
                     "[repiu-linexe-far] n=%u event=decoded target=0x%04X "
                     "selector=0x%04X service=%s\n",
                     static_cast<unsigned>(trace_sequence),
                     static_cast<unsigned>(target_offset),
                     static_cast<unsigned>(target_selector),
                     LinexeServiceName(service));
    }

    ++context->linexe_bridge_entry_count;
    context->linexe_bridge_gate_valid = true;
    context->linexe_bridge_selector = target_selector;
    context->linexe_bridge_offset = target_offset;
    context->linexe_bridge_service = static_cast<std::uint32_t>(service);
    context->linexe_bridge_esp = win32_context->Esp;
    context->linexe_bridge_ebp = win32_context->Ebp;
    const auto* stack = reinterpret_cast<const std::uint32_t*>(
        static_cast<std::uintptr_t>(win32_context->Esp));
    if (!IsGuestRangeReadable(context,
                              stack,
                              sizeof(context->linexe_bridge_stack)))
    {
        return false;
    }
    std::memcpy(context->linexe_bridge_stack,
                stack,
                sizeof(context->linexe_bridge_stack));
    std::memset(context->linexe_bridge_argument_text,
                0,
                sizeof(context->linexe_bridge_argument_text));
    std::memset(context->linexe_bridge_stack_text,
                0,
                sizeof(context->linexe_bridge_stack_text));
    for (std::size_t stack_index = 0;
         stack_index < std::size(context->linexe_bridge_stack);
         ++stack_index)
    {
        const auto* candidate = reinterpret_cast<const char*>(
            static_cast<std::uintptr_t>(
                context->linexe_bridge_stack[stack_index]));
        std::size_t text_length = 0;
        for (; text_length + 1U <
                   sizeof(context->linexe_bridge_stack_text[stack_index]);
             ++text_length)
        {
            if (!IsGuestRangeReadable(context, candidate + text_length, 1U))
            {
                break;
            }
            const unsigned char value = static_cast<unsigned char>(
                candidate[text_length]);
            if (value == 0)
            {
                break;
            }
            if (!std::isprint(value))
            {
                text_length = 0;
                break;
            }
            context->linexe_bridge_stack_text[stack_index][text_length] =
                static_cast<char>(value);
        }
        if (text_length == 0)
        {
            context->linexe_bridge_stack_text[stack_index][0] = '\0';
        }
    }
    const auto* argument = reinterpret_cast<const char*>(
        static_cast<std::uintptr_t>(context->linexe_bridge_stack[9]));
    for (std::size_t index = 0;
         index + 1U < sizeof(context->linexe_bridge_argument_text);
         ++index)
    {
        if (!IsGuestRangeReadable(context, argument + index, 1U))
        {
            break;
        }
        context->linexe_bridge_argument_text[index] = argument[index];
        if (argument[index] == '\0')
        {
            break;
        }
    }

    constexpr std::uint32_t kVirtualGlideModuleHandle = 1U;
    // The bridge consumes its three dwords and restores the ES value saved by
    // the wrapper.  The shared epilogue then owns EBX/ESI/EDI/EBP and RET.
    const bool is_glide_module =
        EqualsIgnoringCase(context->linexe_bridge_argument_text, "glide2x.ovl");
    const repiu::hle::GlideExportGate* glide_export =
        repiu::hle::FindGlideExportByName(
            context->glide_gate_plan,
            context->linexe_bridge_stack_text[12]);
    if (service == repiu::hle::LinexeService::kGetProcedureAddress &&
        context->linexe_bridge_stack[11] == kVirtualGlideModuleHandle)
    {
        std::strncpy(context->linexe_get_proc_name,
                     context->linexe_bridge_stack_text[12],
                     sizeof(context->linexe_get_proc_name) - 1U);
        // Task 751. A name the export plan does not have is what the guest
        // reports as "unable to find entry point in DLL"; say which.
        if (glide_export == nullptr)
        {
            std::fprintf(stderr,
                         "[repiu-linexe] get-proc miss name=%s after=%u"
                         " found" "\n",
                         context->linexe_bridge_stack_text[12],
                         static_cast<unsigned>(
                             context->linexe_get_proc_count));
        }
    }
    if (service == repiu::hle::LinexeService::kGetProcedureAddress &&
        context->linexe_bridge_stack[11] == kVirtualGlideModuleHandle &&
        glide_export != nullptr)
    {
        const std::uint32_t old_esp =
            static_cast<std::uint32_t>(win32_context->Esp);
        const std::uint32_t result_pointer =
            context->linexe_bridge_stack[13];
        const std::uint32_t gate_address =
            context->linexe_arena_layout.gate_code_base +
            glide_export->gate_offset;
        std::uint16_t client_code_selector = 0U;
        if (!repiu::runtime::FindSelectorForLinearAddress(
                context->selector_table,
                context->linexe_bridge_stack[10],
                &client_code_selector))
        {
            return false;
        }
        const repiu::runtime::GuestDescriptor* client_code_descriptor =
            repiu::runtime::FindDescriptor(
                context->selector_table, client_code_selector);
        if (client_code_descriptor == nullptr ||
            !client_code_descriptor->executable)
        {
            return false;
        }
        const std::uint32_t procedure_pointer[2] = {
            gate_address,
            static_cast<std::uint32_t>(client_code_selector),
        };
        if (!WriteGuestBytes(
                context,
                reinterpret_cast<void*>(
                    static_cast<std::uintptr_t>(result_pointer)),
                procedure_pointer,
                sizeof(procedure_pointer)))
        {
            return false;
        }

        ++context->linexe_get_proc_count;
        context->linexe_get_proc_result_pointer = gate_address;
        std::strncpy(context->linexe_get_proc_name,
                     glide_export->name.c_str(),
                     sizeof(context->linexe_get_proc_name) - 1U);
        win32_context->Eax = 1U;
        SetGuestSegmentSelector(
            context, 0U,
            static_cast<std::uint16_t>(
                context->linexe_bridge_stack[5] & 0xFFFFU));
        ReResolveAotSegmentOverrides(context);
        win32_context->Ebx = context->linexe_bridge_stack[6];
        win32_context->Esi = context->linexe_bridge_stack[7];
        win32_context->Edi = context->linexe_bridge_stack[8];
        win32_context->Ebp = context->linexe_bridge_stack[9];
        win32_context->Eip = context->linexe_bridge_stack[10];
        win32_context->Esp += 11U * sizeof(std::uint32_t);
        if (trace_sequence != 0U)
        {
            std::fprintf(stderr,
                         "[repiu-linexe-far] n=%u event=return "
                         "service=%s old_esp=0x%08X new_esp=0x%08X "
                         "eip=0x%08X eax=0x%08X ebx=0x%08X esi=0x%08X "
                         "edi=0x%08X ebp=0x%08X\n",
                         static_cast<unsigned>(trace_sequence),
                         LinexeServiceName(service),
                         static_cast<unsigned>(old_esp),
                         static_cast<unsigned>(win32_context->Esp),
                         static_cast<unsigned>(win32_context->Eip),
                         static_cast<unsigned>(win32_context->Eax),
                         static_cast<unsigned>(win32_context->Ebx),
                         static_cast<unsigned>(win32_context->Esi),
                         static_cast<unsigned>(win32_context->Edi),
                         static_cast<unsigned>(win32_context->Ebp));
        }
        return true;
    }
    if (service != repiu::hle::LinexeService::kLoadModule ||
        !is_glide_module)
    {
        return false;
    }

    ++context->linexe_virtual_module_load_count;
    context->linexe_virtual_module_handle = kVirtualGlideModuleHandle;
    win32_context->Eax = kVirtualGlideModuleHandle;
    SetGuestSegmentSelector(
        context, 0U,
        static_cast<std::uint16_t>(
            context->linexe_bridge_stack[3] & 0xFFFFU));
    ReResolveAotSegmentOverrides(context);
    win32_context->Ebx = context->linexe_bridge_stack[4];
    win32_context->Esi = context->linexe_bridge_stack[5];
    win32_context->Edi = context->linexe_bridge_stack[6];
    win32_context->Ebp = context->linexe_bridge_stack[7];
    win32_context->Eip = context->linexe_bridge_stack[8];
    const std::uint32_t old_esp =
        static_cast<std::uint32_t>(win32_context->Esp);
    win32_context->Esp += 9U * sizeof(std::uint32_t);
    if (trace_sequence != 0U)
    {
        std::fprintf(stderr,
                     "[repiu-linexe-far] n=%u event=return "
                     "service=%s old_esp=0x%08X new_esp=0x%08X "
                     "eip=0x%08X eax=0x%08X ebx=0x%08X esi=0x%08X "
                     "edi=0x%08X ebp=0x%08X\n",
                     static_cast<unsigned>(trace_sequence),
                     LinexeServiceName(service),
                     static_cast<unsigned>(old_esp),
                     static_cast<unsigned>(win32_context->Esp),
                     static_cast<unsigned>(win32_context->Eip),
                     static_cast<unsigned>(win32_context->Eax),
                     static_cast<unsigned>(win32_context->Ebx),
                     static_cast<unsigned>(win32_context->Esi),
                     static_cast<unsigned>(win32_context->Edi),
                     static_cast<unsigned>(win32_context->Ebp));
    }
    return true;
}

namespace
{

// Task 750. Where the execution model can inject during the wait (the cache
// model, Linux x64) it is on unless `0` turns it off. On the direct model the
// first injected tick never returned to the call -- the run stalled without
// a fault on Win32 -- so the switch does not reach those hosts until that
// continuation is made to work.
bool GlideSwapWaitTicksEnabled()
{
    if (!runtime::execution_model::InjectsTicksDuringSwapWait())
    {
        return false;
    }
    // #6. On the direct model the gate leaves into the timer handler through
    // the interrupt exit; without it the swap stays synchronous.
    if (runtime::execution_model::RunsGuestBytesDirectly() &&
        GlideGateInterruptExitAddress() == 0U)
    {
        return false;
    }
    static const bool enabled = [] {
        const char* const value = std::getenv("REPIU_GLIDE_SWAP_WAIT_TICKS");
        return value == nullptr || value[0] != '0';
    }();
    return enabled;
}

// #6. How long a swap wait runs before owed ticks are injected into it.
//
// The cache model injects from the start: that is what gives the guest its
// ticks on time within a frame (Task 750). On the direct model the same
// injection costs frames -- pumpit1 drew about 15% fewer with it than
// without, measured back to back with the window visible -- so there it is
// held until the wait has outlasted any ordinary swap. A swap that blocks
// longer than that is the case the direct model needs it for: a hidden
// window, whose swaps the compositor slows to about one a second, during
// which ticks used to be dropped past the backlog cap.
// `REPIU_GLIDE_SWAP_WAIT_TICK_HOLD_MS` overrides it.
std::chrono::milliseconds GlideSwapWaitTickHold()
{
    static const std::chrono::milliseconds hold = [] {
        const char* const value =
            std::getenv("REPIU_GLIDE_SWAP_WAIT_TICK_HOLD_MS");
        if (value != nullptr && value[0] != '\0')
        {
            char* end = nullptr;
            const long parsed = std::strtol(value, &end, 10);
            if (end != value && *end == '\0' && parsed >= 0L &&
                parsed <= 10000L)
            {
                return std::chrono::milliseconds(parsed);
            }
        }
        return std::chrono::milliseconds(
            runtime::execution_model::RunsGuestBytesDirectly() ? 50 : 0);
    }();
    return hold;
}

// Task 750. The address of the `call rel32` that reached the gate, or zero.
// The call goes either to the gate itself or to an import thunk that jumps
// there: `jmp rel32` (what the loader's fixups leave in pumpitea) or
// `jmp dword ptr [m32]`.
std::uint32_t FindGlideGateCallSite(ThreadContext* context,
                                    const std::uint32_t return_address,
                                    const std::uint32_t gate_eip)
{
    if (return_address < 5U)
    {
        return 0U;
    }
    const std::uint32_t call_eip = return_address - 5U;
    const auto* const call = reinterpret_cast<const std::uint8_t*>(
        static_cast<std::uintptr_t>(call_eip));
    if (!IsGuestInstructionPointer(context, call_eip) ||
        !IsGuestRangeReadable(context, call, 5U) || call[0] != 0xE8U)
    {
        return 0U;
    }
    std::uint32_t displacement = 0U;
    std::memcpy(&displacement, call + 1, sizeof(displacement));
    const std::uint32_t target = return_address + displacement;
    if (target == gate_eip)
    {
        return call_eip;
    }
    const auto* const thunk = reinterpret_cast<const std::uint8_t*>(
        static_cast<std::uintptr_t>(target));
    if (!IsGuestRangeReadable(context, thunk, 6U))
    {
        return 0U;
    }
    if (thunk[0] == 0xE9U)
    {
        std::uint32_t jump = 0U;
        std::memcpy(&jump, thunk + 1, sizeof(jump));
        return target + 5U + jump == gate_eip ? call_eip : 0U;
    }
    if (thunk[0] != 0xFFU || thunk[1] != 0x25U)
    {
        return 0U;
    }
    std::uint32_t slot = 0U;
    std::memcpy(&slot, thunk + 2, sizeof(slot));
    const auto* const slot_pointer = reinterpret_cast<const std::uint8_t*>(
        static_cast<std::uintptr_t>(slot));
    std::uint32_t slot_target = 0U;
    if (!IsGuestRangeReadable(context, slot_pointer, sizeof(slot_target)))
    {
        return 0U;
    }
    std::memcpy(&slot_target, slot_pointer, sizeof(slot_target));
    return slot_target == gate_eip ? call_eip : 0U;
}

// Task 750. The guest waits a posted swap out here, the way the real machine
// waits a vblank: with IRQ0 still arriving. While the present (and its pacing)
// runs on the host thread, an owed tick is injected as if the interrupt had
// arrived just before the `call` that reached the gate: the frame returns to
// that call with the return address popped, so the ISR's `iret` runs the call
// again and this gate is entered again; only when the swap has been presented
// does the gate return to its caller. The call site rather than the gate is
// the return target because the gate's own segment is not one the guest's
// code selector covers. Without this the guest saw an MP3 frame boundary
// 0-16 ms late, once per frame, and its song position advanced in 16/33 ms
// steps instead of 26 ms.
bool ContinueGlideSwapWait(repiu::platform::GuestCpuContext* win32_context,
                           ThreadContext* context,
                           const std::uint32_t return_address)
{
    const std::uint32_t gate_eip =
        static_cast<std::uint32_t>(win32_context->Eip);
    const std::uint32_t gate_esp =
        static_cast<std::uint32_t>(win32_context->Esp);
    const std::uint32_t call_eip =
        FindGlideGateCallSite(context, return_address, gate_eip);
    // `REPIU_GLIDE_SWAP_WAIT_LOG=1`: the first few waits that could not
    // deliver a tick, and why.
    static const bool wait_log =
        std::getenv("REPIU_GLIDE_SWAP_WAIT_LOG") != nullptr;
    static std::uint32_t wait_log_count = 0U;
    if (wait_log && call_eip == 0U && wait_log_count < 8U)
    {
        ++wait_log_count;
        fprintf(stderr,
                "[repiu-glide-swap] wait-tick no call site ret=0x%08X"
                " gate=0x%08X" "\n",
                return_address, gate_eip);
    }
    while (!context->glide_backend.WaitForPendingSwaps(
        std::chrono::microseconds(0)))
    {
        // #6: nothing is injected until the wait has lasted the hold.
        const bool past_hold =
            std::chrono::steady_clock::now() - context->glide_swap_wait_begin >=
            GlideSwapWaitTickHold();
        if (call_eip != 0U && past_hold)
        {
            std::lock_guard<std::mutex> arm_lock(context->timer_tick_arm_mutex);
            if (context->timer_tick_arm)
            {
                context->timer_tick_arm();
            }
        }
        if (call_eip != 0U && past_hold &&
            context->timer_interrupt_pending.load(std::memory_order_acquire))
        {
            win32_context->Eip = call_eip;
            win32_context->Esp = gate_esp + sizeof(std::uint32_t);
            InjectPendingInterrupts(win32_context, context);
            if (static_cast<std::uint32_t>(win32_context->Eip) != call_eip)
            {
                context->glide_gate_interrupt_injected = true;
                context->glide_backend.NoteSwapWaitInjection();
                return true;
            }
            win32_context->Eip = gate_eip;
            win32_context->Esp = gate_esp;
            if (wait_log && wait_log_count < 8U)
            {
                ++wait_log_count;
                fprintf(stderr,
                        "[repiu-glide-swap] wait-tick refused call=0x%08X"
                        " eflags=0x%08X in_service=%d" "\n",
                        call_eip,
                        static_cast<unsigned>(win32_context->EFlags),
                        context->pic_timer_in_service.active ? 1 : 0);
            }
        }
        context->glide_backend.WaitForPendingSwaps(
            std::chrono::microseconds(500));
    }
    context->glide_swap_wait_active = false;
    context->glide_backend_message = context->glide_backend.message();
    ++context->glide_gate_handled_count;
    win32_context->Eip = return_address;
    win32_context->Esp += 2U * sizeof(std::uint32_t);
    return true;
}

}  // namespace

bool HandleGlideGateBoundary(repiu::platform::GuestCpuContext* win32_context,
                             ThreadContext* context)
{
    if (context != nullptr)
    {
        context->glide_gate_interrupt_injected = false;
    }
    const std::uint32_t gate_begin =
        context != nullptr
            ? context->linexe_arena_layout.gate_code_base +
                context->glide_gate_plan.first_gate_offset
            : 0U;
    if (win32_context == nullptr || context == nullptr ||
        !context->linexe_environment_active ||
        win32_context->Eip < gate_begin)
    {
        return false;
    }

    // Task 323: measured after the cheap rejection so the bucket counts real
    // gate work only. This includes any wait on the SDL main-thread render
    // queue, which is exactly what the measurement needs to expose.
    std::uint64_t ordinal_gate_cycles = 0U;
    GlideOrdinalTimingScope ordinal_timing_scope(
        GlideOrdinalTimingProfileEnabled()
            ? &context->glide_ordinal_timing
            : nullptr,
        &context->glide_backend,
        &ordinal_gate_cycles);
    // Task 438: draw primitives are queued and handed over once per ordering
    // boundary rather than one rendezvous each.
    GlideDrawBatch* const draw_batch = &context->glide_draw_batch;
    // Tasks 364/365: declared here so its destructor observes the dispatch outcome
    // on every return path, and begun below once the argument mirror is filled.
    GlideSetterStateScope setter_state_scope(
        GlideSetterCensusEnabled() ? &context->glide_setter_census : nullptr,
        &context->glide_setter_state_cache,
        context);
    const ExecutionTimeScope gate_time_scope(
        context->execution_time_profile.get(),
        ExecutionTimeBucket::kGlideGate,
        &ordinal_gate_cycles);

    const std::uint32_t gate_offset =
        static_cast<std::uint32_t>(win32_context->Eip) -
        context->linexe_arena_layout.gate_code_base;
    const repiu::hle::GlideExportGate* glide_export =
        repiu::hle::DecodeGlideGate(context->glide_gate_plan, gate_offset);
    if (glide_export == nullptr)
    {
        return false;
    }

    using go = repiu::hle::GlideGateId;
    ordinal_timing_scope.Begin(glide_export->ordinal);

    ++context->glide_gate_entry_count;
    // Task 335: no event pump here. Pumping at gate entry cost a full host-thread
    // rendezvous -- 1.92 rendezvous per gate entry, the second one being the
    // pump -- while the host poll loop already pumps every iteration, roughly
    // every 0.68ms since Task 333 and immediately whenever a command is posted.
    context->glide_gate_ordinal = glide_export->ordinal;
    context->glide_gate_argument_bytes = glide_export->argument_byte_count;
    std::memset(context->glide_gate_name,
                0,
                sizeof(context->glide_gate_name));
    std::strncpy(context->glide_gate_name,
                 glide_export->name.c_str(),
                 sizeof(context->glide_gate_name) - 1U);
    context->glide_gate_esp = win32_context->Esp;
    const auto* stack = reinterpret_cast<const std::uint32_t*>(
        static_cast<std::uintptr_t>(win32_context->Esp));
    if (IsGuestRangeReadable(context,
                             stack,
                             sizeof(context->glide_gate_stack)))
    {
        std::memcpy(context->glide_gate_stack,
                    stack,
                    sizeof(context->glide_gate_stack));
    }
    setter_state_scope.Begin(*glide_export);
    if (glide_export->ordinal < context->glide_call_counts.size())
    {
        const std::size_t ordinal = glide_export->ordinal;
        if (context->glide_call_counts[ordinal]++ == 0U)
        {
            context->glide_call_names[ordinal] = glide_export->name;
            std::copy(std::begin(context->glide_gate_stack),
                      std::end(context->glide_gate_stack),
                      context->glide_first_stacks[ordinal].begin());
            if (context->shared_live_telemetry != nullptr)
            {
                using go = repiu::hle::GlideGateId;
                volatile std::int32_t* milestone = nullptr;
                const repiu::hle::GlideGateId gate_id = glide_export->gate_id;
                if (gate_id == go::kGrSstWinOpen)
                {
                    milestone = &context->shared_live_telemetry
                        ->glide_window_gate_milestone;
                }
                else if (gate_id == go::kGrTexDownloadMipMapLevel)
                {
                    milestone = &context->shared_live_telemetry
                        ->glide_texture_milestone;
                }
                // Task 420: the antialiased entry points draw too now, and they
                // sit outside this contiguous block, so they are named here
                // rather than left out of the draw milestone.
                else if ((gate_id >= go::kGrDrawLine &&
                          gate_id <= go::kGrDrawPolygon) ||
                         (gate_id >= go::kGrAADrawPoint &&
                          gate_id <= go::kGrDrawPolygonVertexList))
                {
                    milestone = &context->shared_live_telemetry
                        ->glide_draw_milestone;
                }
                else if (gate_id == go::kGrBufferSwap)
                {
                    milestone = &context->shared_live_telemetry
                        ->glide_swap_milestone;
                }
                if (milestone != nullptr)
                {
                    repiu::platform::AtomicExchange(milestone, 1L);
                }
            }
        }
    }
    // Task 255 R3 observation (env-gated, off by default): dump the real
    // arguments of the texture/combine gates during content draws so the actual
    // format, dimensions, and combine mode can be confirmed before implementing
    // texture decode/upload/sampling. The gate stack mirror holds only 8 dwords,
    // so read directly from the guest stack for the wider download call.
    {
        using go = repiu::hle::GlideGateId;
        static const bool tex_diagnostic_enabled =
            std::getenv("REPIU_GLIDE_TEX_DIAG") != nullptr;
        const repiu::hle::GlideGateId gate_id = glide_export->gate_id;
        if (tex_diagnostic_enabled &&
            (gate_id == go::kGrTexDownloadMipMapLevel ||
             gate_id == go::kGrTexSource ||
             gate_id == go::kGrTexCombine ||
             gate_id == go::kGrColorCombine ||
             gate_id == go::kGrAlphaBlendFunction ||
             gate_id == go::kGrAlphaTestFunction ||
             gate_id == go::kGrAlphaCombine))
        {
            static long tex_diag_count = 0;
            const long diag_index = repiu::platform::AtomicIncrement(&tex_diag_count);
            if (diag_index <= 256)
            {
                std::uint32_t args[9] = {};
                const auto* guest_stack =
                    reinterpret_cast<const std::uint32_t*>(
                        static_cast<std::uintptr_t>(win32_context->Esp));
                if (IsGuestRangeReadable(context, guest_stack, sizeof(args)))
                {
                    std::memcpy(args, guest_stack, sizeof(args));
                }
                fprintf(stderr,
                        "[repiu-live-debug] tex-diag #%ld %s args=%08X %08X %08X"
                        " %08X %08X %08X %08X %08X\n",
                        diag_index, glide_export->name.c_str(),
                        args[1], args[2], args[3], args[4],
                        args[5], args[6], args[7], args[8]);
            }
        }
    }
    // Task 246: gate traffic is tiny (tens of calls), so log every entry to
    // pin which call leaks its stack frame (an unhandled entry resumes the
    // caller without the stdcall cleanup, offsetting ESP for the rest of the
    // frame — the confirmed zero-EIP mechanism at 0x0304ED35).
    {
        static long gate_entry_log_count = 0;
        const long entry_index = repiu::platform::AtomicIncrement(&gate_entry_log_count);
        if (entry_index <= 96)
        {
            fprintf(stderr,
                    "[repiu-live-debug] glide gate entry #%ld ordinal=%u"
                    " name=%s ret=0x%08X esp=0x%08X\n",
                    entry_index, glide_export->ordinal,
                    glide_export->name.c_str(), context->glide_gate_stack[0],
                    static_cast<std::uint32_t>(win32_context->Esp));
        }
    }
    if (context->shared_live_telemetry != nullptr)
    {
        repiu::platform::AtomicExchange(
            &context->shared_live_telemetry->glide_gate_ordinal,
            static_cast<long>(glide_export->ordinal));
        repiu::platform::AtomicExchange(
            &context->shared_live_telemetry->glide_gate_esp,
            static_cast<long>(win32_context->Esp));
        repiu::platform::AtomicExchange(
            &context->shared_live_telemetry->glide_gate_ebx,
            static_cast<long>(win32_context->Ebx));
        repiu::platform::AtomicExchange(
            &context->shared_live_telemetry->glide_gate_ecx,
            static_cast<long>(win32_context->Ecx));
        repiu::platform::AtomicExchange(
            &context->shared_live_telemetry->glide_gate_edx,
            static_cast<long>(win32_context->Edx));
        for (std::size_t index = 0; index < 8U; ++index)
        {
            repiu::platform::AtomicExchange(
                &context->shared_live_telemetry->glide_gate_stack[index],
                static_cast<long>(context->glide_gate_stack[index]));
        }
    }
    const std::uint32_t return_address = context->glide_gate_stack[0];
    if (!IsGuestInstructionPointer(context, return_address))
    {
        RecordGlideImplementationIssue(
            win32_context,
            context,
            *glide_export,
            repiu::hle::GlideImplementationIssueKind::kAbiReject,
            "return-address-not-guest",
            {},
            "terminate");
        return false;
    }
    const repiu::hle::GlideSignature* signature =
        repiu::hle::FindGlideSignature(glide_export->name);
    if (signature == nullptr)
    {
        RecordGlideImplementationIssue(
            win32_context,
            context,
            *glide_export,
            repiu::hle::GlideImplementationIssueKind::
                kUnimplementedFunction,
            "signature-not-cataloged",
            {},
            "terminate");
        return false;
    }
    if (signature->argument_byte_count !=
        glide_export->argument_byte_count)
    {
        const std::string detail =
            "catalog-argument-bytes=" +
            std::to_string(signature->argument_byte_count);
        RecordGlideImplementationIssue(
            win32_context,
            context,
            *glide_export,
            repiu::hle::GlideImplementationIssueKind::kAbiReject,
            "signature-mismatch",
            detail,
            "terminate");
        return false;
    }
    // Task 365: an exact repeat of a state already applied successfully on the
    // host skips only the `InvokeOnHostThread` round trip. Everything the case
    // body would have done that the guest can observe still happens here: the
    // handled count, the return address, and the stdcall cleanup. The
    // `glide_state` mirror write is skipped safely because it is idempotent --
    // the application this key matches already wrote the identical values, and
    // the key holds every argument dword.
    //
    // Placed after the return-address, signature, and argument-size checks so a
    // gate that would have been rejected is never elided. `message_` is left
    // alone: it is host-owned and writing it from the guest thread would race,
    // and the message from the matching application is still accurate.
    //
    // Batch one is void-returning throughout, so `Eax` is untouched.
    if (setter_state_scope.ShouldElide())
    {
        setter_state_scope.MarkElided();
        ++context->glide_gate_handled_count;
        win32_context->Eip = return_address;
        win32_context->Esp +=
            sizeof(std::uint32_t) + signature->argument_byte_count;
        return true;
    }
    const auto decline_gate = [context, win32_context, glide_export, signature,
                               return_address](const char* reason) {
        const std::string_view reason_view(reason);
        const bool backend_failure =
            reason_view.find("backend-failure") != std::string_view::npos;
        const std::string_view detail =
            backend_failure ? std::string_view(context->glide_backend_message)
                            : std::string_view{};
        const bool unsupported_argument =
            backend_failure && detail.starts_with("unsupported Glide");
        RecordGlideImplementationIssue(
            win32_context,
            context,
            *glide_export,
            unsupported_argument
                ? repiu::hle::GlideImplementationIssueKind::
                      kUnsupportedArgument
                : repiu::hle::GlideImplementationIssueKind::kBackendFailure,
            reason_view,
            detail,
            "continue");
        ++context->glide_gate_handled_count;
        if (signature->return_kind != repiu::hle::GlideReturnKind::kVoid)
        {
            win32_context->Eax = 0U;
        }
        win32_context->Eip = return_address;
        win32_context->Esp +=
            sizeof(std::uint32_t) + signature->argument_byte_count;
        return true;
    };
    const auto record_unimplemented =
        [context, win32_context, glide_export](
            const std::string_view reason,
            const std::string_view detail) {
            RecordGlideImplementationIssue(
                win32_context,
                context,
                *glide_export,
                repiu::hle::GlideImplementationIssueKind::
                    kUnimplementedFunction,
                reason,
                detail,
                "continue");
        };
    const auto record_unsupported =
        [context, win32_context, glide_export](
            const std::string_view reason,
            const std::string_view detail) {
            RecordGlideImplementationIssue(
                win32_context,
                context,
                *glide_export,
                repiu::hle::GlideImplementationIssueKind::
                    kUnsupportedArgument,
                reason,
                detail,
                "continue");
        };
    const auto record_backend_failure =
        [context, win32_context, glide_export](
            const std::string_view reason,
            const std::string_view detail) {
            RecordGlideImplementationIssue(
                win32_context,
                context,
                *glide_export,
                repiu::hle::GlideImplementationIssueKind::kBackendFailure,
                reason,
                detail,
                "continue");
        };
    using go = repiu::hle::GlideGateId;
    // Task 438: the whole ordering contract, in one rule. Anything that is not a
    // queueable draw flushes first, so state changes, queries, swap, clear, LFB
    // and downloads all see every triangle that preceded them without any of
    // them having to be enumerated -- an omission in such a list would be an
    // ordering bug that only shows up as a wrong picture.
    //
    // Placed after the elision short-circuit above on purpose: an elided setter
    // returns before this point, changes nothing, and therefore must not force
    // a flush. That is what leaves batches long enough to be worth having.
    //
    // Task 476: the LFB region shadow follows the same rule for the same
    // reason. It goes first so pixels reach the frame buffer before anything
    // else touches it, and it invalidates unconditionally because any other
    // gate -- a draw, a clear, a swap -- may change what the shadow claims to
    // mirror. Region gates are non-draw gates, so the batch is always empty
    // while the shadow is dirty and the two flushes cannot reorder each other.
    if (!IsGlideLfbRegionGate(glide_export->gate_id) &&
        (context->glide_lfb_region_shadow_valid ||
         context->glide_lfb_region_shadow_dirty))
    {
        FlushGlideLfbRegionShadow(context);
        context->glide_lfb_region_shadow_valid = false;
        // A flush presents pixels and rewrites the shared surface. The region
        // gates that made the shadow live already invalidated the lock shadow
        // below, so this cannot be reached with both live; it is stated anyway
        // so the two shadows of one surface do not depend on that ordering.
        InvalidateGlideLfbStagingShadows(
            &context->glide_lfb_staging_shadow,
            GlideLfbStagingShadowInvalidation::kRegionGate);
    }
    // Task 728: the lock/unlock shadow of the same staging surface. Unlike the
    // region shadow above it survives the state setters a guest issues between
    // two locks, so the predicate is consulted rather than invalidating on
    // every gate. It runs after the region flush because that flush presents
    // pixels, which is exactly the kind of change the shadow must not outlive
    // -- and the region gates that produced it are themselves invalidating.
    if (GlideLfbStagingShadowCensusEnabled())
    {
        // Task 729: a swap rotates the pair rather than discarding it, which
        // is the page flip the guest's `lock -> unlock -> swap -> lock` pattern
        // depends on. Everything else either invalidates the render target or,
        // when this layer cannot say which buffer it touched, the whole set.
        ApplyGlideLfbStagingShadowGate(&context->glide_lfb_staging_shadow,
                                       glide_export->gate_id);
    }
    // Task 729: what fills the window between a write unlock and the next write
    // lock. Independent of the shadow above, which stops counting once it is
    // invalid and so cannot see past the swap that broke it.
    if (GlideLfbLockIntervalCensusEnabled())
    {
        NoteGlideLfbLockIntervalGate(&context->glide_lfb_lock_interval_census,
                                     glide_export->gate_id);
    }
    if (!IsGlideDrawBatchGate(glide_export->gate_id))
    {
        FlushGlideDrawBatchToBackend(
            context, GlideDrawBatchFlushReason::kNonDrawGate);
    }
    // 100% Unified GateId O(1) Switch Dispatcher (Task 321)
    switch (glide_export->gate_id)
    {
        case go::kGrHints: // _GRHINTS@8
        {
            // grHints(GrHint_t type, FxU32 mask) declares driver optimizations,
            // not rendering state. STWHINT says which w and s/t values are
            // unique per TMU, FIFOCHECKHINT how often Glide polls the command
            // FIFO, FPUPRECISION what precision Glide may use for its own math,
            // and ALLOW_MIPMAP_DITHER whether mipmap dithering is permitted.
            //
            // The `GrVertex` layout is fixed by the ABI, so a renderer that
            // reads the structure directly draws identically whatever the hint
            // says. Recording the declaration is therefore a complete
            // implementation for this backend rather than a stub, which is why
            // this no longer reports an implementation gap. An unknown hint
            // type still does, because that would be a real gap.
            const std::uint32_t hint_type = context->glide_gate_stack[1];
            const std::uint32_t hint_mask = context->glide_gate_stack[2];
            constexpr std::uint32_t kHintStwHint = 0U;
            constexpr std::uint32_t kHintFifoCheckHint = 1U;
            constexpr std::uint32_t kHintFpuPrecision = 2U;
            constexpr std::uint32_t kHintAllowMipmapDither = 3U;
            // Bits above the three-TMU set are not defined by Glide 2.x.
            constexpr std::uint32_t kStwHintMask = 0x7FU;
            switch (hint_type)
            {
                case kHintStwHint:
                    if ((hint_mask & ~kStwHintMask) != 0U)
                    {
                        RecordGlideImplementationIssue(
                            win32_context, context, *glide_export,
                            repiu::hle::GlideImplementationIssueKind::
                                kUnsupportedArgument,
                            "stw-hint-reserved-bits",
                            "STW hint carries bits outside the Glide 2.x set",
                            "continue");
                    }
                    context->glide_state.stw_hint = hint_mask;
                    break;
                case kHintFifoCheckHint:
                    context->glide_state.fifo_check_hint = hint_mask;
                    break;
                case kHintFpuPrecision:
                    context->glide_state.fpu_precision_hint = hint_mask;
                    break;
                case kHintAllowMipmapDither:
                    context->glide_state.allow_mipmap_dither_hint = hint_mask;
                    break;
                default:
                    RecordGlideImplementationIssue(
                        win32_context, context, *glide_export,
                        repiu::hle::GlideImplementationIssueKind::
                            kUnsupportedArgument,
                        "unknown-hint-type",
                        "hint type is outside GR_HINTTYPE_MIN..MAX",
                        "continue");
                    break;
            }
            context->glide_state.hints_seen = true;
            ++context->glide_gate_handled_count;
            win32_context->Eip = return_address;
            win32_context->Esp += 3U * sizeof(std::uint32_t);
            return true;
        }
        case go::kGrGlideInit: // _GRGLIDEINIT@0
            context->glide_state.initialized = true;
            ++context->glide_gate_handled_count;
            win32_context->Eip = return_address;
            win32_context->Esp += sizeof(std::uint32_t);
            return true;

        case go::kGrSstQueryHardware: // _GRSSTQUERYHARDWARE@4
        {
            void* configuration = reinterpret_cast<void*>(
                static_cast<std::uintptr_t>(context->glide_gate_stack[1]));
            if (!WriteGuestUInt32(context, configuration, 1U, win32_context))
            {
                return decline_gate("query-hardware-unwritable-memory");
            }
            ++context->glide_gate_handled_count;
            win32_context->Eax = 1U;
            win32_context->Eip = return_address;
            win32_context->Esp += 2U * sizeof(std::uint32_t);
            return true;
        }

        case go::kGrSstSelect: // _GRSSTSELECT@4
        {
            const std::uint32_t board = context->glide_gate_stack[1];
            if (board == 0U)
            {
                context->glide_state.selected_board = 0U;
            }
            else
            {
                record_unsupported(
                    "board-selection-unsupported",
                    "only board zero is implemented");
            }
            ++context->glide_gate_handled_count;
            win32_context->Eip = return_address;
            win32_context->Esp += 2U * sizeof(std::uint32_t);
            return true;
        }

        case go::kGrSstWinOpen: // _GRSSTWINOPEN@28
        {
            const std::uint32_t window = context->glide_gate_stack[1];
            const std::uint32_t resolution = context->glide_gate_stack[2];
            const std::uint32_t refresh = context->glide_gate_stack[3];
            const std::uint32_t color_format = context->glide_gate_stack[4];
            const std::uint32_t origin = context->glide_gate_stack[5];
            const std::uint32_t color_buffers = context->glide_gate_stack[6];
            const std::uint32_t auxiliary_buffers =
                context->glide_gate_stack[7];
            std::uint32_t width = 0;
            std::uint32_t height = 0;
            const bool mode_supported = window == 0U && refresh == 0U &&
                repiu::hle::DecodeGlideResolution(
                    resolution, &width, &height);
            bool opened = false;
            try
            {
                opened = mode_supported &&
                    context->glide_backend.OpenWindowed(
                        width, height, color_buffers, auxiliary_buffers, origin);
            }
            catch (const std::exception& e)
            {
                fprintf(stderr, "[repiu-live-debug] _GRSSTWINOPEN@28 caught standard exception: %s\n", e.what());
            }
            catch (...)
            {
                fprintf(stderr, "[repiu-live-debug] _GRSSTWINOPEN@28 caught unknown exception\n");
            }
            context->glide_backend_message = context->glide_backend.message();
            if (!mode_supported)
            {
                record_unsupported(
                    "window-mode-unsupported",
                    "window, refresh, or resolution is unsupported");
            }
            else if (!opened)
            {
                record_backend_failure(
                    "window-open-backend-failure",
                    context->glide_backend_message);
            }
            fprintf(stderr, "[repiu-live-debug] _GRSSTWINOPEN@28: mode_supported=%d opened=%d message=%s\n",
                    mode_supported ? 1 : 0, opened ? 1 : 0, context->glide_backend_message.c_str());
            if (opened)
            {
                ++context->glide_window_open_count;
                if (context->shared_live_telemetry != nullptr)
                {
                    repiu::platform::AtomicExchange(
                        &context->shared_live_telemetry
                             ->glide_window_open_milestone,
                        1L);
                }
                context->glide_logical_width = width;
                context->glide_logical_height = height;
                context->glide_state.window_open = true;
                context->glide_state.width = width;
                context->glide_state.height = height;
                context->glide_state.color_format = color_format;
                context->glide_state.lfb_write_color_format = color_format;
                context->glide_state.origin = origin;
                context->glide_state.color_buffer_count = color_buffers;
                context->glide_state.auxiliary_buffer_count = auxiliary_buffers;
            }
            win32_context->Eax = opened ? 1U : 0U;
            ++context->glide_gate_handled_count;
            win32_context->Eip = return_address;
            win32_context->Esp += 8U * sizeof(std::uint32_t);
            return true;
        }

        case go::kGrSstWinClose: // _GRSSTWINCLOSE@0
            context->glide_backend.Close();
            context->glide_state.window_open = false;
            ++context->glide_gate_handled_count;
            win32_context->Eip = return_address;
            win32_context->Esp += sizeof(std::uint32_t);
            return true;

        case go::kGrSstScreenWidth: // _GRSSTSCREENWIDTH@0
            win32_context->Eax = context->glide_state.width;
            ++context->glide_gate_handled_count;
            win32_context->Eip = return_address;
            win32_context->Esp += sizeof(std::uint32_t);
            return true;

        case go::kGrSstScreenHeight: // _GRSSTSCREENHEIGHT@0
            win32_context->Eax = context->glide_state.height;
            ++context->glide_gate_handled_count;
            win32_context->Eip = return_address;
            win32_context->Esp += sizeof(std::uint32_t);
            return true;

        case go::kGrTexTextureMemRequired: // _GRTEXTEXTUREMEMREQUIRED@8
        {
            repiu::hle::GlideTextureInfo info;
            const void* info_address = reinterpret_cast<const void*>(
                static_cast<std::uintptr_t>(context->glide_gate_stack[2]));
            std::uint32_t required_bytes = 0;
            if (!IsGuestRangeReadable(context, info_address, sizeof(info)))
            {
                return decline_gate("set-state-unreadable-memory");
            }
            // Task 332: the guest's GrTexInfo has to be READ. Until now the
            // pointer was only used for the readability check and a
            // default-constructed, all-zero struct went into the calculation,
            // so every texture answered 8192 bytes -- LOD 0 with aspect 8x1 at
            // one byte per texel. The guest sizes its own TMU address space from
            // this answer (see docs/kb/glide-texture-lod-and-formats.md), so it
            // was packing 256x256 maps 0x2000 apart and overwriting them.
            //
            // The guest layout is five 32-bit fields, confirmed from the raw
            // bytes at this pointer: smallLod, largeLod, aspectRatio, format,
            // data, matching `GlideTextureInfo` exactly.
            std::memcpy(&info, info_address, sizeof(info));
            if (!repiu::hle::CalculateGlideTextureMemoryRequired(
                    context->glide_gate_stack[1], info, &required_bytes))
            {
                return decline_gate("texture-memory-required-invalid-info");
            }
            ++context->glide_gate_handled_count;
            win32_context->Eax = required_bytes;
            win32_context->Eip = return_address;
            win32_context->Esp += 3U * sizeof(std::uint32_t);
            return true;
        }
        case go::kGrTexDownloadTable: // _GRTEXDOWNLOADTABLE@12
        {
            // grTexDownloadTable(GrChipID_t tmu, GrTexTable_t type, void* data).
            // The frame includes the return address before its three arguments.
            std::uint32_t frame[4] = {};
            const auto* guest_stack = reinterpret_cast<const std::uint32_t*>(
                static_cast<std::uintptr_t>(win32_context->Esp));
            GlideTexDownloadTableCall call;
            call.stack_advance = sizeof(frame);
            if (IsGuestRangeReadable(context, guest_stack, sizeof(frame)))
            {
                std::memcpy(frame, guest_stack, sizeof(frame));
                DecodeGlideTexDownloadTableCall(
                    frame, sizeof(frame) / sizeof(frame[0]), &call);
                const auto* data = reinterpret_cast<const std::uint32_t*>(
                    static_cast<std::uintptr_t>(call.data));
                if (call.type != 2U)
                {
                    record_unsupported(
                        "texture-table-type-unsupported",
                        "only GR_TEXTABLE_PALETTE is implemented");
                }
                else if (!IsGuestRangeReadable(context, data, 1024U))
                {
                    record_backend_failure(
                        "texture-table-unreadable-memory",
                        "palette source is not guest-readable");
                }
                else
                {
                    // Standard Glide palettes carry 8-bit RGB in the low 24
                    // bits. The high byte is ignored; AP_88 supplies alpha in
                    // the texel itself and P_8 remains opaque.
                    context->glide_state.palette_valid =
                        repiu::hle::DecodeGlidePaletteToRgba8(
                            data, 256U,
                            context->glide_state.palette_rgba8.data(),
                            context->glide_state.palette_rgba8.size());
                    if (context->glide_state.palette_valid &&
                        !context->glide_backend.RefreshPalettizedTextures(
                            context->glide_state.palette_rgba8.data()))
                    {
                        record_backend_failure(
                            "texture-palette-refresh-failed",
                            "stored palettized textures could not be refreshed");
                    }
                }
            }
            else
            {
                record_backend_failure(
                    "texture-table-unreadable-stack",
                    "arguments are not guest-readable");
            }
            ++context->glide_gate_handled_count;
            win32_context->Eip = return_address;
            win32_context->Esp += call.stack_advance;
            return true;
        }

        case go::kGrTexDownloadMipMapLevel: // _GRTEXDOWNLOADMIPMAPLEVEL@32
        {
            // R3: decode and upload the texel image so a later grTexSource can bind
            // it. Args (from the guest stack, the mirror holds only 8 dwords):
            // tmu, startAddress, thisLod, largeLod, aspectRatio, format, evenOdd,
            // data. Preserve the stdcall ABI regardless of upload success.
            std::uint32_t args[9] = {};
            const auto* guest_stack = reinterpret_cast<const std::uint32_t*>(
                static_cast<std::uintptr_t>(win32_context->Esp));
            if (IsGuestRangeReadable(context, guest_stack, sizeof(args)))
            {
                std::memcpy(args, guest_stack, sizeof(args));
                const std::uint32_t start_address = args[2];
                const std::uint32_t large_lod = args[4];
                const std::uint32_t aspect_ratio = args[5];
                const std::uint32_t format = args[6];
                const auto* data = reinterpret_cast<const std::uint8_t*>(
                    static_cast<std::uintptr_t>(args[8]));
                repiu::hle::GlideTextureDimensions dimensions;
                const bool dimensions_ok =
                    repiu::hle::CalculateGlideTextureDimensions(
                        large_lod, aspect_ratio, &dimensions);
                if (dimensions_ok)
                {
                    const std::size_t bytes_per_texel = format >= 8U ? 2U : 1U;
                    const std::size_t source_size =
                        static_cast<std::size_t>(dimensions.width) *
                        dimensions.height * bytes_per_texel;
                    if (source_size == 0U)
                    {
                        record_unsupported(
                            "texture-download-empty-source",
                            "calculated source size is zero");
                    }
                    else if (!IsGuestRangeReadable(
                                 context, data, source_size))
                    {
                        record_backend_failure(
                            "texture-download-unreadable-memory",
                            "texture source is not guest-readable");
                    }
                    else
                    {
                        const std::uint8_t* palette_ptr =
                            context->glide_state.palette_valid
                            ? context->glide_state.palette_rgba8.data()
                            : nullptr;
                        // Issue #37: the game's Mesa driver made this 4444 or
                        // 565 data from an 8-bit original it still holds; find
                        // and verify that original by reading guest memory.
                        GlideOpenGlBackend::FullPrecisionTexture full_precision;
                        const bool full_precision_candidate =
                            format == 10U || format == 12U;
                        if (full_precision_candidate)
                        {
                            const repiu::hle::MesaFxGuestReader reader =
                                [context](std::uint32_t address, void* out,
                                          std::size_t size) {
                                    const auto* guest =
                                        reinterpret_cast<const void*>(
                                            static_cast<std::uintptr_t>(
                                                address));
                                    if (size == 0U ||
                                        !IsGuestRangeReadable(context, guest,
                                                              size))
                                    {
                                        return false;
                                    }
                                    std::memcpy(out, guest, size);
                                    return true;
                                };
                            repiu::hle::MesaFxGateState gate;
                            gate.esi =
                                static_cast<std::uint32_t>(win32_context->Esi);
                            gate.ebp =
                                static_cast<std::uint32_t>(win32_context->Ebp);
                            gate.esp =
                                static_cast<std::uint32_t>(win32_context->Esp);
                            gate.data_address = args[8];
                            full_precision.outcome =
                                repiu::hle::BuildMesaFxFullPrecisionTexture(
                                    reader, gate, format, dimensions.width,
                                    dimensions.height, data, source_size,
                                    &full_precision.rgba8);
                        }
                        const bool stored = context->glide_backend.StoreTexture(
                            start_address, format, large_lod, aspect_ratio, data,
                            source_size, palette_ptr,
                            full_precision_candidate ? &full_precision
                                                     : nullptr);
                        context->glide_backend_message =
                            context->glide_backend.message();
                        if (!stored)
                        {
                            if (std::string_view(
                                    context->glide_backend_message)
                                    .starts_with("unsupported Glide"))
                            {
                                record_unsupported(
                                    "texture-download-unsupported",
                                    context->glide_backend_message);
                            }
                            else
                            {
                                record_backend_failure(
                                    "texture-download-backend-failure",
                                    context->glide_backend_message);
                            }
                        }
                    }
                }
                else
                {
                    record_unsupported(
                        "texture-dimensions-unsupported",
                        "LOD or aspect ratio is unsupported");
                }
            }
            else
            {
                record_backend_failure(
                    "texture-download-unreadable-stack",
                    "arguments are not guest-readable");
            }
            ++context->glide_gate_handled_count;
            win32_context->Eip = return_address;
            win32_context->Esp += 9U * sizeof(std::uint32_t);
            return true;
        }

        case go::kGrTexSource: // _GRTEXSOURCE@16
            // R3: select the current texture (args: tmu, startAddress, evenOdd,
            // GrTexInfo*). A missing texture simply leaves the previous binding.
            if (!context->glide_backend.SourceTexture(
                    context->glide_gate_stack[2]))
            {
                context->glide_backend_message =
                    context->glide_backend.message();
                if (std::string_view(context->glide_backend_message)
                        .starts_with("unsupported Glide"))
                {
                    record_unsupported(
                        "texture-source-unsupported",
                        context->glide_backend_message);
                }
                else
                {
                    record_backend_failure(
                        "texture-source-backend-failure",
                        context->glide_backend_message);
                }
            }
            ++context->glide_gate_handled_count;
            win32_context->Eip = return_address;
            win32_context->Esp += sizeof(std::uint32_t) +
                glide_export->argument_byte_count;
            return true;

        case go::kGrTexMinAddress: // _GRTEXMINADDRESS@4
            // stdcall: pop return address and the one dword argument. fxTMInit is
            // the sole caller and issues `push arg; call grTexMin; push arg; call
            // grTexMax; mov eax,[esp]` with no caller-side cleanup, so the gate
            // must clean the argument or the reload reads a leftover (NULL gc).
            if (context->glide_gate_stack[1] == 0U)
            {
                RecordGlideTextureGateTrace(context, win32_context, *glide_export, return_address, 0U, false);
                ++context->glide_gate_handled_count;
                win32_context->Eax = 0U;
                win32_context->Eip = return_address;
                win32_context->Esp += 2U * sizeof(std::uint32_t);
                return true;
            }
            break;

        case go::kGrTexMaxAddress: // _GRTEXMAXADDRESS@4
            // stdcall: pop return address and the one dword argument (see the
            // grTexMinAddress note above; fxTMInit relies on callee cleanup).
            if (context->glide_gate_stack[1] == 0U)
            {
                std::uint32_t maximum_address = 0;
                if (!repiu::hle::CalculateGlideTextureMaxAddress(
                        context->glide_state.texture_memory_bytes,
                        &maximum_address))
                {
                    return decline_gate("texture-max-address-calculation-failure");
                }
                RecordGlideTextureGateTrace(context, win32_context, *glide_export, return_address, maximum_address, true);
                ++context->glide_gate_handled_count;
                win32_context->Eax = maximum_address;
                win32_context->Eip = return_address;
                win32_context->Esp += 2U * sizeof(std::uint32_t);
                return true;
            }
            break;

        case go::kGrColorMask: // _GRCOLORMASK@8
        {
            const bool rgb = context->glide_gate_stack[1] != 0U;
            const bool alpha = context->glide_gate_stack[2] != 0U;
            if (!context->glide_backend.SetColorMask(rgb, alpha))
            {
                context->glide_backend_message =
                    context->glide_backend.message();
                return decline_gate("color-mask-backend-failure");
            }
            context->glide_backend_message = context->glide_backend.message();
            ++context->glide_gate_handled_count;
            win32_context->Eip = return_address;
            win32_context->Esp += 3U * sizeof(std::uint32_t);
            return true;
        }

        case go::kGrRenderBuffer: // _GRRENDERBUFFER@4
            // Task 729: where draws and clears land decides which shadow they
            // break. Recorded before the backend call so a declined gate does
            // not leave the shadow set trusting a stale target.
            SetGlideLfbStagingShadowRenderBuffer(
                &context->glide_lfb_staging_shadow,
                context->glide_gate_stack[1]);
            if (!context->glide_backend.SetRenderBuffer(
                    context->glide_gate_stack[1]))
            {
                context->glide_backend_message =
                    context->glide_backend.message();
                return decline_gate("render-buffer-backend-failure");
            }
            context->glide_backend_message = context->glide_backend.message();
            ++context->glide_gate_handled_count;
            win32_context->Eip = return_address;
            win32_context->Esp += 2U * sizeof(std::uint32_t);
            return true;

        case go::kGrDepthBiasLevel: // _GRDEPTHBIASLEVEL@4
            RecordGlideImplementationIssue(
                win32_context,
                context,
                *glide_export,
                repiu::hle::GlideImplementationIssueKind::
                    kUnimplementedFunction,
                "depth-bias-noop",
                "depth bias accepted without backend state",
                "continue");
            ++context->glide_gate_handled_count;
            win32_context->Eip = return_address;
            win32_context->Esp += 2U * sizeof(std::uint32_t);
            return true;

        case go::kGrDepthMask: // _GRDEPTHMASK@4
            if (!context->glide_backend.SetDepthMask(
                    context->glide_gate_stack[1] != 0U))
            {
                context->glide_backend_message =
                    context->glide_backend.message();
                return decline_gate("depth-mask-backend-failure");
            }
            context->glide_backend_message = context->glide_backend.message();
            ++context->glide_gate_handled_count;
            win32_context->Eip = return_address;
            win32_context->Esp += 2U * sizeof(std::uint32_t);
            return true;

        case go::kGrDepthBufferMode: // _GRDEPTHBUFFERMODE@4
            if (!context->glide_backend.SetDepthBufferMode(
                    context->glide_gate_stack[1]))
            {
                context->glide_backend_message =
                    context->glide_backend.message();
                return decline_gate("depth-buffer-mode-backend-failure");
            }
            context->glide_backend_message = context->glide_backend.message();
            ++context->glide_gate_handled_count;
            win32_context->Eip = return_address;
            win32_context->Esp += 2U * sizeof(std::uint32_t);
            return true;

        case go::kGrLfbWriteColorFormat: // _GRLFBWRITECOLORFORMAT@4
            context->glide_state.lfb_write_color_format =
                context->glide_gate_stack[1];
            ++context->glide_gate_handled_count;
            win32_context->Eip = return_address;
            win32_context->Esp += 2U * sizeof(std::uint32_t);
            return true;

        case go::kGrAlphaCombine: // _GRALPHACOMBINE@20
        {
            // Same retain policy as color combine (design 237, Task 247):
            // an equation the GLSL translator merely does not support yet
            // must still preserve the stdcall ABI, or the unhandled gate
            // leaks its 24-byte frame and the caller epilogue returns to 0
            // (the Task 245/246 zero-EIP root cause).
            repiu::hle::GlideAlphaCombineState state;
            state.function = context->glide_gate_stack[1];
            state.factor = context->glide_gate_stack[2];
            state.local = context->glide_gate_stack[3];
            state.other = context->glide_gate_stack[4];
            state.invert = context->glide_gate_stack[5] != 0U;
            state.valid = true;
            if (!context->glide_backend.SetAlphaCombine(state))
            {
                context->glide_backend_message =
                    context->glide_backend.message();
                if (context->glide_backend_message !=
                    "unsupported Glide alpha-combine equation")
                {
                    return decline_gate("alpha-combine-backend-failure");
                }
                RecordGlideImplementationIssue(
                    win32_context,
                    context,
                    *glide_export,
                    repiu::hle::GlideImplementationIssueKind::
                        kUnsupportedArgument,
                    "alpha-combine-unsupported",
                    context->glide_backend_message,
                    "continue");
            }
            context->glide_state.alpha_combine = state;
            context->glide_backend_message = context->glide_backend.message();
            ++context->glide_gate_handled_count;
            win32_context->Eip = return_address;
            win32_context->Esp += 6U * sizeof(std::uint32_t);
            return true;
        }

        case go::kGrColorCombine: // _GRCOLORCOMBINE@20
        {
            repiu::hle::GlideColorCombineState state;
            state.function = context->glide_gate_stack[1];
            state.factor = context->glide_gate_stack[2];
            state.local = context->glide_gate_stack[3];
            state.other = context->glide_gate_stack[4];
            state.invert = context->glide_gate_stack[5] != 0U;
            state.valid = true;
            constexpr std::uint32_t kCombineFunctionScaleOther = 3U;
            constexpr std::uint32_t kCombineOtherTexture = 1U;
            // R3: function 3 (GR_COMBINE_FUNCTION_SCALE_OTHER) with other=1
            // (GR_COMBINE_OTHER_TEXTURE) routes the fragment output to the sourced
            // texture; function 1 (LOCAL) is the iterated vertex color. Toggle the
            // backend texture-combine path accordingly (observed content draws use
            // function 3, init uses function 1).
            context->glide_backend.SetTextureCombineEnabled(
                state.function == kCombineFunctionScaleOther &&
                state.other == kCombineOtherTexture);
            if (!context->glide_backend.SetColorCombine(state))
            {
                context->glide_backend_message =
                    context->glide_backend.message();
                if (context->glide_backend_message !=
                    "unsupported Glide color-combine equation")
                {
                    return decline_gate("color-combine-backend-failure");
                }
                RecordGlideImplementationIssue(
                    win32_context,
                    context,
                    *glide_export,
                    repiu::hle::GlideImplementationIssueKind::
                        kUnsupportedArgument,
                    "color-combine-unsupported",
                    context->glide_backend_message,
                    "continue");
            }
            context->glide_state.color_combine = state;
            context->glide_backend_message = context->glide_backend.message();
            ++context->glide_gate_handled_count;
            win32_context->Eip = return_address;
            win32_context->Esp += 6U * sizeof(std::uint32_t);
            return true;
        }

        case go::kGrAlphaBlendFunction: // _GRALPHABLENDFUNCTION@16
        {
            // Same retain policy as the combine gates (design 237): a blend
            // function the backend does not express yet must still preserve
            // the stdcall ABI, or the unhandled gate leaks its frame (the
            // Task 246 corruption chain).
            repiu::hle::GlideAlphaBlendState state;
            state.rgb_source = context->glide_gate_stack[1];
            state.rgb_destination = context->glide_gate_stack[2];
            state.alpha_source = context->glide_gate_stack[3];
            state.alpha_destination = context->glide_gate_stack[4];
            state.valid = true;
            if (!context->glide_backend.SetAlphaBlend(state))
            {
                context->glide_backend_message =
                    context->glide_backend.message();
                if (context->glide_backend_message !=
                    "unsupported Glide alpha-blend function")
                {
                    return decline_gate("alpha-blend-backend-failure");
                }
                RecordGlideImplementationIssue(
                    win32_context,
                    context,
                    *glide_export,
                    repiu::hle::GlideImplementationIssueKind::
                        kUnsupportedArgument,
                    "alpha-blend-unsupported",
                    context->glide_backend_message,
                    "continue");
            }
            context->glide_state.alpha_blend = state;
            context->glide_backend_message = context->glide_backend.message();
            ++context->glide_gate_handled_count;
            win32_context->Eip = return_address;
            win32_context->Esp += 5U * sizeof(std::uint32_t);
            return true;
        }

        case go::kGrAlphaTestFunction: // _GRALPHATESTFUNCTION@4
        {
            const std::uint32_t function = context->glide_gate_stack[1];
            if (!context->glide_backend.SetAlphaTestFunction(function))
            {
                context->glide_backend_message =
                    context->glide_backend.message();
                return decline_gate("alpha-test-function-backend-failure");
            }
            context->glide_state.alpha_test_function = function;
            context->glide_backend_message = context->glide_backend.message();
            ++context->glide_gate_handled_count;
            win32_context->Eip = return_address;
            win32_context->Esp += 2U * sizeof(std::uint32_t);
            return true;
        }

        case go::kGrAlphaTestReferenceValue: // _GRALPHATESTREFERENCEVALUE@4
        {
            const std::uint32_t reference_value = context->glide_gate_stack[1];
            if (!context->glide_backend.SetAlphaTestReferenceValue(reference_value))
            {
                context->glide_backend_message =
                    context->glide_backend.message();
                return decline_gate("alpha-test-reference-value-backend-failure");
            }
            context->glide_state.alpha_test_reference = reference_value;
            context->glide_backend_message = context->glide_backend.message();
            ++context->glide_gate_handled_count;
            win32_context->Eip = return_address;
            win32_context->Esp += 2U * sizeof(std::uint32_t);
            return true;
        }

        case go::kGrDepthBufferFunction: // _GRDEPTHBUFFERFUNCTION@4
        {
            const std::uint32_t function = context->glide_gate_stack[1];
            if (!context->glide_backend.SetDepthBufferFunction(function))
            {
                context->glide_backend_message =
                    context->glide_backend.message();
                return decline_gate("depth-buffer-function-backend-failure");
            }
            context->glide_state.depth_buffer_function = function;
            context->glide_backend_message = context->glide_backend.message();
            ++context->glide_gate_handled_count;
            win32_context->Eip = return_address;
            win32_context->Esp += 2U * sizeof(std::uint32_t);
            return true;
        }

        case go::kGrClipWindow: // _GRCLIPWINDOW@16
        {
            const std::uint32_t min_x = context->glide_gate_stack[1];
            const std::uint32_t min_y = context->glide_gate_stack[2];
            const std::uint32_t max_x = context->glide_gate_stack[3];
            const std::uint32_t max_y = context->glide_gate_stack[4];
            if (!context->glide_backend.SetClipWindow(
                    min_x, min_y, max_x, max_y))
            {
                context->glide_backend_message =
                    context->glide_backend.message();
                return decline_gate("clip-window-backend-failure");
            }
            context->glide_state.clip_min_x = min_x;
            context->glide_state.clip_min_y = min_y;
            context->glide_state.clip_max_x = max_x;
            context->glide_state.clip_max_y = max_y;
            context->glide_backend_message = context->glide_backend.message();
            ++context->glide_gate_handled_count;
            win32_context->Eip = return_address;
            win32_context->Esp += 5U * sizeof(std::uint32_t);
            return true;
        }

        case go::kGrCullMode: // _GRCULLMODE@4
        {
            const std::uint32_t mode = context->glide_gate_stack[1];
            if (!context->glide_backend.SetCullMode(mode))
            {
                context->glide_backend_message =
                    context->glide_backend.message();
                return decline_gate("cull-mode-backend-failure");
            }
            context->glide_state.cull_mode = mode;
            context->glide_backend_message = context->glide_backend.message();
            ++context->glide_gate_handled_count;
            win32_context->Eip = return_address;
            win32_context->Esp += 2U * sizeof(std::uint32_t);
            return true;
        }

        case go::kGrGlideGetState: // _GRGLIDEGETSTATE@4
        {
            repiu::hle::GlideStateImage image;
            void* output = reinterpret_cast<void*>(static_cast<std::uintptr_t>(
                context->glide_gate_stack[1]));
            if (!repiu::hle::BuildGlideStateImage(context->glide_state, &image) ||
                !WriteGuestBytes(context, output, image.data(), image.size()))
            {
                return decline_gate("get-state-serialization-failure");
            }
            ++context->glide_gate_handled_count;
            win32_context->Eip = return_address;
            win32_context->Esp += 2U * sizeof(std::uint32_t);
            return true;
        }

        case go::kGrGlideSetState: // _GRGLIDESETSTATE@4
        {
            repiu::hle::GlideStateImage image;
            const void* input = reinterpret_cast<const void*>(
                static_cast<std::uintptr_t>(context->glide_gate_stack[1]));
            repiu::hle::GlideLogicalState restored = context->glide_state;
            if (!IsGuestRangeReadable(context, input, image.size()))
            {
                return decline_gate("set-state-unreadable-memory");
            }
            std::memcpy(image.data(), input, image.size());
            if (!repiu::hle::ParseGlideStateImage(image, &restored))
            {
                return decline_gate("set-state-deserialization-failure");
            }
            context->glide_state = restored;
            ++context->glide_gate_handled_count;
            win32_context->Eip = return_address;
            win32_context->Esp += 2U * sizeof(std::uint32_t);
            return true;
        }

        case go::kGrDitherMode: // _GRDITHERMODE@4
        {
            const std::uint32_t mode = context->glide_gate_stack[1];
            if (!context->glide_backend.SetDitherMode(mode))
            {
                context->glide_backend_message =
                    context->glide_backend.message();
                return decline_gate("dither-mode-backend-failure");
            }
            context->glide_state.dither_mode = mode;
            context->glide_backend_message = context->glide_backend.message();
            ++context->glide_gate_handled_count;
            win32_context->Eip = return_address;
            win32_context->Esp += 2U * sizeof(std::uint32_t);
            return true;
        }

        case go::kGrBufferClear: // _GRBUFFERCLEAR@12
        {
            // Task 332: same GrColor_t conversion as grConstantColorValue.
            const std::uint32_t color = repiu::hle::ConvertGlideColorToArgb(
                context->glide_gate_stack[1],
                context->glide_state.color_format);
            const std::uint32_t alpha = context->glide_gate_stack[2];
            const std::uint32_t depth = context->glide_gate_stack[3];
            if (GlideAsyncPresentEnabled())
            {
                // Posted so the wait the swap no longer takes does not simply
                // reappear here, one gate later.
                context->glide_backend.PostBufferClear(color, alpha, depth);
            }
            else if (!context->glide_backend.BufferClear(color, alpha, depth))
            {
                context->glide_backend_message = context->glide_backend.message();
                return decline_gate("buffer-clear-backend-failure");
            }
            context->glide_backend_message = context->glide_backend.message();
            ++context->glide_gate_handled_count;
            win32_context->Eip = return_address;
            win32_context->Esp += 4U * sizeof(std::uint32_t);
            return true;
        }

        case go::kGrBufferSwap: // _GRBUFFERSWAP@4
        {
            // Task 750: back from the ISR of a tick injected during the wait.
            if (context->glide_swap_wait_active)
            {
                // The same call, entered again: the counts stay the game's.
                --context->glide_gate_entry_count;
                if (glide_export->ordinal < context->glide_call_counts.size() &&
                    context->glide_call_counts[glide_export->ordinal] != 0U)
                {
                    --context->glide_call_counts[glide_export->ordinal];
                }
                return ContinueGlideSwapWait(win32_context, context,
                                             return_address);
            }
            // Issue #58: the ticks each drawn frame carried, which is how
            // far the PIU10 arrows move from one frame to the next.
            if (context->piu10_isa_board_enabled)
            {
                context->piu10_song_clock.ObserveSwap(
                    static_cast<std::uint64_t>(
                        std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now()
                                .time_since_epoch())
                            .count()),
                    context->timer_tick_delivery.injected_total.load(
                        std::memory_order_relaxed),
                    repiu::hle::PitFrequencyHz(
                        context->pit_channel0.snapshot().divisor));
            }
            // Task 511: the one place the attribution can be read on a host
            // where the guest thread never stops.
            //
            // Here rather than at gate entry because a clock read on every
            // entry would make the instrument part of what it measures, which
            // is Task 353's rule. Here rather than at teardown because this is
            // the guest thread -- the thread that writes every counter the
            // report reads -- so nothing can tear and nothing needs a lock.
            // Task 516: the cache counters travel as a plain struct rather than
            // the context itself, so the reporter never depends on the execution
            // engine's types. Relaxed loads: the writer is this same thread, and
            // the ordering against anything else is not what is being asked.
            LiveAotCounters live_aot;
            const auto load_counter =
                [](const std::atomic<std::uint32_t>& counter) {
                    return counter.load(std::memory_order_relaxed);
                };
            live_aot.cache_entry = load_counter(context->aot_cache_entry_count);
            live_aot.boundary = load_counter(context->aot_boundary_count);
            live_aot.boundary_return =
                load_counter(context->aot_boundary_return_count);
            live_aot.boundary_indirect =
                load_counter(context->aot_boundary_indirect_count);
            live_aot.boundary_direct =
                load_counter(context->aot_boundary_direct_count);
            live_aot.boundary_conditional =
                load_counter(context->aot_boundary_conditional_count);
            live_aot.boundary_other =
                load_counter(context->aot_boundary_other_count);
            live_aot.reentry = load_counter(context->aot_reentry_count);
            live_aot.legacy_fallback =
                load_counter(context->aot_legacy_fallback_count);
            live_aot.residency_instructions =
                load_counter(context->aot_residency_instruction_total);
            live_aot.residency_samples =
                load_counter(context->aot_residency_sample_count);
            // Task 517: process-global rather than per-context, so it is read
            // through its own accessor rather than off the context above.
            const GlideGateDirectDispatchStats direct_dispatch =
                ReadGlideGateDirectDispatchStats();
            live_aot.glide_patched_sites = direct_dispatch.patched_gate_count;
            live_aot.glide_verified_sites = direct_dispatch.verified_gate_count;
            live_aot.glide_entry = direct_dispatch.entry_count;
            live_aot.glide_success = direct_dispatch.success_count;
            live_aot.glide_target_miss = direct_dispatch.target_miss_count;
            live_aot.glide_terminal_failure =
                direct_dispatch.terminal_failure_count;
            live_aot.glide_resolved_target =
                direct_dispatch.resolved_target_count;
            live_aot.glide_relinked_cache_target =
                direct_dispatch.relinked_cache_target_count;
            live_aot.glide_relink_content =
                direct_dispatch.relink_content_patch_count;
            live_aot.glide_relink_fixup =
                direct_dispatch.relink_fixup_patch_count;
            live_aot.direct_trap_at_target =
                load_counter(context->direct_dispatch_trap_at_target);
            live_aot.direct_trap_elsewhere =
                load_counter(context->direct_dispatch_trap_elsewhere);
            live_aot.direct_other_elsewhere =
                load_counter(context->direct_dispatch_other_elsewhere);
            live_aot.direct_step_after =
                load_counter(context->direct_dispatch_step_after);
            live_aot.direct_clean =
                load_counter(context->direct_dispatch_clean);
            live_aot.retired_entry_trap =
                load_counter(context->aot_retired_entry_trap_count);
            live_aot.quarantine = load_counter(context->aot_quarantine_count);
            live_aot.dynamic_attempt =
                load_counter(context->aot_dynamic_attempt_count);
            {
                const AotBoundaryOpcodeCensus& census =
                    context->aot_boundary_opcode_census;
                live_aot.opcode_samples = census.sample_count;
                AotOpcodeRank ranks[4] = {};
                RankAotOpcodeHistogram(
                    census.effective_opcode_counts, ranks, 4U);
                for (std::size_t slot = 0; slot < 4U; ++slot)
                {
                    live_aot.top_opcode[slot] = ranks[slot].opcode;
                    live_aot.top_opcode_count[slot] = ranks[slot].count;
                }
                AotOpcodeRank escapes[4] = {};
                RankAotOpcodeHistogram(
                    census.escape_opcode_counts, escapes, 4U);
                for (std::size_t slot = 0; slot < 4U; ++slot)
                {
                    live_aot.top_escape[slot] = escapes[slot].opcode;
                    live_aot.top_escape_count[slot] = escapes[slot].count;
                }
                for (std::size_t group = 0; group < 8U; ++group)
                {
                    live_aot.ff_group_counts[group] =
                        census.ff_group_counts[group];
                }
                live_aot.ff_modrm_truncated_count =
                    census.ff_modrm_truncated_count;
                const AotFfBoundaryAttribution& ff4 =
                    context->aot_ff_boundary_attribution;
                live_aot.ff4_sample_count = ff4.sample_count;
                live_aot.ff4_modrm_truncated_count =
                    ff4.modrm_truncated_count;
                live_aot.ff4_site_overflow_count = ff4.site_overflow_count;
                live_aot.ff4_target_resolved_count =
                    ff4.target_resolved_count;
                live_aot.ff4_target_unresolved_count =
                    ff4.target_unresolved_count;
                live_aot.ff4_target_instruction_truncated_count =
                    ff4.target_instruction_truncated_count;
                live_aot.ff4_target_unsupported_count =
                    ff4.target_unsupported_count;
                live_aot.ff4_target_memory_unreadable_count =
                    ff4.target_memory_unreadable_count;
                for (std::size_t mode = 0U;
                     mode < kAotFfAddressingModeCount; ++mode)
                {
                    live_aot.ff4_addressing_mode_counts[mode] =
                        ff4.addressing_mode_counts[mode];
                }
                RankAotFfBoundarySites(
                    ff4,
                    live_aot.ff4_site_hotspots,
                    kAotFfBoundarySiteHotspotCapacity);
                live_aot.ff4_target_timing = ff4.target_timing;
            }
            LiveDosCounters live_dos;
            live_dos.handled_interrupt_count =
                context->handled_dos_interrupt_count;
            live_dos.int21_count = context->handled_dos_int21_count;
            AotOpcodeRank dos_ah_ranks[4] = {};
            RankAotOpcodeHistogram(
                context->handled_dos_int21_ah_counts,
                dos_ah_ranks,
                4U);
            for (std::size_t slot = 0; slot < 4U; ++slot)
            {
                live_dos.top_int21_ah[slot] = dos_ah_ranks[slot].opcode;
                live_dos.top_int21_ah_count[slot] = dos_ah_ranks[slot].count;
            }
            for (std::size_t slot = 0; slot < 8U; ++slot)
            {
                live_aot.direct_trap_site_address[slot] =
                    context->direct_trap_site_address[slot];
                live_aot.direct_trap_site_count[slot] =
                    context->direct_trap_site_count[slot];
            }
            ReportLiveExecutionProfileIfDue(
                context->execution_time_profile.get(),
                context->glide_backend.presented_frame_total(),
                live_aot,
                live_dos);
            const std::uint32_t swap_interval = context->glide_gate_stack[1];
            if (GlideAsyncPresentEnabled())
            {
                // The whole point of the task: the guest returns here instead of
                // waiting out the vblank, which with vsync on is 10.8 ms and
                // 32.8% of guest-run. A failure afterwards is counted rather
                // than declining a gate that has already returned.
                context->glide_backend.PostBufferSwap(swap_interval);
            }
            else if (GlideSwapWaitTicksEnabled() &&
                     context->glide_backend.PostBufferSwap(swap_interval))
            {
                context->glide_swap_wait_active = true;
                context->glide_swap_wait_begin =
                    std::chrono::steady_clock::now();
                context->glide_backend.NoteSwapWaitBegin();
                return ContinueGlideSwapWait(win32_context, context,
                                             return_address);
            }
            else if (!context->glide_backend.BufferSwap(swap_interval))
            {
                context->glide_backend_message = context->glide_backend.message();
                return decline_gate("buffer-swap-backend-failure");
            }
            context->glide_backend_message = context->glide_backend.message();
            ++context->glide_gate_handled_count;
            win32_context->Eip = return_address;
            win32_context->Esp += 2U * sizeof(std::uint32_t);
            return true;
        }

        case go::kGrBufferNumPending: // _GRBUFFERNUMPENDING@0
            // Task 440: the game calls this exactly once per swap -- it is
            // Glide's throttle, and answering a constant zero is what made it
            // inert. With the present posted, the real outstanding count is what
            // lets the game pace itself the way the hardware let it.
            ++context->glide_gate_handled_count;
            win32_context->Eax =
                context->glide_backend.glide_pending_swap_count();
            win32_context->Eip = return_address;
            win32_context->Esp += sizeof(std::uint32_t);
            return true;

        // Task 420: the AA variants carry the same geometry and differ only by
        // antialiasing flags this backend does not implement, so they render the
        // same vertices rather than nothing. `_GRAADRAWLINE@8` takes the same two
        // pointers as `_GRDRAWLINE@8`, so the stack adjustment is shared too.
        case go::kGrAADrawLine: // _GRAADRAWLINE@8
        case go::kGrDrawLine: // _GRDRAWLINE@8
        {
            hle::GlideDrawVertex vertices[2] = {};
            for (std::size_t index = 0U; index < 2U; ++index)
            {
                const auto* source = reinterpret_cast<const std::uint32_t*>(
                    static_cast<std::uintptr_t>(
                        context->glide_gate_stack[index + 1U]));
                if (!IsGuestRangeReadable(context, source,
                                          hle::kGlideProducerVertexByteCount))
                {
                    return decline_gate("draw-line-unreadable-vertex");
                }
                std::uint32_t producer[
                    hle::kGlideProducerVertexDwordCount] = {};
                std::memcpy(producer, source, sizeof(producer));
                if (!hle::DecodeGlideProducerVertex(
                        producer, hle::kGlideProducerVertexDwordCount,
                        &vertices[index]))
                {
                    return decline_gate("draw-line-decode-failure");
                }
            }
            if (!QueueGlideDrawForBatch(context, draw_batch, vertices, 2U,
                                        GlideBatchPrimitive::kLines) &&
                !context->glide_backend.DrawLine(vertices[0], vertices[1]))
            {
                context->glide_backend_message =
                    context->glide_backend.message();
                return decline_gate("draw-line-backend-failure");
            }
            context->glide_backend_message = context->glide_backend.message();
            ++context->glide_gate_handled_count;
            win32_context->Eip = return_address;
            win32_context->Esp += 3U * sizeof(std::uint32_t);
            return true;
        }

        // Task 420: three vertex pointers followed by three antialiasing flags
        // this backend does not implement, so the flags are ignored and the
        // geometry is drawn. Seven stack slots, unlike `_GRDRAWTRIANGLE@12`.
        case go::kGrAADrawTriangle: // _GRAADRAWTRIANGLE@24
        {
            hle::GlideDrawVertex vertices[3] = {};
            for (std::size_t index = 0U; index < 3U; ++index)
            {
                const auto* source = reinterpret_cast<const std::uint32_t*>(
                    static_cast<std::uintptr_t>(
                        context->glide_gate_stack[index + 1U]));
                if (!IsGuestRangeReadable(context, source,
                                          hle::kGlideProducerVertexByteCount))
                {
                    return decline_gate("aa-draw-triangle-unreadable-vertex");
                }
                std::uint32_t producer[
                    hle::kGlideProducerVertexDwordCount] = {};
                std::memcpy(producer, source, sizeof(producer));
                if (!hle::DecodeGlideProducerVertex(
                        producer, hle::kGlideProducerVertexDwordCount,
                        &vertices[index]))
                {
                    return decline_gate("aa-draw-triangle-decode-failure");
                }
            }
            if (!QueueGlideDrawForBatch(context, draw_batch, vertices, 3U,
                                        GlideBatchPrimitive::kTriangles) &&
                !context->glide_backend.DrawTriangle(vertices[0], vertices[1],
                                                     vertices[2]))
            {
                context->glide_backend_message =
                    context->glide_backend.message();
                return decline_gate("aa-draw-triangle-backend-failure");
            }
            context->glide_backend_message = context->glide_backend.message();
            ++context->glide_gate_handled_count;
            win32_context->Eip = return_address;
            win32_context->Esp += 7U * sizeof(std::uint32_t);
            return true;
        }

        // Task 420: this used to accept the request and discard it, so anything
        // a title drew as a point was missing from the screen with nothing to
        // show for it. pumpit2 calls it (Task 420 design section 1).
        case go::kGrAADrawPoint: // _GRAADRAWPOINT@4
        case go::kGrDrawPoint: // _GRDRAWPOINT@4
        {
            const auto* source = reinterpret_cast<const std::uint32_t*>(
                static_cast<std::uintptr_t>(context->glide_gate_stack[1]));
            if (!IsGuestRangeReadable(context, source,
                                      hle::kGlideProducerVertexByteCount))
            {
                return decline_gate("draw-point-unreadable-vertex");
            }
            std::uint32_t producer[hle::kGlideProducerVertexDwordCount] = {};
            std::memcpy(producer, source, sizeof(producer));
            hle::GlideDrawVertex vertex = {};
            if (!hle::DecodeGlideProducerVertex(
                    producer, hle::kGlideProducerVertexDwordCount, &vertex))
            {
                return decline_gate("draw-point-decode-failure");
            }
            if (!QueueGlideDrawForBatch(context, draw_batch, &vertex, 1U,
                                        GlideBatchPrimitive::kPoints) &&
                !context->glide_backend.DrawPoint(vertex))
            {
                context->glide_backend_message =
                    context->glide_backend.message();
                return decline_gate("draw-point-backend-failure");
            }
            context->glide_backend_message = context->glide_backend.message();
            ++context->glide_gate_handled_count;
            win32_context->Eip = return_address;
            win32_context->Esp += 2U * sizeof(std::uint32_t);
            return true;
        }

        case go::kGuFogGenerateExp: // _GUFOGGENERATEEXP@8
        {
            const std::uint32_t table_ptr = context->glide_gate_stack[1];
            float density = 0.0f;
            std::memcpy(&density, &context->glide_gate_stack[2], sizeof(float));

            if (table_ptr != 0U && IsGuestRangeWritable(context, reinterpret_cast<void*>(static_cast<std::uintptr_t>(table_ptr)), 64U))
            {
                std::uint8_t* table = reinterpret_cast<std::uint8_t*>(static_cast<std::uintptr_t>(table_ptr));
                for (std::size_t i = 0; i < 64U; ++i)
                {
                    float factor = 1.0f - std::exp(-density * (static_cast<float>(i) * 4.0f / 255.0f));
                    if (factor < 0.0f) factor = 0.0f;
                    if (factor > 1.0f) factor = 1.0f;
                    table[i] = static_cast<std::uint8_t>(factor * 255.0f);
                }
            }
            ++context->glide_gate_handled_count;
            win32_context->Eip = return_address;
            win32_context->Esp += 3U * sizeof(std::uint32_t);
            return true;
        }

        case go::kGrFogMode: // _GRFOGMODE@4
        {
            const std::uint32_t mode = context->glide_gate_stack[1];
            if (!context->glide_backend.SetFogMode(mode))
            {
                context->glide_backend_message =
                    context->glide_backend.message();
                return decline_gate("fog-mode-backend-failure");
            }
            context->glide_state.fog_mode = mode;
            context->glide_backend_message = context->glide_backend.message();
            ++context->glide_gate_handled_count;
            win32_context->Eip = return_address;
            win32_context->Esp += 2U * sizeof(std::uint32_t);
            return true;
        }

        case go::kGrFogColorValue: // _GRFOGCOLORVALUE@4
            context->glide_state.fog_color =
                repiu::hle::ConvertGlideColorToArgb(
                    context->glide_gate_stack[1],
                    context->glide_state.color_format);
            if (!context->glide_backend.SetFogColor(
                    context->glide_state.fog_color))
            {
                context->glide_backend_message =
                    context->glide_backend.message();
                return decline_gate("fog-color-backend-failure");
            }
            context->glide_backend_message = context->glide_backend.message();
            ++context->glide_gate_handled_count;
            win32_context->Eip = return_address;
            win32_context->Esp += 2U * sizeof(std::uint32_t);
            return true;

        case go::kGrFogTable: // _GRFOGTABLE@4
        {
            context->glide_state.fog_table_pointer =
                context->glide_gate_stack[1];
            const auto* table = reinterpret_cast<const void*>(
                static_cast<std::uintptr_t>(
                    context->glide_state.fog_table_pointer));
            if (!IsGuestRangeReadable(
                    context, table,
                    repiu::hle::kGlideFogTableEntryCount))
            {
                return decline_gate("fog-table-unreadable-memory");
            }
            std::memcpy(context->glide_state.fog_table.data(), table,
                        context->glide_state.fog_table.size());
            context->glide_state.fog_table_valid = true;
            if (!context->glide_backend.SetFogTable(
                    context->glide_state.fog_table))
            {
                context->glide_backend_message =
                    context->glide_backend.message();
                return decline_gate("fog-table-backend-failure");
            }
            context->glide_backend_message = context->glide_backend.message();
            ++context->glide_gate_handled_count;
            win32_context->Eip = return_address;
            win32_context->Esp += 2U * sizeof(std::uint32_t);
            return true;
        }

        case go::kGrConstantColorValue: // _GRCONSTANTCOLORVALUE@4
            // R4: retain the constant color so a later CONSTANT combine source can
            // read it. Observed value during the content phase is 0xFFFFFFFF.
            //
            // Task 332: a GrColor_t is laid out per the GrColorFormat_t chosen at
            // grSstWinOpen, and PIU selects ABGR, so the value has to be
            // converted. Most of the game's constants are greys, which are
            // symmetric and hid this; the difficulty dots' 0xFE6565FE is red in
            // ABGR and came out blue when read as ARGB.
            context->glide_state.constant_color =
                repiu::hle::ConvertGlideColorToArgb(
                    context->glide_gate_stack[1],
                    context->glide_state.color_format);
            if (!context->glide_backend.SetConstantColor(context->glide_state.constant_color))
            {
                context->glide_backend_message =
                    context->glide_backend.message();
                return decline_gate("set-state-backend-failure");
            }
            ++context->glide_gate_handled_count;
            win32_context->Eip = return_address;
            win32_context->Esp += 2U * sizeof(std::uint32_t);
            return true;

        case go::kGrTexClampMode: // _GRTEXCLAMPMODE@12
        {
            // Texture sampler parameters stay within the rendering boundary; the
            // observed filter/clamp/mipmap modes are handled by the backend texture
            // defaults for now.
            const std::uint32_t s_clamp = context->glide_gate_stack[2];
            const std::uint32_t t_clamp = context->glide_gate_stack[3];
            context->glide_backend.SetTextureClampMode(s_clamp, t_clamp);
            ++context->glide_gate_handled_count;
            win32_context->Eip = return_address;
            win32_context->Esp += 4U * sizeof(std::uint32_t);
            return true;
        }

        case go::kGrTexFilterMode: // _GRTEXFILTERMODE@12
        {
            const std::uint32_t min_filter = context->glide_gate_stack[2];
            const std::uint32_t mag_filter = context->glide_gate_stack[3];
            context->glide_backend.SetTextureFilterMode(min_filter, mag_filter);
            ++context->glide_gate_handled_count;
            win32_context->Eip = return_address;
            win32_context->Esp += 4U * sizeof(std::uint32_t);
            return true;
        }

        case go::kGrTexMipMapMode: // _GRTEXMIPMAPMODE@12
            ++context->glide_gate_handled_count;
            win32_context->Eip = return_address;
            win32_context->Esp += 4U * sizeof(std::uint32_t);
            return true;

        case go::kGrTexCombine: // _GRTEXCOMBINE@28
            ++context->glide_gate_handled_count;
            win32_context->Eip = return_address;
            win32_context->Esp += 8U * sizeof(std::uint32_t);
            return true;
        case go::kGrDrawTriangle: // _GRDRAWTRIANGLE@12
        {
            // Decode the confirmed 60-byte 2-TMU GrVertex fields: x/y (dwords 0/1),
            // iterated color r/g/b/a in [0..255] (dwords 3/4/5/7), vertex oow
            // (dword 8), and TMU0 sow/tow (dwords 9/10). Dwords 11..14 are
            // variable in the captured 60-byte producer layout, so the
            // observed non-projected texture path shares dword 8's oow for
            // both perspective correction and table fog.
            GlideTriangleObservation& triangle =
                context->glide_first_triangle;
            if (!triangle.valid)
            {
                triangle.valid = true;
                for (std::size_t index = 0; index < 3U; ++index)
                {
                    triangle.pointers[index] = context->glide_gate_stack[index + 1U];
                    const auto* vertex = reinterpret_cast<const void*>(
                        static_cast<std::uintptr_t>(triangle.pointers[index]));
                    triangle.pointer_readable[index] = IsGuestRangeReadable(
                        context, vertex, sizeof(triangle.dwords[index]));
                    if (triangle.pointer_readable[index])
                    {
                        std::memcpy(triangle.dwords[index], vertex,
                                    sizeof(triangle.dwords[index]));
                    }
                }
                fprintf(stderr,
                        "[repiu-live] Glide first triangle vertices: %08X/%d %08X/%d %08X/%d\n",
                        triangle.pointers[0], triangle.pointer_readable[0],
                        triangle.pointers[1], triangle.pointer_readable[1],
                        triangle.pointers[2], triangle.pointer_readable[2]);
                for (std::size_t index = 0; index < 3U; ++index)
                {
                    fprintf(stderr, "[repiu-live] Glide first triangle vertex %u dwords:",
                            static_cast<unsigned>(index));
                    for (std::uint32_t dword : triangle.dwords[index])
                    {
                        fprintf(stderr, " %08X", dword);
                    }
                    fprintf(stderr, "\n");
                }
            }
            const std::uint32_t sequence = ++context->glide_triangle_trace_count;
            GlideTriangleTraceEntry& trace = context->glide_triangle_trace[
                (sequence - 1U) % kGlideTriangleTraceCapacity];
            trace = GlideTriangleTraceEntry{};
            trace.valid = true;
            trace.sequence = sequence;
            for (std::size_t index = 0; index < 3U; ++index)
            {
                trace.pointers[index] = context->glide_gate_stack[index + 1U];
                const auto* vertex = reinterpret_cast<const void*>(
                    static_cast<std::uintptr_t>(trace.pointers[index]));
                trace.pointer_readable[index] = IsGuestRangeReadable(
                    context, vertex, sizeof(trace.dwords[index]));
                if (trace.pointer_readable[index])
                {
                    std::memcpy(trace.dwords[index], vertex,
                                sizeof(trace.dwords[index]));
                }
            }
            if (sequence > kGlideTriangleTraceCapacity)
            {
                context->glide_triangle_trace_wrapped = true;
            }
            hle::GlideDrawVertex vertices[3] = {};
            for (std::size_t index = 0; index < 3U; ++index)
            {
                if (!trace.pointer_readable[index])
                {
                    return decline_gate("compact-triangle-unreadable-vertex");
                }
                if (!hle::DecodeGlideProducerVertex(
                        trace.dwords[index],
                        kWin32GlideProducerVertexDwordCount,
                        &vertices[index]))
                {
                    return decline_gate("compact-triangle-decode-failure");
                }
            }
            static const bool draw_diagnostic_enabled =
                std::getenv("REPIU_GLIDE_DRAW_DIAG") != nullptr;
            const float min_x = std::min({vertices[0].x, vertices[1].x, vertices[2].x});
            const float max_x = std::max({vertices[0].x, vertices[1].x, vertices[2].x});
            const float min_y = std::min({vertices[0].y, vertices[1].y, vertices[2].y});
            const float max_y = std::max({vertices[0].y, vertices[1].y, vertices[2].y});
            const bool is_large = (max_x - min_x) >= 100.0F && (max_y - min_y) >= 100.0F;
            static long large_diag_count = 0;
            // Draw diagnostic (env-gated): the swap-driven pixel sampler only fires
            // on selected swap indices, so it can miss the content phase entirely.
            // Log the decoded vertices and read the back buffer straight after the
            // draw, which separates "geometry is degenerate", "state kills the
            // fragment", and "it draws but something later erases it".
            // A missing full-screen background would never appear in a first-N
            // sample if the game submits it after the UI text, so also diagnose any
            // triangle large enough to be background geometry regardless of when it
            // arrives.
            const bool diagnose_large =
                is_large && repiu::platform::AtomicIncrement(&large_diag_count) <= 12;
            std::size_t before_non_black = 0;
            const bool diagnose_this_draw =
                draw_diagnostic_enabled && (sequence <= 12U || diagnose_large);
            if (diagnose_this_draw)
            {
                std::vector<std::uint8_t> before;
                if (context->glide_backend.ReadbackFramebuffer(
                        context->glide_state.width, context->glide_state.height,
                        &before))
                {
                    for (std::size_t i = 0; i + 3U < before.size(); i += 4U)
                    {
                        if (before[i] > 8U || before[i + 1U] > 8U ||
                            before[i + 2U] > 8U)
                        {
                            ++before_non_black;
                        }
                    }
                }
            }
            if (!QueueGlideDrawForBatch(context, draw_batch, vertices, 3U,
                                        GlideBatchPrimitive::kTriangles) &&
                !context->glide_backend.DrawTriangle(vertices[0], vertices[1], vertices[2]))
            {
                context->glide_backend_message = context->glide_backend.message();
                return decline_gate("compact-triangle-backend-failure");
            }
            if (diagnose_this_draw)
            {
                std::vector<std::uint8_t> after;
                std::size_t after_non_black = 0;
                if (context->glide_backend.ReadbackFramebuffer(
                        context->glide_state.width, context->glide_state.height,
                        &after))
                {
                    for (std::size_t i = 0; i + 3U < after.size(); i += 4U)
                    {
                        if (after[i] > 8U || after[i + 1U] > 8U ||
                            after[i + 2U] > 8U)
                        {
                            ++after_non_black;
                        }
                    }
                }
                fprintf(stderr,
                        "[repiu-live-debug] tri #%u xy=(%.2f,%.2f)(%.2f,%.2f)"
                        "(%.2f,%.2f) rgba0=(%.3f,%.3f,%.3f,%.3f) st0=(%.2f,%.2f)"
                        " combine=%u/other=%u texEnabled=%d nonblack %zu->%zu\n",
                        sequence, vertices[0].x, vertices[0].y, vertices[1].x,
                        vertices[1].y, vertices[2].x, vertices[2].y,
                        vertices[0].r, vertices[0].g, vertices[0].b,
                        vertices[0].a, vertices[0].s, vertices[0].t,
                        context->glide_state.color_combine.function,
                        context->glide_state.color_combine.other,
                        context->glide_backend.is_texture_combine_enabled() ? 1 : 0,
                        before_non_black, after_non_black);
            }
            ++context->glide_gate_handled_count;
            win32_context->Eip = return_address;
            win32_context->Esp += 4U * sizeof(std::uint32_t);
            return true;
        }

        // Task 420: both polygon forms render as a triangle fan. Glide requires
        // the vertices to describe a convex polygon, so the fan is exact rather
        // than an approximation. The indexed form gathers through `ilist`; the
        // vertex-list form walks a contiguous array at the producer stride.
        case go::kGrDrawPlanarPolygon: // _GRDRAWPLANARPOLYGON@12
        case go::kGrDrawPolygon: // _GRDRAWPOLYGON@12
        case go::kGrAADrawPolygon: // _GRAADRAWPOLYGON@12
        case go::kGrDrawPlanarPolygonVertexList: // _GRDRAWPLANARPOLYGONVERTEXLIST@8
        case go::kGrDrawPolygonVertexList: // _GRDRAWPOLYGONVERTEXLIST@8
        case go::kGrAADrawPolygonVertexList: // _GRAADRAWPOLYGONVERTEXLIST@8
        {
            const bool indexed =
                glide_export->gate_id == go::kGrDrawPlanarPolygon ||
                glide_export->gate_id == go::kGrDrawPolygon ||
                glide_export->gate_id == go::kGrAADrawPolygon;
            const std::uint32_t vertex_count = context->glide_gate_stack[1];
            const std::uint32_t index_list =
                indexed ? context->glide_gate_stack[2] : 0U;
            const std::uint32_t vertex_list =
                context->glide_gate_stack[indexed ? 3U : 2U];
            // The stack adjustment must not depend on whether the draw
            // succeeded, so it is computed once here and applied on every exit.
            const std::uint32_t stack_slots = indexed ? 4U : 3U;

            if (vertex_count < 3U ||
                vertex_count > hle::kMaxGlidePolygonVertices)
            {
                return decline_gate("draw-polygon-vertex-count");
            }
            const auto* indices = reinterpret_cast<const std::int32_t*>(
                static_cast<std::uintptr_t>(index_list));
            if (indexed &&
                !IsGuestRangeReadable(context, indices,
                                      vertex_count * sizeof(std::int32_t)))
            {
                return decline_gate("draw-polygon-unreadable-index-list");
            }

            hle::GlideDrawVertex vertices[hle::kMaxGlidePolygonVertices] = {};
            for (std::uint32_t index = 0U; index < vertex_count; ++index)
            {
                std::int32_t slot = static_cast<std::int32_t>(index);
                if (indexed)
                {
                    std::memcpy(&slot, &indices[index], sizeof(slot));
                    if (slot < 0)
                    {
                        return decline_gate("draw-polygon-negative-index");
                    }
                }
                const auto* source = reinterpret_cast<const std::uint32_t*>(
                    static_cast<std::uintptr_t>(vertex_list) +
                    static_cast<std::uintptr_t>(
                        static_cast<std::uint32_t>(slot)) *
                        hle::kGlideProducerVertexByteCount);
                if (!IsGuestRangeReadable(context, source,
                                          hle::kGlideProducerVertexByteCount))
                {
                    return decline_gate("draw-polygon-unreadable-vertex");
                }
                std::uint32_t producer[
                    hle::kGlideProducerVertexDwordCount] = {};
                std::memcpy(producer, source, sizeof(producer));
                if (!hle::DecodeGlideProducerVertex(
                        producer, hle::kGlideProducerVertexDwordCount,
                        &vertices[index]))
                {
                    return decline_gate("draw-polygon-decode-failure");
                }
            }
            if (!context->glide_backend.DrawPolygon(vertices, vertex_count))
            {
                context->glide_backend_message =
                    context->glide_backend.message();
                return decline_gate("draw-polygon-backend-failure");
            }
            context->glide_backend_message = context->glide_backend.message();
            ++context->glide_gate_handled_count;
            win32_context->Eip = return_address;
            win32_context->Esp += stack_slots * sizeof(std::uint32_t);
            return true;
        }

        case go::kGrLfbLock: // _GRLFBLOCK@24
        {
            // grLfbLock(type, buffer, writeMode, origin, pixelPipeline, info*).
            // The observed content-phase lock is (WRITE_ONLY, BACKBUFFER, 565,
            // UPPER_LEFT, FXTRUE), which is exactly what this path implements.
            const std::uint32_t type = context->glide_gate_stack[1];
            const std::uint32_t buffer = context->glide_gate_stack[2];
            const std::uint32_t write_mode = context->glide_gate_stack[3];
            const std::uint32_t origin = context->glide_gate_stack[4];
            const std::uint32_t info_pointer = context->glide_gate_stack[6];
            auto* info = reinterpret_cast<void*>(
                static_cast<std::uintptr_t>(info_pointer));

            const auto fail_lock = [&](const char* reason) {
                // Retain policy (design 237): an unsupported but ABI-valid lock
                // returns FXFALSE rather than leaking the frame through a reject.
                RecordGlideImplementationIssue(
                    win32_context,
                    context,
                    *glide_export,
                    repiu::hle::GlideImplementationIssueKind::
                        kUnsupportedArgument,
                    reason,
                    "grLfbLock returns FXFALSE",
                    "continue");
                ++context->glide_gate_handled_count;
                win32_context->Eax = 0U;
                win32_context->Eip = return_address;
                win32_context->Esp += 7U * sizeof(std::uint32_t);
                return true;
            };

            if (!IsGuestRangeWritable(context, info,
                                      repiu::hle::kGlideLfbInfoByteCount))
            {
                return decline_gate("lfb-lock-info-pointer-unwritable");
            }
            if (write_mode != repiu::hle::kGlideLfbWriteMode565 ||
                (buffer != repiu::hle::kGlideBufferBackBuffer &&
                 buffer != repiu::hle::kGlideBufferFrontBuffer))
            {
                return fail_lock("unsupported-writemode-or-buffer");
            }
            const std::uint32_t width = context->glide_state.width;
            const std::uint32_t height = context->glide_state.height;
            // Task 708. The same idempotent placement as the region shadow
            // path: this is the call whose result reaches the guest.
            if (!EnsureGlideLfbGuestStorage(&context->glide_lfb_guest_storage,
                                            &context->glide_lfb_surface))
            {
                return fail_lock("lfb-storage-placement-failure");
            }
            if (!context->glide_lfb_surface.Resize(width, height))
            {
                return fail_lock("surface-resize-failure");
            }
            if (!context->glide_lfb_surface.BeginLock(type, buffer, write_mode,
                                                      origin))
            {
                return fail_lock("lock-already-outstanding");
            }
            {
                // Seed the staging surface from the current render target for *every*
                // lock, not just read locks. On real hardware the LFB is the live
                // framebuffer, so a write lock that touches only some pixels leaves the
                // rest untouched. Handing out a zero-filled buffer instead makes unlock
                // blit black over everything already drawn -- observed erasing the
                // triangles submitted immediately before the lock.
                auto* const lfb_timing = GlideLfbTimingProfileEnabled()
                    ? &context->glide_lfb_timing
                    : nullptr;
                const std::uint64_t seed_entry_cycles = lfb_timing != nullptr
                    ? ReadGlideLfbTimingCycles()
                    : 0U;
                // Task 728. The staging surface may already hold this buffer's
                // frame buffer image, because the previous unlock presented all
                // of it and nothing since then could have changed it. Reuse is
                // decided before the seed and recorded either way, so a census
                // run measures the opportunity without taking it.
                const std::uint32_t seed_color_format =
                    type == repiu::hle::kGlideLfbWriteOnly
                        ? context->glide_state.lfb_write_color_format
                        : context->glide_state.color_format;
                if (GlideLfbLockIntervalCensusEnabled())
                {
                    ClassifyGlideLfbLockInterval(
                        &context->glide_lfb_lock_interval_census);
                }
                const bool shadow_census =
                    GlideLfbStagingShadowCensusEnabled();
                const bool shadow_reusable = shadow_census &&
                    CanReuseGlideLfbStagingShadow(
                        context->glide_lfb_staging_shadow, buffer,
                        seed_color_format, width, height);
                // The copy happens here, before the guest is handed the
                // surface. Nothing invalidates the shadow at handoff: it is a
                // host-owned copy rather than the staging surface itself, so
                // the guest writing through `lfbPtr` cannot reach it.
                const bool reuse_shadow = shadow_reusable &&
                    GlideLfbStagingReuseEnabled() &&
                    LoadGlideLfbStagingShadow(
                        context->glide_lfb_staging_shadow, buffer,
                        context->glide_lfb_surface.pixels(),
                        context->glide_lfb_surface.byte_count(),
                        seed_color_format, width, height);
                if (shadow_census)
                {
                    NoteGlideLfbStagingShadowLock(
                        &context->glide_lfb_staging_shadow, shadow_reusable,
                        reuse_shadow);
                }
                std::vector<std::uint8_t> rgba8;
                const bool read_ok = reuse_shadow
                    ? true
                    : context->glide_backend.ReadbackFramebuffer(
                          width, height, &rgba8);
                const std::uint64_t readback_end_cycles = lfb_timing != nullptr
                    ? ReadGlideLfbTimingCycles()
                    : 0U;
                // A reused shadow is already 565 in the surface, so there is
                // nothing to encode; the encode is the inverse of the readback
                // that was skipped above, and running it over an empty `rgba8`
                // would clear what is being reused.
                bool encode_ok = reuse_shadow;
                bool encode_attempted = false;
                if (read_ok && !reuse_shadow)
                {
                    encode_attempted = true;
                    encode_ok = repiu::hle::EncodeRgba8ToGlideLfb565(
                        rgba8.data(), rgba8.size(), width, height,
                        seed_color_format,
                        context->glide_lfb_surface.pixels(),
                        context->glide_lfb_surface.byte_count());
                }
                const std::uint64_t encode_end_cycles = lfb_timing != nullptr
                    ? ReadGlideLfbTimingCycles()
                    : 0U;
                // A reused shadow performed no seed, so recording one would
                // report a readback and an encode that never ran.
                if (!reuse_shadow)
                {
                    RecordGlideLfbTiming(
                        lfb_timing, read_ok, encode_attempted, encode_ok,
                        seed_entry_cycles, readback_end_cycles,
                        encode_end_cycles);
                }
                static long seed_log_count = 0;
                const long seed_index = repiu::platform::AtomicIncrement(&seed_log_count);
                if (seed_index <= 4 && !reuse_shadow)
                {
                    std::size_t seed_non_black = 0;
                    for (std::size_t i = 0; i + 3U < rgba8.size(); i += 4U)
                    {
                        if (rgba8[i] > 8U || rgba8[i + 1U] > 8U ||
                            rgba8[i + 2U] > 8U)
                        {
                            ++seed_non_black;
                        }
                    }
                    fprintf(stderr,
                            "[repiu-live-debug] grLfbLock seed #%ld read=%d"
                            " encode=%d framebuffer non-black=%zu\n",
                            seed_index, read_ok ? 1 : 0, encode_ok ? 1 : 0,
                            seed_non_black);
                }
            }

            // The size field is caller-supplied; echo it back but record a mismatch
            // so an unexpected GrLfbInfo_t layout becomes visible instead of silent.
            std::uint32_t caller_size = 0;
            std::memcpy(&caller_size, info, sizeof(caller_size));
            {
                static long lfb_size_log_count = 0;
                const long index = repiu::platform::AtomicIncrement(&lfb_size_log_count);
                if (index <= 4)
                {
                    // Dump what the caller staged in the struct before we touch it.
                    // Glide 2.4's GrLfbInfo_t starts with a caller-set `size`; PIU
                    // leaves offset 0 at zero, so this dump is what distinguishes "the
                    // game simply never sets size" from "this build's GrLfbInfo_t has
                    // no size field and every field we write is off by one dword".
                    std::uint32_t before[8] = {};
                    const auto* raw = reinterpret_cast<const std::uint32_t*>(
                        static_cast<std::uintptr_t>(info_pointer));
                    if (IsGuestRangeReadable(context, raw, sizeof(before)))
                    {
                        std::memcpy(before, raw, sizeof(before));
                    }
                    fprintf(stderr,
                            "[repiu-live-debug] grLfbLock GrLfbInfo_t caller size=%u"
                            " (expected %u) pre-call dwords="
                            "%08X %08X %08X %08X %08X %08X %08X %08X\n",
                            caller_size,
                            repiu::hle::kGlideLfbInfoExpectedSize,
                            before[0], before[1], before[2], before[3],
                            before[4], before[5], before[6], before[7]);
                }
            }

            // grLfbLock owns every output field, `size` included: it reports the
            // GrLfbInfo_t layout it actually filled. Echoing the caller's value back
            // (PIU leaves it 0) would tell the guest nothing was written.
            std::uint8_t image[repiu::hle::kGlideLfbInfoByteCount] = {};
            const auto staging_pointer = static_cast<std::uint32_t>(
                reinterpret_cast<std::uintptr_t>(
                    context->glide_lfb_surface.pixels()));
            if (!repiu::hle::BuildGlideLfbInfoImage(
                    repiu::hle::kGlideLfbInfoExpectedSize, staging_pointer,
                    context->glide_lfb_surface.stride_in_bytes(), write_mode,
                    origin, image, sizeof(image)) ||
                !WriteGuestBytes(context, info, image, sizeof(image)))
            {
                context->glide_lfb_surface.EndLock();
                return fail_lock("info-write-failure");
            }

            if (type == repiu::hle::kGlideLfbWriteOnly &&
                GlideLfbWriteFootprintEnabled())
            {
                BeginGlideLfbWriteFootprint(
                    &context->glide_lfb_write_footprint,
                    context->glide_lfb_surface.pixels(),
                    context->glide_lfb_surface.byte_count(), width, height);
            }
            if (type == repiu::hle::kGlideLfbWriteOnly &&
                GlideLfbNativeStoreCensusEnabled())
            {
                BeginGlideLfbNativeStoreCensus(
                    &context->glide_lfb_native_store_census, staging_pointer,
                    static_cast<std::uint32_t>(
                        context->glide_lfb_surface.byte_count()));
            }

            ++context->glide_lfb_lock_count;
            if (context->glide_lfb_lock_count <= 4U)
            {
                fprintf(stderr,
                        "[repiu-live-debug] grLfbLock granted #%u type=%u"
                        " buffer=%u writeMode=%u origin=%u lfbPtr=0x%08X"
                        " stride=%u %ux%u\n",
                        context->glide_lfb_lock_count, type, buffer, write_mode,
                        origin, staging_pointer,
                        context->glide_lfb_surface.stride_in_bytes(), width,
                        height);
            }
            ++context->glide_gate_handled_count;
            win32_context->Eax = 1U;
            win32_context->Eip = return_address;
            win32_context->Esp += 7U * sizeof(std::uint32_t);
            return true;
        }

        case go::kGrLfbUnlock: // _GRLFBUNLOCK@8
        {
            // grLfbUnlock(type, buffer): a write lock's staging content becomes the
            // back-buffer image here; the next grBufferSwap presents it.
            const std::uint32_t type = context->glide_gate_stack[1];
            if (context->glide_lfb_surface.locked() &&
                type == repiu::hle::kGlideLfbWriteOnly)
            {
                if (GlideLfbNativeStoreCensusEnabled())
                {
                    EndGlideLfbNativeStoreCensus(
                        &context->glide_lfb_native_store_census);
                }
                if (GlideLfbWriteFootprintEnabled())
                {
                    CompareGlideLfbWriteFootprint(
                        &context->glide_lfb_write_footprint,
                        context->glide_lfb_surface.pixels(),
                        context->glide_lfb_surface.byte_count(),
                        context->glide_lfb_surface.width(),
                        context->glide_lfb_surface.height());
                }
                {
                    // Did the guest actually write into the surface we handed it? A
                    // non-zero count proves the lfbPtr round-trip works end to end and
                    // separates "blit is broken" from "guest never wrote".
                    static long unlock_probe_count = 0;
                    const long probe = repiu::platform::AtomicIncrement(&unlock_probe_count);
                    if (probe <= 8)
                    {
                        const std::uint8_t* pixels =
                            context->glide_lfb_surface.pixels();
                        const std::size_t total =
                            context->glide_lfb_surface.byte_count();
                        std::size_t non_zero = 0;
                        for (std::size_t i = 0; i < total; ++i)
                        {
                            if (pixels[i] != 0U)
                            {
                                ++non_zero;
                            }
                        }
                        fprintf(stderr,
                                "[repiu-live-debug] grLfbUnlock #%ld non-zero"
                                " staging bytes=%zu/%zu first-texels="
                                "%02X%02X %02X%02X %02X%02X\n",
                                probe, non_zero, total, pixels[1], pixels[0],
                                pixels[3], pixels[2], pixels[5], pixels[4]);
                    }
                }
                std::vector<std::uint8_t> rgba8;
                if (repiu::hle::DecodeGlideLfb565ToRgba8(
                        context->glide_lfb_surface.pixels(),
                        context->glide_lfb_surface.byte_count(),
                        context->glide_lfb_surface.width(),
                        context->glide_lfb_surface.height(),
                        context->glide_state.lfb_write_color_format, &rgba8))
                {
                    const bool flip_v =
                        context->glide_lfb_surface.lock_origin() ==
                        repiu::hle::kGlideOriginLowerLeft;
                    const bool present_to_front =
                        context->glide_lfb_surface.lock_buffer() ==
                        repiu::hle::kGlideBufferFrontBuffer;
                    const bool presented =
                        context->glide_backend.PresentLfbSurface(
                            rgba8.data(), context->glide_lfb_surface.width(),
                            context->glide_lfb_surface.height(), flip_v,
                            present_to_front);
                    context->glide_backend_message =
                        context->glide_backend.message();
                    // Task 728. This present wrote the whole staging surface to
                    // the frame buffer, so the surface now mirrors it -- but
                    // only when the blit was row-for-row. A flipped present
                    // leaves the frame buffer upside down relative to the
                    // surface, which is not a shadow of it.
                    if (GlideLfbStagingShadowCensusEnabled())
                    {
                        if (!presented)
                        {
                            InvalidateGlideLfbStagingShadowBuffer(
                                &context->glide_lfb_staging_shadow,
                                context->glide_lfb_surface.lock_buffer(),
                                GlideLfbStagingShadowInvalidation::
                                    kPresentFailed);
                        }
                        else if (flip_v)
                        {
                            InvalidateGlideLfbStagingShadowBuffer(
                                &context->glide_lfb_staging_shadow,
                                context->glide_lfb_surface.lock_buffer(),
                                GlideLfbStagingShadowInvalidation::
                                    kFlippedPresent);
                        }
                        else
                        {
                            StoreGlideLfbStagingShadow(
                                &context->glide_lfb_staging_shadow,
                                context->glide_lfb_surface.lock_buffer(),
                                context->glide_lfb_surface.pixels(),
                                context->glide_lfb_surface.byte_count(),
                                context->glide_state.lfb_write_color_format,
                                context->glide_lfb_surface.width(),
                                context->glide_lfb_surface.height());
                        }
                    }
                    if (presented && GlideLfbLockIntervalCensusEnabled())
                    {
                        OpenGlideLfbLockInterval(
                            &context->glide_lfb_lock_interval_census);
                    }
                    if (presented)
                    {
                        ++context->glide_lfb_present_count;
                        if (context->glide_lfb_present_count <= 4U)
                        {
                            // The swap-driven pixel diagnostic cannot help here: the
                            // guest may never swap again. Sample the back buffer right
                            // after the blit so the blit itself is verifiable.
                            std::vector<std::uint8_t> after;
                            if (context->glide_backend.ReadbackFramebuffer(
                                    context->glide_lfb_surface.width(),
                                    context->glide_lfb_surface.height(), &after))
                            {
                                std::size_t non_black = 0;
                                std::size_t non_zero = 0;
                                std::uint8_t maximum = 0U;
                                for (std::size_t i = 0; i + 3U < after.size();
                                     i += 4U)
                                {
                                    if (after[i] > 8U || after[i + 1U] > 8U ||
                                        after[i + 2U] > 8U)
                                    {
                                        ++non_black;
                                    }
                                    if (after[i] != 0U || after[i + 1U] != 0U ||
                                        after[i + 2U] != 0U)
                                    {
                                        ++non_zero;
                                    }
                                    maximum = (std::max)(maximum, after[i]);
                                    maximum = (std::max)(maximum, after[i + 1U]);
                                    maximum = (std::max)(maximum, after[i + 2U]);
                                }
                                fprintf(stderr,
                                        "[repiu-live-debug] LFB blit #%u back-buffer"
                                        " visible=%zu/%zu nonzero=%zu max=%u\n",
                                        context->glide_lfb_present_count, non_black,
                                        after.size() / 4U, non_zero, maximum);
                            }
                        }
                    }
                    else
                    {
                        record_backend_failure(
                            "lfb-unlock-present-backend-failure",
                            context->glide_backend_message);
                    }
                }
                else
                {
                    record_backend_failure(
                        "lfb-unlock-decode-failure",
                        "565 staging surface could not be decoded");
                }
            }
            else
            {
                record_unsupported(
                    "lfb-unlock-state-unsupported",
                    "no matching write lock is outstanding");
            }
            context->glide_lfb_surface.EndLock();
            ++context->glide_gate_handled_count;
            win32_context->Eip = return_address;
            win32_context->Esp += 3U * sizeof(std::uint32_t);
            return true;
        }

        case go::kGrLfbWriteRegion: // _GRLFBWRITEREGION@32
        {
            const auto arguments = CaptureGlideImplementationIssueArguments(
                win32_context, context, glide_export->argument_byte_count);
            const std::uint32_t dst_buffer = context->glide_gate_stack[1];
            const std::uint32_t dst_x = context->glide_gate_stack[2];
            const std::uint32_t dst_y = context->glide_gate_stack[3];
            const std::uint32_t src_format = context->glide_gate_stack[4];
            const std::uint32_t src_width = context->glide_gate_stack[5];
            const std::uint32_t src_height = context->glide_gate_stack[6];
            const std::int32_t src_stride = static_cast<std::int32_t>(context->glide_gate_stack[7]);
            const std::uint32_t src_data_ptr = arguments[7];

            // The region write lands in the frame buffer shadow, not on the
            // screen: presenting a whole surface per row would both destroy the
            // untouched pixels and cost one full-screen upload per scanline.
            // The shadow reaches the backend at the next non-region gate.
            const std::size_t data_size = ResolveGlideLfbRegionSpan(
                src_width, src_height, src_format, src_stride);
            const auto* guest_ptr = reinterpret_cast<const void*>(
                static_cast<std::uintptr_t>(src_data_ptr));
            const char* decline = ClassifyGlideLfbRegionArguments(
                context, dst_buffer, src_width, src_height, src_stride,
                src_format);
            if (decline != nullptr)
            {
                record_unsupported("lfb-write-region-unsupported", decline);
            }
            else if (data_size == 0U)
            {
                record_unsupported(
                    "lfb-write-region-unsupported",
                    "source geometry does not describe any pixels");
            }
            else if (!IsGuestRangeReadable(
                         context, guest_ptr,
                         static_cast<std::uint32_t>(data_size)))
            {
                record_backend_failure(
                    "lfb-write-region-unreadable-memory",
                    "source region is not guest-readable");
            }
            else if (!EnsureGlideLfbRegionShadow(context, dst_buffer))
            {
                record_backend_failure(
                    "lfb-write-region-shadow-failure",
                    "frame buffer shadow could not be prepared");
            }
            else
            {
                std::vector<std::uint8_t> host_src(data_size);
                std::memcpy(host_src.data(), guest_ptr, data_size);
                if (!repiu::hle::WriteGlideLfbRegion(
                        dst_x, dst_y, src_width, src_height, src_format,
                        src_stride, host_src.data(), host_src.size(),
                        context->glide_state.lfb_write_color_format,
                        context->glide_state.color_format,
                        &context->glide_lfb_surface))
                {
                    record_unsupported(
                        "lfb-write-region-geometry-unsupported",
                        "region lies outside the frame buffer");
                }
                else
                {
                    // Task 761: mirror the write into the RGBA8 shadow while
                    // the high-precision path is on. A shadow that is not
                    // valid yet -- the toggle was flipped mid-run -- starts
                    // from a decode of the current staging, which is exactly
                    // what the off path would present, and gains precision
                    // from this write on.
                    if (context->glide_backend.LfbHighPrecisionEnabled())
                    {
                        const std::uint32_t surface_width =
                            context->glide_lfb_surface.width();
                        const std::uint32_t surface_height =
                            context->glide_lfb_surface.height();
                        if (!context->glide_lfb_region_rgba8_valid)
                        {
                            context->glide_lfb_region_rgba8_valid =
                                repiu::hle::DecodeGlideLfb565ToRgba8(
                                    context->glide_lfb_surface.pixels(),
                                    context->glide_lfb_surface.byte_count(),
                                    surface_width, surface_height,
                                    context->glide_state.color_format,
                                    &context->glide_lfb_region_rgba8_shadow);
                        }
                        if (context->glide_lfb_region_rgba8_valid &&
                            !repiu::hle::WriteGlideLfbRegionRgba8(
                                dst_x, dst_y, src_width, src_height,
                                src_format, src_stride, host_src.data(),
                                host_src.size(),
                                context->glide_state.lfb_write_color_format,
                                context->glide_state.color_format,
                                surface_width, surface_height,
                                context->glide_lfb_region_rgba8_shadow.data(),
                                context->glide_lfb_region_rgba8_shadow.size()))
                        {
                            context->glide_lfb_region_rgba8_valid = false;
                        }
                    }
                    else
                    {
                        context->glide_lfb_region_rgba8_valid = false;
                    }
                    context->glide_lfb_region_shadow_dirty = true;
                    ++context->glide_lfb_region_write_count;
                }
            }
            ++context->glide_gate_handled_count;
            // FXTRUE even when declined, preserving the work-order 002 finding
            // that a false return from the LFB family stalls the guest.
            win32_context->Eax = 1U;
            win32_context->Eip = return_address;
            win32_context->Esp += 9U * sizeof(std::uint32_t);
            return true;
        }

        case go::kGrLfbReadRegion: // _GRLFBREADREGION@28
        {
            // grLfbReadRegion(buffer, x, y, width, height, dstStride, dstData)
            // hands back frame buffer pixels in the buffer's native 565 form.
            const auto arguments = CaptureGlideImplementationIssueArguments(
                win32_context, context, glide_export->argument_byte_count);
            const std::uint32_t src_buffer = context->glide_gate_stack[1];
            const std::uint32_t src_x = context->glide_gate_stack[2];
            const std::uint32_t src_y = context->glide_gate_stack[3];
            const std::uint32_t src_width = context->glide_gate_stack[4];
            const std::uint32_t src_height = context->glide_gate_stack[5];
            const std::int32_t dst_stride =
                static_cast<std::int32_t>(context->glide_gate_stack[6]);
            const std::uint32_t dst_data_ptr = arguments[6];

            const std::size_t data_size = ResolveGlideLfbRegionSpan(
                src_width, src_height, repiu::hle::kGlideLfbSrcFmt565,
                dst_stride);
            auto* guest_ptr = reinterpret_cast<void*>(
                static_cast<std::uintptr_t>(dst_data_ptr));
            const char* decline = ClassifyGlideLfbRegionArguments(
                context, src_buffer, src_width, src_height, dst_stride,
                repiu::hle::kGlideLfbSrcFmt565);
            if (decline != nullptr)
            {
                record_unsupported("lfb-read-region-unsupported", decline);
            }
            else if (data_size == 0U)
            {
                record_unsupported(
                    "lfb-read-region-unsupported",
                    "destination geometry does not describe any pixels");
            }
            else if (!IsGuestRangeWritable(
                         context, guest_ptr,
                         static_cast<std::uint32_t>(data_size)))
            {
                record_backend_failure(
                    "lfb-read-region-unwritable-memory",
                    "destination region is not guest-writable");
            }
            else if (!EnsureGlideLfbRegionShadow(context, src_buffer))
            {
                record_backend_failure(
                    "lfb-read-region-shadow-failure",
                    "frame buffer shadow could not be prepared");
            }
            else
            {
                std::vector<std::uint8_t> host_dst(data_size);
                std::uint32_t copied_width = 0;
                std::uint32_t copied_height = 0;
                const std::size_t row_pitch = static_cast<std::size_t>(
                    repiu::hle::ResolveGlideLfbRegionStride(
                        src_width, repiu::hle::kGlideLfb565BytesPerTexel,
                        dst_stride));
                if (!repiu::hle::ReadGlideLfbRegion(
                        src_x, src_y, src_width, src_height, dst_stride,
                        context->glide_lfb_surface, host_dst.data(),
                        host_dst.size(), &copied_width, &copied_height))
                {
                    record_unsupported(
                        "lfb-read-region-geometry-unsupported",
                        "region lies outside the frame buffer");
                }
                else
                {
                    // Copy row by row so a destination stride wider than the
                    // rectangle keeps whatever the guest put in the padding.
                    const std::size_t row_bytes =
                        static_cast<std::size_t>(copied_width) *
                        repiu::hle::kGlideLfb565BytesPerTexel;
                    bool written = true;
                    for (std::uint32_t row = 0; row < copied_height && written;
                         ++row)
                    {
                        const std::size_t offset =
                            static_cast<std::size_t>(row) * row_pitch;
                        written = WriteGuestBytes(
                            context,
                            reinterpret_cast<void*>(
                                static_cast<std::uintptr_t>(dst_data_ptr) +
                                offset),
                            host_dst.data() + offset, row_bytes);
                    }
                    if (!written)
                    {
                        record_backend_failure(
                            "lfb-read-region-write-failure",
                            "destination region could not be written");
                    }
                    else
                    {
                        ++context->glide_lfb_region_read_count;
                    }
                }
            }
            ++context->glide_gate_handled_count;
            win32_context->Eax = 1U;
            win32_context->Eip = return_address;
            win32_context->Esp += 8U * sizeof(std::uint32_t);
            return true;
        }

        case go::kGrLfbConstantAlpha: // _GRLFBCONSTANTALPHA@4
        case go::kGrLfbConstantDepth: // _GRLFBCONSTANTDEPTH@4
            record_unimplemented(
                "lfb-constant-state-noop",
                "state accepted without implementation");
            ++context->glide_gate_handled_count;
            win32_context->Eip = return_address;
            win32_context->Esp += 2U * sizeof(std::uint32_t);
            return true;

        case go::kGrLfbWriteColorSwizzle: // _GRLFBWRITECOLORSWIZZLE@8
            record_unimplemented(
                "lfb-color-swizzle-noop",
                "state accepted without implementation");
            ++context->glide_gate_handled_count;
            win32_context->Eip = return_address;
            win32_context->Esp += 3U * sizeof(std::uint32_t);
            return true;

        default:
            break;
    }
    // Default handler for unhandled but cataloged gates (Phase R0)
    record_unimplemented(
        "catalog-default-handler",
        "ABI-preserving default return");
    ++context->glide_gate_handled_count;
    if (signature->return_kind != repiu::hle::GlideReturnKind::kVoid)
    {
        win32_context->Eax = 0U;
    }
    win32_context->Eip = return_address;
    win32_context->Esp += sizeof(std::uint32_t) + signature->argument_byte_count;

    return true;
}

// Lightweight VEH transfer paths run without an ExceptionDispatchScope, so
// these counters must be mirrored into shared telemetry here to stay
// externally observable during dispatch-silent phases.

} // namespace repiu::engine
