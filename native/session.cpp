#include "session.h"
#include "wave_source.h"
#include "revision_file.h"
#include "audition.h"

#include "CompanionAPIs.h"
#include "TestHost.h"
#include "ExamplesCommon/Archives/Archives.h"
#include "ExamplesCommon/AudioFiles/AudioFiles.h"
#include "ARA_API/ARAVST3.h"
#include "pluginterfaces/gui/iplugview.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstprocesscontext.h"
#include "public.sdk/source/vst/hosting/hostclasses.h"
#include "public.sdk/source/vst/hosting/plugprovider.h"
#include <windows.h>
#include <avrt.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <fstream>
#include <future>
#include <iterator>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

DEF_CLASS_IID(ARA::IMainFactory)
DEF_CLASS_IID(ARA::IPlugInEntryPoint2)

namespace fs = std::filesystem;
using namespace Steinberg;
using namespace Steinberg::Vst;

// 这些回调由 Melodyne 的 UI 调用。把请求送回 Bitwig，由工程自己的 transport 执行。
void bwaraTransport(const char* command, double seconds)
{
    std::cout << command << ' ' << seconds << '\n' << std::flush;
}

namespace {
void require(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}

void checked(tresult result, const char* operation)
{
    require(result == kResultOk, operation);
}

class ComponentHandler final : public U::Implements<U::Directly<IComponentHandler>> {
public:
    tresult PLUGIN_API beginEdit(ParamID) override { return kResultOk; }
    tresult PLUGIN_API performEdit(ParamID, ParamValue) override { return kResultOk; }
    tresult PLUGIN_API endEdit(ParamID) override { return kResultOk; }
    tresult PLUGIN_API restartComponent(int32) override { return kNotImplemented; }
};

class Vst3Entry final : public PlugInEntry {
public:
    explicit Vst3Entry(const fs::path& path) : PlugInEntry(path.u8string())
    {
        std::string error;
        std::clog << "Loading VST3 module\n";
        module = VST3::Hosting::Module::create(path.u8string(), error);
        if (!module)
            throw std::runtime_error("VST3 load: " + error);

        for (const auto& info : module->getFactory().classInfos()) {
            if (info.category() == kARAMainFactoryClass) {
                require(!factoryProvider, "Expected exactly one ARA factory.");
                factoryProvider = module->getFactory().createInstance<ARA::IMainFactory>(info.ID());
            }
        }
        require(factoryProvider && factoryProvider->getFactory(), "The VST3 has no ARA factory.");
        validateAndSetFactory(factoryProvider->getFactory());
    }

    ~Vst3Entry() override
    {
        if (initialized)
            uninitializeARA();
    }

    void initialize()
    {
        initializeARA(nullptr);
        initialized = true;
    }

    std::unique_ptr<PlugInInstance> createPlugInInstance() override;
    class Instance;

private:
    VST3::Hosting::Module::Ptr module;
    IPtr<ARA::IMainFactory> factoryProvider;
    bool initialized = false;
};

class Vst3Entry::Instance final : public PlugInInstance {
public:
    Instance(const VST3::Hosting::PluginFactory& factory, const VST3::Hosting::ClassInfo& info)
    {
        PluginContextFactory::instance().setPluginContext(&host);
        provider = owned(new PlugProvider(factory, info));
        require(provider->initialize(), "VST3 component/controller initialization failed.");
        component = provider->getComponentPtr();
        controller = provider->getControllerPtr();
        processor = FUnknownPtr<IAudioProcessor>(component);
        require(component && processor, "The VST3 has no audio processor.");
        if (controller)
            controller->setComponentHandler(&handler);
    }

    ~Instance() override
    {
        stopRendering();
        if (controller)
            controller->setComponentHandler(nullptr);
        controller.reset();
        processor = nullptr;
        component.reset();
        provider.reset();
        PluginContextFactory::instance().setPluginContext(nullptr);
    }

