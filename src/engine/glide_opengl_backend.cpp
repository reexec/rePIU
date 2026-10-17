#include "repiu/engine/glide_opengl_backend.h"
#include "repiu/engine/active_jamma_bindings.h"
#include "repiu/engine/gl_renderer_identity.h"
#include "repiu/engine/gl_shader_dialect.h"
#include "repiu/engine/glide_letterbox.h"
#include "repiu/engine/glide_osd.h"
#include "repiu/engine/glide_post_process.h"
#include "repiu/engine/post_shader_catalog.h"
#include "repiu/engine/session_display_preferences.h"
#include "repiu/engine/session_identity.h"
#include "repiu/hle/glide_lfb.h"
#include "repiu/runtime/env_toggle.h"
#include "repiu/runtime/execution_model.h"
#include "repiu/hle/glide_texture_decode.h"
#include "repiu/input/jamma_input_bindings.h"
#include "repiu/input/pad_exit_chord.h"
#include "repiu/input/pad_osd_chord.h"
#include "repiu/engine/execution_time_profile.h"
#include "repiu/engine/jamma_input_timeline.h"
#include "repiu/platform/build_identity.h"
#include "sdl_bios_keyboard_adapter.h"
#include "sdl_input_script.h"
#include "sdl_pad_input.h"
#include "browser_input_bridge.h"
#include "repiu/platform/host_gpu_driver.h"


// SDL is this project's cross-platform layer, so these need no guard. They had
// one because nothing had ever compiled this file anywhere else, and on Linux
// it removed every GL declaration the file uses.
#include <SDL3/SDL.h>
#include <SDL3/SDL_opengl.h>

// Task 419: the pause hint for the rendezvous spin. Task 758: <immintrin.h>
// declares it on MSVC and GCC alike. Issue #67: x86 hosts only; elsewhere
// (the web) the hint is empty, and on one thread the spin is never reached.
#if defined(_M_IX86) || defined(_M_X64) || defined(__i386__) || defined(__x86_64__)
#include <immintrin.h>
#define REPIU_SPIN_PAUSE() _mm_pause()
#else
#define REPIU_SPIN_PAUSE() ((void)0)
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <vector>


#if !defined(REPIU_VERSION)
#define REPIU_VERSION "unknown"
#endif

