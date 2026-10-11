#include "port_io_emulator.h"
#include "repiu/engine/active_jamma_bindings.h"
#include "repiu/engine/eeprom_backing_path.h"
#include "cpu_emul/guest_memory_access.h"
#include "eeprom_93c46.h"
#include "execution_internal.h"
#include "piu10_mp3_frame_batch.h"
#include "piu10_sound_port.h"
#include "port_io_delay_loop.h"
#include "repiu/input/jamma_input_bindings.h"
#include <SDL3/SDL_keyboard.h>
#include "repiu/engine/execution_trampoline.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <memory>
#include <sstream>
#include <vector>
#include "repiu/platform/guest_cpu_context.h"
#include "repiu/platform/host_time.h"


namespace repiu::engine {

namespace {
constexpr std::uint16_t kPortPitChannel0 = 0x0040;
constexpr std::uint16_t kPortPitControl = 0x0043;
constexpr std::uint16_t kPortPicCommand = 0x0020;
constexpr std::uint16_t kPortPiuJammaBase = 0x02A0;
constexpr std::uint16_t kPortPiuJammaEnd = 0x02AF;
constexpr std::uint16_t kPortPiu10IsaBase = 0x02D0;
constexpr std::uint16_t kPortPiu10IsaEnd = 0x02DF;

constexpr std::uint16_t kPortPiuEepromWrite = 0x02AC;
constexpr std::uint16_t kPortPiuEepromRead = 0x02AE;

constexpr std::uint16_t kPortPiuIn0 = 0x02A8;
constexpr std::uint16_t kPortPiuSystem = 0x02A9;
constexpr std::uint16_t kPortPiuIn1 = 0x02AA;

// Task 498: resolved once by the host entry point, which also creates the
// directory. The fallback keeps a tool that never sets it working, and keeps
// honoring REPIU_EEPROM_PATH so the benchmark scripts still isolate the EEPROM
// per run.
std::string &MutableEepromBackingPath() {
  static std::string path = [] {
    const char *value = std::getenv("REPIU_EEPROM_PATH");
    return std::string(value != nullptr && *value != '\0' ? value
                                                          : "eeprom.dat");
  }();
  return path;
}

std::string EepromBackingPath() { return MutableEepromBackingPath(); }

} // namespace

// The guest polls these ports every frame, so logging each read would flood
// the console exactly like the INT 8 line did. Log edges only: one line per
// press and per release, which is what actually confirms a key reached the
// guest. Called per byte from ReadJammaPort8.
//
// Task 497: the bit names come from the shared port-bit table rather than a
// private copy. The two used to disagree -- the log called bit 0x02
// "P1-UpRight" while the scan called the same bit kP1Down.
static void LogJammaInputTransition(std::uint16_t port, std::uint8_t value) {
  if (port != kPortPiuIn0 && port != kPortPiuSystem && port != kPortPiuIn1) {
    return;
  }

  const std::size_t index = port - kPortPiuIn0;

  // Announce the first poll of each port once. Without this a silent log is
  // ambiguous: it cannot distinguish "the key mapping is broken" from "the
  // guest has not started polling this port yet".
  static bool first_poll_logged[3] = {false, false, false};
  if (!first_poll_logged[index]) {
    first_poll_logged[index] = true;
    std::fprintf(stderr,
                 "[repiu-input] polling started port=0x%04X value=0x%02X\n",
                 static_cast<unsigned>(port), static_cast<unsigned>(value));
  }

  static std::uint8_t previous[3] = {0xFFU, 0xFFU, 0xFFU};
  std::uint8_t &last = previous[index];
  const std::uint8_t changed = static_cast<std::uint8_t>(last ^ value);
  if (changed == 0U) {
    return;
  }
  last = value;

  std::uint32_t bit_count = 0;
  const repiu::input::JammaPortBit *bits =
      repiu::input::JammaPortBitTable(&bit_count);
  for (std::uint32_t i = 0; i < bit_count; ++i) {
    if (bits[i].port != port || (changed & bits[i].mask) == 0U) {
      continue;
    }
    const bool pressed = (value & bits[i].mask) == 0U;
    std::fprintf(stderr, "[repiu-input] %-14s %-8s port=0x%04X value=0x%02X\n",
                 bits[i].log_name, pressed ? "PRESSED" : "released",
                 static_cast<unsigned>(port), static_cast<unsigned>(value));
  }
}

// Task 403: counts host key queries so the port I/O cost can be split between
// the host key scan and everything else in the handler. Task 503d-13 turned
// each one from a Win32 call into an array index, and the counter is kept so
// the two measurements are of the same thing.
static std::uint32_t g_jamma_key_query_count = 0;

std::uint32_t TakeJammaKeyQueryCount() {
  const std::uint32_t value = g_jamma_key_query_count;
  g_jamma_key_query_count = 0;
  return value;
}

// Task 497: the host keys come from the configured binding table instead of
// being hardcoded here, but the shape of the work is unchanged. Every alias
// still resolves to an already-translated virtual key, so this walks a fixed
// array and performs no string comparison, map lookup, or allocation -- the
// constraint Task 403 established when GetAsyncKeyState turned out to be
// 99.21% of this handler's body.
static std::uint8_t
ScanJammaPort8(std::uint16_t port,
               const std::uint16_t *replay_pressed_mask = nullptr) {
  std::uint8_t value = 0xFF; // Active Low

  const repiu::input::ResolvedJammaBindings &bindings = ActiveJammaBindings();
  // Read once per port byte rather than per alias, and not at all unless some
  // alias needs it. The default configuration uses no modifiers, so this stays
  // free and the host key query count matches what it was before.
  const SDL_Keymod modifier_state =
      (replay_pressed_mask == nullptr && bindings.any_binding_uses_modifiers)
          ? SDL_GetModState()
          : SDL_KMOD_NONE;

  // SDL owns this array for the life of the process and documents reading it
  // from any thread, so the pointer is taken once and the guest thread indexes
  // it directly. SDL_PumpEvents on the host thread is what refreshes it.
  static const bool *const key_state = SDL_GetKeyboardState(nullptr);
  // Issue #34: what the pads hold, published by the host thread. One atomic
  // load per port byte; zero with no pad attached, and it adds nothing to the
  // host key query count.
  const std::uint16_t pad_mask =
      replay_pressed_mask == nullptr ? PublishedJammaPadMask() : 0U;

  auto is_pressed = [replay_pressed_mask, &bindings, modifier_state,
                     pad_mask](JammaInputKey key) -> bool {
    if (replay_pressed_mask != nullptr) {
      return (*replay_pressed_mask & JammaInputKeyMask(key)) != 0U;
    }
    if ((pad_mask & JammaInputKeyMask(key)) != 0U) {
      return true;
    }
    const repiu::input::JammaInputBinding &binding = bindings.Get(key);
    for (std::uint32_t slot = 0; slot < binding.alias_count; ++slot) {
      const repiu::input::HostKeyAlias &alias = binding.aliases[slot];
      if (alias.scancode == SDL_SCANCODE_UNKNOWN) {
        continue;
      }
      ++g_jamma_key_query_count;
      if (key_state[alias.scancode] &&
          alias.ModifiersMatch(modifier_state)) {
        return true;
      }
    }
    return false;
  };

  std::uint32_t bit_count = 0;
  const repiu::input::JammaPortBit *bits =
      repiu::input::JammaPortBitTable(&bit_count);
  for (std::uint32_t index = 0; index < bit_count; ++index) {
    if (bits[index].port != port) {
      continue;
    }
    if (is_pressed(bits[index].key)) {
      value &= static_cast<std::uint8_t>(~bits[index].mask);
    }
  }

  LogJammaInputTransition(port, value);
  return value;
}

// Task 403: the guest reads port 0x02A8 200 times in a row purely as a settle
// delay and discards every value, so scanning the host keyboard on each read
// made GetAsyncKeyState 99.21% of the port I/O handler body and 30.65% of wall
// clock. It polls about 208 times a second, so a snapshot refreshed on a bound
// far finer than that loses no transition the guest could observe. This does
// not touch the Task 327 rule: the guest IN still traps, EIP still advances,
// and nothing is NOP-patched -- only the rescan inside the trap is bounded.
//
// Guest-thread only, like g_eeprom below: HandlePortIoInstruction runs on the
// VEH path and the Task 311 AOT fast-path thunk, both on the guest thread.
namespace {

constexpr std::uint32_t kJammaSnapshotPortCount =
    kPortPiuJammaEnd - kPortPiuJammaBase + 1U;
constexpr std::uint64_t kDefaultJammaSnapshotMicroseconds = 500U;

std::uint64_t ReadJammaSnapshotIntervalMicroseconds() {
  const char *interval = std::getenv("REPIU_JAMMA_SNAPSHOT_US");
  if (interval == nullptr || interval[0] == 0) {
    return kDefaultJammaSnapshotMicroseconds;
  }
  const long parsed = std::strtol(interval, nullptr, 10);
  return parsed <= 0 ? 0U : static_cast<std::uint64_t>(parsed);
}

std::uint64_t JammaSnapshotIntervalMicroseconds() {
  static const std::uint64_t interval = ReadJammaSnapshotIntervalMicroseconds();
  return interval;
}

std::int64_t PerformanceFrequency() {
  return repiu::platform::PerformanceCounterFrequency();
}

struct JammaSnapshot {
  bool valid = false;
  std::int64_t refreshed_at = 0;
  std::uint8_t values[kJammaSnapshotPortCount] = {};
};

JammaSnapshot g_jamma_snapshot;

void RefreshJammaSnapshot(std::int64_t now) {
  for (std::uint32_t index = 0; index < kJammaSnapshotPortCount; ++index) {
    g_jamma_snapshot.values[index] =
        ScanJammaPort8(static_cast<std::uint16_t>(kPortPiuJammaBase + index));
  }
  g_jamma_snapshot.refreshed_at = now;
  g_jamma_snapshot.valid = true;
}

} // namespace

static std::uint8_t ReadJammaPort8(ThreadContext *context,
                                   std::uint32_t current_esp,
                                   std::uint16_t port) {
  std::uint16_t replay_pressed_mask = 0U;
  if (context != nullptr && context->jamma_input_timeline.TryReplayPressedMask(
                                current_esp, &replay_pressed_mask)) {
    return ScanJammaPort8(port, &replay_pressed_mask);
  }
  // Task 753: a scripted keyboard's keys are in the timeline, not in SDL's
  // keyboard state.
  if (context != nullptr &&
      context->jamma_input_timeline.TryLatestPressedMask(
          &replay_pressed_mask)) {
    return ScanJammaPort8(port, &replay_pressed_mask);
  }
  const std::uint64_t interval = JammaSnapshotIntervalMicroseconds();
  if (interval == 0U || port < kPortPiuJammaBase || port > kPortPiuJammaEnd) {
    return ScanJammaPort8(port);
  }

  const std::int64_t now = repiu::platform::PerformanceCounterTicks();
  const std::int64_t elapsed_ticks = now - g_jamma_snapshot.refreshed_at;
  const std::int64_t stale_ticks = static_cast<std::int64_t>(
      (PerformanceFrequency() * static_cast<std::int64_t>(interval)) / 1000000);
  if (!g_jamma_snapshot.valid || elapsed_ticks < 0 ||
      elapsed_ticks >= stale_ticks) {
    RefreshJammaSnapshot(now);
  }

  return g_jamma_snapshot.values[port - kPortPiuJammaBase];
}

static std::unique_ptr<Eeprom93c46> g_eeprom;

void SetEepromBackingPath(const std::string &path) {
  if (path.empty()) {
    return;
  }
  if (g_eeprom) {
    // The device has already loaded a file, and moving the path now would let
    // the destructor save this run's contents over a different image.
    std::fprintf(stderr,
                 "[repiu-eeprom] ignoring backing path change to %s: the "
                 "device is already open at %s\n",
                 path.c_str(), MutableEepromBackingPath().c_str());
    return;
  }
  MutableEepromBackingPath() = path;
}

void RecordPortIo(ThreadContext *context, std::uint32_t address,
                  std::uint32_t opcode, std::uint16_t port, std::uint32_t width,
                  std::uint32_t value, bool is_input, bool handled,
                  const std::string &result) {
  if (context == nullptr) {
    return;
  }

  ++context->port_io.observed_count;
  context->port_io.last_address = address;
  context->port_io.last_opcode = opcode;
  context->port_io.last_port = port;
  context->port_io.last_width = width;
  context->port_io.last_value = value;
  context->port_io.last_is_input = is_input;
  context->port_io.last_handled = handled;
  context->port_io.last_result = result;
  ++context->port_io.opcode_counts[opcode & 0xFFU];
  if (is_input) {
    ++context->port_io.input_count;
  } else {
    ++context->port_io.output_count;
  }
  if (handled) {
    ++context->port_io.handled_count;
  } else {
    ++context->port_io.unhandled_count;
  }
  if (context->port_io.trace_stored_count < kPortIoTraceCapacity) {
    PortIoTraceEntry &entry =
        context->port_io.trace[context->port_io.trace_stored_count];
    entry.valid = true;
    entry.sequence = context->port_io.observed_count;
    entry.address = address;
    entry.opcode = opcode;
    entry.port = port;
    entry.width = width;
    entry.value = value;
    entry.is_input = is_input;
    entry.handled = handled;
    ++context->port_io.trace_stored_count;
  }
}

bool IsPortIoTraceCandidate(std::uint16_t port, std::uint32_t width,
                            bool is_input) {
  return !is_input && (width == 1 || width == 2 || width == 4) &&
         port >= kPortPiuJammaBase && port <= kPortPiuJammaEnd;
}

// Task 405: which guest addresses issue port I/O, and whether each execution
// came from the AOT cache or from the arena. A linear table is enough because
// the population is a handful of addresses; overflow is counted so the total
// can still be reconciled against the profiled port I/O count.
// Task 406: the mapping lookup is opt-in because `FindAotCacheAddress` costs
// about 6,866 ticks and this site runs roughly 23,000 times a second, which
// would add about 5.8% of a core and distort the very measurement it serves.
static bool PortIoCensusMappingEnabled() {
  static const bool enabled = [] {
    const char *value = std::getenv("REPIU_PORT_IO_CENSUS_MAPPING");
    return value != nullptr &&
           (std::strcmp(value, "1") == 0 || std::strcmp(value, "on") == 0 ||
            std::strcmp(value, "true") == 0);
  }();
  return enabled;
}

// Task 408: the entry sample the census entry keeps for its address.
struct PortIoEntrySample {
  bool is_entry = false;
  std::uint32_t previous_code = 0;
  std::uint32_t previous_eip = 0;
  std::uint8_t flags = 0;
  // Task 410: who resumed the guest after that previous exception, and where.
  std::uint8_t previous_exit_site = 0;
  std::uint32_t previous_exit_eip = 0;
};

static void
ApplyPortIoEntrySample(ThreadContext::PortIoAddressCensusEntry *entry,
                       const PortIoEntrySample &sample) {
  if (!sample.is_entry) {
    return;
  }
  // The first transition is kept whole; later ones only raise the count,
  // which is what makes a busy address unable to overwrite its own evidence.
  if (entry->entry_transition_count == 0U) {
    entry->entry_previous_code = sample.previous_code;
    entry->entry_previous_eip = sample.previous_eip;
    entry->entry_flags = sample.flags;
    entry->entry_previous_exit_site = sample.previous_exit_site;
    entry->entry_previous_exit_eip = sample.previous_exit_eip;
  }
  ++entry->entry_transition_count;
  // Task 409: and the class of every transition, because the first sample
  // turned out to describe at most a tenth of them.
  switch (sample.previous_code) {
  case 0x80000004U:
    ++entry->entry_prev_single_step;
    break;
  case 0x80000003U:
    ++entry->entry_prev_breakpoint;
    break;
  case 0xC0000005U:
    ++entry->entry_prev_access_violation;
    break;
  default:
    ++entry->entry_prev_other;
    break;
  }
}

static void RecordPortIoAddress(ThreadContext *context,
                                std::uint32_t guest_address,
                                bool from_aot_cache, bool mapped,
                                bool reentry_pending,
                                const PortIoEntrySample &entry_sample) {
  for (std::uint32_t index = 0; index < context->port_io_address_census_size;
       ++index) {
    auto &entry = context->port_io_address_census[index];
    if (entry.guest_address == guest_address) {
      ++entry.count;
      if (from_aot_cache) {
        ++entry.cache_count;
      }
      if (mapped) {
        ++entry.mapped_count;
      }
      if (reentry_pending) {
        ++entry.reentry_pending_count;
      }
      ApplyPortIoEntrySample(&entry, entry_sample);
      return;
    }
  }
  if (context->port_io_address_census_size >=
      ThreadContext::kPortIoAddressCensusCapacity) {
    ++context->port_io_address_census_overflow;
    return;
  }
  auto &entry =
      context->port_io_address_census[context->port_io_address_census_size++];
  entry.guest_address = guest_address;
  entry.count = 1U;
  entry.cache_count = from_aot_cache ? 1U : 0U;
  entry.mapped_count = mapped ? 1U : 0U;
  entry.reentry_pending_count = reentry_pending ? 1U : 0U;
  ApplyPortIoEntrySample(&entry, entry_sample);
}

static bool TryPiu10Mp3BytePortFastPath(repiu::platform::GuestCpuContext *win32_context,
                                        ThreadContext *context) {
  if (!context->piu10_isa_board_enabled ||
      !context->piu10_isa_board.available() ||
      context->piu10_isa_board.destination() != 0x008U ||
      !context->piu10_mp3_audio.available() ||
      (win32_context->Edx & 0xFFFFU) != 0x02DAU) {
    return false;
  }
  const auto *instruction = reinterpret_cast<const std::uint8_t *>(
      static_cast<std::uintptr_t>(win32_context->Eip));
  if (!IsGuestRangeReadable(context, instruction, 1U) ||
      instruction[0] != 0xEEU) {
    return false;
  }

  const std::uint8_t mp3_byte =
      static_cast<std::uint8_t>(win32_context->Eax & 0xFFU);
  const bool mp3_byte_accepted = context->piu10_mp3_audio.WriteByte(mp3_byte);
  if (mp3_byte_accepted) {
    const std::uint64_t previous =
        context->piu10_mp3_fast_path_write_count.fetch_add(
            1U, std::memory_order_relaxed);
    if (previous == 0U) {
      std::fprintf(stderr, "[repiu-piu10-mp3] arena byte fast path active\n");
    }
  }
  if (mp3_byte_accepted || context->piu10_mp3_frame_batch_audit_enabled) {
    std::uint32_t guest_ecx = win32_context->Ecx;
    TransferPiu10Mp3FrameTail(context, win32_context->Eip, win32_context->Esp,
                              mp3_byte, &guest_ecx);
    win32_context->Ecx = guest_ecx;
  }
  ++win32_context->Eip;
  return true;
}

bool HandlePortIoInstruction(repiu::platform::GuestCpuContext *win32_context, ThreadContext *context) {
  if (win32_context == nullptr || context == nullptr) {
    return false;
  }

  if (TryPiu10Mp3BytePortFastPath(win32_context, context)) {
    return true;
  }

  // Task 323. Reachable from the single-step HLE path (inside the VEH) and
  // from the Task 311 AOT fast-path thunk (outside it), so the bucket records
  // both and the profile tags which side each entry came from.
  const ExecutionTimeScope port_io_time_scope(
      context->execution_time_profile.get(),
      ExecutionTimeBucket::kPortIoDevice);

  std::uint32_t decode_eip = win32_context->Eip;
  // Task 405: this decision already separates "executing inside the AOT cache"
  // from "executing natively in the arena", which is exactly what the census
  // below needs, so it is kept rather than recomputed.
  const bool from_aot_cache = IsAotCacheAddress(context, win32_context->Eip);
  if (from_aot_cache) {
    if (context->aot_placement != nullptr) {
      FindAotGuestAddress(*context->aot_placement, win32_context->Eip,
                          &decode_eip);
    }
  }

  if (!IsGuestRangeReadable(context,
                            reinterpret_cast<const void *>(
                                static_cast<std::uintptr_t>(decode_eip)),
                            2U)) {
    return false;
  }

  const std::uint8_t *instruction = reinterpret_cast<const std::uint8_t *>(
      static_cast<std::uintptr_t>(decode_eip));

  bool has_prefix = (instruction[0] == 0x66);
  std::uint8_t opcode_byte = has_prefix ? instruction[1] : instruction[0];
  std::uint32_t instruction_len = has_prefix ? 2 : 1;

  if (opcode_byte != 0xEC && opcode_byte != 0xED && opcode_byte != 0xEE &&
      opcode_byte != 0xEF) {
    return false;
  }

  // Task 405: recorded after the early returns so the census counts only port
  // I/O actually handled, which is what makes it comparable with the profiled
  // `kPortIoDevice` count.
  bool mapped = false;
  if (PortIoCensusMappingEnabled() && context->aot_placement != nullptr) {
    std::uint32_t cache_address = 0;
    mapped = FindAotCacheAddress(*context->aot_placement, decode_eip,
                                 &cache_address);
  }
  // Task 407/408: one transition test feeds both the global ring and the
  // per-address sample, so the two can never disagree about what an entry is.
  // `0xC0000096` before an arena port I/O fault means the previous exception
  // was this same loop, which is the steady state rather than an entry.
  PortIoEntrySample entry_sample;
  entry_sample.is_entry =
      !from_aot_cache && context->prev_veh_code != 0xC0000096U;
  if (entry_sample.is_entry) {
    entry_sample.previous_code = context->prev_veh_code;
    entry_sample.previous_eip = context->prev_veh_eip;
    entry_sample.flags = static_cast<std::uint8_t>(
        (context->prev_veh_in_cache ? 0x01U : 0U) |
        (((win32_context->EFlags & 0x00000100U) != 0U) ? 0x02U : 0U) |
        (context->aot_reentry_pending ? 0x04U : 0U) |
        (context->aot_legacy_fallback ? 0x08U : 0U) |
        (context->enable_single_step_trace ? 0x10U : 0U));
    // Task 410: the class of the previous exception said what it was. This
    // says which VEH exit resumed the guest afterwards, and at which EIP --
    // the difference between "the consumer left EIP alone" and "the
    // consumer returned to the cache".
    entry_sample.previous_exit_site = context->prev_veh_exit_site;
    entry_sample.previous_exit_eip = context->prev_veh_exit_eip;
  }
  RecordPortIoAddress(context, decode_eip, from_aot_cache, mapped,
                      context->aot_reentry_pending, entry_sample);
  // A ring, not a prefix: the first attempt kept the first sixteen and they
  // were all consumed during boot, so the loop that costs half of wall clock
  // never appeared. Keeping the most recent sixteen shows the steady state.
  if (entry_sample.is_entry) {
    auto &entry = context->arena_port_io_entry_trace
                      [context->arena_port_io_entry_trace_count %
                       ThreadContext::kArenaPortIoEntryTraceCapacity];
    entry.guest_address = decode_eip;
    entry.previous_code = context->prev_veh_code;
    entry.previous_eip = context->prev_veh_eip;
    entry.previous_in_cache = context->prev_veh_in_cache;
    entry.trap_flag = (win32_context->EFlags & 0x00000100U) != 0U;
    entry.reentry_pending = context->aot_reentry_pending;
    entry.legacy_fallback = context->aot_legacy_fallback;
    entry.single_step_trace = context->enable_single_step_trace;
    ++context->arena_port_io_entry_trace_count;
  }

  const std::uint16_t port =
      static_cast<std::uint16_t>(win32_context->Edx & 0xFFFFU);
  const bool is_input = (opcode_byte == 0xEC || opcode_byte == 0xED);

  std::uint32_t width = 1;
  if (opcode_byte == 0xED || opcode_byte == 0xEF) {
    width = has_prefix ? 2 : 4;
  }

  const std::uint32_t opcode =
      has_prefix ? (0x6600U | opcode_byte) : opcode_byte;
  std::uint32_t value = 0;
  if (!is_input) {
    if (width == 1) {
      value = win32_context->Eax & 0xFFU;
    } else if (width == 2) {
      value = win32_context->Eax & 0xFFFFU;
    } else {
      value = win32_context->Eax;
    }
  }

  // PIU10 is a separate ISA16 flash/MP3/security board. Preserve every
  // guest access because its address, destination, and CAT702 serial state
  // are assembled across multiple OUT instructions.
  if (context->piu10_isa_board_enabled && port >= kPortPiu10IsaBase &&
      port <= kPortPiu10IsaEnd) {
    if ((width != 1U && width != 2U) || !context->piu10_isa_board.available()) {
      RecordPortIo(context, static_cast<std::uint32_t>(win32_context->Eip),
                   opcode, port, width, value, is_input, false,
                   (width != 1U && width != 2U) ? "unsupported-piu10-width"
                                                : "piu10-unavailable");
      std::ostringstream stream;
      stream << "PIU10 ISA port I/O unavailable port=0x" << std::hex
             << static_cast<unsigned>(port) << " width=" << std::dec << width;
      context->hle_message = stream.str();
      return false;
    }

    bool handled = false;
    if (is_input) {
      context->piu10_read_guest_esp = static_cast<std::uint32_t>(win32_context->Esp);
      if (width == 1U) {
        std::uint8_t read_value = 0;
        handled = context->piu10_isa_board.Read8(port, &read_value);
        value = read_value;
        if (handled) {
          win32_context->Eax = (win32_context->Eax & 0xFFFFFF00U) | read_value;
        }
      } else {
        std::uint16_t read_value = 0;
        handled = context->piu10_isa_board.Read16(port, &read_value);
        value = read_value;
        if (handled) {
          win32_context->Eax = (win32_context->Eax & 0xFFFF0000U) | read_value;
        }
      }
    } else {
      handled = width == 1U ? context->piu10_isa_board.Write8(
                                  port, static_cast<std::uint8_t>(value))
                            : context->piu10_isa_board.Write16(
                                  port, static_cast<std::uint16_t>(value));
    }

    RecordPortIo(
        context, static_cast<std::uint32_t>(win32_context->Eip), opcode, port,
        width, value, is_input, handled,
        handled ? (is_input ? "emulated-piu10-read" : "emulated-piu10-write")
                : "unsupported-piu10-register");
    if (!handled) {
      std::ostringstream stream;
      stream << "unsupported PIU10 ISA register port=0x" << std::hex
             << static_cast<unsigned>(port);
      context->hle_message = stream.str();
      return false;
    }
    win32_context->Eip += instruction_len;
    return true;
  }

  // The YMZ280B window is checked before every other PIU10 register because
  // 0x02A0..0x02A3 sits inside the JAMMA input range below, which would
  // otherwise swallow it and answer 0xFF.
  if (context->piu_jamma_board_enabled && IsPiu10SoundPort(port)) {
    Ymz280bAudioOut *audio =
        context->ymz_audio_available ? &context->ymz_audio : nullptr;
    if (is_input) {
      const std::uint32_t emulated_val = ReadPiu10SoundPort(audio, port, width);
      if (width == 1) {
        win32_context->Eax = (win32_context->Eax & 0xFFFFFF00U) | emulated_val;
      } else if (width == 2) {
        win32_context->Eax = (win32_context->Eax & 0xFFFF0000U) | emulated_val;
      } else {
        win32_context->Eax = emulated_val;
      }
      RecordPortIo(context, static_cast<std::uint32_t>(win32_context->Eip),
                   opcode, port, width, emulated_val, true, true,
                   audio != nullptr ? "emulated-ymz-read"
                                    : "ymz-unavailable-read");
    } else {
      WritePiu10SoundPort(audio, port, width, value);
      RecordPortIo(context, static_cast<std::uint32_t>(win32_context->Eip),
                   opcode, port, width, value, false, true,
                   audio != nullptr ? "emulated-ymz-write"
                                    : "ymz-unavailable-write");
    }
    // Never NOP-patch this window. Sound registers are reprogrammed
    // continuously, so latching the first access would mean permanent
    // silence -- the same reason the EEPROM and JAMMA paths advance EIP and
    // re-trap instead of patching.
    win32_context->Eip += instruction_len;
    return true;
  }

  if (is_input) {
    if (context->piu_jamma_board_enabled && port == kPortPiuEepromRead) {
      if (!g_eeprom) {
        g_eeprom = std::make_unique<Eeprom93c46>(EepromBackingPath());
      }
      std::uint32_t emulated_val = 0;
      std::uint8_t do_bit = g_eeprom->ReadData();
      std::uint8_t result_8bit = do_bit | 0xFEU;

      if (width == 1) {
        emulated_val = result_8bit;
        win32_context->Eax = (win32_context->Eax & 0xFFFFFF00U) | emulated_val;
      } else if (width == 2) {
        emulated_val = 0xFF00U | result_8bit;
        win32_context->Eax = (win32_context->Eax & 0xFFFF0000U) | emulated_val;
      } else {
        emulated_val = 0xFFFFFF00U | result_8bit;
        win32_context->Eax = emulated_val;
      }

      RecordPortIo(context, static_cast<std::uint32_t>(win32_context->Eip),
                   opcode, port, width, emulated_val, true, true,
                   "emulated-eeprom-read");
      win32_context->Eip += instruction_len;
      return true;
    }

    if (context->piu_jamma_board_enabled && port >= kPortPiuJammaBase &&
        port <= kPortPiuJammaEnd) {
      std::uint32_t emulated_val = 0;
      const std::uint64_t scan_start = repiu::platform::ReadCycleCounter();
      for (std::uint32_t i = 0; i < width; ++i) {
        emulated_val |= (static_cast<std::uint32_t>(ReadJammaPort8(
                             context, win32_context->Esp,
                             port + static_cast<std::uint16_t>(i)))
                         << (i * 8));
      }
      context->port_io.jamma_scan_cycles += repiu::platform::ReadCycleCounter() - scan_start;
      ++context->port_io.jamma_scan_count;
      context->port_io.key_query_count += TakeJammaKeyQueryCount();

      if (width == 1) {
        win32_context->Eax = (win32_context->Eax & 0xFFFFFF00U) | emulated_val;
      } else if (width == 2) {
        win32_context->Eax = (win32_context->Eax & 0xFFFF0000U) | emulated_val;
      } else {
        win32_context->Eax = emulated_val;
      }

      RecordPortIo(context, static_cast<std::uint32_t>(win32_context->Eip),
                   opcode, port, width, emulated_val, true, true,
                   "emulated-jamma");
      // JAMMA input registers are polled every frame. NOP-patching the
      // guest IN instruction (as the write/init paths do) would latch the
      // first sample forever and never observe later press/release
      // transitions, so advance EIP instead and re-trap on each poll,
      // mirroring the dynamic EEPROM read path above.
      win32_context->Eip += instruction_len;
      // Task 414: when this read is one iteration of a pure delay loop,
      // advance its counter so the guest runs only its final iteration.
      // Attempted only here, on the side-effect-free input path, and only
      // when the whole window the matcher decodes is readable.
      {
        const std::uint32_t window_start = decode_eip > kMaxLoopBodyBytes
                                               ? decode_eip - kMaxLoopBodyBytes
                                               : decode_eip;
        const bool window_readable = IsGuestRangeReadable(
            context,
            reinterpret_cast<const void *>(
                static_cast<std::uintptr_t>(window_start)),
            (decode_eip - window_start) + kMaxLoopTailBytes);
        std::uint32_t registers[8] = {win32_context->Eax, win32_context->Ecx,
                                      win32_context->Edx, win32_context->Ebx,
                                      win32_context->Esp, win32_context->Ebp,
                                      win32_context->Esi, win32_context->Edi};
        if (TryBatchPortIoDelayLoop(context, decode_eip, instruction_len, width,
                                    registers, window_readable)) {
          // The direct form changes only its matched register. The
          // wrapped form updates saved EDX on the guest stack and
          // leaves this register array unchanged.
          win32_context->Ecx = registers[1];
          win32_context->Ebx = registers[3];
          win32_context->Ebp = registers[5];
          win32_context->Esi = registers[6];
          win32_context->Edi = registers[7];
        }
      }
      return true;
    }

    RecordPortIo(context, static_cast<std::uint32_t>(win32_context->Eip),
                 opcode, port, width, value, true, false, "unsupported-in");
    std::ostringstream stream;
    stream << "unsupported port I/O IN EAX,DX port=0x" << std::hex
           << static_cast<unsigned>(port);
    context->hle_message = stream.str();
    return false;
  }

  if (width == 1U && (port == kPortPitControl || port == kPortPitChannel0)) {
    bool handled = false;
    bool configuration_changed = false;
    repiu::hle::PitChannel0Snapshot snapshot;
    if (port == kPortPitControl) {
      handled =
          context->pit_channel0.WriteControl(static_cast<std::uint8_t>(value));
    } else {
      const std::uint32_t previous_generation =
          context->pit_channel0.snapshot().generation;
      handled = context->pit_channel0.WriteData(
          static_cast<std::uint8_t>(value), &snapshot);
      configuration_changed =
          handled &&
          context->pit_channel0.snapshot().generation != previous_generation;
    }

    RecordPortIo(context, static_cast<std::uint32_t>(win32_context->Eip),
                 opcode, port, width, value, false, handled,
                 handled ? "emulated-pit-write" : "unsupported-pit-control");
    if (configuration_changed) {
      std::fprintf(
          stderr,
          "[repiu-pit] channel=0 divisor=%u frequency=%.6fHz generation=%u\n",
          snapshot.divisor, repiu::hle::PitFrequencyHz(snapshot.divisor),
          snapshot.generation);
    }

    // The Watcom runtime funnels all byte output through one OUT DX,AL
    // helper. Patching that shared instruction after the first PIT write
    // would erase the second divisor byte and every later device output.
    win32_context->Eip += instruction_len;
    return true;
  }

  // Task 735. Every OCW2 write is seen here, so a specific EOI for IRQ0
  // (0x60) ends its service too, even though only 0x20 is emulated below.
  if (width == 1U && port == kPortPicCommand) {
    NotePicCommand(&context->pic_timer_in_service,
                   static_cast<std::uint8_t>(value & 0xFFU));
  }
  if (width == 1U && port == kPortPicCommand && value == 0x20U) {
    RecordPortIo(context, static_cast<std::uint32_t>(win32_context->Eip),
                 opcode, port, width, value, false, true, "emulated-pic-eoi");
    // IRQ0 delivery is already coalesced by timer_interrupt_pending. The
    // EOI has no additional host state, but the shared OUT helper must
    // remain intact for subsequent PIT and PIU10 writes.
    win32_context->Eip += instruction_len;
    return true;
  }

  if (context->piu_jamma_board_enabled && port == kPortPiuEepromWrite) {
    if (!g_eeprom) {
      g_eeprom = std::make_unique<Eeprom93c46>(EepromBackingPath());
    }
    g_eeprom->WriteControl(static_cast<std::uint8_t>(value & 0xFF));

    RecordPortIo(context, static_cast<std::uint32_t>(win32_context->Eip),
                 opcode, port, width, value, false, true,
                 "emulated-eeprom-write");
    win32_context->Eip += instruction_len;
    return true;
  }

  if (context->piu_jamma_board_enabled &&
      IsPortIoTraceCandidate(port, width, false)) {
    if (context->port_io.observed_count >= kDeferredPortIoLimit) {
      context->port_io.trace_limit_reached = true;
      RecordPortIo(context, static_cast<std::uint32_t>(win32_context->Eip),
                   opcode, port, width, value, false, true, "deferred-limit");
      // This opcode addresses DX, so a shared output wrapper can target
      // a different device on its next call. Ignore only this access;
      // patching the instruction would suppress every later write.
      win32_context->Eip += instruction_len;
      return true;
    }

    RecordPortIo(context, static_cast<std::uint32_t>(win32_context->Eip),
                 opcode, port, width, value, false, true, "deferred-ignored");
    win32_context->Eip += instruction_len;
    return true;
  }

  RecordPortIo(context, static_cast<std::uint32_t>(win32_context->Eip), opcode,
               port, width, value, false, true, "unsupported-ignored");
  win32_context->Eip += instruction_len;
  return true;
}

} // namespace repiu::engine
