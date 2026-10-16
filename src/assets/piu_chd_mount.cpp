#include "repiu/assets/piu_chd_mount.h"

#include "repiu/media/chd_cd_image.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <fstream>
#include <vector>

namespace repiu::assets
{
namespace
{

constexpr std::uint32_t kIsoSectorBytes = 2048;

std::uint32_t ReadLe32(const std::uint8_t* bytes)
{
    return static_cast<std::uint32_t>(bytes[0]) |
           (static_cast<std::uint32_t>(bytes[1]) << 8U) |
           (static_cast<std::uint32_t>(bytes[2]) << 16U) |
           (static_cast<std::uint32_t>(bytes[3]) << 24U);
}

bool ZipContainsRequiredPiu10EntriesImpl(const std::filesystem::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    if (!stream)
    {
        return false;
    }
    const std::vector<std::string> required = {
        "mk3_1.0_bios.u22", "mk3_1.1_bios.u22", "piu10.u8", "piu10.u9"};
    std::vector<bool> found(required.size(), false);
    std::array<char, 4096> buffer = {};
    std::string overlap;
    while (stream)
    {
        stream.read(buffer.data(), buffer.size());
        const std::streamsize count = stream.gcount();
        if (count <= 0)
        {
            break;
        }
        std::string chunk = overlap;
        chunk.append(buffer.data(), static_cast<std::size_t>(count));
        for (std::size_t index = 0; index < required.size(); ++index)
        {
            found[index] = found[index] ||
                           chunk.find(required[index]) != std::string::npos;
        }
        overlap = chunk.substr(chunk.size() > 64 ? chunk.size() - 64 : 0);
    }
    return std::all_of(found.begin(), found.end(), [](bool value) {
        return value;
    });
}

class IsoTrackReader
{
public:
    bool Open(const std::filesystem::path& path, std::string* message)
    {
        if (!image_.Open(path))
        {
            if (message != nullptr)
            {
                *message = image_.message();
            }
            return false;
        }
        std::uint32_t data_track_count = 0;
        for (const media::ChdCdTrack& track : image_.tracks())
        {
            if (!track.audio)
            {
                ++data_track_count;
                data_track_lba_ = track.data_start_lba;
                data_track_end_lba_ = track.end_lba;
                data_track_type_ = track.type;
            }
        }
        if (data_track_count == 0U)
        {
            if (message != nullptr)
            {
                *message = "CHD contains no data track";
            }
            return false;
        }
        switch (data_track_type_)
        {
            case media::ChdCdTrackType::kMode1:
            case media::ChdCdTrackType::kMode2Form1:
                user_data_offset_ = 0U;
                break;
            case media::ChdCdTrackType::kMode1Raw:
                user_data_offset_ = 16U;
                break;
            case media::ChdCdTrackType::kMode2Raw:
                user_data_offset_ = 24U;
                break;
            default:
                if (message != nullptr)
                {
                    *message =
                        "CHD data track format is unsupported for ISO9660";
                }
                return false;
        }
        return true;
    }

    bool ReadTrackSector(std::uint32_t relative_lba,
                         std::array<std::uint8_t, kIsoSectorBytes>* sector)
    {
        return ReadDiscSector(data_track_lba_ + relative_lba, sector);
    }

    bool ReadIsoSector(std::uint32_t extent_lba,
                       std::array<std::uint8_t, kIsoSectorBytes>* sector)
    {
        const std::int64_t disc_lba =
            static_cast<std::int64_t>(extent_lba) + extent_lba_bias_;
        if (disc_lba < 0 || disc_lba > UINT32_MAX)
        {
            return false;
        }
        return ReadDiscSector(static_cast<std::uint32_t>(disc_lba), sector);
    }

    bool ContainsExtent(std::uint32_t extent_lba,
                        std::uint32_t byte_count) const
    {
        if (byte_count == 0U)
        {
            return true;
        }
        const std::int64_t first =
            static_cast<std::int64_t>(extent_lba) + extent_lba_bias_;
        const std::int64_t sectors =
            (static_cast<std::int64_t>(byte_count) +
             kIsoSectorBytes - 1) / kIsoSectorBytes;
        return first >= data_track_lba_ &&
            first + sectors <= data_track_end_lba_;
    }