namespace repiu::engine {

// Task 748. Wraps the player so the backend header needs only a forward
// declaration.

class SdlInputScriptPlayerHandle {
public:
  SdlInputScriptPlayer player;
};
namespace {

// Task 497: the mapping comes from the configured binding table instead of a
// switch. The keycode needs no translation because SDL_Keycode is what a
// binding stores, so this is a direct comparison against `event.key.key`.
//
// `modifier_state` is `event.key.mod`, i.e. the modifiers held at the instant
// the base key went down. Releasing a modifier afterwards produces no key
// event for the base key, so the input stays held until the base key itself is
// released -- which is also how a real cabinet button behaves. The polling
// path in ScanJammaPort8 samples modifiers live instead; both differences are
// deliberate and documented in the design.
bool FindJammaInputForPress(SDL_Keycode keycode, SDL_Keymod modifier_state,
                            JammaInputKey *key) {
  if (key == nullptr) {
    return false;
  }

  const repiu::input::ResolvedJammaBindings &bindings = ActiveJammaBindings();
  for (std::uint32_t index = 0; index < repiu::input::kJammaInputKeyCount;
       ++index) {
    const repiu::input::JammaInputBinding &binding = bindings.inputs[index];
    for (std::uint32_t slot = 0; slot < binding.alias_count; ++slot) {
      const repiu::input::HostKeyAlias &alias = binding.aliases[slot];
      if (alias.keycode != keycode ||
          !alias.ModifiersMatch(modifier_state)) {
        continue;
      }
      *key = static_cast<JammaInputKey>(index);
      return true;
    }
  }
  return false;
}

// Releases deliberately ignore modifier state, and match every input bound to
// the key rather than just one.
//
// A release event carries the modifiers held at release time, not at press
// time. Press Ctrl+F1, let go of Ctrl, then let go of F1 and the key-up would
// no longer satisfy the Ctrl+F1 alias -- the input would never be released and
// TEST would stay held for the rest of the run. Releasing everything the key
// can drive avoids that, and releasing an input that was not pressed is a
// no-op in the timeline.
std::uint16_t JammaInputMaskForKeycode(SDL_Keycode keycode) {
  const repiu::input::ResolvedJammaBindings &bindings = ActiveJammaBindings();
  std::uint16_t mask = 0U;
  for (std::uint32_t index = 0; index < repiu::input::kJammaInputKeyCount;
       ++index) {
    const repiu::input::JammaInputBinding &binding = bindings.inputs[index];
    for (std::uint32_t slot = 0; slot < binding.alias_count; ++slot) {
      if (binding.aliases[slot].keycode == keycode) {
        mask = static_cast<std::uint16_t>(
            mask | JammaInputKeyMask(static_cast<JammaInputKey>(index)));
        break;
      }
    }
  }
  return mask;
}

} // namespace

// Task 440: the asynchronous channel's storage, kept off `ThreadContext`'s
// stack footprint. Guarded by the backend's `host_command_mutex_`, so it shares
// one lock with the synchronous slot and "drain, then run the sync command"
// stays a single ordered decision.
struct GlideAsyncPresentState {
  std::deque<std::function<void()>> commands;
  // Atomics, not plain counters under the channel lock. The timeout path
  // terminates the guest thread outright, and a thread killed while holding
  // that lock leaves it owned forever -- a snapshot that took it would then
  // hang or fault at teardown, which is exactly what was observed. Diagnostics
  // must never depend on a lock the run may have lost.
  std::atomic<bool> enabled{false};
  std::atomic<std::uint64_t> posted_count{0};
  std::atomic<std::uint64_t> posted_swap_count{0};
  std::atomic<std::uint64_t> executed_count{0};
  std::atomic<std::uint64_t> failure_count{0};
  std::atomic<std::uint64_t> back_pressure_count{0};
  std::atomic<std::uint64_t> refused_count{0};
  std::atomic<std::uint32_t> max_queue_depth{0};
};

// Task 433. Maps the Glide depth value onto the eye-space z this projection
// wants. Two facts make it this simple. Glide's depth buffer is 16 bits holding
// 0..65535, so saturating there is the hardware's own behaviour rather than a
// clamp invented here. And `grDepthBufferFunction` already maps straight onto
// `glDepthFunc`, because GR_CMP_* (0..7) and GL_NEVER..GL_ALWAYS share an order
// -- so any **monotonically increasing** map carries the comparison result
// across unchanged. That is why nothing below needs to decide which end is
// "near": the depth buffer stores numbers and compares them, and identical
// ordering gives identical results.
//
// Under `glOrtho(0, w, 0, h, -1, 1)` the window depth is `(1 - z_eye) / 2`, so
// a desired depth `d` is emitted as `1 - 2d`. The projection is deliberately
// left alone: `d = 0.5` is `z_eye = 0`, which is exactly what every path
// carrying no depth already emits. See
// docs/design/20260806-433-glide-vertex-depth.md.
float GlideOozToOrthoEyeZ(const float ooz) {
  constexpr float kGlideDepthMaximum = 65535.0F;
  if (!std::isfinite(ooz) || ooz <= 0.0F) {
    return 1.0F;
  }
  const float clamped = ooz > kGlideDepthMaximum ? kGlideDepthMaximum : ooz;
  return 1.0F - 2.0F * (clamped / kGlideDepthMaximum);
}

float CalculateGlidePointSize(const std::uint32_t logical_width,
                              const std::uint32_t logical_height,
                              const std::uint32_t drawable_width,
                              const std::uint32_t drawable_height) {
  if (logical_width == 0U || logical_height == 0U || drawable_width == 0U ||
      drawable_height == 0U) {
    return 1.0F;
  }
  const float horizontal_scale =
      static_cast<float>(drawable_width) / static_cast<float>(logical_width);
  const float vertical_scale =
      static_cast<float>(drawable_height) / static_cast<float>(logical_height);
  return std::max(1.0F, std::min(horizontal_scale, vertical_scale));
}

bool TranslateGlideOpenGlCullMode(const std::uint32_t mode,
                                  const bool origin_lower_left,
                                  GlideOpenGlCullFace *face) {
  if (face == nullptr || mode > 2U) {
    return false;
  }
  if (mode == 0U) {
    *face = GlideOpenGlCullFace::kDisabled;
    return true;
  }
  const bool cull_front = origin_lower_left ? mode == 2U : mode == 1U;
  *face = cull_front ? GlideOpenGlCullFace::kFront : GlideOpenGlCullFace::kBack;
  return true;
}

GlideAsyncPresentState &GlideOpenGlBackend::async_present() {
  if (async_present_state_ == nullptr) {
    async_present_state_ = std::make_unique<GlideAsyncPresentState>();
  }
  return *async_present_state_;
}

const GlideAsyncPresentState &GlideOpenGlBackend::async_present() const {
  // Allocated on first use by the non-const path; a reader that arrives first
  // sees an empty state, which is what "nothing has been posted" means.
  static const GlideAsyncPresentState empty;
  return async_present_state_ != nullptr ? *async_present_state_ : empty;
}

GlideOpenGlBackend::GlideOpenGlBackend() = default;

// Runs the posted commands and nothing else. Deliberately separate from
// `PumpHostCommands`: the synchronous slot stays marked pending while its
// command runs, so a command that pumped would re-enter itself forever. `Close`
// is exactly such a command when the guest initiates it.
void GlideOpenGlBackend::DrainAsyncCommands() {
  if (!IsHostThread()) {
    return;
  }
  // Nothing was ever posted: take no lock, allocate nothing, and leave the
  // pump exactly the path it was before this task. The queue depth is an
  // atomic precisely so this test costs nothing.
  if (queued_command_count_.load(std::memory_order_acquire) == 0U) {
    return;
  }
  for (;;) {
    std::function<void()> async_command;
    {
      std::lock_guard<std::mutex> lock(host_command_mutex_);
      if (async_present().commands.empty()) {
        break;
      }
      async_command = std::move(async_present().commands.front());
      async_present().commands.pop_front();
      queued_command_count_.fetch_sub(1U, std::memory_order_relaxed);
    }
    try {
      async_command();
    } catch (...) {
      std::lock_guard<std::mutex> lock(host_command_mutex_);
      ++async_present().failure_count;
    }
    {
      std::lock_guard<std::mutex> lock(host_command_mutex_);
      ++async_present().executed_count;
    }
    // Room freed, and a swap possibly retired: wake anyone the bound stopped.
    host_command_cv_.notify_all();
  }
}

GlideOpenGlBackend::~GlideOpenGlBackend() { Close(); }

void GlideOpenGlBackend::BindHostThread() {
  host_thread_id_ = std::this_thread::get_id();
  if (jamma_input_timeline_ != nullptr) {
    jamma_input_timeline_->Reset(EventClockNanoseconds(),
                                 CaptureCurrentJammaPressedMask());
  }
}

void GlideOpenGlBackend::SetJammaInputTimeline(
    JammaInputTimeline *timeline) {
  jamma_input_timeline_ = timeline;
}

void GlideOpenGlBackend::SetBiosKeyboard(hle::BiosKeyboard *keyboard) {
  bios_keyboard_ = keyboard;
}

namespace {

// Task 754. See repiu/engine/event_clock.h.
bool EventClockUsesSteady() {
  static const bool steady =
      ResolveEventClockUsesSteady(std::getenv("REPIU_EVENT_CLOCK"));
  return steady;
}

std::uint64_t SteadyClockNanoseconds() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

} // namespace

std::uint64_t GlideOpenGlBackend::EventClockNanoseconds() const {
  return EventClockUsesSteady() ? SteadyClockNanoseconds() : SDL_GetTicksNS();
}

std::uint64_t GlideOpenGlBackend::EventTimestampNanoseconds(
    std::uint64_t sdl_timestamp) const {
  if (!EventClockUsesSteady()) {
    return sdl_timestamp;
  }
  return TranslateEventTimestamp(sdl_timestamp, SDL_GetTicksNS(),
                                 SteadyClockNanoseconds());
}

void GlideOpenGlBackend::SetExecutionBackend(
    runtime::ExecutionBackend backend) {
  execution_backend_ = backend;
}

// Issue #64. On one thread (RunsHostOnOneThread) every command comes from the
// host thread itself and runs in place, so a command that has to be handed to
// the host waits for a host that only runs after the caller returns: a
// deadlock. Reaching here on one thread is a bug, and stopping at once with a
// line is better than hanging without one.
void GlideOpenGlBackend::RefuseCrossThreadCommandOnOneThread() const {
  if (!runtime::execution_model::RunsHostOnOneThread()) {
    return;
  }
  fprintf(stderr,
          "[repiu-glide] a command was handed to the host thread on the "
          "one-thread host, which would deadlock\n");
  fflush(stderr);
  std::abort();
}

bool GlideOpenGlBackend::IsHostThread() const {
  return host_thread_id_ == std::thread::id{} ||
         host_thread_id_ == std::this_thread::get_id();
}

// Task 333: resolved once rather than per command, since the rendezvous is on
// the hot path and `getenv` is not.
bool GlideOpenGlBackend::GlideGateTimingEnabled() {
  if (!glide_gate_timing_resolved_) {
    glide_gate_timing_enabled_ = ExecutionTimeProfileEnabled();
    glide_gate_timing_resolved_ = true;
  }
  return glide_gate_timing_enabled_;
}

// Task 419: the rendezvous spin budget, resolved once like the gate above
// because this sits on the hot path. Zero restores the pure condition-variable
// wait, which is the A/B control.
// See docs/design/20260805-419-glide-rendezvous-spin-wait.md.
namespace {
constexpr std::uint32_t kDefaultRendezvousSpinMicroseconds = 20U;
// A budget above this would trade more CPU than a round trip could ever cost.
constexpr unsigned long kMaxRendezvousSpinMicroseconds = 1000UL;
// Clock reads are not free either, so the deadline is only checked every this
// many pauses. At ~30 cycles per pause this keeps the check under a percent of
// the spin while still ending within a microsecond of the budget.
constexpr int kRendezvousSpinPausesPerClockCheck = 64;
} // namespace

std::uint32_t GlideOpenGlBackend::RendezvousSpinMicroseconds() {
  if (!rendezvous_spin_resolved_) {
    rendezvous_spin_microseconds_ = kDefaultRendezvousSpinMicroseconds;
    if (const char *value = std::getenv("REPIU_GLIDE_RENDEZVOUS_SPIN_US")) {
      char *end = nullptr;
      const unsigned long parsed = std::strtoul(value, &end, 10);
      if (end != value && parsed <= kMaxRendezvousSpinMicroseconds) {
        rendezvous_spin_microseconds_ = static_cast<std::uint32_t>(parsed);
      }
    }
    rendezvous_spin_resolved_ = true;
  }
  return rendezvous_spin_microseconds_;
}

// Task 419: Task 418 measured 65.9% of pumpit3's gate time as thread wake-up
// latency rather than GL work, at about 70,000 cycles of round trip against
// 34,745 cycles of work per call. Spinning briefly before blocking pays that
// latency with cycles instead of two context switches.
//
// The returned bool is a **hint**. The caller must still take the mutex and
// re-test the real predicate: the flags this reads are published under the lock
// but read outside it, so acting on one directly would reintroduce exactly the
// lost-wakeup race the condition variable exists to prevent.
bool GlideOpenGlBackend::SpinForRendezvousHint(const std::atomic<bool> &hint,
                                               bool expected, bool guest_side) {
  const std::uint32_t budget = RendezvousSpinMicroseconds();
  if (budget == 0U) {
    return false;
  }

  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::microseconds(budget);
  for (;;) {
    for (int pause = 0; pause < kRendezvousSpinPausesPerClockCheck; ++pause) {
      if (hint.load(std::memory_order_acquire) == expected) {
        if (guest_side) {
          ++rendezvous_spin_guest_hit_;
        } else {
          ++rendezvous_spin_host_hit_;
        }
        return true;
      }
      REPIU_SPIN_PAUSE();
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      break;
    }
  }

  if (guest_side) {
    ++rendezvous_spin_guest_miss_;
  } else {
    ++rendezvous_spin_host_miss_;
  }
  return false;
}

// Task 364: same one-time resolution as above. This gate is separate from the
// gate timing gate because it adds clock reads inside the OpenGL interval,
// which is exactly the observer risk Task 353's method rule warns about.
bool GlideOpenGlBackend::GlideSetterPhaseEnabled() {
  if (!glide_setter_phase_resolved_) {
    glide_setter_phase_enabled_ = GlideSetterPhaseProfileEnabled();
    glide_setter_phase_resolved_ = true;
  }
  return glide_setter_phase_enabled_;
}

// Task 369: same one-time resolution. Off by default, so the setter bodies stop
// issuing the pipeline flush that Task 364's phase instrument measured
// at 99.81% of `grDepthMask` cost.
bool GlideOpenGlBackend::GlideGlErrorCheckEnabled() {
  if (!glide_gl_error_check_resolved_) {
    glide_gl_error_check_enabled_ = GlideGlErrorCheckPolicyEnabled();
    glide_gl_error_check_resolved_ = true;
  }
  return glide_gl_error_check_enabled_;
}

namespace {

// GL_KHR_debug constants. `SDL_opengl.h` here is the 1.1 header, so the tokens
// are spelled out rather than assumed present.
constexpr GLenum kGlDebugOutput = 0x92E0;
constexpr GLenum kGlDebugOutputSynchronous = 0x8242;
constexpr GLenum kGlDebugTypeError = 0x824C;
constexpr GLenum kGlDebugSeverityNotification = 0x826B;
constexpr GLenum kGlDontCare = 0x1100;

using GlDebugProc = void(APIENTRY *)(GLenum source, GLenum type, GLuint id,
                                     GLenum severity, GLsizei length,
                                     const char *message,
                                     const void *user_param);
using GlDebugMessageCallbackProc = void(APIENTRY *)(GlDebugProc callback,
                                                    const void *user_param);
using GlDebugMessageControlProc = void(APIENTRY *)(GLenum source, GLenum type,
                                                   GLenum severity,
                                                   GLsizei count,
                                                   const GLuint *ids,
                                                   GLboolean enabled);

// Same validation the shader module uses: a GL loader reports failure as 0, 1,
// 2, 3, or -1 rather than only null. That convention comes from
// `wglGetProcAddress`, which is where this check was learned; the call below is
// SDL's, and it inherits the convention from whatever loader SDL is sitting on.
//
// Task 505: the address is held as `SDL_FunctionPointer`, which is what SDL3
// returns, rather than `void*`. MSVC converts between the two silently and GCC
// refuses -- and GCC is right, because C++ does not guarantee a function
// pointer fits in an object pointer. The failure test is unaffected: both
// convert to `std::uintptr_t`.
template <typename Function>
bool ResolveOpenGlFunction(const char *name, Function *function) {
  SDL_FunctionPointer address = SDL_GL_GetProcAddress(name);
  const auto value = reinterpret_cast<std::uintptr_t>(address);
  if (address == nullptr || value <= 3U || value == ~std::uintptr_t{0}) {
    return false;
  }
  *function = reinterpret_cast<Function>(address);
  return true;
}

// A named function rather than a lambda so the driver's calling convention is
// stated rather than inferred from a conversion.
void APIENTRY GlideGlDebugTrampoline(GLenum /*source*/, GLenum type, GLuint id,
                                     GLenum severity, GLsizei length,
                                     const char *message,
                                     const void *user_param) {
  auto *backend =
      static_cast<GlideOpenGlBackend *>(const_cast<void *>(user_param));
  if (backend == nullptr) {
    return;
  }
  // Drivers emit performance and notification chatter through the same
  // channel; only a real error supplies the retained first message.
  const bool is_error =
      type == kGlDebugTypeError && severity != kGlDebugSeverityNotification;
  backend->RecordGlDebugMessage(
      static_cast<std::uint32_t>(id), is_error, message,
      length < 0 ? 0U : static_cast<std::size_t>(length));
}

} // namespace

bool GlideOpenGlBackend::InstallGlDebugOutput() {
  GlDebugMessageCallbackProc debug_message_callback = nullptr;
  if (!ResolveOpenGlFunction("glDebugMessageCallback",
                             &debug_message_callback)) {
    return false;
  }
  // Task 370: this must stay disabled. Synchronous debug output reinstates
  // exactly the pipeline stall the per-frame glGetError was removed for.
  glDisable(kGlDebugOutputSynchronous);
  glEnable(kGlDebugOutput);

  GlDebugMessageControlProc debug_message_control = nullptr;
  if (ResolveOpenGlFunction("glDebugMessageControl", &debug_message_control)) {
    debug_message_control(kGlDontCare, kGlDontCare, kGlDontCare, 0, nullptr,
                          GL_TRUE);
  }

  debug_message_callback(&GlideGlDebugTrampoline, this);
  glide_gl_error_policy_.debug_output_installed = true;
  return true;
}

void GlideOpenGlBackend::RunGlErrorFrameCheck() {
  if (glide_gl_error_frame_interval_ == 0U) {
    return;
  }
  ++glide_gl_error_frame_counter_;
  if (glide_gl_error_frame_counter_ < glide_gl_error_frame_interval_) {
    return;
  }
  glide_gl_error_frame_counter_ = 0;
  std::uint32_t drain_iterations = 0;
  std::uint32_t first_error_code = 0;
  for (GLenum code = glGetError(); code != GL_NO_ERROR; code = glGetError()) {
    ++drain_iterations;
    if (first_error_code == 0U) {
      first_error_code = static_cast<std::uint32_t>(code);
    }
    // A stuck context would otherwise spin here forever.
    if (drain_iterations >= 32U) {
      break;
    }
  }
  RecordGlideGlErrorFrameCheck(&glide_gl_error_policy_, first_error_code,
                               drain_iterations);
}

bool GlideOpenGlBackend::CheckGlErrorIfEnabled() {
  // Short-circuits before the call, not after it: the cost being removed is
  // the driver round trip inside `glGetError`, not the comparison.
  return !GlideGlErrorCheckEnabled() || glGetError() == GL_NO_ERROR;
}

void GlideOpenGlBackend::InvokeOnHostThread(std::function<void()> command) {
  const bool timing =
      GlideGateTimingEnabled() || active_ordinal_timing_ != nullptr;
  if (IsHostThread()) {
    const std::uint64_t start = timing ? ReadGlideGateTimingCycles() : 0U;
    command();
    if (timing) {
      const std::uint64_t finish = ReadGlideGateTimingCycles();
      const std::uint64_t cycles =
          GlideGateTimingDelta(&glide_gate_timing_, start, finish);
      RecordGlideGateDirectCommand(&glide_gate_timing_, cycles);
      RecordGlideOrdinalDirectWork(active_ordinal_timing_, active_ordinal_,
                                   cycles);
    }
    return;
  }

  RefuseCrossThreadCommandOnOneThread();
  const std::uint64_t enter = timing ? ReadGlideGateTimingCycles() : 0U;
  std::exception_ptr command_exception;
  // Task 431: from here until this function returns, the guest thread runs no
  // guest code, so none of the AOT timer safe points can be reached and any
  // tick coming due is coalesced away. Published for the poll thread to
  // attribute those losses. Scoped because the tail rethrows.
  struct GateOccupancyScope {
    std::atomic<bool> *flag;
    explicit GateOccupancyScope(std::atomic<bool> *f) : flag(f) {
      flag->store(true, std::memory_order_relaxed);
    }
    ~GateOccupancyScope() { flag->store(false, std::memory_order_relaxed); }
  } gate_occupancy_scope(&guest_in_glide_gate_);
  // Task 419: spin for the previous command to drain before taking the lock.
  // Only contended when the guest outruns the host, so usually a single read.
  SpinForRendezvousHint(host_command_pending_hint_, false, true);
  std::unique_lock<std::mutex> lock(host_command_mutex_);
  host_command_cv_.wait(lock, [this]() { return !host_command_pending_; });
  host_command_ = std::move(command);
  host_command_exception_ = nullptr;
  host_command_complete_ = false;
  host_command_pending_ = true;
  host_command_complete_hint_.store(false, std::memory_order_release);
  host_command_pending_hint_.store(true, std::memory_order_release);
  if (timing) {
    // Published under the lock the host takes before reading it, so the
    // timestamp is visible to the host without an atomic.
    RecordGlideGatePublish(&glide_gate_timing_, enter,
                           ReadGlideGateTimingCycles());
  }
  host_command_cv_.notify_all();
  // Task 419: this is the `complete` interval, 30.5% of gate time in Task
  // 418's measurement — the cost of the OS waking this thread back up. Spin
  // through it with the lock released, then re-take it and let the original
  // predicate decide, so a missed hint costs nothing but the spin.
  if (rendezvous_spin_microseconds_ != 0U ||
      RendezvousSpinMicroseconds() != 0U) {
    lock.unlock();
    SpinForRendezvousHint(host_command_complete_hint_, true, true);
    lock.lock();
  }
  host_command_cv_.wait(lock, [this]() { return host_command_complete_; });
  // The pump drains the queue before it touches the synchronous slot, so a
  // completed synchronous command also proves every command posted before it
  // has run. Nothing further is needed to order the two paths.
  if (timing) {
    const std::uint64_t resume = ReadGlideGateTimingCycles();
    RecordGlideGateResume(&glide_gate_timing_, enter, resume);
    RecordGlideOrdinalRendezvous(active_ordinal_timing_, active_ordinal_, enter,
                                 glide_gate_timing_.publish_cycles,
                                 glide_gate_timing_.host_start_cycles,
                                 glide_gate_timing_.host_finish_cycles, resume);
  }
  command_exception = host_command_exception_;
  lock.unlock();
  if (command_exception != nullptr) {
    std::rethrow_exception(command_exception);
  }
}

// Task 440: the asynchronous half of the host channel. Shares the mutex and the
// condition variable with the synchronous slot on purpose -- "drain the queue,
// then run the sync command" has to be one ordered decision, and two locks
// would make it two racing ones.
bool GlideOpenGlBackend::PostToHostThread(std::function<void()> command,
                                          const bool swap_command) {
  if (IsHostThread()) {
    // Already where the command must run, so posting would only defer it
    // behind work this same thread is responsible for pumping.
    command();
    std::lock_guard<std::mutex> lock(host_command_mutex_);
    ++async_present().executed_count;
    return true;
  }
  RefuseCrossThreadCommandOnOneThread();
  std::unique_lock<std::mutex> lock(host_command_mutex_);
  async_present().enabled.store(true, std::memory_order_relaxed);
  const auto has_room = [this, swap_command]() {
    return async_present().commands.size() < kGlideAsyncCommandCapacity &&
           (!swap_command ||
            pending_swap_count_.load(std::memory_order_relaxed) <
                kGlideMaxOutstandingSwaps);
  };
  if (!has_room()) {
    // The bound reached: this is the back pressure that keeps the guest from
    // running more than a frame ahead, and it is exactly what
    // `grBufferNumPending` reports to the game.
    ++async_present().back_pressure_count;
    host_command_cv_.wait(lock, [this, &has_room]() {
      return has_room() ||
             host_stopped_pumping_.load(std::memory_order_acquire);
    });
  }
  if (host_stopped_pumping_.load(std::memory_order_acquire)) {
    // Dropping is the only alternative to deadlocking against a host that
    // will never pump again. Counted so the loss is visible.
    ++async_present().refused_count;
    return false;
  }
  async_present().commands.push_back(std::move(command));
  queued_command_count_.fetch_add(1U, std::memory_order_relaxed);
  ++async_present().posted_count;
  if (swap_command) {
    ++async_present().posted_swap_count;
    pending_swap_count_.fetch_add(1U, std::memory_order_relaxed);
  }
  const std::uint32_t depth =
      static_cast<std::uint32_t>(async_present().commands.size());
  if (depth > async_present().max_queue_depth.load(std::memory_order_relaxed)) {
    async_present().max_queue_depth.store(depth, std::memory_order_relaxed);
  }
  host_command_pending_hint_.store(true, std::memory_order_release);
  lock.unlock();
  host_command_cv_.notify_all();
  return true;
}

bool GlideOpenGlBackend::PostBufferSwap(const std::uint32_t swap_interval) {
  return PostToHostThread(
      [this, swap_interval]() {
        const bool swapped = BufferSwapOnHostThread(swap_interval, true);
        {
          std::lock_guard<std::mutex> lock(host_command_mutex_);
          if (pending_swap_count_.load(std::memory_order_relaxed) != 0U) {
            pending_swap_count_.fetch_sub(1U, std::memory_order_relaxed);
          }
          if (!swapped) {
            ++async_present().failure_count;
          }
        }
        // Task 750: a guest waiting the swap out is on this variable.
        host_command_cv_.notify_all();
      },
      true);
}

bool GlideOpenGlBackend::WaitForPendingSwaps(
    const std::chrono::microseconds timeout) {
  const auto done = [this]() {
    return pending_swap_count_.load(std::memory_order_relaxed) == 0U ||
           host_stopped_pumping_.load(std::memory_order_acquire);
  };
  std::unique_lock<std::mutex> lock(host_command_mutex_);
  if (timeout.count() <= 0) {
    return done();
  }
  return host_command_cv_.wait_for(lock, timeout, done);
}

bool GlideOpenGlBackend::PostBufferClear(const std::uint32_t color,
                                         const std::uint32_t alpha,
                                         const std::uint32_t depth) {
  return PostToHostThread(
      [this, color, alpha, depth]() {
        if (!BufferClear(color, alpha, depth)) {
          std::lock_guard<std::mutex> lock(host_command_mutex_);
          ++async_present().failure_count;
        }
      },
      false);
}

bool GlideOpenGlBackend::PostDrawPrimitiveBatch(
    const hle::GlideDrawVertex *vertices, const std::size_t vertex_count,
    const GlideBatchPrimitive primitive) {
  if (vertices == nullptr || vertex_count == 0U) {
    return true;
  }
  // The command owns its vertices: the guest refills its batch the moment this
  // returns, so borrowing the storage would draw whatever it holds later.
  std::vector<hle::GlideDrawVertex> owned(vertices, vertices + vertex_count);
  return PostToHostThread(
      [this, owned = std::move(owned), primitive]() {
        if (!DrawPrimitiveBatch(owned.data(), owned.size(), primitive)) {
          std::lock_guard<std::mutex> lock(host_command_mutex_);
          ++async_present().failure_count;
        }
      },
      false);
}

std::uint32_t GlideOpenGlBackend::glide_pending_swap_count() const {
  // A plain atomic load: the guest calls this once per frame on its own
  // stack, so it must not reach the lazily-created state behind it.
  return pending_swap_count_.load(std::memory_order_relaxed);
}

GlideAsyncPresentSnapshot GlideOpenGlBackend::glide_async_present() const {
  GlideAsyncPresentSnapshot snapshot;
  const auto &state = async_present();
  const auto load64 = [](const std::atomic<std::uint64_t> &value) {
    return value.load(std::memory_order_relaxed);
  };
  snapshot.enabled = state.enabled.load(std::memory_order_relaxed);
  snapshot.posted_count = load64(state.posted_count);
  snapshot.posted_swap_count = load64(state.posted_swap_count);
  snapshot.executed_count = load64(state.executed_count);
  snapshot.failure_count = load64(state.failure_count);
  snapshot.back_pressure_count = load64(state.back_pressure_count);
  snapshot.refused_count = load64(state.refused_count);
  snapshot.pending_swap_count =
      pending_swap_count_.load(std::memory_order_relaxed);
  snapshot.max_queue_depth =
      state.max_queue_depth.load(std::memory_order_relaxed);
  return snapshot;
}

void GlideOpenGlBackend::PumpHostCommands() {
  if (!IsHostThread()) {
    return;
  }

  const bool timing = GlideGateTimingEnabled();
  // Task 440: the queue is drained first and completely. That single ordering
  // rule is what lets a posted command and a waited-on one share a channel:
  // both run here, on this thread, in the order they were submitted.
  DrainAsyncCommands();
  std::function<void()> command;
  {
    std::lock_guard<std::mutex> lock(host_command_mutex_);
    if (!host_command_pending_) {
      return;
    }
    command = host_command_;
  }

  const std::uint64_t start = timing ? ReadGlideGateTimingCycles() : 0U;
  std::exception_ptr command_exception;
  try {
    command();
  } catch (...) {
    command_exception = std::current_exception();
  }

  {
    std::lock_guard<std::mutex> lock(host_command_mutex_);
    if (timing) {
      RecordGlideGateHostCommand(&glide_gate_timing_, start,
                                 ReadGlideGateTimingCycles());
    }
    host_command_ = {};
    host_command_exception_ = command_exception;
    host_command_pending_ = false;
    host_command_complete_ = true;
    // Task 419: published under the same lock as the flags they mirror, so
    // a guest spinning outside the lock sees them in the same order.
    host_command_pending_hint_.store(false, std::memory_order_release);
    host_command_complete_hint_.store(true, std::memory_order_release);
  }
  host_command_cv_.notify_all();
}

// Task 333: the host thread used to sleep unconditionally between polls, so a
// guest command published just after a pump waited out the whole sleep before
// anyone looked at it. Waiting on the same condition variable keeps the poll
// cadence (the timeout is the old sleep) while making publication wake the host
// immediately.
bool GlideOpenGlBackend::WaitAndPumpHostCommands(
    std::uint32_t timeout_milliseconds) {
  if (!IsHostThread()) {
    return false;
  }
  // Task 419: the `wake` interval, 35.2% of gate time — the OS waking this
  // thread after the guest publishes. Spin outside the lock first; the wait
  // below still decides, so a missed hint only costs the spin.
  SpinForRendezvousHint(host_command_pending_hint_, true, false);
  {
    std::unique_lock<std::mutex> lock(host_command_mutex_);
    // Task 440: posted work wakes the host exactly like a published
    // synchronous command, so both are in the predicate. Read through the
    // atomic depth so an unused channel never allocates its state here.
    const auto has_work = [this]() {
      return host_command_pending_ ||
             queued_command_count_.load(std::memory_order_acquire) != 0U;
    };
    if (!has_work()) {
      host_command_cv_.wait_for(
          lock, std::chrono::milliseconds(timeout_milliseconds), has_work);
    }
    if (!has_work()) {
      return false;
    }
  }
  PumpHostCommands();
  return true;
}

bool GlideOpenGlBackend::ApplyWindowScale(std::uint32_t scale) {
  if (host_window_ == nullptr || scale < 1U || scale > 4U ||
      logical_width_ >
          static_cast<std::uint32_t>(std::numeric_limits<int>::max()) / scale ||
      logical_height_ >
          static_cast<std::uint32_t>(std::numeric_limits<int>::max()) / scale) {
    return false;
  }

  // Task 769: a fullscreen window has no size of its own to scale.
  if (host_window_->IsFullscreen()) {
    return false;
  }
  const int width = static_cast<int>(logical_width_ * scale);
  const int height = static_cast<int>(logical_height_ * scale);
  if (!host_window_->SetSize(width, height)) {
    message_ = std::string("SDL3 window scale failed: ") + SDL_GetError();
    return false;
  }
  window_scale_ = scale;
  ApplyDrawableViewport();
  std::ostringstream stream;
  stream << "SDL3 window scale " << scale << "x (" << width << "x" << height
         << ")";
  message_ = stream.str();
  fprintf(stderr, "[repiu-live-debug] %s\n", message_.c_str());
  return true;
}

void GlideOpenGlBackend::ApplyDrawableViewport() {
  if (host_window_ == nullptr || dummy_mode_) {
    return;
  }
  int drawable_width = 0;
  int drawable_height = 0;
  if (!host_window_->DrawableSize(&drawable_width, &drawable_height) ||
      drawable_width <= 0 || drawable_height <= 0) {
    return;
  }
  // Task 769: the picture keeps the guest's aspect ratio inside the
  // drawable; the bars around it are cleared at every present. Issue #45:
  // unless keep-aspect is off, when it fills the drawable.
  drawable_width_ = static_cast<std::uint32_t>(drawable_width);
  drawable_height_ = static_cast<std::uint32_t>(drawable_height);
  if (osd_ != nullptr) {
    // Issue #76: an overlay with no SDL window lays out in the drawable.
    osd_->SetDisplaySize(static_cast<float>(drawable_width),
                         static_cast<float>(drawable_height));
  }
  content_rect_ =
      ComputeGlidePictureRect(logical_width_, logical_height_,
                              drawable_width_, drawable_height_, keep_aspect_);
  // Issue #69: the game draws into the offscreen pair, which is the picture
  // rectangle's size, so its viewport starts at the origin; the rectangle's
  // place in the window is the present's business.
  std::string resize_message;
  if (!framebuffers_.Resize(content_rect_.width, content_rect_.height,
                            &resize_message) &&
      !resize_message.empty()) {
    fprintf(stderr, "[repiu-glide] %s\n", resize_message.c_str());
  }
  glViewport(0, 0, static_cast<GLsizei>(content_rect_.width),
             static_cast<GLsizei>(content_rect_.height));
  glScissor(0, 0, static_cast<GLsizei>(content_rect_.width),
            static_cast<GLsizei>(content_rect_.height));
  // Issue #69: the range is the context's, read once when it was made; this
  // runs at every grClipWindow, and a query there is a round trip in WebGL.
  const float supported_minimum = std::max(1.0F, point_size_range_[0]);
  const float supported_maximum =
      std::max(supported_minimum, point_size_range_[1]);
  const float requested_size = CalculateGlidePointSize(
      logical_width_, logical_height_, content_rect_.width,
      content_rect_.height);
  point_size_ =
      std::clamp(requested_size, supported_minimum, supported_maximum);
}

void GlideOpenGlBackend::ToggleFullscreen() {
  if (host_window_ == nullptr || dummy_mode_) {
    return;
  }
  // Issue #45: an operator's toggle, so the new state is stored.
  SetFullscreen(!IsFullscreen(), true);
}

bool GlideOpenGlBackend::IsFullscreen() const {
  return host_window_ != nullptr && host_window_->IsFullscreen();
}

bool GlideOpenGlBackend::SetFullscreen(bool fullscreen, bool report) {
  if (host_window_ == nullptr || dummy_mode_) {
    return false;
  }
  // No fullscreen mode is ever set on this window, so SDL3 makes this a
  // borderless window at the desktop resolution: the display mode never
  // changes. The resulting pixel-size event re-runs ApplyDrawableViewport.
  if (!host_window_->SetFullscreen(fullscreen)) {
    fprintf(stderr, "[repiu-glide] fullscreen toggle failed: %s\n",
            SDL_GetError());
    return false;
  }
  fprintf(stderr, "[repiu-glide] %s\n",
          fullscreen ? "fullscreen (desktop resolution)" : "windowed mode");
  // Issue #45: the requested value, not the window flag read back -- on
  // X11 and Wayland the change lands asynchronously.
  if (report) {
    ReportSessionDisplayPreferences(fullscreen, keep_aspect_);
  }
  return true;
}

void GlideOpenGlBackend::ApplyPendingDisplayOptions() {
  if (!display_change_pending_) {
    return;
  }
  display_change_pending_ = false;
  bool changed = false;
  if (pending_keep_aspect_ != keep_aspect_) {
    keep_aspect_ = pending_keep_aspect_;
    ApplyDrawableViewport();
    fprintf(stderr, "[repiu-glide] keep aspect ratio %s\n",
            keep_aspect_ ? "on" : "off (stretched to the window)");
    changed = true;
  }
  bool fullscreen = IsFullscreen();
  if (pending_fullscreen_ != fullscreen &&
      SetFullscreen(pending_fullscreen_, false)) {
    fullscreen = pending_fullscreen_;
    changed = true;
  }
  // One report carries both values, whichever of them changed.
  if (changed) {
    ReportSessionDisplayPreferences(fullscreen, keep_aspect_);
  }
}

void GlideOpenGlBackend::ApplyProjection() {
  // Glide vertices arrive in screen pixel coordinates. Honor the origin
  // grSstWinOpen actually asked for: GR_ORIGIN_UPPER_LEFT (0) needs a
  // y-flipped projection, while GR_ORIGIN_LOWER_LEFT (1) matches OpenGL's
  // default orientation. Hardcoding the flipped form renders the whole scene
  // upside down for a lower-left guest. Culling is disabled by the game
  // (grCullMode 0), so the resulting winding difference is harmless.
  //
  // Issue #69: the matrix glOrtho(0, w, bottom, top, -1, 1) built, column
  // major, handed to the shader in place of the fixed-function stack.
  const float width = static_cast<float>(logical_width_);
  const float height = static_cast<float>(logical_height_);
  const float bottom = origin_lower_left_ ? 0.0F : height;
  const float top = origin_lower_left_ ? height : 0.0F;
  const float matrix[16] = {2.0F / width,
                            0.0F,
                            0.0F,
                            0.0F,
                            0.0F,
                            2.0F / (top - bottom),
                            0.0F,
                            0.0F,
                            0.0F,
                            0.0F,
                            -1.0F,
                            0.0F,
                            -1.0F,
                            -(top + bottom) / (top - bottom),
                            0.0F,
                            1.0F};
  std::memcpy(projection_, matrix, sizeof(projection_));
  shader_.SetProjection(projection_);
}

void GlideOpenGlBackend::RestoreGameGlState() {
  const auto set = [](GLenum capability, bool enabled) {
    if (enabled) {
      glEnable(capability);
    } else {
      glDisable(capability);
    }
  };
  framebuffers_.BindDrawTarget();
  glViewport(0, 0, static_cast<GLsizei>(framebuffers_.width()),
             static_cast<GLsizei>(framebuffers_.height()));
  glScissor(0, 0, static_cast<GLsizei>(framebuffers_.width()),
            static_cast<GLsizei>(framebuffers_.height()));
  set(GL_DEPTH_TEST, game_gl_state_.depth_test);
  set(GL_BLEND, game_gl_state_.blend);
  set(GL_CULL_FACE, game_gl_state_.cull_face);
  set(GL_SCISSOR_TEST, game_gl_state_.scissor_test);
  const bool *const mask = game_gl_state_.color_mask;
  glColorMask(mask[0] ? GL_TRUE : GL_FALSE, mask[1] ? GL_TRUE : GL_FALSE,
              mask[2] ? GL_TRUE : GL_FALSE, mask[3] ? GL_TRUE : GL_FALSE);
  shader_.Use();
  gl_.active_texture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D,
                current_texture_ != nullptr ? current_texture_->gl_name : 0U);
  vertex_stream_.Bind();
}