    void bindToDocumentControllerWithRoles(ARA::ARADocumentControllerRef doc,
                                           ARA::ARAPlugInInstanceRoleFlags roles) override
    {
        FUnknownPtr<ARA::IPlugInEntryPoint2> entry(component);
        require(entry, "The VST3 component does not expose ARA2.");
        constexpr auto known = ARA::kARAPlaybackRendererRole | ARA::kARAEditorRendererRole |
                               ARA::kARAEditorViewRole;
        auto* extension = entry->bindToDocumentControllerWithRoles(doc, known, roles);
        require(extension != nullptr, "The VST3 rejected the ARA document binding.");
        validateAndSetPlugInExtensionInstance(extension, roles);
    }

    IPlugView* createEditor()
    {
        return controller ? controller->createView(ViewType::kEditor) : nullptr;
    }

    void startRendering(int channels, int blockSize, double rate) override
    {
        startProcessing(channels, blockSize, rate, kOffline);
    }

    void startProcessing(int channels, int blockSize, double rate, int32 mode)
    {
        ProcessSetup setup{mode, kSample32, blockSize, rate};
        auto input = channels == 1 ? SpeakerArr::kMono : SpeakerArr::kStereo;
        auto output = input;
        checked(processor->setBusArrangements(&input, 1, &output, 1), "VST3 rejected the channel layout.");
        checked(component->activateBus(kAudio, kInput, 0, true), "VST3 input activation failed.");
        checked(component->activateBus(kAudio, kOutput, 0, true), "VST3 output activation failed.");
        checked(processor->setupProcessing(setup), "VST3 processing setup failed.");
        checked(component->setActive(true), "VST3 activation failed.");
        active = true;
        checked(processor->setProcessing(true), "VST3 start processing failed.");
        processing = true;
        channelCount = channels;
        sampleRate = rate;
        processMode = mode;
        for (int channel = 0; channel < channels; ++channel)
            silence[channel].assign(blockSize, 0.0f);
    }

    void renderSamples(int frames, int64_t position, float** buffers) override
    {
        processSamples(frames, position, position, true, buffers);
    }

    void processSamples(int frames, int64_t position, int64_t continuous, bool playing, float** buffers)
    {
        for (int channel = 0; channel < channelCount; ++channel)
            std::fill_n(buffers[channel], frames, 0.0f);
        std::array<float*, 2> inputPointers{silence[0].data(), silence[1].data()};
        AudioBusBuffers input;
        input.numChannels = channelCount;
        // 试听会在没有输入信号时出声。实时路径每个块都交给插件处理。
        input.silenceFlags = processMode == kOffline ? (1u << channelCount) - 1 : 0;
        input.channelBuffers32 = inputPointers.data();
        AudioBusBuffers output;
        output.numChannels = channelCount;
        output.channelBuffers32 = buffers;

        ProcessContext time{};
        time.state = ProcessContext::kContTimeValid | ProcessContext::kProjectTimeMusicValid |
                     ProcessContext::kTempoValid | ProcessContext::kTimeSigValid;
        if (playing)
            time.state |= ProcessContext::kPlaying;
        time.sampleRate = sampleRate;
        time.projectTimeSamples = position;
        time.continousTimeSamples = continuous;
        time.projectTimeMusic = position / sampleRate;
        time.tempo = 60;
        time.timeSigNumerator = 4;
        time.timeSigDenominator = 4;

        ProcessData block;
        block.processMode = processMode;
        block.symbolicSampleSize = kSample32;
        block.numSamples = frames;
        block.numInputs = 1;
        block.numOutputs = 1;
        block.inputs = &input;
        block.outputs = &output;
        block.processContext = &time;
        checked(processor->process(block), "VST3 processing failed.");
    }