    bool ResolveExtentAddressing(std::uint32_t root_extent,
                                 std::uint32_t root_bytes)
    {
        std::array<std::uint8_t, kIsoSectorBytes> sector = {};
        for (std::uint32_t lba = data_track_lba_;
             lba < data_track_end_lba_; ++lba)
        {
            if (!ReadDiscSector(lba, &sector))
            {
                return false;
            }
            const std::uint8_t record_length = sector[0];
            if (record_length < 34U || sector[32] != 1U ||
                sector[33] != 0U || (sector[25] & 0x02U) == 0U ||
                ReadLe32(sector.data() + 2) != root_extent ||
                ReadLe32(sector.data() + 10) != root_bytes)
            {
                continue;
            }
            extent_lba_bias_ = static_cast<std::int64_t>(lba) -
                static_cast<std::int64_t>(root_extent);
            return true;
        }
        return false;
    }

    const std::string& Identity() const
    {
        return image_.identity();
    }

    std::uint32_t data_track_lba() const
    {
        return data_track_lba_;
    }

    std::int64_t extent_lba_bias() const
    {
        return extent_lba_bias_;
    }

private:
    bool ReadDiscSector(
        std::uint32_t disc_lba,
        std::array<std::uint8_t, kIsoSectorBytes>* sector)
    {
        if (sector == nullptr)
        {
            return false;
        }
        std::array<std::uint8_t, 2352> raw = {};
        if (!image_.ReadRawSector(disc_lba, raw.data(),
                                  static_cast<std::uint32_t>(raw.size())))
        {
            return false;
        }
        if (user_data_offset_ + sector->size() > raw.size())
        {
            return false;
        }
        std::memcpy(sector->data(), raw.data() + user_data_offset_,
                    sector->size());
        return true;
    }

    media::ChdCdImage image_;
    std::uint32_t data_track_lba_ = 0;
    std::uint32_t data_track_end_lba_ = 0;
    media::ChdCdTrackType data_track_type_ =
        media::ChdCdTrackType::kUnknown;
    std::uint32_t user_data_offset_ = 0;
    std::int64_t extent_lba_bias_ = 0;
};

struct IsoRecord
{
    std::uint32_t extent = 0;
    std::uint32_t bytes = 0;
    bool directory = false;
    std::string name;
};

std::string NormalizeIsoName(const std::uint8_t* bytes, std::size_t length)
{
    std::string name(reinterpret_cast<const char*>(bytes), length);
    const std::size_t version = name.find(';');
    if (version != std::string::npos)
    {
        name.resize(version);
    }
    while (!name.empty() && name.back() == '.')
    {
        name.pop_back();
    }
    for (char& value : name)
    {
        if (value == '/' || value == '\\' || value == ':')
        {
            value = '_';
        }
    }
    return name;
}

bool ReadExtent(IsoTrackReader* reader,
                std::uint32_t extent,
                std::uint32_t byte_count,
                std::vector<std::uint8_t>* bytes)
{
    if (reader == nullptr || bytes == nullptr)
    {
        return false;
    }
    bytes->assign(byte_count, 0);
    std::array<std::uint8_t, kIsoSectorBytes> sector = {};
    for (std::uint32_t offset = 0; offset < byte_count;
         offset += kIsoSectorBytes)
    {
        if (!reader->ReadIsoSector(extent + offset / kIsoSectorBytes,
                                   &sector))
        {
            return false;
        }
        const std::uint32_t copy = std::min<std::uint32_t>(
            kIsoSectorBytes, byte_count - offset);
        std::memcpy(bytes->data() + offset, sector.data(), copy);
    }
    return true;
}

bool ParseDirectory(IsoTrackReader* reader,
                    std::uint32_t extent,
                    std::uint32_t byte_count,
                    std::vector<IsoRecord>* records)
{
    std::vector<std::uint8_t> bytes;
    if (records == nullptr || !ReadExtent(reader, extent, byte_count, &bytes))
    {
        return false;
    }
    for (std::size_t offset = 0; offset < bytes.size();)
    {
        const std::uint8_t length = bytes[offset];
        if (length == 0)
        {
            offset = ((offset / kIsoSectorBytes) + 1U) * kIsoSectorBytes;
            continue;
        }
        if (offset + length > bytes.size() || length < 34)
        {
            return false;
        }
        const std::uint8_t name_length = bytes[offset + 32];
        if (33U + name_length > length)
        {
            return false;
        }
        const std::uint8_t* name = bytes.data() + offset + 33;
        if (!(name_length == 1 && (name[0] == 0 || name[0] == 1)))
        {
            records->push_back(IsoRecord{
                ReadLe32(bytes.data() + offset + 2),
                ReadLe32(bytes.data() + offset + 10),
                (bytes[offset + 25] & 0x02U) != 0,
                NormalizeIsoName(name, name_length),
            });
        }
        offset += length;
    }
    return true;
}

// Issue #72. What an unpack will write: the files ExtractTree extracts,
// counted from the directory records alone, so progress can be a fraction.
bool CountTree(IsoTrackReader* reader,
               const IsoRecord& directory,
               std::uint32_t depth,
               PiuChdUnpackProgress* totals)
{
    if (depth > 32)
    {
        return false;
    }
    std::vector<IsoRecord> records;
    if (!ParseDirectory(reader, directory.extent, directory.bytes, &records))
    {
        return false;
    }
    for (const IsoRecord& record : records)
    {
        if (record.name.empty() || record.name == "." || record.name == "..")
        {
            continue;
        }
        if (record.directory)
        {
            if (!CountTree(reader, record, depth + 1U, totals))
            {
                return false;
            }
        }
        else if (reader->ContainsExtent(record.extent, record.bytes))
        {
            ++totals->total_files;
            totals->total_bytes += record.bytes;
        }
    }
    return true;
}

// Issue #72. Reports an unpack's progress to the observer about every 16 MB.
struct UnpackReporter
{
    static constexpr std::uint64_t kStepBytes = 16U * 1024U * 1024U;

