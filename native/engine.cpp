#include "engine_control.h"
#include "wave_pcm.h"

#include <MinHook.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <vector>

namespace fs = std::filesystem;
using namespace bwara;

namespace {

// 仅对应 Bitwig 6.1.7.24824 Windows x64 AVX2；加载器还会核对整份 EXE 的哈希。
// 这些是私有内存布局，不是稳定 API。升级宿主时重新核实，不能凭长相猜地址。
constexpr uintptr_t readerRva = 0x897910;
constexpr std::array<unsigned char, 16> readerPrologue{
    0x55, 0x41, 0x57, 0x41, 0x56, 0x56, 0x57, 0x53, 0x48, 0x83, 0xec, 0x58, 0x48, 0x8d, 0x6c, 0x24
};
constexpr uintptr_t leftScratch = 0x18;
constexpr uintptr_t rightScratch = 0x20030;
constexpr uintptr_t clipSource = 0x40060;
constexpr uintptr_t reverseFlag = 0x40078;
constexpr uintptr_t sourceFrame = 0x40080;
constexpr uintptr_t sampleCache = 0x20;
constexpr uintptr_t sourceChannels = 0xa1c;
constexpr uintptr_t sourceRate = 0xa20;
constexpr uintptr_t sourceLength = 0xa28;
constexpr int scratchFrames = 32768;

template<class T> T& field(void* object, uintptr_t offset)
{
    return *reinterpret_cast<T*>(static_cast<unsigned char*>(object) + offset);
}

struct Preview {
    int64_t taggedLength;
    std::shared_ptr<const WavePcm> baseline;
    WavePcm edited;
};

using Read = bool (*)(void*, int, const float**, const float**);
Read readOriginal;
std::atomic<const Preview*> published{nullptr};
std::atomic<unsigned> readers{0};
alignas(8) volatile LONG64* hits;

struct ReadLease {
    // 音频线程只借用快照。回收交给后台，别在演出途中收走谱架。
    ReadLease() { ++readers; }
    ~ReadLease() { --readers; }
};

bool readPatched(void* context, int count, const float** left, const float** right)
{
    const bool result = readOriginal(context, count, left, right);
    if (!result || count <= 0 || count > scratchFrames)
        return result;

    ReadLease lease;
    const auto* snapshot = published.load();
    if (!snapshot)
        return result;
    auto* source = field<void*>(context, clipSource);
    auto* cache = source ? field<void*>(source, sampleCache) : nullptr;
    const auto& baseline = *snapshot->baseline;
    if (!cache || field<int64_t>(cache, sourceLength) != snapshot->taggedLength ||
        field<unsigned>(cache, sourceChannels) != baseline.getChannelCount() ||
        field<float>(cache, sourceRate) != baseline.getSampleRate())
        return result;

    // 原函数成功后已经推进游标；减去本次长度才是这一块音频的起点。
    const auto pos = field<int64_t>(context, sourceFrame) - count;
    const bool reverse = field<uint8_t>(context, reverseFlag) != 0;
    const auto first = reverse ? snapshot->taggedLength - 1 - pos : pos;
    const auto step = reverse ? -1 : 1;
    const float* const input[] = {*left, *right};
    float* const scratch[] = {&field<float>(context, leftScratch), &field<float>(context, rightScratch)};
    // 随机补尾只是第一道筛选，还要核对原 PCM。长度一样不等于同一个片段。
    // 只改当前 reader 的临时缓冲；共享解码缓存一碰，别的片段就跟着被“修音”了。
    auto checked = 0;
    for (int i = 0; i < count; i += std::max(1, count / 12)) {
        const auto frame = first + i * step;
        if (frame < 0 || frame >= baseline.getSampleCount())
            continue;
        for (int ch = 0; ch < baseline.getChannelCount(); ++ch) {
            if (!(std::abs(input[ch][i] - baseline.channelData(ch)[frame]) <= 0.00002f))
                return result;
        }
        ++checked;
    }
    if (checked == 0)
        return result;

    const auto& edited = snapshot->edited;
    for (int ch = 0; ch < baseline.getChannelCount(); ++ch) {
        if (!reverse && first >= 0 && first + count <= edited.getSampleCount()) {
            std::memcpy(scratch[ch], edited.channelData(ch) + first, count * sizeof(float));
        } else {
            for (int i = 0; i < count; ++i) {
                const auto frame = first + i * step;
                scratch[ch][i] = frame >= 0 && frame < edited.getSampleCount() ? edited.channelData(ch)[frame] : input[ch][i];
            }
        }
    }
    *left = scratch[0];
    *right = baseline.getChannelCount() == 2 ? scratch[1] : scratch[0];
    InterlockedIncrement64(hits);
    return result;
}

bool install()
{
    const auto image = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    void* entry = reinterpret_cast<void*>(image + readerRva);
    MEMORY_BASIC_INFORMATION page{};
    return VirtualQuery(entry, &page, sizeof(page)) && page.AllocationBase == reinterpret_cast<void*>(image) &&
           (page.Protect & (PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE)) &&
           std::memcmp(entry, readerPrologue.data(), readerPrologue.size()) == 0 &&
           MH_Initialize() == MH_OK &&
           MH_CreateHook(entry, reinterpret_cast<void*>(readPatched), reinterpret_cast<void**>(&readOriginal)) == MH_OK &&
           MH_EnableHook(entry) == MH_OK;
}

DWORD WINAPI serve(void*)
{
    Handle mapping(OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, controlName(GetCurrentProcessId()).c_str()));
    auto* control = mapping ? static_cast<EngineControl*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(EngineControl))) : nullptr;
    if (!control)
        return 0;
    hits = &control->hits;
    const bool installed = install();
    InterlockedExchange(&control->adapterState, installed ? 1 : -1);
    if (!installed)
        return 0;

    DWORD producer = 0;
    Handle owner;
    fs::path directory;
    unsigned long long installedRevision = 0;
    unsigned long long installedBasis = 0;
    std::shared_ptr<const WavePcm> baseline;
    std::unique_ptr<Preview> current;
    std::vector<std::unique_ptr<Preview>> retired;
    auto publish = [&](std::unique_ptr<Preview> preview) {
        if (current)
            retired.push_back(std::move(current));
        current = std::move(preview);
        published.store(current.get());
    };

    for (;;) {
        const auto requested = static_cast<DWORD>(InterlockedCompareExchange(&control->ownerPid, 0, 0));
        if (requested != producer || (owner && WaitForSingleObject(owner, 0) != WAIT_TIMEOUT)) {
            publish(nullptr);
            owner = Handle(requested ? OpenProcess(SYNCHRONIZE, FALSE, requested) : nullptr);
            producer = requested;
            directory = owner ? fs::path(control->directory) : fs::path{};
            installedRevision = 0;
            installedBasis = 0;
            baseline.reset();
        }
        if (owner && WaitForSingleObject(owner, 0) == WAIT_TIMEOUT) {
            try {
                std::ifstream command(directory / "engine-preview.txt");
                unsigned long long revision, basis;
                int64_t length;
                if (command >> revision >> length >> basis && revision > installedRevision) {
                    if (basis != installedBasis) {
                        baseline = std::make_shared<WavePcm>(directory / (L"preview-" + std::to_wstring(basis) + L".wav"));
                        installedBasis = basis;
                    }
                    WavePcm edited(directory / (L"preview-" + std::to_wstring(revision) + L".wav"));
                    if (baseline && length > baseline->getSampleCount() && edited.getSampleCount() == baseline->getSampleCount() &&
                        edited.getChannelCount() == baseline->getChannelCount() && edited.getSampleRate() == baseline->getSampleRate()) {
                        publish(std::make_unique<Preview>(Preview{length, baseline, std::move(edited)}));
                        installedRevision = revision;
                    }
                }
            } catch (const std::exception&) {
                // 新版本读失败就继续播上一个完整版本；半成品没有上台资格。
            }
        }
        // 默认顺序一致的原子操作把“取快照”和“回收旧快照”排好队；别随手改成 relaxed。
        if (readers.load() == 0)
            retired.clear();
        Sleep(25);
    }
}
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, void*)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(instance);
        Handle worker(CreateThread(nullptr, 0, serve, nullptr, 0, nullptr));
        return worker ? TRUE : FALSE;
    }
    return TRUE;
}