    void stopRendering() override
    {
        if (processing) {
            processor->setProcessing(false);
            processing = false;
        }
        if (active) {
            component->setActive(false);
            active = false;
        }
    }

private:
    HostApplication host;
    ComponentHandler handler;
    IPtr<PlugProvider> provider;
    IPtr<IComponent> component;
    IPtr<IEditController> controller;
    FUnknownPtr<IAudioProcessor> processor;
    std::array<std::vector<float>, 2> silence;
    int channelCount = 0;
    int32 processMode = kOffline;
    double sampleRate = 0;
    bool active = false;
    bool processing = false;
};

std::unique_ptr<PlugInInstance> Vst3Entry::createPlugInInstance()
{
    const auto& factory = module->getFactory();
    for (const auto& info : factory.classInfos())
        if (info.category() == kVstAudioEffectClass && info.name() == getARAFactory()->plugInName)
            return std::make_unique<Instance>(factory, info);
    throw std::runtime_error("The ARA factory has no matching VST3 audio component.");
}

void writeBytes(std::ofstream& stream, const void* bytes, std::streamsize size)
{
    stream.write(static_cast<const char*>(bytes), size);
}

void writeUInt(std::ofstream& stream, uint32_t number, unsigned count)
{
    for (unsigned i = 0; i < count; ++i)
        stream.put(static_cast<char>(number >> (8 * i)));
}

void writeWaveHeader(std::ofstream& stream, uint32_t rate, uint32_t frames, uint16_t channels)
{
    const uint32_t dataBytes = frames * channels * sizeof(float);
    stream.write("RIFF", 4);
    writeUInt(stream, 48 + dataBytes, 4);
    stream.write("WAVEfmt ", 8);
    writeUInt(stream, 16, 4);
    writeUInt(stream, 3, 2);
    writeUInt(stream, channels, 2);
    writeUInt(stream, rate, 4);
    writeUInt(stream, rate * channels * sizeof(float), 4);
    writeUInt(stream, channels * sizeof(float), 2);
    writeUInt(stream, 32, 2);
    stream.write("fact", 4);
    writeUInt(stream, 4, 4);
    writeUInt(stream, frames, 4);
    stream.write("data", 4);
    writeUInt(stream, dataBytes, 4);
}

constexpr int blockSize = 1024;

void renderWave(PlugInInstance& instance, const WaveSource& audio, const fs::path& output)
{
    const auto channels = audio.getChannelCount();
    const auto rate = static_cast<uint32_t>(audio.getSampleRate());
    const auto frames = static_cast<uint32_t>(audio.getSampleCount());
    require(frames <= (UINT32_MAX - 48) / (sizeof(float) * channels), "The source exceeds WAV limits.");

    std::ofstream wav(output, std::ios::binary);
    wav.exceptions(std::ios::badbit | std::ios::failbit);
    writeWaveHeader(wav, rate, frames, static_cast<uint16_t>(channels));
    std::array<std::array<float, blockSize>, 2> samples{};
    std::array<float*, 2> pointers{samples[0].data(), samples[1].data()};
    std::array<float, blockSize * 2> interleaved{};
    for (uint32_t position = 0; position < frames;) {
        const auto count = static_cast<int>(std::min<uint32_t>(blockSize, frames - position));
        for (int channel = 0; channel < channels; ++channel)
            std::fill_n(pointers[channel], count, 0.0f);
        instance.renderSamples(count, position, pointers.data());
        for (int i = 0; i < count; ++i)
            for (int channel = 0; channel < channels; ++channel) {
                require(std::isfinite(samples[channel][i]), "The plugin rendered invalid samples.");
                interleaved[i * channels + channel] = samples[channel][i];
            }
        writeBytes(wav, interleaved.data(), sizeof(float) * count * channels);
        position += count;
    }
    wav.close();
}

}

struct AraSession::State {
    // ARA 文档只由窗口线程管理；后台仅渲染音频。版本号用来淘汰渲染期间过时的结果。
    static constexpr int transportBlockSize = 256;
    using Clock = std::chrono::steady_clock;
    struct Position {
        Clock::time_point received{};
        double seconds = 0;
        double speed = 1;
        bool playing = false;
        bool hostPlaying = false;
    };

