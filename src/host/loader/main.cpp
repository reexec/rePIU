#include "repiu/exe/dos4gw_loader.h"
#include "repiu/assets/piu_chd_mount.h"
#include "repiu/exe/dos16m_bound_module.h"
#include "repiu/hle/dos_file_system.h"
#include "repiu/hle/hle_dispatcher.h"
#include "repiu/hle/privileged_instruction.h"
#include "repiu/config/romset_config.h"
#include "repiu/storage/nvram_path.h"
#include "repiu/engine/active_jamma_bindings.h"
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_error.h>
#include "repiu/engine/eeprom_backing_path.h"
#include "repiu/engine/execution_trampoline.h"
#include "repiu/engine/timer_latency_trace.h"
#include "repiu/engine/final_execution_report.h"
#include "repiu/engine/session_display_preferences.h"
#include "repiu/platform/build_identity.h"
#include "repiu/platform/host_environment.h"
#include "repiu/platform/host_process.h"
#include "repiu/update/launcher_updater.h"
#include "repiu/update/semantic_version.h"
#include "repiu/update/update_install.h"
#include "repiu/engine/session_identity.h"
#include "repiu/engine/aot_code_cache.h"
#include "../../engine/aot/aot_dbt_glide_gate_dispatch.h"
#include "../../engine/telemetry/aot_residency_sample.h"
#include "../../engine/io/port_io_delay_loop.h"
#include "../../engine/aot/aot_generation_failure_policy.h"
#include "repiu/platform/host_crash_report.h"
#include "repiu/engine/aot_boundary_opcode_census.h"
#include "repiu/engine/live_telemetry.h"
#include "repiu/engine/veh_exit_site.h"
#include "repiu/engine/runtime_memory_policy.h"
#include "repiu/platform/host_environment.h"
#include "repiu/platform/host_process.h"
#include "repiu/runtime/guest_context.h"
#include "repiu/runtime/aot_code_cache.h"
#include "repiu/runtime/aot_translation_plan.h"
#include "repiu/runtime/env_toggle.h"
#include "repiu/runtime/execution_backend.h"
#include "repiu/runtime/execution_timeout.h"
#include "repiu/runtime/image_address.h"
#include "repiu/runtime/runtime_memory.h"
#include "repiu/runtime/runtime_memory_arena.h"
#include "repiu/runtime/selector_table.h"
#include "repiu/launcher/command_line_options.h"
#include "repiu/launcher/launcher_settings.h"
#include "repiu/launcher/launcher_ui.h"
#include "repiu/launcher/rom_set_catalog.h"
#include "repiu/target/target_profile.h"

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <cstdio>
#include <cctype>
#include <algorithm>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <system_error>
#include <string_view>
#include <vector>