std::string
GlideOpenGlBackend::BuildWindowTitle(double frames_per_second) const {
  std::ostringstream stream;
  stream.imbue(std::locale::classic());
  // Task 738. The build identity sits beside the version: a screenshot or a
  // report then says which binary it came from.
  stream << "rePIU v" REPIU_VERSION " ("
         << repiu::platform::BuildIdentityLabel() << ") - Build " __DATE__
         << " - Glide 2 OpenGL ["
         << runtime::ExecutionBackendDisplayName(execution_backend_)
         << "] - FPS : " << std::fixed << std::setprecision(1)
         << frames_per_second;
  return stream.str();
}

void GlideOpenGlBackend::ResetFrameRateMeasurement() {
  frame_rate_period_start_ = {};
  frame_rate_frame_count_ = 0;
  // Task 509: resolved here rather than per frame. This runs when the window
  // opens or is rebuilt, which is often enough and never on the frame path.
  //
  // The presented-frame total is deliberately *not* reset: a window rebuilt
  // mid-run does not un-draw the frames before it, and the total exists to say
  // what the run drew.
  log_frame_rate_ = std::getenv("REPIU_GLIDE_FRAME_RATE_LOG") != nullptr;
}

std::uint64_t GlideOpenGlBackend::presented_frame_total() const {
  return presented_frame_total_;
}

bool GlideOpenGlBackend::TakeFrameHold(
    std::chrono::steady_clock::time_point* const until) {
  if (!frame_hold_pending_) {
    return false;
  }
  frame_hold_pending_ = false;
  if (until != nullptr) {
    *until = frame_hold_until_;
  }
  return true;
}

std::uint64_t GlideOpenGlBackend::presented_frame_span_milliseconds() const {
  if (presented_frame_total_ == 0) {
    return 0;
  }
  const auto elapsed = std::chrono::steady_clock::now() - presented_frame_first_;
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count());
}

void GlideOpenGlBackend::RecordPresentedFrame() {
  const auto now = std::chrono::steady_clock::now();
  // Task 509: the run's own total, kept apart from the one-second window below.
  if (presented_frame_total_ == 0) {
    presented_frame_first_ = now;
  }
  ++presented_frame_total_;
  if (frame_rate_frame_count_ == 0) {
    frame_rate_period_start_ = now;
    frame_rate_frame_count_ = 1;
    return;
  }

  ++frame_rate_frame_count_;
  const double elapsed_seconds =
      std::chrono::duration<double>(now - frame_rate_period_start_).count();
  if (elapsed_seconds < 1.0) {
    return;
  }

  const double frames_per_second =
      static_cast<double>(frame_rate_frame_count_ - 1) / elapsed_seconds;
  // Issue #88: the OSD's graph, kept while the overlay is closed too.
  if (osd_ != nullptr) {
    osd_->RecordFrameRate(static_cast<float>(frames_per_second),
                          elapsed_seconds);
  }
  // Task 509: the same value the title carries, on the log, when asked for.
  //
  // One average over a run cannot tell "uniformly slow" from "collapses
  // somewhere", and this port has twice been misled by a period longer than the
  // observation window. A value per second separates them from one run.
  if (log_frame_rate_) {
    fprintf(stderr, "[repiu-frame-rate] fps=%.1f frames=%llu span_ms=%llu\n",
            frames_per_second,
            static_cast<unsigned long long>(presented_frame_total_),
            static_cast<unsigned long long>(
                presented_frame_span_milliseconds()));
  }
  const std::string window_title = BuildWindowTitle(frames_per_second);
  if (host_window_ != nullptr && !host_window_->SetTitle(window_title.c_str())) {
    fprintf(stderr, "[repiu-live-debug] SDL3 window title update failed: %s\n",
            SDL_GetError());
  }
  frame_rate_period_start_ = now;
  frame_rate_frame_count_ = 1;
}

