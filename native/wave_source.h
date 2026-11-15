#pragma once

#include "wave_pcm.h"
#include "ExamplesCommon/AudioFiles/AudioFiles.h"

class WaveSource final : public AudioFileBase {
public:
    explicit WaveSource(const std::filesystem::path& path) : AudioFileBase(path.filename().u8string()), pcm(path) {}
    int64_t getSampleCount() const noexcept override { return pcm.getSampleCount(); }
    double getSampleRate() const noexcept override { return pcm.getSampleRate(); }
    int getChannelCount() const noexcept override { return pcm.getChannelCount(); }
    bool merits64BitSamples() const noexcept override { return false; }
    bool readSamples(int64_t start, int64_t count, void* const buffers[], bool useDoubles) noexcept override
    {
        return pcm.readSamples(start, count, buffers, useDoubles);
    }
    bool saveToFile(const std::string&) override { return false; }

private:
    WavePcm pcm;
};
