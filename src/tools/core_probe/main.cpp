// Task 501. The probes that depend on nothing platform-specific, gathered into
// one binary that builds on every host.
//
// repiu_aot_probe cannot serve this purpose: most of its 56 probes reach into
// the Win32 execution layer, so making it build elsewhere would mean excluding
// most of it and leaving one binary that verifies different things depending on
// the operating system. This target instead holds only what is genuinely
// shared, so the same assertions can be compared directly across platforms.
// Windows loses no coverage, because repiu_aot_probe still runs these same
// probes there.
//
// Membership was decided by compiling and linking, not by reading includes: a
// probe can name a header like `dos/dos_int21_services.h` that resolves into
// the Win32 layer only through an include directory, which no amount of
// grepping for "win32" reveals.

// Task 513: six of the fifteen are compiled out for wasm32, and the binary says
// so on every run rather than reporting a smaller total in silence.
//
// The reason differs by probe and both reasons are real. Two of them --
// stack_bridge and guest_stack_switch -- are inline x86 assembly and do not
// reach the compiler at all. The other four exercise the platform facilities
// wasm does not have, so on this host they would only ever measure that the
// Stage 1 stubs return false, which is a fact the stubs already state.
//
// Excluding them is not the same as their passing. `core_probe_skipped` below
// keeps that distinction on screen, because "9 of 9 passed" printed alone would
// read as a complete run.
#include "code_cache_placement_probe.h"
#include "dos_console_device_probe.h"
#include "dos_console_input_probe.h"
#include "glide_letterbox_probe.h"
#include "host_pad_input_probe.h"
#include "mesa_fx_texture_source_probe.h"
#include "dos_file_handle_cache_probe.h"
#include "env_toggle_probe.h"
#include "event_clock_probe.h"
#include "piu10_song_clock_census_probe.h"
#include "far_jump_probe.h"
#include "far_return_probe.h"
#include "final_execution_report_probe.h"
#include "shutdown_recovery_policy_probe.h"
#include "pic_timer_in_service_probe.h"
#include "glide_gate_fixup_index_probe.h"
#include "glide_gate_interrupt_exit_probe.h"
#include "gl_renderer_identity_probe.h"
#include "imgui_ui_scale_probe.h"
#include "timer_return_pad_probe.h"
#include "build_identity_probe.h"
#include "flat_stack_segment_fold_probe.h"
#include "execution_backend_probe.h"
#include "execution_timeout_probe.h"
#include "glide_lfb_region_probe.h"
#include "guest_address_space_probe.h"
#include "jump_table_guard_probe.h"
#include "launcher_probe.h"
#include "launcher_update_probe.h"
#include "long_mode_compatibility_probe.h"
#include "long_mode_emission_probe.h"
#include "low_address_reservation_probe.h"
#include "linux_x64_transfer_failure_provenance_probe.h"
#include "nvram_path_probe.h"
#include "pit_timer_probe.h"
#include "segment_push_probe.h"

#if !defined(__EMSCRIPTEN__)
#include "fault_handler_probe.h"
#include "fault_recovery_provenance_probe.h"
#include "guest_cpu_context_probe.h"
#include "host_thread_probe.h"
#include "virtual_memory_probe.h"
#if !defined(REPIU_NO_I386_PROBES)
#include "guest_stack_switch_probe.h"
#include "stack_bridge_probe.h"
#endif
#if defined(REPIU_LINUX_X64_AOT_FRAME_PROBE)
#include "linux_x64_aot_frame_probe.h"
#endif
#if defined(REPIU_LINUX_X64_LOWERING_PROBE)
#include "long_mode_lowering_probe.h"
#endif
#if defined(REPIU_LINUX_X64_GUEST_REGISTER_PROBE)
#include "linux_x64_guest_register_probe.h"
#endif
#if defined(REPIU_LINUX_X64_GENERAL_STACK_PROBE)
#include "general_stack_probe.h"
#endif
#endif

#include <cstddef>
#include <iostream>

