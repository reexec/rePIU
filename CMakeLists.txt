cmake_minimum_required(VERSION 3.20)

project(rePIU LANGUAGES C CXX)

# Task 503d-2: NOMINMAX for every Windows target.
#
# It was already wanted project-wide, but was arriving by accident: one header,
# thread_context.h, defined it before including <windows.h>, and nearly
# everything included that header. Removing that include so the header could
# compile under GCC let the min/max macros loose, and they break std::max at the
# call site with an error that names neither. Declaring it here is what was
# meant all along.
if(WIN32)
    add_compile_definitions(NOMINMAX)
endif()

# Task 758: the platform and architecture every platform-specific source list
# below is chosen by. Platform code lives in <dir>/<platform>/ and
# <dir>/<platform>/<arch>/ (see AGENTS.md); the web build reuses the linux/
# POSIX files that have no reason to differ, as it already did for
# host_environment and worker_signal (Task 513).
if(EMSCRIPTEN)
    set(REPIU_PLATFORM web)
    set(REPIU_ARCH wasm32)
elseif(WIN32)
    set(REPIU_PLATFORM win32)
    set(REPIU_ARCH x86)
elseif(UNIX)
    set(REPIU_PLATFORM linux)
    if(CMAKE_SIZEOF_VOID_P EQUAL 4)
        set(REPIU_ARCH x86)
    else()
        set(REPIU_ARCH x64)
    endif()
else()
    message(FATAL_ERROR "rePIU builds for Win32, Linux and the web only")
endif()

# Task 759: how the host runs the guest. A 32-bit x86 host runs the guest's
# bytes as they are (direct); an x86-64 host runs the translated long-mode code
# cache (cache). The engine's sources that differ by it are named
# <name>_direct.cpp and <name>_cache.cpp and sit next to their shared file; the
# engine has no platform or architecture subdirectories (see AGENTS.md).
# Issue #67: the web runs the guest on the rex86 CPU core (emulated), the model
# issue #61 made for it; until then it built the core alone (none).
if(REPIU_ARCH STREQUAL "x86")
    set(REPIU_EXECUTION_MODEL direct)
elseif(REPIU_ARCH STREQUAL "x64")
    set(REPIU_EXECUTION_MODEL cache)
else()
    set(REPIU_EXECUTION_MODEL emulated)
endif()

# Issue #61: the emulated model runs the guest on the rex86 CPU core, so this
# process executes none of its bytes. A 32-bit x86 host builds it on request,
# in its own build tree, to compare it against direct.
set(REPIU_EXECUTION_MODEL_OVERRIDE "" CACHE STRING
    "Build this execution model instead of the host's (only: emulated)")
if(REPIU_EXECUTION_MODEL_OVERRIDE)
    if(NOT REPIU_EXECUTION_MODEL_OVERRIDE STREQUAL "emulated")
        message(FATAL_ERROR
            "REPIU_EXECUTION_MODEL_OVERRIDE must be empty or emulated")
    endif()
    if(NOT REPIU_ARCH STREQUAL "x86")
        message(FATAL_ERROR
            "the emulated model is built on a 32-bit x86 host for now")
    endif()
    set(REPIU_EXECUTION_MODEL emulated)
endif()
message(STATUS "rePIU execution model: ${REPIU_EXECUTION_MODEL}")

# Task 767: release builds link the C++ runtime statically so the Linux
# archives run on distributions whose libstdc++ is older than the build host's.
# Off by default: local builds keep the system runtime.
option(REPIU_STATIC_RUNTIME
    "Link libstdc++ and libgcc statically (Linux release archives)" OFF)
if(REPIU_STATIC_RUNTIME AND UNIX AND NOT EMSCRIPTEN)
    add_link_options(-static-libstdc++ -static-libgcc)
endif()

# Task 503d: the AOT/DBT dispatch thunks are hand-written assembly on Linux,
# because GCC has no __declspec(naked) on x86.
#
# Task 513: `AND NOT EMSCRIPTEN` here and at every other `if(UNIX)` below.
# CMake sets UNIX for an Emscripten toolchain, so each of those guards would
# otherwise be taken for the web build and pull in x86 assembly, -m32, and the
# Linux platform layer -- none of which mean anything for wasm32. The guards
# were written when UNIX and "the Linux host" were the same thing; they are not
# any more, and this is the whole of what that change costs.
if(UNIX AND NOT EMSCRIPTEN)
    enable_language(ASM)
endif()

set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/VERSION"
)
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" REPIU_VERSION
    LIMIT_COUNT 1)
