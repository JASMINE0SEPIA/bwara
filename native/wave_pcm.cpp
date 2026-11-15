#include "wave_pcm.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <stdexcept>

namespace {
uint32_t little(const unsigned char* bytes, unsigned count)
{
    uint32_t value = 0;
    for (unsigned i = 0; i < count; ++i)
        value |= static_cast<uint32_t>(bytes[i]) << (8 * i);
    return value;
}

void read(std::istream& stream, unsigned char* bytes, std::streamsize count)
{
    if (!stream.read(reinterpret_cast<char*>(bytes), count))
        throw std::runtime_error("Truncated WAV file.");
}

float decode(const unsigned char* bytes, unsigned count, bool floatingPoint)
{
    const uint32_t value = little(bytes, count);
    if (floatingPoint) {
        float sample;
        std::memcpy(&sample, &value, sizeof sample);
        return sample;
    }
    if (count == 1)
        return (static_cast<int>(value) - 128) / 128.0f;
    const auto midpoint = int64_t{1} << (8 * count - 1);
    const auto signedValue = value >= midpoint ? int64_t{value} - 2 * midpoint : int64_t{value};
    return static_cast<float>(signedValue) / static_cast<float>(midpoint);
}
}

WavePcm::WavePcm(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    const auto size = std::filesystem::file_size(path);
    if (!input || size < 12 || size > 256u * 1024u * 1024u)
        throw std::runtime_error("Expected a local WAV file up to 256 MiB.");
    std::array<unsigned char, 12> head{};
    read(input, head.data(), head.size());
    if (std::memcmp(head.data(), "RIFF", 4) || std::memcmp(head.data() + 8, "WAVE", 4))
        throw std::runtime_error("Expected a RIFF WAVE file.");

    const auto end = uint64_t{8} + little(head.data() + 4, 4);
    if (end > size)
        throw std::runtime_error("Truncated WAV file.");
    unsigned encoding = 0;
    unsigned bits = 0;
    unsigned stride = 0;
    uint64_t dataStart = 0;
    uint32_t dataSize = 0;
    for (uint64_t offset = 12; offset + 8 <= end;) {
        std::array<unsigned char, 8> chunk{};
        input.seekg(offset);
        read(input, chunk.data(), chunk.size());
        const auto length = little(chunk.data() + 4, 4);
        const auto start = offset + 8;
        if (start + length + (length & 1u) > end)
            throw std::runtime_error("Invalid WAV chunk length.");
        if (!std::memcmp(chunk.data(), "fmt ", 4) && !encoding) {
            std::array<unsigned char, 40> fmt{};
            if (length < 16)
                throw std::runtime_error("Invalid WAV format chunk.");
            read(input, fmt.data(), std::min<uint32_t>(length, static_cast<uint32_t>(fmt.size())));
            encoding = little(fmt.data(), 2);
            channels = static_cast<int>(little(fmt.data() + 2, 2));
            rate = little(fmt.data() + 4, 4);
            stride = little(fmt.data() + 12, 2);
            bits = little(fmt.data() + 14, 2);
            if (encoding == 0xfffe && length >= 40 && little(fmt.data() + 16, 2) >= 22) {
                constexpr unsigned char waveGuidTail[]{0, 0, 0, 0, 0x10, 0, 0x80, 0, 0, 0xaa, 0, 0x38, 0x9b, 0x71};
                if (!std::memcmp(fmt.data() + 26, waveGuidTail, sizeof waveGuidTail))
                    encoding = little(fmt.data() + 24, 2);
            }
        } else if (!std::memcmp(chunk.data(), "data", 4) && !dataStart) {
            dataStart = start;
            dataSize = length;
        }
        offset = start + length + (length & 1u);
        if (encoding && dataStart)
            break;
    }
    const auto pcm = encoding == 1 && (bits == 8 || bits == 16 || bits == 24 || bits == 32);
    const auto floatingPoint = encoding == 3 && bits == 32;
    if ((!pcm && !floatingPoint) || channels < 1 || channels > 2 || rate < 8000 || rate > 384000 ||
        stride != bits / 8 * channels || !dataSize || dataSize % stride)
        throw std::runtime_error("Expected a mono or stereo PCM or float WAV.");

    frames = dataSize / stride;
    samples.resize(static_cast<std::size_t>(frames) * channels);
    input.seekg(dataStart);
    std::array<unsigned char, 4096 * 8> bytes{};
    for (int64_t position = 0; position < frames;) {
        const auto count = std::min<int64_t>(4096, frames - position);
        read(input, bytes.data(), count * stride);
        for (int64_t i = 0; i < count * channels; ++i) {
            const auto sample = decode(bytes.data() + i * (bits / 8), bits / 8, floatingPoint);
            if (!std::isfinite(sample))
                throw std::runtime_error("The WAV contains invalid samples.");
            samples[(i % channels) * frames + position + i / channels] = sample;
        }
        position += count;
    }
}

bool WavePcm::readSamples(int64_t start, int64_t count, void* const buffers[], bool useDoubles) const noexcept
{
    if (count < 0 || !buffers)
        return false;
    for (int channel = 0; channel < channels; ++channel) {
        if (!buffers[channel])
            return false;
        for (int64_t index = 0; index < count; ++index) {
            const bool valid = start >= -index && start < frames - index;
            // ARA 可能读到素材两端之外。这些地方补静音，别让数组替歌手尖叫。
            const auto sample = valid ? channelData(channel)[start + index] : 0.0f;
            if (useDoubles)
                static_cast<double*>(buffers[channel])[index] = sample;
            else
                static_cast<float*>(buffers[channel])[index] = sample;
        }
    }
    return true;
}

