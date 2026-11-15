#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

class WavePcm {
public:
    explicit WavePcm(const std::filesystem::path& path);
    int64_t getSampleCount() const noexcept { return frames; }
    double getSampleRate() const noexcept { return rate; }
    int getChannelCount() const noexcept { return channels; }
    // 解码后按声道存放；ARA 和引擎读同一种数据，不再各自搬一次家。
    const float* channelData(unsigned channel) const noexcept { return samples.data() + channel * frames; }
    bool readSamples(int64_t start, int64_t count, void* const buffers[], bool useDoubles) const noexcept;

private:
    std::vector<float> samples;
    int64_t frames = 0;
    unsigned rate = 0;
    int channels = 0;
};
