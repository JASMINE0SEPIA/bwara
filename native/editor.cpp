#include "session.h"
#include "pluginterfaces/gui/iplugview.h"
#include "pluginterfaces/base/funknownimpl.h"
#include <windows.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <exception>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

using namespace Steinberg;

namespace {
constexpr DWORD baseStyle = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
constexpr LONG maxViewSize = 8000;
constexpr int projectSaveKey = 1;

HWND bitwigOwner(DWORD pid, bool allowForeground)
{
    if (!pid)
        return nullptr;
    const auto matches = [pid](HWND window) {
        DWORD candidate = 0;
        return window && GetWindowThreadProcessId(window, &candidate) && candidate == pid;
    };
    const auto foreground = GetAncestor(GetForegroundWindow(), GA_ROOTOWNER);
    if (allowForeground && matches(foreground))
        return foreground;
    struct Search { DWORD pid; HWND window = nullptr; bool ambiguous = false; } found{pid};
    EnumWindows([](HWND window, LPARAM data) -> BOOL {
        auto& search = *reinterpret_cast<Search*>(data);
        DWORD candidate = 0;
        if (GetWindowThreadProcessId(window, &candidate) && candidate == search.pid &&
            IsWindowVisible(window) && !GetWindow(window, GW_OWNER) && GetWindowTextLengthW(window)) {
            if (search.window)
                search.ambiguous = true;
            search.window = window;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&found));
    return found.ambiguous ? nullptr : found.window;
}

bool validSize(const ViewRect& size)
{
    return size.getWidth() > 0 && size.getHeight() > 0 &&
           size.getWidth() <= maxViewSize && size.getHeight() <= maxViewSize;
}

RECT outerSize(HWND hwnd, const ViewRect& size)
{
    RECT rect{0, 0, size.getWidth(), size.getHeight()};
    AdjustWindowRectExForDpi(&rect, GetWindowLongW(hwnd, GWL_STYLE), FALSE,
                            GetWindowLongW(hwnd, GWL_EXSTYLE), GetDpiForWindow(hwnd));
    return rect;
}

class EditorFrame final : public U::Implements<U::Directly<IPlugFrame>> {
public:
    HWND window = nullptr;
    HWND container = nullptr;
    bool resizing = false;
    bool docked = false;

    tresult PLUGIN_API resizeView(IPlugView* view, ViewRect* size) override
    {
        // 窗口拖拽和插件主动改尺寸都会走这里。挡住重入，免得两边互相喊“你再大一点”。
        if (!view || !size || !window || !validSize(*size))
            return kInvalidArgument;
        if (resizing)
            return kResultOk;
        if (docked) {
            // 贴靠时分隔条决定大小，插件不能通过 resizeView 挤走 Bitwig 的其他面板。
            RECT bounds{};
            GetClientRect(window, &bounds);
            return size->getWidth() == bounds.right && size->getHeight() == bounds.bottom ? kResultOk : kResultFalse;
        }

        const auto area = outerSize(window, *size);
        resizing = true;
        const bool moved = SetWindowPos(window, nullptr, 0, 0, area.right - area.left, area.bottom - area.top,
                                        SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        const auto result = moved ? updateView(view, *size) : kResultFalse;
        resizing = false;
        return result;
    }

    tresult updateView(IPlugView* view, ViewRect size)
    {
        if (container)
            MoveWindow(container, 0, 0, size.getWidth(), size.getHeight(), TRUE);
        ViewRect current{};
        if (view->getSize(&current) == kResultOk && current.getWidth() == size.getWidth() &&
            current.getHeight() == size.getHeight())
            return kResultOk;
        return view->onSize(&size);
    }
};

struct Window {
    AraSession& session;
    IPtr<IPlugView> view;
    IPtr<EditorFrame> frame;
    const std::filesystem::path& sizeFile;
    HANDLE transportInput = nullptr;
    std::string transportPending;
    bool attached = false;
    bool resizable = false;
    bool saveKeyRegistered = false;
    ViewRect minimum{0, 0, 420, 300};
    ViewRect normalSize{};
    std::exception_ptr failure;
    HWND owner = nullptr;
    HANDLE bitwig = nullptr;
    DWORD bitwigPid = 0;
    RECT floatingBounds{};
    RECT panelBounds{};
    RECT dockBounds{};

    ~Window()
    {
        detach();
        if (frame->window)
            DestroyWindow(frame->window);
        if (bitwig)
            CloseHandle(bitwig);
    }

    void place()
    {
        const auto hwnd = frame->window;
        auto bounds = panelBounds;
        auto target = bounds.right > bounds.left ? owner : nullptr;
        if (target) {
            if (IsIconic(target))
                return;
            RECT client{};
            const auto panelDpi = GetDpiForWindow(target);
            const auto editorDpi = GetDpiForWindow(hwnd);
            if (panelDpi && editorDpi && panelDpi != editorDpi) {
                // Bitwig 给的是设备像素，窗口 API 用编辑器自己的 DPI 单位。
                bounds = {MulDiv(bounds.left, editorDpi, panelDpi), MulDiv(bounds.top, editorDpi, panelDpi),
                          MulDiv(bounds.right, editorDpi, panelDpi), MulDiv(bounds.bottom, editorDpi, panelDpi)};
            }
            POINT origin{};
            if (!GetClientRect(target, &client) || !ClientToScreen(target, &origin) ||
                bounds.left < 0 || bounds.top < 0 ||
                bounds.right > client.right || bounds.bottom > client.bottom ||
                bounds.right - bounds.left < minimum.right || bounds.bottom - bounds.top < minimum.bottom)
                target = nullptr;
            else
                OffsetRect(&bounds, origin.x, origin.y);
        }
        if (!target && !frame->docked)
            return;
        if (target && frame->docked && EqualRect(&bounds, &dockBounds))
            return;
        // 原型只认唯一的 Bitwig 主窗。多开窗口时留在悬浮状态，避免钻进隔壁工程。
        if (target && !frame->docked && bitwigOwner(bitwigPid, false) != target)
            return;
        if (target && !frame->docked) {
            if (IsZoomed(hwnd))
                ShowWindow(hwnd, SW_RESTORE);
            GetWindowRect(hwnd, &floatingBounds);
            // Bitwig 自己绘制面板；让同一个拥有者窗口对齐到面板里，保留编辑器和焦点链。
            frame->docked = true;
            SetWindowLongPtrW(hwnd, GWL_STYLE, WS_POPUP | WS_CLIPCHILDREN | WS_VISIBLE);
            std::clog << "Melodyne docked\n";
        } else if (!target) {
            frame->docked = false;
            SetWindowLongPtrW(hwnd, GWL_STYLE, floatingStyle() | WS_VISIBLE);
            bounds = floatingBounds;
            std::clog << "Melodyne floated\n";
        }
        dockBounds = bounds;
        SetWindowPos(hwnd, HWND_TOP, bounds.left, bounds.top, bounds.right - bounds.left, bounds.bottom - bounds.top,
                     SWP_FRAMECHANGED | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    }

    DWORD floatingStyle() const
    {
        return baseStyle | (resizable ? WS_THICKFRAME | WS_MAXIMIZEBOX : 0);
    }

    void detach()
    {
        if (attached) {
            view->removed();
            attached = false;
        }
        if (view) {
            view->setFrame(nullptr);
            view.reset();
        }
    }

    void setSaveKeyActive(bool active)
    {
        // 仅前台窗口临时接住 Ctrl+S，离开就归还；不能把别的软件的保存键扣下来。
        if (active && !saveKeyRegistered && session.canRequestProjectSave())
            saveKeyRegistered = RegisterHotKey(frame->window, projectSaveKey, MOD_CONTROL | MOD_NOREPEAT, 'S') != FALSE;
        if (!active && saveKeyRegistered) {
            UnregisterHotKey(frame->window, projectSaveKey);
            saveKeyRegistered = false;
        }
    }

    void forwardProjectSave()
    {
        try {
            session.requestProjectSave();
        } catch (const std::exception& error) {
            std::cerr << "BwARA: " << error.what() << '\n';
        }
    }

    void pollTransport()
    {
        if (!transportInput || transportInput == INVALID_HANDLE_VALUE)
            return;
        DWORD available = 0;
        if (!PeekNamedPipe(transportInput, nullptr, 0, nullptr, &available, nullptr)) {
            transportInput = nullptr;
            return;
        }
        std::array<char, 4096> buffer{};
        DWORD count = 0;
        if (!available || !ReadFile(transportInput, buffer.data(),
                std::min<DWORD>(available, static_cast<DWORD>(buffer.size())), &count, nullptr))
            return;

        transportPending.append(buffer.data(), count);
        const auto newline = transportPending.rfind('\n');
        if (newline == std::string::npos) {
            if (transportPending.size() > 256)
                transportPending.clear();
            return;
        }
        const auto previous = newline ? transportPending.rfind('\n', newline - 1) : std::string::npos;
        // 光标只关心最新完整坐标，不排队补播旧坐标；末尾半行留到下次拼起来。
        const auto start = previous == std::string::npos ? 0 : previous + 1;
        if (newline > start) {
            std::istringstream packet(transportPending.substr(start, newline - start));
            int mode = 0;
            double seconds = 0;
            if (packet >> mode >> seconds && mode >= 0 && mode <= 2 && std::isfinite(seconds)) {
                session.updateTransport(mode, seconds);
                LONG x, y, width, height;
                panelBounds = (packet >> x >> y >> width >> height)
                    ? RECT{x, y, x + width, y + height} : RECT{};
            }
        }
        transportPending.erase(0, newline + 1);
    }

    ViewRect constrain(ViewRect size)
    {
        size.right = std::max(size.right, minimum.right);
        size.bottom = std::max(size.bottom, minimum.bottom);
        auto checked = size;
        if (view->checkSizeConstraint(&checked) == kResultOk && validSize(checked))
            return checked;
        return size;
    }

    void restoreSize()
    {
        ViewRect size{0, 0, 860, 600};
        ViewRect supplied{};
        if (view->getSize(&supplied) == kResultOk && validSize(supplied))
            size = supplied;
        if (resizable) {
            ViewRect saved{};
            if (!sizeFile.empty()) {
                std::ifstream in(sizeFile);
                if (in >> saved.right >> saved.bottom && validSize(saved))
                    size = saved;
            }
            MONITORINFO display{sizeof(display)};
            if (GetMonitorInfoW(MonitorFromWindow(frame->window, MONITOR_DEFAULTTONEAREST), &display)) {
                const auto border = outerSize(frame->window, ViewRect{});
                size.right = std::min<int32>(size.right, display.rcWork.right - display.rcWork.left - border.right + border.left);
                size.bottom = std::min<int32>(size.bottom, display.rcWork.bottom - display.rcWork.top - border.bottom + border.top);
            }
            size = constrain(size);
        }
        frame->resizeView(view.get(), &size);
        normalSize = size;
    }

    static LRESULT CALLBACK proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
    {
        if (message == WM_NCCREATE) {
            auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
            auto* owner = static_cast<Window*>(create->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(owner));
            owner->frame->window = hwnd;
        }
        auto* owner = reinterpret_cast<Window*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (!owner)
            return DefWindowProcW(hwnd, message, wparam, lparam);

        switch (message) {
        case WM_ACTIVATE: {
            const bool active = LOWORD(wparam) != WA_INACTIVE && !HIWORD(wparam);
            owner->setSaveKeyActive(active);
            if (owner->attached)
                owner->view->onFocus(active);
            break;
        }
        case WM_HOTKEY:
            if (wparam == projectSaveKey && GetForegroundWindow() == hwnd) {
                owner->forwardProjectSave();
                return 0;
            }
            break;
        case WM_GETMINMAXINFO:
            if (owner->resizable) {
                auto* info = reinterpret_cast<MINMAXINFO*>(lparam);
                const auto area = outerSize(hwnd, owner->constrain(owner->minimum));
                info->ptMinTrackSize = {area.right - area.left, area.bottom - area.top};
                return 0;
            }
            break;
        case WM_SIZING:
            // 先征求插件的尺寸约束，再调整用户正在拖的那条边，鼠标另一侧保持不动。
            if (owner->resizable && owner->attached) {
                auto& bounds = *reinterpret_cast<RECT*>(lparam);
                const auto border = outerSize(hwnd, ViewRect{});
                auto size = owner->constrain({0, 0, static_cast<int32>(bounds.right - bounds.left - border.right + border.left),
                                                  static_cast<int32>(bounds.bottom - bounds.top - border.bottom + border.top)});
                const auto area = outerSize(hwnd, size);
                if (wparam == WMSZ_LEFT || wparam == WMSZ_TOPLEFT || wparam == WMSZ_BOTTOMLEFT)
                    bounds.left = bounds.right - (area.right - area.left);
                else
                    bounds.right = bounds.left + area.right - area.left;
                if (wparam == WMSZ_TOP || wparam == WMSZ_TOPLEFT || wparam == WMSZ_TOPRIGHT)
                    bounds.top = bounds.bottom - (area.bottom - area.top);
                else
                    bounds.bottom = bounds.top + area.bottom - area.top;
                return TRUE;
            }
            break;
        case WM_SIZE: {
            if (wparam == SIZE_MINIMIZED || !owner->attached || owner->frame->resizing)
                return 0;
            RECT client{};
            GetClientRect(hwnd, &client);
            ViewRect size{0, 0, static_cast<int32>(client.right), static_cast<int32>(client.bottom)};
            if (validSize(size)) {
                owner->frame->resizing = true;
                owner->frame->updateView(owner->view.get(), size);
                owner->frame->resizing = false;
                if (wparam == SIZE_RESTORED && !owner->frame->docked)
                    owner->normalSize = size;
            }
            return 0;
        }
        case WM_TIMER:
            try {
                if (owner->session.projectClosed() ||
                    (owner->bitwig && WaitForSingleObject(owner->bitwig, 0) != WAIT_TIMEOUT)) {
                    PostMessageW(hwnd, WM_CLOSE, 0, 0);
                    return 0;
                }
                owner->pollTransport();
                owner->place();
                owner->session.poll();
                owner->session.updateLiveRendering();
            } catch (...) {
                owner->failure = std::current_exception();
                PostMessageW(hwnd, WM_CLOSE, 0, 0);
            }
            return 0;
        case WM_CLOSE:
            KillTimer(hwnd, 1);
            owner->session.stopTransport();
            if (owner->resizable && !owner->sizeFile.empty() && validSize(owner->normalSize)) {
                std::ofstream out(owner->sizeFile);
                out << owner->normalSize.right << ' ' << owner->normalSize.bottom << '\n';
            }
            owner->detach();
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        case WM_NCDESTROY:
            owner->setSaveKeyActive(false);
            owner->frame->window = nullptr;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            break;
        }
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }
};
}

void editInWindow(AraSession& session, bool followTransport, const std::filesystem::path& sizeFile,
                  unsigned long bitwigPid)
{
    Window window{session, owned(session.createEditor()), owned(new EditorFrame), sizeFile};
    if (!window.view || window.view->isPlatformTypeSupported(kPlatformTypeHWND) != kResultOk)
        throw std::runtime_error("The ARA plugin has no Windows editor.");

    const auto program = GetModuleHandleW(nullptr);
    WNDCLASSW kind{};
    kind.lpfnWndProc = Window::proc;
    kind.hInstance = program;
    kind.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    kind.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
    kind.lpszClassName = L"BwAraEditor";
    if (!RegisterClassW(&kind) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        throw std::runtime_error("Could not register the editor window.");

    window.resizable = window.view->canResize() == kResultTrue;
    window.bitwigPid = bitwigPid;
    window.bitwig = bitwigPid ? OpenProcess(SYNCHRONIZE, FALSE, bitwigPid) : nullptr;
    window.owner = bitwigOwner(bitwigPid, true);
    const auto hwnd = CreateWindowExW(WS_EX_APPWINDOW, kind.lpszClassName, L"Melodyne - BwARA",
        window.floatingStyle(),
        CW_USEDEFAULT, CW_USEDEFAULT, 870, 630, window.owner, nullptr, program, &window);
    if (!hwnd)
        throw std::runtime_error("Could not create the editor window.");
    window.frame->container = CreateWindowExW(0, L"STATIC", nullptr, WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN,
        0, 0, 0, 0, hwnd, nullptr, program, nullptr);
    if (!window.frame->container)
        throw std::runtime_error("Could not create the editor container.");

    if (followTransport)
        window.transportInput = GetStdHandle(STD_INPUT_HANDLE);

    window.view->setFrame(window.frame.get());
    window.restoreSize();
    if (window.view->attached(window.frame->container, kPlatformTypeHWND) != kResultOk)
        throw std::runtime_error("The ARA plugin could not attach its editor.");
    window.attached = true;
    SendMessageW(hwnd, WM_SIZE, SIZE_RESTORED, 0);
    session.selectRegion();
    if (followTransport)
        session.startTransport();
    SetTimer(hwnd, 1, 24, nullptr);
    ShowWindow(hwnd, SW_SHOW);

    MSG message{};
    BOOL status;
    while ((status = GetMessageW(&message, nullptr, 0, 0)) > 0) {
        if (message.message == WM_KEYDOWN && (message.hwnd == hwnd || IsChild(hwnd, message.hwnd)) &&
            GetKeyState(VK_SHIFT) >= 0 && GetKeyState(VK_MENU) >= 0) {
            const bool control = GetKeyState(VK_CONTROL) < 0;
            const bool repeat = (message.lParam & (1L << 30)) != 0;
            if (message.wParam == VK_SPACE && !control && followTransport) {
                wchar_t name[32]{};
                GetClassNameW(message.hwnd, name, 32);
                if (lstrcmpiW(name, L"Edit") != 0) {
                    if (!repeat)
                        bwaraTransport("toggle", 0);
                    continue;
                }
            }
            if (message.wParam == 'S' && control && !repeat &&
                !window.saveKeyRegistered && session.canRequestProjectSave()) {
                window.forwardProjectSave();
                continue;
            }
        }
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    if (status < 0)
        throw std::runtime_error("The editor message loop failed.");
    if (window.failure)
        std::rethrow_exception(window.failure);
}


