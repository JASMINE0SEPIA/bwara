#pragma once

#include <windows.h>
#include <string>

namespace bwara {
constexpr wchar_t engineName[] = L"BitwigAudioEngine-X64-AVX2.exe";
constexpr wchar_t moduleName[] = L"bwara-engine.dll";
constexpr char engineSha256[] = "8023aa0607b5cb841d1dbbc2804bd5440befdd2da86dc37df67e3a336c52d721";

inline std::wstring controlName(DWORD pid)
{
    return L"Local\\BwARA.Engine.1." + std::to_wstring(pid);
}

struct EngineControl {
    volatile LONG adapterState; // 0 启动中，1 就绪，-1 不支持；跨进程访问用 Interlocked。
    volatile LONG ownerPid;     // 目录写完后才公布 PID；会话存活时目录不变。
    alignas(8) volatile LONG64 hits;
    wchar_t directory[4096];
};

class Handle {
public:
    explicit Handle(HANDLE value = nullptr) : value(value) {}
    ~Handle() { if (*this) CloseHandle(value); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    Handle(Handle&& other) noexcept : value(other.value) { other.value = nullptr; }
    Handle& operator=(Handle&& other) noexcept
    {
        if (this != &other) {
            if (*this)
                CloseHandle(value);
            value = other.value;
            other.value = nullptr;
        }
        return *this;
    }
    explicit operator bool() const { return value && value != INVALID_HANDLE_VALUE; }
    operator HANDLE() const { return value; }
private:
    HANDLE value;
};
}