bool GlideOpenGlBackend::OpenWindowed(std::uint32_t logical_width,
                                      std::uint32_t logical_height,
                                      std::uint32_t color_buffer_count,
                                      std::uint32_t auxiliary_buffer_count,
                                      std::uint32_t origin) {
  if (!IsHostThread()) {
    bool result = false;
    InvokeOnHostThread([this, logical_width, logical_height, color_buffer_count,
                        auxiliary_buffer_count, origin, &result]() {
      result = OpenWindowed(logical_width, logical_height, color_buffer_count,
                            auxiliary_buffer_count, origin);
    });
    return result;
  }
  Close();
  // Task 440: `Close` above marks the asynchronous channel shut so teardown
  // cannot deadlock. Opening a window means the host is pumping again, so the
  // channel reopens here -- without this, every later post is refused and the
  // frames it carried are silently dropped.
  host_stopped_pumping_.store(false, std::memory_order_release);
  exit_requested_ = false;
  dummy_mode_ = false;
  origin_lower_left_ = origin == repiu::hle::kGlideOriginLowerLeft;
  if (logical_width == 0 || logical_height == 0 || color_buffer_count < 2U) {
    message_ = "invalid Glide windowed framebuffer request";
    return false;
  }

  if (logical_width >
          static_cast<std::uint32_t>(std::numeric_limits<int>::max()) /
              window_scale_ ||
      logical_height >
          static_cast<std::uint32_t>(std::numeric_limits<int>::max()) /
              window_scale_) {
    message_ = "scaled Glide window dimensions are too large";
    return false;
  }
  const int window_width = static_cast<int>(logical_width * window_scale_);
  const int window_height = static_cast<int>(logical_height * window_scale_);

  glide_swap_interval_policy_.wsl_d3d12_selected =
      repiu::platform::SelectWslD3d12Driver();
  // Issue #72: SDL's window, or the browser page's canvas in a Worker.
  {
    std::unique_ptr<GlideHostWindow> window = CreateGlideHostWindow();
    const std::string window_title = BuildWindowTitle(0.0);
    std::string stage;
    std::string detail;
    if (!window->Open(window_title.c_str(), window_width, window_height,
                      &stage, &detail)) {
      dummy_mode_ = true;
      logical_width_ = logical_width;
      logical_height_ = logical_height;
      message_ = "Glide dummy fallback (" + stage + "): " + detail;
      return true;
    }
    host_window_ = std::move(window);
  }
  {
    std::string missing;
    if (!LoadGlApi(&gl_, &missing)) {
      dummy_mode_ = true;
      logical_width_ = logical_width;
      logical_height_ = logical_height;
      message_ = "Glide dummy fallback (OpenGL entry point missing: " +
                 missing + ")";
      host_window_.reset();
      return true;
    }
  }
  GlRendererIdentity renderer_identity;
  {
    // Task 752: say what draws. A software renderer here is why a frame's
    // present takes 11 ms where the GPU takes 5.
    const char *const renderer =
        reinterpret_cast<const char *>(glGetString(GL_RENDERER));
    std::snprintf(glide_swap_interval_policy_.gl_renderer,
                  sizeof(glide_swap_interval_policy_.gl_renderer), "%s",
                  renderer != nullptr ? renderer : "unknown");
    fprintf(stderr, "[repiu-glide] GL renderer: %s%s" "\n",
            glide_swap_interval_policy_.gl_renderer,
            glide_swap_interval_policy_.wsl_d3d12_selected
                ? " (Mesa D3D12 chosen for WSL)"
                : "");
    // #5: the rest of what the OSD shows. The line above stays as it is;
    // scripts look for it.
    renderer_identity = MakeGlRendererIdentity(
        renderer, reinterpret_cast<const char *>(glGetString(GL_VENDOR)),
        reinterpret_cast<const char *>(glGetString(GL_VERSION)),
        host_window_->VideoDriver().c_str(),
        glide_swap_interval_policy_.wsl_d3d12_selected);
    fprintf(stderr,
            "[repiu-glide] GL vendor/version/video driver/software: "
            "%s/%s/%s/%s\n",
            renderer_identity.vendor.c_str(),
            renderer_identity.version.c_str(),
            renderer_identity.video_driver.c_str(),
            renderer_identity.software ? "true" : "false");
  }

  // Task 761: the presentation toggle's initial value, then the OSD that can
  // flip it. On by default: unset and empty mean on, and only an explicit
  // `0`, `off` or `false` keeps the 565 decode path.
  lfb_high_precision_.store(repiu::runtime::ResolvePromotedToggle(
                                std::getenv("REPIU_GLIDE_LFB_HIGH_PRECISION")),
                            std::memory_order_relaxed);
  // Issue #37: the same rule for the full-precision texture option, applied
  // before any texture is uploaded so nothing needs re-uploading.
  texture_full_precision_.store(
      repiu::runtime::ResolvePromotedToggle(
          std::getenv("REPIU_GLIDE_TEXTURE_FULL_PRECISION")),
      std::memory_order_relaxed);
  texture_full_precision_applied_ =
      texture_full_precision_.load(std::memory_order_relaxed);
  // Issue #45: keep-aspect is on unless REPIU_GLIDE_KEEP_ASPECT says 0;
  // the loader publishes cfg/repiu.ini's value there. Read before the first
  // ApplyDrawableViewport below.
  keep_aspect_ = repiu::runtime::ResolvePromotedToggle(
      std::getenv("REPIU_GLIDE_KEEP_ASPECT"));
  display_change_pending_ = false;
  osd_ = std::make_unique<GlideOsd>();
  {
    std::string osd_message;
    if (!osd_->Initialize(host_window_->SdlWindow(),
                          host_window_->SdlContext(),
                          ImGuiGlslVersion(gl_.es), &osd_message)) {
      fprintf(stderr, "[repiu-osd] disabled: %s\n", osd_message.c_str());
      osd_.reset();
    } else {
      osd_->SetRendererIdentity(renderer_identity);
      // #15: the overlay opens the way re2DJ's does, with the first half of
      // the window title and the target profile.
      osd_->SetInfoLines(
          {std::string("rePIU v" REPIU_VERSION " (") +
               repiu::platform::BuildIdentityLabel() + ") - Build " __DATE__,
           "Target Profile : " + SessionTargetProfile()});
    }
  }
  // Task 768: the post-processing pass and its initial shader. `none` when
  // the variable is unset; an unknown or broken shader also runs as `none`,
  // with the reason logged and shown in the OSD.
  post_process_ = std::make_unique<GlidePostProcess>();
  {
    std::string post_message;
    if (post_process_->Initialize(&post_message)) {
      const char *const requested = std::getenv(kPostShaderVariable);
      if (requested != nullptr && requested[0] != '\0') {
        post_process_->Select(requested);
      }
    } else {
      fprintf(stderr, "[repiu-post] disabled: %s\n", post_message.c_str());
      post_process_.reset();
    }
  }
  // Task 748: a scripted keyboard, if one was asked for.
  input_script_ = std::make_unique<SdlInputScriptPlayerHandle>();
  if (!input_script_->player.StartFromEnvironment(
          host_window_->SdlWindowId())) {
    input_script_.reset();
  }
  if (jamma_input_timeline_ != nullptr) {
    jamma_input_timeline_->ServeLiveReadsFromLatestState(input_script_ !=
                                                         nullptr);
  }
  logical_width_ = logical_width;
  logical_height_ = logical_height;
  // Task 433: the depth buffer is only made when the guest asked for an
  // auxiliary buffer, so report what was asked for and what there is. Issue
  // #69: it is the offscreen pair's depth24 renderbuffer, which a complete
  // framebuffer always has.
  const int granted_depth_bits = auxiliary_buffer_count != 0U ? 24 : 0;
  fprintf(stderr,
          "[repiu-live-debug] Glide depth buffer color-buffers/aux-buffers/"
          "requested-bits/granted-bits: %u/%u/%d/%d\n",
          color_buffer_count, auxiliary_buffer_count,
          auxiliary_buffer_count != 0U ? 24 : 0, granted_depth_bits);
  // Task 371 made the interval a measurement-only override. Task 766: the
  // interval is now always requested -- the variable when it is set, vsync on
  // otherwise -- because the default left to SDL or the driver differs by
  // host (SDL's Wayland backend runs uncapped unless asked).
  std::int32_t requested_interval = 0;
  glide_swap_interval_policy_.interval_requested = true;
  glide_swap_interval_policy_.override_requested =
      ReadGlideSwapInterval(&requested_interval);
  glide_swap_interval_policy_.requested_interval = requested_interval;
  glide_swap_interval_policy_.applied =
      host_window_->SetSwapInterval(static_cast<int>(requested_interval));
  int effective = 0;
  glide_swap_interval_policy_.effective_valid =
      host_window_->SwapInterval(&effective);
  glide_swap_interval_policy_.effective_interval =
      static_cast<std::int32_t>(effective);
  // Task 745. A refused interval used to leave the game unthrottled with
  // only a line in the final report to say so: WSLg's llvmpipe GL
  // advertises GLX_EXT_swap_control and then refuses the call. The engine
  // then paces the swaps itself at the display's refresh rate.
  if (!glide_swap_interval_policy_.applied) {
    const char *const error = SDL_GetError();
    std::strncpy(glide_swap_interval_policy_.failure,
                 error != nullptr ? error : "",
                 sizeof(glide_swap_interval_policy_.failure) - 1U);
  }
  glide_swap_interval_policy_.refresh_rate_hz = host_window_->RefreshRateHz();
  const bool honoured = glide_swap_interval_policy_.applied &&
      (!glide_swap_interval_policy_.effective_valid ||
       glide_swap_interval_policy_.effective_interval == requested_interval);
  const std::uint32_t pacing_period_us =
      ResolveGlideSwapPacingPeriodMicroseconds(
          requested_interval, glide_swap_interval_policy_.refresh_rate_hz);
  // Issue #87: a window that leaves the wait to the host loop (a browser
  // canvas) is paced the same way, the loop waiting for each deadline.
  swap_wait_deferred_ = host_window_->DefersSwapWait();
  frame_hold_pending_ = false;
  if ((!honoured || swap_wait_deferred_) && pacing_period_us != 0U) {
    glide_swap_interval_policy_.pacing_active = true;
    glide_swap_interval_policy_.pacing_period_us = pacing_period_us;
    swap_pacing_enabled_ = true;
    swap_pacing_period_ = std::chrono::microseconds(pacing_period_us);
    swap_pacing_deadline_valid_ = false;
    if (swap_wait_deferred_) {
      fprintf(stderr,
              "[repiu-glide-swap] the window leaves the swap wait to the "
              "host loop; holding frames every %u us\n",
              static_cast<unsigned>(pacing_period_us));
    } else {
      fprintf(stderr,
              "[repiu-glide-swap] driver refused swap interval %d (%s); "
              "pacing swaps every %u us (display %.2f Hz)\n",
              static_cast<int>(requested_interval),
              glide_swap_interval_policy_.failure,
              static_cast<unsigned>(pacing_period_us),
              glide_swap_interval_policy_.refresh_rate_hz);
    }
  } else {
    swap_wait_deferred_ = false;
  }
  // Task 370: prefer asynchronous reporting. When the driver provides it the
  // frame check is removed entirely; otherwise it falls back to sampling. An
  // explicit interval wins over both so a run can force either behaviour.
  const bool debug_output_installed = InstallGlDebugOutput();
  glide_gl_error_frame_interval_ =
      debug_output_installed ? 0U : kDefaultGlideGlErrorFrameInterval;
  std::uint32_t configured_interval = 0;
  if (TryReadGlideGlErrorFrameInterval(&configured_interval)) {
    glide_gl_error_frame_interval_ = configured_interval;
  }
  // Issue #69: the combine program, the vertex stream that replaced
  // immediate mode, and the offscreen pair the game draws into. Any of them
  // missing leaves nothing to draw with, so the window falls back to the
  // dummy with the reason.
  std::string renderer_failure;
  if (!shader_.Initialize()) {
    renderer_failure = shader_.message();
  } else if (!vertex_stream_.Initialize(&gl_)) {
    renderer_failure = "vertex stream unavailable";
  } else if (!framebuffers_.Initialize(&gl_, auxiliary_buffer_count != 0U,
                                       &renderer_failure)) {
    renderer_failure = "offscreen framebuffers: " + renderer_failure;
  }
  if (!renderer_failure.empty()) {
    fprintf(stderr,
            "[repiu-glide] renderer unavailable, falling back to dummy "
            "mode: %s\n",
            renderer_failure.c_str());
    Close();
    dummy_mode_ = true;
    logical_width_ = logical_width;
    logical_height_ = logical_height;
    message_ = "Glide dummy fallback activated (" + renderer_failure + ")";
    return true;
  }
  if (!gl_.es) {
    // Desktop core takes the point size from gl_PointSize only when asked;
    // ES always does.
    glEnable(GL_PROGRAM_POINT_SIZE);
  }
  glGetFloatv(GL_ALIASED_POINT_SIZE_RANGE, point_size_range_);
  ApplyDrawableViewport();
  if (!framebuffers_.ready()) {
    fprintf(stderr,
            "[repiu-glide] renderer unavailable, falling back to dummy "
            "mode: offscreen framebuffers could not be made\n");
    Close();
    dummy_mode_ = true;
    logical_width_ = logical_width;
    logical_height_ = logical_height;
    message_ = "Glide dummy fallback activated (no offscreen framebuffers)";
    return true;
  }
  ApplyProjection();
  // The window starts black; the offscreen pair was cleared when it was made.
  gl_.bind_framebuffer(GL_FRAMEBUFFER, 0U);
  glClearColor(0.0F, 0.0F, 0.0F, 1.0F);
  glClear(GL_COLOR_BUFFER_BIT);
  host_window_->Present();
  framebuffers_.BindDrawTarget();
  // Issue #45: the stored fullscreen choice, entered after the window has
  // its scaled size so leaving fullscreen returns to that window. Applying
  // the stored value is not an operator's change, so nothing is reported.
  if (repiu::runtime::ResolveOptInToggle(
          std::getenv("REPIU_GLIDE_FULLSCREEN"))) {
    SetFullscreen(true, false);
  }
  if (!keep_aspect_) {
    fprintf(stderr, "[repiu-glide] keep aspect ratio off (stretched to the "
                    "window)\n");
  }
  ResetFrameRateMeasurement();
  std::ostringstream stream;
  stream << logical_width << "x" << logical_height
         << " logical Glide window opened at " << window_scale_ << "x ("
         << window_width << "x" << window_height << ")";
  message_ = stream.str();
  return true;
}

void GlideOpenGlBackend::PumpEvents() {
  if (!IsHostThread()) {
    InvokeOnHostThread([this]() { PumpEvents(); });
    return;
  }
  if (dummy_mode_ || host_window_ == nullptr) {
    return;
  }
  // Issue #64: on one thread the script has no thread of its own.
  if (input_script_ != nullptr) {
    input_script_->player.Pump();
  }
  // Task 754: the two clocks, compared a second at a time.
  glide_swap_interval_policy_.event_clock_steady = EventClockUsesSteady();
  if (host_clock_divergence_.Sample(SDL_GetTicksNS(),
                                    SteadyClockNanoseconds())) {
    fprintf(stderr,
            "[repiu-clock] the steady clock ran %.2f%% %s than SDL's raw "
            "clock over the last second: the system is slewing its "
            "monotonic clock (time synchronisation). Timer ticks follow "
            "the %s clock; audio follows the steady clock\n",
            static_cast<double>(
                host_clock_divergence_.snapshot().worst_window_ppm < 0
                    ? -host_clock_divergence_.snapshot().worst_window_ppm
                    : host_clock_divergence_.snapshot().worst_window_ppm) /
                10000.0,
            host_clock_divergence_.snapshot().worst_window_ppm < 0
                ? "faster"
                : "slower",
            EventClockUsesSteady() ? "steady" : "raw");
  }
  {
    const HostClockDivergenceSnapshot clocks =
        host_clock_divergence_.snapshot();
    glide_swap_interval_policy_.clock_windows = clocks.windows;
    glide_swap_interval_policy_.clock_windows_over_limit =
        clocks.windows_over_limit;
    glide_swap_interval_policy_.clock_total_ppm = clocks.total_ppm;
    glide_swap_interval_policy_.clock_worst_window_ppm =
        clocks.worst_window_ppm;
  }
  // Issue #37: an OSD change to the texture option takes effect here.
  ApplyTextureFullPrecision();
  // Issue #45: and an OSD change to fullscreen or keep-aspect.
  ApplyPendingDisplayOptions();
  // Issue #34: opened here, on the thread that pumps events, so device events
  // and the state they update stay on one thread. A failure leaves input
  // keyboard only and is not retried.
  if (pad_input_ == nullptr) {
    pad_input_ = std::make_unique<SdlPadInput>();
    pad_input_->Initialize();
  }
  // Issue #76: what the browser page sent becomes SDL events here, after the
  // gamepad subsystem is up (its virtual gamepads need it) and before the
  // events are polled, so the path below handles them as a window's.
  if (browser_input_ == nullptr) {
    browser_input_ = std::make_unique<BrowserInputBridge>();
  }
  browser_input_->Pump();
  SDL_Event event{};
  while (SDL_PollEvent(&event)) {
    // Task 761: Tab belongs to the OSD alone; it never reaches game input.
    // (It was F1 first, which is TEST's default key and kept TEST from the
    // game.)
    // Everything else is forwarded to ImGui while the overlay is open so its
    // checkbox can be clicked, and still flows to the game below, because the
    // cabinet's controls must never go dead behind an overlay.
    if ((event.type == SDL_EVENT_KEY_DOWN || event.type == SDL_EVENT_KEY_UP) &&
        event.key.key == SDLK_TAB) {
      if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat &&
          osd_ != nullptr) {
        osd_->ToggleVisible();
      }
      continue;
    }
    // Task 769: Alt+Enter toggles fullscreen and, like Tab, never reaches
    // game input; plain Enter still does.
    if ((event.type == SDL_EVENT_KEY_DOWN || event.type == SDL_EVENT_KEY_UP) &&
        (event.key.key == SDLK_RETURN || event.key.key == SDLK_KP_ENTER) &&
        (event.key.mod & SDL_KMOD_ALT) != 0) {
      if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat) {
        ToggleFullscreen();
      }
      continue;
    }
    // A left double click toggles too, unless it landed on the OSD.
    if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN &&
        event.button.button == SDL_BUTTON_LEFT && event.button.clicks == 2U &&
        (osd_ == nullptr || !osd_->WantsMouse())) {
      ToggleFullscreen();
    }
    if (osd_ != nullptr && osd_->visible()) {
      osd_->ProcessEvent(&event);
    }
    // Issue #34: a pad change becomes edges (RecordPadMaskChange).
    if (pad_input_ != nullptr) {
      const SdlPadInput::Change change = pad_input_->HandleEvent(event);
      if (change.handled) {
        RecordPadMaskChange(change.before, change.after,
                            EventTimestampNanoseconds(event.common.timestamp));
        continue;
      }
    }
    if (event.type == SDL_EVENT_KEY_DOWN || event.type == SDL_EVENT_KEY_UP) {
      HandleSdlBiosKeyboardEvent(event.key, bios_keyboard_);
    }
    if ((event.type == SDL_EVENT_KEY_DOWN || event.type == SDL_EVENT_KEY_UP) &&
        !event.key.repeat && jamma_input_timeline_ != nullptr) {
      if (event.type == SDL_EVENT_KEY_DOWN) {
        JammaInputKey key;
        if (FindJammaInputForPress(event.key.key, event.key.mod, &key)) {
          jamma_input_timeline_->RecordKeyEdge(
              EventTimestampNanoseconds(event.key.timestamp), key, true);
        }
      } else {
        // Issue #34: an input a pad still holds stays pressed.
        const std::uint16_t released = static_cast<std::uint16_t>(
            JammaInputMaskForKeycode(event.key.key) &
            ~(pad_input_ != nullptr ? pad_input_->pressed_mask() : 0U));
        const std::uint64_t released_at =
            EventTimestampNanoseconds(event.key.timestamp);
        for (std::uint32_t index = 0;
             index < repiu::input::kJammaInputKeyCount; ++index) {
          const auto key = static_cast<JammaInputKey>(index);
          if ((released & JammaInputKeyMask(key)) != 0U) {
            jamma_input_timeline_->RecordKeyEdge(released_at, key, false);
          }
        }
      }
    } else if (event.type == SDL_EVENT_WINDOW_FOCUS_LOST &&
               jamma_input_timeline_ != nullptr) {
      jamma_input_timeline_->RecordAllReleased(
          EventTimestampNanoseconds(event.common.timestamp));
    }
    // Issue #34: SDL sends no pad events to a background window, so pads let
    // go with the focus as keys do.
    if (event.type == SDL_EVENT_WINDOW_FOCUS_LOST && pad_input_ != nullptr) {
      pad_input_->ReleaseAll();
    }
    if (event.type == SDL_EVENT_WINDOW_FOCUS_LOST) {
      HandleSdlBiosKeyboardFocusLost(bios_keyboard_);
    }
    if (event.type == SDL_EVENT_QUIT ||
        event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
      exit_requested_ = true;
      message_ = "SDL3 host exit requested";
      break;
    } else if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat &&
               (event.key.mod & SDL_KMOD_ALT) != 0) {
      std::uint32_t scale = 0;
      switch (event.key.key) {
      case SDLK_1:
        scale = 1U;
        break;
      case SDLK_2:
        scale = 2U;
        break;
      case SDLK_3:
        scale = 3U;
        break;
      case SDLK_4:
        scale = 4U;
        break;
      default:
        break;
      }
      if (scale != 0U) {
        ApplyWindowScale(scale);
      }
    } else if (event.type == SDL_EVENT_WINDOW_RESIZED ||
               event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) {
      ApplyDrawableViewport();
    }
  }
  // Issue #52: LT+RT+L3+R3 held on one pad for a second ends the run the way
  // closing the window does. Checked every pump rather than per event, since
  // a held chord sends none.
  if (pad_input_ != nullptr &&
      pad_exit_chord_.Update(input::IsPadExitChordDown(pad_input_->state()),
                             SDL_GetTicks())) {
    fprintf(stderr, "[repiu-pad] exit chord held for 1 s: exit requested\n");
    exit_requested_ = true;
  }
  // Issue #55: LT+RT+Y opens and closes the OSD, and while it is open (by
  // either way) the pad drives it and the game sees no pad input.
  if (pad_input_ != nullptr && osd_ != nullptr) {
    if (pad_osd_chord_.Update(input::IsPadOsdChordDown(pad_input_->state()))) {
      osd_->ToggleVisible();
    }
    const bool osd_open = osd_->visible();
    if (osd_open != pad_input_->game_input_suppressed()) {
      const SdlPadInput::Change change =
          pad_input_->SetGameInputSuppressed(osd_open);
      RecordPadMaskChange(change.before, change.after,
                          EventTimestampNanoseconds(SDL_GetTicksNS()));
      fprintf(stderr, "[repiu-pad] OSD %s: pad input %s the game\n",
              osd_open ? "open" : "closed",
              osd_open ? "kept from" : "back to");
    }
  }
}

// Issue #34: a pad change becomes edges for exactly the inputs whose pad state
// changed. A release is held back while the keyboard still holds the same
// input, so letting go of one source never drops an input the other is
// pressing. Issue #55 moved it here so a change from the OSD's suppression is
// recorded the same way as one from an event.
void GlideOpenGlBackend::RecordPadMaskChange(std::uint16_t before,
                                             std::uint16_t after,
                                             std::uint64_t changed_at) {
  const std::uint16_t changed = static_cast<std::uint16_t>(before ^ after);
  if (changed == 0U || jamma_input_timeline_ == nullptr) {
    return;
  }
  const std::uint16_t keyboard = CaptureKeyboardJammaPressedMask();
  for (std::uint32_t index = 0; index < repiu::input::kJammaInputKeyCount;
       ++index) {
    const auto key = static_cast<JammaInputKey>(index);
    const std::uint16_t bit = JammaInputKeyMask(key);
    if ((changed & bit) == 0U) {
      continue;
    }
    const bool pressed = (after & bit) != 0U;
    if (!pressed && (keyboard & bit) != 0U) {
      continue;
    }
    jamma_input_timeline_->RecordKeyEdge(changed_at, key, pressed);
  }
}

bool GlideOpenGlBackend::BufferClear(std::uint32_t color, std::uint32_t alpha,
                                     std::uint32_t depth) {
  if (!IsHostThread()) {
    bool result = false;
    InvokeOnHostThread([this, color, alpha, depth, &result]() {
      result = BufferClear(color, alpha, depth);
    });
    return result;
  }
  if (!is_open()) {
    message_ = "cannot clear Glide buffer without an OpenGL window";
    return false;
  }
  if (dummy_mode_) {
    return true;
  }
  const float r = static_cast<float>((color >> 16) & 0xFF) / 255.0f;
  const float g = static_cast<float>((color >> 8) & 0xFF) / 255.0f;
  const float b = static_cast<float>(color & 0xFF) / 255.0f;
  const float a = static_cast<float>(alpha & 0xFF) / 255.0f;
  const float d = static_cast<float>(depth) / 65535.0f;
  glClearColor(r, g, b, a);
  gl_.ClearDepth(d);
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
  message_ = "Glide buffer cleared";
  return true;
}

