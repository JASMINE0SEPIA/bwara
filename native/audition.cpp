#include "audition.h"
#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <memory>

using Microsoft::WRL::ComPtr;

struct Audition::Output {
    ComPtr<IAudioClient> client;
    ComPtr<IAudioRenderClient> render;
    HANDLE ready = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    UINT32 capacity = 0;
    UINT32 frames = 0;
    int channels = 0;
    unsigned drains = 0;
    bool submittedAudio = false;

    ~Output()
    {
        if (client)
            client->Stop();
        render.Reset();
        client.Reset();
        if (ready)
            CloseHandle(ready);
        if (drains)
            std::clog << "BwARA audition buffer drained " << drains << " times\n";
    }

    bool open(int channelCount, unsigned rate, int blockFrames)
    {
        channels = channelCount;
        frames = static_cast<UINT32>(blockFrames);
        ComPtr<IMMDeviceEnumerator> devices;
        ComPtr<IMMDevice> endpoint;
        const auto stride = static_cast<WORD>(channels * sizeof(float));
        const WAVEFORMATEX format{WAVE_FORMAT_IEEE_FLOAT, static_cast<WORD>(channels), rate,
                                  rate * stride, stride, 32, 0};
        // 声卡按自己的格式运行；共享模式负责把源文件的采样率和声道数对齐。
        constexpr auto flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                               AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
        if (!ready ||
            FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&devices))) ||
            FAILED(devices->GetDefaultAudioEndpoint(eRender, eMultimedia, &endpoint)) ||
            FAILED(endpoint->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                      reinterpret_cast<void**>(client.GetAddressOf()))) ||
            FAILED(client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 300000, 0, &format, nullptr)) ||
            FAILED(client->SetEventHandle(ready)) ||
            FAILED(client->GetService(IID_PPV_ARGS(&render))) ||
            FAILED(client->GetBufferSize(&capacity)) || capacity < frames)
            return false;
        BYTE* buffer = nullptr;
        return SUCCEEDED(render->GetBuffer(capacity, &buffer)) &&
               SUCCEEDED(render->ReleaseBuffer(capacity, AUDCLNT_BUFFERFLAGS_SILENT)) &&
               SUCCEEDED(client->Start());
    }
};

Audition::Audition(bool enabled, int channels, unsigned rate, int frames)
{
    if (!enabled)
        return;
    initialized = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
    if (initialized) {
        auto candidate = std::make_unique<Output>();
        if (candidate->open(channels, rate, frames)) {
            output = candidate.release();
            return;
        }
    }
    std::clog << "BwARA: Windows default output is unavailable for audition.\n";
}

Audition::~Audition()
{
    delete output;
    if (initialized)
        CoUninitialize();
}

void Audition::closeOutput() noexcept
{
    std::clog << "BwARA: audition output stopped; continuing transport without it.\n";
    delete output;
    output = nullptr;
}

bool Audition::waitForBlock() noexcept
{
    while (output) {
        UINT32 used = 0;
        if (FAILED(output->client->GetCurrentPadding(&used)) || used > output->capacity) {
            closeOutput();
            break;
        }
        if (output->capacity - used >= output->frames) {
            if (!used)
                ++output->drains;
            return true;
        }
        if (WaitForSingleObject(output->ready, 100) != WAIT_OBJECT_0)
            closeOutput();
    }
    return false;
}

void Audition::submit(float* const samples[]) noexcept
{
    if (!output)
        return;
    BYTE* buffer = nullptr;
    if (FAILED(output->render->GetBuffer(output->frames, &buffer))) {
        closeOutput();
        return;
    }
    auto* pcm = reinterpret_cast<float*>(buffer);
    bool audible = false;
    for (UINT32 frame = 0; samples && frame < output->frames; ++frame) {
        for (int channel = 0; channel < output->channels; ++channel) {
            const auto value = samples[channel][frame];
            const auto sample = std::isfinite(value) ? std::clamp(value, -1.0f, 1.0f) : 0.0f;
            *pcm++ = sample;
            audible |= sample != 0.0f;
        }
    }
    if (FAILED(output->render->ReleaseBuffer(output->frames, audible ? 0 : AUDCLNT_BUFFERFLAGS_SILENT))) {
        closeOutput();
    } else if (audible && !output->submittedAudio) {
        std::clog << "Audition output submitted\n";
        output->submittedAudio = true;
    }
}