namespace
{

struct CoreProbe
{
    const char* name;
    bool (*run)();
};

constexpr CoreProbe kCoreProbes[] = {
    {"env_toggle", &repiu::tools::RunEnvToggleProbe},
    {"far_jump", &repiu::tools::RunFarJumpProbe},
    {"far_return", &repiu::tools::RunFarReturnProbe},
    {"execution_backend", &repiu::tools::RunExecutionBackendProbe},
    {"execution_timeout", &repiu::tools::RunExecutionTimeoutProbe},
    {"dos_file_handle_cache", &repiu::tools::RunDosFileHandleCacheProbe},
    {"dos_console_device", &repiu::tools::RunDosConsoleDeviceProbe},
    // Task 764. INT 21h AH=07h/08h against the BIOS keyboard buffer.
    {"dos_console_input", &repiu::tools::RunDosConsoleInputProbe},
    // Issue #34. Pad names, defaults and the state-to-input mapping, with no
    // device attached.
    {"host_pad_input", &repiu::tools::RunHostPadInputProbe},
    // Issue #37. Finding the game's 8-bit texture originals through fake
    // Mesa structures.
    {"mesa_fx_texture_source", &repiu::tools::RunMesaFxTextureSourceProbe},
    {"pit_timer", &repiu::tools::RunPitTimerProbe},
    // Task 754. Pure arithmetic over two clock readings.
    {"event_clock", &repiu::tools::RunEventClockProbe},
    // Issue #58. The song clock overwrite replayed over synthetic reads.
    {"piu10_song_clock_census",
     &repiu::tools::RunPiu10SongClockCensusProbe},
    // Issue #58. The frame-sync bit answered at a tick's due time.
    {"piu10_frame_sync_history",
     &repiu::tools::RunPiu10FrameSyncHistoryProbe},
    {"segment_push", &repiu::tools::RunSegmentPushProbe},
    {"glide_lfb_region", &repiu::tools::RunGlideLfbRegionProbe},
    // Issue #45: the picture rectangle, stretch included.
    {"glide_letterbox", &repiu::tools::RunGlideLetterboxProbe},
    {"jump_table_guard", &repiu::tools::RunJumpTableGuardProbe},
    // Task 550. Decodes bytes and judges them; it executes nothing, so it runs
    // on every host and its answers are the same everywhere. That is the point
    // of having it here rather than under an x64 fence -- the classifier is a
    // claim about the x86-64 host, and a Windows or i386 run that disagreed
    // with the Linux x64 one would mean the claim had drifted.
    {"long_mode_compatibility",
     &repiu::tools::RunLongModeCompatibilityProbe},
    // Task 553. The emitter's side of the same claim, and here for the same
    // reason: it builds a code cache image and executes none of it, so the
    // bytes it produces for a given plan are the same on every host. A Windows
    // run disagreeing with a Linux x64 one would mean the emitter and the
    // classifier had drifted apart.
    {"long_mode_emission", &repiu::tools::RunLongModeEmissionProbe},
    // Task 709. The seam that lets a shutdown which cannot return still
    // report. Pure registration and dispatch -- it reserves nothing and faults
    // nothing -- so it sits with the computations rather than the placements.
    // Task 712. Which base an explicit SS override folds. A pure rule over one
    // resolution, so it runs everywhere; the fold table itself is built from a
    // live ThreadContext and is measured by the runs instead.
    {"flat_stack_segment_fold",
     &repiu::tools::RunFlatStackSegmentFoldProbe},
    {"final_execution_report",
     &repiu::tools::RunFinalExecutionReportProbe},
    // Task 708. The shared candidate ladder behind the AOT code cache, the
    // shadow selector block, and the Glide LFB staging surface. It reserves
    // real address ranges, so it sits with the other placement probes and ahead
    // of them: if this one fails, theirs fail for the same reason.
    {"low_address_reservation",
     &repiu::tools::RunLowAddressReservationProbe},
    {"linux_x64_transfer_failure_provenance",
     &repiu::tools::RunLinuxX64TransferFailureProvenanceProbe},
    {"nvram_path", &repiu::tools::RunNvramPathProbe},
    // Task 551. Reserves real address ranges, so it belongs after the probes
    // that only compute -- but before the ones that fault, because what it
    // measures is whether this host can give the guest its arena at all.
    {"guest_address_space", &repiu::tools::RunGuestAddressSpaceProbe},
    // Task 554. Reserves the code cache the way the engine really does, so it
    // sits with guest_address_space rather than with the pure computations: the
    // two together say whether this host can hold both halves of the execution
    // layer -- the guest's memory and the cache that runs it.
    {"code_cache_placement", &repiu::tools::RunCodeCachePlacementProbe},
#if !defined(__EMSCRIPTEN__)
    {"guest_cpu_context", &repiu::tools::RunGuestCpuContextProbe},
    {"virtual_memory", &repiu::tools::RunVirtualMemoryProbe},
    {"fault_handler", &repiu::tools::RunFaultHandlerProbe},
    {"fault_recovery_provenance",
     &repiu::tools::RunFaultRecoveryProvenanceProbe},
#if !defined(__EMSCRIPTEN__) && !defined(REPIU_NO_I386_PROBES)
    {"stack_bridge", &repiu::tools::RunStackBridgeProbe},
    {"guest_stack_switch", &repiu::tools::RunGuestStackSwitchProbe},
#endif
#if defined(REPIU_LINUX_X64_AOT_FRAME_PROBE)
    {"linux_x64_aot_frame", &repiu::tools::RunLinuxX64AotFrameProbe},
#endif
#if defined(REPIU_LINUX_X64_LOWERING_PROBE)
    {"long_mode_lowering", &repiu::tools::RunLongModeLoweringProbe},
#endif
#if defined(REPIU_LINUX_X64_GUEST_REGISTER_PROBE)
    // Task 558. The first probe that runs what the x64 emitter produced. It
    // comes after the lowering probe because it depends on that lowering being
    // right, and a failure here reads very differently once the one above has
    // passed.
    {"linux_x64_guest_register",
     &repiu::tools::RunLinuxX64GuestRegisterProbe},
#endif
#if defined(REPIU_LINUX_X64_GENERAL_STACK_PROBE)
    {"general_stack", &repiu::tools::RunGeneralStackProbe},
#endif
    {"host_thread", &repiu::tools::RunHostThreadProbe},
#endif
    {"launcher", &repiu::tools::RunLauncherProbe},
    // Issue #48: the update path, without a network.
#if !defined(__EMSCRIPTEN__)
    // Issue #67: the updater downloads releases on a thread and replaces the
    // installed files, and the web has neither.
    {"launcher_update", &repiu::tools::RunLauncherUpdateProbe},
#endif
    // Task 730. Here as well as in the Win32 AOT probe because the case it
    // exists for -- a host address aliasing guest code in its low half --
    // needs a pointer wider than 32 bits, which only the x64 hosts have.
    {"shutdown_recovery_policy",
     &repiu::tools::RunShutdownRecoveryPolicyProbe},
    // Task 735. Pure policy, so every host runs it.
    {"pic_timer_in_service", &repiu::tools::RunPicTimerInServiceProbe},
    {"timer_return_pad", &repiu::tools::RunTimerReturnPadProbe},
    // Task 773. Pure bookkeeping over the fixup array, so every host runs it.
    {"glide_gate_fixup_index",
     &repiu::tools::RunGlideGateFixupIndexProbe},
    // #6. Bytes and a replayed stack, so every host runs it.
    {"glide_gate_interrupt_exit",
     &repiu::tools::RunGlideGateInterruptExitProbe},
    // #5. String matching, no GL context, so every host runs it.
    {"gl_renderer_identity", &repiu::tools::RunGlRendererIdentityProbe},
    // #15. Arithmetic and a string, no ImGui context, so every host runs it.
    {"imgui_ui_scale", &repiu::tools::RunImGuiUiScaleProbe},
    // Task 738. The window-title label, checked against this compiler.
    {"build_identity", &repiu::tools::RunBuildIdentityProbe},
};

// Task 513. Named, not counted: a list of what this host cannot ask is worth
// more than a number, and the next reader wants to know which ones.
//
// Task 558 widened it. The x64-only probes were being left out, so a 32-bit
// host printed "19 of 19 passed" with no sign that three probes had not been
// built -- the exact reading this list exists to prevent, grown back on the
// other side.
constexpr const char* kSkippedProbes[] = {
#if defined(__EMSCRIPTEN__)
    "guest_cpu_context", "virtual_memory", "fault_handler",
    "fault_recovery_provenance", "stack_bridge",
    "guest_stack_switch", "host_thread", "launcher_update",
#elif defined(REPIU_NO_I386_PROBES)
    "stack_bridge", "guest_stack_switch",
#endif
#if !defined(REPIU_LINUX_X64_AOT_FRAME_PROBE)
    "linux_x64_aot_frame",
#endif
#if !defined(REPIU_LINUX_X64_LOWERING_PROBE)
    "long_mode_lowering",
#endif
#if !defined(REPIU_LINUX_X64_GUEST_REGISTER_PROBE)
    "linux_x64_guest_register",
#endif
#if !defined(REPIU_LINUX_X64_GENERAL_STACK_PROBE)
    "general_stack",
#endif
};

// Every configuration this project builds leaves at least one probe out, so the
// array is never empty -- which C++ would reject. Asserted rather than assumed,
// because the day a configuration excludes nothing is the day this stops
// compiling for a reason nobody would guess from the error.
static_assert(sizeof(kSkippedProbes) / sizeof(kSkippedProbes[0]) > 0);

}  // namespace

