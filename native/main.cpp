#include "session.h"
#include "ARA_Library/Debug/ARADebug.h"
#include <windows.h>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

ARA_SETUP_DEBUG_MESSAGE_PREFIX("BwARA");

int wmain(int argc, wchar_t** argv)
{
    // live 供模组使用；edit/render 留作脱离 Bitwig 的诊断入口，排查时能少一层干扰。
    const std::wstring_view mode = argc > 1 ? argv[1] : L"";
    const bool editor = mode == L"edit" || mode == L"live";
    const bool followTransport = argc >= 8 && editor && std::wstring_view(argv[7]) == L"--follow";
    if (argc < 6 || (argc > 7 && !followTransport) || argc > 10 || (!editor && mode != L"render")) {
        std::cerr << "Usage: bwara-host (edit|live|render) PLUGIN SOURCE OUTPUT.wav OUTPUT.ara [INPUT.ara|-] [--follow [WINDOW_SIZE [BITWIG_PID]]]\n";
        return 2;
    }
    const auto com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(com)) {
        std::cerr << "Could not initialize the Windows UI thread.\n";
        return 1;
    }
    int status = 0;
    try {
        const auto* archive = argc >= 7 && std::wstring_view(argv[6]) != L"-" ? argv[6] : L"";
        AraSession session(argv[2], argv[3], archive, mode == L"live");
        if (mode == L"live")
            session.beginLiveRendering(std::filesystem::absolute(argv[4]).parent_path());
        if (editor)
            editInWindow(session, followTransport, argc >= 9 ? argv[8] : L"",
                         argc >= 10 ? std::stoul(argv[9]) : 0);
        if (mode == L"live")
            session.finishLiveRendering();
        else
            session.render(argv[4], argv[5]);
    } catch (const std::exception& error) {
        std::cerr << "BwARA: " << error.what() << '\n';
        status = 1;
    }
    CoUninitialize();
    return status;
}