    const PiuChdUnpackObserver* observer = nullptr;
    PiuChdUnpackProgress progress;
    std::uint64_t next_report = kStepBytes;

    void FileWritten(const std::uint64_t bytes)
    {
        ++progress.files;
        progress.bytes += bytes;
        if (observer != nullptr && *observer && progress.bytes >= next_report)
        {
            (*observer)(progress);
            while (next_report <= progress.bytes)
            {
                next_report += kStepBytes;
            }
        }
    }
};

bool ExtractTree(IsoTrackReader* reader,
                 const IsoRecord& directory,
                 const std::filesystem::path& output,
                 PiuChdMountResult* result,
                 std::uint32_t depth,
                 UnpackReporter* reporter)
{
    if (depth > 32 || !std::filesystem::create_directories(output) &&
                          !std::filesystem::is_directory(output))
    {
        return false;
    }
    std::vector<IsoRecord> records;
    if (!ParseDirectory(reader, directory.extent, directory.bytes, &records))
    {
        result->message = "failed to parse ISO9660 directory extent " +
            std::to_string(directory.extent) + " (" +
            std::to_string(directory.bytes) + " bytes)";
        return false;
    }
    for (const IsoRecord& record : records)
    {
        if (record.name.empty() || record.name == "." || record.name == "..")
        {
            continue;
        }
        const std::filesystem::path target = output / record.name;
        if (record.directory)
        {
            if (!ExtractTree(reader, record, target, result, depth + 1U, reporter))
            {
                return false;
            }
            continue;
        }
        if (!reader->ContainsExtent(record.extent, record.bytes))
        {
            ++result->skipped_external_extent_file_count;
            continue;
        }
        std::vector<std::uint8_t> bytes;
        if (!ReadExtent(reader, record.extent, record.bytes, &bytes))
        {
            result->message = "failed to read ISO9660 file " +
                record.name + " at extent " +
                std::to_string(record.extent);
            return false;
        }
        std::ofstream stream(target, std::ios::binary | std::ios::trunc);
        if (!stream)
        {
            return false;
        }
        stream.write(reinterpret_cast<const char*>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()));
        if (!stream)
        {
            return false;
        }
        ++result->extracted_file_count;
        result->extracted_byte_count += bytes.size();
        reporter->FileWritten(bytes.size());
    }
    return true;
}

}  // namespace

bool PiuRomZipHasRequiredEntries(const std::filesystem::path& rom_zip_path)
{
    return ZipContainsRequiredPiu10EntriesImpl(rom_zip_path);
}

