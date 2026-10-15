#ifndef REPIU_RUNTIME_EXECUTION_MODEL_H_
#define REPIU_RUNTIME_EXECUTION_MODEL_H_

#include <cstdint>

// Task 759. How this host runs the guest, answered in one place.
//
// The guest is 32-bit x86 code. A 32-bit x86 host (Win32, Linux i386) runs the
// guest's own bytes in this process: the `direct` model. An x86-64 host cannot,
// because long mode reads some of those encodings differently (Task 550), so it
// runs the translated long-mode code cache instead (Task 546): the `cache`
// model. A host whose CPU executes no guest instruction at all runs the guest
// on the rex86 CPU core (issue #61): the `emulated` model, which the web build
// and ARM hosts need and a 32-bit x86 host can build to compare against
// `direct`. Issue #67: the web build runs it; the `none` model it used before,
// for a build that ran no guest, is gone.
//
// That is the one difference by architecture above the platform layer, and
// these functions are where shared code asks about it. They are implemented in
// src/runtime/execution_model_direct.cpp, execution_model_cache.cpp and
// execution_model_emulated.cpp; CMake builds the one that matches the host
// (REPIU_EXECUTION_MODEL). Code that differs by model and needs the engine's
// types sits next to its shared file as <name>_direct.cpp, <name>_cache.cpp
// and <name>_emulated.cpp.

namespace repiu::runtime::execution_model
{

// Whether the guest's own bytes may be run in this process as they are.
[[nodiscard]] bool RunsGuestBytesDirectly();

// Whether the guest is entered through the emitted long-mode code cache, so
// that what is emitted for it must be long-mode code. Not the negation of the
// one above: the `emulated` model answers false to both.
[[nodiscard]] bool RunsLongModeCodeCache();

// Issue #61. Whether the guest runs on the rex86 CPU core, which stops where a
// Win32 user-mode process would fault and hands those stops to the same
// dispatch. False on the other models; like the two above, not the negation
// of either.
[[nodiscard]] bool RunsGuestOnEmulatedCpu();

// Issue #64. Whether the host runs the guest, the Glide commands, the timer
// schedule and the audio on one thread, taking turns, as the browser's single
// Worker has to. True on the emulated model only: it has the same value as the
// question above, but the audio and Glide code asks whether there are threads,
// not what the CPU is.
[[nodiscard]] bool RunsHostOnOneThread();

// Task 562. Where an emitted return goes to ask where a guest address lives.
// Zero where the thunk does not exist, and zero is the answer that matters
// rather than a missing case: a host without it emits the boundary it emitted
// before.
[[nodiscard]] std::uintptr_t LongModeReturnThunkAddress();

// Task 750. Whether owed timer ticks can be injected while grBufferSwap waits.
// The injected frame returns to the call that reached the gate, which the
// cache model resumes through the cache. Since #6 the direct model does it
// too: its gate thunk leaves into the handler through the interrupt exit
// (glide_gate_interrupt_exit.h). Before that the first injected tick never
// returned to the call, because the interrupt frame lay over the thunk's own
// frame on the guest stack.
[[nodiscard]] bool InjectsTicksDuringSwapWait();

}  // namespace repiu::runtime::execution_model

#endif  // REPIU_RUNTIME_EXECUTION_MODEL_H_