namespace
{

bool ReadExecutionBackend(repiu::runtime::ExecutionBackend* backend)
{
    return repiu::runtime::ResolveExecutionBackend(
        std::getenv("REPIU_EXECUTION_BACKEND"), backend);
}

bool ReadAotIndirectInlineCacheEntryCount(std::uint32_t* entry_count)
{
    if (entry_count == nullptr)
    {
        return false;
    }
    const char* value = std::getenv("REPIU_AOT_INDIRECT_CACHE_SLOTS");
    if (value == nullptr || *value == '\0' || std::string_view(value) == "4")
    {
        *entry_count =
            repiu::runtime::kDefaultAotIndirectInlineCacheEntryCount;
        return true;
    }
    if (std::string_view(value) == "1")
    {
        *entry_count = 1U;
        return true;
    }
    return false;
}

// Task 435: resolving the budget is runtime policy; all that happens here is
// translating "no limit" into the value the Win32 wait API knows.
std::uint32_t ReadExecutionTimeoutMilliseconds()
{
    const char* text = std::getenv(
        repiu::engine::kExecutionTimeoutEnvironment);
    fprintf(stderr, "[repiu-live-debug] env REPIU_EXECUTION_TIMEOUT_MS = %s\n", text ? text : "NULL");
    const std::uint32_t milliseconds =
        repiu::runtime::ResolveExecutionTimeoutMilliseconds(text);
    return milliseconds ==
            repiu::runtime::kUnlimitedExecutionTimeoutMilliseconds
        ? repiu::runtime::kWaitForeverMilliseconds
        : milliseconds;
}

std::uint32_t ReadStallTimeoutMilliseconds()
{
    const char* text = std::getenv(
        repiu::engine::kStallTimeoutEnvironment);
    const std::uint32_t milliseconds =
        repiu::runtime::ResolveStallTimeoutMilliseconds(text);
    return milliseconds == 0U ? repiu::runtime::kWaitForeverMilliseconds
                              : milliseconds;
}

std::shared_ptr<spdlog::logger> CreateLoaderLogger()
{
    std::shared_ptr<spdlog::logger> logger =
        spdlog::stderr_color_mt("loader");
    logger->set_pattern("[%X.%e] [%8l] [%n] %v");
    logger->set_level(spdlog::level::info);
    return logger;
}

std::string Hex32(std::uint32_t value)
{
    std::ostringstream stream;
    stream << "0x" << std::uppercase << std::hex << std::setw(8)
           << std::setfill('0') << value;
    return stream.str();
}

std::string Hex8(std::uint8_t value)
{
    std::ostringstream stream;
    stream << "0x" << std::uppercase << std::hex << std::setw(2)
           << std::setfill('0') << static_cast<unsigned>(value);
    return stream.str();
}

std::string Hex16(std::uint16_t value)
{
    std::ostringstream stream;
    stream << "0x" << std::uppercase << std::hex << std::setw(4)
           << std::setfill('0') << value;
    return stream.str();
}

std::string HexBytes(const std::uint8_t* bytes, std::size_t byte_count)
{
    std::ostringstream stream;
    for (std::size_t index = 0; index < byte_count; ++index)
    {
        if (index != 0U)
        {
            stream << ' ';
        }
        stream << std::uppercase << std::hex << std::setw(2)
               << std::setfill('0')
               << static_cast<unsigned>(bytes[index]);
    }
    return stream.str();
}

const char* SegmentRegisterName(std::uint32_t segment_register)
{
    switch (segment_register)
    {
        case 0:
            return "ES";
        case 2:
            return "SS";
        case 3:
            return "DS";
        case 4:
            return "FS";
        case 5:
            return "GS";
        default:
            return "unknown";
    }
}

void LogGuestOutput(spdlog::logger& logger,
                    spdlog::level::level_enum level,
                    std::string_view output)
{
    std::size_t begin = 0;
    while (begin < output.size())
    {
        const std::size_t newline = output.find('\n', begin);
        const std::size_t end = newline == std::string_view::npos
            ? output.size()
            : newline;
        std::size_t content_end = end;
        if (content_end > begin && output[content_end - 1U] == '\r')
        {
            --content_end;
        }
        logger.log(level, "{}", output.substr(begin, content_end - begin));
        if (newline == std::string_view::npos)
        {
            break;
        }
        begin = newline + 1U;
    }
}

// Task 524: resolve an asset beside the executable without trusting its case.
//
// The arcade dumps carry whatever case the original media had -- this tree has
// DOS4GW.EXE next to glide2x.ovl -- and a Windows or DrvFs host hides the
// difference because those filesystems match case-insensitively. On ext4 an
// exact-case lookup misses, and the callers below treat a miss as "the asset
// is absent" and carry on, so the mismatch surfaces much later as a guest that
// cannot initialize its DLL loader.
std::filesystem::path ResolveSiblingAssetPath(
    const std::filesystem::path& directory,
    std::string_view filename)
{
    const std::filesystem::path exact = directory / filename;
    if (std::filesystem::exists(exact))
    {
        return exact;
    }

    const auto equal_ignoring_case = [](std::string_view left,
                                        std::string_view right) {
        return left.size() == right.size() &&
            std::equal(left.begin(), left.end(), right.begin(),
                       [](unsigned char a, unsigned char b) {
                           return std::tolower(a) == std::tolower(b);
                       });
    };

    std::error_code directory_error;
    for (const auto& entry :
         std::filesystem::directory_iterator(directory, directory_error))
    {
        const std::string candidate = entry.path().filename().string();
        if (equal_ignoring_case(candidate, filename))
        {
            return entry.path();
        }
    }
    return {};
}
bool ReadBinaryFile(const std::filesystem::path& path,
                    std::vector<std::uint8_t>* data,
                    std::string* error_message)
{
    if (data == nullptr)
    {
        if (error_message != nullptr)
        {
            *error_message = "output buffer is null";
        }
        return false;
    }

    std::ifstream file(path, std::ios::binary);
    if (!file)
    {
        if (error_message != nullptr)
        {
            *error_message = "failed to open file";
        }
        return false;
    }

    file.seekg(0, std::ios::end);
    const std::streamoff size = file.tellg();
    if (size < 0)
    {
        if (error_message != nullptr)
        {
            *error_message = "failed to determine file size";
        }
        return false;
    }

    file.seekg(0, std::ios::beg);
    data->resize(static_cast<std::size_t>(size));
    if (!data->empty())
    {
        file.read(reinterpret_cast<char*>(data->data()), size);
        if (!file)
        {
            if (error_message != nullptr)
            {
                *error_message = "failed to read file contents";
            }
            return false;
        }
    }

    return true;
}


std::string BuildByteWindowText(
    const repiu::runtime::RelocatedImageByteWindow& window)
{
    std::ostringstream stream;
    for (std::size_t index = 0; index < window.bytes.size(); ++index)
    {
        if (index == window.focus_offset)
        {
            stream << " [";
        }
        else
        {
            stream << " ";
        }
        stream << std::uppercase << std::hex << std::setw(2)
               << std::setfill('0')
               << static_cast<unsigned>(window.bytes[index])
               << std::dec << std::setfill(' ');
        if (index == window.focus_offset)
        {
            stream << "]";
        }
    }
    return stream.str();
}

void PrintByteWindow(
    spdlog::logger& logger,
    const repiu::runtime::RelocatedImageByteWindow& window)
{
    logger.error("Relocated exception byte window: {}",
                 window.valid ? "valid" : "invalid");
    logger.error("Relocated exception byte window message: {}",
                 window.message);
    if (!window.valid)
    {
        return;
    }

    logger.error("Relocated exception byte object: {}",
                 window.object_index);
    logger.error("Relocated exception byte base: {}",
                 Hex32(window.window_base));
    logger.error("Relocated exception byte focus offset: {}",
                 Hex32(window.focus_offset));
    logger.error("Relocated exception bytes:{}",
                 BuildByteWindowText(window));
}

void PrintPrivilegedInstructionClassification(
    spdlog::logger& logger,
    const repiu::hle::PrivilegedInstructionClassification& classification)
{
    const bool breakpoint_trap =
        classification.instruction_class ==
        repiu::hle::PrivilegedInstructionClass::kGuestBreakpointTrap;
    if (!classification.valid)
    {
        logger.error("Privileged instruction classification: unknown");
        logger.error("Privileged instruction opcode: {}",
                     Hex8(classification.opcode));
    }
    else if (breakpoint_trap)
    {
        logger.error("Privileged instruction classification: breakpoint");
        logger.error("Privileged instruction opcode: {}",
                     Hex8(classification.opcode));
    }
    else
    {
        logger.info("Privileged instruction classification: valid");
        logger.info("Privileged instruction opcode: {}",
                    Hex8(classification.opcode));
    }
    if (classification.valid)
    {
        spdlog::logger& output_logger = logger;
        const spdlog::level::level_enum level =
            breakpoint_trap ? spdlog::level::err : spdlog::level::info;
        output_logger.log(level,
                          "Privileged instruction mnemonic: {}",
                          classification.mnemonic);
        output_logger.log(level,
                          "Privileged instruction length: {}",
                          classification.length);
        output_logger.log(level,
                          "Privileged instruction class: {}",
                          repiu::hle::PrivilegedInstructionClassName(
                              classification.instruction_class));
        output_logger.log(level,
                          "Privileged instruction HLE trap candidate: {}",
                          classification.hle_trap_candidate ? "true"
                                                            : "false");
        output_logger.log(
            level,
            "Privileged instruction CPU/DPMI state candidate: {}",
            classification.cpu_state_initialization_candidate ? "true"
                                                             : "false");
    }
    if (!classification.valid)
    {
        logger.error("Privileged instruction classification message: {}",
                     classification.message);
        logger.error("Current execution blocker: unhandled or unclassified "
                     "instruction/memory access at exception point");
    }
    else if (breakpoint_trap)
    {
        logger.error("Privileged instruction classification message: {}",
                     classification.message);
        logger.error("Current execution blocker: guest breakpoint trap");
    }
    else if (classification.hle_trap_candidate)
    {
        logger.error("Privileged instruction classification message: {}",
                     classification.message);
        logger.error("Current execution blocker: unhandled HLE trap candidate");
    }
    else
    {
        logger.info("Privileged instruction classification message: {}",
                    classification.message);
    }
}

void PrintX86ExecutionSnapshot(spdlog::logger& logger,
                               std::string_view label,
                               const repiu::engine::
                                   X86ExecutionSnapshot& snapshot)
{
    logger.error("{} context captured: {}",
                 label,
                 snapshot.captured ? "true" : "false");
    if (!snapshot.captured)
    {
        return;
    }

    logger.error("{} EIP: {}", label, Hex32(snapshot.eip));
    logger.error("{} EAX: {}", label, Hex32(snapshot.eax));
    logger.error("{} EBX: {}", label, Hex32(snapshot.ebx));
    logger.error("{} ECX: {}", label, Hex32(snapshot.ecx));
    logger.error("{} EDX: {}", label, Hex32(snapshot.edx));
    logger.error("{} ESI: {}", label, Hex32(snapshot.esi));
    logger.error("{} EDI: {}", label, Hex32(snapshot.edi));
    logger.error("{} ESP: {}", label, Hex32(snapshot.esp));
    logger.error("{} EBP: {}", label, Hex32(snapshot.ebp));
    logger.error("{} EFLAGS: {}", label, Hex32(snapshot.eflags));
    logger.error("{} CS: {}", label, Hex16(snapshot.cs));
    logger.error("{} DS: {}", label, Hex16(snapshot.ds));
    logger.error("{} ES: {}", label, Hex16(snapshot.es));
    logger.error("{} SS: {}", label, Hex16(snapshot.ss));
    logger.error("{} FS: {}", label, Hex16(snapshot.fs));
    logger.error("{} GS: {}", label, Hex16(snapshot.gs));
}

void PrintGuestStackPlan(
    spdlog::logger& logger,
    const repiu::runtime::GuestStackSwitchPlan& plan)
{
    logger.info("Guest stack switch plan: {}",
                plan.valid ? "valid" : "invalid");
    logger.info("Guest stack switch entry: {}", Hex32(plan.entry_eip));
    logger.info("Guest stack switch stack base: {}",
                Hex32(plan.stack_base));
    logger.info("Guest stack switch stack limit: {}",
                Hex32(plan.stack_limit));
    logger.info("Guest stack switch initial ESP: {}",
                Hex32(plan.initial_esp));
    logger.info("Guest stack switch message: {}", plan.message);
}

void PrintHleDispatcherTable(
    spdlog::logger& logger,
    const repiu::hle::HleDispatcherTable& table)
{
    logger.info("HLE dispatcher table: {}",
                table.valid ? "valid" : "invalid");
    logger.info("HLE dispatcher trap count: {}", table.traps.size());
    logger.info("HLE dispatcher message: {}", table.message);
}

void PrintRuntimeMemoryArenaPlan(
    spdlog::logger& logger,
    const repiu::runtime::RuntimeMemoryArenaPlan& plan)
{
    logger.info("Runtime memory arena plan: {}",
                plan.valid ? "valid" : "invalid");
    logger.info("Runtime memory arena base: {}",
                Hex32(plan.base_address));
    logger.info("Runtime memory arena image reserve size: {}",
                Hex32(plan.image_reserve_size));
    logger.info("Runtime memory arena expansion slack size: {}",
                Hex32(plan.expansion_slack_size));
    if (plan.valid)
    {
        logger.info("Runtime memory arena reserve size: {}",
                    Hex32(plan.arena_reserve_size));
        logger.info("Runtime memory arena end: {}",
                    Hex32(plan.arena_end_address));
    }
    if (!plan.message.empty())
    {
        logger.info("Runtime memory arena message: {}", plan.message);
    }
}

void PrintDosVirtualFileSystem(
    spdlog::logger& logger,
    const repiu::hle::DosVirtualFileSystemState& state)
{
    logger.info("DOS virtual filesystem: {}",
                state.valid ? "valid" : "invalid");
    logger.info("DOS virtual filesystem root: {}",
                state.host_root.string());
    const std::string current_directory =
        repiu::hle::GetDosCurrentDirectory(state);
    logger.info("DOS virtual filesystem current directory: {}",
                current_directory.empty()
                    ? "\\"
                    : "\\" + current_directory);
    logger.info("DOS virtual filesystem message: {}", state.message);
}

void PrintParseError(spdlog::logger& logger,
                     const repiu::exe::ParseError& error)
{
    logger.error("Parse error at {}: {}",
                 Hex32(error.file_offset),
                 error.message);
}

void PrintPolicy(
    spdlog::logger& logger,
    const repiu::engine::RuntimeMemoryPolicy& policy)
{
    logger.info("loader policy: {}",
                policy.valid ? "valid" : "invalid");
    logger.info("host pointer bits: {}", policy.host_pointer_bits);
    logger.info("direct x86 execution: {}",
                policy.direct_x86_execution_supported ? "supported"
                                                       : "unsupported");
    logger.info("fixed reserve base: {}",
                Hex32(policy.preferred_allocation_base));
    logger.info("fixed reserve size: {}",
                Hex32(policy.required_reserve_size));
    logger.info("fixed reserve end: {}",
                Hex32(policy.hle_reserve_base));
    logger.info("loader policy message: {}", policy.message);
}

void PrintReservation(
    spdlog::logger& logger,
    const repiu::engine::AddressRangeReservation& reservation)
{
    logger.info("early reservation attempt: {}",
                reservation.valid ? "valid" : "invalid");
    if (reservation.reserved)
    {
        logger.info("early reservation result: reserved");
    }
    else
    {
        logger.warn("early reservation result: not reserved");
    }
    logger.info("requested reserve base: {}",
                Hex32(reservation.requested_base));
    logger.info("requested reserve size: {}",
                Hex32(reservation.requested_size));
    if (reservation.reserved)
    {
        logger.info("reserved base: {}",
                    Hex32(reservation.reserved_base));
        logger.info("reserved size: {}",
                    Hex32(reservation.reserved_size));
    }
    if (reservation.windows_error != 0)
    {
        logger.warn("reservation error: {}",
                    reservation.windows_error);
    }
    if (reservation.reserved)
    {
        logger.info("early reservation message: {}",
                    reservation.message);
    }
    else
    {
        logger.warn("early reservation message: {}",
                    reservation.message);
    }
}

void PrintProbe(
    spdlog::logger& logger,
    const repiu::engine::AddressRangeProbe& probe)
{
    logger.info("host range probe: {}",
                probe.valid ? "valid" : "invalid");
    if (probe.range_available)
    {
        logger.info("host range available: true");
    }
    else
    {
        logger.warn("host range available: false");
    }
    logger.info("host probe base: {}",
                Hex32(probe.checked_base));
    logger.info("host probe size: {}",
                Hex32(probe.checked_size));
    if (probe.valid && !probe.range_available)
    {
        logger.warn("host first blocking block base: {}",
                    Hex32(probe.first_block_base));
        logger.warn("host first blocking block size: {}",
                    Hex32(probe.first_block_size));
        logger.warn("host first blocking block state: {}",
                    probe.first_block_state);
    }
    if (probe.range_available)
    {
        logger.info("host range probe message: {}", probe.message);
    }
    else
    {
        logger.warn("host range probe message: {}", probe.message);
    }
}

void PrintPlacement(
    spdlog::logger& logger,
    const repiu::engine::RelocatedImagePlacement& placement)
{
    logger.info("relocated image placement: {}",
                placement.valid ? "valid" : "invalid");
    logger.info("relocated image placement result: {}",
                placement.placed ? "placed" : "not placed");
    logger.info("relocated image requested base: {}",
                Hex32(placement.requested_base));
    logger.info("relocated image requested size: {}",
                Hex32(placement.requested_size));
    if (placement.placed)
    {
        logger.info("relocated image placed base: {}",
                    Hex32(placement.placed_base));
        logger.info("relocated image placed size: {}",
                    Hex32(placement.placed_size));
    }
    logger.info("relocated image copied objects: {}",
                placement.copied_object_count);
    logger.info("relocated image protected objects: {}",
                placement.protected_object_count);
    logger.info("relocated selector binding count: {}",
                placement.selector_bindings.size());
    for (const repiu::runtime::RelocatedSelectorBinding& binding :
         placement.selector_bindings)
    {
        logger.info(
            "relocated selector binding: selector={} object={} base={} limit={}",
            Hex16(binding.selector),
            binding.target_object,
            Hex32(binding.relocated_base_address),
            Hex32(binding.limit));
    }
    if (placement.windows_error != 0)
    {
        logger.info("relocated image placement error: {}",
                    placement.windows_error);
    }
    logger.info("relocated image placement message: {}",
                placement.message);
}

void PrintExecutionAttempt(
    spdlog::logger& logger,
    const repiu::engine::MinimalExecutionAttempt& attempt,
    std::string_view executable_name)
{
    logger.info("minimal execution attempt: {}",
                attempt.valid ? "valid" : "invalid");
    logger.info("minimal execution supported: {}",
                attempt.supported ? "true" : "false");
    logger.info("minimal execution attempted: {}",
                attempt.attempted ? "true" : "false");
    logger.info("minimal execution entry: {}",
                Hex32(attempt.entry_address));
    logger.info("minimal execution returned: {}",
                attempt.returned ? "true" : "false");
    if (attempt.exception_caught)
    {
        logger.error("minimal execution exception caught: true");
    }
    else
    {
        logger.info("minimal execution exception caught: false");
    }
    if (attempt.exception_caught)
    {
        logger.error("minimal execution exception code: {}",
                     Hex32(attempt.seh_exception_code));
        logger.error("minimal execution exception address: {}",
                     Hex32(attempt.seh_exception_address));
        logger.error("minimal execution exception EAX: {}",
                     Hex32(attempt.exception_eax));
        logger.error("minimal execution exception EBX: {}",
                     Hex32(attempt.exception_ebx));
        logger.error("minimal execution exception ECX: {}",
                     Hex32(attempt.exception_ecx));
        logger.error("minimal execution exception EDX: {}",
                     Hex32(attempt.exception_edx));
        logger.error("minimal execution exception ESI: {}",
                     Hex32(attempt.exception_esi));
        logger.error("minimal execution exception EDI: {}",
                     Hex32(attempt.exception_edi));
        logger.error("minimal execution exception access/fault VA: {}/{}",
                     attempt.exception_access_kind,
                     Hex32(attempt.exception_fault_va));
        logger.error(
            "exception fault page base/alloc/state/protect/size: "
            "{}/{}/{}/{}/{}",
            Hex32(attempt.exception_fault_region_base),
            Hex32(attempt.exception_fault_alloc_base),
            Hex32(attempt.exception_fault_state),
            Hex32(attempt.exception_fault_protect),
            Hex32(attempt.exception_fault_region_size));
        logger.error(
            "exception ESI structure +0x20..+0x3C (mask {}): "
            "{} {} {} {} {} {} {} {}",
            Hex32(attempt.exception_esi_dword_valid_mask),
            Hex32(attempt.exception_esi_dwords[0]),
            Hex32(attempt.exception_esi_dwords[1]),
            Hex32(attempt.exception_esi_dwords[2]),
            Hex32(attempt.exception_esi_dwords[3]),
            Hex32(attempt.exception_esi_dwords[4]),
            Hex32(attempt.exception_esi_dwords[5]),
            Hex32(attempt.exception_esi_dwords[6]),
            Hex32(attempt.exception_esi_dwords[7]));
        {
            static const char* const kRegisterNames[6] = {
                "EAX", "EBX", "ECX", "EDX", "ESI", "EDI"};
            for (std::uint32_t reg = 0; reg < 6U; ++reg)
            {
                if ((attempt.exception_register_string_valid_mask &
                     (1U << reg)) == 0)
                {
                    continue;
                }
                const std::uint8_t* bytes =
                    attempt.exception_register_strings[reg];
                std::string ascii;
                std::string hex;
                for (std::uint32_t index = 0;
                     index < sizeof(attempt.exception_register_strings[reg]);
                     ++index)
                {
                    const std::uint8_t value = bytes[index];
                    if (!hex.empty())
                    {
                        hex += ' ';
                    }
                    hex += Hex8(value);
                    ascii += (value >= 0x20 && value < 0x7F)
                                 ? static_cast<char>(value)
                                 : '.';
                }
                logger.error(
                    "exception register string {}: \"{}\" [{}]",
                    kRegisterNames[reg], ascii, hex);
            }
        }
        logger.error("exception stack window base/count: {}/{}",
                     Hex32(attempt.exception_stack_base),
                     attempt.exception_stack_dword_count);
        for (std::uint32_t row = 0;
             row < attempt.exception_stack_dword_count; row += 4U)
        {
            std::string line;
            const std::uint32_t row_end = std::min<std::uint32_t>(
                row + 4U, attempt.exception_stack_dword_count);
            for (std::uint32_t index = row; index < row_end; ++index)
            {
                line += ' ';
                line += Hex32(attempt.exception_stack_dwords[index]);
            }
            logger.error("exception stack +{}:{}",
                         Hex32(row * 4U), line);
        }
        const auto& breakpoint = attempt.unhandled_breakpoint_evidence;
        logger.error("unhandled breakpoint evidence valid: {}",
                     breakpoint.valid ? "true" : "false");
        if (breakpoint.valid)
        {
            logger.error(
                "unhandled breakpoint code/raw/entry/final: {}/{}/{}/{}",
                Hex32(breakpoint.code),
                Hex32(breakpoint.exception_address),
                Hex32(breakpoint.entry_eip),
                Hex32(breakpoint.final_eip));
            logger.error(
                "unhandled breakpoint ESP entry/final EFLAGS DR6/DR7: "
                "{}/{}/{}/{}/{}",
                Hex32(breakpoint.entry_esp),
                Hex32(breakpoint.final_esp),
                Hex32(breakpoint.entry_eflags),
                Hex32(breakpoint.entry_dr6),
                Hex32(breakpoint.entry_dr7));
            logger.error(
                "unhandled breakpoint state entry/final "
                "(reentry/trace/fast/span/region bits): {}/{}",
                Hex32(breakpoint.entry_state_flags),
                Hex32(breakpoint.final_state_flags));
            logger.error(
                "unhandled breakpoint AOT reentry cache/return count "
                "entry/final: {}/{}/{}",
                Hex32(breakpoint.entry_aot_reentry_cache_address),
                breakpoint.entry_aot_return_dispatch_count,
                breakpoint.final_aot_return_dispatch_count);
            logger.error(
                "unhandled breakpoint AOT return source/target "
                "entry/final: {}/{} / {}/{}",
                Hex32(breakpoint.entry_aot_last_return_source),
                Hex32(breakpoint.entry_aot_last_return_target),
                Hex32(breakpoint.final_aot_last_return_source),
                Hex32(breakpoint.final_aot_last_return_target));
            logger.error(
                "unhandled breakpoint cache raw/eip: {}/{}",
                breakpoint.exception_address_in_aot_cache ? "true" : "false",
                breakpoint.entry_eip_in_aot_cache ? "true" : "false");
            logger.error(
                "unhandled breakpoint raw mapping exact/previous: "
                "{}/{}/{} / {}/{}/{}",
                breakpoint.exception_exact_mapping_valid ? "valid" : "invalid",
                Hex32(breakpoint.exception_exact_guest),
                breakpoint.exception_exact_provenance_valid
                    ? breakpoint.exception_exact_provenance : 0xFFFFFFFFU,
                breakpoint.exception_previous_mapping_valid ? "valid" : "invalid",
                Hex32(breakpoint.exception_previous_guest),
                breakpoint.exception_previous_provenance_valid
                    ? breakpoint.exception_previous_provenance : 0xFFFFFFFFU);
            logger.error(
                "unhandled breakpoint EIP mapping exact/previous: "
                "{}/{}/{} / {}/{}/{}",
                breakpoint.eip_exact_mapping_valid ? "valid" : "invalid",
                Hex32(breakpoint.eip_exact_guest),
                breakpoint.eip_exact_provenance_valid
                    ? breakpoint.eip_exact_provenance : 0xFFFFFFFFU,
                breakpoint.eip_previous_mapping_valid ? "valid" : "invalid",
                Hex32(breakpoint.eip_previous_guest),
                breakpoint.eip_previous_provenance_valid
                    ? breakpoint.eip_previous_provenance : 0xFFFFFFFFU);
            logger.error(
                "unhandled breakpoint raw bytes base/count: {}/{} {}",
                Hex32(breakpoint.exception_window_base),
                breakpoint.exception_window_count,
                HexBytes(breakpoint.exception_window,
                         breakpoint.exception_window_count));
            logger.error(
                "unhandled breakpoint EIP bytes base/count: {}/{} {}",
                Hex32(breakpoint.eip_window_base),
                breakpoint.eip_window_count,
                HexBytes(breakpoint.eip_window,
                         breakpoint.eip_window_count));
            logger.error(
                "unhandled breakpoint stack mask/dwords: {} {} {} {} {}",
                Hex32(breakpoint.stack_valid_mask),
                Hex32(breakpoint.stack_dwords[0]),
                Hex32(breakpoint.stack_dwords[1]),
                Hex32(breakpoint.stack_dwords[2]),
                Hex32(breakpoint.stack_dwords[3]));
        }
        PrintX86ExecutionSnapshot(logger,
                                  "minimal execution exception",
                                  attempt.exception_snapshot);
    }
    logger.info("handled original fatal breakpoint count: {}",
                attempt.handled_fatal_breakpoint_count);
    if (attempt.handled_fatal_breakpoint_count != 0)
    {
        logger.error("last original fatal breakpoint address: {}",
                     Hex32(attempt.last_fatal_breakpoint_address));
        logger.error("last original fatal message address: {}",
                     Hex32(attempt.last_fatal_message_address));
        logger.error("last original fatal message: {}",
                     attempt.last_fatal_message);
    }
    logger.info("original fatal halt reached: {}",
                attempt.fatal_halt_reached ? "true" : "false");
    logger.info("minimal execution timed out: {}",
                attempt.timed_out ? "true" : "false");
    logger.info("minimal execution stall timed out: {}",
                attempt.stall_timed_out ? "true" : "false");
    logger.info("SDL exit requested: {}",
                attempt.quit_requested ? "true" : "false");
    if (attempt.timed_out)
    {
        const auto& snapshot = attempt.timeout_snapshot;
        logger.info("minimal execution timeout context captured: {}",
                    snapshot.captured ? "true" : "false");
        if (snapshot.captured)
        {
            logger.info("minimal execution timeout EIP: {}",
                        Hex32(snapshot.eip));
            logger.info("minimal execution timeout EAX: {}",
                        Hex32(snapshot.eax));
            logger.info("minimal execution timeout EBX: {}",
                        Hex32(snapshot.ebx));
            logger.info("minimal execution timeout ECX: {}",
                        Hex32(snapshot.ecx));
            logger.info("minimal execution timeout EDX: {}",
                        Hex32(snapshot.edx));
            logger.info("minimal execution timeout ESI: {}",
                        Hex32(snapshot.esi));
            logger.info("minimal execution timeout EDI: {}",
                        Hex32(snapshot.edi));
            logger.info("minimal execution timeout ESP: {}",
                        Hex32(snapshot.esp));
            logger.info("minimal execution timeout EBP: {}",
                        Hex32(snapshot.ebp));
            logger.info("minimal execution timeout EFLAGS: {}",
                        Hex32(snapshot.eflags));
            logger.info("minimal execution timeout CS: {}",
                        Hex16(snapshot.cs));
            logger.info("minimal execution timeout DS: {}",
                        Hex16(snapshot.ds));
            logger.info("minimal execution timeout ES: {}",
                        Hex16(snapshot.es));
            logger.info("minimal execution timeout SS: {}",
                        Hex16(snapshot.ss));
            logger.info("minimal execution timeout FS: {}",
                        Hex16(snapshot.fs));
            logger.info("minimal execution timeout GS: {}",
                        Hex16(snapshot.gs));
        }
    }
    logger.info("single-step trace count: {}",
                attempt.single_step_trace_count);
    // Task 337: exclusive by construction -- classified in the VEH prologue
    // before any handler can consume the exception. Kernel transition is now
    // 27.7-30.4% of Release wall clock (Task 336) and the remedy differs by
    // class, so which class dominates decides what to build.
    {
        const std::uint32_t census_total =
            attempt.veh_single_step_exception_count +
            attempt.veh_breakpoint_exception_count +
            attempt.veh_access_violation_exception_count +
            attempt.veh_other_exception_count;
        logger.info(
            "exception census single-step/breakpoint/access-violation/"
            "other/total: {}/{}/{}/{}/{}",
            attempt.veh_single_step_exception_count,
            attempt.veh_breakpoint_exception_count,
            attempt.veh_access_violation_exception_count,
            attempt.veh_other_exception_count, census_total);
        if (census_total != 0U)
        {
            const auto census_share = [census_total](std::uint32_t value) {
                return 100.0 * static_cast<double>(value) /
                    static_cast<double>(census_total);
            };
            logger.info(
                "exception census share single-step/breakpoint/"
                "access-violation/other: {:.2f}%/{:.2f}%/{:.2f}%/{:.2f}%",
                census_share(attempt.veh_single_step_exception_count),
                census_share(attempt.veh_breakpoint_exception_count),
                census_share(attempt.veh_access_violation_exception_count),
                census_share(attempt.veh_other_exception_count));
        }
        // Task 343: which codes "other" holds.
        for (std::uint32_t index = 0; index < 4U; ++index)
        {
            if (attempt.veh_other_exception_code_counts[index] == 0U)
            {
                continue;
            }
            logger.info("exception census other code/count: {}/{}",
                        Hex32(attempt.veh_other_exception_codes[index]),
                        attempt.veh_other_exception_code_counts[index]);
        }
        if (attempt.veh_other_exception_code_overflow != 0U)
        {
            logger.info("exception census other code overflow: {}",
                        attempt.veh_other_exception_code_overflow);
        }
        logger.info(
            "single-step run buckets 1/2/3/4/5-8/9-16/17-32/33+: "
            "{}/{}/{}/{}/{}/{}/{}/{}",
            attempt.veh_single_step_run_buckets[0],
            attempt.veh_single_step_run_buckets[1],
            attempt.veh_single_step_run_buckets[2],
            attempt.veh_single_step_run_buckets[3],
            attempt.veh_single_step_run_buckets[4],
            attempt.veh_single_step_run_buckets[5],
            attempt.veh_single_step_run_buckets[6],
            attempt.veh_single_step_run_buckets[7]);
        logger.info(
            "single-step run count/max/mean: {}/{}/{}",
            attempt.veh_single_step_run_total,
            attempt.veh_single_step_run_max,
            attempt.veh_single_step_run_total != 0U
                ? attempt.veh_single_step_exception_count /
                      attempt.veh_single_step_run_total
                : 0U);
    }
    // Task 340: the post-HLE return funnel by reason. Task 339 showed the guest
    // walks under TF because this return fails; this names which condition
    // rejects it.
    {
        // Task 426: the `backend` bucket is gone. It could never count a
        // rejection once the backend set shrank to legacy and dynamic, so the
        // line carries seven fields rather than eight.
        const std::uint32_t reentry_attempts =
            attempt.hle_reentry_reject_not_pending +
            attempt.hle_reentry_reject_segment_write +
            attempt.hle_reentry_reject_outside_arena +
            attempt.hle_reentry_reject_quarantined +
            attempt.hle_reentry_reject_span_unsafe +
            attempt.hle_reentry_success;
        logger.info(
            "hle reentry funnel not-pending/segment-write/"
            "outside-arena/quarantined/span-unsafe/success/total: "
            "{}/{}/{}/{}/{}/{}/{}",
            attempt.hle_reentry_reject_not_pending,
            attempt.hle_reentry_reject_segment_write,
            attempt.hle_reentry_reject_outside_arena,
            attempt.hle_reentry_reject_quarantined,
            attempt.hle_reentry_reject_span_unsafe,
            attempt.hle_reentry_success, reentry_attempts);
        {
            // Task 376: 70.1% of single-step exceptions land here, and until now
            // no counter saw them because both other instruments gate on
            // IsGuestInstructionPointer.
            const auto& oos = attempt.out_of_arena_step_census;
            logger.info(
                "out-of-arena step total/aot-cache/other: {}/{}/{}",
                oos.total_count, oos.location_counts[0], oos.location_counts[1]);
            logger.info(
                "out-of-arena step trace-on/reentry-pending: {}/{}",
                oos.trace_enabled_count, oos.reentry_pending_count);
            logger.info(
                "single-step disposition trace-on/trace-off: {}/{}",
                oos.trace_enabled_handled_count,
                oos.trace_disabled_fallthrough_count);
            logger.info(
                "out-of-arena step first/last eip/address-overflow: "
                "{}/{}/{}",
                Hex32(oos.first_eip), Hex32(oos.last_eip),
                oos.address_overflow_count);
            for (const auto& slot : oos.addresses)
            {
                if (slot.count != 0U)
                {
                    logger.info("out-of-arena step eip {}: {}",
                                Hex32(slot.eip), slot.count);
                }
            }
        }
        logger.info("hle reentry cache miss count: {}",
                    attempt.hle_reentry_reject_cache_miss);
        logger.info("hle reentry segment-write resumed: {}",
                    attempt.hle_reentry_segment_write_resumed);
        // Task 341: which pages quarantine, and whether the write source was
        // known. An unknown source quarantines by default, not by evidence.
        logger.info(
            "quarantine events/unknown-source/deferred/overflow: "
            "{}/{}/{}/{}",
            attempt.quarantine_trace_count,
            attempt.quarantine_unknown_source_count,
            attempt.quarantine_deferred_count,
            attempt.guest_page_write_history_overflow);
        for (std::uint32_t index = 0;
             index < 16U && index < attempt.quarantine_trace_count; ++index)
        {
            const auto& entry = attempt.quarantine_trace[index];
            logger.info(
                "quarantine #{} page/source/destination/bytes: "
                "{}/{}/{}/{}",
                index + 1U, Hex32(entry.page), Hex32(entry.source),
                Hex32(entry.destination), entry.byte_count);
        }
        // Task 405: 98.6% of port I/O faults instead of taking the dispatch
        // slot. `cache` counts executions from inside the AOT cache and `arena`
        // the rest, which separates "the cache emitted a raw in" from "this code
        // was never translated". The total is printed so it can be reconciled
        // against the profiled port I/O count.
        // Task 741: how often the HLE reentry memo spared a decode.
        logger.info(
            "AOT reentry memo hit/miss segment/span/long-mode, generation: "
            "{}/{} {}/{} {}/{}, {}",
            attempt.aot_reentry_memo_hits[0], attempt.aot_reentry_memo_misses[0],
            attempt.aot_reentry_memo_hits[1], attempt.aot_reentry_memo_misses[1],
            attempt.aot_reentry_memo_hits[2], attempt.aot_reentry_memo_misses[2],
            attempt.aot_reentry_memo_generation);
        // Task 741: the breakpoint exceptions by host address, with the VEH
        // exit that ended each, so a per-kind total of a million has names.
        {
            std::vector<std::uint32_t> order;
            std::uint64_t census_total = 0;
            for (std::uint32_t index = 0;
                 index < attempt.breakpoint_site_census_size; ++index)
            {
                order.push_back(index);
                census_total += attempt.breakpoint_site_census[index].count;
            }
            std::sort(order.begin(), order.end(),
                      [&attempt](std::uint32_t left, std::uint32_t right) {
                          return attempt.breakpoint_site_census[left].count >
                              attempt.breakpoint_site_census[right].count;
                      });
            logger.info(
                "breakpoint site census distinct/reported/overflow/"
                "reported-total: {}/{}/{}/{}",
                attempt.breakpoint_site_census_distinct,
                attempt.breakpoint_site_census_size,
                attempt.breakpoint_site_census_overflow, census_total);
            for (std::uint32_t rank = 0;
                 rank < 16U && rank < order.size(); ++rank)
            {
                const auto& entry =
                    attempt.breakpoint_site_census[order[rank]];
                std::string exits;
                for (std::uint32_t slot = 0; slot < 4U; ++slot)
                {
                    if (entry.exit_counts[slot] == 0U)
                    {
                        break;
                    }
                    if (!exits.empty())
                    {
                        exits += ",";
                    }
                    exits += repiu::engine::VehExitSiteName(
                        entry.exit_sites[slot]);
                    exits += ":";
                    exits += std::to_string(entry.exit_counts[slot]);
                }
                if (entry.exit_overflow != 0U)
                {
                    exits += ",other:";
                    exits += std::to_string(entry.exit_overflow);
                }
                logger.info(
                    "breakpoint site #{} host/guest/cache/count/exits: "
                    "{}/{}/{}/{}/{}",
                    rank + 1U, Hex32(entry.host_address),
                    entry.guest_mapped ? Hex32(entry.guest_address)
                                       : std::string("unmapped"),
                    entry.in_cache ? 1U : 0U, entry.count, exits);
            }
        }
        {
            std::vector<std::uint32_t> order;
            std::uint64_t census_total = 0;
            for (std::uint32_t index = 0;
                 index < attempt.port_io_address_census_size; ++index)
            {
                order.push_back(index);
                census_total += attempt.port_io_address_census[index].count;
            }
            std::sort(order.begin(), order.end(),
                      [&attempt](std::uint32_t left, std::uint32_t right) {
                          return attempt.port_io_address_census[left].count >
                              attempt.port_io_address_census[right].count;
                      });
            logger.info(
                "port I/O address census entries/overflow/total: {}/{}/{}",
                attempt.port_io_address_census_size,
                attempt.port_io_address_census_overflow, census_total);
            for (std::uint32_t rank = 0;
                 rank < 16U && rank < order.size(); ++rank)
            {
                const auto& entry =
                    attempt.port_io_address_census[order[rank]];
                logger.info(
                    "port I/O address #{} "
                    "guest/count/cache/arena/mapped/reentry: {}/{}/{}/{}/{}/{}",
                    rank + 1U, Hex32(entry.guest_address), entry.count,
                    entry.cache_count, entry.count - entry.cache_count,
                    entry.mapped_count, entry.reentry_pending_count);
                // Task 408: the first arena-entry transition for this address.
                // `flags` is prev-in-cache/tf/reentry/legacy/step, bit 0 first.
                if (entry.entry_transition_count != 0U)
                {
                    logger.info(
                        "port I/O address #{} entry "
                        "count/prev-code/prev-eip/flags: {}/{}/{}/{}",
                        rank + 1U, entry.entry_transition_count,
                        Hex32(entry.entry_previous_code),
                        Hex32(entry.entry_previous_eip),
                        Hex32(entry.entry_flags));
                    // Task 409: the class of every transition, since the first
                    // sample described at most a tenth of them.
                    logger.info(
                        "port I/O address #{} entry prev "
                        "step/bp/av/other: {}/{}/{}/{}",
                        rank + 1U, entry.entry_prev_single_step,
                        entry.entry_prev_breakpoint,
                        entry.entry_prev_access_violation,
                        entry.entry_prev_other);
                    // Task 410: and who resumed the guest after it. `exit-eip`
                    // equal to `prev-eip` means the consumer did not advance
                    // EIP; a cache address means it went back to the cache.
                    logger.info(
                        "port I/O address #{} entry prev "
                        "exit-site/exit-eip: {}/{}",
                        rank + 1U,
                        repiu::engine::VehExitSiteName(
                            entry.entry_previous_exit_site),
                        Hex32(entry.entry_previous_exit_eip));
                }
            }
        }
        // Task 407: how free-running arena execution is entered. Steady-state
        // faults are filtered out at the recording site, so each line here is a
        // transition.
        // The trace is a ring holding the newest sixteen transitions, so the
        // count is the total and the entries are printed oldest to newest.
        const std::uint32_t arena_entry_total =
            attempt.arena_port_io_entry_trace_count;
        const std::uint32_t arena_entry_shown =
            arena_entry_total < 16U ? arena_entry_total : 16U;
        const std::uint32_t arena_entry_first =
            arena_entry_total < 16U ? 0U : arena_entry_total % 16U;
        logger.info(
            "arena port I/O entry trace total/shown: {}/{}",
            arena_entry_total, arena_entry_shown);
        for (std::uint32_t offset = 0; offset < arena_entry_shown; ++offset)
        {
            const std::uint32_t index = (arena_entry_first + offset) % 16U;
            const auto& entry = attempt.arena_port_io_entry_trace[index];
            logger.info(
                "arena port I/O entry #{} guest/prev-code/prev-eip/"
                "prev-in-cache/tf/reentry/legacy/step: {}/{}/{}/{}/{}/{}/{}/{}",
                index + 1U, Hex32(entry.guest_address),
                Hex32(entry.previous_code), Hex32(entry.previous_eip),
                entry.previous_in_cache, entry.trap_flag,
                entry.reentry_pending, entry.legacy_fallback,
                entry.single_step_trace);
        }
        // Task 410: the population behind the per-address first sample -- every
        // single step taken at an arena EIP, by the VEH exit that consumed it.
        // The total is printed beside the sum so `sum == total` is checkable
        // rather than assumed; Task 409 was undone by trusting a first sample.
        {
            std::uint32_t exit_site_sum = 0;
            for (std::uint32_t site = 0;
                 site < repiu::engine::kVehExitSiteCount; ++site)
            {
                exit_site_sum +=
                    attempt.veh_arena_single_step_exit_site_counts[site];
            }
            logger.info(
                "arena single-step exit total/sum: {}/{}",
                attempt.veh_arena_single_step_count, exit_site_sum);
            for (std::uint32_t site = 0;
                 site < repiu::engine::kVehExitSiteCount; ++site)
            {
                const std::uint32_t count =
                    attempt.veh_arena_single_step_exit_site_counts[site];
                if (count == 0U)
                {
                    continue;
                }
                logger.info(
                    "arena single-step exit {}: {}",
                    repiu::engine::VehExitSiteName(site), count);
            }
        }
        // Task 404: the other way a page quarantines -- a re-translation that
        // failed. The message is the reason, and it decides where the fix
        // belongs, so it is printed verbatim rather than classified here.
        logger.info(
            "AOT generation failure events/overflow: {}/{}",
            attempt.generation_failure_trace_count,
            attempt.generation_failure_trace_overflow);
        for (std::uint32_t index = 0;
             index < 8U && index < attempt.generation_failure_trace_count;
             ++index)
        {
            const auto& entry = attempt.generation_failure_trace[index];
            logger.info(
                "AOT generation failure #{} "
                "target/page/quarantined/terminal/message: {}/{}/{}/{}/{}",
                index + 1U, Hex32(entry.target), Hex32(entry.page),
                entry.quarantined, entry.terminal,
                static_cast<const char*>(entry.message));
        }
        if (reentry_attempts != 0U)
        {
            const auto funnel_share = [reentry_attempts](std::uint32_t value) {
                return 100.0 * static_cast<double>(value) /
                    static_cast<double>(reentry_attempts);
            };
            logger.info(
                "hle reentry funnel share not-pending/segment-write/"
                "outside-arena/quarantined/span-unsafe/success: "
                "{:.2f}%/{:.2f}%/{:.2f}%/{:.2f}%/{:.2f}%/{:.2f}%",
                funnel_share(attempt.hle_reentry_reject_not_pending),
                funnel_share(attempt.hle_reentry_reject_segment_write),
                funnel_share(attempt.hle_reentry_reject_outside_arena),
                funnel_share(attempt.hle_reentry_reject_quarantined),
                funnel_share(attempt.hle_reentry_reject_span_unsafe),
                funnel_share(attempt.hle_reentry_success));
        }
    }
    const auto& step_profile = attempt.single_step_hotspot_profile;
    const double step_count_coverage =
        step_profile.total_sample_count != 0U
            ? 100.0 * step_profile.top_count_coverage_count /
                  step_profile.total_sample_count
            : 0.0;
    const double step_cycle_coverage =
        step_profile.total_cycles != 0U
            ? 100.0 * static_cast<double>(
                  step_profile.top_cycle_coverage_cycles) /
                  static_cast<double>(step_profile.total_cycles)
            : 0.0;
    logger.info(
        "single-step hotspot enabled/total/distinct/overflow: "
        "{}/{}/{}/{}",
        step_profile.enabled,
        step_profile.total_sample_count,
        step_profile.distinct_guest_count,
        step_profile.overflow_count);
    logger.info(
        "single-step hotspot cycles total/max: {}/{}",
        step_profile.total_cycles,
        step_profile.max_cycles);
    logger.info(
        "single-step hotspot dump written/entries/path: {}/{}/{}",
        step_profile.dump_written,
        step_profile.dump_entry_count,
        step_profile.dump_path);
    logger.info(
        "single-step hotspot outcome count "
        "HLE/timer/native/TF: {}/{}/{}/{}",
        step_profile.outcome_counts[0],
        step_profile.outcome_counts[1],
        step_profile.outcome_counts[2],
        step_profile.outcome_counts[3]);
    logger.info(
        "single-step hotspot outcome cycles "
        "HLE/timer/native/TF: {}/{}/{}/{}",
        step_profile.outcome_cycles[0],
        step_profile.outcome_cycles[1],
        step_profile.outcome_cycles[2],
        step_profile.outcome_cycles[3]);
    logger.info(
        "single-step hotspot top count/cycle coverage: "
        "{:.2f}%/{:.2f}%",
        step_count_coverage,
        step_cycle_coverage);
    // Task 322 stage attribution. Stages are sequential regions of
    // HandleSingleStepTrace, so the residual is whatever the total scope
    // measured outside them: inter-stage branching, profiling overhead, and
    // preemption. Kernel #DB entry and post-VEH return stay outside the total
    // and are therefore absent from the residual as well.
    {
        std::uint64_t staged_cycles = 0;
        for (std::uint32_t index = 0;
             index < repiu::engine::kSingleStepProfileStageCount;
             ++index)
        {
            staged_cycles += step_profile.stage_cycles[index];
        }
        const std::uint64_t residual_cycles =
            step_profile.total_cycles > staged_cycles
                ? step_profile.total_cycles - staged_cycles
                : 0U;
        logger.info(
            "single-step stage count "
            "prologue/hle/aot-resume/timer/native: {}/{}/{}/{}/{}",
            step_profile.stage_counts[0], step_profile.stage_counts[1],
            step_profile.stage_counts[2], step_profile.stage_counts[3],
            step_profile.stage_counts[4]);
        logger.info(
            "single-step stage cycles "
            "prologue/hle/aot-resume/timer/native/residual: "
            "{}/{}/{}/{}/{}/{}",
            step_profile.stage_cycles[0], step_profile.stage_cycles[1],
            step_profile.stage_cycles[2], step_profile.stage_cycles[3],
            step_profile.stage_cycles[4], residual_cycles);
        // Task 323 sub-stages of kAotResume. Their residual is the branching
        // and early-out cost between them inside TryResumeAotAfterHandledHle.
        std::uint64_t sub_stage_cycles = 0;
        for (std::uint32_t index =
                 repiu::engine::
                     kSingleStepProfileFirstAotResumeSubStage;
             index < repiu::engine::kSingleStepProfileStageCount;
             ++index)
        {
            sub_stage_cycles += step_profile.stage_cycles[index];
        }
        const std::uint64_t aot_resume_cycles = step_profile.stage_cycles[2];
        logger.info(
            "single-step aot-resume sub-stage count "
            "seg-write/quarantine/cache-lookup/span-safety: {}/{}/{}/{}",
            step_profile.stage_counts[5], step_profile.stage_counts[6],
            step_profile.stage_counts[7], step_profile.stage_counts[8]);
        logger.info(
            "single-step aot-resume sub-stage cycles "
            "seg-write/quarantine/cache-lookup/span-safety/residual: "
            "{}/{}/{}/{}/{}",
            step_profile.stage_cycles[5], step_profile.stage_cycles[6],
            step_profile.stage_cycles[7], step_profile.stage_cycles[8],
            aot_resume_cycles > sub_stage_cycles
                ? aot_resume_cycles - sub_stage_cycles
                : 0U);
    }
    for (std::uint32_t index = 0;
         index < step_profile.count_hotspot_count; ++index)
    {
        const auto& hotspot = step_profile.count_hotspots[index];
        logger.info(
            "single-step count hotspot #{} "
            "address/count/cycles/max/outcome: {}/{}/{}/{}/{}/{}/{}/{}",
            index + 1U, Hex32(hotspot.guest_address),
            hotspot.sample_count, hotspot.total_cycles, hotspot.max_cycles,
            hotspot.outcome_counts[0], hotspot.outcome_counts[1],
            hotspot.outcome_counts[2], hotspot.outcome_counts[3]);
    }
    for (std::uint32_t index = 0;
         index < step_profile.cycle_hotspot_count; ++index)
    {
        const auto& hotspot = step_profile.cycle_hotspots[index];
        logger.info(
            "single-step cycle hotspot #{} "
            "address/count/cycles/max/outcome: {}/{}/{}/{}/{}/{}/{}/{}",
            index + 1U, Hex32(hotspot.guest_address),
            hotspot.sample_count, hotspot.total_cycles, hotspot.max_cycles,
            hotspot.outcome_counts[0], hotspot.outcome_counts[1],
            hotspot.outcome_counts[2], hotspot.outcome_counts[3]);
        logger.info(
            "single-step cycle hotspot #{} stage cycles "
            "prologue/hle/aot-resume/timer/native: {}/{}/{}/{}/{}",
            index + 1U,
            hotspot.stage_cycles[0], hotspot.stage_cycles[1],
            hotspot.stage_cycles[2], hotspot.stage_cycles[3],
            hotspot.stage_cycles[4]);
    }
    // Task 415: whether the generation-failure penalty stayed at address scope
    // or fell back to quarantining a whole page.
    logger.info(
        "AOT generation failure addresses/skips/quarantine-fallbacks/"
        "spanning-activations: {}/{}/{}/{}",
        repiu::engine::AotGenerationFailureAddressCount(),
        repiu::engine::AotGenerationFailureSkipCount(),
        repiu::engine::AotGenerationFailureQuarantineCount(),
        repiu::engine::AotSpanningEntryActivationCount());
    // Task 414: how many port I/O faults the delay-loop batch removed, and why
    // the attempts that did not batch were refused.
    {
        const auto& delay_loop =
            repiu::engine::GetPortIoDelayLoopStats();
        logger.info(
            "port I/O delay loop enabled/attempts/batches/skipped/max: "
            "{}/{}/{}/{}/{}",
            delay_loop.enabled, delay_loop.attempt_count,
            delay_loop.batch_count, delay_loop.skipped_iteration_count,
            delay_loop.max_skipped_iterations);
        logger.info(
            "port I/O delay loop outcome "
            "batched/shape/register/not-dead/nothing/unreadable: "
            "{}/{}/{}/{}/{}/{}",
            delay_loop.outcome_counts[0], delay_loop.outcome_counts[1],
            delay_loop.outcome_counts[2], delay_loop.outcome_counts[3],
            delay_loop.outcome_counts[4], delay_loop.outcome_counts[5]);
        logger.info(
            "port I/O delay loop last body/limit: {}/{}",
            Hex32(delay_loop.last_loop_address), delay_loop.last_limit);
        logger.info(
            "port I/O delay loop wrapped candidates/batches/last-return: "
            "{}/{}/{}",
            delay_loop.wrapped_candidate_count,
            delay_loop.wrapped_batch_count,
            Hex32(delay_loop.last_wrapped_return_address));
    }
    // Task 411: where the guest thread was, sampled on a wall-clock interval.
    // Unlike the hotspot census above, this one sees code that runs in the AOT
    // cache without faulting, which is where a stalled run's wait loop can hide.
    {
        const auto& position = attempt.guest_position_census;
        std::uint32_t origin_total = 0;
        for (std::uint32_t index = 0;
             index < repiu::engine::kGuestPositionOriginCount;
             ++index)
        {
            origin_total += position.origin_counts[index];
        }
        logger.info(
            "guest position census "
            "enabled/total/distinct/overflow/capture-failures/interval-ms: "
            "{}/{}/{}/{}/{}/{}",
            position.enabled, position.total_sample_count,
            position.distinct_address_count, position.overflow_count,
            position.capture_failure_count, position.interval_milliseconds);
        // The sum check is printed rather than assumed, for the reason Task 410
        // recorded: a classification that cannot be reconciled with its total
        // must not be read as a distribution.
        logger.info(
            "guest position origin "
            "arena/cache-mapped/cache-unmapped/host/sum-matches-total: "
            "{}/{}/{}/{}/{}",
            position.origin_counts[0], position.origin_counts[1],
            position.origin_counts[2], position.origin_counts[3],
            origin_total == position.total_sample_count);
        logger.info(
            "guest position census dump written/entries/path: {}/{}/{}",
            position.dump_written, position.dump_entry_count,
            position.dump_path);
        // Task 412: the one measurement that separates "busy in kernel
        // exception dispatch" from "blocked". CPU share near 100% retires the
        // blocked hypothesis; near zero retires the busy one.
        const double cpu_milliseconds =
            static_cast<double>(position.thread_kernel_time_100ns +
                                position.thread_user_time_100ns) /
            10000.0;
        const double cpu_share =
            position.thread_time_elapsed_milliseconds != 0U
                ? 100.0 * cpu_milliseconds /
                      position.thread_time_elapsed_milliseconds
                : 0.0;
        logger.info(
            "guest position thread time "
            "valid/kernel-ms/user-ms/wall-ms/cpu-share: "
            "{}/{:.0f}/{:.0f}/{}/{:.2f}%",
            position.thread_time_valid,
            static_cast<double>(position.thread_kernel_time_100ns) / 10000.0,
            static_cast<double>(position.thread_user_time_100ns) / 10000.0,
            position.thread_time_elapsed_milliseconds,
            cpu_share);
        // Reconciliation before distribution, as with the origin sum above.
        const std::uint32_t scan_total = position.host_scan_sited_count +
            position.host_scan_no_site_count +
            position.host_scan_failed_count;
        logger.info(
            "guest position host scan "
            "samples/sited/no-site/failed/distinct/overflow/parts-match: "
            "{}/{}/{}/{}/{}/{}/{}",
            position.host_scan_sample_count, position.host_scan_sited_count,
            position.host_scan_no_site_count, position.host_scan_failed_count,
            position.host_site_distinct_count,
            position.host_site_overflow_count,
            scan_total == position.host_scan_sample_count);
        for (std::uint32_t index = 0; index < position.top_count; ++index)
        {
            const auto& sample = position.top[index];
            const double share =
                position.total_sample_count != 0U
                    ? 100.0 * sample.sample_count /
                          position.total_sample_count
                    : 0.0;
            logger.info(
                "guest position top #{} "
                "address/count/share/arena/cache/cache-unmapped/host/module: "
                "{}/{}/{:.2f}%/{}/{}/{}/{}/{}+{}",
                index + 1U, Hex32(sample.address), sample.sample_count, share,
                sample.origin_counts[0], sample.origin_counts[1],
                sample.origin_counts[2], sample.origin_counts[3],
                position.top_module_names[index].empty()
                    ? std::string("-")
                    : position.top_module_names[index],
                Hex32(position.top_module_offsets[index]));
        }
        for (std::uint32_t index = 0; index < position.host_site_top_count;
             ++index)
        {
            const auto& site = position.host_site_top[index];
            const double share =
                position.host_scan_sited_count != 0U
                    ? 100.0 * site.sample_count /
                          position.host_scan_sited_count
                    : 0.0;
            logger.info(
                "guest position host site #{} "
                "address/count/share-of-sited/module/offset/symbol: "
                "{}/{}/{:.2f}%/{}/{}/{}",
                index + 1U, Hex32(site.address), site.sample_count, share,
                site.module_name.empty() ? std::string("-") : site.module_name,
                Hex32(site.module_offset),
                site.symbol.empty() ? std::string("-") : site.symbol);
        }
    }
    // Task 323 guest-thread wall-clock attribution. Buckets may nest, so the
    // derived figures are the interpretable ones: kVehExclusive removes service
    // work reached from inside the VEH, and kUnaccounted is guest execution in
    // the AOT cache plus kernel exception transition, neither of which any
    // in-handler scope can observe.
    {
        using repiu::engine::ExecutionTimeBucket;
        const auto& time_profile = attempt.execution_time_profile;
        const auto bucket = [&time_profile](ExecutionTimeBucket id) {
            return time_profile.cycles[static_cast<std::uint32_t>(id)];
        };
        const auto inside = [&time_profile](ExecutionTimeBucket id) {
            return time_profile.inside_veh_cycles[
                static_cast<std::uint32_t>(id)];
        };
        // Task 511: the formulas moved into the profile module, because the live
        // report needs the same five numbers and a second copy is how two
        // reports come to disagree. The names below are unchanged, so everything
        // downstream reads exactly what it read before.
        const repiu::engine::ExecutionTimeShares shares =
            repiu::engine::ComputeExecutionTimeShares(time_profile);
        const std::uint64_t total = shares.total;
        const std::uint64_t veh = shares.veh;
        const std::uint64_t veh_exclusive = shares.veh_exclusive;
        const std::uint64_t unaccounted = shares.unaccounted;
        logger.info(
            "execution time profile enabled: {}", time_profile.enabled);
        logger.info(
            "execution time cycles "
            "guest-run/veh/glide-gate/port-io/dos: {}/{}/{}/{}/{}",
            total, veh,
            bucket(ExecutionTimeBucket::kGlideGate),
            bucket(ExecutionTimeBucket::kPortIoDevice),
            bucket(ExecutionTimeBucket::kDosService));
        logger.info(
            "execution time count "
            "guest-run/veh/glide-gate/port-io/dos: {}/{}/{}/{}/{}",
            time_profile.counts[0], time_profile.counts[1],
            time_profile.counts[2], time_profile.counts[3],
            time_profile.counts[4]);
        {
            // Task 372: the kernel exception round trip, which every bucket above
            // is blind to. Single step is the class that reads as a pure round
            // trip -- one guest instruction separates two consecutive ones.
            const auto& gaps = time_profile.veh_gap_cycles;
            const auto& gap_counts = time_profile.veh_gap_counts;
            const std::uint64_t gap_total =
                gaps[0] + gaps[1] + gaps[2] +
                time_profile.veh_gap_unclassified_cycles;
            logger.info(
                "VEH gap cycles single-step/breakpoint/other/"
                "unclassified/total: {}/{}/{}/{}/{}",
                gaps[0], gaps[1], gaps[2],
                time_profile.veh_gap_unclassified_cycles, gap_total);
            logger.info(
                "VEH gap counts single-step/breakpoint/other: {}/{}/{}",
                gap_counts[0], gap_counts[1], gap_counts[2]);
            logger.info(
                "VEH gap mean single-step/breakpoint/other: {}/{}/{}",
                gap_counts[0] != 0U ? gaps[0] / gap_counts[0] : 0U,
                gap_counts[1] != 0U ? gaps[1] / gap_counts[1] : 0U,
                gap_counts[2] != 0U ? gaps[2] / gap_counts[2] : 0U);
            logger.info(
                "VEH gap min/max/clamped: {}/{}/{}",
                time_profile.veh_gap_min_cycles,
                time_profile.veh_gap_max_cycles,
                time_profile.veh_gap_clamped_count);
            if (total != 0U)
            {
                logger.info(
                    "VEH gap share of wall total/single-step: "
                    "{:.2f}%/{:.2f}%",
                    100.0 * static_cast<double>(gap_total) /
                        static_cast<double>(total),
                    100.0 * static_cast<double>(gaps[0]) /
                        static_cast<double>(total));
            }
        }
        logger.info(
            "execution time inside-veh cycles "
            "glide-gate/port-io/dos: {}/{}/{}",
            inside(ExecutionTimeBucket::kGlideGate),
            inside(ExecutionTimeBucket::kPortIoDevice),
            inside(ExecutionTimeBucket::kDosService));
        logger.info(
            "execution time inside-veh count "
            "glide-gate/port-io/dos: {}/{}/{}",
            time_profile.inside_veh_counts[2],
            time_profile.inside_veh_counts[3],
            time_profile.inside_veh_counts[4]);
        logger.info(
            "execution time derived veh-exclusive/unaccounted: {}/{}",
            veh_exclusive, unaccounted);
        // Task 368 stage one: what exception-free Glide gate dispatch would
        // remove, as opposed to the gate body it would still run.
        logger.info(
            "Glide gate prologue cycles/count/mean/clamped: {}/{}/{}/{}",
            time_profile.glide_gate_prologue_cycles,
            time_profile.glide_gate_prologue_count,
            time_profile.glide_gate_prologue_count == 0U
                ? 0ULL
                : time_profile.glide_gate_prologue_cycles /
                      time_profile.glide_gate_prologue_count,
            time_profile.glide_gate_prologue_clamped_count);
        // Task 325: decomposition of kVehTotal. These are parts of the VEH
        // bucket, not additions to it, so they are reported separately and
        // deliberately excluded from the derived figures above. The residual is
        // VEH time outside every measured region, including the single-step
        // handler and the Glide gate which carry their own instrumentation.
        {
            // Stops before the Task 326 buckets: those nest inside
            // kVehAotTransfer, so including them would over-subtract.
            std::uint64_t veh_sub_cycles = 0;
            for (std::uint32_t index =
                     repiu::engine::kFirstVehSubBucket;
                 index < repiu::engine::kFirstAotHandlerBucket;
                 ++index)
            {
                veh_sub_cycles += time_profile.cycles[index];
            }
            std::uint64_t single_step_cycles = 0;
            for (std::uint32_t index = 0;
                 index <
                     repiu::engine::kSingleStepProfileStageCount;
                 ++index)
            {
                // Sub-stages of kAotResume would double count against their
                // parent stage, so only the top-level five contribute.
                if (index <
                    repiu::engine::
                        kSingleStepProfileFirstAotResumeSubStage)
                {
                    single_step_cycles += step_profile.stage_cycles[index];
                }
            }
            const std::uint64_t veh_measured = veh_sub_cycles +
                inside(ExecutionTimeBucket::kGlideGate) + single_step_cycles;
            const std::uint64_t veh_residual =
                veh > veh_measured ? veh - veh_measured : 0U;
            logger.info(
                "execution time veh sub-bucket cycles "
                "prologue/aot-transfer/telemetry/gates/hle-chain/residual: "
                "{}/{}/{}/{}/{}/{}",
                bucket(ExecutionTimeBucket::kVehPrologue),
                bucket(ExecutionTimeBucket::kVehAotTransfer),
                bucket(ExecutionTimeBucket::kVehTelemetry),
                bucket(ExecutionTimeBucket::kVehBoundaryGates),
                bucket(ExecutionTimeBucket::kVehHleChain),
                veh_residual);
            logger.info(
                "execution time veh sub-bucket count "
                "prologue/aot-transfer/telemetry/gates/hle-chain: "
                "{}/{}/{}/{}/{}",
                time_profile.counts[
                    static_cast<std::uint32_t>(
                        ExecutionTimeBucket::kVehPrologue)],
                time_profile.counts[
                    static_cast<std::uint32_t>(
                        ExecutionTimeBucket::kVehAotTransfer)],
                time_profile.counts[
                    static_cast<std::uint32_t>(
                        ExecutionTimeBucket::kVehTelemetry)],
                time_profile.counts[
                    static_cast<std::uint32_t>(
                        ExecutionTimeBucket::kVehBoundaryGates)],
                time_profile.counts[
                    static_cast<std::uint32_t>(
                        ExecutionTimeBucket::kVehHleChain)]);
            if (veh != 0U)
            {
                logger.info(
                    "execution time veh sub-bucket share "
                    "prologue/aot-transfer/telemetry/gates/hle-chain/"
                    "single-step/residual: "
                    "{:.2f}%/{:.2f}%/{:.2f}%/{:.2f}%/{:.2f}%/{:.2f}%/{:.2f}%",
                    100.0 * static_cast<double>(
                        bucket(ExecutionTimeBucket::kVehPrologue)) /
                        static_cast<double>(veh),
                    100.0 * static_cast<double>(
                        bucket(ExecutionTimeBucket::kVehAotTransfer)) /
                        static_cast<double>(veh),
                    100.0 * static_cast<double>(
                        bucket(ExecutionTimeBucket::kVehTelemetry)) /
                        static_cast<double>(veh),
                    100.0 * static_cast<double>(
                        bucket(ExecutionTimeBucket::kVehBoundaryGates)) /
                        static_cast<double>(veh),
                    100.0 * static_cast<double>(
                        bucket(ExecutionTimeBucket::kVehHleChain)) /
                        static_cast<double>(veh),
                    100.0 * static_cast<double>(single_step_cycles) /
                        static_cast<double>(veh),
                    100.0 * static_cast<double>(veh_residual) /
                        static_cast<double>(veh));
            }
        }
        // Task 326: two decompositions of kVehAotTransfer. The handler axis is
        // mutually exclusive; the function axis nests inside it. The two are
        // never summed together -- each is a share of kVehAotTransfer alone.
        {
            const std::uint64_t transfer =
                bucket(ExecutionTimeBucket::kVehAotTransfer);
            std::uint64_t handler_cycles = 0;
            for (std::uint32_t index =
                     repiu::engine::kFirstAotHandlerBucket;
                 index < repiu::engine::kFirstAotFunctionBucket;
                 ++index)
            {
                handler_cycles += time_profile.cycles[index];
            }
            const std::uint64_t handler_residual =
                transfer > handler_cycles ? transfer - handler_cycles : 0U;
            logger.info(
                "aot transfer handler cycles "
                "write-completion/write-fault/reentry/indirect/conditional/"
                "return/residual: {}/{}/{}/{}/{}/{}/{}",
                bucket(ExecutionTimeBucket::kAotWriteCompletion),
                bucket(ExecutionTimeBucket::kAotWriteFault),
                bucket(ExecutionTimeBucket::kAotReentry),
                bucket(ExecutionTimeBucket::kAotIndirect),
                bucket(ExecutionTimeBucket::kAotConditional),
                bucket(ExecutionTimeBucket::kAotReturn),
                handler_residual);
            logger.info(
                "aot transfer function cycles "
                "resolve/hle-boundary-scan/dynamic-translate/residency: "
                "{}/{}/{}/{}",
                bucket(ExecutionTimeBucket::kAotTransferResolve),
                bucket(ExecutionTimeBucket::kAotHleBoundaryScan),
                bucket(ExecutionTimeBucket::kAotDynamicTranslate),
                bucket(ExecutionTimeBucket::kAotResidency));
            logger.info(
                "aot transfer function count "
                "resolve/hle-boundary-scan/dynamic-translate/residency: "
                "{}/{}/{}/{}",
                time_profile.counts[static_cast<std::uint32_t>(
                    ExecutionTimeBucket::kAotTransferResolve)],
                time_profile.counts[static_cast<std::uint32_t>(
                    ExecutionTimeBucket::kAotHleBoundaryScan)],
                time_profile.counts[static_cast<std::uint32_t>(
                    ExecutionTimeBucket::kAotDynamicTranslate)],
                time_profile.counts[static_cast<std::uint32_t>(
                    ExecutionTimeBucket::kAotResidency)]);
            if (transfer != 0U)
            {
                const auto share = [transfer](std::uint64_t value) {
                    return 100.0 * static_cast<double>(value) /
                        static_cast<double>(transfer);
                };
                logger.info(
                    "aot transfer handler share "
                    "write-completion/write-fault/reentry/indirect/conditional/"
                    "return/residual: "
                    "{:.2f}%/{:.2f}%/{:.2f}%/{:.2f}%/{:.2f}%/{:.2f}%/{:.2f}%",
                    share(bucket(ExecutionTimeBucket::kAotWriteCompletion)),
                    share(bucket(ExecutionTimeBucket::kAotWriteFault)),
                    share(bucket(ExecutionTimeBucket::kAotReentry)),
                    share(bucket(ExecutionTimeBucket::kAotIndirect)),
                    share(bucket(ExecutionTimeBucket::kAotConditional)),
                    share(bucket(ExecutionTimeBucket::kAotReturn)),
                    share(handler_residual));
                logger.info(
                    "aot transfer function share "
                    "resolve/hle-boundary-scan/dynamic-translate/residency: "
                    "{:.2f}%/{:.2f}%/{:.2f}%/{:.2f}%",
                    share(bucket(ExecutionTimeBucket::kAotTransferResolve)),
                    share(bucket(ExecutionTimeBucket::kAotHleBoundaryScan)),
                    share(bucket(ExecutionTimeBucket::kAotDynamicTranslate)),
                    share(bucket(ExecutionTimeBucket::kAotResidency)));
            }
            // Task 334: kAotReentry holds 97.48% of the handler axis while the
            // function axis explains only 7.84% of it, so these six intervals
            // partition the reentry handler itself. The residual against
            // kAotReentry shows a wrong boundary instead of absorbing it.
            const std::uint64_t reentry =
                bucket(ExecutionTimeBucket::kAotReentry);
            std::uint64_t reentry_named = 0;
            for (std::uint32_t index =
                     repiu::engine::kFirstAotReentryBucket;
                 index < repiu::engine::kExecutionTimeBucketCount;
                 ++index)
            {
                reentry_named += time_profile.cycles[index];
            }
            const std::uint64_t reentry_residual =
                reentry > reentry_named ? reentry - reentry_named : 0U;
            logger.info(
                "aot reentry cycles guest-lookup/provenance/retired/"
                "boundary-reason/single-step/residual: "
                "{}/{}/{}/{}/{}/{}",
                bucket(ExecutionTimeBucket::kAotReentryGuestLookup),
                bucket(ExecutionTimeBucket::kAotReentryProvenance),
                bucket(ExecutionTimeBucket::kAotReentryRetired),
                bucket(ExecutionTimeBucket::kAotReentryBoundaryReason),
                bucket(ExecutionTimeBucket::kAotReentrySingleStep),
                reentry_residual);
            const auto reentry_count = [&time_profile](
                                           ExecutionTimeBucket id) {
                return time_profile.counts[static_cast<std::uint32_t>(id)];
            };
            logger.info(
                "aot reentry count guest-lookup/provenance/retired/"
                "boundary-reason/single-step: {}/{}/{}/{}/{}",
                reentry_count(ExecutionTimeBucket::kAotReentryGuestLookup),
                reentry_count(ExecutionTimeBucket::kAotReentryProvenance),
                reentry_count(ExecutionTimeBucket::kAotReentryRetired),
                reentry_count(ExecutionTimeBucket::kAotReentryBoundaryReason),
                reentry_count(ExecutionTimeBucket::kAotReentrySingleStep));
            if (reentry != 0U)
            {
                const auto reentry_share = [reentry](std::uint64_t value) {
                    return 100.0 * static_cast<double>(value) /
                        static_cast<double>(reentry);
                };
                logger.info(
                    "aot reentry share guest-lookup/provenance/retired/"
                    "boundary-reason/single-step/residual: "
                    "{:.2f}%/{:.2f}%/{:.2f}%/{:.2f}%/{:.2f}%/{:.2f}%",
                    reentry_share(
                        bucket(ExecutionTimeBucket::kAotReentryGuestLookup)),
                    reentry_share(
                        bucket(ExecutionTimeBucket::kAotReentryProvenance)),
                    reentry_share(
                        bucket(ExecutionTimeBucket::kAotReentryRetired)),
                    reentry_share(
                        bucket(ExecutionTimeBucket::kAotReentryBoundaryReason)),
                    reentry_share(
                        bucket(ExecutionTimeBucket::kAotReentrySingleStep)),
                    reentry_share(reentry_residual));
            }
        }
        // Task 327: split one translation rendezvous into scheduling latency
        // and worker CPU. The residual against guest_total exposes a wrong
        // measurement boundary rather than hiding it.
        {
            const auto& worker = attempt.aot_worker_timing;
            const std::uint64_t measured =
                worker.wake_latency_cycles + worker.segment_table_cycles +
                worker.append_cycles + worker.complete_latency_cycles;
            const std::uint64_t worker_residual =
                worker.guest_total_cycles > measured
                    ? worker.guest_total_cycles - measured
                    : 0U;
            logger.info(
                "aot worker timing enabled/translate/other/clamped: "
                "{}/{}/{}/{}",
                worker.enabled, worker.translate_count,
                worker.other_operation_count, worker.clamped_sample_count);
            logger.info(
                "aot worker timing cycles "
                "wake/segment-table/append/complete/residual/guest-total: "
                "{}/{}/{}/{}/{}/{}",
                worker.wake_latency_cycles, worker.segment_table_cycles,
                worker.append_cycles, worker.complete_latency_cycles,
                worker_residual, worker.guest_total_cycles);
            logger.info(
                "aot worker timing max wake/append/guest-total: "
                "{}/{}/{}",
                worker.max_wake_latency_cycles, worker.max_append_cycles,
                worker.max_guest_total_cycles);
            if (worker.guest_total_cycles != 0U && worker.translate_count != 0U)
            {
                const auto share = [&worker](std::uint64_t value) {
                    return 100.0 * static_cast<double>(value) /
                        static_cast<double>(worker.guest_total_cycles);
                };
                logger.info(
                    "aot worker timing share "
                    "wake/segment-table/append/complete/residual: "
                    "{:.2f}%/{:.2f}%/{:.2f}%/{:.2f}%/{:.2f}%",
                    share(worker.wake_latency_cycles),
                    share(worker.segment_table_cycles),
                    share(worker.append_cycles),
                    share(worker.complete_latency_cycles),
                    share(worker_residual));
                logger.info(
                    "aot worker timing mean per translate "
                    "guest-total/append/request-gap: {}/{}/{}",
                    worker.guest_total_cycles / worker.translate_count,
                    worker.append_cycles / worker.translate_count,
                    worker.request_gap_cycles / worker.translate_count);
            }
            // Task 328: phases inside one append, plus what one translation
            // covers, so "shrink the translation unit" can be judged rather
            // than assumed.
            const std::uint64_t phase_sum =
                worker.arena_snapshot_cycles + worker.plan_build_cycles +
                worker.image_emit_cycles + worker.validate_cycles +
                worker.placement_cycles;
            const std::uint64_t phase_residual =
                worker.append_cycles > phase_sum
                    ? worker.append_cycles - phase_sum
                    : 0U;
            logger.info(
                "aot append phase cycles "
                "arena-snapshot/plan-build/image-emit/validate/placement/"
                "residual: {}/{}/{}/{}/{}/{}",
                worker.arena_snapshot_cycles, worker.plan_build_cycles,
                worker.image_emit_cycles, worker.validate_cycles,
                worker.placement_cycles, phase_residual);
            if (worker.append_cycles != 0U)
            {
                const auto phase_share = [&worker](std::uint64_t value) {
                    return 100.0 * static_cast<double>(value) /
                        static_cast<double>(worker.append_cycles);
                };
                logger.info(
                    "aot append phase share "
                    "arena-snapshot/plan-build/image-emit/validate/placement/"
                    "residual: "
                    "{:.2f}%/{:.2f}%/{:.2f}%/{:.2f}%/{:.2f}%/{:.2f}%",
                    phase_share(worker.arena_snapshot_cycles),
                    phase_share(worker.plan_build_cycles),
                    phase_share(worker.image_emit_cycles),
                    phase_share(worker.validate_cycles),
                    phase_share(worker.placement_cycles),
                    phase_share(phase_residual));
            }
            if (worker.append_phase_count != 0U)
            {
                logger.info(
                    "aot append scale count/mean "
                    "blocks/instructions/emitted-bytes/snapshot-bytes: "
                    "{}/{}/{}/{}/{}",
                    worker.append_phase_count,
                    worker.plan_block_total / worker.append_phase_count,
                    worker.plan_instruction_total / worker.append_phase_count,
                    worker.emitted_byte_total / worker.append_phase_count,
                    worker.snapshot_byte_total / worker.append_phase_count);
                logger.info(
                    "aot append max snapshot-cycles/instructions: {}/{}",
                    worker.max_arena_snapshot_cycles,
                    worker.max_plan_instruction_count);
            }
            // Task 330: inside plan_build, which Task 329 left as the largest
            // append phase at 39.94%.
            if (worker.plan_profile_count != 0U)
            {
                const std::uint64_t plan_stage_sum =
                    worker.plan_decoder_init_cycles +
                    worker.plan_decode_cycles +
                    worker.plan_record_build_cycles +
                    worker.plan_classify_cycles + worker.plan_walk_cycles +
                    worker.plan_sweep_cycles;
                const std::uint64_t plan_stage_residual =
                    worker.plan_total_cycles > plan_stage_sum
                        ? worker.plan_total_cycles - plan_stage_sum
                        : 0U;
                logger.info(
                    "aot plan stage cycles "
                    "decoder-init/decode/record-build/classify/walk/sweep/"
                    "residual: {}/{}/{}/{}/{}/{}/{}",
                    worker.plan_decoder_init_cycles, worker.plan_decode_cycles,
                    worker.plan_record_build_cycles,
                    worker.plan_classify_cycles, worker.plan_walk_cycles,
                    worker.plan_sweep_cycles, plan_stage_residual);
                if (worker.plan_total_cycles != 0U)
                {
                    const auto plan_share = [&worker](std::uint64_t value) {
                        return 100.0 * static_cast<double>(value) /
                            static_cast<double>(worker.plan_total_cycles);
                    };
                    logger.info(
                        "aot plan stage share "
                        "decoder-init/decode/record-build/classify/walk/sweep/"
                        "residual: "
                        "{:.2f}%/{:.2f}%/{:.2f}%/{:.2f}%/{:.2f}%/{:.2f}%/"
                        "{:.2f}%",
                        plan_share(worker.plan_decoder_init_cycles),
                        plan_share(worker.plan_decode_cycles),
                        plan_share(worker.plan_record_build_cycles),
                        plan_share(worker.plan_classify_cycles),
                        plan_share(worker.plan_walk_cycles),
                        plan_share(worker.plan_sweep_cycles),
                        plan_share(plan_stage_residual));
                }
                logger.info(
                    "aot plan scale builds/decodes/records/sweep-passes/"
                    "sweep-visits/max-passes: {}/{}/{}/{}/{}/{}",
                    worker.plan_profile_count, worker.plan_decode_count,
                    worker.plan_record_count, worker.plan_sweep_pass_count,
                    worker.plan_sweep_record_visit_count,
                    worker.max_plan_sweep_pass_count);
                if (worker.plan_decode_count != 0U)
                {
                    logger.info(
                        "aot plan cycles per instruction "
                        "total/decode/record-build: {}/{}/{}",
                        worker.plan_total_cycles / worker.plan_decode_count,
                        worker.plan_decode_cycles / worker.plan_decode_count,
                        worker.plan_record_build_cycles /
                            worker.plan_decode_count);
                }
            }
        }
        // Task 333: the Glide gate holds 60.78% of Release wall clock, and this
        // splits one rendezvous into waiting for the host thread and the host
        // executing the command. `wake` is the host pump cadence, so a large
        // share there means the cost is latency rather than GL work.
        {
            const auto& gate = attempt.glide_gate_timing;
            logger.info(
                "glide gate timing enabled/rendezvous/direct/clamped: "
                "{}/{}/{}/{}",
                gate.enabled, gate.rendezvous_count, gate.direct_count,
                gate.clamped_sample_count);
            logger.info(
                "glide gate cycles queue/wake/work/complete/residual/"
                "total: {}/{}/{}/{}/{}/{}",
                gate.queue_cycles, gate.wake_cycles, gate.work_cycles,
                gate.complete_cycles, gate.residual_cycles,
                gate.total_cycles);
            logger.info(
                "glide gate direct cycles/max wake/work/total: "
                "{}/{}/{}/{}",
                gate.direct_work_cycles, gate.max_wake_cycles,
                gate.max_work_cycles, gate.max_total_cycles);
            if (gate.total_cycles != 0U)
            {
                const auto gate_share = [&gate](std::uint64_t value) {
                    return 100.0 * static_cast<double>(value) /
                        static_cast<double>(gate.total_cycles);
                };
                logger.info(
                    "glide gate share queue/wake/work/complete/residual: "
                    "{:.2f}%/{:.2f}%/{:.2f}%/{:.2f}%/{:.2f}%",
                    gate_share(gate.queue_cycles),
                    gate_share(gate.wake_cycles),
                    gate_share(gate.work_cycles),
                    gate_share(gate.complete_cycles),
                    gate_share(gate.residual_cycles));
            }
            if (gate.rendezvous_count != 0U)
            {
                logger.info(
                    "glide gate mean per rendezvous total/wake/work: "
                    "{}/{}/{}",
                    gate.total_cycles / gate.rendezvous_count,
                    gate.wake_cycles / gate.rendezvous_count,
                    gate.work_cycles / gate.rendezvous_count);
            }
            // Task 419: how much of that wake and complete latency the spin
            // caught before the condition variable had to. A miss-dominated
            // ratio means the budget is short or the delay is not the
            // scheduler's.
            logger.info(
                "glide gate spin budget-us/guest-hit/guest-miss/"
                "host-hit/host-miss: {}/{}/{}/{}/{}",
                attempt.glide_rendezvous_spin.budget_microseconds,
                attempt.glide_rendezvous_spin.guest_hit,
                attempt.glide_rendezvous_spin.guest_miss,
                attempt.glide_rendezvous_spin.host_hit,
                attempt.glide_rendezvous_spin.host_miss);
        }
        {
            const auto& ordinal = attempt.glide_ordinal_timing;
            logger.info(
                "Glide ordinal timing enabled/entries/completed/"
                "overflow/clamped: {}/{}/{}/{}/{}",
                ordinal.enabled, ordinal.active_entry_count,
                ordinal.completed_gate_count, ordinal.overflow_count,
                ordinal.clamped_sample_count);
            logger.info(
                "Glide ordinal cycles gate/queue/wake/work/complete/"
                "residual/backend-total/direct-work: {}/{}/{}/{}/{}/{}/{}/{}",
                ordinal.gate_cycles, ordinal.queue_cycles,
                ordinal.wake_cycles, ordinal.work_cycles,
                ordinal.complete_cycles, ordinal.residual_cycles,
                ordinal.backend_total_cycles,
                ordinal.direct_work_cycles);
            logger.info(
                "Glide ordinal backend rendezvous/direct: {}/{}",
                ordinal.rendezvous_count, ordinal.direct_count);
        }
        {
            const auto& swap = attempt.glide_buffer_swap_timing;
            logger.info(
                "Glide buffer swap timing enabled/calls/success/failure/"
                "clamped: {}/{}/{}/{}/{}",
                swap.enabled, swap.call_count, swap.success_count,
                swap.failure_count, swap.clamped_sample_count);
            logger.info(
                "Glide buffer swap cycles setup/present/accounting/"
                "finalize/total/max-present: {}/{}/{}/{}/{}/{}",
                swap.setup_cycles, swap.present_cycles,
                swap.accounting_cycles, swap.finalize_cycles,
                swap.total_cycles, swap.max_present_cycles);
            logger.info(
                "Glide buffer swap requested interval "
                "zero/one/other/min/max/last: {}/{}/{}/{}/{}/{}",
                swap.requested_zero_count, swap.requested_one_count,
                swap.requested_other_count, swap.requested_minimum,
                swap.requested_maximum, swap.requested_last);
            logger.info(
                "Glide buffer swap SDL interval "
                "queries/success/failure/value: {}/{}/{}/{}",
                swap.sdl_interval_query_count,
                swap.sdl_interval_query_success_count,
                swap.sdl_interval_query_failure_count,
                swap.observed_sdl_interval);
        }
        {
            const auto& lfb = attempt.glide_lfb_timing;
            logger.info(
                "Glide LFB timing enabled/locks/read-ok/read-fail/"
                "encode-ok/encode-fail/clamped: {}/{}/{}/{}/{}/{}/{}",
                lfb.enabled, lfb.lock_count, lfb.readback_success_count,
                lfb.readback_failure_count, lfb.encode_success_count,
                lfb.encode_failure_count, lfb.clamped_sample_count);
            logger.info(
                "Glide LFB timing cycles readback/encode/total/"
                "max-readback/max-encode/max-total: {}/{}/{}/{}/{}/{}",
                lfb.readback_cycles, lfb.encode_cycles, lfb.total_cycles,
                lfb.max_readback_cycles, lfb.max_encode_cycles,
                lfb.max_total_cycles);
        }
        {
            const auto& footprint = attempt.glide_lfb_write_footprint;
            logger.info(
                "Glide LFB write census enabled/locks/compared/unchanged/"
                "partial-extent/full-extent/all-pixels/malformed: {}/{}/{}/{}/{}/{}/{}/{}",
                footprint.enabled, footprint.write_lock_count,
                footprint.compared_lock_count, footprint.unchanged_lock_count,
                footprint.partial_extent_lock_count,
                footprint.full_extent_lock_count,
                footprint.all_pixels_changed_lock_count,
                footprint.malformed_baseline_count);
            logger.info(
                "Glide LFB write census changed/max-changed/max-bbox pixels: {}/{}/{}",
                footprint.changed_pixel_count, footprint.max_changed_pixel_count,
                footprint.max_bounding_box_pixel_count);
        }
        {
            const auto& stores = attempt.glide_lfb_native_store_census;
            logger.info(
                "Glide LFB native-store census enabled/locks/completed/"
                "malformed/observer/decoded/active-decoded: {}/{}/{}/{}/{}/{}/{}",
                stores.enabled, stores.write_lock_count,
                stores.completed_write_lock_count, stores.malformed_range_count,
                stores.observer_call_count, stores.decoded_store_count,
                stores.active_decoded_store_count);
            logger.info(
                "Glide LFB native-store census overlap stores/bytes/max-bytes: "
                "{}/{}/{}", stores.overlapping_store_count,
                stores.overlapping_byte_count, stores.max_overlapping_byte_count);
            logger.info(
                "Glide LFB native-store census source samples/stride/"
                "overflow-samples/overflow-bytes: {}/{}/{}/{}",
                stores.source_sample_count,
                repiu::engine::kGlideLfbNativeStoreCensusSourceSampleStride,
                stores.source_overflow_sample_count,
                stores.source_overflow_sampled_byte_count);
            repiu::engine::GlideLfbNativeStoreCensusSourceEntry ranks[
                repiu::engine::kGlideLfbNativeStoreCensusReportCapacity] = {};
            repiu::engine::RankGlideLfbNativeStoreCensusSources(
                stores.source_entries.data(), stores.source_entries.size(), ranks,
                repiu::engine::kGlideLfbNativeStoreCensusReportCapacity);
            for (std::size_t index = 0U; index < std::size(ranks); ++index)
            {
                const auto& source = ranks[index];
                if (source.sample_count == 0U)
                {
                    break;
                }
                logger.info(
                    "Glide LFB native-store census source #{} eip=0x{:08X} "
                    "samples/bytes/max-bytes: {}/{}/{}",
                    index + 1U, source.guest_eip,
                    source.sample_count,
                    source.sampled_byte_count,
                    source.max_sampled_byte_count);
            }
        }
        {
            const auto& shadow = attempt.glide_lfb_staging_shadow;
            logger.info(
                "Glide LFB staging shadow census/reuse/locks/reusable/"
                "reused/seeds/stores/rotations: {}/{}/{}/{}/{}/{}/{}/{}",
                shadow.census_enabled, shadow.reuse_enabled, shadow.lock_count,
                shadow.reusable_lock_count, shadow.reused_lock_count,
                shadow.seed_count, shadow.store_count,
                shadow.swap_rotation_count);
            for (std::size_t index = 0U;
                 index < repiu::engine::kGlideLfbStagingShadowReasonCount;
                 ++index)
            {
                const auto reason =
                    static_cast<repiu::engine::GlideLfbStagingShadowInvalidation>(
                        index);
                logger.info(
                    "Glide LFB staging shadow invalidation {}: {}",
                    repiu::engine::GlideLfbStagingShadowInvalidationName(reason),
                    shadow.invalidate_counts[index]);
            }
        }
        {
            const auto& interval = attempt.glide_lfb_lock_interval;
            logger.info(
                "Glide LFB lock interval census enabled/swaps total/min/max: "
                "{}/{}/{}/{}",
                interval.enabled, interval.swap_total, interval.swap_minimum,
                interval.swap_maximum);
            logger.info(
                "Glide LFB lock interval swap buckets zero/one/multi, "
                "clear/draw totals: {}/{}/{}, {}/{}",
                interval.zero_swap_interval_count,
                interval.single_swap_interval_count,
                interval.multi_swap_interval_count, interval.clear_total,
                interval.draw_total);
            for (std::size_t index = 0U;
                 index < repiu::engine::kGlideLfbLockIntervalKindCount; ++index)
            {
                logger.info(
                    "Glide LFB lock interval {}: {}",
                    repiu::engine::GlideLfbLockIntervalName(
                        static_cast<repiu::engine::GlideLfbLockInterval>(index)),
                    interval.interval_counts[index]);
            }
        }
        {
            const auto& census = attempt.glide_setter_census;
            logger.info(
                "Glide setter census enabled/entries/calls/first/same/"
                "changed/failure/unsupported: {}/{}/{}/{}/{}/{}/{}/{}",
                census.enabled, census.active_entry_count, census.call_count,
                census.first_count, census.same_count, census.changed_count,
                census.failure_count, census.unsupported_count);
            logger.info(
                "Glide setter census key-overflow/distinct-overflow/"
                "ordinal-overflow/invalidations/frames/texture-generation: "
                "{}/{}/{}/{}/{}/{}",
                census.key_overflow_count, census.distinct_overflow_count,
                census.ordinal_overflow_count, census.invalidation_count,
                census.frame_count, census.texture_generation);
            const auto& phase = attempt.glide_setter_phase_timing;
            logger.info(
                "Glide setter phase enabled/clamped: {}/{}",
                phase.enabled, phase.clamped_sample_count);
            logger.info(
                "Glide setter phase depth-mask calls/drain/apply/error/"
                "total/max-total/max-apply/max-error/drain-iterations/errors: "
                "{}/{}/{}/{}/{}/{}/{}/{}/{}/{}",
                phase.depth_mask.call_count, phase.depth_mask.drain_cycles,
                phase.depth_mask.apply_cycles, phase.depth_mask.error_cycles,
                phase.depth_mask.total_cycles,
                phase.depth_mask.max_total_cycles,
                phase.depth_mask.max_apply_cycles,
                phase.depth_mask.max_error_cycles,
                phase.depth_mask.drain_iteration_count,
                phase.depth_mask.error_count);
            logger.info(
                "Glide setter phase alpha-blend calls/drain/apply/error/"
                "total/max-total/max-apply/max-error/drain-iterations/errors: "
                "{}/{}/{}/{}/{}/{}/{}/{}/{}/{}",
                phase.alpha_blend.call_count, phase.alpha_blend.drain_cycles,
                phase.alpha_blend.apply_cycles, phase.alpha_blend.error_cycles,
                phase.alpha_blend.total_cycles,
                phase.alpha_blend.max_total_cycles,
                phase.alpha_blend.max_apply_cycles,
                phase.alpha_blend.max_error_cycles,
                phase.alpha_blend.drain_iteration_count,
                phase.alpha_blend.error_count);
            const auto& elision = attempt.glide_setter_state_cache;
            logger.info(
                "Glide setter elision enabled/entries/elided/applied/voided/"
                "invalidations/ordinal-overflow/texture-generation: "
                "{}/{}/{}/{}/{}/{}/{}/{}",
                elision.enabled, elision.active_entry_count,
                elision.elided_count, elision.applied_count,
                elision.voided_count, elision.invalidation_count,
                elision.ordinal_overflow_count, elision.texture_generation);
            // Task 438: the reduction factor is meant to be readable straight
            // from this line -- vertices queued against flushes performed.
            const auto& batch = attempt.glide_draw_batch;
            using reason = repiu::engine::
                GlideDrawBatchFlushReason;
            const auto reason_count = [&batch](reason value) {
                return batch.flush_reasons[static_cast<std::size_t>(value)];
            };
            logger.info(
                "Glide draw batch enabled/primitives-queued/"
                "primitives-drawn/vertices-drawn/flushes/failures/max-batch/"
                "pending/mean-batch: {}/{}/{}/{}/{}/{}/{}/{}/{:.2f}",
                batch.enabled, batch.queued_primitive_count,
                batch.drawn_primitive_count, batch.drawn_vertex_count,
                batch.flush_count, batch.failure_count,
                batch.max_batch_primitive_count,
                batch.pending_primitive_count,
                batch.flush_count == 0U
                    ? 0.0
                    : static_cast<double>(batch.drawn_primitive_count) /
                        static_cast<double>(batch.flush_count));
            logger.info(
                "Glide draw batch flush reason "
                "non-draw-gate/primitive-change/capacity: {}/{}/{}",
                reason_count(reason::kNonDrawGate),
                reason_count(reason::kPrimitiveChange),
                reason_count(reason::kCapacity));
            // Task 440: `failures` and `refused` are the fields to read first --
            // a posted command cannot decline its gate, so this is the only
            // place work lost on that path becomes visible.
            const auto& async_present = attempt.glide_async_present;
            logger.info(
                "Glide async present enabled/posted/swaps/executed/"
                "failures/back-pressure/refused/pending/max-depth: "
                "{}/{}/{}/{}/{}/{}/{}/{}/{}",
                async_present.enabled, async_present.posted_count,
                async_present.posted_swap_count,
                async_present.executed_count, async_present.failure_count,
                async_present.back_pressure_count,
                async_present.refused_count,
                async_present.pending_swap_count,
                async_present.max_queue_depth);

        }
        if (total != 0U)
        {
            logger.info(
                "execution time share "
                "veh/glide-gate/port-io/dos/unaccounted: "
                "{:.2f}%/{:.2f}%/{:.2f}%/{:.2f}%/{:.2f}%",
                100.0 * static_cast<double>(veh) /
                    static_cast<double>(total),
                100.0 * static_cast<double>(
                    bucket(ExecutionTimeBucket::kGlideGate)) /
                    static_cast<double>(total),
                100.0 * static_cast<double>(
                    bucket(ExecutionTimeBucket::kPortIoDevice)) /
                    static_cast<double>(total),
                100.0 * static_cast<double>(
                    bucket(ExecutionTimeBucket::kDosService)) /
                    static_cast<double>(total),
                100.0 * static_cast<double>(unaccounted) /
                    static_cast<double>(total));
        }
    }
    logger.info("native fast path entry/return/cancel: {}/{}/{}",
                attempt.native_fast_path_entry_count,
                attempt.native_fast_path_return_count,
                attempt.native_fast_path_cancel_count);
    logger.info("native fast path last entry/return: {}/{}",
                Hex32(attempt.native_fast_path_last_entry),
                Hex32(attempt.native_fast_path_last_return));
    logger.info(
        "native linear span entry/boundary/cancel/instructions/reject: "
        "{}/{}/{}/{}/{}",
        attempt.native_linear_span_entry_count,
        attempt.native_linear_span_boundary_count,
        attempt.native_linear_span_cancel_count,
        attempt.native_linear_span_instruction_total,
        attempt.native_linear_span_reject_count);
    logger.info(
        "native linear span reject cache hit/miss/stale/store/capacity-skip: "
        "{}/{}/{}/{}/{}",
        attempt.native_linear_span_reject_cache_hit_count,
        attempt.native_linear_span_reject_cache_miss_count,
        attempt.native_linear_span_reject_cache_stale_count,
        attempt.native_linear_span_reject_cache_store_count,
        attempt.native_linear_span_reject_cache_capacity_skip_count);
    logger.info("native linear span write fault-cancel: {}",
                attempt.native_linear_span_write_fault_cancel_count);
    logger.info("native linear span last cancel code/eip: {}/{}",
                Hex32(attempt.native_linear_span_last_cancel_code),
                Hex32(attempt.native_linear_span_last_cancel_eip));
    logger.info("native linear span #DB cancel tf/dr0/dr1/dr2/dr3/other: {}/{}/{}/{}/{}/{}",
                attempt.native_linear_span_cancel_tf_count,
                attempt.native_linear_span_cancel_dr0_count,
                attempt.native_linear_span_cancel_dr1_count,
                attempt.native_linear_span_cancel_dr2_count,
                attempt.native_linear_span_cancel_dr3_count,
                attempt.native_linear_span_cancel_other_db_count);
    logger.info("native linear span #DB first eip tf/dr0/dr1/dr2/dr3/other: {}/{}/{}/{}/{}/{}",
                Hex32(attempt.native_linear_span_cancel_tf_first_eip),
                Hex32(attempt.native_linear_span_cancel_dr0_first_eip),
                Hex32(attempt.native_linear_span_cancel_dr1_first_eip),
                Hex32(attempt.native_linear_span_cancel_dr2_first_eip),
                Hex32(attempt.native_linear_span_cancel_dr3_first_eip),
                Hex32(attempt.native_linear_span_cancel_other_db_first_eip));
    logger.info("execution backend: {}",
                repiu::runtime::ExecutionBackendName(
                    attempt.execution_backend));
    logger.info("AOT entry/boundary/reentry/fallback: {}/{}/{}/{}",
                attempt.aot_cache_entry_count,
                attempt.aot_boundary_count,
                attempt.aot_reentry_count,
                attempt.aot_legacy_fallback_count);
    logger.info("AOT-DBT HLE reentry attempt/success: {}/{}",
                attempt.aot_dbt_hle_reentry_attempt_count,
                attempt.aot_dbt_hle_reentry_success_count);
    logger.info("AOT-DBT post-HLE translation attempt/success: {}/{}",
                attempt.aot_dbt_hle_translation_attempt_count,
                attempt.aot_dbt_hle_translation_success_count);
    logger.info(
        "AOT-DBT HLE host dispatch entry/attempt/success/fallback: "
        "{}/{}/{}/{}",
        attempt.aot_dbt_hle_dispatch_entry_count,
        attempt.aot_dbt_hle_dispatch_attempt_count,
        attempt.aot_dbt_hle_dispatch_success_count,
        attempt.aot_dbt_hle_dispatch_fallback_count);
    logger.info(
        "AOT-DBT HLE host fallback reason "
        "site/veh-required/unhandled/target/state/unknown: "
        "{}/{}/{}/{}/{}/{}",
        attempt.aot_dbt_hle_dispatch_fallback_reason_counts[0],
        attempt.aot_dbt_hle_dispatch_fallback_reason_counts[1],
        attempt.aot_dbt_hle_dispatch_fallback_reason_counts[2],
        attempt.aot_dbt_hle_dispatch_fallback_reason_counts[3],
        attempt.aot_dbt_hle_dispatch_fallback_reason_counts[4],
        attempt.aot_dbt_hle_dispatch_fallback_reason_counts[5]);
    logger.info(
        "AOT selector guard native/HLE/unresolved-site/HLE-exit/mismatch: {}/{}/{}/{}/{}",
        attempt.aot_selector_guard_native_site_count,
        attempt.aot_selector_guard_hle_site_count,
        attempt.aot_selector_guard_unresolved_site_count,
        attempt.aot_selector_guard_hle_exit_count,
        attempt.aot_selector_guard_mismatch_count);
    logger.info(
        "AOT-DBT HLE host last source/next/bytes: {}/{}/{}",
        Hex32(attempt.aot_dbt_hle_dispatch_last_source),
        Hex32(attempt.aot_dbt_hle_dispatch_last_next),
        Hex32(attempt.aot_dbt_hle_dispatch_last_bytes));
    logger.info("AOT guarded segment-pop success/fallback: {}/{}",
                attempt.aot_guarded_segment_pop_success_count,
                attempt.aot_guarded_segment_pop_fallback_count);
    logger.info("AOT guarded segment-load success/fallback: {}/{}",
                attempt.aot_guarded_segment_load_success_count,
                attempt.aot_guarded_segment_load_fallback_count);
    logger.info("AOT timer safe-point trap/injected/deferred: {}/{}/{}",
                attempt.aot_timer_safe_point_trap_count,
                attempt.aot_timer_safe_point_injected_count,
                attempt.aot_timer_safe_point_deferred_count);
    logger.info("INT8 handler entry cache/translated/native: {}/{}/{}",
                attempt.timer_handler_entry_cache_count,
                attempt.timer_handler_entry_translated_count,
                attempt.timer_handler_entry_native_count);
    logger.info(
        "AOT timer source profile enabled/entries/overflow/"
        "attributed-ticks: {}/{}/{}/{}",
        attempt.aot_timer_source_profile.enabled,
        attempt.aot_timer_source_profile.entry_count,
        attempt.aot_timer_source_profile.overflow_count,
        attempt.aot_timer_source_profile.attributed_tick_count);
    {
        const auto timer_sources =
            repiu::engine::
                BuildAotTimerSourceProfileTopEntries(
                    attempt.aot_timer_source_profile,
                    attempt.aot_timer_source_profile.entry_count);
        for (std::size_t index = 0;
             index < timer_sources.size(); ++index)
        {
            const auto& source = timer_sources[index];
            logger.info(
                "AOT timer source top {} "
                "guest/trap/injected/deferred/attributed-ticks/"
                "first-tick/last-tick: {}/{}/{}/{}/{}/{}/{}",
                index + 1U,
                Hex32(source.guest_source),
                source.trap_count,
                source.injected_count,
                source.deferred_count,
                source.attributed_tick_count,
                source.first_global_tick,
                source.last_global_tick);
        }
    }
    logger.info(
        "AOT-DBT return entry/attempt/success/fallback: {}/{}/{}/{}",
        attempt.aot_dbt_return_entry_count,
        attempt.aot_dbt_return_attempt_count,
        attempt.aot_dbt_return_success_count,
        attempt.aot_dbt_return_fallback_count);
    logger.info(
        "AOT-DBT return fallback reason "
        "site/state/opcode/source/zero/hle/quarantine/non-guest/translate/unknown: "
        "{}/{}/{}/{}/{}/{}/{}/{}/{}/{}",
        attempt.aot_dbt_return_fallback_reason_counts[0],
        attempt.aot_dbt_return_fallback_reason_counts[1],
        attempt.aot_dbt_return_fallback_reason_counts[2],
        attempt.aot_dbt_return_fallback_reason_counts[3],
        attempt.aot_dbt_return_fallback_reason_counts[4],
        attempt.aot_dbt_return_fallback_reason_counts[5],
        attempt.aot_dbt_return_fallback_reason_counts[6],
        attempt.aot_dbt_return_fallback_reason_counts[7],
        attempt.aot_dbt_return_fallback_reason_counts[8],
        attempt.aot_dbt_return_fallback_reason_counts[9]);
    std::uint64_t aot_dbt_return_fallback_reason_total = 0;
    for (std::uint32_t count :
         attempt.aot_dbt_return_fallback_reason_counts)
    {
        aot_dbt_return_fallback_reason_total += count;
    }
    logger.info("AOT-DBT return fallback reason total: {}",
                aot_dbt_return_fallback_reason_total);
    logger.info("AOT boundary reason ret/indir/direct/cond/other: "
                "{}/{}/{}/{}/{}",
                attempt.aot_boundary_return_count,
                attempt.aot_boundary_indirect_count,
                attempt.aot_boundary_direct_count,
                attempt.aot_boundary_conditional_count,
                attempt.aot_boundary_other_count);
    logger.info("AOT breakpoint provenance "
                "hle/seg/inline/jtable/retired/probe/fixup/unknown: "
                "{}/{}/{}/{}/{}/{}/{}/{}",
                attempt.aot_breakpoint_provenance_counts[0],
                attempt.aot_breakpoint_provenance_counts[1],
                attempt.aot_breakpoint_provenance_counts[2],
                attempt.aot_breakpoint_provenance_counts[3],
                attempt.aot_breakpoint_provenance_counts[4],
                attempt.aot_breakpoint_provenance_counts[5],
                attempt.aot_breakpoint_provenance_counts[6],
                attempt.aot_breakpoint_provenance_counts[7]);
    logger.info(
        "AOT boundary opcode census samples/escapes/prefixed/segment/"
        "opsize/truncated/prefix-overflow/empty: {}/{}/{}/{}/{}/{}/{}/{}",
        attempt.aot_opcode_census_samples,
        attempt.aot_opcode_census_escapes,
        attempt.aot_opcode_census_prefixed,
        attempt.aot_opcode_census_segment_prefixed,
        attempt.aot_opcode_census_operand_size_prefixed,
        attempt.aot_opcode_census_truncated,
        attempt.aot_opcode_census_prefix_overflow,
        attempt.aot_opcode_census_empty);
    logger.info(
        "AOT boundary effective opcodes "
        "[{:02X}:{} {:02X}:{} {:02X}:{} {:02X}:{} {:02X}:{} {:02X}:{} "
        "{:02X}:{} {:02X}:{}]",
        attempt.aot_effective_opcode_ranks[0].opcode,
        attempt.aot_effective_opcode_ranks[0].count,
        attempt.aot_effective_opcode_ranks[1].opcode,
        attempt.aot_effective_opcode_ranks[1].count,
        attempt.aot_effective_opcode_ranks[2].opcode,
        attempt.aot_effective_opcode_ranks[2].count,
        attempt.aot_effective_opcode_ranks[3].opcode,
        attempt.aot_effective_opcode_ranks[3].count,
        attempt.aot_effective_opcode_ranks[4].opcode,
        attempt.aot_effective_opcode_ranks[4].count,
        attempt.aot_effective_opcode_ranks[5].opcode,
        attempt.aot_effective_opcode_ranks[5].count,
        attempt.aot_effective_opcode_ranks[6].opcode,
        attempt.aot_effective_opcode_ranks[6].count,
        attempt.aot_effective_opcode_ranks[7].opcode,
        attempt.aot_effective_opcode_ranks[7].count);
    logger.info(
        "AOT boundary 0F escape opcodes "
        "[{:02X}:{} {:02X}:{} {:02X}:{} {:02X}:{} {:02X}:{} {:02X}:{} "
        "{:02X}:{} {:02X}:{}]",
        attempt.aot_escape_opcode_ranks[0].opcode,
        attempt.aot_escape_opcode_ranks[0].count,
        attempt.aot_escape_opcode_ranks[1].opcode,
        attempt.aot_escape_opcode_ranks[1].count,
        attempt.aot_escape_opcode_ranks[2].opcode,
        attempt.aot_escape_opcode_ranks[2].count,
        attempt.aot_escape_opcode_ranks[3].opcode,
        attempt.aot_escape_opcode_ranks[3].count,
        attempt.aot_escape_opcode_ranks[4].opcode,
        attempt.aot_escape_opcode_ranks[4].count,
        attempt.aot_escape_opcode_ranks[5].opcode,
        attempt.aot_escape_opcode_ranks[5].count,
        attempt.aot_escape_opcode_ranks[6].opcode,
        attempt.aot_escape_opcode_ranks[6].count,
        attempt.aot_escape_opcode_ranks[7].opcode,
        attempt.aot_escape_opcode_ranks[7].count);
    logger.info("AOT other-boundary top opcodes "
                "[{:02X}:{} {:02X}:{} {:02X}:{} {:02X}:{} {:02X}:{} {:02X}:{} "
                "{:02X}:{} {:02X}:{}] last={}/{}",
                attempt.aot_other_top_opcodes[0], attempt.aot_other_top_counts[0],
                attempt.aot_other_top_opcodes[1], attempt.aot_other_top_counts[1],
                attempt.aot_other_top_opcodes[2], attempt.aot_other_top_counts[2],
                attempt.aot_other_top_opcodes[3], attempt.aot_other_top_counts[3],
                attempt.aot_other_top_opcodes[4], attempt.aot_other_top_counts[4],
                attempt.aot_other_top_opcodes[5], attempt.aot_other_top_counts[5],
                attempt.aot_other_top_opcodes[6], attempt.aot_other_top_counts[6],
                attempt.aot_other_top_opcodes[7], attempt.aot_other_top_counts[7],
                Hex32(attempt.aot_last_other_eip),
                Hex32(attempt.aot_last_other_bytes));
    {
        const std::uint64_t residency_denominator =
            static_cast<std::uint64_t>(attempt.aot_residency_total) +
            attempt.single_step_trace_count;
        const double coverage =
            residency_denominator != 0
                ? 100.0 * static_cast<double>(attempt.aot_residency_total) /
                      static_cast<double>(residency_denominator)
                : 0.0;
        const double average_residency =
            attempt.aot_residency_samples != 0
                ? static_cast<double>(attempt.aot_residency_total) /
                      static_cast<double>(attempt.aot_residency_samples)
                : 0.0;
        // Task 478 gated the sampler off by default, so a zero here means "not
        // measured" rather than "no straight-line instructions". The state is
        // reported alongside the values so a later session cannot read the zero
        // as a result.
        logger.info("AOT residency enabled/total/samples/avg/max/"
                    "coverage%: {}/{}/{}/{:.2f}/{}/{:.2f}",
                    repiu::engine::AotResidencySampleEnabled(),
                    attempt.aot_residency_total, attempt.aot_residency_samples,
                    average_residency, attempt.aot_residency_max, coverage);
    }
    logger.info("AOT last fallback address: {}",
                Hex32(attempt.aot_last_fallback_address));
    logger.info("AOT dynamic attempt/success/bytes: {}/{}/{}",
                attempt.aot_dynamic_attempt_count,
                attempt.aot_dynamic_success_count,
                attempt.aot_dynamic_added_bytes);
    logger.info("AOT indirect dispatch/source/target: {}/{}/{}",
                attempt.aot_indirect_dispatch_count,
                Hex32(attempt.aot_last_indirect_source),
                Hex32(attempt.aot_last_indirect_target));
    logger.info("AOT inline-cache patch attempt/success: {}/{}",
                attempt.aot_inline_cache_patch_attempt_count,
                attempt.aot_inline_cache_patch_success_count);
    logger.info("AOT inline-cache sites/last cache boundary: {}/{}",
                attempt.aot_inline_cache_site_count,
                Hex32(attempt.aot_last_reentry_cache_address));
    // Task 479: `scans` counts patches the site index did not answer, which is
    // how a silently invalidated index shows up as something other than speed.
    logger.info(
        "AOT inline-cache site index sites/indexed/scans/rebuilds: "
        "{}/{}/{}/{}",
        attempt.aot_inline_cache_site_count,
        attempt.aot_inline_cache_site_index_lookup_count,
        attempt.aot_inline_cache_site_index_scan_count,
        attempt.aot_inline_cache_site_index_rebuild_count);
    logger.info(
        "AOT return dispatch site index sites/lookups/scans/rebuilds: "
        "{}/{}/{}/{}",
        attempt.aot_return_dispatch_site_count,
        attempt.aot_return_dispatch_site_index_lookup_count,
        attempt.aot_return_dispatch_site_index_scan_count,
        attempt.aot_return_dispatch_site_index_rebuild_count);
    logger.info(
        "AOT return patch policy observations/megamorphic/bypasses: "
        "{}/{}/{}",
        attempt.aot_return_patch_policy_observation_count,
        attempt.aot_return_patch_policy_megamorphic_site_count,
        attempt.aot_return_patch_policy_bypass_count);
    {
        // Task 482: the return bucket split into five mutually exclusive
        // stages. Coverage is reported beside the outer window so a residual
        // that grows is visible as an attribution gap rather than as speed.
        const auto& stage = attempt.aot_return_stage_profile;
        const std::uint64_t covered =
            repiu::engine::AotReturnStageCoveredCycles(stage);
        const double coverage = stage.outer_cycles != 0U
            ? 100.0 * static_cast<double>(covered) /
                static_cast<double>(stage.outer_cycles)
            : 0.0;
        logger.info(
            "AOT return stage profile enabled/returns/outer/max-outer/"
            "covered/coverage/residual: {}/{}/{}/{}/{}/{:.2f}%/{}",
            stage.enabled, stage.outer_count, stage.outer_cycles,
            stage.max_outer_cycles, covered, coverage,
            stage.residual_cycles);
        logger.info(
            "AOT return stage cycles entry/read/resolve/patch/"
            "continuation: {}/{}/{}/{}/{}",
            stage.cycles[0], stage.cycles[1], stage.cycles[2],
            stage.cycles[3], stage.cycles[4]);
        logger.info(
            "AOT return stage counts entry/read/resolve/patch/"
            "continuation: {}/{}/{}/{}/{}",
            stage.counts[0], stage.counts[1], stage.counts[2],
            stage.counts[3], stage.counts[4]);
        logger.info(
            "AOT return stage max entry/read/resolve/patch/"
            "continuation: {}/{}/{}/{}/{}",
            stage.max_cycles[0], stage.max_cycles[1], stage.max_cycles[2],
            stage.max_cycles[3], stage.max_cycles[4]);
        logger.info(
            "AOT return stage clamped residual/sample: {}/{}",
            stage.residual_clamp_count, stage.clamped_sample_count);
    }
    {
        // Task 499. Hits are counted by the emitted probe itself, so a nonzero
        // value is direct evidence that generated code resolved a return
        // without crossing to the host.
        const std::uint64_t reached =
            attempt.aot_direct_return_table_hit_count +
            attempt.aot_return_patch_policy_observation_count;
        const double hit_share = reached != 0U
            ? 100.0 *
                static_cast<double>(attempt.aot_direct_return_table_hit_count) /
                static_cast<double>(reached)
            : 0.0;
        logger.info(
            "AOT direct-return table enabled/sites/entries/hits/share/"
            "inserts/overwrites/clears: {}/{}/{}/{}/{:.2f}%/{}/{}/{}",
            attempt.aot_direct_return_table_enabled,
            attempt.aot_direct_return_probe_site_count,
            attempt.aot_direct_return_table_entry_count,
            attempt.aot_direct_return_table_hit_count, hit_share,
            attempt.aot_direct_return_table_insert_count,
            attempt.aot_direct_return_table_overwrite_count,
            attempt.aot_direct_return_table_clear_count);
    }
    for (const auto& site : attempt.aot_return_stage_sites)
    {
        logger.info(
            "AOT return stage site: index={} guest={} miss_offset={} "
            "observations={} distinct={} bypasses={} megamorphic={}",
            site.site_index, Hex32(site.guest_source),
            Hex32(site.miss_cache_offset), site.observation_count,
            site.distinct_target_count, site.bypass_count,
            site.megamorphic);
    }
    logger.info("AOT code writes/retire attempt/success: {}/{}/{}",
                attempt.aot_code_write_count,
                attempt.aot_page_retire_attempt_count,
                attempt.aot_page_retire_success_count);
    logger.info("AOT inline cache patches: {}",
                attempt.aot_inline_cache_direct_patch_count);
    logger.info("AOT generation publishes/quarantines: {}/{}",
                attempt.aot_generation_publish_count,
                attempt.aot_quarantine_count);
    logger.info("AOT generation failures/relinked/retired traps: {}/{}/{}",
                attempt.aot_generation_failure_count,
                attempt.aot_generation_relinked_entry_count,
                attempt.aot_retired_entry_trap_count);
    const auto& retired_profile = attempt.aot_retired_trap_profile;
    const double retired_top_coverage =
        retired_profile.total_trap_count != 0U
            ? 100.0 * retired_profile.top_guest_coverage_count /
                  retired_profile.total_trap_count
            : 0.0;
    logger.info(
        "AOT retired profile enabled/total/distinct guest/cache: "
        "{}/{}/{}/{}",
        retired_profile.enabled,
        retired_profile.total_trap_count,
        retired_profile.distinct_guest_count,
        retired_profile.distinct_cache_count);
    logger.info(
        "AOT retired profile top16 coverage/relinkable/short/metadata miss: "
        "{:.2f}%/{}/{}/{}",
        retired_top_coverage,
        retired_profile.relinkable_trap_count,
        retired_profile.short_trap_count,
        retired_profile.metadata_miss_count);
    logger.info(
        "AOT retired profile overflow guest/cache: {}/{}",
        retired_profile.guest_histogram_overflow_count,
        retired_profile.cache_histogram_overflow_count);
    logger.info(
        "AOT retired profile resolution active/generation/quarantine/"
        "failure/fallback/trace: {}/{}/{}/{}/{}/{}",
        retired_profile.resolution_counts[0],
        retired_profile.resolution_counts[1],
        retired_profile.resolution_counts[2],
        retired_profile.resolution_counts[3],
        retired_profile.resolution_counts[4],
        retired_profile.resolution_counts[5]);
    for (std::uint32_t index = 0;
         index < retired_profile.guest_hotspot_count; ++index)
    {
        const auto& hotspot = retired_profile.guest_hotspots[index];
        logger.info("AOT retired guest hotspot #{} address/count: {}/{}",
                    index + 1U,
                    Hex32(hotspot.guest_address),
                    hotspot.trap_count);
    }
    for (std::uint32_t index = 0;
         index < retired_profile.cache_hotspot_count; ++index)
    {
        const auto& hotspot = retired_profile.cache_hotspots[index];
        logger.info(
            "AOT retired cache hotspot #{} cache/guest/count/generation/"
            "guest-length/emitted-length/relinkable: {}/{}/{}/{}/{}/{}/{}",
            index + 1U,
            Hex32(hotspot.cache_address),
            Hex32(hotspot.guest_address),
            hotspot.trap_count,
            hotspot.generation,
            static_cast<std::uint32_t>(hotspot.guest_length),
            static_cast<std::uint32_t>(hotspot.emitted_length),
            hotspot.metadata_valid && hotspot.emitted_length >= 5U);
    }
    logger.info("AOT last code write source/destination: {}/{}",
                Hex32(attempt.aot_last_code_write_source),
                Hex32(attempt.aot_last_code_write_destination));
    logger.info("AOT last retired page/published generation: {}/{}",
                Hex32(attempt.aot_last_retired_page),
                attempt.aot_last_published_generation);
    logger.info("AOT exception cache/guest mapping: {}/{}/{}",
                attempt.aot_exception_mapping_valid ? "valid" : "invalid",
                Hex32(attempt.aot_exception_cache_address),
                Hex32(attempt.aot_exception_guest_address));
    if (attempt.aot_exception_mapping_valid)
    {
        logger.info("AOT exception cache bytes: {}",
                    HexBytes(attempt.aot_exception_cache_bytes,
                             sizeof(attempt.aot_exception_cache_bytes)));
        logger.info("AOT exception guest bytes: {}",
                    HexBytes(attempt.aot_exception_guest_bytes,
                             sizeof(attempt.aot_exception_guest_bytes)));
    }
    logger.info("AOT return dispatch/source/target: {}/{}/{}",
                attempt.aot_return_dispatch_count,
                Hex32(attempt.aot_last_return_source),
                Hex32(attempt.aot_last_return_target));
    logger.info("AOT return stack: {} {} {} {}",
                Hex32(attempt.aot_last_return_stack[0]),
                Hex32(attempt.aot_last_return_stack[1]),
                Hex32(attempt.aot_last_return_stack[2]),
                Hex32(attempt.aot_last_return_stack[3]));
    logger.info("AOT call depth/return match/expected: {}/{}/{}",
                attempt.aot_call_depth,
                attempt.aot_last_return_matches_call ? "true" : "false",
                Hex32(attempt.aot_last_expected_return));
    logger.info("AOT last call source/target: {}/{}",
                Hex32(attempt.aot_last_call_source),
                Hex32(attempt.aot_last_call_target));
    logger.info("AOT expected call source/target: {}/{}",
                Hex32(attempt.aot_last_expected_call_source),
                Hex32(attempt.aot_last_expected_call_target));
    logger.info("AOT return trace entries: {}",
                attempt.aot_return_trace_count);
    const std::uint32_t trace_begin = attempt.aot_return_trace_count >
        repiu::engine::kAotReturnTraceCapacity
        ? attempt.aot_return_trace_count -
              repiu::engine::kAotReturnTraceCapacity
        : 0U;
    for (std::uint32_t sequence = trace_begin;
         sequence < attempt.aot_return_trace_count; ++sequence)
    {
        const auto& trace = attempt.aot_return_trace[
            sequence % repiu::engine::kAotReturnTraceCapacity];
        logger.info("AOT return trace #{} source/actual/expected/ESP/match: {}/{}/{}/{}/{}",
                    sequence + 1U, Hex32(trace.source),
                    Hex32(trace.actual_target), Hex32(trace.expected_target),
                    Hex32(trace.esp), trace.matches ? "true" : "false");
    }
    logger.info("AOT transfer trace entries: {}",
                attempt.aot_transfer_trace_count);
    const std::uint32_t transfer_begin = attempt.aot_transfer_trace_count >
        repiu::engine::kAotTransferTraceCapacity
        ? attempt.aot_transfer_trace_count -
              repiu::engine::kAotTransferTraceCapacity
        : 0U;
    for (std::uint32_t sequence = transfer_begin;
         sequence < attempt.aot_transfer_trace_count; ++sequence)
    {
        const auto& transfer = attempt.aot_transfer_trace[
            sequence % repiu::engine::kAotTransferTraceCapacity];
        logger.info("AOT transfer trace #{} source/target/kind: {}/{}/{}",
                    sequence + 1U, Hex32(transfer.source),
                    Hex32(transfer.target),
                    transfer.is_call ? "call" : "jump");
    }
    if (attempt.aot_dbt_call_return_trace_configured)
    {
        logger.info(
            "AOT-DBT CALL/RET trace "
            "stored-events/calls/returns-observed/matches/mismatches/"
            "overwrites: "
            "{}/{}/{}/{}/{}/{}",
            attempt.aot_dbt_call_return_trace_count,
            attempt.aot_dbt_call_return_call_count,
            attempt.aot_dbt_call_return_return_count,
            attempt.aot_dbt_call_return_match_count,
            attempt.aot_dbt_call_return_mismatch_count,
            attempt.aot_dbt_call_return_overwrite_count);
        const auto log_call_return_trace =
            [&logger](const char* prefix,
                      const repiu::engine::
                          AotCallReturnTraceEntry& entry) {
                const char* origin = entry.origin ==
                        repiu::engine::AotTransferOrigin::kHost
                    ? "host"
                    : "veh";
                if (entry.kind == repiu::engine::
                                      AotCallReturnTraceEventKind::kCall)
                {
                    logger.info(
                        "{} #{} CALL origin/source/target/return/ESP: "
                        "{}/{}/{}/{}/{}",
                        prefix, entry.sequence, origin,
                        Hex32(entry.source), Hex32(entry.target),
                        Hex32(entry.return_address), Hex32(entry.esp));
                    return;
                }
                logger.info(
                    "{} #{} RET origin/source/actual/ESP/call#/expected-"
                    "source/target/return/ESP/correlated/target-match/"
                    "esp-match: {}/{}/{}/{}/{}/{}/{}/{}/{}/{}/{}/{}",
                    prefix, entry.sequence, origin,
                    Hex32(entry.source), Hex32(entry.target),
                    Hex32(entry.esp), entry.call_sequence,
                    Hex32(entry.expected_source),
                    Hex32(entry.expected_target),
                    Hex32(entry.expected_return_address),
                    Hex32(entry.expected_esp),
                    entry.correlated ? "true" : "false",
                    entry.target_matches ? "true" : "false",
                    entry.esp_matches ? "true" : "false");
            };
        if (attempt.aot_dbt_call_return_first_divergence_valid)
        {
            log_call_return_trace(
                "AOT-DBT CALL/RET first divergence",
                attempt.aot_dbt_call_return_first_divergence);
        }
        const std::uint32_t call_return_begin =
            attempt.aot_dbt_call_return_trace_count >
                    repiu::engine::
                        kAotCallReturnTraceCapacity
                ? attempt.aot_dbt_call_return_trace_count -
                      repiu::engine::
                          kAotCallReturnTraceCapacity
                : 0U;
        for (std::uint32_t sequence = call_return_begin;
             sequence < attempt.aot_dbt_call_return_trace_count;
             ++sequence)
        {
            const auto& entry = attempt.aot_dbt_call_return_trace[
                sequence %
                repiu::engine::
                    kAotCallReturnTraceCapacity];
            log_call_return_trace("AOT-DBT CALL/RET trace", entry);
        }
    }
    logger.info("execution probe configured/hit/offset: {}/{}/{}",
                attempt.execution_probe_configured ? "true" : "false",
                attempt.execution_probe_hit ? "true" : "false",
                Hex32(attempt.execution_probe_offset));
    logger.info("execution probe memory offset: {}",
                Hex32(attempt.execution_probe_memory_offset));
    if (attempt.execution_probe_dump_configured)
    {
        logger.info(
            "execution probe dump captured/written/base/source/bytes: "
            "{}/{}/{}/{}/{}",
            attempt.execution_probe_dump_captured ? "true" : "false",
            attempt.execution_probe_dump_written ? "true" : "false",
            Hex32(attempt.execution_probe_dump_base_address),
            Hex32(attempt.execution_probe_dump_source_address),
            Hex32(attempt.execution_probe_dump_byte_count));
        logger.info("execution probe dump path: {}",
                    attempt.execution_probe_dump_path);
    }
    if (attempt.execution_probe_hit)
    {
        const auto& probe = attempt.execution_probe_snapshot;
        logger.info("execution probe EIP/ESP/EFLAGS: {}/{}/{}",
                    Hex32(probe.eip), Hex32(probe.esp),
                    Hex32(probe.eflags));
        logger.info("execution probe EAX/EBX/ECX/EDX: {}/{}/{}/{}",
                    Hex32(probe.eax), Hex32(probe.ebx),
                    Hex32(probe.ecx), Hex32(probe.edx));
        logger.info("execution probe ESI/EDI/EBP: {}/{}/{}",
                    Hex32(probe.esi), Hex32(probe.edi), Hex32(probe.ebp));
        logger.info("execution probe stack: {} {} {} {} {} {} {} {}",
                    Hex32(attempt.execution_probe_stack[0]),
                    Hex32(attempt.execution_probe_stack[1]),
                    Hex32(attempt.execution_probe_stack[2]),
                    Hex32(attempt.execution_probe_stack[3]),
                    Hex32(attempt.execution_probe_stack[4]),
                    Hex32(attempt.execution_probe_stack[5]),
                    Hex32(attempt.execution_probe_stack[6]),
                    Hex32(attempt.execution_probe_stack[7]));
        constexpr const char* kRegisterNames[
            repiu::engine::kExecutionProbeRegisterCount] = {
                "EAX", "EBX", "ECX", "EDX", "ESI", "EDI", "EBP"};
        for (std::uint32_t index = 0;
             index < repiu::engine::
                         kExecutionProbeRegisterCount;
             ++index)
        {
            const auto& window = attempt.execution_probe_memory[index];
            if (!window.valid)
            {
                logger.info(
                    "execution probe memory {} address/valid: {}/false",
                    kRegisterNames[index], Hex32(window.address));
                continue;
            }
            logger.info(
                "execution probe memory {} address/valid/bytes: "
                "{}/true/{}",
                kRegisterNames[index], Hex32(window.address),
                HexBytes(window.bytes, sizeof(window.bytes)));
        }
    }
    if (attempt.execution_trace_configured)
    {
        logger.info(
            "execution trace start/end/esp_offset/hits: "
            "{}/{}/{}/{}",
            Hex32(attempt.execution_trace_start_offset),
            Hex32(attempt.execution_trace_end_offset),
            Hex32(attempt.execution_trace_esp_offset),
            attempt.execution_trace_hit_count);
        if (attempt.execution_trace_sentinel2_configured)
        {
            logger.info(
                "execution trace sentinel2_offset/rearm_count: {}/{}",
                Hex32(attempt.execution_trace_sentinel2_offset),
                attempt.execution_trace_sentinel_rearm_count);
        }
        const std::uint32_t stored = std::min<std::uint32_t>(
            attempt.execution_trace_hit_count,
            static_cast<std::uint32_t>(
                sizeof(attempt.execution_trace) /
                sizeof(attempt.execution_trace[0])));
        for (std::uint32_t index = 0; index < stored; ++index)
        {
            const auto& entry = attempt.execution_trace[index];
            logger.info(
                "execution trace #{} seq={} eip/esp/value: {}/{}/{}",
                index, entry.sequence, Hex32(entry.eip), Hex32(entry.esp),
                Hex32(entry.value_at_esp_offset));
        }
    }
    logger.info("diagnostic poll iterations: {}",
                attempt.diagnostic_poll_iteration_count);
    logger.info("diagnostic progress count: {}",
                attempt.diagnostic_progress_count);
    logger.info("diagnostic quiet iterations: {}",
                attempt.diagnostic_quiet_iteration_count);
    logger.info("exception dispatch entry count: {}",
                attempt.exception_dispatch_entry_count);
    logger.info("exception dispatch exit count: {}",
                attempt.exception_dispatch_exit_count);
    const std::uint32_t outstanding_dispatch_count =
        attempt.exception_dispatch_entry_count >=
                attempt.exception_dispatch_exit_count
            ? attempt.exception_dispatch_entry_count -
                  attempt.exception_dispatch_exit_count
            : 0;
    logger.info("exception dispatch outstanding count: {}",
                outstanding_dispatch_count);
    logger.info("exception dispatch last EIP: {}",
                Hex32(attempt.exception_dispatch_last_eip));
    logger.info("exception dispatch malformed count: {}",
                attempt.exception_dispatch_malformed_count);
    logger.info("exception dispatch last bad ContextRecord: {}",
                Hex32(attempt.exception_dispatch_last_bad_context));
    logger.info("exception dispatch last bad ExceptionRecord: {}",
                Hex32(attempt.exception_dispatch_last_bad_record));
    logger.info("selector table valid: {}",
                attempt.selector_table_valid ? "true" : "false");
    logger.info("selector descriptor count: {}",
                attempt.selector_descriptor_count);
    logger.info("LINEXE environment active: {}",
                attempt.linexe_environment_active ? "true" : "false");
    logger.info("LINEXE saved client GS: {}",
                Hex32(attempt.linexe_saved_client_gs));
    logger.info("LINEXE client descriptor: {} base={} limit={}",
                attempt.linexe_client_descriptor_valid ? "valid" : "invalid",
                Hex32(attempt.linexe_client_descriptor_base),
                Hex32(attempt.linexe_client_descriptor_limit));
    logger.info("LINEXE direct root: {}:{}",
                Hex32(attempt.linexe_root_selector),
                Hex32(attempt.linexe_root_offset));
    logger.info("LINEXE data descriptor: {} base={}",
                attempt.linexe_data_descriptor_valid ? "valid" : "invalid",
                Hex32(attempt.linexe_data_descriptor_base));
    logger.info("LINEXE module name pointer: {}:{}",
                Hex32(attempt.linexe_module_name_selector),
                Hex32(attempt.linexe_module_name_offset));
    logger.info("LINEXE direct module name: {}",
                attempt.linexe_direct_module_name);
    logger.info("LINEXE direct exports: count={} table={}:{}",
                attempt.linexe_direct_export_count,
                Hex32(attempt.linexe_direct_export_table_selector),
                Hex32(attempt.linexe_direct_export_table_offset));
    logger.info("LINEXE direct first export name: {}:{}",
                Hex32(attempt.linexe_direct_first_export_name_selector),
                Hex32(attempt.linexe_direct_first_export_name_offset));
    logger.info("LINEXE GS byte loads: {} first_offset={} first_value={}",
                attempt.linexe_gs_byte_load_count,
                Hex32(attempt.linexe_first_gs_byte_offset),
                Hex32(attempt.linexe_first_gs_byte_value));
    logger.info("LINEXE resolved export count: {}",
                attempt.linexe_resolved_export_count);
    logger.info("LINEXE scan entry/match/return: {}/{}/{}",
                attempt.linexe_scan_entry_count,
                attempt.linexe_export_match_count,
                attempt.linexe_scan_return_count);
    logger.info("LINEXE indirect far call count/source/pointer/target: {}/{}/{}/{}:{} ({})",
                attempt.linexe_indirect_far_call_count,
                Hex32(attempt.linexe_indirect_far_call_source),
                Hex32(attempt.linexe_indirect_far_call_pointer),
                Hex32(attempt.linexe_indirect_far_call_selector),
                Hex32(attempt.linexe_indirect_far_call_offset),
                attempt.linexe_indirect_far_call_known_export ? "known export" : "unknown");
    {
        const auto& ticks = attempt.timer_tick_delivery;
        logger.info(
            "timer tick delivery due/injected/dropped/deferred/max-backlog/"
            "remaining: {}/{}/{}/{}/{}/{}",
            ticks.due_total, ticks.injected_total, ticks.dropped_total,
            ticks.deferred_total, ticks.max_backlog, ticks.backlog);
        // Task 431: ticks that came due while the guest was blocked in the
        // Glide gate, where no safe point is reachable until the gate returns.
        logger.info("timer tick in-gate due: {}", ticks.due_in_gate_total);
        // Task 735: ticks held back because IRQ0 was still in service, and how
        // each service ended -- by the ISR's EOI or by its return.
        // Task 740: the PIU10 MP3 pipeline. `stats()` existed but nothing
        // printed it, so a run with noise or starvation left no number.
        const auto& mp3 = attempt.piu10_mp3_stats;
        logger.info(
            "PIU10 MP3 received/dropped/decoded/starvation/toggles/multi/"
            "pcm-empty/pcm-low-water-bytes/device-buffer-frames: "
            "{}/{}/{}/{}/{}/{}/{}/{}/{}",
            mp3.received_bytes, mp3.dropped_bytes, mp3.decoded_frames,
            mp3.starvation_events, mp3.sync_toggles,
            mp3.sync_multi_toggle_events, mp3.pcm_empty_events,
            mp3.pcm_queued_low_water_bytes, mp3.device_buffer_frames);
        // Issue #58: the guest thread has stopped, so the trace is complete.
        repiu::engine::WriteTimerLatencyTrace();
        // Issue #58: how far the arrows moved at the song clock overwrites.
        const auto& song_clock = attempt.piu10_song_clock;
        logger.info(
            "PIU10 song clock songs/resyncs/jumps-over-{:.0f}ms/"
            "max-ahead-ms/max-back-ms/max-tick-gap-ms/max-toggle-gap-ms: "
            "{}/{}/{}/{:.1f}/{:.1f}/{:.1f}/{:.1f}",
            repiu::engine::kPiu10SongClockReportJumpMs, song_clock.songs,
            song_clock.resyncs, song_clock.jumps_over_report,
            song_clock.jump_max_ahead_ms, song_clock.jump_max_back_ms,
            song_clock.tick_gap_max_ms, song_clock.toggle_gap_max_ms);
        const auto& frame_ticks = song_clock.frame_ticks;
        logger.info(
            "PIU10 song frame ticks 0/1/2/3/4/5/6/7/8/9+: "
            "{}/{}/{}/{}/{}/{}/{}/{}/{}/{}",
            frame_ticks[0], frame_ticks[1], frame_ticks[2], frame_ticks[3],
            frame_ticks[4], frame_ticks[5], frame_ticks[6], frame_ticks[7],
            frame_ticks[8], frame_ticks[9]);
        const auto& in_service = attempt.pic_timer_in_service;
        logger.info(
            "timer IRQ0 in-service blocked/sti-delivered/sti-chain-blocked/"
            "eoi-cleared/stack-retired/active-at-end: {}/{}/{}/{}/{}/{}",
            in_service.blocked_in_service_total,
            in_service.delivered_at_sti_total,
            in_service.blocked_sti_chain_total,
            in_service.cleared_by_eoi_total,
            in_service.retired_by_stack_total, in_service.active);
        // Task 751: the guest's own `cli`, which the host context cannot hold.
        logger.info(
            "guest cli hold cli/sti/blocked/expired/max-hold-us/"
            "held-at-end: {}/{}/{}/{}/{}/{}",
            in_service.cli_total, in_service.sti_total,
            in_service.blocked_by_cli_total,
            in_service.cli_hold_expired_total,
            in_service.cli_hold_max_ns / 1000U, in_service.cli_hold);
        logger.info(
            "timer IRQ0 after-return delivered/blocked: {}/{}",
            in_service.post_return_delivered_total,
            in_service.blocked_post_return_total);
        logger.info(
            "timer IRQ0 nesting return-seen/blocked-nested/depth-at-end: "
            "{}/{}/{}",
            in_service.handler_return_seen, in_service.blocked_nested_total,
            in_service.handler_depth);
        logger.info(
            "timer IRQ0 turn blocked/last-handler-us/handler-max-us: {}/{}/{}",
            in_service.blocked_by_turn_total,
            in_service.last_handler_ns / 1000U,
            in_service.handler_max_ns / 1000U);
        // Task 762. The handler returns the direct model saw.
        const auto& return_pad = attempt.timer_return_pad;
        logger.info(
            "timer return pad pushed/returned/supervised/overflow/unmatched/"
            "abandoned/depth-max/depth-at-end: {}/{}/{}/{}/{}/{}/{}/{}",
            return_pad.pushed_total, return_pad.returned_total,
            return_pad.supervised_total, return_pad.overflow_total,
            return_pad.unmatched_total, return_pad.abandoned_total,
            return_pad.depth_max, return_pad.depth);
    }
    logger.info("INT 8 chain HLE count/source/pointer/target: {}/{}/{}/{}:{}",
                attempt.timer_interrupt_chain_hle_count,
                Hex32(attempt.timer_interrupt_chain_hle_source),
                Hex32(attempt.timer_interrupt_chain_hle_pointer),
                Hex32(attempt.timer_interrupt_chain_hle_selector),
                Hex32(attempt.timer_interrupt_chain_hle_offset));
    logger.info("LINEXE export entry loop count: {}",
                attempt.linexe_export_entry_loop_count);
    logger.info("LINEXE export compare count/EAX/ECX/EFLAGS: {}/{}/{}/{}",
                attempt.linexe_export_compare_count,
                Hex32(attempt.linexe_export_compare_eax),
                Hex32(attempt.linexe_export_compare_ecx),
                Hex32(attempt.linexe_export_compare_eflags));
    logger.info("LINEXE export count load EDX/GS: {}/{}",
                Hex32(attempt.linexe_export_count_load_edx),
                Hex32(attempt.linexe_export_count_load_gs));
    logger.info("LINEXE module candidate/match: {}/{}",
                attempt.linexe_module_candidate_count,
                attempt.linexe_module_match_count);
    logger.info("LINEXE name pointer/byte instruction: {}/{}",
                attempt.linexe_name_pointer_valid_count,
                attempt.linexe_name_byte_instruction_count);
    logger.info("LINEXE GS=0090h load count: {}",
                attempt.linexe_data_gs_load_count);
    logger.info("LINEXE module selector stack value: {}",
                Hex32(attempt.linexe_module_selector_stack_value));
    logger.info("LINEXE module/export stack pointer: {}:{}/{}:{}",
                Hex32(attempt.linexe_module_selector_stack_value),
                Hex32(attempt.linexe_module_offset_stack_value),
                Hex32(attempt.linexe_export_selector_stack_value),
                Hex32(attempt.linexe_export_offset_stack_value));
    logger.info("LINEXE export jump source ESP/module: {}/{}:{}",
                Hex32(attempt.linexe_export_jump_source_esp),
                Hex32(attempt.linexe_export_jump_source_module_selector),
                Hex32(attempt.linexe_export_jump_source_module_offset));
    logger.info("LINEXE export jump target ESP/module: {}/{}:{}",
                Hex32(attempt.linexe_export_jump_target_esp),
                Hex32(attempt.linexe_export_jump_target_module_selector),
                Hex32(attempt.linexe_export_jump_target_module_offset));
    logger.info("LINEXE export name compare count/GS/EDI/ESI/bytes: {}/{}/{}/{}/{}/{}",
                attempt.linexe_export_name_compare_count,
                Hex32(attempt.linexe_export_name_compare_gs),
                Hex32(attempt.linexe_export_name_compare_edi),
                Hex32(attempt.linexe_export_name_compare_esi),
                Hex32(attempt.linexe_export_name_actual_byte),
                Hex32(attempt.linexe_export_name_expected_byte));
    logger.info("LINEXE export name stage mask: {}",
                Hex32(attempt.linexe_export_name_stage_mask));
    logger.info("LINEXE export entry name loaded offset/selector: {}/{}",
                Hex32(attempt.linexe_export_entry_name_offset_value),
                Hex32(attempt.linexe_export_entry_name_selector_value));
    logger.info("LINEXE export result stores count/destination/value: {}/{}/{}",
                attempt.linexe_export_result_store_count,
                Hex32(attempt.linexe_export_result_store_destination),
                Hex32(attempt.linexe_export_result_store_value));
    logger.info("LINEXE export value load selector/offset/value: {}/{}/{}",
                Hex32(attempt.linexe_export_value_load_selector),
                Hex32(attempt.linexe_export_value_load_offset),
                Hex32(attempt.linexe_export_value_load_value));
    logger.info("LINEXE bridge entry/gate/target/service: {}/{}/{}:{}/{}",
                attempt.linexe_bridge_entry_count,
                attempt.linexe_bridge_gate_valid ? "valid" : "invalid",
                Hex32(attempt.linexe_bridge_selector),
                Hex32(attempt.linexe_bridge_offset),
                attempt.linexe_bridge_service);
    logger.info("LINEXE bridge ESP/EBP: {}/{}",
                Hex32(attempt.linexe_bridge_esp),
                Hex32(attempt.linexe_bridge_ebp));
    logger.info("LINEXE bridge stack: {} {} {} {} {} {}",
                Hex32(attempt.linexe_bridge_stack[0]),
                Hex32(attempt.linexe_bridge_stack[1]),
                Hex32(attempt.linexe_bridge_stack[2]),
                Hex32(attempt.linexe_bridge_stack[3]),
                Hex32(attempt.linexe_bridge_stack[4]),
                Hex32(attempt.linexe_bridge_stack[5]));
    logger.info("LINEXE bridge stack continued: {} {} {} {} {} {}",
                Hex32(attempt.linexe_bridge_stack[6]),
                Hex32(attempt.linexe_bridge_stack[7]),
                Hex32(attempt.linexe_bridge_stack[8]),
                Hex32(attempt.linexe_bridge_stack[9]),
                Hex32(attempt.linexe_bridge_stack[10]),
                Hex32(attempt.linexe_bridge_stack[11]));
    logger.info("LINEXE bridge stack tail: {} {} {} {} {} {} {} {}",
                Hex32(attempt.linexe_bridge_stack[12]),
                Hex32(attempt.linexe_bridge_stack[13]),
                Hex32(attempt.linexe_bridge_stack[14]),
                Hex32(attempt.linexe_bridge_stack[15]),
                Hex32(attempt.linexe_bridge_stack[16]),
                Hex32(attempt.linexe_bridge_stack[17]),
                Hex32(attempt.linexe_bridge_stack[18]),
                Hex32(attempt.linexe_bridge_stack[19]));
    for (std::size_t index = 0;
         index < std::size(attempt.linexe_bridge_stack_text);
         ++index)
    {
        if (attempt.linexe_bridge_stack_text[index][0] != '\0')
        {
            logger.info("LINEXE bridge stack text #{}: {}",
                        index,
                        attempt.linexe_bridge_stack_text[index]);
        }
    }
    logger.info("LINEXE bridge argument text: {}",
                attempt.linexe_bridge_argument_text);
    logger.info("LINEXE virtual module loads/handle: {}/{}",
                attempt.linexe_virtual_module_load_count,
                Hex32(attempt.linexe_virtual_module_handle));
    logger.info("LINEXE get-proc count/name/result: {}/{}/{}",
                attempt.linexe_get_proc_count,
                attempt.linexe_get_proc_name,
                Hex32(attempt.linexe_get_proc_result_pointer));
    logger.info("Glide gate entries/handled/ESP: {}/{}/{}",
                attempt.glide_gate_entry_count,
                attempt.glide_gate_handled_count,
                Hex32(attempt.glide_gate_esp));
    {
        // Task 369. Unconditional, unlike the profile blocks above: this line
        // says whether GL errors were being reported at all, so a run that
        // omitted it could not be told apart from a clean one.
        const auto& gl_error = attempt.glide_gl_error_policy;
        logger.info(
            "Glide GL error policy per-call-check/frame-interval/"
            "frame-checks/frame-errors/first-code/drain-iterations: "
            "{}/{}/{}/{}/{}/{}",
            gl_error.per_call_check_enabled ? "true" : "false",
            gl_error.frame_interval,
            gl_error.frame_check_count,
            gl_error.frame_error_count,
            Hex32(gl_error.first_error_code),
            gl_error.drain_iteration_count);
        logger.info(
            "Glide GL debug output installed/messages/errors/first-id: "
            "{}/{}/{}/{}",
            gl_error.debug_output_installed ? "true" : "false",
            gl_error.debug_message_count,
            gl_error.debug_error_count,
            Hex32(gl_error.first_debug_message_id));
        if (gl_error.first_debug_message[0] != '\0')
        {
            logger.error("Glide GL debug first message: {}",
                         gl_error.first_debug_message.data());
        }
        // Task 371: the effective value is read back from the driver rather than
        // echoed, because a refused or clamped request would otherwise invalidate
        // an A/B without saying so.
        const auto& swap_policy = attempt.glide_swap_interval_policy;
        logger.info(
            "Glide swap interval override requested/value/applied/"
            "effective: {}/{}/{}/{}",
            swap_policy.override_requested ? "true" : "false",
            swap_policy.requested_interval,
            swap_policy.applied ? "true" : "false",
            swap_policy.effective_valid
                ? std::to_string(swap_policy.effective_interval)
                : std::string("unknown"));
        // Task 752: what draws.
        logger.info("Glide GL renderer/wsl-d3d12-chosen: {}/{}",
                    swap_policy.gl_renderer[0] != '\0'
                        ? swap_policy.gl_renderer : "none",
                    swap_policy.wsl_d3d12_selected ? "true" : "false");
        // Task 745: the refusal's reason and the pacing that stood in for it.
        // Task 766: printed whenever an interval was requested, which is now
        // every windowed run rather than only an override.
        if (swap_policy.interval_requested)
        {
            logger.info(
                "Glide swap pacing active/refresh-hz/period-us/paced-swaps/"
                "slept-ms/refusal: {}/{:.2f}/{}/{}/{}/{}",
                swap_policy.pacing_active ? "true" : "false",
                swap_policy.refresh_rate_hz, swap_policy.pacing_period_us,
                swap_policy.paced_swaps, swap_policy.paced_sleep_us / 1000U,
                swap_policy.failure[0] != '\0' ? swap_policy.failure
                                                : "none");
            // Task 748: the paced frames' jitter sources.
            logger.info(
                "Glide swap pacing late-swaps/late-max-us/resyncs/"
                "oversleep>1ms/oversleep-max-us: {}/{}/{}/{}/{}",
                swap_policy.late_swaps, swap_policy.late_max_us,
                swap_policy.resyncs, swap_policy.oversleep_over_1ms,
                swap_policy.oversleep_max_us);
            // Task 750: the waits the guest spent at the swap gate with the
            // timer ISR running.
            logger.info(
                "Glide swap wait ticks swaps/injections: {}/{}",
                swap_policy.wait_tick_swaps, swap_policy.wait_tick_injections);
            // Task 754: SDL's raw clock against the steady clock, and which
            // of them the timer ticks followed.
            logger.info(
                "host clock raw-against-steady tick-clock/total-ppm/"
                "worst-second-ppm/seconds/seconds-over-0.5%: {}/{}/{}/{}/{}",
                swap_policy.event_clock_steady ? "steady" : "raw",
                swap_policy.clock_total_ppm,
                swap_policy.clock_worst_window_ppm,
                swap_policy.clock_windows,
                swap_policy.clock_windows_over_limit);
        }
    }
    logger.info("Glide gate ordinal/name/argument bytes: {}/{}/{}",
                attempt.glide_gate_ordinal,
                attempt.glide_gate_name,
                attempt.glide_gate_argument_bytes);
    const auto& glide_issues = attempt.glide_implementation_issues;
    logger.info(
        "Glide implementation issues"
        " unimplemented/unsupported/backend/abi/unique/overflow:"
        " {}/{}/{}/{}/{}/{}",
        glide_issues.total(
            repiu::hle::GlideImplementationIssueKind::
                kUnimplementedFunction),
        glide_issues.total(
            repiu::hle::GlideImplementationIssueKind::
                kUnsupportedArgument),
        glide_issues.total(
            repiu::hle::GlideImplementationIssueKind::kBackendFailure),
        glide_issues.total(
            repiu::hle::GlideImplementationIssueKind::kAbiReject),
        glide_issues.observations().size(),
        glide_issues.overflow_count());
    for (const auto& issue : glide_issues.observations())
    {
        const bool terminate =
            issue.kind ==
                repiu::hle::GlideImplementationIssueKind::kAbiReject ||
            issue.reason == "signature-not-cataloged";
        const std::string line =
            repiu::hle::FormatGlideImplementationIssue(
                issue, terminate ? "terminate" : "continue");
        if (repiu::hle::IsGlideImplementationIssueFatal(issue.kind))
        {
            logger.critical("FATAL {}", line);
        }
        else
        {
            logger.error("{}", line);
        }
    }
    logger.info("Glide window opens/logical size: {}/{}x{}",
                attempt.glide_window_open_count,
                attempt.glide_logical_width,
                attempt.glide_logical_height);
    logger.info("Glide backend message: {}",
                attempt.glide_backend_message);
    logger.info("Glide virtual texture bytes/max address: {}/{}",
                attempt.glide_texture_memory_bytes,
                Hex32(attempt.glide_texture_max_address));
    logger.info("Glide gate stack: {} {} {} {} {} {} {} {}",
                Hex32(attempt.glide_gate_stack[0]),
                Hex32(attempt.glide_gate_stack[1]),
                Hex32(attempt.glide_gate_stack[2]),
                Hex32(attempt.glide_gate_stack[3]),
                Hex32(attempt.glide_gate_stack[4]),
                Hex32(attempt.glide_gate_stack[5]),
                Hex32(attempt.glide_gate_stack[6]),
                Hex32(attempt.glide_gate_stack[7]));
    {
        // Task 375: identical-repeat is the number that decides whether an upload
        // cache is worth building; decode failures are textures the screen is
        // silently missing.
        const auto& tex = attempt.glide_texture_census;
        logger.info(
            "Glide texture census uploads/distinct/identical-repeats/"
            "changed-repeats: {}/{}/{}/{}",
            tex.upload_count, tex.distinct_address_count,
            tex.identical_repeat_count, tex.changed_repeat_count);
        logger.info(
            "Glide texture census decode-failures/last-failed-format/"
            "extent-mismatch/palettized-without-palette/bytes: {}/{}/{}/{}/{}",
            tex.decode_failure_count, tex.last_failed_format,
            tex.extent_mismatch_count, tex.palettized_without_palette_count,
            tex.decoded_byte_total);
        logger.info(
            "Glide texture census dump written/limited: {}/{}",
            tex.dump_written_count,
            tex.dump_limit_reached ? "true" : "false");
        // Issue #37: the full-precision texture path, by outcome.
        logger.info(
            "Glide texture full precision used/not-applicable/unreadable/"
            "link-mismatch/data-mismatch/verify-mismatch: {}/{}/{}/{}/{}/{}",
            tex.full_precision_counts[0], tex.full_precision_counts[1],
            tex.full_precision_counts[2], tex.full_precision_counts[3],
            tex.full_precision_counts[4], tex.full_precision_counts[5]);
        logger.info(
            "Glide palette downloads/changed/identical: {}/{}/{}",
            tex.palette_download_count, tex.palette_changed_count,
            tex.palette_identical_count);
        logger.info(
            "Glide palette lazy refreshes/failures/source-bytes/"
            "rgba-bytes: {}/{}/{}/{}",
            tex.palette_refresh_count, tex.palette_refresh_failure_count,
            tex.palette_refresh_source_bytes,
            tex.palette_refresh_rgba_bytes);
        logger.info(
            "Glide palette lazy refresh decode/upload ms: {:.3f}/{:.3f}",
            static_cast<double>(tex.palette_refresh_decode_nanoseconds) /
                1000000.0,
            static_cast<double>(tex.palette_refresh_upload_nanoseconds) /
                1000000.0);
        for (std::uint32_t index = 0;
             index < repiu::engine::kGlideTextureFormatBuckets;
             ++index)
        {
            if (tex.format_counts[index] != 0U)
            {
                logger.info("Glide texture census format {}: {}",
                            index, tex.format_counts[index]);
            }
        }
        for (std::uint32_t index = 0;
             index < repiu::engine::kGlideTextureDimensionBuckets;
             ++index)
        {
            if (tex.dimension_counts[index] != 0U)
            {
                logger.info(
                    "Glide texture census longer-edge {}: {}",
                    1U << index, tex.dimension_counts[index]);
            }
        }
    }
    logger.info("Glide texture gate trace count/wrapped: {}/{}",
                attempt.glide_texture_gate_trace_count,
                attempt.glide_texture_gate_trace_wrapped ? "true" : "false");
    for (const auto& entry : attempt.glide_texture_gate_trace)
    {
        if (!entry.valid)
        {
            continue;
        }
        logger.info("Glide texture gate trace #{} kind={} ordinal={} entry-eip={} entry-esp={} return-address={} tmu={} entry-eax={} return-eax={} planned-return-esp={}",
                    entry.sequence,
                    entry.is_max_address ? "max" : "min",
                    entry.ordinal,
                    Hex32(entry.entry_eip),
                    Hex32(entry.entry_esp),
                    Hex32(entry.return_address),
                    entry.tmu,
                    Hex32(entry.entry_eax),
                    Hex32(entry.return_eax),
                    Hex32(entry.planned_return_esp));
    }
    if (attempt.glide_first_triangle.valid)
    {
        for (std::size_t index = 0; index < 3U; ++index)
        {
            std::ostringstream vertex;
            for (std::uint32_t dword : attempt.glide_first_triangle.dwords[index])
            {
                if (!vertex.str().empty())
                {
                    vertex << ' ';
                }
                vertex << Hex32(dword);
            }
            logger.info("Glide first triangle vertex {} pointer/readable/dwords: {}/{}/{}",
                        index,
                        Hex32(attempt.glide_first_triangle.pointers[index]),
                        attempt.glide_first_triangle.pointer_readable[index] ? "true" : "false",
                        vertex.str());
        }
    }
    for (const auto& call : attempt.glide_calls)    {
        logger.info("Glide call trace: ordinal={} name={} count={} first_stack={} {} {} {} {} {} {} {}",
                    call.ordinal,
                    call.name,
                    call.count,
                    Hex32(call.first_stack[0]),
                    Hex32(call.first_stack[1]),
                    Hex32(call.first_stack[2]),
                    Hex32(call.first_stack[3]),
                    Hex32(call.first_stack[4]),
                    Hex32(call.first_stack[5]),
                    Hex32(call.first_stack[6]),
                    Hex32(call.first_stack[7]));
    }
    for (const auto& observation : attempt.glide_ordinal_timings)
    {
        const auto& timing = observation.timing;
        logger.info(
            "Glide ordinal timing: ordinal={} name={} count={} "
            "gate={} max={} rendezvous={} queue={} wake={} work={} "
            "complete={} residual={} backend_total={} direct={} "
            "direct_work={}",
            observation.ordinal, observation.name, timing.count,
            timing.gate_cycles, timing.max_gate_cycles,
            timing.rendezvous_count, timing.queue_cycles,
            timing.wake_cycles, timing.work_cycles,
            timing.complete_cycles, timing.residual_cycles,
            timing.backend_total_cycles, timing.direct_count,
            timing.direct_work_cycles);
    }
    for (const auto& observation : attempt.glide_setter_censuses)
    {
        const auto& census = observation.census;
        logger.info(
            "Glide setter census: ordinal={} name={} calls={} "
            "first={} same={} changed={} failure={} unsupported={} "
            "key_overflow={} distinct={} distinct_overflow={} "
            "max_run={} max_frame_calls={} max_frame_changes={} "
            "elided={} applied={}",
            observation.ordinal, observation.name, census.call_count,
            census.first_count, census.same_count, census.changed_count,
            census.failure_count, census.unsupported_count,
            census.key_overflow_count, census.distinct_key_count,
            census.distinct_overflow_count, census.max_repeat_run,
            census.max_frame_call_count, census.max_frame_change_count,
            observation.elided_count, observation.applied_count);
    }
    logger.info("MSCDEX available/audio/tracks/requests/current LBA: {}/{}/{}/{}/{}",
                attempt.mscdex_available ? "true" : "false",
                attempt.cd_audio_available ? "true" : "false",
                attempt.mscdex_track_count,
                attempt.mscdex_request_count,
                attempt.cd_audio_current_lba);
    // Task 421: a regression is a music position that moved backwards while
    // playing, which the game reads as the song jumping.
    logger.info(
        "CD audio position census entries/regressions: {}/{}",
        attempt.cd_audio_position_dump_entry_count,
        attempt.cd_audio_position_regression_count);
    logger.info(
        "MSCDEX command trace entries/commands: {}/{}",
        attempt.mscdex_command_trace_entry_count,
        attempt.mscdex_command_trace_total);
    logger.info("MSCDEX request ES/resolve kind/declines/reason/header: {}/{}/{}/{}/{}",
                Hex32(attempt.mscdex_frame_es),
                attempt.mscdex_last_resolve_kind,
                attempt.mscdex_decline_count,
                attempt.mscdex_last_decline_reason,
                Hex32(attempt.mscdex_last_header_bytes));
    logger.info("MSCDEX IOCTL last subfunction/handled/declared length/reject mask: {}/{}/{}/{}",
                Hex32(attempt.mscdex_last_ioctl_subfunction),
                attempt.mscdex_last_ioctl_handled ? "true" : "false",
                attempt.mscdex_last_ioctl_length,
                Hex32(attempt.mscdex_ioctl_reject_mask));
    logger.info("MSCDEX last play mode/start/length/seek target: {}/{}/{}/{}",
                attempt.mscdex_last_play_mode,
                attempt.mscdex_last_play_start,
                attempt.mscdex_last_play_length,
                attempt.mscdex_last_seek_target);
    logger.info("LINEXE scan return EAX/EBP/caller EAX: {}/{}/{}",
                Hex32(attempt.linexe_scan_return_eax),
                Hex32(attempt.linexe_scan_return_ebp),
                Hex32(attempt.linexe_scan_caller_eax));
    logger.info("LINEXE selector init results: {} {} {}",
                Hex32(attempt.linexe_selector_init_results[0]),
                Hex32(attempt.linexe_selector_init_results[1]),
                Hex32(attempt.linexe_selector_init_results[2]));
    logger.info("DPMI selector allocations count/request/result: {}/{}/{}",
                attempt.dpmi_allocate_call_count,
                attempt.dpmi_last_allocate_requested_count,
                Hex32(attempt.dpmi_last_allocated_selector));
    logger.info("LINEXE root selector EAX/GS: {}/{}",
                Hex32(attempt.linexe_root_selector_eax),
                Hex32(attempt.linexe_root_read_gs));
    logger.info("LINEXE shared segment loads entry/read: {}/{}",
                attempt.linexe_shared_load_entry_count,
                attempt.linexe_shared_load_read_count);
    logger.info("LINEXE shared segment last selector/offset/value: {}/{}/{}",
                Hex32(attempt.linexe_shared_load_selector),
                Hex32(attempt.linexe_shared_load_offset),
                Hex32(attempt.linexe_shared_load_value));
    logger.info("LINEXE root word loads offset/selector: {}({})/{}({})",
                Hex32(attempt.linexe_root_offset_load_value),
                attempt.linexe_root_offset_load_success,
                Hex32(attempt.linexe_root_selector_load_value),
                attempt.linexe_root_selector_load_success);
    for (std::uint32_t index = 0; index < 8; ++index)
    {
        logger.info("LINEXE export slot #{}: {}",
                    index,
                    Hex32(attempt.linexe_resolved_exports[index]));
    }
    logger.info("LINEXE selector words: {} {} {} {}",
                Hex32(attempt.linexe_selector_words[0]),
                Hex32(attempt.linexe_selector_words[1]),
                Hex32(attempt.linexe_selector_words[2]),
                Hex32(attempt.linexe_selector_words[3]));
    logger.info("DOS low memory valid: {}",
                attempt.dos_low_memory_valid ? "true" : "false");
    logger.info("DOS low memory bytes: {}",
                attempt.dos_low_memory_size);
    logger.info("last single-step context captured: {}",
                attempt.last_single_step_snapshot.captured ? "true"
                                                           : "false");
    if (attempt.last_single_step_snapshot.captured)
    {
        const auto& snapshot = attempt.last_single_step_snapshot;
        logger.info("last single-step EIP: {}", Hex32(snapshot.eip));
        logger.info("last single-step EAX: {}", Hex32(snapshot.eax));
        logger.info("last single-step EBX: {}", Hex32(snapshot.ebx));
        logger.info("last single-step ECX: {}", Hex32(snapshot.ecx));
        logger.info("last single-step EDX: {}", Hex32(snapshot.edx));
        logger.info("last single-step ESI: {}", Hex32(snapshot.esi));
        logger.info("last single-step EDI: {}", Hex32(snapshot.edi));
        logger.info("last single-step ESP: {}", Hex32(snapshot.esp));
        logger.info("last single-step EBP: {}", Hex32(snapshot.ebp));
        logger.info("last single-step EFLAGS: {}",
                    Hex32(snapshot.eflags));
        logger.info("last single-step CS: {}", Hex16(snapshot.cs));
        logger.info("last single-step DS: {}", Hex16(snapshot.ds));
        logger.info("last single-step ES: {}", Hex16(snapshot.es));
        logger.info("last single-step SS: {}", Hex16(snapshot.ss));
        logger.info("last single-step FS: {}", Hex16(snapshot.fs));
        logger.info("last single-step GS: {}", Hex16(snapshot.gs));
    }
    logger.info("DOS environment block bytes: {}",
                attempt.dos_environment_block_size);
    logger.info("DOS environment access observed: {}",
                attempt.last_dos_environment_access_valid ? "true" :
                                                            "false");
    if (attempt.last_dos_environment_access_valid)
    {
        logger.info("last DOS environment read offset: {}",
                    Hex32(attempt.last_dos_environment_access_offset));
        logger.info("last DOS environment entry offset: {}",
                    Hex32(attempt.last_dos_environment_entry_offset));
        logger.info("last DOS environment entry: {}=<redacted>",
                    attempt.last_dos_environment_entry_name);
        logger.info("last DOS environment value bytes: {}",
                    attempt.last_dos_environment_value_length);
    }
    logger.info("guest stack switch supported: {}",
                attempt.guest_stack_switch_supported ? "true" : "false");
    logger.info("guest stack switch attempted: {}",
                attempt.guest_stack_switch_attempted ? "true" : "false");
    if (attempt.guest_stack_switch_attempted)
    {
        logger.info("guest stack initial ESP: {}",
                    Hex32(attempt.guest_stack_initial_esp));
        if (attempt.guest_stack_return_esp != 0)
        {
            logger.info("guest stack return ESP: {}",
                        Hex32(attempt.guest_stack_return_esp));
        }
    }
    logger.info("handled HLE trap count: {}",
                attempt.handled_hle_trap_count);
    if (attempt.handled_hle_trap_count > 0)
    {
        logger.info("last handled HLE trap address: {}",
                    Hex32(attempt.last_hle_trap_address));
        logger.info("last handled HLE trap opcode: {}",
                    Hex8(static_cast<std::uint8_t>(
                        attempt.last_hle_trap_opcode & 0xFFU)));
    }
    logger.info("port I/O observation count: {}",
                attempt.port_io.observed_count);
    logger.info(
        "JAMMA scan cycles/scans/key-queries/cycles-per-scan/"
        "cycles-per-query: {}/{}/{}/{}/{}",
        attempt.port_io.jamma_scan_cycles,
        attempt.port_io.jamma_scan_count,
        attempt.port_io.key_query_count,
        attempt.port_io.jamma_scan_count != 0U
            ? attempt.port_io.jamma_scan_cycles / attempt.port_io.jamma_scan_count
            : 0U,
        attempt.port_io.key_query_count != 0U
            ? attempt.port_io.jamma_scan_cycles / attempt.port_io.key_query_count
            : 0U);
    logger.info(
        "JAMMA timeline edges/history-pruned/history-peak/"
        "history-overflow/history-coverage-miss/due/due-overflow/"
        "replays/replay-reads/missing-due/frames-retired/frame-overflow/"
        "active-depth: {}/{}/{}/{}/{}/{}/{}/{}/{}/{}/{}/{}/{}",
        attempt.port_io.jamma_timeline_edge_count,
        attempt.port_io.jamma_timeline_history_pruned_count,
        attempt.port_io.jamma_timeline_history_peak_size,
        attempt.port_io.jamma_timeline_history_overflow_count,
        attempt.port_io.jamma_timeline_history_coverage_miss_count,
        attempt.port_io.jamma_timeline_due_enqueued_count,
        attempt.port_io.jamma_timeline_due_overflow_count,
        attempt.port_io.jamma_timeline_replay_begin_count,
        attempt.port_io.jamma_timeline_replay_read_count,
        attempt.port_io.jamma_timeline_missing_due_count,
        attempt.port_io.jamma_timeline_frame_retire_count,
        attempt.port_io.jamma_timeline_frame_overflow_count,
        attempt.port_io.jamma_timeline_active_frame_depth);
    // Task 753: how replay frames left other than by the stack test.
    logger.info(
        "JAMMA timeline frames ended-by-return/retired-stale/"
        "latest-state-reads: {}/{}/{}",
        attempt.port_io.jamma_timeline_frame_end_count,
        attempt.port_io.jamma_timeline_frame_stale_count,
        attempt.port_io.jamma_timeline_latest_state_read_count);
    logger.info("port I/O input/output/handled/unhandled: {}/{}/{}/{}",
                attempt.port_io.input_count, attempt.port_io.output_count,
                attempt.port_io.handled_count, attempt.port_io.unhandled_count);
    if (attempt.port_io.observed_count > 0)
    {
        logger.info("last port I/O address: {}",
                    Hex32(attempt.port_io.last_address));
        logger.info("last port I/O opcode: {}",
                    Hex16(static_cast<std::uint16_t>(
                        attempt.port_io.last_opcode & 0xFFFFU)));
        logger.info("last port I/O direction: {}",
                    attempt.port_io.last_is_input ? "in" : "out");
        logger.info("last port I/O port: {}",
                    Hex16(static_cast<std::uint16_t>(
                        attempt.port_io.last_port & 0xFFFFU)));
        logger.info("last port I/O width: {}",
                    attempt.port_io.last_width);
        logger.info("last port I/O value: {}",
                    Hex32(attempt.port_io.last_value));
        logger.info("last port I/O handled: {}",
                    attempt.port_io.last_handled ? "true" : "false");
        logger.info("last port I/O result: {}",
                    attempt.port_io.last_result);
        logger.info("port I/O trace stored count: {}",
                    attempt.port_io.trace_stored_count);
        logger.info("port I/O trace limit reached: {}",
                    attempt.port_io.trace_limit_reached ? "true" : "false");
        for (std::uint32_t index = 0;
             index < attempt.port_io.trace_stored_count &&
             index < repiu::engine::kPortIoTraceCapacity;
             ++index)
        {
            const repiu::engine::PortIoTraceEntry& entry =
                attempt.port_io.trace[index];
            if (!entry.valid)
            {
                continue;
            }
            logger.info(
                "port I/O trace #{} address={} opcode={} direction={} port={} width={} value={} handled={}",
                entry.sequence,
                Hex32(entry.address),
                Hex16(static_cast<std::uint16_t>(entry.opcode & 0xFFFFU)),
                entry.is_input ? "in" : "out",
                Hex16(static_cast<std::uint16_t>(entry.port & 0xFFFFU)),
                entry.width,
                Hex32(entry.value),
                entry.handled ? "true" : "false");
        }
    }
    logger.info("DOS path trace stored count: {}",
                attempt.dos_path.trace_stored_count);
    logger.info("DOS path trace limit reached: {}",
                attempt.dos_path.trace_limit_reached ? "true" : "false");
    if (attempt.dos_path.trace_stored_count > 0)
    {
        const std::uint32_t first_sequence =
            attempt.dos_path.observed_count >
                    attempt.dos_path.trace_stored_count
                ? attempt.dos_path.observed_count -
                      attempt.dos_path.trace_stored_count + 1
                : 1;
        const std::uint32_t last_sequence =
            attempt.dos_path.observed_count;
        for (std::uint32_t sequence = first_sequence;
             sequence <= last_sequence;
             ++sequence)
        {
            const std::uint32_t slot =
                (sequence - 1) % repiu::engine::
                    kDosPathTraceCapacity;
            const repiu::engine::DosPathTraceEntry& entry =
                attempt.dos_path.trace[slot];
            if (!entry.valid || entry.sequence != sequence)
            {
                continue;
            }

            logger.info(
                "DOS path trace #{} service={} result={} error={} drive={} access={} guest={} virtual={} host={}",
                entry.sequence,
                entry.service,
                entry.result,
                Hex16(entry.dos_error),
                Hex8(entry.drive),
                Hex8(entry.access_mode),
                entry.guest_path,
                entry.virtual_path,
                entry.host_path);
        }
    }
    logger.info("DOS file I/O trace observed/stored: {}/{}",
                attempt.dos_file_io.observed_count,
                attempt.dos_file_io.trace_stored_count);
    logger.info("DOS file I/O trace wrapped: {}",
                attempt.dos_file_io.trace_wrapped ? "true" : "false");
    // Task 374: one host open per read is the defect this reports. A healthy run
    // opens roughly once per file and reads many times against it.
    logger.info("DOS file reads/host opens/reads per open: {}/{}/{:.2f}",
                attempt.dos_file_io.read_count,
                attempt.dos_file_io.host_open_count,
                attempt.dos_file_io.host_open_count != 0U
                    ? static_cast<double>(attempt.dos_file_io.read_count) /
                        static_cast<double>(attempt.dos_file_io.host_open_count)
                    : 0.0);
    if (attempt.dos_file_io.trace_stored_count != 0)
    {
        const std::uint32_t first_sequence =
            attempt.dos_file_io.observed_count -
            attempt.dos_file_io.trace_stored_count + 1U;
        for (std::uint32_t sequence = first_sequence;
             sequence <= attempt.dos_file_io.observed_count;
             ++sequence)
        {
            const std::uint32_t slot = (sequence - 1U) %
                repiu::engine::kDosFileIoTraceCapacity;
            const auto& entry = attempt.dos_file_io.trace[slot];
            if (!entry.valid || entry.sequence != sequence)
            {
                continue;
            }
            std::ostringstream prefix;
            std::ostringstream guest_stack;
            for (std::uint32_t index = 0;
                 index < entry.prefix_size;
                 ++index)
            {
                if (index != 0)
                {
                    prefix << ' ';
                }
                prefix << std::uppercase << std::hex << std::setw(2)
                       << std::setfill('0')
                       << static_cast<unsigned>(entry.prefix[index]);
            }
            for (const std::uint32_t value : entry.guest_stack)
            {
                if (guest_stack.tellp() != std::streampos(0))
                {
                    guest_stack << ' ';
                }
                guest_stack << Hex32(value);
            }
            logger.info(
                "DOS file I/O #{} op={} handle={} before={} after={} "
                "origin={} offset={} requested={} actual={} error={} "
                "eip={} esp={} stack=[{}] prefix=[{}] path={}",
                entry.sequence,
                entry.operation,
                Hex16(entry.handle),
                Hex32(entry.position_before),
                Hex32(entry.position_after),
                Hex8(entry.origin),
                entry.seek_offset,
                entry.requested_bytes,
                entry.actual_bytes,
                Hex16(entry.dos_error),
                Hex32(entry.guest_eip),
                Hex32(entry.guest_esp),
                guest_stack.str(),
                prefix.str(),
                entry.host_path);
        }
    }
    logger.info("allocator probe observation count: {}",
                attempt.allocator_probe.observed_count);
    logger.info("allocator probe trace stored count: {}",
                attempt.allocator_probe.trace_stored_count);
    logger.info("allocator probe trace wrapped: {}",
                attempt.allocator_probe.trace_wrapped ? "true" : "false");
    if (attempt.allocator_probe.trace_stored_count > 0)
    {
        const std::uint32_t first_sequence =
            attempt.allocator_probe.observed_count >
                    attempt.allocator_probe.trace_stored_count
                ? attempt.allocator_probe.observed_count -
                      attempt.allocator_probe.trace_stored_count + 1
                : 1;
        for (std::uint32_t sequence = first_sequence;
             sequence <= attempt.allocator_probe.observed_count;
             ++sequence)
        {
            const std::uint32_t slot =
                (sequence - 1) % repiu::engine::
                    kAllocatorProbeTraceCapacity;
            const auto& entry = attempt.allocator_probe.trace[slot];
            if (!entry.valid || entry.sequence != sequence)
            {
                continue;
            }
            logger.info(
                "allocator probe trace #{} EAX={} ESI={} source={} DS={} pending-before={} size-before={} pending-after={} size-after={} result={}",
                entry.sequence,
                Hex32(entry.eax),
                Hex32(entry.esi),
                Hex32(entry.source),
                Hex16(entry.ds),
                entry.pending_before ? "true" : "false",
                Hex32(entry.pending_size_before),
                entry.pending_after ? "true" : "false",
                Hex32(entry.pending_size_after),
                entry.result);
        }
    }
    logger.info("allocator control-flow observation count: {}",
                attempt.allocator_control_flow.observed_count);
    logger.info("allocator control-flow trace stored count: {}",
                attempt.allocator_control_flow.trace_stored_count);
    logger.info("allocator control-flow trace wrapped: {}",
                attempt.allocator_control_flow.trace_wrapped ? "true"
                                                             : "false");
    if (attempt.allocator_control_flow.trace_stored_count > 0)
    {
        const std::uint32_t first_sequence =
            attempt.allocator_control_flow.observed_count >
                    attempt.allocator_control_flow.trace_stored_count
                ? attempt.allocator_control_flow.observed_count -
                      attempt.allocator_control_flow.trace_stored_count + 1
                : 1;
        for (std::uint32_t sequence = first_sequence;
             sequence <= attempt.allocator_control_flow.observed_count;
             ++sequence)
        {
            const std::uint32_t slot =
                (sequence - 1) % repiu::engine::
                    kAllocatorControlFlowTraceCapacity;
            const auto& entry =
                attempt.allocator_control_flow.trace[slot];
            if (!entry.valid || entry.sequence != sequence)
            {
                continue;
            }
            logger.info(
                "allocator control-flow trace #{} offset={} exception={} bytes={:02X} {:02X} {:02X} {:02X} EAX={} EBX={} EDX={} ESI={} EDI={} EFLAGS={} pending={} size={} read={} address={} value={} explicit-shadow={} zero-backed={} writer={} writer-sequence={} writer-offset={} writer-opcode={} writer-destination={} writer-value={} writer-width={}",
                entry.sequence,
                Hex32(entry.eip_offset),
                Hex32(entry.seh_code),
                entry.opcode[0],
                entry.opcode[1],
                entry.opcode[2],
                entry.opcode[3],
                Hex32(entry.eax),
                Hex32(entry.ebx),
                Hex32(entry.edx),
                Hex32(entry.esi),
                Hex32(entry.edi),
                Hex32(entry.eflags),
                entry.pending_valid ? "true" : "false",
                Hex32(entry.pending_size),
                entry.read_valid ? "true" : "false",
                Hex32(entry.read_address),
                Hex32(entry.read_value),
                entry.read_explicit_shadow ? "true" : "false",
                entry.read_zero_backed ? "true" : "false",
                entry.writer_valid ? "true" : "false",
                entry.writer_sequence,
                Hex32(entry.writer_eip_offset),
                Hex32(entry.writer_opcode),
                Hex32(entry.writer_destination),
                Hex32(entry.writer_value),
                entry.writer_width);
        }
    }
    const auto log_allocator_link_transition = [&](const char* name,
                                                    bool valid,
                                                    const auto& entry) {
        logger.info("allocator {} link transition valid: {}",
                    name,
                    valid ? "true" : "false");
        if (!valid)
        {
            return;
        }
        logger.info(
            "allocator {} link transition sequence={} node={} address={} value={} explicit-shadow={} zero-backed={} writer={} writer-offset={} writer-opcode={} writer-destination={} writer-value={} writer-width={}",
            name,
            entry.sequence,
            Hex32(entry.esi),
            Hex32(entry.read_address),
            Hex32(entry.read_value),
            entry.read_explicit_shadow ? "true" : "false",
            entry.read_zero_backed ? "true" : "false",
            entry.writer_valid ? "true" : "false",
            Hex32(entry.writer_eip_offset),
            Hex32(entry.writer_opcode),
            Hex32(entry.writer_destination),
            Hex32(entry.writer_value),
            entry.writer_width);
    };
    log_allocator_link_transition(
        "null",
        attempt.allocator_control_flow.null_link_transition_valid,
        attempt.allocator_control_flow.null_link_transition);
    log_allocator_link_transition(
        "poison",
        attempt.allocator_control_flow.poison_link_transition_valid,
        attempt.allocator_control_flow.poison_link_transition);
    log_allocator_link_transition(
        "root-null",
        attempt.allocator_control_flow.root_transition_valid,
        attempt.allocator_control_flow.root_transition);
    logger.info("handled DOS interrupt count: {}",
                attempt.handled_dos_interrupt_count);
    repiu::engine::AotOpcodeRank dos_ah_ranks[4] = {};
    repiu::engine::RankAotOpcodeHistogram(
        attempt.handled_dos_interrupt_ah_counts, dos_ah_ranks, 4U);
    logger.info("DOS AH hotspots [{:02X}:{} {:02X}:{} {:02X}:{} {:02X}:{}]",
                dos_ah_ranks[0].opcode, dos_ah_ranks[0].count,
                dos_ah_ranks[1].opcode, dos_ah_ranks[1].count,
                dos_ah_ranks[2].opcode, dos_ah_ranks[2].count,
                dos_ah_ranks[3].opcode, dos_ah_ranks[3].count);
    if (attempt.handled_dos_interrupt_count > 0)
    {
        logger.info("last handled DOS interrupt vector: {}",
                    Hex8(static_cast<std::uint8_t>(
                        attempt.last_dos_interrupt_vector & 0xFFU)));
        logger.info("last handled DOS interrupt AH: {}",
                    Hex8(static_cast<std::uint8_t>(
                        attempt.last_dos_interrupt_ah & 0xFFU)));
        logger.info("last handled DOS interrupt AX: {}",
                    Hex16(static_cast<std::uint16_t>(
                        attempt.last_dos_interrupt_ax & 0xFFFFU)));
    }
    logger.info("handled DOS chdir count: {}",
                attempt.handled_dos_chdir_count);
    if (attempt.handled_dos_chdir_count > 0)
    {
        logger.info("last DOS chdir guest path: {}",
                    attempt.last_dos_chdir_guest_path);
        logger.info("last DOS chdir virtual path: {}",
                    attempt.last_dos_chdir_virtual_path);
        logger.info("last DOS chdir host path: {}",
                    attempt.last_dos_chdir_host_path);
        logger.info("last DOS chdir result: {}",
                    attempt.last_dos_chdir_success ? "success" : "failure");
        if (!attempt.last_dos_chdir_success)
        {
            logger.info("last DOS chdir error: {}",
                        Hex16(attempt.last_dos_chdir_error));
        }
    }
    logger.info("handled DOS getcwd count: {}",
                attempt.handled_dos_getcwd_count);
    if (attempt.handled_dos_getcwd_count > 0)
    {
        logger.info("last DOS getcwd drive: {}",
                    Hex8(attempt.last_dos_getcwd_drive));
        logger.info("last DOS getcwd path: {}",
                    attempt.last_dos_getcwd_path);
        logger.info("last DOS getcwd result: {}",
                    attempt.last_dos_getcwd_success
                        ? "success"
                        : "failure");
        if (!attempt.last_dos_getcwd_success)
        {
            logger.info("last DOS getcwd error: {}",
                        Hex16(attempt.last_dos_getcwd_error));
        }
    }
    logger.info("handled DOS get drive count: {}",
                attempt.handled_dos_getdrive_count);
    if (attempt.handled_dos_getdrive_count > 0)
    {
        logger.info("last DOS get drive value: {}",
                    Hex8(attempt.last_dos_getdrive_value));
    }
    logger.info("handled DOS open count: {}",
                attempt.handled_dos_open_count);
    if (attempt.handled_dos_open_count > 0)
    {
        logger.info("last DOS open guest path: {}",
                    attempt.last_dos_open_guest_path);
        logger.info("last DOS open virtual path: {}",
                    attempt.last_dos_open_virtual_path);
        logger.info("last DOS open host path: {}",
                    attempt.last_dos_open_host_path);
        logger.info("last DOS open access mode: {}",
                    Hex8(attempt.last_dos_open_access_mode));
        logger.info("last DOS open result: {}",
                    attempt.last_dos_open_success ? "success" : "failure");
        if (attempt.last_dos_open_success)
        {
            logger.info("last DOS open handle: {}",
                        Hex16(attempt.last_dos_open_handle));
        }
        else
        {
            logger.info("last DOS open error: {}",
                        Hex16(attempt.last_dos_open_error));
        }
    }
    logger.info("handled DOS read count: {}",
                attempt.handled_dos_read_count);
    if (attempt.handled_dos_read_count > 0)
    {
        logger.info("last DOS read handle: {}",
                    Hex16(attempt.last_dos_read_handle));
        logger.info("last DOS read requested bytes: {}",
                    attempt.last_dos_read_requested_bytes);
        logger.info("last DOS read actual bytes: {}",
                    attempt.last_dos_read_actual_bytes);
        logger.info("last DOS read buffer: {}",
                    Hex32(attempt.last_dos_read_buffer));
        logger.info("last DOS read result: {}",
                    attempt.last_dos_read_success ? "success" : "failure");
        if (!attempt.last_dos_read_success)
        {
            logger.info("last DOS read error: {}",
                        Hex16(attempt.last_dos_read_error));
        }
    }
    logger.info("handled DOS create count: {}",
                attempt.handled_dos_create_count);
    if (attempt.handled_dos_create_count > 0)
    {
        logger.info("last DOS create guest path: {}",
                    attempt.last_dos_create_guest_path);
        logger.info("last DOS create host path: {}",
                    attempt.last_dos_create_host_path);
        logger.info("last DOS create virtual path: {}",
                    attempt.last_dos_create_virtual_path);
        logger.info("last DOS create handle: {}",
                    Hex16(attempt.last_dos_create_handle));
        logger.info("last DOS create attributes: {}",
                    Hex16(attempt.last_dos_create_attributes));
        logger.info("last DOS create result: {}",
                    attempt.last_dos_create_success ? "success" : "failure");
        if (!attempt.last_dos_create_success)
        {
            logger.info("last DOS create error: {}",
                        Hex16(attempt.last_dos_create_error));
        }
    }
    logger.info("handled DOS write count: {}",
                attempt.handled_dos_write_count);
    if (attempt.handled_dos_write_count > 0)
    {
        logger.info("last DOS write handle: {}",
                    Hex16(attempt.last_dos_write_handle));
        logger.info("last DOS write requested bytes: {}",
                    attempt.last_dos_write_requested_bytes);
        logger.info("last DOS write actual bytes: {}",
                    attempt.last_dos_write_actual_bytes);
        logger.info("last DOS write buffer: {}",
                    Hex32(attempt.last_dos_write_buffer));
        logger.info("last DOS write result: {}",
                    attempt.last_dos_write_success ? "success" : "failure");
        if (!attempt.last_dos_write_success)
        {
            logger.info("last DOS write error: {}",
                        Hex16(attempt.last_dos_write_error));
        }
    }
    logger.info("handled DOS seek count: {}",
                attempt.handled_dos_seek_count);
    if (attempt.handled_dos_seek_count > 0)
    {
        logger.info("last DOS seek handle: {}",
                    Hex16(attempt.last_dos_seek_handle));
        logger.info("last DOS seek origin: {}",
                    Hex8(attempt.last_dos_seek_origin));
        logger.info("last DOS seek offset: {}",
                    attempt.last_dos_seek_offset);
        logger.info("last DOS seek position: {}",
                    attempt.last_dos_seek_position);
        logger.info("last DOS seek result: {}",
                    attempt.last_dos_seek_success ? "success" : "failure");
        if (!attempt.last_dos_seek_success)
        {
            logger.info("last DOS seek error: {}",
                        Hex16(attempt.last_dos_seek_error));
        }
    }
    logger.info("handled DOS close count: {}",
                attempt.handled_dos_close_count);
    logger.info("DOS console input chars/waits: {}/{}",
                attempt.dos_console_input_count,
                attempt.dos_console_input_wait_count);
    if (attempt.handled_dos_close_count > 0)
    {
        logger.info("last DOS close handle: {}",
                    Hex16(attempt.last_dos_close_handle));
        logger.info("last DOS close result: {}",
                    attempt.last_dos_close_success ? "success" : "failure");
        if (!attempt.last_dos_close_success)
        {
            logger.info("last DOS close error: {}",
                        Hex16(attempt.last_dos_close_error));
        }
    }
    logger.info("handled DOS IOCTL count: {}",
                attempt.handled_dos_ioctl_count);
    if (attempt.handled_dos_ioctl_count > 0)
    {
        logger.info("last DOS IOCTL subfunction: {}",
                    Hex8(attempt.last_dos_ioctl_subfunction));
        logger.info("last DOS IOCTL handle: {}",
                    Hex16(attempt.last_dos_ioctl_handle));
        logger.info("last DOS IOCTL result: {}",
                    attempt.last_dos_ioctl_success ? "success" : "failure");
        if (attempt.last_dos_ioctl_success)
        {
            logger.info("last DOS IOCTL device info: {}",
                        Hex16(attempt.last_dos_ioctl_device_info));
        }
        else
        {
            logger.info("last DOS IOCTL error: {}",
                        Hex16(attempt.last_dos_ioctl_error));
        }
    }
    logger.info("handled DOS resize count: {}",
                attempt.handled_dos_resize_count);
    if (attempt.handled_dos_resize_count > 0)
    {
        logger.info("last DOS resize selector: {}",
                    Hex16(attempt.last_dos_resize_selector));
        logger.info("last DOS resize paragraphs: {}",
                    Hex16(attempt.last_dos_resize_paragraphs));
        logger.info("last DOS resize result: {}",
                    attempt.last_dos_resize_success ? "success" : "failure");
        logger.info("last DOS resize requested end: {}",
                    Hex32(attempt.last_dos_resize_requested_end));
        logger.info("last DOS resize allocator end: {}",
                    Hex32(attempt.last_dos_resize_allocator_end));
        if (!attempt.last_dos_resize_success)
        {
            logger.info("last DOS resize error: {}",
                        Hex16(attempt.last_dos_resize_error));
        }
    }
    logger.info("handled segment load count: {}",
                attempt.handled_segment_load_count);
    if (attempt.handled_segment_load_count > 0)
    {
        logger.info("last handled segment load address: {}",
                    Hex32(attempt.last_segment_load_address));
        logger.info("last handled segment load opcode: {}",
                    Hex8(static_cast<std::uint8_t>(
                        attempt.last_segment_load_opcode & 0xFFU)));
        logger.info("last handled segment register: {}",
                    SegmentRegisterName(
                        attempt.last_segment_load_register));
        logger.info("last handled segment selector: {}",
                    Hex16(static_cast<std::uint16_t>(
                        attempt.last_segment_load_selector & 0xFFFFU)));
        if (attempt.last_segment_load_source != 0)
        {
        logger.info("last segment load source: {}",
                    Hex32(attempt.last_segment_load_source));
    }
    logger.info("segment load trace stored count: {}",
                attempt.segment_load.trace_stored_count);
    logger.info("segment load trace wrapped: {}",
                attempt.segment_load.trace_wrapped ? "true" : "false");
    if (attempt.segment_load.trace_stored_count > 0)
    {
        const std::uint32_t first_sequence =
            attempt.segment_load.observed_count >
                    attempt.segment_load.trace_stored_count
                ? attempt.segment_load.observed_count -
                      attempt.segment_load.trace_stored_count + 1
                : 1;
        for (std::uint32_t sequence = first_sequence;
             sequence <= attempt.segment_load.observed_count;
             ++sequence)
        {
            const std::uint32_t slot =
                (sequence - 1) % repiu::engine::
                    kSegmentLoadTraceCapacity;
            const auto& entry = attempt.segment_load.trace[slot];
            if (!entry.valid || entry.sequence != sequence)
            {
                continue;
            }
            logger.info(
                "segment load trace #{} offset={} register={} selector={} source={}",
                entry.sequence,
                Hex32(entry.eip_offset),
                SegmentRegisterName(entry.segment_register),
                Hex16(entry.selector),
                Hex32(entry.source));
        }
    }
    }
    logger.info("handled segment store count: {}",
                attempt.handled_segment_store_count);
    if (attempt.handled_segment_store_count > 0)
    {
        logger.info("last handled segment store address: {}",
                    Hex32(attempt.last_segment_store_address));
        logger.info("last handled segment store opcode: {}",
                    Hex8(static_cast<std::uint8_t>(
                        attempt.last_segment_store_opcode & 0xFFU)));
        logger.info("last stored segment register: {}",
                    SegmentRegisterName(
                        attempt.last_segment_store_register));
        logger.info("last stored segment selector: {}",
                    Hex16(static_cast<std::uint16_t>(
                        attempt.last_segment_store_selector & 0xFFFFU)));
        logger.info("last segment store destination: {}",
                    Hex32(attempt.last_segment_store_destination));
    }
    logger.info("handled segment memory load count: {}",
                attempt.handled_segment_memory_load_count);
    if (attempt.handled_segment_memory_load_count > 0)
    {
        logger.info("last handled segment memory load address: {}",
                    Hex32(attempt.last_segment_memory_load_address));
        logger.info("last handled segment memory load opcode: {}",
                    Hex8(static_cast<std::uint8_t>(
                        attempt.last_segment_memory_load_opcode & 0xFFU)));
        logger.info("last segment memory load register: {}",
                    SegmentRegisterName(
                        attempt.last_segment_memory_load_register));
        logger.info("last segment memory load selector: {}",
                    Hex16(static_cast<std::uint16_t>(
                        attempt.last_segment_memory_load_selector &
                        0xFFFFU)));
        logger.info("last segment memory load offset: {}",
                    Hex32(attempt.last_segment_memory_load_offset));
        logger.info("last segment memory load width: {}",
                    attempt.last_segment_memory_load_width);
        if (attempt.last_segment_memory_load_width == 4)
        {
            logger.info("last segment memory load value: {}",
                        Hex32(attempt.last_segment_memory_load_value));
        }
        else
        {
            logger.info("last segment memory load value: {}",
                        Hex8(static_cast<std::uint8_t>(
                            attempt.last_segment_memory_load_value & 0xFFU)));
        }
    }
    logger.info("handled low-memory access count: {}",
                attempt.handled_low_memory_access_count);
    if (attempt.handled_low_memory_access_count > 0)
    {
        logger.info("last low-memory access address: {}",
                    Hex32(attempt.last_low_memory_access_address));
        logger.info("last low-memory access opcode: {}",
                    Hex8(static_cast<std::uint8_t>(
                        attempt.last_low_memory_access_opcode & 0xFFU)));
        logger.info("last low-memory access ESI: {}",
                    Hex32(attempt.last_low_memory_access_esi));
        logger.info("last low-memory access EDI: {}",
                    Hex32(attempt.last_low_memory_access_edi));
        logger.info("last low-memory access destination: {}",
                    Hex32(attempt.last_low_memory_access_destination));
        logger.info("last low-memory access value: {}",
                    Hex32(attempt.last_low_memory_access_value));
    }
    logger.info("low-memory read emulate count: {}",
                attempt.low_memory_read_emulate_count);
    logger.info("low-memory read emulate debug stage: {}",
                attempt.debug_emulate_stage);
    if (attempt.debug_emulate_stage > 0)
    {
        logger.info("low-memory read emulate debug decode result: {}",
                    Hex32(attempt.debug_emulate_decode_result));
        logger.info("low-memory read emulate debug calculated address: {}",
                    Hex32(attempt.debug_emulate_calculated_address));
    }
    if (attempt.low_memory_read_emulate_count > 0)
    {
        logger.info("last low-memory read emulate address: {}",
                    Hex32(attempt.last_low_memory_read_emulate_address));
        logger.info("last low-memory read emulate EIP: {}",
                    Hex32(attempt.last_low_memory_read_emulate_eip));
        logger.info("last low-memory read emulate value: {}",
                    Hex32(attempt.last_low_memory_read_emulate_value));
        logger.info("last low-memory read emulate reg: {}",
                    static_cast<unsigned>(attempt.last_low_memory_read_emulate_reg));
    }
    logger.info("low-memory string service/iteration count: {}/{}",
                attempt.low_memory_string_service_count,
                attempt.low_memory_string_iteration_count);
    if (attempt.low_memory_string_service_count > 0)
    {
        logger.info(
            "last low-memory string EIP/address/mnemonic/iterations: "
            "{}/{}/{}/{}",
            Hex32(attempt.last_low_memory_string_eip),
            Hex32(attempt.last_low_memory_string_address),
            attempt.last_low_memory_string_mnemonic,
            attempt.last_low_memory_string_iterations);
    }
    logger.info("handled memory store count: {}",
                attempt.handled_memory_store_count);
    if (attempt.handled_memory_store_count > 0)
    {
        logger.info("last handled memory store address: {}",
                    Hex32(attempt.last_memory_store_address));
        if (attempt.last_memory_store_opcode > 0xFFU)
        {
            logger.info("last memory store opcode: {}",
                        Hex16(static_cast<std::uint16_t>(
                            attempt.last_memory_store_opcode & 0xFFFFU)));
        }
        else
        {
            logger.info("last memory store opcode: {}",
                        Hex8(static_cast<std::uint8_t>(
                            attempt.last_memory_store_opcode & 0xFFU)));
        }
        logger.info("last memory store width: {}",
                    attempt.last_memory_store_width);
        logger.info("last memory store source kind: {}",
                    attempt.last_memory_store_source_kind);
        logger.info("last memory store destination: {}",
                    Hex32(attempt.last_memory_store_destination));
        logger.info("last memory store value: {}",
                    Hex32(attempt.last_memory_store_value));
        logger.info("last memory store applied: {}",
                    attempt.last_memory_store_applied ? "true" : "false");
    }
    logger.info("shadow memory write count: {}",
                attempt.shadow_memory_write_count);
    logger.info("REP MOVS safe-copy failure count: {}",
                attempt.rep_movs_copy_failure_count);
    if (attempt.rep_movs_copy_failure_count != 0)
    {
        logger.info("last REP MOVS failure stage/error: {}/{}",
                    attempt.last_rep_movs_copy_failure_stage,
                    attempt.last_rep_movs_copy_error);
        logger.info("last REP MOVS source/destination/bytes: {}/{}/{}",
                    Hex32(attempt.last_rep_movs_copy_source),
                    Hex32(attempt.last_rep_movs_copy_destination),
                    attempt.last_rep_movs_copy_bytes);
    }
    logger.info("shadow memory read hit count: {}",
                attempt.shadow_memory_read_hit_count);
    logger.info("shadow memory byte count: {}",
                attempt.shadow_memory_byte_count);
    logger.info("shadow memory range valid: {}",
                attempt.shadow_memory_range_valid ? "true" : "false");
    if (attempt.shadow_memory_range_valid)
    {
        logger.info("shadow memory min address: {}",
                    Hex32(attempt.shadow_memory_min_address));
        logger.info("shadow memory max address: {}",
                    Hex32(attempt.shadow_memory_max_address));
    }
    logger.info("minimal execution thread exit code: {}",
                attempt.thread_exit_code);
    logger.info("DOS termination captured: {}",
                attempt.dos_termination_captured ? "true" : "false");
    if (attempt.dos_termination_captured)
    {
        logger.info("DOS termination AX/EIP/ESP: {}/{}/{}",
                    Hex16(static_cast<std::uint16_t>(
                        attempt.dos_termination_ax)),
                    Hex32(attempt.dos_termination_eip),
                    Hex32(attempt.dos_termination_esp));
        std::ostringstream stack;
        for (std::uint32_t index = 0;
             index < repiu::engine::
                 kDosTerminationStackCapacity;
             ++index)
        {
            if (index != 0)
            {
                stack << ' ';
            }
            stack << Hex32(attempt.dos_termination_stack[index]);
        }
        logger.info("DOS termination stack: {}", stack.str());
    }
    if (!attempt.hle_stdout_output.empty() ||
        !attempt.hle_stderr_output.empty())
    {
        logger.info("HLE stdout/stderr bytes: {}/{}",
                    attempt.hle_stdout_output.size(),
                    attempt.hle_stderr_output.size());
        std::shared_ptr<spdlog::logger> guest_logger =
            spdlog::get(std::string(executable_name));
        if (guest_logger == nullptr)
        {
            guest_logger = spdlog::stderr_color_mt(
                std::string(executable_name));
            guest_logger->set_pattern("[%X.%e] [%8l] [%n] %v");
            guest_logger->set_level(spdlog::level::info);
        }
        LogGuestOutput(*guest_logger,
                       spdlog::level::info,
                       attempt.hle_stdout_output);
        LogGuestOutput(*guest_logger,
                       spdlog::level::err,
                       attempt.hle_stderr_output);
    }
    if (attempt.exception_caught)
    {
        logger.error("minimal execution message: {}", attempt.message);
    }
    else
    {
        logger.info("minimal execution message: {}", attempt.message);
    }
}

bool SelectAndReserveRelocatedImageBase(
    std::uint32_t reserve_size,
    spdlog::logger& logger,
    repiu::engine::AddressRangeReservation* reservation)
{
    if (reservation == nullptr)
    {
        return false;
    }
    *reservation = repiu::engine::AddressRangeReservation{};

    const std::vector<std::uint32_t> candidates = {
        0x01000000,
        0x02000000,
        0x03000000,
        0x04000000,
        0x05000000,
        0x06000000,
        0x07000000,
        0x08000000,
        0x09000000,
    };

    for (std::uint32_t candidate : candidates)
    {
        repiu::engine::RuntimeMemoryPolicy policy;
        if (!repiu::engine::BuildRuntimeMemoryPolicyFromFixedRange(
                candidate, reserve_size, &policy))
        {
            continue;
        }

        repiu::engine::AddressRangeProbe probe;
        if (!repiu::engine::ProbeRuntimeAddressRange(
                policy, &probe))
        {
            continue;
        }

        logger.info("relocated base candidate {}: {}",
                    Hex32(candidate),
                    probe.range_available ? "available" : "occupied");
        if (!probe.range_available)
        {
            continue;
        }

        repiu::engine::AddressRangeReservation candidate_reservation;
        if (!repiu::engine::ReserveAndCommitRuntimeAddressRange(
                policy, &candidate_reservation))
        {
            continue;
        }

        if (!candidate_reservation.reserved ||
            candidate_reservation.reserved_base != candidate)
        {
            logger.warn("relocated base candidate {} reserve result: {}",
                        Hex32(candidate),
                        candidate_reservation.message);
            repiu::engine::ReleaseRuntimeAddressRange(
                candidate_reservation);
            continue;
        }

        logger.info("relocated base candidate {} reserved size: {}",
                    Hex32(candidate),
                    Hex32(candidate_reservation.reserved_size));
        *reservation = candidate_reservation;
        return true;
    }

    return false;
}

std::filesystem::path ExecutableDirectory(int argc, char** argv)
{
    if (argc < 1 || argv[0] == nullptr)
    {
        return std::filesystem::path();
    }
    std::error_code error;
    const std::filesystem::path absolute =
        std::filesystem::absolute(argv[0], error);
    if (error)
    {
        return std::filesystem::path(argv[0]).parent_path();
    }
    return absolute.parent_path();
}

// Resolves cfg/<rom set>.ini, layering it over any parent ROM set's file and
// the built-in defaults, then installs the result as the bindings for this
// run. Config problems are logged and never fatal: a typo in a key mapping
// must not stop the game from starting.
void LoadRomSetInputConfiguration(spdlog::logger& logger,
                                  std::string_view rom_set_id,
                                  const std::filesystem::path& exe_directory)
{
    repiu::config::RomSetConfigRequest request =
        repiu::config::MakeRomSetConfigRequestFromEnvironment(exe_directory);
    request.layer_ids = repiu::config::BuildRomSetLayerIds(
        rom_set_id,
        [](std::string_view id) -> std::string_view
        {
            const repiu::target::TargetProfile* entry =
                repiu::target::FindTargetProfileById(id);
            return entry != nullptr ? entry->parent_rom_set_id
                                    : std::string_view();
        });

    const repiu::config::RomSetConfigResult result =
        repiu::config::LoadRomSetConfig(request);

    logger.info("Config directory: {}", result.cfg_directory.string());
    if (result.applied_files.empty())
    {
        logger.info("Config files applied: none (built-in defaults)");
    }
    else
    {
        for (const std::filesystem::path& path : result.applied_files)
        {
            logger.info("Config applied: {}", path.string());
        }
    }
    if (result.generated_default_file)
    {
        logger.info("Config created: {}", result.generated_file.string());
    }
    for (const std::string& warning : result.warnings)
    {
        logger.warn("Config: {}", warning);
    }

    repiu::engine::SetActiveJammaBindings(result.bindings);
}

// Points the 93C46 EEPROM at nvram/<rom set>/eeprom.dat, creating the
// directory and carrying a pre-existing shared eeprom.dat forward the first
// time. Path problems are logged and never fatal; a missing EEPROM starts at
// 0xFFFF, which costs settings but not the run.
void ResolveRomSetNvramPaths(spdlog::logger& logger,
                             std::string_view rom_set_id,
                             const std::filesystem::path& exe_directory)
{
    const repiu::storage::EepromPathResult eeprom =
        repiu::storage::ResolveEepromPath(rom_set_id, exe_directory);

    logger.info("EEPROM image: {}{}", eeprom.path.string(),
                eeprom.from_override ? " (REPIU_EEPROM_PATH)" : "");
    if (eeprom.carried_legacy_file)
    {
        logger.info("EEPROM carried forward from the shared eeprom.dat");
    }
    for (const std::string& warning : eeprom.warnings)
    {
        logger.warn("EEPROM: {}", warning);
    }

    repiu::engine::SetEepromBackingPath(eeprom.path.string());
}

const repiu::target::TargetProfile* SelectTargetProfile(int argc,
                                                        char** argv)
{
    std::string_view target_id = "pumpit1";
    if (argc >= 2)
    {
        target_id = argv[1];
    }

    return repiu::target::FindTargetProfileById(target_id);
}

// Task 500. With no arguments the launcher chooses the ROM set; with any
// argument the existing selection path is used untouched, so every scripted
// invocation behaves exactly as before. REPIU_LAUNCHER is a fail-closed opt-out
// for automation that runs the binary bare.
bool LauncherRequested(int argc)
{
    if (argc >= 2)
    {
        return false;
    }
    return repiu::runtime::ResolvePromotedToggle(
        std::getenv("REPIU_LAUNCHER"));
}

// Publishes the launcher's stored options as the environment variables the
// existing consumers already read, without ever overwriting one the caller set.
void PublishLauncherSettings(
    const repiu::launcher::LauncherSettings& settings,
    const repiu::launcher::LauncherEnvironmentOverrides& overrides,
    const std::shared_ptr<spdlog::logger>& logger)
{
    const auto applied = repiu::launcher::ApplyLauncherSettings(
        settings, overrides,
        [](const char* name, const std::string& value) {
            repiu::platform::PublishEnvironmentSetting(name, value.c_str());
        });
    logger->info("Launcher settings published swap-interval/volume/post-shader/"
                 "full-precision-textures/fullscreen/keep-aspect: "
                 "{}/{}/{}/{}/{}/{}",
                 applied.swap_interval_published,
                 applied.ymz_volume_published,
                 applied.post_shader_published,
                 applied.texture_full_precision_published,
                 applied.fullscreen_published,
                 applied.keep_aspect_published);
}

// Issue #48. The launcher's update check, unless `[Launcher] check_updates`
// is 0 or REPIU_UPDATE_CHECK says off. REPIU_UPDATE_CURRENT_VERSION stands in
// for the build's version, for testing an update from an older one. The check
// starts at once, on its own thread.
std::unique_ptr<repiu::update::LauncherUpdater> CreateLauncherUpdater(
    const repiu::launcher::LauncherSettings& settings,
    const std::filesystem::path& install_folder,
    const std::shared_ptr<spdlog::logger>& logger)
{
    if (settings.has_check_updates && !settings.check_updates)
    {
        logger->info("Update check off ([Launcher] check_updates = 0)");
        return nullptr;
    }
    if (!repiu::runtime::ResolvePromotedToggle(
            std::getenv("REPIU_UPDATE_CHECK")))
    {
        logger->info("Update check off (REPIU_UPDATE_CHECK)");
        return nullptr;
    }
    auto build_version = repiu::update::ParseSemanticVersion(
        repiu::update::BuildVersionText());
    if (const char* pretend = std::getenv("REPIU_UPDATE_CURRENT_VERSION"))
    {
        const auto pretended = repiu::update::ParseSemanticVersion(pretend);
        if (pretended.has_value())
        {
            logger->warn("Update check treats this build as v{} "
                         "(REPIU_UPDATE_CURRENT_VERSION)",
                         repiu::update::FormatSemanticVersion(*pretended));
            build_version = pretended;
        }
    }
    if (!build_version.has_value() || install_folder.empty())
    {
        return nullptr;
    }
    repiu::update::LauncherUpdaterConfig config;
    config.build_version = *build_version;
    config.install_folder = install_folder;
    config.platform = std::string(repiu::platform::BuildPlatformName());
    config.architecture =
        std::string(repiu::platform::BuildArchitectureName());
    config.user_agent = "rePIU/" +
        std::string(repiu::update::BuildVersionText()) + " (" +
        repiu::platform::BuildIdentityLabel() + ")";
    config.log = [logger](const std::string& line) {
        logger->info("{}", line);
    };
    auto updater =
        std::make_unique<repiu::update::LauncherUpdater>(std::move(config));
    updater->StartCheck();
    return updater;
}

// Issue #48. Starts the freshly installed launcher in this one's place. The
// variables this session published for its games are withdrawn first, or the
// new launcher would take them for its caller's and freeze those settings; the
// test override goes too, since it described the old build. Linux replaces the
// process image, keeping the process id a game launcher such as Steam tracks;
// Win32 runs the new launcher as a child and passes its exit code on.
int RestartLauncherAfterUpdate(
    const std::filesystem::path& executable,
    const repiu::launcher::LauncherEnvironmentOverrides& caller_overrides,
    const std::shared_ptr<spdlog::logger>& logger)
{
    const std::pair<const char*, bool> published[] = {
        {repiu::launcher::kLauncherSwapIntervalVariable,
         caller_overrides.swap_interval},
        {repiu::launcher::kLauncherYmzVolumeVariable,
         caller_overrides.ymz_volume},
        {repiu::launcher::kLauncherPostShaderVariable,
         caller_overrides.post_shader},
        {repiu::launcher::kLauncherTextureFullPrecisionVariable,
         caller_overrides.texture_full_precision},
        {repiu::launcher::kLauncherFullscreenVariable,
         caller_overrides.fullscreen},
        {repiu::launcher::kLauncherKeepAspectVariable,
         caller_overrides.keep_aspect},
    };
    for (const auto& [name, caller_set] : published)
    {
        if (!caller_set)
        {
            repiu::platform::WithdrawEnvironmentSetting(name);
        }
    }
    repiu::platform::WithdrawEnvironmentSetting("REPIU_UPDATE_CURRENT_VERSION");
    logger->info("Restarting the updated launcher: {}", executable.string());
    logger->flush();
    std::uint32_t host_error = 0;
    repiu::platform::ReplaceProcessImage(executable, &host_error);
    const std::string path = executable.string();
    const std::string command_line = "\"" + path + "\"";
    repiu::platform::ChildProcessLaunch launch;
    launch.executable_path = path.c_str();
    launch.command_line = command_line.c_str();
    const int exit_code =
        repiu::platform::RunChildProcessAndWait(launch, &host_error);
    if (exit_code == repiu::platform::kChildProcessDidNotStart)
    {
        logger->error("The updated launcher did not start (host error {}); "
                      "start rePIU again",
                      host_error);
        return 1;
    }
    return exit_code;
}

// Issue #45. Stores a fullscreen or keep-aspect change the operator made in
// the game. The file is read again rather than kept from start-up, so a value
// stored meanwhile is not lost, and only the two keys change.
void StoreSessionDisplayPreferences(
    const std::filesystem::path& config_directory, bool fullscreen,
    bool keep_aspect, const std::shared_ptr<spdlog::logger>& logger)
{
    repiu::launcher::LauncherSettingsLoad stored =
        repiu::launcher::LoadLauncherSettings(config_directory);
    stored.settings.has_fullscreen = true;
    stored.settings.fullscreen = fullscreen;
    stored.settings.has_keep_aspect = true;
    stored.settings.keep_aspect = keep_aspect;
    const std::string path =
        repiu::launcher::LauncherSettingsPath(config_directory).string();
    if (!repiu::launcher::SaveLauncherSettings(config_directory,
                                               stored.settings))
    {
        logger->warn("Failed to write {}", path);
        return;
    }
    logger->info("Display settings stored in {}: fullscreen={} keep_aspect={}",
                 path, fullscreen ? 1 : 0, keep_aspect ? 1 : 0);
}

// Task 500 revision. The launcher has to load a GPU driver to draw anything,
// and that driver claims low address space the guest needs: a first attempt
// that continued in the same process failed with the fixed reservation at
// 0x00010000 blocked by a committed 12 KiB block and every relocated-image
// candidate from 0x01000000 through 0x09000000 occupied. The guest address
// space must be claimed before any driver loads, which cannot be reconciled
// with drawing first, so the selection is carried into a fresh process whose
// address space is still intact. The child receives the ROM set as its
// argument, so it never opens the launcher itself, and it inherits the
// environment the launcher just published.
int RunSelectedRomSetInNewProcess(const char* executable_path,
                                  const std::string& rom_set_id,
                                  const std::shared_ptr<spdlog::logger>& logger)
{
    std::string command_line =
        repiu::launcher::BuildLauncherChildCommandLine(executable_path,
                                                       rom_set_id);

    // Task 503d-17: the same launch stated twice, because Windows takes a
    // command line and POSIX takes argv. The command line is still built by the
    // launcher function the probe covers, so its quoting has one home.
    const char* arguments[] = {rom_set_id.c_str()};
    repiu::platform::ChildProcessLaunch launch;
    launch.executable_path = executable_path;
    launch.command_line = command_line.c_str();
    launch.arguments = arguments;
    launch.argument_count = rom_set_id.empty() ? 0U : 1U;

    std::uint32_t host_error = 0;
    const int exit_code =
        repiu::platform::RunChildProcessAndWait(launch, &host_error);
    if (exit_code == repiu::platform::kChildProcessDidNotStart)
    {
        logger->error("Failed to start {} for {}: host error {}",
                      executable_path, rom_set_id, host_error);
        return 1;
    }
    return exit_code;
}

std::optional<repiu::target::TargetProfile> BuildDirectExecutableProfile(
    int argc,
    char** argv)
{
    if (argc < 2)
    {
        return std::nullopt;
    }

    std::filesystem::path executable_path = argv[1];
    if (!std::filesystem::is_regular_file(executable_path))
    {
        return std::nullopt;
    }

    std::filesystem::path working_directory =
        executable_path.parent_path();
    if (working_directory.empty())
    {
        working_directory = ".";
    }

    return repiu::target::TargetProfile{
        "direct_executable",
        "Direct executable",
        executable_path,
        working_directory,
        working_directory,
        repiu::target::ExecutableFormatHint::kDos4gwLe,
        "dos4gw_console_sample",
        "",
        "",
        repiu::target::TargetRuntimeReservationHint{
            true,
            0x00010000,
            0x00800000,
        },
    };
}

}  // namespace