bool PreparePiuChdMount(std::string_view rom_set_id,
                        const std::filesystem::path& roms_root,
                        const std::filesystem::path& cache_root,
                        PiuChdMountResult* result,
                        const PiuChdUnpackObserver& observer)
{
    if (result == nullptr)
    {
        return false;
    }
    *result = PiuChdMountResult{};
    if (rom_set_id.empty() ||
        !std::all_of(rom_set_id.begin(), rom_set_id.end(), [](char value) {
            return std::isalnum(static_cast<unsigned char>(value)) != 0 ||
                value == '_' || value == '-';
        }))
    {
        result->message = "invalid PIU ROM-set id";
        return true;
    }

    result->rom_set_id = std::string(rom_set_id);
    result->rom_zip_path = roms_root / (result->rom_set_id + ".zip");
    const std::filesystem::path chd_directory =
        roms_root / result->rom_set_id;
    if (!PiuRomZipHasRequiredEntries(result->rom_zip_path))
    {
        result->message = result->rom_set_id +
            ".zip is missing required PIU10 ROM entries";
        return true;
    }
    std::error_code directory_error;
    const std::filesystem::directory_iterator directory(
        chd_directory, directory_error);
    if (directory_error)
    {
        result->message = result->rom_set_id +
            " CHD directory not found";
        return true;
    }
    for (const auto& entry : directory)
    {
        if (entry.is_regular_file() && entry.path().extension() == ".chd")
        {
            if (!result->chd_path.empty())
            {
                result->message = "multiple " + result->rom_set_id +
                    " CHD files found";
                return true;
            }
            result->chd_path = entry.path();
        }
    }
    if (result->chd_path.empty())
    {
        result->message = result->rom_set_id + " CHD file not found";
        return true;
    }

    IsoTrackReader reader;
    if (!reader.Open(result->chd_path, &result->message))
    {
        return true;
    }
    result->data_track_lba = reader.data_track_lba();
    result->mount_root = cache_root / result->rom_set_id;
    result->executable_path = result->mount_root / "PIU" / "PIU.EXE";
    const std::filesystem::path marker = result->mount_root / ".chd-identity";
    const std::string identity = reader.Identity();
    std::ifstream marker_input(marker);
    std::string cached_identity;
    std::getline(marker_input, cached_identity);
    marker_input >> result->iso_extent_lba_bias;
    if (cached_identity == identity &&
        std::filesystem::is_regular_file(result->executable_path))
    {
        result->valid = true;
        result->mounted = true;
        result->cache_reused = true;
        result->message = result->rom_set_id + " CHD mount cache reused";
        return true;
    }

    std::error_code remove_error;
    std::filesystem::remove_all(result->mount_root, remove_error);
    std::filesystem::create_directories(result->mount_root);
    std::array<std::uint8_t, kIsoSectorBytes> pvd = {};
    if (!reader.ReadTrackSector(16, &pvd) || pvd[0] != 1 ||
        std::memcmp(pvd.data() + 1, "CD001", 5) != 0)
    {
        result->message =
            "CHD data track does not contain an ISO9660 primary volume";
        return true;
    }
    const std::uint8_t* root = pvd.data() + 156;
    if (root[0] < 34)
    {
        result->message = "ISO9660 root directory record is invalid";
        return true;
    }
    const IsoRecord root_record{
        ReadLe32(root + 2), ReadLe32(root + 10), true, {}};
    if (!reader.ResolveExtentAddressing(root_record.extent,
                                        root_record.bytes))
    {
        result->message =
            "failed to resolve ISO9660 extent addressing";
        return true;
    }
    result->iso_extent_lba_bias = reader.extent_lba_bias();
    UnpackReporter reporter;
    reporter.observer = &observer;
    if (observer)
    {
        // A count that fails only costs the fraction; the unpack decides.
        CountTree(&reader, root_record, 0, &reporter.progress);
        PiuChdUnpackProgress starting = reporter.progress;
        starting.starting = true;
        observer(starting);
    }
    if (!ExtractTree(&reader, root_record, result->mount_root, result, 0, &reporter))
    {
        if (result->message.empty())
        {
            result->message =
                "failed to materialize ISO9660 mount view";
        }
        return true;
    }
    std::ofstream marker_output(marker, std::ios::trunc);
    marker_output << identity << '\n'
                  << result->iso_extent_lba_bias << '\n';
    if (!marker_output ||
        !std::filesystem::is_regular_file(result->executable_path))
    {
        result->message = "mounted ISO9660 does not contain PIU/PIU.EXE";
        return true;
    }
    result->valid = true;
    result->mounted = true;
    result->message = result->rom_set_id +
        " CHD mounted as ISO9660 read-only cache";
    return true;
}
}  // namespace repiu::assets