bool GlideOpenGlBackend::BufferSwap(std::uint32_t swap_interval) {
  if (!IsHostThread()) {
    bool result = false;
    InvokeOnHostThread([this, swap_interval, &result]() {
      result = BufferSwapOnHostThread(swap_interval, true);
    });
    return result;
  }
  return BufferSwapOnHostThread(swap_interval, false);
}

// Task 745. Deadlines accumulate from one swap to the next so the average
// rate is exact; a deadline more than a period behind (a stall, a load) is
// resynchronised to now rather than paid back in a burst. The last ~300 us
// are spun because the sleep's wake-up is late by about that much.
void GlideOpenGlBackend::PaceSwapAfterPresent(
    std::chrono::steady_clock::time_point present_begin,
    std::chrono::steady_clock::time_point present_end) {
  if (!swap_pacing_enabled_) {
    return;
  }
  const auto now = present_end;
  // Task 752. This used to resynchronise whenever the frame was late at all
  // (`deadline + period < now` compares against this frame's own deadline)
  // and then slept a whole period from now, so a frame 1 ms over its period
  // became a 33 ms frame and a screen whose frames take 17 ms ran at half
  // rate. A late frame is now simply not held; the deadlines go on
  // accumulating so the frames after it make the time up, and only a frame
  // more than a period late starts the schedule again, from now.
  const auto deadline = swap_pacing_deadline_ + swap_pacing_period_;
  if (!swap_pacing_deadline_valid_ || deadline + swap_pacing_period_ < now) {
    if (swap_pacing_deadline_valid_) {
      ++glide_swap_interval_policy_.resyncs;
    }
    swap_pacing_deadline_ = now;
    swap_pacing_deadline_valid_ = true;
  } else {
    swap_pacing_deadline_ = deadline;
  }
  if (swap_pacing_deadline_ <= now) {
    ++glide_swap_interval_policy_.late_swaps;
    const auto late = std::chrono::duration_cast<std::chrono::microseconds>(
                          now - swap_pacing_deadline_)
                          .count();
    if (static_cast<std::uint64_t>(late) >
        glide_swap_interval_policy_.late_max_us) {
      glide_swap_interval_policy_.late_max_us = static_cast<std::uint64_t>(late);
    }
  }
  // Issue #87: the host loop waits; the wait it reports is not counted here.
  if (swap_wait_deferred_) {
    if (swap_pacing_deadline_ > now) {
      frame_hold_until_ = swap_pacing_deadline_;
      frame_hold_pending_ = true;
    }
    ++glide_swap_interval_policy_.paced_swaps;
    return;
  }
  if (swap_pacing_deadline_ > now) {
    constexpr auto kSpinMargin = std::chrono::microseconds(300);
    if (swap_pacing_deadline_ - now > kSpinMargin) {
      const auto wake_target = swap_pacing_deadline_ - kSpinMargin;
      std::this_thread::sleep_until(wake_target);
      const auto woke = std::chrono::steady_clock::now();
      if (woke > wake_target) {
        const auto over = std::chrono::duration_cast<std::chrono::microseconds>(
                              woke - wake_target)
                              .count();
        if (over > 1000) {
          ++glide_swap_interval_policy_.oversleep_over_1ms;
        }
        if (static_cast<std::uint64_t>(over) >
            glide_swap_interval_policy_.oversleep_max_us) {
          glide_swap_interval_policy_.oversleep_max_us =
              static_cast<std::uint64_t>(over);
        }
      }
    }
    while (std::chrono::steady_clock::now() < swap_pacing_deadline_) {
    }
    glide_swap_interval_policy_.paced_sleep_us += static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - now)
            .count());
  }
  ++glide_swap_interval_policy_.paced_swaps;
}

// Task 748. On stderr, where the 20 ms MP3 census already is, so what
// happened inside the frame can be read off the same time axis. A frame is
// the previous pace's sleep, the guest's share up to the next swap and the
// present; the three are printed so the reader can tell the guest (a load,
// a decode) from the display (a slow present, a compositor).
void GlideOpenGlBackend::LogLongFrame(
    std::chrono::steady_clock::time_point present_begin,
    std::chrono::steady_clock::time_point present_end) {
  // `1` names every frame over one and a half periods (a missed vblank);
  // a larger value is the threshold itself in microseconds.
  static const long long long_frame_threshold_us = [] {
    const char* value = std::getenv("REPIU_GLIDE_LONG_FRAME_LOG");
    if (value == nullptr) {
      return -1LL;
    }
    const long long parsed = std::strtoll(value, nullptr, 10);
    return parsed > 1 ? parsed : 0LL;
  }();
  if (long_frame_threshold_us < 0 || !swap_last_present_valid_) {
    return;
  }
  const auto period = swap_pacing_enabled_
                          ? swap_pacing_period_
                          : std::chrono::steady_clock::duration(
                                std::chrono::microseconds(16667));
  const auto threshold =
      long_frame_threshold_us == 0
          ? period * 3 / 2
          : std::chrono::steady_clock::duration(
                std::chrono::microseconds(long_frame_threshold_us));
  const auto frame = present_end - swap_last_present_end_;
  if (frame <= threshold) {
    return;
  }
  const auto us = [](std::chrono::steady_clock::duration d) {
    return static_cast<long long>(
        std::chrono::duration_cast<std::chrono::microseconds>(d).count());
  };
  const auto since_first = std::chrono::duration_cast<std::chrono::milliseconds>(
                               present_end - presented_frame_first_)
                               .count();
  fprintf(stderr,
          "[repiu-glide-swap] long-frame elapsed_ms=%lld frame=%llu"
          " frame_us=%lld sleep_us=%lld guest_us=%lld present_us=%lld\n",
          static_cast<long long>(since_first),
          static_cast<unsigned long long>(presented_frame_total_), us(frame),
          us(swap_last_return_ - swap_last_present_end_),
          us(present_begin - swap_last_return_), us(present_end - present_begin));
}

namespace {

// Issue #69: the swap diagnostics' RGB samples, read from the back buffer as
// RGBA, the one format ES guarantees, and packed down.
std::vector<unsigned char> ReadBackBufferRgb(const GlideFramebufferPair &pair,
                                             int width, int height) {
  const std::size_t count = static_cast<std::size_t>(width) * height;
  std::vector<unsigned char> rgba(count * 4U);
  std::vector<unsigned char> rgb(count * 3U);
  // A picture smaller than the logical screen (a tiny window) is not
  // sampled: a clipped read would change the row stride.
  if (static_cast<std::uint32_t>(width) > pair.width() ||
      static_cast<std::uint32_t>(height) > pair.height() ||
      !pair.ReadBack(0U, 0U, static_cast<std::uint32_t>(width),
                     static_cast<std::uint32_t>(height), rgba.data())) {
    return rgb;
  }
  for (std::size_t index = 0U; index < count; ++index) {
    std::memcpy(rgb.data() + index * 3U, rgba.data() + index * 4U, 3U);
  }
  return rgb;
}

} // namespace

bool GlideOpenGlBackend::BufferSwapOnHostThread(std::uint32_t swap_interval,
                                                bool guest_gate_command) {
  if (!is_open()) {
    message_ = "cannot swap Glide buffer without an OpenGL window";
    return false;
  }
  if (dummy_mode_) {
    return true;
  }
  GlideBufferSwapTimingProfile *swap_timing =
      guest_gate_command && GlideBufferSwapTimingProfileEnabled()
          ? &glide_buffer_swap_timing_
          : nullptr;
  const std::uint64_t entry_cycles =
      swap_timing != nullptr ? ReadGlideBufferSwapTimingCycles() : 0U;
  if (swap_timing != nullptr && swap_timing->sdl_interval_query_count == 0U) {
    int interval = 0;
    const bool queried = host_window_->SwapInterval(&interval);
    RecordGlideBufferSwapSdlInterval(swap_timing, queried,
                                     static_cast<std::int32_t>(interval));
  }
  // Optional verification diagnostic (off by default): in a headless session
  // the GL window is not screenshot-able, so sampling the back buffer for
  // non-black pixels proves geometry is rasterized (the black clear is the
  // discriminator). Enable with REPIU_GLIDE_PIXEL_DIAG; sampling is bounded so
  // it never adds steady-state glReadPixels cost. Used to verify Task 254.
  static const bool pixel_diagnostic_enabled =
      std::getenv("REPIU_GLIDE_PIXEL_DIAG") != nullptr;
  static std::atomic<long> swap_diag_count{0};
  const long swap_index = swap_diag_count.fetch_add(1) + 1;
  // Task 719. With REPIU_GLIDE_PIXEL_DIAG_INTERVAL_MS the sample is taken by
  // time rather than by swap number, because the hosts swap at different rates
  // and the same swap number is not the same moment on both. Each sample also
  // carries a 4x4 brightness grid, which tells apart scenes whose average
  // colour is close. The swap-numbered output below is unchanged without it.
  static const long scene_interval_ms = [] {
    const char *const text = std::getenv("REPIU_GLIDE_PIXEL_DIAG_INTERVAL_MS");
    return text == nullptr ? 0L : std::max(0L, std::strtol(text, nullptr, 10));
  }();
  if (scene_interval_ms > 0 && logical_width_ > 0 && logical_height_ > 0) {
    static const auto first_swap = std::chrono::steady_clock::now();
    static long next_sample_ms = 0;
    const long elapsed_ms = static_cast<long>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - first_swap)
            .count());
    if (elapsed_ms >= next_sample_ms) {
      next_sample_ms =
          elapsed_ms - elapsed_ms % scene_interval_ms + scene_interval_ms;
      const int width = static_cast<int>(logical_width_);
      const int height = static_cast<int>(logical_height_);
      // Task 769: from the picture's corner, not the letterbox bar's --
      // issue #69: the back buffer's origin.
      const std::vector<unsigned char> pixels =
          ReadBackBufferRgb(framebuffers_, width, height);
      std::uint64_t cell_luma[16] = {};
      std::uint64_t cell_count[16] = {};
      std::size_t non_black = 0;
      std::uint64_t sum[3] = {};
      for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
          const unsigned char *const pixel =
              pixels.data() + (static_cast<std::size_t>(y) * width + x) * 3U;
          const int cell = (y * 4 / height) * 4 + x * 4 / width;
          cell_luma[cell] += (pixel[0] * 299U + pixel[1] * 587U +
                              pixel[2] * 114U) / 1000U;
          ++cell_count[cell];
          if (pixel[0] > 8U || pixel[1] > 8U || pixel[2] > 8U) {
            ++non_black;
            sum[0] += pixel[0];
            sum[1] += pixel[1];
            sum[2] += pixel[2];
          }
        }
      }
      char grid[17] = {};
      for (int cell = 0; cell < 16; ++cell) {
        const std::uint64_t mean =
            cell_count[cell] == 0U ? 0U : cell_luma[cell] / cell_count[cell];
        grid[cell] = "0123456789ABCDEF"[std::min<std::uint64_t>(mean / 16U, 15U)];
      }
      const std::size_t denom = non_black == 0 ? 1U : non_black;
      fprintf(stderr,
              "[repiu-scene] t_ms=%ld swap=%ld non_black=%zu avg=%llu,%llu,%llu"
              " grid=%s\n",
              elapsed_ms, swap_index, non_black,
              static_cast<unsigned long long>(sum[0] / denom),
              static_cast<unsigned long long>(sum[1] / denom),
              static_cast<unsigned long long>(sum[2] / denom), grid);
    }
  }
  if (pixel_diagnostic_enabled && scene_interval_ms == 0 &&
      (swap_index <= 60 || swap_index % 200 == 0) &&
      swap_index <= 4000 && logical_width_ > 0 && logical_height_ > 0) {
    const int width = static_cast<int>(logical_width_);
    const int height = static_cast<int>(logical_height_);
    const std::vector<unsigned char> pixels =
        ReadBackBufferRgb(framebuffers_, width, height);
    std::size_t non_black = 0;
    std::uint64_t sum_r = 0;
    std::uint64_t sum_g = 0;
    std::uint64_t sum_b = 0;
    for (std::size_t i = 0; i + 2U < pixels.size(); i += 3U) {
      if (pixels[i] > 8U || pixels[i + 1U] > 8U || pixels[i + 2U] > 8U) {
        ++non_black;
        sum_r += pixels[i];
        sum_g += pixels[i + 1U];
        sum_b += pixels[i + 2U];
      }
    }
    const std::size_t denom = non_black == 0 ? 1U : non_black;
    fprintf(stderr,
            "[repiu-live-debug] Glide swap #%ld non-black pixels=%zu/%d"
            " avg-rgb=%llu,%llu,%llu\n",
            swap_index, non_black, width * height,
            static_cast<unsigned long long>(sum_r / denom),
            static_cast<unsigned long long>(sum_g / denom),
            static_cast<unsigned long long>(sum_b / denom));
  }
  // Issue #69: Glide's page flip. Back becomes front and is drawn into the
  // window: the letterbox bars cleared black every frame (Task 769), the
  // picture through the post-process shader when one is active (Task 768) --
  // after the diagnostics above sampled it unprocessed and before the OSD,
  // which must stay legible. The game's GL state is put back after the swap.
  framebuffers_.Exchange();
  framebuffers_.Present(drawable_width_, drawable_height_, content_rect_,
                        post_process_.get(), logical_width_, logical_height_);
  // Task 761: the OSD overlays the finished frame in the window's
  // framebuffer; ImGui's GL3 renderer saves and restores the GL state it
  // touches.
  if (osd_ != nullptr) {
    // Issue #45: the OSD shows the current display options and leaves a
    // change for the next event pump.
    GlideOsdDisplayOptions display;
    display.fullscreen = IsFullscreen();
    display.keep_aspect = keep_aspect_;
    osd_->Render(&lfb_high_precision_, &texture_full_precision_,
                 post_process_.get(), &display);
    if (display.fullscreen_changed || display.keep_aspect_changed) {
      display_change_pending_ = true;
      pending_fullscreen_ = display.fullscreen;
      pending_keep_aspect_ = display.keep_aspect;
    }
  }
  const std::uint64_t present_start_cycles =
      swap_timing != nullptr ? ReadGlideBufferSwapTimingCycles() : 0U;
  const auto present_begin = std::chrono::steady_clock::now();
  const bool swapped = host_window_->Present();
  const auto present_end = std::chrono::steady_clock::now();
  RestoreGameGlState();
  const std::uint64_t present_end_cycles =
      swap_timing != nullptr ? ReadGlideBufferSwapTimingCycles() : 0U;
  if (!swapped) {
    message_ = std::string("SDL3 Glide buffer swap failed: ") + SDL_GetError();
    if (swap_timing != nullptr) {
      const std::uint64_t finish_cycles = ReadGlideBufferSwapTimingCycles();
      RecordGlideBufferSwapTiming(
          swap_timing, swap_interval, false, entry_cycles, present_start_cycles,
          present_end_cycles, present_end_cycles, finish_cycles);
    }
    return false;
  }
  RecordPresentedFrame();
  LogLongFrame(present_begin, present_end);
  PaceSwapAfterPresent(present_begin, present_end);
  swap_last_present_end_ = present_end;
  swap_last_present_valid_ = true;
  swap_last_return_ = std::chrono::steady_clock::now();
  // Task 370: this was a check on every frame in Task 369, on the assumption
  // that the present had already synchronised. It had not -- the swap queues
  // the flip in 44 microseconds without draining -- so the check became the
  // frame's only synchronisation point at 3.64 ms, 10.71% of wall. It is now
  // inert whenever the debug callback is installed, and sampled otherwise.
  RunGlErrorFrameCheck();
  const std::uint64_t accounting_end_cycles =
      swap_timing != nullptr ? ReadGlideBufferSwapTimingCycles() : 0U;
  message_ = "Glide buffer swapped";
  if (swap_timing != nullptr) {
    const std::uint64_t finish_cycles = ReadGlideBufferSwapTimingCycles();
    RecordGlideBufferSwapTiming(swap_timing, swap_interval, true, entry_cycles,
                                present_start_cycles, present_end_cycles,
                                accounting_end_cycles, finish_cycles);
  }
  return true;
}

// Task 420: `grDrawPoint` was accepted and discarded until now, so anything a
// title drew this way was missing from the screen with no error to show for it.
bool GlideOpenGlBackend::DrawPoint(const hle::GlideDrawVertex &a) {
  if (!IsHostThread()) {
    bool result = false;
    InvokeOnHostThread([this, &a, &result]() { result = DrawPoint(a); });
    return result;
  }
  const hle::GlideDrawVertex *const vertices[1] = {&a};
  return DrawPrimitive(vertices, 1U, GL_POINTS, "Glide point drawn");
}

// Task 420: Glide requires `grDrawPolygon`'s vertices to describe a convex
// polygon, which makes a triangle fan an exact rendering rather than an
// approximation. The caller has already bounded `count`.
bool GlideOpenGlBackend::DrawPolygon(const hle::GlideDrawVertex *vertices,
                                     const std::size_t count) {
  if (vertices == nullptr || count < 3U ||
      count > hle::kMaxGlidePolygonVertices) {
    message_ = "Glide polygon vertex count is out of range";
    return false;
  }
  if (!IsHostThread()) {
    bool result = false;
    InvokeOnHostThread([this, vertices, count, &result]() {
      result = DrawPolygon(vertices, count);
    });
    return result;
  }
  const hle::GlideDrawVertex *pointers[hle::kMaxGlidePolygonVertices] = {};
  for (std::size_t index = 0U; index < count; ++index) {
    pointers[index] = &vertices[index];
  }
  return DrawPrimitive(pointers, count, GL_TRIANGLE_FAN, "Glide polygon drawn");
}