    Vst3Entry entry;
    std::unique_ptr<WaveSource> audio;
    TestHost host;
    ARADocumentController* doc = nullptr;
    ARA::ARAPlaybackRegionRef region = nullptr;
    std::unique_ptr<PlugInInstance> instance;
    std::mutex positionMutex;
    Position position;
    std::atomic<bool> following{false};
    std::thread transportThread;
    std::unique_ptr<PlugInInstance> previewRenderer;
    std::future<void> previewJob;
    fs::path previewDirectory;
    unsigned long long observedRevision = 0;
    unsigned long long renderingRevision = 0;
    unsigned long long publishedRevision = 0;
    unsigned long long completedSave = 0;
    unsigned long long completedCloseCheck = 0;
    unsigned long long completedRestore = 0;

    const bool audition;
    const bool restoredOnOpen;

    State(const fs::path& plugin, const fs::path& input, const fs::path& priorArchive, const fs::path& liveDirectory)
        : entry(plugin), audio(std::make_unique<WaveSource>(input)), previewDirectory(liveDirectory),
          audition(!liveDirectory.empty()), restoredOnOpen(!priorArchive.empty())
    {
        std::clog << "Creating ARA document\n";
        entry.initialize();
        auto* document = host.addDocument("BwARA", &entry);
        doc = host.getDocumentController(document);
        require(doc != nullptr, "Could not create the ARA document.");
        doc->setMinimalContentUpdateLogging(true);
        doc->beginEditing();
        auto* context = host.addMusicalContext(document, "Clip", {0.3f, 0.6f, 0.8f});
        auto* sequence = host.addRegionSequence(document, "Audio", context, {0.3f, 0.6f, 0.8f});
        auto* source = host.addAudioSource(document, audio.get(), "source");
        auto* modification = host.addAudioModification(document, source, "Clip", "modification");
        // 按源文件时间建立区域，时间拉伸留给 Bitwig。两边都拉一次，歌就真的拉长了。
        auto* playback = host.addPlaybackRegion(document, modification, ARA::kARAPlaybackTransformationNoChanges,
                                               0.0, source->getDuration(), 0.0, source->getDuration(),
                                               sequence, "Clip", {0.3f, 0.6f, 0.8f});
        doc->endEditing();
        region = doc->getRef(playback);

        if (restoredOnOpen) {
            // 从已有改音开窗时，先留一份原音状态，Bitwig 才能一路撤回第一笔之前。
            if (audition)
                saveArchive(previewDirectory / "original.ara");
            restoreArchive(priorArchive);
        }
        doc->enableAudioSourceSamplesAccess(source, true);

        std::clog << "Initializing VST3 component\n";
        instance = entry.createPlugInInstance();
        std::clog << "Binding ARA roles\n";
        const auto renderRole = ARA::kARAPlaybackRendererRole | (audition ? ARA::kARAEditorRendererRole : 0);
        instance->bindToDocumentControllerWithRoles(doc->getDocumentController()->getRef(), renderRole | ARA::kARAEditorViewRole);
        if (audition)
            instance->getEditorRenderer().addPlaybackRegion(region);
        instance->getPlaybackRenderer().addPlaybackRegion(region);
        if (audition && !restoredOnOpen)
            saveArchive(previewDirectory / "original.ara");
    }

    ~State()
    {
        stopTransport();
        if (previewJob.valid())
            previewJob.wait();
        if (previewRenderer) {
            previewRenderer->stopRendering();
            previewRenderer->getPlaybackRenderer().removePlaybackRegion(region);
        }
        if (instance) {
            instance->stopRendering();
            if (audition)
                instance->getEditorRenderer().removePlaybackRegion(region);
            instance->getPlaybackRenderer().removePlaybackRegion(region);
        }
    }