int main()
{
    // Task 549. Unbuffered, so that where the output stops is where the run
    // stopped.
    //
    // A newline flushes nothing when stdout is a pipe, and a pipe is what it is
    // whenever this runs through `wsl.exe` from a Windows session or under any
    // capture. The stream then holds whole blocks, so a run that is killed --
    // for a hang, a timeout, or an operator's patience -- loses everything the
    // buffer still held. What survives is the last flushed boundary, and
    // reading that as the stopping point names the wrong probe.
    //
    // Two bring-up sessions, Tasks 547 and 548, recorded a stop at `pit_timer`
    // on Linux x64 and left every probe after it unmeasured. `pit_timer` holds
    // no loop, no wait, and no syscall, so that attribution could not have been
    // right either way. One flush per line on a binary that prints a few dozen
    // is what rules the question out for good.
    std::cout << std::unitbuf;

    std::size_t failures = 0;
    for (const CoreProbe& probe : kCoreProbes)
    {
        // Printed before the probe runs, which is what lets a killed run name
        // the probe it was inside rather than the last one that finished.
        std::cout << "== " << probe.name << " ==\n";
        // Every probe is run even after one fails, because a platform bring-up
        // wants the whole picture rather than the first stumble.
        if (!probe.run())
        {
            ++failures;
            std::cout << "!! " << probe.name << " failed\n";
        }
    }
    const std::size_t total =
        sizeof(kCoreProbes) / sizeof(kCoreProbes[0]);
    std::cout << "core_probe_total=" << total
              << "\ncore_probe_failures=" << failures
              << "\ncore_probe_all=" << (failures == 0 ? "true" : "false")
              << "\n";
    std::cout << "core_probe_skipped="
              << sizeof(kSkippedProbes) / sizeof(kSkippedProbes[0]);
    for (const char* name : kSkippedProbes)
    {
        std::cout << ' ' << name;
    }
#if defined(__EMSCRIPTEN__)
    std::cout << "\ncore_probe_host=wasm32 (issue #67: the engine on the "
                 "emulated model; the native-execution probes are not built)\n";
#elif defined(REPIU_NO_I386_PROBES)
    std::cout << "\ncore_probe_host=x64 (Task 545: i386 assembly probes are "
                 "not built)\n";
#else
    std::cout << "\ncore_probe_host=i386 (Task 558: the x64-only probes have "
                 "nothing to test here)\n";
#endif
    return failures == 0 ? 0 : 1;
}