bool GlideOpenGlBackend::DrawLine(const hle::GlideDrawVertex &a,
                                  const hle::GlideDrawVertex &b) {
  if (!IsHostThread()) {
    bool result = false;
    InvokeOnHostThread([this, &a, &b, &result]() { result = DrawLine(a, b); });
    return result;
  }
  const hle::GlideDrawVertex *const vertices[2] = {&a, &b};
  return DrawPrimitive(vertices, 2U, GL_LINES, "Glide line drawn");
}

bool GlideOpenGlBackend::DrawTriangle(const hle::GlideDrawVertex &a,
                                      const hle::GlideDrawVertex &b,
                                      const hle::GlideDrawVertex &c) {
  if (!IsHostThread()) {
    bool result = false;
    InvokeOnHostThread(
        [this, &a, &b, &c, &result]() { result = DrawTriangle(a, b, c); });
    return result;
  }
  const hle::GlideDrawVertex *const vertices[3] = {&a, &b, &c};
  return DrawPrimitive(vertices, 3U, GL_TRIANGLES,
                       "Glide compact triangle drawn");
}

bool GlideOpenGlBackend::PrepareDrawState(const std::uint32_t primitive,
                                          bool *sample_texture,
                                          float *inverse_width,
                                          float *inverse_height) {
  // R3: when the color combine selects the texture (SCALE_OTHER) and a texture
  // is currently sourced, bind it and sample; otherwise output iterated color.
  *sample_texture = texture_combine_enabled_ && current_texture_ != nullptr &&
                    current_texture_->gl_name != 0U;
  *inverse_width = 1.0F;
  *inverse_height = 1.0F;
  if (*sample_texture) {
    if (!RefreshCurrentPalettizedTexture()) {
      return false;
    }
    glBindTexture(GL_TEXTURE_2D, current_texture_->gl_name);
    // Task 332: normalize by the Glide coordinate extent, not the pixel
    // size. They are equal for every map whose longer edge is 256, which is
    // why only smaller sprites -- the difficulty dots and the arrows --
    // were wrong.
    *inverse_width = current_texture_->s_extent != 0U
                         ? 1.0F / static_cast<float>(current_texture_->s_extent)
                         : 1.0F;
    *inverse_height =
        current_texture_->t_extent != 0U
            ? 1.0F / static_cast<float>(current_texture_->t_extent)
            : 1.0F;
  }
  shader_.SetTextureEnabled(*sample_texture);
  if (primitive == GL_POINTS) {
    shader_.SetPointSize(point_size_);
  } else if (primitive == GL_LINES) {
    glLineWidth(1.0F);
  }
  return true;
}

GlideStreamVertex GlideOpenGlBackend::MakeStreamVertex(
    const hle::GlideDrawVertex &vertex, const bool sample_texture,
    const float inverse_width, const float inverse_height) {
  GlideStreamVertex out;
  out.r = vertex.r;
  out.g = vertex.g;
  out.b = vertex.b;
  out.a = vertex.a;
  if (sample_texture) {
    // Pack normalized sow/tow and the shared texture/fog oow. Because
    // orthographic clip w is constant, the shader receives linearly
    // interpolated numerators and reciprocal-w and performs the Glide
    // perspective divide per fragment.
    out.s = vertex.s * inverse_width;
    out.t = vertex.t * inverse_height;
    out.fog_oow = vertex.fog_oow;
    out.texture_oow = vertex.texture_oow;
  } else {
    out.s = 0.0F;
    out.t = 0.0F;
    out.fog_oow = vertex.fog_oow;
    out.texture_oow = 1.0F;
  }
  out.x = vertex.x;
  out.y = vertex.y;
  out.z = GlideOozToOrthoEyeZ(vertex.ooz);
  return out;
}

bool GlideOpenGlBackend::DrawPrimitive(
    const hle::GlideDrawVertex *const *vertices, const std::size_t vertex_count,
    const std::uint32_t primitive, const char *success_message) {
  if (!is_open()) {
    message_ = "cannot draw Glide primitive without an OpenGL window";
    return false;
  }
  if (dummy_mode_) {
    return true;
  }
  bool sample_texture = false;
  float inv_w = 1.0F;
  float inv_h = 1.0F;
  if (!PrepareDrawState(primitive, &sample_texture, &inv_w, &inv_h)) {
    return false;
  }
  stream_vertices_.clear();
  for (std::size_t index = 0U; index < vertex_count; ++index) {
    stream_vertices_.push_back(
        MakeStreamVertex(*vertices[index], sample_texture, inv_w, inv_h));
  }
  if (!vertex_stream_.Draw(static_cast<GLenum>(primitive),
                           stream_vertices_.data(), stream_vertices_.size())) {
    message_ = "Glide primitive did not fit the vertex stream";
    return false;
  }
  message_ = success_message;
  return true;
}

bool GlideOpenGlBackend::DrawPrimitiveBatch(
    const hle::GlideDrawVertex *vertices, const std::size_t vertex_count,
    const GlideBatchPrimitive primitive) {
  if (vertices == nullptr || vertex_count == 0U ||
      primitive == GlideBatchPrimitive::kNone) {
    // An empty flush is not a failure: the boundary flushes unconditionally
    // before every non-draw gate, and most of those find nothing pending.
    return true;
  }
  if (!IsHostThread()) {
    bool result = false;
    InvokeOnHostThread([this, vertices, vertex_count, primitive, &result]() {
      result = DrawPrimitiveBatch(vertices, vertex_count, primitive);
    });
    return result;
  }
  if (!is_open()) {
    message_ = "cannot draw Glide primitive without an OpenGL window";
    return false;
  }
  if (dummy_mode_) {
    return true;
  }
  const std::uint32_t gl_primitive =
      primitive == GlideBatchPrimitive::kPoints  ? GL_POINTS
      : primitive == GlideBatchPrimitive::kLines ? GL_LINES
                                                      : GL_TRIANGLES;
  bool sample_texture = false;
  float inv_w = 1.0F;
  float inv_h = 1.0F;
  if (!PrepareDrawState(gl_primitive, &sample_texture, &inv_w, &inv_h)) {
    return false;
  }
  // The whole batch shares one state application and one draw call, which is
  // exactly what makes it cheaper than the same primitives drawn separately.
  // The vertices themselves are emitted identically.
  stream_vertices_.clear();
  for (std::size_t index = 0U; index < vertex_count; ++index) {
    stream_vertices_.push_back(
        MakeStreamVertex(vertices[index], sample_texture, inv_w, inv_h));
  }
  if (!vertex_stream_.Draw(static_cast<GLenum>(gl_primitive),
                           stream_vertices_.data(), stream_vertices_.size())) {
    message_ = "Glide primitive batch did not fit the vertex stream";
    return false;
  }
  message_ = "Glide primitive batch drawn";
  return true;
}

bool GlideOpenGlBackend::StoreTexture(
    std::uint32_t start_address, std::uint32_t format, std::uint32_t large_lod,
    std::uint32_t aspect_ratio, const std::uint8_t *source,
    std::size_t source_size, const std::uint8_t *palette_rgba8,
    const FullPrecisionTexture *full_precision) {
  if (!IsHostThread()) {
    bool result = false;
    InvokeOnHostThread([this, start_address, format, large_lod, aspect_ratio,
                        source, source_size, palette_rgba8, full_precision,
                        &result]() {
      result = StoreTexture(start_address, format, large_lod, aspect_ratio,
                            source, source_size, palette_rgba8,
                            full_precision);
    });
    return result;
  }
  if (full_precision != nullptr) {
    RecordGlideTextureFullPrecision(&glide_texture_census_,
                                    full_precision->outcome);
  }
  // Task 375: the rejection paths are recorded too. A format or a geometry the
  // backend refuses is exactly the "texture silently missing from the screen"
  // case the census exists to catch, and reporting only the decode failures
  // would leave those invisible -- the same blind spot the census replaced.
  const auto record_rejection = [&](std::uint32_t rejected_format) {
    GlideTextureUpload rejected;
    rejected.start_address = start_address;
    rejected.format = rejected_format;
    rejected.large_lod = large_lod;
    rejected.aspect_ratio = aspect_ratio;
    rejected.source_size = static_cast<std::uint32_t>(source_size);
    rejected.has_palette = palette_rgba8 != nullptr;
    RecordGlideTextureUpload(&glide_texture_census_, rejected, nullptr, 0U);
  };
  if (!repiu::hle::IsGlideTextureFormatAcceptable(format)) {
    record_rejection(format);
    message_ = "unacceptable Glide texture format";
    return false;
  }
  if (!is_open() || dummy_mode_ || source == nullptr) {
    return false;
  }
  repiu::hle::GlideTextureDimensions dimensions;
  if (!repiu::hle::CalculateGlideTextureDimensions(large_lod, aspect_ratio,
                                                   &dimensions)) {
    record_rejection(format);
    message_ = "unsupported Glide texture dimensions";
    return false;
  }
  GlideTextureUpload census_upload;
  census_upload.start_address = start_address;
  census_upload.format = format;
  census_upload.large_lod = large_lod;
  census_upload.aspect_ratio = aspect_ratio;
  census_upload.width = dimensions.width;
  census_upload.height = dimensions.height;
  census_upload.source_size = static_cast<std::uint32_t>(source_size);
  census_upload.has_palette = palette_rgba8 != nullptr;
  repiu::hle::CalculateGlideTextureCoordinateExtent(
      aspect_ratio, &census_upload.s_extent, &census_upload.t_extent);

  std::vector<std::uint8_t> rgba8;
  if (!repiu::hle::DecodeGlideTextureToRgba8(
          format, dimensions.width, dimensions.height, source, source_size,
          palette_rgba8, &rgba8)) {
    RecordGlideTextureUpload(&glide_texture_census_, census_upload, nullptr,
                             0U);
    message_ = "unsupported Glide texture format";
    return false;
  }
  RecordGlideTextureUpload(&glide_texture_census_, census_upload, rgba8.data(),
                           rgba8.size());
  {
    static const std::filesystem::path dump_directory =
        GlideTextureDumpDirectory();
    if (!dump_directory.empty()) {
      if (glide_texture_census_.dump_written_count < GlideTextureDumpLimit()) {
        if (WriteGlideTextureDump(
                dump_directory, glide_texture_census_.dump_written_count,
                census_upload,
                HashGlideTexturePixels(rgba8.data(), rgba8.size()),
                rgba8.data(), rgba8.size())) {
          ++glide_texture_census_.dump_written_count;
        }
      } else {
        glide_texture_census_.dump_limit_reached = true;
      }
    }
  }
  static const bool tex_diagnostic_enabled =
      std::getenv("REPIU_GLIDE_TEX_DIAG") != nullptr;
  if (tex_diagnostic_enabled) {
    static std::atomic<long> store_diag_count{0};
    const long store_index = store_diag_count.fetch_add(1) + 1;
    if (store_index <= 16) {
      fprintf(stderr,
              "[repiu-live-debug] StoreTexture #%ld addr=0x%08X format=%u"
              " %ux%u texel0-rgba=%u,%u,%u,%u\n",
              store_index, start_address, format, dimensions.width,
              dimensions.height, rgba8[0], rgba8[1], rgba8[2], rgba8[3]);
    }
  }
  TextureEntry &entry = textures_[start_address];
  if (entry.gl_name == 0U) {
    GLuint name = 0;
    glGenTextures(1, &name);
    entry.gl_name = name;
  }
  entry.width = dimensions.width;
  entry.height = dimensions.height;
  entry.format = format;
  if (repiu::hle::IsGlidePalettizedTextureFormat(format)) {
    entry.source.assign(source, source + source_size);
    entry.palette_generation =
        palette_valid_ && palette_rgba8 != nullptr &&
                std::memcmp(palette_rgba8_.data(), palette_rgba8,
                            palette_rgba8_.size()) == 0
            ? palette_generation_
            : 0U;
  } else {
    entry.source.clear();
    entry.palette_generation = 0U;
  }
  // Task 332: the coordinate extent follows the aspect ratio alone, so it
  // equals the pixel size only when the longer edge is already 256.
  repiu::hle::CalculateGlideTextureCoordinateExtent(
      aspect_ratio, &entry.s_extent, &entry.t_extent);
  // Issue #37: keep both images when the original was found and verified, so
  // the option can switch between them later without the guest's help.
  // game_rgba8 could be rebuilt by re-truncating the original (that is what
  // the verification checked), halving this memory; see the issue #37 work
  // log.
  const bool has_full_precision =
      full_precision != nullptr &&
      full_precision->outcome == repiu::hle::MesaFxSourceOutcome::kUsed &&
      full_precision->rgba8.size() == rgba8.size();
  if (has_full_precision) {
    entry.game_rgba8 = rgba8;
    entry.full_precision_rgba8 = full_precision->rgba8;
  } else {
    entry.game_rgba8.clear();
    entry.full_precision_rgba8.clear();
  }
  const std::uint8_t *upload =
      has_full_precision && texture_full_precision_applied_
          ? entry.full_precision_rgba8.data()
          : rgba8.data();
  glBindTexture(GL_TEXTURE_2D, entry.gl_name);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA,
               static_cast<GLsizei>(dimensions.width),
               static_cast<GLsizei>(dimensions.height), 0, GL_RGBA,
               GL_UNSIGNED_BYTE, upload);
  message_ = "Glide texture stored";
  return glGetError() == GL_NO_ERROR;
}

void GlideOpenGlBackend::ApplyTextureFullPrecision() {
  const bool wanted =
      texture_full_precision_.load(std::memory_order_relaxed);
  if (wanted == texture_full_precision_applied_) {
    return;
  }
  texture_full_precision_applied_ = wanted;
  std::uint32_t uploaded = 0U;
  for (auto &[address, entry] : textures_) {
    (void)address;
    if (entry.gl_name == 0U || entry.full_precision_rgba8.empty() ||
        entry.game_rgba8.size() != entry.full_precision_rgba8.size()) {
      continue;
    }
    glBindTexture(GL_TEXTURE_2D, entry.gl_name);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0,
                    static_cast<GLsizei>(entry.width),
                    static_cast<GLsizei>(entry.height), GL_RGBA,
                    GL_UNSIGNED_BYTE,
                    wanted ? entry.full_precision_rgba8.data()
                           : entry.game_rgba8.data());
    ++uploaded;
  }
  if (current_texture_ != nullptr) {
    glBindTexture(GL_TEXTURE_2D, current_texture_->gl_name);
  }
  fprintf(stderr,
          "[repiu-glide] full-precision textures %s: %u re-uploaded\n",
          wanted ? "on" : "off", uploaded);
}

bool GlideOpenGlBackend::RefreshPalettizedTextures(
    const std::uint8_t *palette_rgba8) {
  if (!IsHostThread()) {
    bool result = false;
    InvokeOnHostThread([this, palette_rgba8, &result]() {
      result = RefreshPalettizedTextures(palette_rgba8);
    });
    return result;
  }
  if (palette_rgba8 == nullptr) {
    return false;
  }
  const bool identical =
      palette_valid_ && std::memcmp(palette_rgba8_.data(), palette_rgba8,
                                    palette_rgba8_.size()) == 0;
  RecordGlidePaletteDownload(&glide_texture_census_, identical);
  if (identical) {
    message_ = "identical Glide palette retained";
    return true;
  }
  std::memcpy(palette_rgba8_.data(), palette_rgba8, palette_rgba8_.size());
  palette_valid_ = true;
  ++palette_generation_;
  if (palette_generation_ == 0U) {
    palette_generation_ = 1U;
    for (auto &[address, entry] : textures_) {
      (void)address;
      entry.palette_generation = 0U;
    }
  }
  message_ = "Glide palette generation advanced";
  return true;
}

bool GlideOpenGlBackend::RefreshCurrentPalettizedTexture() {
  if (current_texture_ == nullptr ||
      !repiu::hle::IsGlidePalettizedTextureFormat(current_texture_->format) ||
      current_texture_->source.empty() || !palette_valid_ ||
      current_texture_->palette_generation == palette_generation_) {
    return true;
  }

  const auto decode_start = std::chrono::steady_clock::now();
  std::vector<std::uint8_t> rgba8;
  const bool decoded = repiu::hle::DecodeGlideTextureToRgba8(
      current_texture_->format, current_texture_->width,
      current_texture_->height, current_texture_->source.data(),
      current_texture_->source.size(), palette_rgba8_.data(), &rgba8);
  const auto decode_end = std::chrono::steady_clock::now();
  const std::uint64_t decode_nanoseconds = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(decode_end -
                                                           decode_start)
          .count());
  if (!decoded) {
    RecordGlidePaletteRefresh(&glide_texture_census_, false,
                              current_texture_->source.size(), 0U,
                              decode_nanoseconds, 0U);
    message_ = "Glide palette texture decode failed";
    return false;
  }

  const auto upload_start = std::chrono::steady_clock::now();
  glBindTexture(GL_TEXTURE_2D, current_texture_->gl_name);
  glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0,
                  static_cast<GLsizei>(current_texture_->width),
                  static_cast<GLsizei>(current_texture_->height), GL_RGBA,
                  GL_UNSIGNED_BYTE, rgba8.data());
  const bool uploaded = glGetError() == GL_NO_ERROR;
  const auto upload_end = std::chrono::steady_clock::now();
  const std::uint64_t upload_nanoseconds = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(upload_end -
                                                           upload_start)
          .count());
  RecordGlidePaletteRefresh(&glide_texture_census_, uploaded,
                            current_texture_->source.size(), rgba8.size(),
                            decode_nanoseconds, upload_nanoseconds);
  if (!uploaded) {
    message_ = "Glide palette texture upload failed";
    return false;
  }
  current_texture_->palette_generation = palette_generation_;
  message_ = "Glide palette texture lazily refreshed";
  return true;
}

