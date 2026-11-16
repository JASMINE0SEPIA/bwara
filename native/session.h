#pragma once

#include <filesystem>
#include <memory>
#include <cstdint>

namespace Steinberg {
class IPlugView;
}

// 一个编辑窗口、一份源音频、一份 ARA 文档。Bitwig 的拉伸与混音不在这里重做。
class AraSession {
public:
    AraSession(const std::filesystem::path& plugin, const std::filesystem::path& input,
               const std::filesystem::path& priorArchive, bool audition);
    ~AraSession();

    Steinberg::IPlugView* createEditor();
    void selectRegion();
    void poll();
    void startTransport();
    void updateTransport(int mode, double sourceSeconds);
    void stopTransport();
    void render(const std::filesystem::path& output, const std::filesystem::path& archive);
    void beginLiveRendering(const std::filesystem::path& directory);
    bool canRequestProjectSave() const;
    bool projectClosed() const;
    void requestProjectSave();
    void updateLiveRendering(bool editingWithMouse);
    void finishLiveRendering();

private:
    struct State;
    std::unique_ptr<State> state;
};

void editInWindow(AraSession& session, bool followTransport, const std::filesystem::path& sizeFile,
                  unsigned long bitwigPid);
void bwaraTransport(const char* command, double seconds);
