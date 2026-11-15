#pragma once

// Windows 默认输出上的短试听。歌曲音频仍从 Bitwig 的设备出来。
class Audition {
public:
    Audition(bool enabled, int channels, unsigned rate, int frames);
    ~Audition();
    bool waitForBlock() noexcept;
    void submit(float* const samples[]) noexcept;
    Audition(const Audition&) = delete;
    Audition& operator=(const Audition&) = delete;

private:
    struct Output;
    Output* output = nullptr;
    bool initialized = false;
    void closeOutput() noexcept;
};