bool GlideOpenGlBackend::SourceTexture(std::uint32_t start_address) {
  if (!IsHostThread()) {
    bool result = false;
    InvokeOnHostThread([this, start_address, &result]() {
      result = SourceTexture(start_address);
    });
    return result;
  }
  const auto found = textures_.find(start_address);
  if (found == textures_.end()) {
    // Task 332: record the miss. A miss leaves the draw untextured, which
    // looks like a missing sprite rather than a wrong one, so the address
    // is kept for the census to report.
    current_texture_ = nullptr;
    current_texture_address_ = 0U;
    last_missing_texture_address_ = start_address;
    ++missing_texture_source_count_;
    return false;
  }
  current_texture_ = &found->second;
  current_texture_address_ = start_address;
  glBindTexture(GL_TEXTURE_2D, current_texture_->gl_name);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                  tmu_min_filter_ == 1 ? GL_LINEAR : GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER,
                  tmu_mag_filter_ == 1 ? GL_LINEAR : GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,
                  tmu_s_clamp_ == 1 ? GL_CLAMP_TO_EDGE : GL_REPEAT);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T,
                  tmu_t_clamp_ == 1 ? GL_CLAMP_TO_EDGE : GL_REPEAT);
  return true;
}

void GlideOpenGlBackend::SetTextureClampMode(std::uint32_t s_clamp,
                                             std::uint32_t t_clamp) {
  if (!IsHostThread()) {
    InvokeOnHostThread(
        [this, s_clamp, t_clamp]() { SetTextureClampMode(s_clamp, t_clamp); });
    return;
  }
  tmu_s_clamp_ = s_clamp;
  tmu_t_clamp_ = t_clamp;
  if (is_open() && !dummy_mode_ && current_texture_ != nullptr &&
      current_texture_->gl_name != 0U) {
    glBindTexture(GL_TEXTURE_2D, current_texture_->gl_name);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,
                    tmu_s_clamp_ == 1 ? GL_CLAMP_TO_EDGE : GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T,
                    tmu_t_clamp_ == 1 ? GL_CLAMP_TO_EDGE : GL_REPEAT);
  }
}

void GlideOpenGlBackend::SetTextureFilterMode(std::uint32_t min_filter,
                                              std::uint32_t mag_filter) {
  if (!IsHostThread()) {
    InvokeOnHostThread([this, min_filter, mag_filter]() {
      SetTextureFilterMode(min_filter, mag_filter);
    });
    return;
  }
  tmu_min_filter_ = min_filter;
  tmu_mag_filter_ = mag_filter;
  if (is_open() && !dummy_mode_ && current_texture_ != nullptr &&
      current_texture_->gl_name != 0U) {
    glBindTexture(GL_TEXTURE_2D, current_texture_->gl_name);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                    tmu_min_filter_ == 1 ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER,
                    tmu_mag_filter_ == 1 ? GL_LINEAR : GL_NEAREST);
  }
}

void GlideOpenGlBackend::SetTextureCombineEnabled(bool enabled) {
  if (!IsHostThread()) {
    InvokeOnHostThread(
        [this, enabled]() { SetTextureCombineEnabled(enabled); });
    return;
  }
  texture_combine_enabled_ = enabled;
}

bool GlideOpenGlBackend::PresentLfbSurface(const std::uint8_t *rgba8,
                                           std::uint32_t width,
                                           std::uint32_t height, bool flip_v,
                                           bool present_to_front) {
  if (!IsHostThread()) {
    bool result = false;
    InvokeOnHostThread(
        [this, rgba8, width, height, flip_v, present_to_front, &result]() {
          result =
              PresentLfbSurface(rgba8, width, height, flip_v, present_to_front);
        });
    return result;
  }
  if (!is_open()) {
    message_ = "cannot present Glide LFB surface without an OpenGL window";
    return false;
  }
  if (dummy_mode_) {
    message_ = "Glide LFB surface presented (dummy)";
    return true;
  }
  if (rgba8 == nullptr || width == 0U || height == 0U) {
    message_ = "invalid Glide LFB surface";
    return false;
  }

  if (lfb_texture_ == 0U) {
    GLuint name = 0;
    glGenTextures(1, &name);
    lfb_texture_ = name;
  }
  glBindTexture(GL_TEXTURE_2D, lfb_texture_);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, static_cast<GLsizei>(width),
               static_cast<GLsizei>(height), 0, GL_RGBA, GL_UNSIGNED_BYTE,
               rgba8);

  // The blit must not inherit the geometry state (depth compare, blending,
  // culling, combine routing) the game set for its triangles, so force the
  // pieces that matter and restore them afterwards. It always lands in the
  // back buffer. The alpha test is the shader's, which blit mode bypasses.
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_BLEND);
  glDisable(GL_CULL_FACE);
  glDisable(GL_SCISSOR_TEST);
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  const bool drawing_to_front = framebuffers_.draw_target_front();
  if (drawing_to_front) {
    framebuffers_.SetDrawTargetFront(false);
  }
  shader_.SetBlitMode(true);

  // The orthographic projection installed at OpenWindowed already maps screen
  // pixels directly, so the quad spans the logical surface. Two independent
  // orientations decide where staging row 0 lands: the lock's origin
  // (`flip_v`) and the window projection's direction. Under a lower-left
  // projection, vertex y=0 is the bottom of the screen rather than the top,
  // which inverts the mapping again -- XOR them so the two never cancel out
  // silently.
  const bool invert_v = flip_v != origin_lower_left_;
  const float top_v = invert_v ? 1.0F : 0.0F;
  const float bottom_v = invert_v ? 0.0F : 1.0F;
  const float right = static_cast<float>(logical_width_);
  const float bottom = static_cast<float>(logical_height_);
  const auto corner = [](float x, float y, float v, float u) {
    GlideStreamVertex vertex;
    vertex.x = x;
    vertex.y = y;
    vertex.s = u;
    vertex.t = v;
    vertex.fog_oow = 0.0F;
    vertex.texture_oow = 1.0F;
    return vertex;
  };
  const GlideStreamVertex quad[6] = {
      corner(0.0F, 0.0F, top_v, 0.0F),     corner(right, 0.0F, top_v, 1.0F),
      corner(right, bottom, bottom_v, 1.0F), corner(0.0F, 0.0F, top_v, 0.0F),
      corner(right, bottom, bottom_v, 1.0F), corner(0.0F, bottom, bottom_v, 0.0F)};
  vertex_stream_.Draw(GL_TRIANGLES, quad, 6U);

  static std::atomic<long> lfb_diagnostic_count{0};
  const long diagnostic_index = lfb_diagnostic_count.fetch_add(1) + 1;
  if (std::getenv("REPIU_GLIDE_DRAW_DIAG") != nullptr &&
      diagnostic_index <= 4) {
    std::size_t input_non_black = 0U;
    std::size_t input_non_zero = 0U;
    std::uint8_t maximum_red = 0U;
    std::uint8_t maximum_green = 0U;
    std::uint8_t maximum_blue = 0U;
    for (std::size_t index = 0U;
         index + 3U < static_cast<std::size_t>(width) * height * 4U;
         index += 4U) {
      if (rgba8[index] > 8U || rgba8[index + 1U] > 8U ||
          rgba8[index + 2U] > 8U) {
        ++input_non_black;
      }
      if (rgba8[index] != 0U || rgba8[index + 1U] != 0U ||
          rgba8[index + 2U] != 0U) {
        ++input_non_zero;
      }
      maximum_red = (std::max)(maximum_red, rgba8[index]);
      maximum_green = (std::max)(maximum_green, rgba8[index + 1U]);
      maximum_blue = (std::max)(maximum_blue, rgba8[index + 2U]);
    }
    GLint viewport[4]{};
    GLint current_program = 0;
    GLint current_texture = 0;
    glGetIntegerv(GL_VIEWPORT, viewport);
    glGetIntegerv(GL_CURRENT_PROGRAM, &current_program);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &current_texture);
    const float *const projection = projection_;
    const std::size_t sample_width = static_cast<std::size_t>(viewport[2]);
    const std::size_t sample_height = static_cast<std::size_t>(viewport[3]);
    const std::vector<std::uint8_t> pixels =
        ReadBackBufferRgb(framebuffers_, viewport[2], viewport[3]);
    std::size_t output_non_black = 0U;
    std::size_t output_non_zero = 0U;
    std::uint8_t output_maximum = 0U;
    for (std::size_t index = 0U; index + 2U < pixels.size(); index += 3U) {
      if (pixels[index] > 8U || pixels[index + 1U] > 8U ||
          pixels[index + 2U] > 8U) {
        ++output_non_black;
      }
      if (pixels[index] != 0U || pixels[index + 1U] != 0U ||
          pixels[index + 2U] != 0U) {
        ++output_non_zero;
      }
      output_maximum = (std::max)(output_maximum, pixels[index]);
      output_maximum = (std::max)(output_maximum, pixels[index + 1U]);
      output_maximum = (std::max)(output_maximum, pixels[index + 2U]);
    }
    fprintf(stderr,
            "[repiu-live-debug] LFB draw #%ld input=%zu/%zu max=%u,%u,%u"
            " viewport=%d,%d,%d,%d"
            " program=%d texture=%d projection=%g,%g,%g,%g"
            " output=%zu/%zu nonzero=%zu max=%u\n",
            diagnostic_index, input_non_black, input_non_zero, maximum_red,
            maximum_green, maximum_blue, viewport[0], viewport[1], viewport[2],
            viewport[3], current_program, current_texture, projection[0],
            projection[5], projection[12], projection[13], output_non_black,
            sample_width * sample_height, output_non_zero, output_maximum);
  }

  shader_.SetBlitMode(false);
  shader_.SetTextureEnabled(texture_combine_enabled_);
  if (drawing_to_front) {
    framebuffers_.SetDrawTargetFront(true);
  }
  RestoreGameGlState();

  if (present_to_front) {
    BufferSwap(0);
  }

  message_ = "Glide LFB surface presented";
  return glGetError() == GL_NO_ERROR;
}

bool GlideOpenGlBackend::ReadbackFramebuffer(std::uint32_t width,
                                             std::uint32_t height,
                                             std::vector<std::uint8_t> *rgba8) {
  if (!IsHostThread()) {
    bool result = false;
    InvokeOnHostThread([this, width, height, rgba8, &result]() {
      result = ReadbackFramebuffer(width, height, rgba8);
    });
    return result;
  }
  if (rgba8 == nullptr || width == 0U || height == 0U || !is_open()) {
    message_ = "cannot read back Glide framebuffer";
    return false;
  }
  if (dummy_mode_) {
    rgba8->assign(static_cast<std::size_t>(width) * height * 4U, 0U);
    message_ = "Glide framebuffer read back (dummy)";
    return true;
  }
  // Task 769: only the picture, never the letterbox bars, so what the guest
  // reads back does not depend on the window's shape. Issue #69: the picture
  // is the whole back buffer of the offscreen pair.
  const int drawable_width = static_cast<int>(framebuffers_.width());
  const int drawable_height = static_cast<int>(framebuffers_.height());
  std::vector<std::uint8_t> drawable_rgba(
      static_cast<std::size_t>(drawable_width) * drawable_height * 4U);
  if (!framebuffers_.ReadBack(0U, 0U, framebuffers_.width(),
                              framebuffers_.height(), drawable_rgba.data())) {
    message_ = "cannot read back Glide framebuffer";
    return false;
  }
  // glReadPixels returns row 0 as the bottom scanline, while every consumer
  // here treats row 0 as the top. Sample the complete scaled picture and
  // return the logical Glide dimensions with nearest-neighbor filtering.
  rgba8->assign(static_cast<std::size_t>(width) * height * 4U, 0U);
  for (std::uint32_t y = 0; y < height; ++y) {
    const std::size_t source_top_y =
        static_cast<std::size_t>(y) * drawable_height / height;
    const std::size_t source_y =
        static_cast<std::size_t>(drawable_height - 1) - source_top_y;
    for (std::uint32_t x = 0; x < width; ++x) {
      const std::size_t source_x =
          static_cast<std::size_t>(x) * drawable_width / width;
      const std::size_t source_offset =
          (source_y * drawable_width + source_x) * 4U;
      const std::size_t destination_offset =
          (static_cast<std::size_t>(y) * width + x) * 4U;
      std::memcpy(rgba8->data() + destination_offset,
                  drawable_rgba.data() + source_offset, 4U);
    }
  }
  message_ = "Glide framebuffer read back";
  return glGetError() == GL_NO_ERROR;
}
bool GlideOpenGlBackend::SetColorMask(bool rgb, bool alpha) {
  if (!IsHostThread()) {
    bool result = false;
    InvokeOnHostThread(
        [this, rgb, alpha, &result]() { result = SetColorMask(rgb, alpha); });
    return result;
  }
  if (!is_open()) {
    message_ = "cannot set Glide color mask without an OpenGL window";
    return false;
  }
  if (dummy_mode_) {
    message_ = "Glide color mask dummy-applied";
    return true;
  }
  game_gl_state_.color_mask[0] = rgb;
  game_gl_state_.color_mask[1] = rgb;
  game_gl_state_.color_mask[2] = rgb;
  game_gl_state_.color_mask[3] = alpha;
  glColorMask(rgb ? GL_TRUE : GL_FALSE, rgb ? GL_TRUE : GL_FALSE,
              rgb ? GL_TRUE : GL_FALSE, alpha ? GL_TRUE : GL_FALSE);
  message_ = "Glide color mask applied to OpenGL";
  return CheckGlErrorIfEnabled();
}

bool GlideOpenGlBackend::SetRenderBuffer(std::uint32_t buffer) {
  if (!IsHostThread()) {
    bool result = false;
    InvokeOnHostThread(
        [this, buffer, &result]() { result = SetRenderBuffer(buffer); });
    return result;
  }
  constexpr std::uint32_t kGlideFrontBuffer = 0U;
  constexpr std::uint32_t kGlideBackBuffer = 1U;
  if (!is_open() ||
      (buffer != kGlideFrontBuffer && buffer != kGlideBackBuffer)) {
    message_ = "unsupported Glide render buffer";
    return false;
  }
  if (dummy_mode_) {
    message_ = buffer == kGlideBackBuffer
                   ? "Glide back buffer selected (dummy)"
                   : "Glide front buffer selected (dummy)";
    return true;
  }
  // Issue #69: the draw target is one of the offscreen pair. What is drawn
  // to front shows at the next present rather than at once; the game keeps
  // its target on back (design 729), so the first front selection is noted.
  if (buffer == kGlideFrontBuffer && front_render_buffer_selects_++ == 0U) {
    fprintf(stderr,
            "[repiu-glide] front render buffer selected: drawn offscreen, "
            "shown at the next swap\n");
  }
  framebuffers_.SetDrawTargetFront(buffer == kGlideFrontBuffer);
  message_ = buffer == kGlideBackBuffer ? "Glide back buffer selected"
                                        : "Glide front buffer selected";
  return CheckGlErrorIfEnabled();
}

bool GlideOpenGlBackend::SetDepthMask(bool enabled) {
  if (!IsHostThread()) {
    bool result = false;
    InvokeOnHostThread(
        [this, enabled, &result]() { result = SetDepthMask(enabled); });
    return result;
  }
  if (!is_open()) {
    message_ = "cannot set Glide depth mask without an OpenGL window";
    return false;
  }
  if (dummy_mode_) {
    message_ = enabled ? "Glide depth writes enabled (dummy)"
                       : "Glide depth writes disabled (dummy)";
    return true;
  }
  // Task 364: the message is assigned before the timed region so string cost
  // is not attributed to OpenGL. It is only read after the call returns, so
  // the reordering is semantically inert.
  message_ =
      enabled ? "Glide depth writes enabled" : "Glide depth writes disabled";
  const bool phase = GlideSetterPhaseEnabled();
  const std::uint64_t phase_entry = phase ? ReadGlideGateTimingCycles() : 0U;
  glDepthMask(enabled ? GL_TRUE : GL_FALSE);
  const std::uint64_t phase_error_start =
      phase ? ReadGlideGateTimingCycles() : 0U;
  // Task 369: the phase timestamps stay in place on purpose. With the policy
  // off this interval collapses to nothing, and that collapse is the evidence
  // the change took effect on a live capture.
  const bool no_error = CheckGlErrorIfEnabled();
  if (phase) {
    // Depth mask has no leading drain, so entry and apply-start are the
    // same instant and the drain interval is zero by construction.
    RecordGlideSetterPhaseSample(&glide_setter_phase_timing_,
                                 GlideSetterPhaseKind::kDepthMask,
                                 phase_entry, phase_entry, phase_error_start,
                                 ReadGlideGateTimingCycles(), 0U, !no_error);
  }
  return no_error;
}

bool GlideOpenGlBackend::SetDepthBufferMode(std::uint32_t mode) {
  if (!IsHostThread()) {
    bool result = false;
    InvokeOnHostThread(
        [this, mode, &result]() { result = SetDepthBufferMode(mode); });
    return result;
  }
  constexpr std::uint32_t kGlideDepthDisabled = 0U;
  constexpr std::uint32_t kGlideZBuffer = 1U;
  if (!is_open() || (mode != kGlideDepthDisabled && mode != kGlideZBuffer)) {
    message_ = "unsupported Glide depth buffer mode";
    return false;
  }
  if (dummy_mode_) {
    message_ = mode == kGlideZBuffer ? "Glide Z-buffer mode enabled (dummy)"
                                     : "Glide depth buffer disabled (dummy)";
    return true;
  }
  game_gl_state_.depth_test = mode == kGlideZBuffer;
  if (mode == kGlideZBuffer) {
    glEnable(GL_DEPTH_TEST);
    message_ = "Glide Z-buffer mode enabled";
  } else {
    glDisable(GL_DEPTH_TEST);
    message_ = "Glide depth buffer disabled";
  }
  return CheckGlErrorIfEnabled();
}

