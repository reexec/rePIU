#ifndef REPIU_ASSETS_PIU_CHD_MOUNT_H_
#define REPIU_ASSETS_PIU_CHD_MOUNT_H_

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>

namespace repiu::assets
{

struct PiuChdMountResult
{
    bool valid = false;
    bool mounted = false;
    bool cache_reused = false;
    std::string rom_set_id;
    std::filesystem::path rom_zip_path;
    std::filesystem::path chd_path;
    std::filesystem::path mount_root;
    std::filesystem::path executable_path;
    std::uint32_t data_track_lba = 0;
    std::int64_t iso_extent_lba_bias = 0;
    std::uint32_t extracted_file_count = 0;
    std::uint32_t skipped_external_extent_file_count = 0;
    std::uint64_t extracted_byte_count = 0;
    std::string message;
};

// Task 500: the ROM-zip half of the mount's own precondition, exposed so the
// launcher can report availability without extracting a disc. Scans for the
// PIU10 entry names rather than parsing the archive, which is what the mount
// has always done.
// Issue #72. How far an unpack has come. Unpacking the game's files from the
// CHD takes a minute or more on a first run, and on every run in a browser,
// where nothing else is printed meanwhile.
struct PiuChdUnpackProgress
{
    // The first report, before any file is written.
    bool starting = false;
    std::uint32_t files = 0;
    std::uint64_t bytes = 0;
    // What the whole unpack writes, counted from the directories first.
    std::uint32_t total_files = 0;
    std::uint64_t total_bytes = 0;
};

// Called when an unpack starts and then about every 16 MB. Not called when
// the cache is reused.
using PiuChdUnpackObserver = std::function<void(const PiuChdUnpackProgress&)>;

[[nodiscard]] bool PiuRomZipHasRequiredEntries(
    const std::filesystem::path& rom_zip_path);

bool PreparePiuChdMount(std::string_view rom_set_id,
                        const std::filesystem::path& roms_root,
                        const std::filesystem::path& cache_root,
                        PiuChdMountResult* result,
                        const PiuChdUnpackObserver& observer = {});

}  // namespace repiu::assets

#endif  // REPIU_ASSETS_PIU_CHD_MOUNT_H_