int main(int argc, char** argv)
{
    // Task 441: first thing, so a host crash anywhere after this point names
    // itself instead of vanishing into an exit code.
    repiu::platform::InstallHostCrashReporter();
    std::shared_ptr<spdlog::logger> logger = CreateLoaderLogger();

    // Task 771. Options come off the command line before anything reads
    // argv[1], and each is published as the variable its consumer already
    // reads. The command line is the most direct instruction for this run, so
    // it overwrites the caller's variable; published this early, it also wins
    // over cfg/repiu.ini and is inherited by a launcher's child processes.
    const repiu::launcher::CommandLineOptions command_line =
        repiu::launcher::ParseCommandLineOptions(
            std::vector<std::string>(argv + (argc > 0 ? 1 : 0), argv + argc));
    if (!command_line.error.empty())
    {
        logger->error("Command line: {}", command_line.error);
        return 1;
    }
    if (command_line.has_post_shader)
    {
        repiu::platform::PublishEnvironmentSetting(
            repiu::launcher::kLauncherPostShaderVariable,
            command_line.post_shader.c_str());
        logger->info("Command line post shader: {}", command_line.post_shader);
    }
    std::vector<char*> remaining_argv;
    remaining_argv.push_back(argc > 0 ? argv[0] : nullptr);
    for (const std::string& argument : command_line.positional)
    {
        remaining_argv.push_back(const_cast<char*>(argument.c_str()));
    }
    remaining_argv.push_back(nullptr);
    argc = static_cast<int>(remaining_argv.size() - 1U);
    argv = remaining_argv.data();

    if (LauncherRequested(argc))
    {
        const std::filesystem::path config_directory("cfg");
        // The caller's environment is captured once, before anything is
        // published. After the first launch our own published variables would
        // otherwise look like caller-set ones and freeze every later change the
        // operator makes in the launcher.
        const repiu::launcher::LauncherEnvironmentOverrides caller_overrides =
            repiu::launcher::ResolveLauncherEnvironmentOverrides(
                std::getenv(repiu::launcher::kLauncherSwapIntervalVariable),
                std::getenv(repiu::launcher::kLauncherYmzVolumeVariable),
                std::getenv(repiu::launcher::kLauncherPostShaderVariable),
                std::getenv(
                    repiu::launcher::kLauncherTextureFullPrecisionVariable),
                std::getenv(repiu::launcher::kLauncherFullscreenVariable),
                std::getenv(repiu::launcher::kLauncherKeepAspectVariable));
        const char* const executable_path =
            argc >= 1 && argv[0] != nullptr ? argv[0] : "repiu.exe";
        // Issue #48: files an earlier update renamed aside go now, and the
        // update check starts while the launcher opens.
        std::filesystem::path host_executable =
            repiu::platform::HostExecutablePath();
        if (host_executable.empty())
        {
            std::error_code path_error;
            host_executable =
                std::filesystem::absolute(executable_path, path_error);
        }
        const std::filesystem::path install_folder =
            host_executable.parent_path();
        if (const std::size_t removed =
                repiu::update::RemovePreviousInstallLeftovers(install_folder);
            removed != 0U)
        {
            logger->info("Removed {} files left by the previous update",
                         removed);
        }
        const std::unique_ptr<repiu::update::LauncherUpdater> updater =
            CreateLauncherUpdater(
                repiu::launcher::LoadLauncherSettings(config_directory)
                    .settings,
                install_folder, logger);
        bool launcher_available = true;
        // The launcher is the front end for the whole session: a finished game
        // returns here rather than ending the process, so an operator can pick
        // another ROM set without starting rePIU again.
        while (launcher_available)
        {
            auto stored =
                repiu::launcher::LoadLauncherSettings(config_directory);
            for (const std::string& warning : stored.warnings)
            {
                logger->warn("{}", warning);
            }
            const repiu::launcher::LauncherUiResult chosen =
                repiu::launcher::RunLauncherUi(
                    repiu::launcher::BuildRomSetCatalog("roms"),
                    stored.settings, updater.get());
            if (chosen.unavailable)
            {
                // A headless or driver-less host must still run the previous
                // no-argument behavior rather than fail to start.
                logger->warn(
                    "Launcher unavailable, using the default target: {}",
                    chosen.message);
                launcher_available = false;
                break;
            }
            if (chosen.settings_changed &&
                !repiu::launcher::SaveLauncherSettings(config_directory,
                                                       chosen.settings))
            {
                logger->warn("Failed to write {}",
                             repiu::launcher::LauncherSettingsPath(
                                 config_directory)
                                 .string());
            }
            if (chosen.install_update && updater != nullptr)
            {
                std::string install_error;
                if (updater->Install(&install_error))
                {
                    return RestartLauncherAfterUpdate(host_executable,
                                                      caller_overrides, logger);
                }
                // The old files are back in place; the launcher reopens and
                // shows the failure with a Retry.
                logger->warn("Update not installed: {}", install_error);
                continue;
            }
            if (!chosen.launch)
            {
                logger->info("Launcher closed without starting a ROM set{}",
                             chosen.closed_by_exit_chord
                                 ? " (gamepad exit chord held for 1 s)"
                                 : "");
                return 0;
            }
            PublishLauncherSettings(chosen.settings, caller_overrides, logger);
            logger->info("Launcher selected ROM set: {}", chosen.rom_set_id);
            const int child_exit_code = RunSelectedRomSetInNewProcess(
                executable_path, chosen.rom_set_id, logger);
            // A failed run returns to the launcher instead of ending the
            // session, so the operator can read the reason and choose again.
            logger->info("{} exited with code {}; returning to the launcher",
                         chosen.rom_set_id, child_exit_code);
        }
    }

    // Task 746. A run with arguments skips the launcher, and until now that
    // meant it skipped `cfg/repiu.ini` too, so a swap interval or a volume
    // stored there took effect only through the launcher. The same settings
    // are read and published here, with the same rule: a variable already in
    // the caller's environment wins over the file. The launcher's own child
    // process passes through here as well and sees the parent's published
    // variables as caller-set, so its outcome is unchanged.
    {
        const std::filesystem::path config_directory("cfg");
        const repiu::launcher::LauncherSettingsLoad stored =
            repiu::launcher::LoadLauncherSettings(config_directory);
        for (const std::string& warning : stored.warnings)
        {
            logger->warn("launcher settings: {}", warning);
        }
        if (stored.file_present)
        {
            const repiu::launcher::LauncherEnvironmentOverrides caller_overrides =
                repiu::launcher::ResolveLauncherEnvironmentOverrides(
                    std::getenv(repiu::launcher::kLauncherSwapIntervalVariable),
                    std::getenv(repiu::launcher::kLauncherYmzVolumeVariable),
                    std::getenv(repiu::launcher::kLauncherPostShaderVariable),
                    std::getenv(repiu::launcher::
                                    kLauncherTextureFullPrecisionVariable),
                    std::getenv(repiu::launcher::kLauncherFullscreenVariable),
                    std::getenv(repiu::launcher::kLauncherKeepAspectVariable));
            logger->info(
                "Launcher settings read from {} for an argument run "
                "(environment wins: swap-interval/volume {}/{})",
                repiu::launcher::LauncherSettingsPath(config_directory).string(),
                caller_overrides.swap_interval ? "env" : "file",
                caller_overrides.ymz_volume ? "env" : "file");
            PublishLauncherSettings(stored.settings, caller_overrides, logger);
        }
        // Issue #45: the game's OSD, Alt+Enter and double click store the
        // display options here. Registered before the execution thread and
        // the Glide window exist; the launcher parent never reaches this
        // point while a game runs, so only this process writes the file then.
        repiu::engine::SetSessionDisplayPreferenceSink(
            [config_directory, logger](bool fullscreen, bool keep_aspect) {
                StoreSessionDisplayPreferences(config_directory, fullscreen,
                                               keep_aspect, logger);
            });
    }

    const repiu::target::TargetProfile* profile =
        SelectTargetProfile(argc, argv);
    std::optional<repiu::target::TargetProfile> direct_profile;
    std::optional<repiu::target::TargetProfile> mounted_profile;
    std::optional<std::filesystem::path> cd_chd_path;
    std::optional<std::filesystem::path> sound_rom_zip_path;
    if (profile == nullptr)
    {
        direct_profile = BuildDirectExecutableProfile(argc, argv);
        if (!direct_profile.has_value())
        {
            logger->error("Target profile or executable path was not found");
            return 1;
        }
        profile = &direct_profile.value();
    }

    if (!profile->rom_set_id.empty())
    {
        repiu::assets::PiuChdMountResult mount;
        if (!repiu::assets::PreparePiuChdMount(
                profile->rom_set_id, "roms", "build/runtime_mounts", &mount) ||
            !mount.valid || !mount.mounted)
        {
            logger->error("{} CHD mount failed: {}", profile->rom_set_id,
                          mount.message);
            return 1;
        }
        logger->info("{} ROM ZIP: {}", profile->rom_set_id,
                     mount.rom_zip_path.string());
        logger->info("{} CHD: {}", profile->rom_set_id,
                     mount.chd_path.string());
        logger->info("{} CHD data track LBA: {}", profile->rom_set_id,
                     mount.data_track_lba);
        logger->info("{} ISO extent LBA bias: {}", profile->rom_set_id,
                     mount.iso_extent_lba_bias);
        logger->info("{} mount root: {}", profile->rom_set_id,
                     mount.mount_root.string());
        logger->info("{} mount cache reused: {}", profile->rom_set_id,
                     mount.cache_reused ? "true" : "false");
        logger->info("{} extracted files/bytes: {}/{}", profile->rom_set_id,
                     mount.extracted_file_count,
                     mount.extracted_byte_count);
        logger->info("{} skipped external-extent files: {}",
                     profile->rom_set_id,
                     mount.skipped_external_extent_file_count);
        mounted_profile = *profile;
        mounted_profile->executable_path = mount.executable_path;
        mounted_profile->working_directory = mount.mount_root / "PIU";
        mounted_profile->asset_root = mount.mount_root;
        cd_chd_path = mount.chd_path;
        sound_rom_zip_path = mount.rom_zip_path;
        profile = &mounted_profile.value();
    }
    logger->info("loader target: {}", profile->id);
    // #15: shown in the in-game OSD. Written before any execution thread or
    // window exists.
    repiu::engine::SetSessionTargetProfile(profile->id);
    logger->info("loader executable: {}",
                 profile->executable_path.string());
    logger->info("PIU JAMMA board enabled: {}",
                 profile->enable_piu_jamma_board ? "true" : "false");
    logger->info("PIU10 ISA board enabled: {}",
                 profile->enable_piu10_isa_board ? "true" : "false");
    logger->info("CAT702 enabled: {}",
                 profile->enable_cat702 ? "true" : "false");
    logger->info("Parent ROM set: {}",
                 profile->parent_rom_set_id.empty()
                     ? "none"
                     : profile->parent_rom_set_id);

    // Task 497: per-ROM-set configuration. Loaded here, before the guest
    // thread and the SDL window exist, because the resolved bindings are read
    // without a lock from both of them afterwards.
    //
    // Task 503d-13: the video subsystem is brought up first because that is
    // what gives SDL a keyboard layout, and resolving a binding without one
    // answers from SDL's default layout instead. It matters for the built-in
    // defaults too, whose P1 keys are letters. The call is reference counted,
    // so the render backend still initializes video for itself later; a
    // failure here is not fatal and only costs the layout.
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO))
    {
        logger->warn("SDL video for keyboard layout: {}", SDL_GetError());
    }
    if (!profile->rom_set_id.empty())
    {
        LoadRomSetInputConfiguration(*logger, profile->rom_set_id,
                                     ExecutableDirectory(argc, argv));
    }
    // Covers the run that loads no configuration, whose defaults would
    // otherwise stay resolved as they were on first use.
    repiu::engine::ResolveActiveJammaScancodes();

    // Task 498: per-ROM-set NVRAM. Resolved before the guest runs because the
    // EEPROM device is created on its first port access and cannot be moved
    // afterwards.
    ResolveRomSetNvramPaths(*logger, profile->rom_set_id,
                            ExecutableDirectory(argc, argv));