    void stopTransport()
    {
        following = false;
        if (transportThread.joinable()) {
            transportThread.join();
            instance->stopRendering();
        }
    }
    void saveArchive(const fs::path& archive)
    {
        MemoryArchive data(entry.getARAFactory()->documentArchiveID);
        require(doc->storeObjectsToArchive(&data), "The plugin could not save its edit archive.");
        const auto bytes = static_cast<std::string>(data);
        require(!bytes.empty(), "The plugin returned an empty edit archive.");
        std::ofstream saved(archive, std::ios::binary);
        saved.exceptions(std::ios::badbit | std::ios::failbit);
        saved.write(bytes.data(), bytes.size());
        saved.close();
    }

    void restoreArchive(const fs::path& path)
    {
        std::ifstream in(path, std::ios::binary);
        require(in.is_open(), "Could not open edit archive.");
        const std::string bytes{std::istreambuf_iterator<char>(in), {}};
        require(!bytes.empty() && !in.bad(), "Could not read edit archive.");
        MemoryArchive archive(bytes, entry.getARAFactory()->documentArchiveID);
        doc->beginEditing();
        const auto restored = doc->restoreObjectsFromArchive(&archive);
        doc->endEditing();
        require(restored, "The plugin could not restore its edit archive.");
    }

    fs::path previewPath(const wchar_t* extension) const
    {
        return previewDirectory / (L"preview-" + std::to_wstring(renderingRevision) + extension);
    }

    void startPreview()
    {
        // 编辑窗口负责跟随光标，另一个实例负责离线渲染，二者不抢同一套 process 状态。
        if (!previewRenderer) {
            previewRenderer = entry.createPlugInInstance();
            previewRenderer->bindToDocumentControllerWithRoles(doc->getDocumentController()->getRef(),
                                                               ARA::kARAPlaybackRendererRole);
            previewRenderer->getPlaybackRenderer().addPlaybackRegion(region);
            previewRenderer->startRendering(audio->getChannelCount(), blockSize, audio->getSampleRate());
        }
        renderingRevision = observedRevision;
        // 存档在 UI 线程抓取。渲染期间若又改了音，整对结果作废，下次再来。
        saveArchive(previewPath(L".ara"));
        previewJob = std::async(std::launch::async, [this, wave = previewPath(L".wav")] {
            renderWave(*previewRenderer, *audio, wave);
        });
    }

    void completePreview()
    {
        previewJob.get();
        if (renderingRevision == observedRevision) {
            // 音频和存档都完整后才更新门牌；Java 不必逐个翻遍历史版本。
            bwara::publishRevision(previewDirectory / "preview.ready", renderingRevision);
            publishedRevision = renderingRevision;
            std::clog << "Published preview " << renderingRevision << '\n';
        } else {
            fs::remove(previewPath(L".wav"));
            fs::remove(previewPath(L".ara"));
        }
    }

    void replyWithRevision(const std::wstring& stem)
    {
        bwara::publishRevision(previewDirectory / (stem + L".ready"), observedRevision);
    }
};

AraSession::AraSession(const fs::path& plugin, const fs::path& input, const fs::path& priorArchive,
                       const fs::path& liveDirectory)
    : state(std::make_unique<State>(plugin, input, priorArchive, liveDirectory)) {}
AraSession::~AraSession() = default;

IPlugView* AraSession::createEditor()
{
    return static_cast<Vst3Entry::Instance*>(state->instance.get())->createEditor();
}

void AraSession::selectRegion()
{
    const ARA::ARAViewSelection selection{
        ARA_IMPLEMENTED_STRUCT_SIZE(ARAViewSelection, timeRange),
        1, &state->region, 0, nullptr, nullptr
    };
    state->instance->getEditorView().notifySelection(&selection);
}