string(STRIP "${REPIU_VERSION}" REPIU_VERSION)
if(NOT REPIU_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
    message(FATAL_ERROR "VERSION must use major.minor.patch format")
endif()

include(FetchContent)

set(INSTALL_STATIC_LIBS OFF CACHE BOOL "" FORCE)
set(CHDR_WANT_RAW_DATA_SECTOR ON CACHE BOOL "" FORCE)
set(CHDR_WANT_SUBCODE OFF CACHE BOOL "" FORCE)
# libchdr already bundles miniz. Turning on its ZIP reader lets us extract MAME
# PIU10 ROM-set members without vendoring a second copy of miniz.
# The file-based ZIP APIs stay off, so archives are read through memory buffers.
set(MINIZ_ARCHIVE_APIS ON CACHE BOOL "" FORCE)
add_subdirectory(third_party/libchdr EXCLUDE_FROM_ALL)
set(REPIU_MINIZ_INCLUDE_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/libchdr/deps/miniz-3.1.1")

set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    sdl3
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    GIT_TAG release-3.4.10
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(sdl3)

FetchContent_Declare(
    minimp3
    GIT_REPOSITORY https://github.com/lieff/minimp3.git
    GIT_TAG ea99364f61c14656440e8d77e9c233ccf3124633
)
FetchContent_MakeAvailable(minimp3)

# Issue #61: the rex86 CPU core, pinned to a release tag (rex86 design #1,
# decision 6). Changing both repositories together, point
# FETCHCONTENT_SOURCE_DIR_REX86 at a sibling checkout.
if(REPIU_EXECUTION_MODEL STREQUAL "emulated")
    FetchContent_Declare(
        rex86
        GIT_REPOSITORY https://github.com/reexec/rex86.git
        GIT_TAG v0.0.34
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(rex86)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Task 513: spdlog is not fetched for Emscripten, and this is a scope decision
# rather than a workaround.
#
# Its only consumer is the `repiu` loader, which is a Windows and Linux target.
# But FetchContent_MakeAvailable adds spdlog's compiled library to `all`, so a
# bare `cmake --build` builds a dependency no wasm target links -- and spdlog
# 1.14.1 vendors a copy of fmt that its FMT_COMPILE_STRING macro cannot get past
# Emscripten's clang. Fetching it here would mean carrying a version bump for a
# library the web build never uses.
#
# `spdlog::spdlog_header_only` is named only inside the WIN32 and Linux target
# blocks, so the missing target is unreachable rather than merely unused.
#
# Issue #67: the loader is built for the web too now, so spdlog is fetched there
# as well, with std::format in place of the bundled fmt that Emscripten's clang
# could not compile.
if(EMSCRIPTEN)
    set(SPDLOG_USE_STD_FORMAT ON CACHE BOOL "" FORCE)
    FetchContent_Declare(
        spdlog
        GIT_REPOSITORY https://github.com/gabime/spdlog.git
        GIT_TAG v1.14.1
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(spdlog)
else()
    find_package(spdlog CONFIG QUIET)
    if(NOT spdlog_FOUND)
        FetchContent_Declare(
            spdlog
            GIT_REPOSITORY https://github.com/gabime/spdlog.git
            GIT_TAG v1.14.1
            GIT_SHALLOW TRUE
        )
        FetchContent_MakeAvailable(spdlog)
    endif()
endif()

# Task 412: Release keeps every optimisation flag and gains debug information
# only, so code generation is unchanged while host-side samples in the guest
# position census can be resolved to function names. Declared before any target
# so it applies to all of them.
if(MSVC)
    add_compile_options($<$<CONFIG:Release>:/Zi>)
    add_link_options($<$<CONFIG:Release>:/DEBUG>
                     $<$<CONFIG:Release>:/OPT:REF>
                     $<$<CONFIG:Release>:/OPT:ICF>)
endif()

# Issue #61: rex86 builds the same Zydis amalgamation (4.1.1), and two copies
# in one link collide, so a build with rex86 uses rex86's.
if(TARGET rex86_zydis)
    add_library(repiu_zydis INTERFACE)
    target_link_libraries(repiu_zydis INTERFACE rex86_zydis)
else()
add_library(repiu_zydis STATIC
    third_party/zydis/Zydis.c
)
target_include_directories(repiu_zydis
    PUBLIC
        third_party/zydis
)
target_compile_definitions(repiu_zydis
    PUBLIC
        ZYDIS_STATIC_BUILD
)
endif()

function(repiu_configure_win32_loader_host target_name)
    if(MSVC AND WIN32 AND CMAKE_SIZEOF_VOID_P EQUAL 4)
        target_link_options(${target_name}
            PRIVATE
                /BASE:0x10000000
                /DYNAMICBASE:NO
        )
        target_compile_definitions(${target_name}
            PRIVATE
                REPIU_WIN32_HOST_IMAGE_BASE=0x10000000
        )
    endif()
endfunction()

# Task 500: the launcher draws inside our own SDL window instead of using a
# native toolkit, because every cross-platform native toolkit that would serve
# Linux is LGPL-family and the project forbids copyleft dependencies. Dear ImGui
# is MIT and ships the SDL3 and OpenGL3 backends this host already has.
FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    # #15: the revision re2DJ's OSD uses, for ProggyForever, the scalable
    # default font (AddFontDefaultVector) the OSD and the launcher draw with.
    GIT_TAG 01380c579715e62fb9a8d6ec0502c4ea83bfde6e  # v1.92.9
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(imgui)

# ImGui ships sources rather than a CMake target, so the sources it needs are
# collected here: the core, plus only the two backends in use.
add_library(repiu_imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
)
target_include_directories(repiu_imgui
    PUBLIC
        ${imgui_SOURCE_DIR}
        ${imgui_SOURCE_DIR}/backends
)
target_link_libraries(repiu_imgui PUBLIC SDL3::SDL3-static)

add_library(repiu_exe
    src/assets/piu_chd_mount.cpp
    src/assets/rom_zip_archive.cpp
    src/config/ini_document.cpp
    src/config/romset_config.cpp
    src/config/romset_config_template.cpp
    src/input/host_key_binding.cpp
    src/input/host_key_names.cpp
    src/input/host_pad_binding.cpp
    src/input/host_pad_state.cpp
    src/input/browser_key_codes.cpp
    src/input/pad_exit_chord.cpp
    src/input/pad_osd_chord.cpp
    src/input/jamma_input_bindings.cpp
    src/input/jamma_input_key.cpp
    src/exe/dos16m_bound_module.cpp
    src/exe/dos4gw_loader.cpp
    src/exe/executable_headers.cpp
    src/hle/dos_file_system.cpp
    src/hle/dos_date.cpp
    src/hle/bios_keyboard.cpp
    src/hle/glide_fog.cpp
    src/hle/glide_hle.cpp
    src/hle/glide_implementation_issue.cpp
    src/hle/glide_lfb.cpp
    src/hle/glide_lfb_region.cpp
    src/hle/glide_texture_decode.cpp
    src/hle/glide_vertex.cpp
    src/hle/mesa_fx_texture_source.cpp
    src/hle/hle_dispatcher.cpp
    src/hle/hle_profile.cpp
    src/hle/linexe_call_gate.cpp
    src/hle/pit_timer.cpp
    src/hle/piu10_isa_board.cpp
    src/hle/privileged_instruction.cpp
    src/media/chd_cd_image.cpp
    src/sound/ymz280b_device.cpp
    src/sound/ymz280b_sample_rom.cpp
    src/sound/dac3350a_control.cpp
    src/sound/decoder_input_fifo.cpp
    src/sound/mpeg_audio_frame.cpp
    src/sound/spsc_byte_ring.cpp
    src/sound/stream_chunk_audit.cpp
    src/runtime/guest_context.cpp
    src/runtime/aot_code_cache.cpp
    src/runtime/aot_segment_patch.cpp
    src/runtime/aot_shadow_selector_block.cpp
    src/runtime/aot_direct_return_table.cpp
    src/launcher/rom_set_catalog.cpp
    src/launcher/launcher_settings.cpp
    # Issue #48: the launcher's update check and in-place install.
    src/update/launcher_updater.cpp
    src/update/release_archive.cpp
    src/update/release_info.cpp
    src/update/semantic_version.cpp
    src/update/sha256.cpp
    src/update/update_install.cpp
    src/launcher/command_line_options.cpp
    src/runtime/aot_translation_plan.cpp
    src/runtime/aot_code_cache_reservation.cpp
    src/runtime/low_address_reservation.cpp
    src/runtime/aot_long_mode_compatibility.cpp
    src/runtime/dos_low_memory.cpp
    src/runtime/env_toggle.cpp
    src/runtime/execution_backend.cpp
    src/runtime/execution_model_${REPIU_EXECUTION_MODEL}.cpp
    src/runtime/execution_timeout.cpp
    src/runtime/image_address.cpp
    src/runtime/runtime_memory_arena.cpp
    src/runtime/runtime_memory.cpp
    src/runtime/selector_table.cpp
    src/runtime/guest_stack_access.cpp
    src/storage/nvram_path.cpp
    src/target/target_profile.cpp
    src/platform/guest_stack_switch_state.cpp
    src/platform/build_identity.cpp
    src/platform/host_thread.cpp
    src/platform/host_time.cpp
    src/platform/memory_change_observer.cpp
    src/platform/fault_report_state.cpp
)

# Task 501 put the execution engine behind if(WIN32) because none of it built
# anywhere else. Task 503 moved it, one sub-stage at a time.
#
# Task 503d-17: no longer inside if(WIN32). Every one of these compiles on Linux
# as of 3d-16, and linking them measured zero undefined symbols of the engine's
# own -- what Win32 API remains is behind a fence in the file that carries it.
#
# The five with Linux counterparts (fault_handler_win32, virtual_memory_win32,
# worker_signal_win32, safe_memory_copy_win32, host_environment_win32) compile
# to empty objects there. Keeping one list rather than two is deliberate: a
# split list makes every new file a question about which half it belongs in,
# and the answer already lives in the fence inside the file.
#
# Task 513: excluded from the Emscripten configuration, and this is the one
# place the web build differs in kind rather than in detail. These sources are
# the execution engine, and the engine executes guest x86 on the host CPU --
# an RX code cache, INT3 sentinels, trap-flag single-stepping, hand-written
# thunks. Not one of those has a wasm form. The Linux stage could take the whole
# list because Linux still runs x86; the web stage cannot, and Stage 3 is where
# the engine gets a shape that does not need to.
#
# So the wasm32 build is `repiu_exe`'s platform-neutral core alone, which is
# exactly what Stage 1 sets out to prove portable.
#
# The list below is deliberately left at its original indentation rather than
# shifted inside this guard: re-indenting 82 lines would bury a two-line change
# in an 84-line diff, and every later `git blame` on this list would stop here.
#
# Issue #67: the guard is gone. The web runs the emulated model, whose engine
# executes no guest byte on the host CPU, so the list builds there too.
target_sources(repiu_exe
    PRIVATE
        src/engine/cd_audio_wave_out.cpp
        src/engine/aot_code_cache.cpp
        src/engine/aot_cache_address_index.cpp
        src/engine/aot_glide_gate_fixup_index.cpp
        src/engine/glide_gate_interrupt_exit.cpp
        src/engine/gl_renderer_identity.cpp
        src/engine/session_display_preferences.cpp
        src/engine/session_identity.cpp
        src/engine/aot_inline_cache_site_index.cpp
        src/engine/aot_return_dispatch_site_index.cpp
        src/engine/aot_return_patch_policy.cpp
        src/engine/aot/aot_worker_timing.cpp
        src/engine/aot/aot_timer_source_profile.cpp
        src/engine/aot_page_coherence.cpp
        src/engine/aot/aot_boundary_provenance.cpp
        src/engine/aot/aot_boundary_reason.cpp
        src/engine/aot/aot_dbt_call_return_trace.cpp
        src/engine/aot/aot_dbt_dispatch.cpp
        src/engine/aot/aot_dbt_direct_edge_dispatch.cpp
        src/engine/aot/aot_dbt_hle_dispatch.cpp
        src/engine/aot/aot_dbt_glide_gate_dispatch.cpp
        src/engine/aot/aot_dbt_return_dispatch.cpp
        src/engine/aot/aot_retired_trap_profile.cpp
        src/engine/aot/aot_guard_compare_fault.cpp
        src/engine/aot/aot_runtime_dispatch.cpp
        src/engine/bios/bios_keyboard_services.cpp
        src/engine/input/sdl_bios_keyboard_adapter.cpp
        src/engine/input/active_jamma_bindings.cpp
        src/engine/input/sdl_pad_input.cpp
        src/engine/execution/execution_probe_memory_dump.cpp
        src/engine/execution/execution_trampoline.cpp
        src/engine/execution/host_turn.cpp
        src/engine/execution/single_step_hotspot_profile.cpp
        src/engine/dos/dos_int21_services.cpp
        src/engine/dos/dpmi_mscdex_services.cpp
        src/engine/exception/breakpoint_evidence.cpp
        src/engine/exception/guest_owned_breakpoint.cpp
        src/engine/cpu_emul/guest_memory_access.cpp
        src/engine/cpu_emul/instruction_emulation.cpp
        src/engine/cpu_emul/interrupt_return.cpp
        src/engine/cpu_emul/mode16_far_return.cpp
        src/engine/cpu_emul/mode16_stack_push.cpp
        src/engine/cpu_emul/low_memory_string_access.cpp
        src/engine/boundary/glide_lfb_guest_storage.cpp
        src/engine/boundary/linexe_glide_boundary.cpp
        src/engine/boundary/timer_interrupt_boundary.cpp
        src/engine/telemetry/final_execution_report.cpp
        src/engine/telemetry/hle_boundary_trace.cpp
        src/engine/telemetry/live_telemetry_snapshot.cpp
        src/engine/telemetry/execution_time_profile.cpp
        src/engine/telemetry/live_execution_profile_report.cpp
        src/engine/telemetry/cd_audio_position_census.cpp
        src/engine/telemetry/mscdex_command_trace.cpp
        src/engine/telemetry/guest_position_census.cpp
        src/engine/telemetry/glide_gate_timing.cpp
        src/engine/telemetry/glide_ordinal_timing.cpp
        src/engine/telemetry/glide_buffer_swap_timing.cpp
        src/engine/telemetry/glide_lfb_timing.cpp
        src/engine/telemetry/glide_lfb_write_footprint.cpp
        src/engine/telemetry/glide_lfb_native_store_census.cpp
        src/engine/glide_lfb_staging_shadow.cpp
        src/engine/glide_lfb_lock_interval_census.cpp
        src/engine/execution/shutdown_recovery_policy.cpp
        src/engine/telemetry/glide_setter_state_model.cpp
        src/engine/telemetry/glide_setter_state_census.cpp
        src/engine/telemetry/glide_async_present.cpp
        src/engine/telemetry/glide_draw_batch.cpp
        src/engine/telemetry/glide_setter_state_cache.cpp
        src/engine/telemetry/glide_setter_phase_timing.cpp
        src/engine/telemetry/timer_tick_delivery.cpp
        src/engine/telemetry/piu10_song_clock_census.cpp
        src/engine/telemetry/timer_latency_trace.cpp
        src/engine/telemetry/piu10_frame_sync_history.cpp
        src/engine/telemetry/guest_address_watch.cpp
        src/engine/telemetry/guest_write_trace.cpp
        src/engine/telemetry/fault_exit_trace.cpp
        src/engine/telemetry/fault_recovery_provenance.cpp
        src/engine/telemetry/linux_x64_transfer_failure_provenance.cpp
        src/engine/telemetry/aot_residency_sample.cpp
        src/engine/telemetry/aot_return_stage_profile.cpp
        src/engine/aot/aot_boundary_opcode_census.cpp
        src/engine/aot/aot_ff_boundary_attribution.cpp
        src/engine/aot/aot_ff_boundary_target_attribution.cpp
        src/engine/aot/aot_ff_target_timing.cpp
        src/engine/io/port_io_emulator.cpp
        src/engine/io/jamma_input_timeline.cpp
        src/engine/io/pic_timer_in_service.cpp
        src/engine/io/timer_return_pad.cpp
        src/engine/io/piu10_mp3_frame_batch.cpp
        src/engine/io/port_io_delay_loop.cpp
        src/engine/io/eeprom_93c46.cpp
        src/engine/io/piu10_sound_port.cpp
        src/engine/ymz280b_audio_out.cpp
        src/engine/piu10_mp3_audio_out.cpp
        src/engine/native_fast_path.cpp
        src/engine/native_linear_span.cpp
        src/engine/native_phase_sampler.cpp
        src/engine/verified_region_analyzer.cpp
        src/engine/glide_gl_error_policy.cpp
        src/engine/glide_swap_interval_policy.cpp
        src/engine/sdl_input_script.cpp
        src/engine/glide_texture_census.cpp
        src/engine/out_of_arena_step_census.cpp
        src/engine/glide_opengl_backend.cpp
        src/engine/glide_opengl_shader.cpp
        # Issue #69: the GL 3.3 core / ES 3.0 subset the renderer draws with.
        src/engine/gl_api.cpp
        src/engine/gl_shader_dialect.cpp
        src/engine/gl_unit_quad.cpp
        src/engine/glide_vertex_stream.cpp
        src/engine/glide_framebuffer_pair.cpp
        # Issue #72: the window the backend draws into, SDL's or the page's.
        src/engine/glide_host_window_sdl.cpp
        src/engine/glide_host_window_browser.cpp
        src/engine/glide_osd.cpp
        src/engine/imgui_ui_scale.cpp
        src/engine/glide_post_process.cpp
        src/engine/glide_letterbox.cpp
        src/engine/post_shader_source.cpp
        src/engine/post_shader_catalog.cpp
        src/engine/runtime_memory_policy.cpp
)

# Task 759: the engine's sources that differ by execution model or exist for
# one host. The engine has no platform or architecture subdirectories; what
# depends on the OS is in the platform layer below. Issue #67: built for the
# web too, on the emulated model.
if(TRUE)
    if(REPIU_PLATFORM STREQUAL "win32")
        target_sources(repiu_exe
            PRIVATE
                src/engine/x87_context.cpp
                src/engine/exception/exception_rescue_win32.cpp
        )
    endif()
    # Task 759: the sources that differ by execution model.
    target_sources(repiu_exe
        PRIVATE
            src/engine/aot/aot_dbt_dispatch_thunks_${REPIU_EXECUTION_MODEL}.cpp
            src/engine/execution/execution_trampoline_${REPIU_EXECUTION_MODEL}.cpp
            src/engine/native_phase_sampler_${REPIU_EXECUTION_MODEL}.cpp
    )
    # A diagnostic of the Linux x64 host.
    if(REPIU_PLATFORM STREQUAL "linux" AND REPIU_ARCH STREQUAL "x64")
        target_sources(repiu_exe
            PRIVATE
                src/engine/telemetry/linux_x64_native_write_trace.cpp
        )
    endif()
endif()

# Task 758: the platform layer's per-platform sources. Before this, the six
# Win32 files above sat in the engine list and compiled to empty objects on
# Linux behind whole-file #if guards.
if(REPIU_PLATFORM STREQUAL "win32")
    target_sources(repiu_exe
        PRIVATE
            src/platform/win32/virtual_memory_win32.cpp
            src/platform/win32/fault_handler_win32.cpp
            src/platform/win32/worker_signal_win32.cpp
            src/platform/win32/safe_memory_copy_win32.cpp
            src/platform/win32/host_environment_win32.cpp
            src/platform/win32/host_process_win32.cpp
            src/platform/win32/https_download_win32.cpp
            src/platform/win32/host_thread_win32.cpp
            src/platform/win32/host_time_win32.cpp
            src/platform/win32/host_error_stream_win32.cpp
            src/platform/win32/build_identity_win32.cpp
    )
    # Task 759: the thunks and the stack switch, MSVC inline assembly. The
    # Linux i386 counterparts are the .S files in src/platform/linux/x86/.
    if(NOT EMSCRIPTEN)
        target_sources(repiu_exe
            PRIVATE
                src/platform/win32/aot_dbt_dispatch_thunks_win32.cpp
                src/platform/win32/guest_stack_switch_win32.cpp
        )
    endif()
    # Task 759: what the engine asks of the host beyond running the guest.
    if(NOT EMSCRIPTEN)
        target_sources(repiu_exe
            PRIVATE
                src/platform/win32/host_cpu_entry_state_win32.cpp
                src/platform/win32/host_crash_report_win32.cpp
                src/platform/win32/host_fault_report_win32.cpp
                src/platform/win32/host_gpu_driver_win32.cpp
                src/platform/win32/host_symbols_win32.cpp
                src/platform/win32/host_telemetry_win32.cpp
                src/platform/win32/host_trace_descriptor_win32.cpp
        )
    endif()
else()
    # POSIX, shared by the Linux and web builds.
    target_sources(repiu_exe
        PRIVATE
            src/platform/linux/host_time.cpp
            src/platform/linux/host_error_stream.cpp
    )
    # Issue #67: the web has no threads (it links without pthreads), so its
    # thread layer is its own.
    if(EMSCRIPTEN)
        target_sources(repiu_exe PRIVATE src/platform/web/host_thread.cpp)
    else()
        target_sources(repiu_exe PRIVATE src/platform/linux/host_thread.cpp)
    endif()
endif()
if(REPIU_PLATFORM STREQUAL "linux")
    target_sources(repiu_exe
        PRIVATE
            # Task 759: what the engine asks of the host beyond running the guest.
            src/platform/linux/host_cpu_entry_state.cpp
            src/platform/linux/host_crash_report.cpp
            src/platform/linux/host_fault_report.cpp
            src/platform/linux/host_gpu_driver.cpp
            src/platform/linux/${REPIU_ARCH}/host_gpu_driver_arch.cpp
            src/platform/linux/host_symbols.cpp
            src/platform/linux/host_telemetry.cpp
            src/platform/linux/host_trace_descriptor.cpp
            src/platform/linux/host_cycle_counter.cpp
            src/platform/linux/build_identity.cpp
            src/platform/linux/${REPIU_ARCH}/build_identity.cpp
    )
endif()
if(REPIU_PLATFORM STREQUAL "web")
    target_sources(repiu_exe
        PRIVATE
            src/platform/web/host_cycle_counter.cpp
            src/platform/web/build_identity.cpp
            # Issue #67: what the engine asks of the host beyond running the
            # guest, now that the engine is built here.
            src/platform/web/host_cpu_entry_state.cpp
            src/platform/web/host_reports.cpp
            src/platform/web/guest_stack_switch.cpp
            src/platform/linux/host_trace_descriptor.cpp
            # Issue #72: the page's canvas and event loop.
            src/platform/web/browser_host.cpp
    )
else()
    # Issue #72: Win32 and Linux have no browser canvas or event loop.
    target_sources(repiu_exe PRIVATE src/platform/browser_host_none.cpp)
endif()

# Task 513: the web platform layer.
#
# Two of its files are not here. `host_environment.cpp` and `worker_signal.cpp`
# are taken from `src/platform/linux/` unchanged, because both open with
# `#if !defined(_WIN32)` and both are plain POSIX inside -- `environ`/`setenv`
# and a mutex with a condition variable. A second copy under a `web` path would
# read more accurately and diverge the first time one of them is fixed.
#
# `safe_memory_copy` cannot be shared the same way: the Linux one calls
# `process_vm_readv`, which Emscripten does not have, and wasm needs a bounds
# check rather than a fault-tolerant read.
#
# The remaining four are stubs that return failure and say why, once. See
# src/platform/web/README.md for why none of them pretends to succeed.
if(EMSCRIPTEN)
    target_sources(repiu_exe
        PRIVATE
            src/platform/linux/host_environment.cpp
            src/platform/linux/worker_signal.cpp
            src/platform/web/safe_memory_copy.cpp
            src/platform/web/guest_cpu_context.cpp
            src/platform/web/fault_handler.cpp
            src/platform/web/virtual_memory.cpp
            src/platform/web/host_process.cpp
            src/platform/web/https_download.cpp
    )
endif()

# Task 503d: the Linux binaries are linked non-PIE with their text segment
# lifted to 0x40000000.
#
# Both halves of that matter. Hand-written thunks address globals directly, the
# way the MSVC originals do, and in a PIE that produces a text relocation --
# link-time DT_TEXTREL, which leaves the text segment writable and so defeats
# the page protection this engine detects self-modifying code with. Linking
# non-PIE removes the problem at the source.
#
# But plain -no-pie puts an i386 image at 0x08048000, which sits inside the
# 0x01000000-0x09000000 range the guest's relocated image needs. Lifting the
# text segment keeps that range free.
function(repiu_link_linux_engine target)
    if(UNIX AND NOT EMSCRIPTEN)
        target_compile_options(${target} PRIVATE -fno-pie)
        target_link_options(${target} PRIVATE
            -no-pie
            -Wl,-Ttext-segment=0x40000000
        )
    endif()
endfunction()

# Task 503a-503c: the Linux platform layer. It holds the guest register context
# conversion, the virtual memory API, and the signal-based fault handler.
#
# Task 757: src/platform/linux/ holds what i386 and x86-64 share; what differs
# lives in x86/ and x64/, chosen here by pointer size. Each architecture file
# checks its own architecture with #error, so a wrong pick fails to compile.
if(UNIX AND NOT EMSCRIPTEN)
    target_sources(repiu_exe
        PRIVATE
            src/platform/linux/guest_cpu_context.cpp
            src/platform/linux/virtual_memory.cpp
            src/platform/linux/fault_handler.cpp
            src/platform/linux/fault_report_writer.cpp
            src/platform/linux/worker_signal.cpp
            src/platform/linux/safe_memory_copy.cpp
            src/platform/linux/host_environment.cpp
            src/platform/linux/host_process.cpp
            src/platform/linux/https_download.cpp
    )

    # Task 545: the assembly units below are i386 execution contracts. They
    # use 32-bit registers, stack frames, and selector state; collecting them
    # in an x64 build does not provide a partial port and only hides the next
    # C++/link barrier behind an assembler failure.
    if(CMAKE_SIZEOF_VOID_P EQUAL 4)
        target_sources(repiu_exe
            PRIVATE
                src/platform/linux/x86/guest_cpu_context.cpp
                src/platform/linux/x86/fault_handler_arch.cpp
                # Task 503d-12: the five dispatch thunks. Their resolvers are
                # still Win32-only sources, so nothing on Linux references these
                # symbols yet and the archive member simply goes unused until it
                # does.
                src/platform/linux/x86/aot_dbt_dispatch_thunks.S
                # Task 503d-16: the trampoline's three entries into the guest.
                # These do link today -- the globals they read moved to a
                # source built on every host, so the probe can call them.
                src/platform/linux/x86/guest_stack_switch.S
        )
    endif()

    # Task 562: the x64 return-dispatch thunk and the three pointers it reads.
    # The mirror of the fence above -- this assembly is a long-mode contract and
    # means nothing in a 32-bit build.
    if(CMAKE_SIZEOF_VOID_P GREATER 4)
        target_sources(repiu_exe
            PRIVATE
                src/platform/linux/x64/guest_cpu_context.cpp
                src/platform/linux/x64/fault_handler_arch.cpp
                src/platform/linux/x64/aot_dbt_dispatch_x64.cpp
                src/platform/linux/x64/aot_dbt_return_thunk_x64.S
                src/platform/linux/x64/aot_dbt_glide_gate_thunk_x64.S
                # Task 575: the two fault-recovery symbols the fence above
                # takes away. They are the only ones an x64 `repiu` link was
                # missing, and they are `ud2` here rather than a return --
                # nothing on x64 can reach them, and the file says why.
                src/platform/linux/x64/guest_stack_recover_x64.S
                # Task 578: the guest entry itself. Not the guest's own bytes --
                # the placed long-mode cache, with guest state loaded under Task
                # 558's mapping.
                src/platform/linux/x64/guest_entry_x64.S
        )
    endif()
endif()

target_include_directories(repiu_exe
    PUBLIC
        include
    PRIVATE
        # Task 520: the engine's own module headers live in subsystem
        # subdirectories and are included by short name ("foo.h"), so each one
        # is on the private include path. The Windows backend is not here: the
        # engine reaches it only through include/repiu/platform/win32, which is
        # what keeps the dependency one-way.
        src/engine
        src/engine/execution
        src/engine/exception
        src/engine/io
        src/engine/input
        src/engine/dos
        src/engine/bios
        src/engine/cpu_emul
        src/engine/aot
        src/engine/boundary
        src/engine/telemetry
        # miniz ships inside libchdr, which links it PRIVATE, so the headers do
        # not propagate even though the symbols do.
        ${REPIU_MINIZ_INCLUDE_DIR}
        ${minimp3_SOURCE_DIR}
)

# Task 503d-17: miniz publishes MINIZ_NO_* on its own target, and taking its
# include directory above without them leaves the header describing an API the
# library does not contain. GCC then materialises the unused inline deflate
# wrappers and nine symbols go undefined at link.
#
# The definitions are read from the target rather than spelled again here. The
# options that produce them are miniz's, and a copy of their consequences goes
# stale the first time one is turned on.
set(REPIU_MINIZ_DEFINITIONS "")
if(TARGET miniz)
    get_target_property(REPIU_MINIZ_DEFINITIONS miniz
                        INTERFACE_COMPILE_DEFINITIONS)
    if(NOT REPIU_MINIZ_DEFINITIONS)
        set(REPIU_MINIZ_DEFINITIONS "")
    endif()
endif()

target_compile_definitions(repiu_exe
    PRIVATE
        REPIU_VERSION="${REPIU_VERSION}"
        # Task 738. The active configuration, right under a multi-config
        # generator too; a single-config generator with no build type yields
        # an empty string, which the reader treats as "decide from NDEBUG".
        REPIU_BUILD_CONFIG="$<CONFIG>"
        ${REPIU_MINIZ_DEFINITIONS}
)

target_link_libraries(repiu_exe PRIVATE repiu_zydis chdr-static)
if(REPIU_EXECUTION_MODEL STREQUAL "emulated")
    target_link_libraries(repiu_exe PRIVATE rex86::core)
endif()

# PUBLIC, not PRIVATE: SDL_Keycode and SDL_Keymod appear in the public input
# binding headers, so SDL3's include directories are part of this library's
# interface and every consumer needs them.
target_link_libraries(repiu_exe PUBLIC SDL3::SDL3-static)

# Task 768: the built-in post-processing shaders are ordinary .glsl files in
# the tree, embedded at configure time as raw string literals so a package
# never has to ship them. Editing one re-runs the configure step.
set(REPIU_POST_SHADER_SOURCE_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/src/engine/post_shaders")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${REPIU_POST_SHADER_SOURCE_DIR}/crt.glsl"
    "${REPIU_POST_SHADER_SOURCE_DIR}/scanline.glsl"
    "${REPIU_POST_SHADER_SOURCE_DIR}/post_shader_builtins.inc.in"
)
file(READ "${REPIU_POST_SHADER_SOURCE_DIR}/crt.glsl" REPIU_POST_SHADER_CRT)
file(READ "${REPIU_POST_SHADER_SOURCE_DIR}/scanline.glsl"
     REPIU_POST_SHADER_SCANLINE)
configure_file(
    "${REPIU_POST_SHADER_SOURCE_DIR}/post_shader_builtins.inc.in"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/repiu/engine/post_shader_builtins.inc"
    @ONLY)
target_include_directories(repiu_exe
    PRIVATE
        "${CMAKE_CURRENT_BINARY_DIR}/generated"
)

# Task 761: the in-game OSD draws with Dear ImGui inside the Glide backend's
# window. Issue #67: the engine, and with it the OSD, is built for the web too.
target_link_libraries(repiu_exe PRIVATE repiu_imgui)

# Task 739. The Glide backend inside this static library calls OpenGL directly,
# so on Linux every consumer has to link GL -- and a static library's
# dependency belongs to the target that has it, not to whichever consumer
# happened to notice. Until now only `repiu` and the x64 `repiu_core_probe`
# named GL themselves, so `repiu_instruction_census` failed the default build's
# last link on both Linux architectures and the i386 `repiu_core_probe` failed
# once Task 720's Glide gate probe reached the backend. The plain name is used
# for the reason `repiu` records: only it resolves in the i386 library
# directory `-m32` puts on the search path.
if(UNIX AND NOT EMSCRIPTEN)
    target_link_libraries(repiu_exe PUBLIC GL)
endif()

add_executable(repiu_chd_cd_probe
    src/tools/chd_cd_probe/main.cpp
)

add_executable(repiu_glide_issue_probe
    src/tools/glide_issue_probe/main.cpp
)

# Task 514, web port Stage 2. Counts the x86 instructions the guest reaches, so
# the size of the Stage 3 interpreter stops being a guess.
#
# On all three configurations deliberately. The whole chain it walks -- parse,
# runtime plan, relocation, translation plan -- lives in src/exe and src/runtime,
# so the tool stands on repiu_exe alone and the three hosts can be compared the
# way Task 501 set up with repiu_core_probe.
add_executable(repiu_instruction_census
    src/tools/instruction_census/main.cpp
)

target_link_libraries(repiu_instruction_census
    PRIVATE
        repiu_exe
        repiu_zydis
)

repiu_link_linux_engine(repiu_instruction_census)

if(EMSCRIPTEN)
    # This tool reads an EXE off the host filesystem, and Emscripten's default
    # MEMFS cannot see one -- the wall repiu_chd_cd_probe hit in Stage 1. Scoped
    # to this target: NODERAWFS is a Node-only escape hatch and has no meaning in
    # a browser, which is where Stage 5 goes.
    #
    # ALLOW_MEMORY_GROWTH is not a build-system detail, it is the first hard
    # number the web target has produced about memory. Emscripten starts with a
    # 16 MiB heap and, by default, aborts rather than growing it. The relocated
    # guest image is 20,382,644 bytes on its own, so the very first thing this
    # tool does exceeds the default -- it died with Aborted(OOM) out of
    # emscripten_resize_heap before printing a line.
    #
    # Growth rather than a larger INITIAL_MEMORY: the plan structures on top of
    # the image are not a fixed size, and a fixed initial heap picked to fit one
    # title would be a number nothing keeps true.
    target_link_options(repiu_instruction_census
        PRIVATE
            -sNODERAWFS=1
            -sALLOW_MEMORY_GROWTH=1
    )
endif()

# Task 501: the probes that need no platform layer, in one binary that builds
# on every host so the same assertions can be compared across them.
add_executable(repiu_core_probe
    src/tools/core_probe/main.cpp
    src/tools/aot_probe/shutdown_recovery_policy_probe.cpp
    src/tools/aot_probe/pic_timer_in_service_probe.cpp
    src/tools/aot_probe/timer_return_pad_probe.cpp
    src/tools/aot_probe/glide_gate_fixup_index_probe.cpp
    src/tools/aot_probe/glide_gate_interrupt_exit_probe.cpp
    src/tools/aot_probe/gl_renderer_identity_probe.cpp
    src/tools/aot_probe/imgui_ui_scale_probe.cpp
    src/tools/aot_probe/build_identity_probe.cpp
    src/tools/aot_probe/code_cache_placement_probe.cpp
    src/tools/aot_probe/dos_console_device_probe.cpp
    src/tools/aot_probe/dos_console_input_probe.cpp
    src/tools/aot_probe/host_pad_input_probe.cpp
    src/tools/aot_probe/browser_key_code_probe.cpp
    src/tools/aot_probe/mesa_fx_texture_source_probe.cpp
    src/tools/aot_probe/dos_file_handle_cache_probe.cpp
    src/tools/aot_probe/env_toggle_probe.cpp
    src/tools/aot_probe/far_jump_probe.cpp
    src/tools/aot_probe/far_return_probe.cpp
    src/tools/aot_probe/execution_backend_probe.cpp
    src/tools/aot_probe/execution_timeout_probe.cpp
    src/tools/aot_probe/glide_letterbox_probe.cpp
    src/tools/aot_probe/glide_lfb_region_probe.cpp
    src/tools/aot_probe/guest_address_space_probe.cpp
    src/tools/aot_probe/jump_table_guard_probe.cpp
    src/tools/aot_probe/launcher_update_probe.cpp
    src/tools/aot_probe/launcher_probe.cpp
    src/tools/aot_probe/long_mode_compatibility_probe.cpp
    src/tools/aot_probe/long_mode_emission_probe.cpp
    src/tools/aot_probe/final_execution_report_probe.cpp
    src/tools/aot_probe/flat_stack_segment_fold_probe.cpp
    src/tools/aot_probe/low_address_reservation_probe.cpp
    src/tools/aot_probe/linux_x64_transfer_failure_provenance_probe.cpp
    src/tools/aot_probe/nvram_path_probe.cpp
    src/tools/aot_probe/pit_timer_probe.cpp
    src/tools/aot_probe/event_clock_probe.cpp
    src/tools/aot_probe/piu10_song_clock_census_probe.cpp
    src/tools/aot_probe/segment_push_probe.cpp
    src/engine/telemetry/final_execution_report.cpp
    src/engine/telemetry/linux_x64_transfer_failure_provenance.cpp
)

# Task 513: the six that ask the platform layer for something wasm does not
# have. Two of them are inline x86 assembly and would not compile; the other
# four would only measure that the Stage 1 stubs refuse. `main.cpp` fences the
# same six and prints their names, so a shorter run is never a quieter one.
if(NOT EMSCRIPTEN)
    target_sources(repiu_core_probe
        PRIVATE
            src/tools/aot_probe/fault_handler_probe.cpp
            src/tools/aot_probe/fault_recovery_provenance_probe.cpp
            src/tools/aot_probe/guest_cpu_context_probe.cpp
            src/tools/aot_probe/host_thread_probe.cpp
            src/tools/aot_probe/virtual_memory_probe.cpp
    )

    if(CMAKE_SIZEOF_VOID_P EQUAL 4)
        target_sources(repiu_core_probe
            PRIVATE
                src/tools/aot_probe/guest_stack_switch_probe.cpp
                src/tools/aot_probe/stack_bridge_probe.cpp
        )
    else()
        target_compile_definitions(repiu_core_probe
            PRIVATE
                REPIU_NO_I386_PROBES=1
        )
    endif()
endif()

target_include_directories(repiu_core_probe
    PRIVATE
        src/tools/aot_probe
        src/engine
        src/engine/cpu_emul
        src/engine/execution
)

# Task 557 added repiu_zydis. The classifier's probe now decodes the bytes its
# lowering produces with a long-mode decoder, which is what catches a
# transcription slip in a hand-written encoding table -- and that needs Zydis's
# headers, which repiu_exe links PRIVATE and so does not pass on.
target_link_libraries(repiu_core_probe
    PRIVATE
        repiu_exe
        repiu_zydis
)

# Issue #67: the probes run the engine's platform layer, so the web probe gets
# the loader's memory layout (repiu_link_web_engine, below).

if(UNIX AND NOT EMSCRIPTEN)
    if(CMAKE_SIZEOF_VOID_P EQUAL 4)
        target_sources(repiu_core_probe
            PRIVATE
                src/tools/aot_probe/stack_bridge_probe.S
                src/tools/aot_probe/guest_stack_switch_probe.S
        )
    endif()
endif()

# Task 547: the first x64 execution contract is a synthetic frame/ABI probe.
# It is Linux x64-only because the bridge tests the SysV AMD64 register and stack
# contract; it does not claim to execute a guest block.
if(UNIX AND NOT EMSCRIPTEN AND CMAKE_SIZEOF_VOID_P GREATER 4)
    target_sources(repiu_core_probe
        PRIVATE
            src/tools/aot_probe/linux_x64_aot_frame_probe.cpp
            src/tools/aot_probe/linux_x64_aot_frame_probe.S
    )
    target_compile_definitions(repiu_core_probe
        PRIVATE
            REPIU_LINUX_X64_AOT_FRAME_PROBE=1
    )
endif()

# Task 558: the guest register mapping, checked by running emitted bytes under
# it. Linux x64-only because it executes long-mode output; a 32-bit host has
# nothing here to test.
if(UNIX AND NOT EMSCRIPTEN AND CMAKE_SIZEOF_VOID_P GREATER 4)
    target_sources(repiu_core_probe
        PRIVATE
            src/tools/aot_probe/linux_x64_guest_register_probe.cpp
            src/tools/aot_probe/general_stack_probe.cpp
            src/tools/aot_probe/linux_x64_guest_register_probe.S
    )
    target_compile_definitions(repiu_core_probe
        PRIVATE
            REPIU_LINUX_X64_GUEST_REGISTER_PROBE=1
            REPIU_LINUX_X64_GENERAL_STACK_PROBE=1
    )
    target_link_libraries(repiu_core_probe PRIVATE GL)
endif()

# Task 552: the memory-operand lowering, checked by executing it. Linux x64-only
# for the same reason as Task 547's: on a 32-bit host the lowering is not merely
# unnecessary, it has nothing to demonstrate -- the bytes already mean there what
# the rewrite makes them mean here.
if(UNIX AND NOT EMSCRIPTEN AND CMAKE_SIZEOF_VOID_P GREATER 4)
    target_sources(repiu_core_probe
        PRIVATE
            src/tools/aot_probe/long_mode_lowering_probe.cpp
    )
    target_compile_definitions(repiu_core_probe
        PRIVATE
            REPIU_LINUX_X64_LOWERING_PROBE=1
    )
endif()

repiu_link_linux_engine(repiu_core_probe)

# Task 502: the Linux launcher. It shares the launcher itself with the Windows
# host and keeps only its own entry point, because the loop and the child
# process on Windows exist for Win32-specific reasons.
if(UNIX AND NOT EMSCRIPTEN)
    add_executable(repiu_launcher
        src/host/linux/main.cpp
        src/launcher/launcher_ui.cpp
    )
    # The launcher clears and sizes its own GL viewport, and on Windows those
    # symbols arrive with opengl32 through the core library. Linux needs the
    # library named. Plain `GL` rather than the imported target, because the
    # 32-bit link has to resolve in the i386 library directory that -m32 puts on
    # the search path.
    target_link_libraries(repiu_launcher
        PRIVATE
            repiu_exe
            repiu_imgui
            GL
    )
    repiu_link_linux_engine(repiu_launcher)

    # Task 503d-17: the loader itself, built from the same source as the
    # Windows host. Task 731 moved it from `src/host/win32/` to
    # `src/host/loader/`: it includes no Windows header and branches on the
    # platform in one log line, so the old path named a platform the code does
    # not depend on. Task 732 then dropped the "Win32 " prefix from its log
    # lines, which already carry the `[loader]` logger tag.
    #
    # GL by name for the same reason repiu_launcher names it: the Glide backend
    # calls into it directly, and only the plain name resolves in the i386
    # library directory `-m32` puts on the search path.
    add_executable(repiu
        src/host/loader/main.cpp
        src/launcher/launcher_ui.cpp
    )
    target_link_libraries(repiu
        PRIVATE
            repiu_exe
            repiu_imgui
            spdlog::spdlog_header_only
            GL
    )
    repiu_link_linux_engine(repiu)
endif()

# Task 501: these carry the Win32 platform layer or the Win32 host, so they exist
# only on Windows. The core library and the core probe below build everywhere.
if(WIN32)
    add_executable(repiu_exe_analyzer
        src/tools/exe_analyzer/main.cpp
    )
    add_executable(repiu_aot_probe
        src/tools/aot_probe/bios_keyboard_input_probe.cpp
        src/tools/aot_probe/aot_timer_source_profile_probe.cpp
        src/tools/aot_probe/append_phase_benchmark_probe.cpp
        src/tools/aot_probe/arena_view_probe.cpp
        src/tools/aot_probe/boundary_provenance_probe.cpp
        src/tools/aot_probe/dbt_call_return_trace_probe.cpp
        src/tools/aot_probe/direct_edge_dispatch_probe.cpp
        src/tools/aot_probe/dbt_return_fallback_probe.cpp
        src/tools/aot_probe/env_toggle_probe.cpp
        src/tools/aot_probe/execution_backend_probe.cpp
        src/tools/aot_probe/execution_timeout_probe.cpp
        src/tools/aot_probe/inline_cache_probe.cpp
        src/tools/aot_probe/jump_table_guard_probe.cpp
        src/tools/aot_probe/native_linear_span_probe.cpp
        src/tools/aot_probe/plan_build_benchmark_probe.cpp
        src/tools/aot_probe/pit_timer_probe.cpp
        src/tools/aot_probe/event_clock_probe.cpp
        src/tools/aot_probe/piu10_isa_board_probe.cpp
        src/tools/aot_probe/retired_trap_profile_probe.cpp
        src/tools/aot_probe/single_step_hotspot_profile_probe.cpp
        src/tools/aot_probe/exception_transition_calibration_probe.cpp
        src/tools/aot_probe/aot_cache_address_index_probe.cpp
        src/tools/aot_probe/aot_inline_cache_site_index_probe.cpp
        src/tools/aot_probe/aot_return_dispatch_site_index_probe.cpp
        src/tools/aot_probe/aot_return_patch_policy_probe.cpp
        src/tools/aot_probe/aot_return_stage_profile_probe.cpp
        src/tools/aot_probe/aot_direct_return_table_probe.cpp
        src/tools/aot_probe/launcher_probe.cpp
        src/tools/aot_probe/execution_time_profile_probe.cpp
        src/tools/aot_probe/glide_gate_timing_probe.cpp
        src/tools/aot_probe/glide_ordinal_timing_probe.cpp
        src/tools/aot_probe/glide_buffer_swap_timing_probe.cpp
        src/tools/aot_probe/glide_lfb_timing_probe.cpp
        src/tools/aot_probe/glide_lfb_write_footprint_probe.cpp
        src/tools/aot_probe/glide_lfb_native_store_census_probe.cpp
        src/tools/aot_probe/glide_lfb_staging_shadow_probe.cpp
        src/tools/aot_probe/glide_lfb_lock_interval_probe.cpp
        src/tools/aot_probe/shutdown_recovery_policy_probe.cpp
        src/tools/aot_probe/pic_timer_in_service_probe.cpp
        src/tools/aot_probe/timer_return_pad_probe.cpp
        src/tools/aot_probe/glide_gate_fixup_index_probe.cpp
        src/tools/aot_probe/glide_gate_interrupt_exit_probe.cpp
        src/tools/aot_probe/gl_renderer_identity_probe.cpp
        src/tools/aot_probe/imgui_ui_scale_probe.cpp
        src/tools/aot_probe/build_identity_probe.cpp
        src/tools/aot_probe/glide_setter_state_census_probe.cpp
        src/tools/aot_probe/glide_async_present_probe.cpp
        src/tools/aot_probe/glide_draw_batch_probe.cpp
        src/tools/aot_probe/glide_lfb_region_probe.cpp
        src/tools/aot_probe/glide_texture_table_stack_probe.cpp
        src/tools/aot_probe/dos_file_create_probe.cpp
        src/tools/aot_probe/dos_console_input_probe.cpp
        src/tools/aot_probe/host_pad_input_probe.cpp
        src/tools/aot_probe/mesa_fx_texture_source_probe.cpp
    src/tools/aot_probe/host_pad_input_probe.cpp
    src/tools/aot_probe/mesa_fx_texture_source_probe.cpp
        src/tools/aot_probe/dos_date_probe.cpp
        src/tools/aot_probe/glide_setter_state_cache_probe.cpp
        src/tools/aot_probe/glide_setter_phase_timing_probe.cpp
        src/tools/aot_probe/glide_gl_error_policy_probe.cpp
        src/tools/aot_probe/glide_swap_interval_policy_probe.cpp
        src/tools/aot_probe/post_shader_probe.cpp
        src/tools/aot_probe/glide_letterbox_probe.cpp
        src/tools/aot_probe/veh_exception_gap_probe.cpp
        src/tools/aot_probe/dos_file_handle_cache_probe.cpp
        src/tools/aot_probe/glide_texture_census_probe.cpp
        src/tools/aot_probe/out_of_arena_step_census_probe.cpp
        src/tools/aot_probe/timer_tick_delivery_probe.cpp
        src/tools/aot_probe/jamma_input_timeline_probe.cpp
        src/tools/aot_probe/romset_config_probe.cpp
        src/tools/aot_probe/nvram_path_probe.cpp
        src/tools/aot_probe/aot_boundary_opcode_census_probe.cpp
        src/tools/aot_probe/aot_worker_timing_probe.cpp
        src/tools/aot_probe/selector_guard_probe.cpp
        src/tools/aot_probe/segment_restore_probe.cpp
        src/tools/aot_probe/main.cpp
    )
    add_executable(repiu_glide_render_probe
        src/tools/glide_render_probe/main.cpp
    )
    add_executable(repiu
        src/host/loader/main.cpp
        src/launcher/launcher_ui.cpp
    )
    target_link_libraries(repiu_exe_analyzer
        PRIVATE
            repiu_exe
    )
    target_link_libraries(repiu_aot_probe
        PRIVATE
            repiu_exe
            SDL3::SDL3-static
    )
    target_link_libraries(repiu_glide_render_probe
        PRIVATE
            repiu_exe
    )
    target_include_directories(repiu_aot_probe
        PRIVATE
            src/engine
            src/engine/execution
            src/engine/input
            third_party/zydis
    )
    target_link_libraries(repiu
        PRIVATE
            repiu_exe
            repiu_imgui
            spdlog::spdlog_header_only
    )
endif()

# Issue #67: the memory layout of every web binary that runs the engine. The
# guest range sits at the same addresses in linear memory, below Emscripten's
# data, stack and heap, which GLOBAL_BASE lifts to 0x10000000
# (src/platform/web/virtual_memory.cpp hands out the range below it, and
# refuses to in a binary linked without this). The stack is far above
# Emscripten's 64 KB default: on one thread the guest, the HLE, the Glide
# backend and the decoders all run on it, and the reports are built on it.
function(repiu_link_web_engine target)
    if(EMSCRIPTEN)
        target_link_options(${target}
            PRIVATE
                -sGLOBAL_BASE=268435456
                -sINITIAL_MEMORY=805306368
                -sALLOW_MEMORY_GROWTH=1
                -sSTACK_SIZE=16777216
                # Issue #69: the renderer is the WebGL2 (ES 3.0) subset and
                # finds its functions beyond 1.1 by name, as on the desktop.
                -sMIN_WEBGL_VERSION=2
                -sMAX_WEBGL_VERSION=2
                -sGL_ENABLE_GET_PROC_ADDRESS=1
        )
    endif()
endfunction()
repiu_link_web_engine(repiu_core_probe)

# Issue #67: the loader on the web, run under Node for now (phase 2 of design
# #64). NODERAWFS reads the ROMs from the host filesystem and NODE_HOST_ENV
# passes the environment through; both are Node-only, and phase 4's browser
# build replaces them.
if(EMSCRIPTEN)
    add_executable(repiu
        src/host/loader/main.cpp
        src/launcher/launcher_ui.cpp
    )
    target_link_libraries(repiu
        PRIVATE
            repiu_exe
            repiu_imgui
            spdlog::spdlog_header_only
    )
    repiu_link_web_engine(repiu)
    target_link_options(repiu
        PRIVATE
            -sNODERAWFS=1
            -sNODE_HOST_ENV=1
            -sEXIT_RUNTIME=1
            "--pre-js=${CMAKE_CURRENT_SOURCE_DIR}/src/platform/web/node_pre.js"
    )
    set_property(TARGET repiu APPEND PROPERTY LINK_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/src/platform/web/node_pre.js")

    # Issue #72: the same loader for a browser page's Worker (design
    # 20261016-i072). worker.js mounts the ROM files the user picked with
    # WORKERFS, hands over the page's OffscreenCanvas and calls main. The
    # single-threaded loop yields to the page at the host turn through
    # Asyncify; indirect calls are ignored so that rex86's callbacks into the
    # HLE and its interpreter stay uninstrumented, and the one indirect call
    # on the way down to the yield -- RunGuestOnThisThread calling the guest
    # thread procedure -- is added by name; Asyncify adds its callers.
    add_executable(repiu_browser
        src/host/loader/main.cpp
        src/launcher/launcher_ui.cpp
    )
    target_link_libraries(repiu_browser
        PRIVATE
            repiu_exe
            repiu_imgui
            spdlog::spdlog_header_only
    )
    repiu_link_web_engine(repiu_browser)
    target_link_options(repiu_browser
        PRIVATE
            -sENVIRONMENT=worker
            -sMODULARIZE=1
            -sEXPORT_NAME=createRepiu
            -sINVOKE_RUN=0
            -sFORCE_FILESYSTEM=1
            -lworkerfs.js
            "-sEXPORTED_RUNTIME_METHODS=['FS','ENV','callMain']"
            -sASYNCIFY=1
            -sASYNCIFY_IGNORE_INDIRECT=1
            # RunGuestOnThisThread is called once and binaryen inlines it into
            # its caller, so the indirect call sits in RunExecutionThread (a
            # pattern for the inlined name matches nothing and only warns).
            "-sASYNCIFY_ADD=['repiu::engine::RunExecutionThread*']"
            -sASYNCIFY_STACK_SIZE=1048576
            # The add list matches function names, which a release link
            # strips unless they are kept.
            --profiling-funcs
            "--pre-js=${CMAKE_CURRENT_SOURCE_DIR}/src/platform/web/node_pre.js"
    )
    set_property(TARGET repiu_browser APPEND PROPERTY LINK_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/src/platform/web/node_pre.js")
    # The page that loads it, next to repiu_browser.{js,wasm}.
    set(repiu_browser_page
        "${CMAKE_CURRENT_SOURCE_DIR}/src/platform/web/page/index.html"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/platform/web/page/main.js"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/platform/web/page/rom_selection.js"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/platform/web/page/worker.js"
    )
    add_custom_command(TARGET repiu_browser POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${repiu_browser_page}
                $<TARGET_FILE_DIR:repiu_browser>
        VERBATIM
    )
endif()

if(WIN32)
    add_executable(repiu_supervisor_win32
        src/host/win32/supervisor_main.cpp
    )
    target_include_directories(repiu_supervisor_win32
        PRIVATE
            include
    )
endif()

target_link_libraries(repiu_chd_cd_probe
    PRIVATE
        repiu_exe
)

target_link_libraries(repiu_glide_issue_probe
    PRIVATE
        repiu_exe
)

if(WIN32)
    target_link_libraries(repiu_exe
        PUBLIC
            opengl32
            gdi32
            user32
            # Task 412: symbolises the guest position census's host call sites
            # at teardown. Without a PDB it resolves nothing and the report
            # falls back to module+offset, so it is safe in every build.
            dbghelp
            # Issue #48: the launcher's update download.
            winhttp
    )
endif()

repiu_configure_win32_loader_host(repiu_exe_analyzer)
repiu_configure_win32_loader_host(repiu)