#if defined(REPIU_WIN32_HOST_IMAGE_BASE)
    logger->info("host image base policy: {}",
                 Hex32(REPIU_WIN32_HOST_IMAGE_BASE));
#endif

    if (!profile->runtime_reservation_hint.valid)
    {
        logger->error("Target has no runtime reservation hint");
        return 1;
    }

    repiu::engine::RuntimeMemoryPolicy policy;
    if (!repiu::engine::BuildRuntimeMemoryPolicyFromFixedRange(
            profile->runtime_reservation_hint.base_address,
            profile->runtime_reservation_hint.reserve_size,
            &policy))
    {
        logger->error("Failed to build fixed range policy");
        return 1;
    }

    PrintPolicy(*logger, policy);

    repiu::engine::AddressRangeProbe probe;
    if (!repiu::engine::ProbeRuntimeAddressRange(
            policy, &probe))
    {
        logger->error("Failed to probe fixed runtime range: {}",
                      probe.message);
        return 1;
    }

    PrintProbe(*logger, probe);

    repiu::engine::AddressRangeReservation reservation;
    if (!repiu::engine::ReserveRuntimeAddressRange(
            policy, &reservation))
    {
        logger->error("Failed to run early reservation attempt");
        return 1;
    }

    PrintReservation(*logger, reservation);
    repiu::engine::ReleaseRuntimeAddressRange(reservation);

    std::vector<std::uint8_t> data;
    std::string read_error;
    if (!ReadBinaryFile(profile->executable_path, &data, &read_error))
    {
        logger->error("Failed to read {}: {}",
                      profile->executable_path.string(),
                      read_error);
        return 1;
    }

    const std::filesystem::path dos4gw_path = ResolveSiblingAssetPath(
        profile->executable_path.parent_path(), "DOS4GW.EXE");
    std::optional<repiu::exe::Dos16mBoundModule> linexe_runtime_module;
    if (!dos4gw_path.empty())
    {
        std::vector<std::uint8_t> dos4gw_data;
        if (!ReadBinaryFile(dos4gw_path, &dos4gw_data, &read_error))
        {
            logger->error("Failed to read user DOS4GW asset {}: {}",
                          dos4gw_path.string(),
                          read_error);
            return 1;
        }
        std::vector<repiu::exe::Dos16mBoundModule> dos16m_modules;
        repiu::exe::ParseError dos16m_error;
        if (!repiu::exe::ParseDos16mBoundModules(
                dos4gw_data, &dos16m_modules, &dos16m_error))
        {
            PrintParseError(*logger, dos16m_error);
            return 1;
        }
        const repiu::exe::Dos16mBoundModule* linexe_module =
            repiu::exe::FindDos16mBoundModule(
                dos16m_modules, "LINEXE.EXP");
        if (linexe_module == nullptr)
        {
            logger->error("User DOS4GW asset contains no LINEXE.EXP module");
            return 1;
        }
        linexe_runtime_module = *linexe_module;
        logger->info("DOS4GW bound module count: {}", dos16m_modules.size());
        logger->info("LINEXE runtime extraction: header={} next={} entry={}:{} segments={} relocations={}",
                     Hex32(linexe_module->header_file_offset),
                     Hex32(linexe_module->next_header_file_offset),
                     Hex32(linexe_module->initial_cs),
                     Hex32(linexe_module->initial_ip),
                     linexe_module->segments.size(),
                     linexe_module->relocation_count);
        for (const repiu::exe::Dos16mBoundSegment& segment :
             linexe_module->segments)
        {
            logger->info("LINEXE extracted segment: selector={} limit={} access={} image={} relocations={}",
                         Hex32(segment.selector),
                         Hex32(segment.limit),
                         Hex32(segment.access),
                         segment.image.size(),
                         segment.selector_relocation_offsets.size());
        }
    }
    else
    {
        logger->info("No adjacent user DOS4GW asset; LINEXE extraction skipped");
    }

    std::vector<repiu::exe::LeResidentName> glide_exports;
    const std::filesystem::path glide_path = ResolveSiblingAssetPath(
        profile->executable_path.parent_path(), "Glide2x.ovl");
    if (glide_path.empty())
    {
        // Silence here is what made this expensive to find: the whole DOS/4G
        // environment is built from these exports, so an absent overlay shows
        // up much later as a guest whose DLL loader will not initialize.
        logger->warn("No Glide2x.ovl beside {}; Glide gates will be absent",
                     profile->executable_path.string());
    }
    else
    {
        std::vector<std::uint8_t> glide_data;
        repiu::exe::MzHeader glide_mz;
        repiu::exe::LeHeader glide_le;
        repiu::exe::ParseError glide_error;
        if (!ReadBinaryFile(glide_path, &glide_data, &read_error) ||
            !repiu::exe::ParseMzHeader(
                glide_data, &glide_mz, &glide_error) ||
            !repiu::exe::ParseLeHeader(
                glide_data, glide_mz.le_offset, &glide_le, &glide_error) ||
            !repiu::exe::ParseLeResidentNames(
                glide_data, glide_le, &glide_exports, &glide_error))
        {
            logger->error("Failed to parse Glide2x.ovl export metadata");
            return 1;
        }
        logger->info("Glide2x resident export metadata count: {}",
                     glide_exports.size());
    }

    repiu::exe::ParseError error;
    repiu::exe::Dos4gwLoadResult load_result;
    if (!repiu::exe::LoadDos4gwExecutable(data, *profile, &load_result,
                                          &error))
    {
        PrintParseError(*logger, error);
        return 1;
    }

    // The guest allocator grows its program block to ~0x38AA000 bytes
    // (~57 MiB) via INT 21h AH=4Ah with 32-bit EBX paragraph counts
    // (Task 221); 64 MiB of slack lets those resizes succeed against
    // genuinely committed arena memory while the allocator ceiling still
    // guards the LINEXE pages at the arena end.
    constexpr std::uint32_t kRuntimeArenaExpansionSlack = 0x08000000;
    repiu::runtime::RuntimeMemoryArenaPlan arena_size_plan;
    if (!repiu::runtime::BuildRuntimeMemoryArenaPlan(
            profile->runtime_reservation_hint.base_address,
            profile->runtime_reservation_hint.reserve_size,
            kRuntimeArenaExpansionSlack,
            &arena_size_plan) ||
        !arena_size_plan.valid)
    {
        PrintRuntimeMemoryArenaPlan(*logger, arena_size_plan);
        logger->error("Failed to build runtime memory arena plan");
        return 1;
    }

    repiu::engine::AddressRangeReservation
        relocated_arena_reservation;
    if (!SelectAndReserveRelocatedImageBase(
            arena_size_plan.arena_reserve_size,
            *logger,
            &relocated_arena_reservation))
    {
        logger->error("Failed to reserve an available relocated image base");
        return 1;
    }

    const std::uint32_t relocated_image_base =
        relocated_arena_reservation.reserved_base;

    logger->info("selected relocated image base: {}",
                 Hex32(relocated_image_base));

    repiu::runtime::RuntimeMemoryArenaPlan arena_plan;
    if (!repiu::runtime::BuildRuntimeMemoryArenaPlan(
            relocated_image_base,
            profile->runtime_reservation_hint.reserve_size,
            kRuntimeArenaExpansionSlack,
            &arena_plan) ||
        !arena_plan.valid)
    {
        PrintRuntimeMemoryArenaPlan(*logger, arena_plan);
        logger->error("Failed to build relocated runtime memory arena plan");
        repiu::engine::ReleaseRuntimeAddressRange(
            relocated_arena_reservation);
        return 1;
    }

    PrintRuntimeMemoryArenaPlan(*logger, arena_plan);

    repiu::runtime::RelocatableRuntimeImagePlan relocatable_plan;
    if (!repiu::runtime::BuildRelocatableRuntimeImagePlan(
            load_result, relocated_image_base, &relocatable_plan, &error))
    {
        PrintParseError(*logger, error);
        repiu::engine::ReleaseRuntimeAddressRange(
            relocated_arena_reservation);
        return 1;
    }

    repiu::runtime::RelocatedRuntimeImage relocated_image;
    if (!repiu::runtime::BuildRelocatedRuntimeImage(
            load_result, relocatable_plan, &relocated_image, &error))
    {
        PrintParseError(*logger, error);
        repiu::engine::ReleaseRuntimeAddressRange(
            relocated_arena_reservation);
        return 1;
    }
    if (const char* const trace_address_text =
            std::getenv("REPIU_AOT_PLAN_TRACE");
        trace_address_text != nullptr &&
        std::strcmp(trace_address_text, "0x01100027") == 0)
    {
        constexpr std::uint32_t trace_address = 0x01100027U;
        std::fprintf(stderr,
                     "[repiu-runtime-image-trace] address=0x%08X\n",
                     trace_address);
        for (const repiu::runtime::RelocatedRuntimeObject& object :
             relocated_image.objects)
        {
            if (trace_address < object.relocated_base_address)
            {
                continue;
            }
            const std::uint64_t offset =
                static_cast<std::uint64_t>(trace_address) -
                object.relocated_base_address;
            if (offset >= repiu::runtime::RelocatedRuntimeObjectByteCount(
                              object))
            {
                continue;
            }
            const std::uint8_t* const bytes =
                repiu::runtime::RelocatedRuntimeObjectBytes(object) + offset;
            std::fprintf(stderr,
                         "[repiu-runtime-image-trace-object] index=%u "
                         "base=0x%08X size=0x%08X offset=0x%llX "
                         "bytes=%02X%02X%02X flags=0x%08X\n",
                         static_cast<unsigned>(object.object_index),
                         static_cast<unsigned>(object.relocated_base_address),
                         static_cast<unsigned>(object.virtual_size),
                         static_cast<unsigned long long>(offset), bytes[0],
                         offset + 1U < repiu::runtime::RelocatedRuntimeObjectByteCount(object)
                             ? bytes[1] : 0U,
                         offset + 2U < repiu::runtime::RelocatedRuntimeObjectByteCount(object)
                             ? bytes[2] : 0U,
                         static_cast<unsigned>(object.flags));
        }
    }

    repiu::runtime::ExecutionBackend execution_backend =
        repiu::runtime::kDefaultExecutionBackend;
    if (!ReadExecutionBackend(&execution_backend))
    {
        logger->error(
            "REPIU_EXECUTION_BACKEND must be legacy or dynamic");
        repiu::engine::ReleaseRuntimeAddressRange(
            relocated_arena_reservation);
        return 1;
    }
    const bool use_dynamic_backend =
        repiu::runtime::ExecutionBackendUsesDynamicTranslation(
            execution_backend);
    const bool direct_glide_dispatch_requested =
        use_dynamic_backend &&
        repiu::engine::
            ResolveGlideGateDirectDispatchEnabled(
                std::getenv("REPIU_AOT_DBT_GLIDE_GATE_DISPATCH"));
    const bool direct_glide_dispatch_capable =
        repiu::engine::GetGlideGateDirectDispatchThunkAddress() != nullptr;
    const bool direct_glide_dispatch_enabled =
        direct_glide_dispatch_requested && direct_glide_dispatch_capable;
    repiu::runtime::AotCodeCacheBuildOptions aot_build_options;
    // Always on for the dynamic backend. Return-miss and direct-edge dispatch
    // and timer safe points were promoted in Task 424, the direct return table
    // in Task 499 (pumpit8 +59.38% frames over three alternating 60-second
    // pairs); Task i020 removed their environment kill switches. The fields
    // stay because the legacy backend and the probes' control images build
    // with them off. Direct-edge dispatch was never optional in practice:
    // without it, images with direct edges outside the cache fail to build.
    aot_build_options.enable_dbt_return_miss_dispatch = use_dynamic_backend;
    aot_build_options.enable_dbt_direct_edge_dispatch = use_dynamic_backend;
    aot_build_options.enable_timer_safe_points = use_dynamic_backend;
    aot_build_options.enable_direct_return_table = use_dynamic_backend;
    aot_build_options.direct_return_table_bits =
        repiu::runtime::ResolveAotDirectReturnTableBits(
            std::getenv("REPIU_AOT_DIRECT_RETURN_TABLE_BITS"));
    // Task i018 promoted this from opt-in. With the INT 21h allowlist
    // narrowed to the register-only services (AH=2Ch/42h; AH=3Fh read broke
    // eight titles by skipping the reentry funnel after guest memory
    // changed), the 16-romset smoke matrix runs clean and pumpitea's
    // post-logo gap drops from ~6 s to ~1.5-2.7 s. REPIU_AOT_DBT_SUPERBLOCK=0
    // restores the INT3-only boundaries.
    aot_build_options.enable_dbt_hle_dispatch =
        use_dynamic_backend &&
        repiu::runtime::ResolvePromotedToggle(
            std::getenv("REPIU_AOT_DBT_SUPERBLOCK"));
    // Task 386 promoted the isolated Port-I/O dispatch after a Music Select
    // capture confirmed lower per-frame exception and HLE costs; Task i020
    // removed its kill switch.
    aot_build_options.enable_dbt_port_io_dispatch = use_dynamic_backend;
    // Tasks 291, 390 and 384 promoted the guarded segment pop, load and read
    // paths for aot-dbt; v0.0.206 (#18) moved their slots onto the shadow, and
    // issue #30 removed their kill switches after releases with no regression.
    aot_build_options.enable_guarded_segment_pop = use_dynamic_backend;
    aot_build_options.enable_guarded_segment_load = use_dynamic_backend;
    aot_build_options.enable_guarded_segment_read = use_dynamic_backend;
    if (use_dynamic_backend && !ReadAotIndirectInlineCacheEntryCount(
            &aot_build_options.indirect_inline_cache_entry_count))
    {
        logger->error(
            "REPIU_AOT_INDIRECT_CACHE_SLOTS must be either 1 or 4");
        repiu::engine::ReleaseRuntimeAddressRange(
            relocated_arena_reservation);
        return 1;
    }
    // Task 576. Not a toggle beside the ones above: on an x86-64 host an image
    // built without long-mode emission is wrong rather than different, because
    // long mode reads several of the guest's encodings differently and some of
    // them raise nothing. The host is asked instead of an environment variable
    // so a wrong configuration is not quietly runnable.
    aot_build_options.enable_long_mode_emission =
        repiu::runtime::HostRequiresLongModeEmission();
    repiu::runtime::AotTranslationPlan aot_plan;
    repiu::runtime::AotCodeCacheImage aot_image;
    if (use_dynamic_backend &&
        (!repiu::runtime::BuildAotTranslationPlan(relocated_image,
                                                  &aot_plan) ||
         !repiu::runtime::BuildAotCodeCacheImage(
             aot_plan, aot_build_options, &aot_image)))
    {
        logger->error("Failed to build requested AOT execution image: {} / {}",
                      aot_plan.message, aot_image.message);
        // Task 735. Name the rejected entries: this image never runs, so its
        // failure is only diagnosable here.
        logger->error("AOT decode failures total/sampled: {}/{}",
                      aot_image.decode_failure_count,
                      aot_image.decode_failure_samples.size());
        for (const repiu::runtime::AotDecodeFailureSample& sample :
             aot_image.decode_failure_samples)
        {
            std::string guest_bytes;
            int guest_kind = -1;
            for (const repiu::runtime::AotBasicBlock& block : aot_plan.blocks)
            {
                for (const repiu::runtime::AotInstructionRecord& instruction :
                     block.instructions)
                {
                    if (instruction.guest_address == sample.guest_address)
                    {
                        guest_kind = static_cast<int>(instruction.kind);
                        for (const std::uint8_t byte : instruction.bytes)
                        {
                            char hex[3] = {};
                            std::snprintf(hex, sizeof(hex), "%02X", byte);
                            guest_bytes += hex;
                        }
                    }
                }
            }
            std::string emitted_bytes;
            const std::size_t emitted_end = std::min<std::size_t>(
                aot_image.bytes.size(),
                static_cast<std::size_t>(sample.cache_offset) +
                    std::min<std::uint32_t>(sample.emitted_length, 48U));
            for (std::size_t offset = sample.cache_offset;
                 offset < emitted_end; ++offset)
            {
                char hex[3] = {};
                std::snprintf(hex, sizeof(hex), "%02X",
                              aot_image.bytes[offset]);
                emitted_bytes += hex;
            }
            logger->error(
                "AOT decode failure guest=0x{:08X} kind={} guest_bytes={} "
                "cache=0x{:08X} emitted_len={} decoded_bytes={} "
                "decoded_instructions={} expected_instructions={} emitted={}",
                sample.guest_address, guest_kind, guest_bytes,
                sample.cache_offset, sample.emitted_length,
                sample.decoded_bytes, sample.decoded_instructions,
                sample.expected_instructions, emitted_bytes);
        }
        repiu::engine::ReleaseRuntimeAddressRange(
            relocated_arena_reservation);
        return 1;
    }
    logger->info("requested execution backend: {}",
                 repiu::runtime::ExecutionBackendName(execution_backend));
    if (use_dynamic_backend)
    {
        logger->info("AOT indirect inline-cache slots: {}",
                     aot_build_options.indirect_inline_cache_entry_count);
        logger->info("AOT guarded segment-pop enabled: {}",
                     aot_build_options.enable_guarded_segment_pop);
        logger->info("AOT guarded segment-load enabled/sites: {}/{}",
                     aot_build_options.enable_guarded_segment_load,
                     aot_image.guarded_segment_load_sites.size());
        logger->info("AOT guarded segment-read enabled/sites: {}/{}",
                     aot_build_options.enable_guarded_segment_read,
                     aot_image.guarded_segment_read_sites.size());
        // Task 424: return-miss dispatch had no log line of its own, so its
        // toggle could not be verified from the log alone. Direct-edge already
        // reported its site count and now reports the toggle beside it.
        logger->info("AOT-DBT return-miss dispatch enabled: {}",
                     aot_build_options.enable_dbt_return_miss_dispatch);
        logger->info(
            "AOT-DBT unresolved direct-edge dispatch enabled/sites: {}/{}",
            aot_build_options.enable_dbt_direct_edge_dispatch,
            aot_image.dbt_direct_edge_dispatch_sites.size());
        logger->info("AOT-DBT superblock HLE dispatch enabled: {}",
                     aot_build_options.enable_dbt_hle_dispatch);
        logger->info("AOT-DBT Port-I/O dispatch enabled: {}",
                     aot_build_options.enable_dbt_port_io_dispatch);
        logger->info(
            "AOT-DBT Glide gate direct dispatch requested/capable/enabled: {}/{}/{}",
            direct_glide_dispatch_requested,
            direct_glide_dispatch_capable,
            direct_glide_dispatch_enabled);
        logger->info("AOT timer safe points enabled/sites: {}/{}",
                     aot_build_options.enable_timer_safe_points,
                     aot_image.timer_safe_point_sites.size());
    }

    repiu::runtime::GuestStackSwitchPlan stack_plan;
    std::uint32_t stack_base = relocated_image.relocated_image_base;
    std::uint32_t stack_limit =
        relocated_image.relocated_stack_top_linear_address;
    if (load_result.le_header.stack_object > 0 &&
        load_result.le_header.stack_object <=
            relocatable_plan.object_regions.size())
    {
        const repiu::runtime::RelocatableRuntimeObjectRegion& stack_region =
            relocatable_plan
                .object_regions[load_result.le_header.stack_object - 1];
        stack_base = stack_region.relocated_base_address;
        stack_limit = stack_region.relocated_base_address +
                      stack_region.virtual_size;
    }
    const std::uint32_t stack_size = stack_limit > stack_base
                                         ? stack_limit - stack_base
                                         : 0;
    const std::uint32_t guard_bytes = stack_size > 8192 ? 4096 : 256;
    repiu::runtime::BuildGuestStackSwitchPlan(
        relocated_image.relocated_entry_linear_address,
        stack_base,
        stack_limit,
        relocated_image.relocated_stack_top_linear_address,
        guard_bytes,
        &stack_plan);
    PrintGuestStackPlan(*logger, stack_plan);

    repiu::hle::HleDispatcherTable hle_dispatcher;
    repiu::hle::BuildInitialHleDispatcherTable(&hle_dispatcher);
    PrintHleDispatcherTable(*logger, hle_dispatcher);

    repiu::hle::DosVirtualFileSystemState dos_file_system;
    if (!repiu::hle::InitializeDosVirtualFileSystem(
            profile->asset_root,
            profile->working_directory,
            &dos_file_system) ||
        !dos_file_system.valid)
    {
        PrintDosVirtualFileSystem(*logger, dos_file_system);
        logger->error("Failed to initialize DOS virtual filesystem");
        repiu::engine::ReleaseRuntimeAddressRange(
            relocated_arena_reservation);
        return 1;
    }
    PrintDosVirtualFileSystem(*logger, dos_file_system);

    repiu::engine::RelocatedImagePlacement placement;
    if (!repiu::engine::PlaceRelocatedImageInReservedRange(
            relocated_image,
            relocated_arena_reservation,
            &placement))
    {
        logger->error("Failed to place relocated image");
        repiu::engine::ReleaseRuntimeAddressRange(
            relocated_arena_reservation);
        return 1;
    }
    if (const char* const trace_address_text =
            std::getenv("REPIU_AOT_PLAN_TRACE");
        trace_address_text != nullptr &&
        std::strcmp(trace_address_text, "0x01100027") == 0)
    {
        const auto* const bytes = reinterpret_cast<const std::uint8_t*>(
            static_cast<std::uintptr_t>(0x01100027U));
        std::fprintf(stderr,
                     "[repiu-runtime-placement-trace] address=0x01100027 "
                     "bytes=%02X%02X%02X\n",
                     bytes[0], bytes[1], bytes[2]);
    }

    PrintPlacement(*logger, placement);
    if (!placement.placed)
    {
        repiu::engine::ReleaseRuntimeAddressRange(
            relocated_arena_reservation);
        logger->error("Failed to place relocated image");
        return 1;
    }
    repiu::engine::AotCodeCachePlacement aot_placement;
    if (use_dynamic_backend &&
        (!repiu::engine::PlaceAotCodeCache(
             aot_image, &aot_placement) ||
         !aot_placement.placed))
    {
        logger->error("Failed to place requested AOT code cache: {}",
                      aot_placement.message);
        repiu::engine::ReleaseRelocatedImage(placement);
        return 1;
    }
    if (use_dynamic_backend)
    {
        logger->info("AOT cache base/bytes/entry: {}/{}/{}",
                     Hex32(aot_placement.base_address),
                     aot_placement.size,
                     Hex32(aot_placement.entry_address));
        logger->info("AOT plan jump tables/targets: {}/{}",
                     aot_plan.jump_table_count,
                     aot_plan.jump_table_target_count);
    }
    logger->flush();
    repiu::engine::MinimalExecutionAttempt attempt;
    const bool use_dos_console_hle =
        profile->hle_profile_id == "dos4gw_console_sample";
    const std::uint32_t execution_timeout_milliseconds =
        ReadExecutionTimeoutMilliseconds();
    const std::uint32_t stall_timeout_milliseconds =
        ReadStallTimeoutMilliseconds();
    if (execution_timeout_milliseconds ==
        repiu::runtime::kWaitForeverMilliseconds)
    {
        logger->info("guest execution timeout: disabled");
    }
    else
    {
        logger->info("guest execution timeout: {} ms",
                     execution_timeout_milliseconds);
    }
    if (stall_timeout_milliseconds ==
        repiu::runtime::kWaitForeverMilliseconds)
    {
        logger->info("guest stall timeout: disabled");
    }
    else
    {
        logger->info("guest stall timeout: {} ms",
                     stall_timeout_milliseconds);
    }
    // Task 709. How to report, registered before the guest starts.
    //
    // The shutdown arm that refuses to recover the guest thread ends in
    // `_Exit` and never returns `attempt` here, so on that arm this is the
    // only way the summary gets printed. Linux x64 takes it on every run.
    struct FinalReportContext
    {
        spdlog::logger* logger;
        std::string executable_name;
    };
    static FinalReportContext final_report_context{
        logger.get(), profile->executable_path.filename().string()};
    repiu::engine::SetFinalExecutionReport(
        [](const repiu::engine::MinimalExecutionAttempt& reported,
           void* user) {
            auto* const reporting =
                static_cast<FinalReportContext*>(user);
            if (reporting == nullptr || reporting->logger == nullptr)
            {
                return;
            }
            PrintExecutionAttempt(*reporting->logger, reported,
                                  reporting->executable_name);
        },
        &final_report_context);

    const bool attempted_execution = use_dynamic_backend
        ? repiu::engine::AttemptGuestStackAotExecution(
              placement,
              aot_placement,
              stack_plan,
              dos_file_system,
              linexe_runtime_module ? &*linexe_runtime_module : nullptr,
              glide_exports.empty() ? nullptr : &glide_exports,
              cd_chd_path ? &*cd_chd_path : nullptr,
              sound_rom_zip_path ? &*sound_rom_zip_path : nullptr,
              profile->enable_piu_jamma_board,
              profile->enable_piu10_isa_board,
              profile->enable_cat702,
              profile->parent_rom_set_id,
              profile->piu10_mp3_latency_ms,
              execution_backend,
              execution_timeout_milliseconds,
              stall_timeout_milliseconds,
              &attempt)
        : use_dos_console_hle
            ? repiu::engine::AttemptGuestStackHleExecution(
                  placement,
                  stack_plan,
                  dos_file_system,
                  execution_timeout_milliseconds,
                  stall_timeout_milliseconds,
                  &attempt)
            : repiu::engine::AttemptGuestStackTrapExecution(
                  placement,
                  stack_plan,
                  dos_file_system,
                  linexe_runtime_module ? &*linexe_runtime_module : nullptr,
                  glide_exports.empty() ? nullptr : &glide_exports,
                  cd_chd_path ? &*cd_chd_path : nullptr,
                  sound_rom_zip_path ? &*sound_rom_zip_path : nullptr,
                  profile->enable_piu_jamma_board,
                  profile->enable_piu10_isa_board,
                  profile->enable_cat702,
                  profile->parent_rom_set_id,
                  profile->piu10_mp3_latency_ms,
                  execution_timeout_milliseconds,
                  stall_timeout_milliseconds,
                  &attempt);
    if (!attempted_execution)
    {
        logger->error("Failed to attempt minimal original entry execution");
        repiu::engine::ReleaseAotCodeCache(&aot_placement);
        repiu::engine::ReleaseRelocatedImage(placement);
        return 1;
    }

    for (const auto& resident : glide_exports)
    {
        if (resident.ordinal == 95)
        {
            logger->info("[debug-ordinal-95] ordinal 95 name: {}", resident.name);
        }
    }

    // Through the same emit as the immediate-exit arm, so a run that already
    // reported on the way out does not print a second copy here.
    repiu::engine::EmitFinalExecutionReport(attempt);
    const auto glide_dispatch_stats = repiu::engine::
        ReadGlideGateDirectDispatchStats();
    logger->info(
        "Glide direct dispatch patched/verified/resolved-target/"
        "relinked-cache/entry/success/target-miss/terminal: "
        "{}/{}/{}/{}/{}/{}/{}/{}",
        glide_dispatch_stats.patched_gate_count,
        glide_dispatch_stats.verified_gate_count,
        glide_dispatch_stats.resolved_target_count,
        glide_dispatch_stats.relinked_cache_target_count,
        glide_dispatch_stats.entry_count,
        glide_dispatch_stats.success_count,
        glide_dispatch_stats.target_miss_count,
        glide_dispatch_stats.terminal_failure_count);
    // Task 507: skipped, on the same terms as `ReleaseRelocatedImage`
    // below, when the shutdown path could not stop the guest thread. A thread
    // left running keeps a claim on this memory -- a host call the AOT cache
    // made can return into it -- and freeing it under that thread is a
    // use-after-free the process is exiting anyway, not a leak worth avoiding.
    if (attempt.guest_thread_stopped)
    {
        repiu::engine::ReleaseAotCodeCache(&aot_placement);
    }
    if (attempt.exception_caught)
    {
        std::uint32_t target_address = attempt.seh_exception_address;
        if (attempt.aot_exception_mapping_valid)
        {
            target_address = attempt.aot_exception_guest_address;
        }
        repiu::runtime::RelocatedImageByteWindow window;
        repiu::runtime::BuildRelocatedImageByteWindow(
            relocated_image,
            target_address,
            16,
            16,
            &window);
        PrintByteWindow(*logger, window);
        if (window.valid)
        {
            repiu::hle::PrivilegedInstructionClassification classification;
            repiu::hle::ClassifyPrivilegedInstruction(
                window.bytes,
                window.focus_offset,
                &classification);
            PrintPrivilegedInstructionClassification(*logger, classification);
        }
    }
    if (attempt.last_single_step_snapshot.captured)
    {
        repiu::runtime::RelocatedImageByteWindow window;
        repiu::runtime::BuildRelocatedImageByteWindow(
            relocated_image,
            attempt.last_single_step_snapshot.eip,
            16,
            16,
            &window);
        PrintByteWindow(*logger, window);
    }
    if (!attempt.timed_out)
    {
        repiu::engine::ReleaseRelocatedImage(placement);
    }
    return 0;
}