void AraSession::poll()
{
    auto& current = *state;
    current.doc->pollModelUpdates();
    if (current.audition && !current.restoredOnOpen && !current.doc->playbackRevision && current.doc->sourceChanged)
        current.saveArchive(current.previewDirectory / "original.ara");
}

void AraSession::startTransport()
{
    // Melodyne 可以在这里发出试听音频。Bitwig 在播歌时，编辑器的输出保持静音。
    auto& doc = *state;
    auto& instance = static_cast<Vst3Entry::Instance&>(*doc.instance);
    const auto rate = doc.audio->getSampleRate();
    const auto frames = doc.audio->getSampleCount();
    const auto channels = doc.audio->getChannelCount();
    instance.startProcessing(channels, State::transportBlockSize, rate, kRealtime);
    doc.following = true;
    doc.transportThread = std::thread([&doc, &instance, rate, frames, channels] {
        constexpr int blockSize = State::transportBlockSize;
        using std::chrono::duration;
        using namespace std::chrono_literals;
        const auto period = std::chrono::duration_cast<State::Clock::duration>(duration<double>(blockSize / rate));
        auto next = State::Clock::now();
        int64_t continuous = 0;
        std::array<std::array<float, blockSize>, 2> samples{};
        std::array<float*, 2> pointers{samples[0].data(), samples[1].data()};
        const auto clock = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                                  TIMER_MODIFY_STATE | SYNCHRONIZE);
        DWORD task = 0;
        const auto audioPriority = AvSetMmThreadCharacteristicsW(L"Audio", &task);
        try {
            Audition output(doc.audition, channels, static_cast<unsigned>(rate), blockSize);
            while (doc.following) {
                const bool deviceClock = output.waitForBlock();
                if (!doc.following)
                    break;
                State::Position snapshot;
                {
                    std::lock_guard lock(doc.positionMutex);
                    snapshot = doc.position;
                }
                const auto now = State::Clock::now();
                const auto elapsed = now - snapshot.received;
                const bool playing = snapshot.playing && elapsed < 150ms;
                const double advance = playing ? duration<double>(elapsed).count() * snapshot.speed : 0;
                const auto frame = std::clamp<int64_t>(std::llround((snapshot.seconds + advance) * rate), 0, frames);
                instance.processSamples(blockSize, frame, continuous, playing, pointers.data());
                output.submit(snapshot.hostPlaying ? nullptr : pointers.data());
                continuous += blockSize;
                // 试听的静音块也照送。只在没有声卡时才让软件钟接管光标。
                if (deviceClock) {
                    next = now;
                    continue;
                }
                next = std::max(next + period, now);
                auto remaining = std::chrono::duration_cast<duration<LONGLONG, std::ratio<1, 10000000>>>(next - State::Clock::now());
                if (remaining.count() <= 0)
                    continue;
                LARGE_INTEGER due{};
                due.QuadPart = -remaining.count();
                if (clock && SetWaitableTimer(clock, &due, 0, nullptr, nullptr, FALSE))
                    WaitForSingleObject(clock, INFINITE);
                else
                    std::this_thread::sleep_until(next);
            }
        } catch (const std::exception& error) {
            std::cerr << "BwARA transport: " << error.what() << '\n';
            doc.following = false;
        }
        if (clock)
            CloseHandle(clock);
        if (audioPriority)
            AvRevertMmThreadCharacteristics(audioPriority);
    });
}

void AraSession::updateTransport(int mode, double sourceSeconds)
{
    if (!std::isfinite(sourceSeconds))
        return;
    const bool playing = mode == 1;
    const auto now = State::Clock::now();
    std::lock_guard lock(state->positionMutex);
    auto& position = state->position;
    const auto interval = std::chrono::duration<double>(now - position.received).count();
    double speed = playing && position.playing && interval > 0.005 && interval < 0.15
        ? (sourceSeconds - position.seconds) / interval : 1;
    if (std::abs(speed) > 16)
        speed = 1; // 跳播或循环折返，不能当成歌手突然开始百米冲刺。
    position = {now, sourceSeconds, speed, playing, mode != 0};
}

