#ifndef REPIU_ENGINE_YMZ280B_AUDIO_OUT_H_
#define REPIU_ENGINE_YMZ280B_AUDIO_OUT_H_

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace repiu::engine
{

// SDL3 playback backend for the PIU10 board's YMZ280B.
//
// Owns the platform-neutral chip core plus the worker thread that pushes
// generated PCM into an SDL audio stream, and serializes guest register access
// against generation. The guest thread calls the register methods from the port
// I/O trap; the worker thread calls the core's generator. Everything the guest
// can observe goes through one mutex.
//
// The chip runs at a fixed 88200 Hz; conversion to the device rate is left to
// SDL_AudioStream.
class Ymz280bAudioOut
{
public:
    Ymz280bAudioOut();
    ~Ymz280bAudioOut();
    Ymz280bAudioOut(const Ymz280bAudioOut&) = delete;
    Ymz280bAudioOut& operator=(const Ymz280bAudioOut&) = delete;

    // Loads the sample ROM from a MAME ROM ZIP and opens the audio device.
    // Returns false and leaves the object closed on any failure; the caller must
    // treat that as "no sound", never as a fatal execution error.
    bool Open(const std::filesystem::path& rom_zip_path);
    void Close();

    // Issue #64. On one thread (RunsHostOnOneThread) there is no worker, and
    // the host turn calls this to fill the stream. A no-op while a worker runs.
    void Pump();

    bool available() const;
    const std::string& message() const;

    // PIU10 port interface, in chip register terms.
    void WriteRegisterSelect(std::uint8_t value);
    void WriteRegisterData(std::uint8_t value);
    std::uint8_t ReadExternalMemory();
    std::uint8_t ReadStatus();

    // Verification counters. Cheap enough to read from a telemetry path.
    std::uint64_t register_write_count() const;
    std::uint64_t key_on_count() const;
    std::uint64_t non_silent_sample_count() const;
    std::int32_t peak_amplitude() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace repiu::engine

#endif  // REPIU_ENGINE_YMZ280B_AUDIO_OUT_H_
