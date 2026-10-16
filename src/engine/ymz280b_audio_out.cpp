#include "repiu/engine/ymz280b_audio_out.h"
#include "repiu/engine/host_audio_stream.h"

#include "repiu/sound/ymz280b_device.h"
#include "repiu/sound/ymz280b_sample_rom.h"
#include "repiu/runtime/execution_model.h"

#include <SDL3/SDL.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <thread>
#include <vector>

namespace repiu::engine
{
namespace
{

// One generated block. Short enough that a register write lands within a couple
// of milliseconds, long enough that the worker is not spinning.
constexpr std::uint32_t kFramesPerBlock = 512U;

// Queue ceiling, about 40 ms at 88200 Hz. The guest runs in real time, so writes
// cannot apply retroactively to blocks already queued; this bounds that error.
constexpr std::uint32_t kMaxQueuedFrames = 3528U;

constexpr int kBytesPerFrame = 2 * static_cast<int>(sizeof(std::int16_t));

float ReadVolumeScale()
{
    const char* raw = std::getenv("REPIU_YMZ_VOLUME");
    if (raw == nullptr || *raw == '\0')
    {
        return 1.0F;
    }
    const double parsed = std::strtod(raw, nullptr);
    if (!(parsed > 0.0) || parsed > 8.0)
    {
        return 1.0F;
    }
    return static_cast<float>(parsed);
}

// Minimal RIFF/WAVE writer used only for verification.
//
// The RIFF size fields are refreshed about once a second rather than only on
// close, because the supervisor terminates the loader process when its run
// timeout expires and Close() never gets to run. Without the periodic refresh
// every captured file ends up advertising a zero-length data chunk and no
// player will open it.
class WavCapture
{
public:
    bool Open(const char* path, std::uint32_t sample_rate)
    {
        stream_.open(path, std::ios::binary | std::ios::trunc);
        if (!stream_)
        {
            return false;
        }
        sample_rate_ = sample_rate;
        header_refresh_interval_bytes_ =
            sample_rate * 2U * static_cast<std::uint32_t>(sizeof(std::int16_t));
        WriteHeader(0);
        return true;
    }

    bool active() const { return stream_.is_open(); }

    void Append(const std::int16_t* samples, std::size_t count)
    {
        if (!stream_.is_open())
        {
            return;
        }
        stream_.write(reinterpret_cast<const char*>(samples),
                      static_cast<std::streamsize>(count * sizeof(std::int16_t)));
        data_bytes_ += static_cast<std::uint32_t>(count * sizeof(std::int16_t));
        if (data_bytes_ - last_header_bytes_ >= header_refresh_interval_bytes_)
        {
            RefreshHeader();
        }
    }

    void Close()
    {
        if (!stream_.is_open())
        {
            return;
        }
        RefreshHeader();
        stream_.close();
    }

private:
    void WriteU32(std::uint32_t value)
    {
        const std::uint8_t bytes[4] = {
            static_cast<std::uint8_t>(value & 0xFFU),
            static_cast<std::uint8_t>((value >> 8) & 0xFFU),
            static_cast<std::uint8_t>((value >> 16) & 0xFFU),
            static_cast<std::uint8_t>((value >> 24) & 0xFFU)};
        stream_.write(reinterpret_cast<const char*>(bytes), 4);
    }

    void WriteU16(std::uint16_t value)
    {
        const std::uint8_t bytes[2] = {
            static_cast<std::uint8_t>(value & 0xFFU),
            static_cast<std::uint8_t>((value >> 8) & 0xFFU)};
        stream_.write(reinterpret_cast<const char*>(bytes), 2);
    }

    void RefreshHeader()
    {
        stream_.seekp(0, std::ios::beg);
        WriteHeader(data_bytes_);
        stream_.seekp(0, std::ios::end);
        stream_.flush();
        last_header_bytes_ = data_bytes_;
    }

    void WriteHeader(std::uint32_t data_bytes)
    {
        stream_.write("RIFF", 4);
        WriteU32(36U + data_bytes);
        stream_.write("WAVEfmt ", 8);
        WriteU32(16U);
        WriteU16(1U);                                  // PCM
        WriteU16(2U);                                  // stereo
        WriteU32(sample_rate_);
        WriteU32(sample_rate_ * 2U * sizeof(std::int16_t));
        WriteU16(static_cast<std::uint16_t>(2U * sizeof(std::int16_t)));
        WriteU16(16U);                                 // bits per sample
        stream_.write("data", 4);
        WriteU32(data_bytes);
    }