bool GlideOpenGlBackend::SetAlphaCombine(
    const hle::GlideAlphaCombineState &state) {
  if (!IsHostThread()) {
    bool result = false;
    InvokeOnHostThread(
        [this, &state, &result]() { result = SetAlphaCombine(state); });
    return result;
  }
  if (!is_open()) {
    message_ = "cannot set Glide alpha combine without an OpenGL window";
    return false;
  }
  if (dummy_mode_) {
    message_ = "Glide alpha combine dummy-applied";
    return true;
  }
  const bool applied = shader_.SetAlphaCombine(state);
  message_ = shader_.message();
  return applied;
}

bool GlideOpenGlBackend::SetConstantColor(std::uint32_t argb) {
  if (!IsHostThread()) {
    bool result = false;
    InvokeOnHostThread(
        [this, argb, &result]() { result = SetConstantColor(argb); });
    return result;
  }
  if (!is_open()) {
    message_ = "cannot set constant color without an OpenGL window";
    return false;
  }
  if (dummy_mode_) {
    return true;
  }
  const bool result = shader_.SetConstantColor(argb);
  message_ = shader_.message();
  return result;
}

bool GlideOpenGlBackend::SetColorCombine(
    const hle::GlideColorCombineState &state) {
  if (!IsHostThread()) {
    bool result = false;
    InvokeOnHostThread(
        [this, &state, &result]() { result = SetColorCombine(state); });
    return result;
  }
  if (!is_open()) {
    message_ = "cannot set Glide color combine without an OpenGL window";
    return false;
  }
  if (dummy_mode_) {
    message_ = "Glide color combine dummy-applied";
    return true;
  }
  const bool applied = shader_.SetColorCombine(state);
  message_ = shader_.message();
  return applied;
}

namespace {

// Map a Glide GrAlphaBlendFnc_t factor to its OpenGL blend factor. Glide uses
// the same enum value for the color-scaled variants (2, 6) but interprets them
// relative to the operand, so the source and destination factors differ.
bool MapGlideBlendFactor(std::uint32_t factor, bool is_source,
                         GLenum *gl_factor) {
  switch (factor) {
  case 0U:
    *gl_factor = GL_ZERO;
    return true; // GR_BLEND_ZERO
  case 1U:
    *gl_factor = GL_SRC_ALPHA;
    return true; // SRC_ALPHA
  case 2U:
    *gl_factor = is_source ? GL_DST_COLOR : GL_SRC_COLOR;
    return true;
  case 3U:
    *gl_factor = GL_DST_ALPHA;
    return true; // DST_ALPHA
  case 4U:
    *gl_factor = GL_ONE;
    return true; // GR_BLEND_ONE
  case 5U:
    *gl_factor = GL_ONE_MINUS_SRC_ALPHA;
    return true;
  case 6U:
    *gl_factor = is_source ? GL_ONE_MINUS_DST_COLOR : GL_ONE_MINUS_SRC_COLOR;
    return true;
  case 7U:
    *gl_factor = GL_ONE_MINUS_DST_ALPHA;
    return true;
  case 15U:
    *gl_factor = GL_SRC_ALPHA_SATURATE;
    return true;
  default:
    return false;
  }
}

} // namespace

bool GlideOpenGlBackend::SetAlphaBlend(const hle::GlideAlphaBlendState &state) {
  if (!IsHostThread()) {
    bool result = false;
    InvokeOnHostThread(
        [this, &state, &result]() { result = SetAlphaBlend(state); });
    return result;
  }
  constexpr std::uint32_t kGlideBlendZero = 0U;
  constexpr std::uint32_t kGlideBlendOne = 4U;
  GLenum gl_source = GL_ONE;
  GLenum gl_destination = GL_ZERO;
  if (!is_open() || !state.valid ||
      !MapGlideBlendFactor(state.rgb_source, true, &gl_source) ||
      !MapGlideBlendFactor(state.rgb_destination, false, &gl_destination)) {
    message_ = "unsupported Glide alpha-blend function";
    return false;
  }
  const bool opaque = state.rgb_source == kGlideBlendOne &&
                      state.rgb_destination == kGlideBlendZero;
  if (dummy_mode_) {
    message_ = opaque ? "Glide ONE/ZERO alpha blending disabled (dummy)"
                      : "Glide alpha blending enabled (dummy)";
    return true;
  }
  // Task 364: as in SetDepthMask, the message is assigned outside the timed
  // region so only OpenGL work lands in the phase totals.
  message_ = opaque ? "Glide ONE/ZERO alpha blending disabled in OpenGL"
                    : "Glide alpha blending enabled in OpenGL";
  const bool phase = GlideSetterPhaseEnabled();
  const std::uint64_t phase_entry = phase ? ReadGlideGateTimingCycles() : 0U;
  std::uint32_t drain_iterations = 0;
  // Task 369: the leading drain exists only to make the trailing check
  // meaningful, so it follows the same policy. Draining is itself a flush --
  // the phase instrument measured this loop at 62,653 cycles per call.
  if (GlideGlErrorCheckEnabled()) {
    while (glGetError() != GL_NO_ERROR) {
      ++drain_iterations;
    }
  }
  const std::uint64_t phase_apply_start =
      phase ? ReadGlideGateTimingCycles() : 0U;
  game_gl_state_.blend = !opaque;
  if (opaque) {
    glDisable(GL_BLEND);
  } else {
    glEnable(GL_BLEND);
    glBlendFunc(gl_source, gl_destination);
  }
  const std::uint64_t phase_error_start =
      phase ? ReadGlideGateTimingCycles() : 0U;
  const bool no_error = CheckGlErrorIfEnabled();
  if (phase) {
    RecordGlideSetterPhaseSample(
        &glide_setter_phase_timing_, GlideSetterPhaseKind::kAlphaBlend,
        phase_entry, phase_apply_start, phase_error_start,
        ReadGlideGateTimingCycles(), drain_iterations, !no_error);
  }
  return no_error;
}

bool GlideOpenGlBackend::SetAlphaTestFunction(std::uint32_t function) {
  if (!IsHostThread()) {
    bool result = false;
    InvokeOnHostThread([this, function, &result]() {
      result = SetAlphaTestFunction(function);
    });
    return result;
  }
  if (!is_open() || function > 7U) {
    message_ = "unsupported Glide alpha-test function";
    return false;
  }
  alpha_test_function_ = function;
  if (dummy_mode_) {
    message_ = "Glide alpha test applied (dummy)";
    return true;
  }
  // Issue #69: the shader compares the final alpha (glAlphaFunc is gone from
  // core and ES); GR_CMP_ALWAYS (7) passes everything.
  shader_.SetAlphaTest(function, alpha_test_reference_);
  message_ = function == 7U ? "Glide ALWAYS alpha test disabled in GLSL"
                            : "Glide alpha test enabled in GLSL";
  return CheckGlErrorIfEnabled();
}

bool GlideOpenGlBackend::SetAlphaTestReferenceValue(
    std::uint32_t reference_value) {
  if (!IsHostThread()) {
    bool result = false;
    InvokeOnHostThread([this, reference_value, &result]() {
      result = SetAlphaTestReferenceValue(reference_value);
    });
    return result;
  }
  if (!is_open()) {
    return false;
  }
  alpha_test_reference_ = static_cast<float>(reference_value) / 255.0f;
  if (dummy_mode_) {
    return true;
  }
  shader_.SetAlphaTest(alpha_test_function_, alpha_test_reference_);
  return CheckGlErrorIfEnabled();
}

bool GlideOpenGlBackend::SetDepthBufferFunction(std::uint32_t function) {
  if (!IsHostThread()) {
    bool result = false;
    InvokeOnHostThread([this, function, &result]() {
      result = SetDepthBufferFunction(function);
    });
    return result;
  }
  constexpr std::uint32_t kGlideCompareAlways = 7U;
  if (!is_open() || function > kGlideCompareAlways) {
    message_ = "unsupported Glide depth-buffer function";
    return false;
  }
  if (dummy_mode_) {
    message_ = "Glide depth comparison applied (dummy)";
    return true;
  }
  glDepthFunc(GL_NEVER + function);
  message_ = "Glide depth comparison applied to OpenGL";
  return CheckGlErrorIfEnabled();
}

bool GlideOpenGlBackend::SetFogMode(std::uint32_t mode) {
  if (!IsHostThread()) {
    bool result = false;
    InvokeOnHostThread([this, mode, &result]() { result = SetFogMode(mode); });
    return result;
  }
  if (!is_open() || (mode != 0U && mode != 2U)) {
    message_ = "unsupported Glide fog mode";
    return false;
  }
  if (dummy_mode_) {
    message_ = mode == 0U ? "Glide fog disabled (dummy)"
                          : "Glide table fog enabled (dummy)";
    return true;
  }
  const bool applied = shader_.SetFogMode(mode);
  message_ = applied ? (mode == 0U ? "Glide fog disabled in GLSL"
                                   : "Glide table fog enabled in GLSL")
                     : "failed to apply Glide fog mode to GLSL";
  return applied && CheckGlErrorIfEnabled();
}

bool GlideOpenGlBackend::SetFogColor(std::uint32_t argb) {
  if (!IsHostThread()) {
    bool result = false;
    InvokeOnHostThread([this, argb, &result]() { result = SetFogColor(argb); });
    return result;
  }
  if (!is_open()) {
    message_ = "cannot set Glide fog color without an OpenGL window";
    return false;
  }
  if (dummy_mode_) {
    message_ = "Glide fog color applied (dummy)";
    return true;
  }
  const bool applied = shader_.SetFogColor(argb);
  message_ = applied ? "Glide fog color applied to GLSL"
                     : "failed to apply Glide fog color to GLSL";
  return applied;
}

bool GlideOpenGlBackend::SetFogTable(const hle::GlideFogTable &table) {
  if (!IsHostThread()) {
    bool result = false;
    InvokeOnHostThread(
        [this, &table, &result]() { result = SetFogTable(table); });
    return result;
  }
  if (!is_open()) {
    message_ = "cannot set Glide fog table without an OpenGL window";
    return false;
  }
  if (dummy_mode_) {
    message_ = "Glide fog table applied (dummy)";
    return true;
  }
  const bool applied = shader_.SetFogTable(table);
  message_ = applied ? "Glide fog table applied to GLSL"
                     : "failed to apply Glide fog table to GLSL";
  return applied;
}

bool GlideOpenGlBackend::SetClipWindow(std::uint32_t min_x, std::uint32_t min_y,
                                       std::uint32_t max_x,
                                       std::uint32_t max_y) {
  if (!IsHostThread()) {
    bool result = false;
    InvokeOnHostThread([this, min_x, min_y, max_x, max_y, &result]() {
      result = SetClipWindow(min_x, min_y, max_x, max_y);
    });
    return result;
  }
  if (!is_open() || min_x != 0U || min_y != 0U || max_x != logical_width_ ||
      max_y != logical_height_) {
    message_ = "unsupported partial Glide clip window";
    return false;
  }
  if (dummy_mode_) {
    message_ = "full Glide clip window applied (dummy)";
    return true;
  }
  ApplyDrawableViewport();
  game_gl_state_.scissor_test = true;
  glEnable(GL_SCISSOR_TEST);
  message_ = "full Glide clip window applied to OpenGL";
  return CheckGlErrorIfEnabled();
}

bool GlideOpenGlBackend::SetCullMode(std::uint32_t mode) {
  if (!IsHostThread()) {
    bool result = false;
    InvokeOnHostThread([this, mode, &result]() { result = SetCullMode(mode); });
    return result;
  }
  GlideOpenGlCullFace face = GlideOpenGlCullFace::kDisabled;
  if (!is_open() ||
      !TranslateGlideOpenGlCullMode(mode, origin_lower_left_, &face)) {
    message_ = "unsupported Glide cull mode";
    return false;
  }
  if (dummy_mode_) {
    message_ = "Glide cull mode applied (dummy)";
    return true;
  }
  game_gl_state_.cull_face = face != GlideOpenGlCullFace::kDisabled;
  if (face == GlideOpenGlCullFace::kDisabled) {
    glDisable(GL_CULL_FACE);
  } else {
    glFrontFace(GL_CCW);
    glCullFace(face == GlideOpenGlCullFace::kFront ? GL_FRONT : GL_BACK);
    glEnable(GL_CULL_FACE);
  }
  message_ = "Glide cull mode applied to OpenGL";
  return CheckGlErrorIfEnabled();
}

bool GlideOpenGlBackend::SetDitherMode(std::uint32_t mode) {
  if (!IsHostThread()) {
    bool result = false;
    InvokeOnHostThread(
        [this, mode, &result]() { result = SetDitherMode(mode); });
    return result;
  }
  constexpr std::uint32_t kObservedDitherMode = 2U;
  if (!is_open() || mode != kObservedDitherMode) {
    message_ = "unsupported Glide dither mode";
    return false;
  }
  if (dummy_mode_) {
    message_ = "observed Glide dither mode delegated (dummy)";
    return true;
  }
  // TODO(Glide fidelity): replace host dithering with a verified Voodoo
  // ordered-dither GLSL path once mode-2 matrix and PIU color quantization
  // are confirmed. See docs/design/20260712-158-glide-host-dither-policy.md.
  glEnable(GL_DITHER);
  message_ = "observed Glide dither mode delegated to OpenGL";
  return CheckGlErrorIfEnabled();
}

void GlideOpenGlBackend::Close() {
  if (!IsHostThread()) {
    InvokeOnHostThread([this]() { Close(); });
    return;
  }
  // Task 440: declare the channel closed so a later post refuses instead of
  // waiting on a host that will never pump again.
  //
  // **No lock is taken here, and the queue is deliberately not drained.** The
  // timeout path reaches this after terminating the guest thread, and that
  // thread takes `host_command_mutex_` on every Glide gate -- taking a mutex
  // it still owns is what crashed teardown while this code did. What stays
  // queued is at most a fraction of a frame that was never presented.
  host_stopped_pumping_.store(true, std::memory_order_release);
  pending_swap_count_.store(0U, std::memory_order_relaxed);
  // Task 748: stop the scripted keyboard before the window goes.
  if (input_script_ != nullptr) {
    input_script_->player.Stop();
    fprintf(stderr, "[repiu-input-script] pushed %llu events\n",
            static_cast<unsigned long long>(
                input_script_->player.pushed_events()));
    input_script_.reset();
  }
  // Issue #76: the browser's virtual gamepads go first, while the gamepad
  // subsystem the pads hold is still up.
  if (browser_input_ != nullptr) {
    if (browser_input_->keys_pushed() != 0U) {
      fprintf(stderr, "[repiu-browser-input] pushed %llu key events\n",
              static_cast<unsigned long long>(browser_input_->keys_pushed()));
    }
    browser_input_.reset();
  }
  // Issue #34: close the pads with the window.
  pad_input_.reset();
  // **No `notify_all` here.** The timeout path terminates the guest thread,
  // and that thread waits on `host_command_cv_` inside every synchronous gate.
  // A thread killed while waiting leaves its wait block linked into the
  // variable, and waking that list faults in `RtlWakeAllConditionVariable` --
  // measured, with a stack, as the teardown crash that cost Task 440 five
  // rounds of guessing. Nothing needs the wake: the only thread that could be
  // blocked on back pressure is the guest, and it is already gone.
  if (dummy_mode_) {
    dummy_mode_ = false;
    logical_width_ = 0;
    logical_height_ = 0;
    window_scale_ = 2U;
    point_size_ = 1.0F;
    palette_rgba8_.fill(0U);
    palette_valid_ = false;
    palette_generation_ = 0U;
    ResetFrameRateMeasurement();
    return;
  }
  try {
    if (host_window_ != nullptr) {
      host_window_->MakeCurrent();
      // Task 761: the OSD's GL objects belong to this context, so it goes
      // first, while the context is still current.
      if (osd_ != nullptr) {
        osd_->Shutdown();
        osd_.reset();
      }
      // Task 768: the same holds for the post-processing program and texture.
      if (post_process_ != nullptr) {
        post_process_->Shutdown();
      }
      for (auto &entry : textures_) {
        if (entry.second.gl_name != 0U) {
          GLuint name = entry.second.gl_name;
          glDeleteTextures(1, &name);
        }
      }
      if (lfb_texture_ != 0U) {
        GLuint lfb_name = lfb_texture_;
        glDeleteTextures(1, &lfb_name);
      }
      shader_.Shutdown();
      vertex_stream_.Shutdown();
      framebuffers_.Shutdown();
      host_window_->Close();
    }
  } catch (const std::exception &e) {
    fprintf(stderr,
            "[repiu-live-debug] GlideOpenGlBackend::Close caught standard "
            "exception: %s\n",
            e.what());
  } catch (...) {
    fprintf(stderr, "[repiu-live-debug] GlideOpenGlBackend::Close caught "
                    "unknown exception\n");
  }
  // Its destructor touches no GL, so this is safe with the context gone.
  post_process_.reset();
  host_window_.reset();
  logical_width_ = 0;
  logical_height_ = 0;
  window_scale_ = 2U;
  point_size_ = 1.0F;
  content_rect_ = GlideLetterboxRect{};
  drawable_width_ = 0U;
  drawable_height_ = 0U;
  ResetFrameRateMeasurement();
  origin_lower_left_ = false;
  textures_.clear();
  current_texture_ = nullptr;
  palette_rgba8_.fill(0U);
  palette_valid_ = false;
  palette_generation_ = 0U;
  texture_combine_enabled_ = false;
  lfb_texture_ = 0;
  gl_ = GlApi{};
  game_gl_state_ = GameGlState{};
  point_size_range_[0] = 1.0F;
  point_size_range_[1] = 1.0F;
  stream_vertices_.clear();
  front_render_buffer_selects_ = 0U;
}

} // namespace repiu::engine