void AraSession::stopTransport()
{
    state->stopTransport();
}

void AraSession::render(const fs::path& output, const fs::path& archive)
{
    auto& instance = *state->instance;
    require(!fs::exists(output) && !fs::exists(archive), "Render destinations must be new files.");
    poll();
    instance.startRendering(state->audio->getChannelCount(), blockSize, state->audio->getSampleRate());
    renderWave(instance, *state->audio, output);
    instance.stopRendering();
    state->saveArchive(archive);
}

bool AraSession::canRequestProjectSave() const
{
    return !state->previewDirectory.empty();
}

bool AraSession::projectClosed() const
{
    return !state->previewDirectory.empty() && fs::exists(state->previewDirectory / "project-closed");
}

void AraSession::requestProjectSave()
{
    std::ofstream request(state->previewDirectory / "save-shortcut");
    require(static_cast<bool>(request), "Could not request a Bitwig project save.");
    std::clog << "Forwarded Ctrl+S to Bitwig\n";
}

void AraSession::updateLiveRendering(bool editingWithMouse)
{
    using namespace std::chrono_literals;
    auto& current = *state;
    if (current.previewDirectory.empty())
        return;
    current.observedRevision = current.doc->playbackRevision;
    const auto closeCheck = L"close-check-" + std::to_wstring(current.completedCloseCheck + 1);
    if (fs::exists(current.previewDirectory / (closeCheck + L".request"))) {
        current.replyWithRevision(closeCheck);
        ++current.completedCloseCheck;
    }
    if (current.previewJob.valid() && current.previewJob.wait_for(0ms) == std::future_status::ready)
        current.completePreview();

    const auto restore = L"restore-" + std::to_wstring(current.completedRestore + 1);
    const auto restoreRequest = current.previewDirectory / (restore + L".request");
    if (fs::exists(restoreRequest)) {
        if (current.previewJob.valid())
            return;
        const bool restoringOriginal = fs::file_size(restoreRequest) == 0;
        current.restoreArchive(restoringOriginal ? current.previewDirectory / "original.ara" : restoreRequest);
        selectRegion();
        // 这是 Bitwig 恢复旧版本，不是一笔新改音。收掉这次恢复带来的通知。
        current.doc->pollModelUpdates();
        // 还原原音后可能再跑一次分析。这次迟到的通知和刚开窗时一样，也别进撤销栈。
        current.doc->initialAnalysis = restoringOriginal;
        current.observedRevision = current.doc->playbackRevision;
        current.renderingRevision = current.observedRevision;
        current.replyWithRevision(restore);
        ++current.completedRestore;
        return;
    }

    const auto token = current.completedSave + 1;
    const auto stem = L"save-" + std::to_wstring(token);
    const bool saving = fs::exists(current.previewDirectory / (stem + L".request"));
    unsigned long long consumed = 0;
    std::ifstream(current.previewDirectory / "preview-consumed") >> consumed;
    // 拖动中先收变更，松手就处理最新一版。一次只挂出一份预览，保存请求可以直接排进来。
    if (!current.previewJob.valid() && current.observedRevision != current.renderingRevision &&
        (saving || (!editingWithMouse && consumed >= current.publishedRevision)))
        current.startPreview();

    if (saving && !current.previewJob.valid()) {
        // 保存必须等当前音频和 ARA 存档配齐；关闭询问只查有没有改动，不必等音频。
        current.replyWithRevision(stem);
        current.completedSave = token;
    }
}

void AraSession::finishLiveRendering()
{
    auto& current = *state;
    if (current.previewDirectory.empty() || projectClosed())
        return;
    poll();
    current.observedRevision = current.doc->playbackRevision;
    if (current.previewJob.valid())
        current.completePreview();
    if (current.observedRevision != current.renderingRevision) {
        current.startPreview();
        current.completePreview();
    }
}
