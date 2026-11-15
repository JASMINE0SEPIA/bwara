#include "wave_pcm.h"
#include "revision_file.h"

#include <windows.h>
#include <atomic>
#include <cstring>
#include <fstream>
#include <future>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {
void check(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}

template<class F> void rejects(F action)
{
    bool rejected = false;
    try { action(); } catch (const std::exception&) { rejected = true; }
    check(rejected, "Invalid input unexpectedly succeeded.");
}

struct Fixture {
    fs::path directory = fs::temp_directory_path() /
        (L"bwara-core-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    Fixture() { check(fs::create_directory(directory), "Test directory already exists."); }
    ~Fixture()
    {
        // 只删本次新建目录里的测试文件，不递归触碰任何已有目录。
        std::error_code ignored;
        for (const auto& file : fs::directory_iterator(directory))
            fs::remove(file.path(), ignored);
        fs::remove(directory, ignored);
    }
};

void little(std::ostream& out, uint32_t value, unsigned bytes)
{
    for (unsigned n = 0; n < bytes; ++n)
        out.put(static_cast<char>(value >> (n * 8)));
}

float sample(int frame, int channel)
{
    return ((frame * 3 + channel * 5) % 8 - 4) / 4.0f;
}

constexpr int frames = 4103; // 跨过解码器的 4096 帧块，顺便抓出声道偏移算错的情况。

void makeWave(const fs::path& path, unsigned bits, unsigned channels, bool floating, bool invalid = false)
{
    const unsigned bytes = frames * channels * (bits / 8);
    std::ofstream out(path, std::ios::binary);
    out.exceptions(std::ios::badbit | std::ios::failbit);
    out.write("RIFF", 4);
    little(out, 46 + bytes + (bytes & 1), 4);
    out.write("WAVEJUNK", 8);
    little(out, 1, 4);
    out.write("x\0", 2); // 未知块和奇数字节填充也必须正常跳过。
    out.write("fmt ", 4);
    little(out, 16, 4);
    little(out, floating ? 3 : 1, 2);
    little(out, channels, 2);
    little(out, 48000, 4);
    little(out, 48000 * channels * bits / 8, 4);
    little(out, channels * bits / 8, 2);
    little(out, bits, 2);
    out.write("data", 4);
    little(out, bytes, 4);
    for (int frame = 0; frame < frames; ++frame) {
        for (unsigned channel = 0; channel < channels; ++channel) {
            float value = invalid && frame == 0 ? std::numeric_limits<float>::quiet_NaN() : sample(frame, channel);
            uint32_t encoded;
            if (floating)
                std::memcpy(&encoded, &value, sizeof encoded);
            else if (bits == 8)
                encoded = static_cast<uint32_t>(value * 128 + 128);
            else
                encoded = static_cast<uint32_t>(static_cast<int64_t>(value * (int64_t{1} << (bits - 1))));
            little(out, encoded, bits / 8);
        }
    }
    if (bytes & 1)
        out.put(0);
}

void checkPcm(const fs::path& file)
{
    for (unsigned channels : {1u, 2u}) {
        for (unsigned bits : {8u, 16u, 24u, 32u, 0u}) {
            makeWave(file, bits ? bits : 32, channels, bits == 0);
            WavePcm pcm(file);
            check(pcm.getSampleCount() == frames && pcm.getChannelCount() == channels &&
                  pcm.getSampleRate() == 48000, "Wrong PCM metadata.");
            std::vector<float> left(frames + 5), right(frames + 5);
            std::vector<double> doubleLeft(frames + 5), doubleRight(frames + 5);
            void* floats[]{left.data(), right.data()};
            void* doubles[]{doubleLeft.data(), doubleRight.data()};
            check(pcm.readSamples(-2, frames + 5, floats, false), "Float read failed.");
            check(pcm.readSamples(-2, frames + 5, doubles, true), "Double read failed.");
            for (unsigned channel = 0; channel < channels; ++channel) {
                for (int i = 0; i < frames + 5; ++i) {
                    const auto expected = i >= 2 && i < frames + 2 ? sample(i - 2, channel) : 0;
                    check(static_cast<float*>(floats[channel])[i] == expected, "Wrong planar float sample.");
                    check(static_cast<double*>(doubles[channel])[i] == expected, "Wrong planar double sample.");
                    if (i < frames)
                        check(pcm.channelData(channel)[i] == sample(i, channel), "Wrong engine channel sample.");
                }
            }
            check(!pcm.readSamples(0, -1, floats, false) && !pcm.readSamples(0, 1, nullptr, false),
                  "Invalid read accepted.");
        }
    }
    makeWave(file, 32, 2, true, true);
    rejects([&] { WavePcm invalid(file); });
    makeWave(file, 16, 2, false);
    fs::resize_file(file, fs::file_size(file) - 1);
    rejects([&] { WavePcm truncated(file); });
}

unsigned long long readRevision(const fs::path& path)
{
    // 和 Java NIO 一样允许替换打开的文件：读者拿到的是完整的旧文件或新文件。
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    check(file != INVALID_HANDLE_VALUE, "Revision marker disappeared.");
    char bytes[64]{};
    DWORD count = 0;
    const bool read = ReadFile(file, bytes, sizeof bytes, &count, nullptr);
    CloseHandle(file);
    check(read, "Could not read revision.");
    const std::string text(bytes, count);
    const auto revision = std::stoull(text);
    check(text == std::to_string(revision) + "\n", "Partially published revision.");
    return revision;
}

void checkPublication(const fs::path& marker)
{
    bwara::publishRevision(marker, 0);
    std::atomic<bool> finished{false};
    auto writer = std::async(std::launch::async, [&] {
        try {
            for (unsigned revision = 1; revision <= 500; ++revision)
                bwara::publishRevision(marker, revision);
        } catch (...) {
            finished = true;
            throw;
        }
        finished = true;
    });
    unsigned long long previous = 0;
    do {
        const auto current = readRevision(marker);
        check(current >= previous && current <= 500, "Revision went backwards.");
        previous = current;
    } while (!finished);
    writer.get();
    check(readRevision(marker) == 500, "Latest revision was lost.");

    // 故意不允许替换，验证发布失败时旧版本仍然可读，而不是留下空门牌。
    HANDLE locked = CreateFileW(marker.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    check(locked != INVALID_HANDLE_VALUE, "Could not lock marker for failure test.");
    bool rejected = false;
    try { bwara::publishRevision(marker, 501); } catch (const std::exception&) { rejected = true; }
    CloseHandle(locked);
    check(rejected && readRevision(marker) == 500, "Failed publication damaged previous revision.");
    bwara::publishRevision(marker, 501);
    check(readRevision(marker) == 501 && !fs::exists(marker.wstring() + L".tmp"), "Publication did not recover.");
}
}

int main()
{
    try {
        Fixture fixture;
        checkPcm(fixture.directory / L"双声道.wav");
        checkPublication(fixture.directory / "preview.ready");
        std::cout << "PCM formats, channel layout, bounds, malformed input and atomic revisions passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