    std::ofstream stream_;
    std::uint32_t sample_rate_ = 0;
    std::uint32_t data_bytes_ = 0;
    std::uint32_t last_header_bytes_ = 0;
    std::uint32_t header_refresh_interval_bytes_ = 0;
};

}  // namespace

struct Ymz280bAudioOut::Impl
{
    sound::Ymz280bDevice device;
    std::mutex mutex;

    SDL_AudioStream* stream = nullptr;
    std::thread worker;
    std::atomic<bool> shutdown{false};
    std::atomic<bool> ready{false};

    float volume_scale = 1.0F;
    WavCapture capture;
    std::string message;

    // Snapshots so the accessors never need the mutex on a hot telemetry path.
    std::atomic<std::uint64_t> register_writes{0};
    std::atomic<std::uint64_t> key_ons{0};
    std::atomic<std::uint64_t> non_silent_samples{0};
    std::atomic<std::int32_t> peak{0};

    std::vector<std::int16_t> block;

    std::chrono::milliseconds Step();
};

// One block into the stream: what the worker's loop did once. The answer is how
// long to wait before the next step, zero for "step again". Issue #64: the
// worker thread repeats it, or on one thread the host turn does (Pump).
std::chrono::milliseconds Ymz280bAudioOut::Impl::Step()
{
    const int queued = SDL_GetAudioStreamQueued(stream);
    if (queued >= static_cast<int>(kMaxQueuedFrames) * kBytesPerFrame)
    {
        return std::chrono::milliseconds(2);
    }

    std::vector<sound::Ymz280bDevice::KeyOnEvent> key_on_events;
    {
        std::lock_guard<std::mutex> lock(mutex);
        device.Generate(block.data(), kFramesPerBlock);
        key_on_events = device.TakeKeyOnEvents();
        non_silent_samples.store(
            device.non_silent_sample_count());
        peak.store(device.peak_amplitude());
    }

    for (const auto& event : key_on_events)
    {
        std::fprintf(stderr,
                     "[repiu-ymz] keyon voice=%d mode=%s start=0x%06X "
                     "stop=0x%06X rate=%uHz level=%d pan=%d loop=%s\n",
                     event.voice,
                     event.mode == sound::Ymz280bDevice::kModeAdpcm
                         ? "ADPCM"
                         : (event.mode == sound::Ymz280bDevice::kModePcm8
                                ? "PCM8"
                                : "PCM16"),
                     event.start_byte_address,
                     event.stop_byte_address,
                     event.sample_rate_hz,
                     event.level,
                     event.pan,
                     event.looping ? "yes" : "no");
    }

    if (volume_scale != 1.0F)
    {
        for (std::int16_t& sample : block)
        {
            const float scaled =
                static_cast<float>(sample) * volume_scale;
            sample = static_cast<std::int16_t>(
                scaled > 32767.0F ? 32767.0F
                                  : (scaled < -32768.0F ? -32768.0F
                                                        : scaled));
        }
    }

    capture.Append(block.data(), block.size());

    if (!SDL_PutAudioStreamData(
            stream, block.data(),
            static_cast<int>(block.size() * sizeof(std::int16_t))))
    {
        std::fprintf(stderr, "[repiu-ymz] SDL queue failed: %s\n",
                     SDL_GetError());
        return std::chrono::milliseconds(5);
    }
    return std::chrono::milliseconds(0);
}

Ymz280bAudioOut::Ymz280bAudioOut() : impl_(std::make_unique<Impl>()) {}
Ymz280bAudioOut::~Ymz280bAudioOut() { Close(); }

bool Ymz280bAudioOut::Open(const std::filesystem::path& rom_zip_path)
{
    Close();

    const sound::Ymz280bSampleRom rom =
        sound::LoadPiu10SampleRom(rom_zip_path);
    if (!rom.valid)
    {
        impl_->message = "YMZ280B sample ROM unavailable: " + rom.message;
        std::fprintf(stderr, "[repiu-ymz] %s\n", impl_->message.c_str());
        return false;
    }
    std::fprintf(stderr, "[repiu-ymz] %s\n", rom.message.c_str());
    if (!rom.crc32_matches_reference)
    {
        std::fprintf(stderr,
                     "[repiu-ymz] warning: sample ROM checksum differs from the "
                     "MAME pumpit1 reference; sounds may be wrong\n");
    }

    impl_->device.Initialize(rom.data);
    const std::uint32_t sample_rate = impl_->device.output_sample_rate();

    // Issue #81: a device natively, the page's mixer in a browser.
    const SDL_AudioSpec format{SDL_AUDIO_S16LE, 2, static_cast<int>(sample_rate)};
    std::string failure;
    impl_->stream = OpenHostAudioStream(format, &failure);
    if (impl_->stream == nullptr)
    {
        impl_->message = "SDL YMZ280B " + failure;
        std::fprintf(stderr, "[repiu-ymz] %s\n", impl_->message.c_str());
        return false;
    }

    impl_->volume_scale = ReadVolumeScale();
    const char* capture_path = std::getenv("REPIU_YMZ_WAV_PATH");
    if (capture_path != nullptr && *capture_path != '\0' &&
        impl_->capture.Open(capture_path, sample_rate))
    {
        std::fprintf(stderr, "[repiu-ymz] capturing generated PCM to %s\n",
                     capture_path);
    }

    impl_->shutdown = false;
    impl_->block.assign(static_cast<std::size_t>(kFramesPerBlock) * 2U, 0);
    // Issue #64: on one thread the host turn pumps the stream instead.
    if (!runtime::execution_model::RunsHostOnOneThread())
    {
        impl_->worker = std::thread([this]() {
            while (!impl_->shutdown.load())
            {
                const std::chrono::milliseconds wait = impl_->Step();
                if (wait.count() != 0)
                {
                    std::this_thread::sleep_for(wait);
                }
            }
        });
    }

    SDL_ResumeAudioStreamDevice(impl_->stream);
    impl_->ready = true;
    impl_->message = "YMZ280B ready through SDL3 at " +
                     std::to_string(sample_rate) + " Hz";
    std::fprintf(stderr, "[repiu-ymz] %s\n", impl_->message.c_str());
    return true;
}

void Ymz280bAudioOut::Close()
{
    impl_->ready = false;
    impl_->shutdown = true;
    if (impl_->worker.joinable())
    {
        impl_->worker.join();
    }
    CloseHostAudioStream(impl_->stream);
    impl_->stream = nullptr;
    impl_->capture.Close();
}

// Issue #64. A bounded number of blocks, so one turn cannot run long; the
// queue ceiling is reached in a few.
void Ymz280bAudioOut::Pump()
{
    constexpr int kStepsPerPump = 16;
    if (!impl_->ready.load() || impl_->worker.joinable())
    {
        return;
    }
    for (int step = 0; step < kStepsPerPump; ++step)
    {
        if (impl_->Step().count() != 0)
        {
            return;
        }
    }
}

bool Ymz280bAudioOut::available() const { return impl_->ready.load(); }

const std::string& Ymz280bAudioOut::message() const { return impl_->message; }

void Ymz280bAudioOut::WriteRegisterSelect(std::uint8_t value)
{
    if (!impl_->ready.load())
    {
        return;
    }
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->device.WriteRegisterSelect(value);
}

void Ymz280bAudioOut::WriteRegisterData(std::uint8_t value)
{
    if (!impl_->ready.load())
    {
        return;
    }
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->device.WriteRegisterData(value);
    impl_->register_writes.store(impl_->device.register_write_count());
    impl_->key_ons.store(impl_->device.key_on_count());
}

std::uint8_t Ymz280bAudioOut::ReadExternalMemory()
{
    if (!impl_->ready.load())
    {
        return 0xFFU;
    }
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->device.ReadExternalMemory();
}

std::uint8_t Ymz280bAudioOut::ReadStatus()
{
    if (!impl_->ready.load())
    {
        return 0x00U;
    }
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->device.ReadStatus();
}

std::uint64_t Ymz280bAudioOut::register_write_count() const
{
    return impl_->register_writes.load();
}

std::uint64_t Ymz280bAudioOut::key_on_count() const
{
    return impl_->key_ons.load();
}

std::uint64_t Ymz280bAudioOut::non_silent_sample_count() const
{
    return impl_->non_silent_samples.load();
}

std::int32_t Ymz280bAudioOut::peak_amplitude() const
{
    return impl_->peak.load();
}

}  // namespace repiu::engine
