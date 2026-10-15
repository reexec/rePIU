#ifndef REPIU_ENGINE_CD_AUDIO_WAVE_OUT_H_
#define REPIU_ENGINE_CD_AUDIO_WAVE_OUT_H_

#include "repiu/engine/cd_audio_position_census.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace repiu::engine
{

class CdAudioWaveOut
{
public:
    CdAudioWaveOut();
    ~CdAudioWaveOut();
    CdAudioWaveOut(const CdAudioWaveOut&) = delete;
    CdAudioWaveOut& operator=(const CdAudioWaveOut&) = delete;

    bool Open(const std::filesystem::path& chd_path);
    // start_lba is a logical (Red Book) LBA, matching what the TOC reports.
    bool Play(std::uint32_t start_lba, std::uint32_t frame_count);
    // MSCDEX 85h: stops playback but remembers where it stopped so that a
    // following Resume continues from the same frame.
    void Stop();
    bool Resume();
    // MSCDEX 83h: park the head at a logical LBA without producing audio.
    bool Seek(std::uint32_t lba);
    void Close();
    // Issue #64. On one thread (RunsHostOnOneThread) there is no worker, and
    // the host turn calls this to fill the stream. A no-op while a worker runs.
    void Pump();
    bool playing() const;
    bool paused() const;
    // Position of the audio actually reaching the device, not the decode
    // cursor: queued-but-unplayed frames are subtracted.
    std::uint32_t current_lba() const;
    std::uint32_t last_play_start_lba() const;
    std::uint32_t last_play_end_lba() const;
    // Task 421: everything `current_lba()` is derived from, read together so a
    // wrong position can be attributed instead of guessed at. Safe to call from
    // another thread; every field is an atomic load.
    void FillPositionSample(CdAudioPositionEntry* entry) const;
    const std::string& message() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace repiu::engine

#endif  // REPIU_ENGINE_CD_AUDIO_WAVE_OUT_H_
