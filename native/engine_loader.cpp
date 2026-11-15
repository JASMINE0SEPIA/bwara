#include "engine_control.h"

#include <bcrypt.h>
#include <tlhelp32.h>
#include <array>
#include <chrono>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;
using namespace bwara;

namespace {

fs::path imagePath(HANDLE process)
{
    std::wstring buffer(32768, L'\0');
    DWORD length = static_cast<DWORD>(buffer.size());
    if (!QueryFullProcessImageNameW(process, 0, buffer.data(), &length))
        return {};
    buffer.resize(length);
    return buffer;
}

bool supportedImage(const fs::path& image)
{
    // 精确认版本，认不出来就停止。私有地址上“试试看”的代价是整个音频引擎。
    std::ifstream input(image, std::ios::binary | std::ios::ate);
    if (!input || input.tellg() != 73634720)
        return false;
    std::vector<UCHAR> file(static_cast<size_t>(input.tellg()));
    input.seekg(0);
    if (!input.read(reinterpret_cast<char*>(file.data()), file.size()))
        return false;
    std::array<UCHAR, 32> hash{};
    if (BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, file.data(), static_cast<ULONG>(file.size()),
                   hash.data(), static_cast<ULONG>(hash.size())) < 0)
        return false;
    char hex[65]{};
    for (size_t index = 0; index < hash.size(); ++index)
        sprintf_s(hex + index * 2, 3, "%02x", hash[index]);
    return std::string_view(hex) == engineSha256;
}

uintptr_t moduleBase(DWORD pid, const wchar_t* name)
{
    Handle list(CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, pid));
    MODULEENTRY32W module{sizeof(module)};
    if (list && Module32FirstW(list, &module)) {
        do {
            if (_wcsicmp(module.szModule, name) == 0)
                return reinterpret_cast<uintptr_t>(module.modBaseAddr);
        } while (Module32NextW(list, &module));
    }
    return 0;
}

bool loadModule(HANDLE process, DWORD pid, const fs::path& path)
{
    if (moduleBase(pid, moduleName))
        return true;
    const auto load = GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW");
    HMODULE containing = nullptr;
    wchar_t kernel[MAX_PATH]{};
    if (!load || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(load), &containing) ||
        !GetModuleFileNameW(containing, kernel, MAX_PATH))
        return false;
    const auto remoteBase = moduleBase(pid, fs::path(kernel).filename().c_str());
    if (!remoteBase)
        return false;
    const auto remoteEntry = remoteBase + reinterpret_cast<uintptr_t>(load) - reinterpret_cast<uintptr_t>(containing);
    const auto bytes = (path.native().size() + 1) * sizeof(wchar_t);
    void* argument = VirtualAllocEx(process, nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!argument)
        return false;
    Handle worker;
    if (WriteProcessMemory(process, argument, path.c_str(), bytes, nullptr))
        worker = Handle(CreateRemoteThread(process, nullptr, 0, reinterpret_cast<LPTHREAD_START_ROUTINE>(remoteEntry),
                                           argument, 0, nullptr));
    const bool finished = worker && WaitForSingleObject(worker, 3000) == WAIT_OBJECT_0;
    // 远程线程超时后可能还在读这个参数，不能急着释放。
    if (!worker || finished)
        VirtualFreeEx(process, argument, 0, MEM_RELEASE);
    return finished && moduleBase(pid, moduleName);
}

struct Link {
    Handle process;
    Handle mapping;
    EngineControl* control = nullptr;
    LONG64 reported = -1;

    ~Link()
    {
        if (control) {
            InterlockedCompareExchange(&control->ownerPid, 0, static_cast<LONG>(GetCurrentProcessId()));
            UnmapViewOfFile(control);
        }
    }
};

std::unique_ptr<Link> connect(DWORD pid, const fs::path& engine, const fs::path& module, const fs::path& directory)
{
    auto link = std::make_unique<Link>();
    link->process = Handle(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_CREATE_THREAD |
        PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ | SYNCHRONIZE, FALSE, pid));
    if (!link->process || _wcsicmp(imagePath(link->process).c_str(), engine.c_str()) != 0)
        return {};
    const auto name = controlName(pid);
    link->mapping = Handle(CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
        sizeof(EngineControl), name.c_str()));
    if (!link->mapping)
        return {};
    link->control = static_cast<EngineControl*>(MapViewOfFile(link->mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(EngineControl)));
    if (!link->control)
        return {};
    const auto existing = InterlockedCompareExchange(&link->control->ownerPid, 0, 0);
    Handle owner(existing > 0 ? OpenProcess(SYNCHRONIZE, FALSE, existing) : nullptr);
    if (owner && WaitForSingleObject(owner, 0) == WAIT_TIMEOUT)
        return {};
    InterlockedExchange(&link->control->ownerPid, 0);
    if (wcscpy_s(link->control->directory, directory.c_str()) != 0)
        return {};
    InterlockedExchange64(&link->control->hits, 0);
    InterlockedExchange(&link->control->ownerPid, static_cast<LONG>(GetCurrentProcessId()));
    if (!loadModule(link->process, pid, module))
        return {};
    return link;
}

std::vector<DWORD> engines(DWORD ui)
{
    // 只找这个 Bitwig UI 的子孙进程；另一份宿主恰好也开着，不代表它邀请了我们。
    std::vector<DWORD> found;
    std::map<DWORD, DWORD> parents;
    std::vector<DWORD> candidates;
    Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    PROCESSENTRY32W entry{sizeof(entry)};
    if (snapshot && Process32FirstW(snapshot, &entry)) {
        do {
            parents.emplace(entry.th32ProcessID, entry.th32ParentProcessID);
            if (_wcsicmp(entry.szExeFile, engineName) == 0)
                candidates.push_back(entry.th32ProcessID);
        } while (Process32NextW(snapshot, &entry));
    }
    for (const auto pid : candidates) {
        auto ancestor = pid;
        for (int depth = 0; depth < 8 && parents.count(ancestor); ++depth) {
            ancestor = parents.at(ancestor);
            if (ancestor == ui) {
                found.push_back(pid);
                break;
            }
        }
    }
    return found;
}
}

int wmain(int argc, wchar_t** argv)
{
    if (argc != 3)
        return 2;
    try {
        const auto ui = static_cast<DWORD>(std::stoul(argv[1]));
        const auto directory = fs::absolute(argv[2]);
        Handle bitwig(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, ui));
        const auto image = bitwig ? imagePath(bitwig) : fs::path{};
        if (_wcsicmp(image.filename().c_str(), L"BitwigStudioApp.exe") != 0)
            throw std::runtime_error("The Bitwig UI process is unavailable.");
        const auto engine = image.parent_path() / engineName;
        const auto module = fs::absolute(argv[0]).parent_path() / moduleName;
        if (!supportedImage(engine) || !fs::is_regular_file(module))
            throw std::runtime_error("This native adapter requires Bitwig Studio 6.1.7.24824 Windows x64 AVX2.");

        std::map<DWORD, std::unique_ptr<Link>> links;
        std::map<DWORD, std::chrono::steady_clock::time_point> retry;
        while (WaitForSingleObject(bitwig, 0) == WAIT_TIMEOUT && !fs::exists(directory / "engine-stop")) {
            const auto now = std::chrono::steady_clock::now();
            for (const auto pid : engines(ui)) {
                if (!links.count(pid) && now >= retry[pid]) {
                    if (auto link = connect(pid, engine, module, directory))
                        links.emplace(pid, std::move(link));
                    retry[pid] = now + std::chrono::seconds(1);
                }
            }
            for (auto it = links.begin(); it != links.end();) {
                const auto pid = it->first;
                auto& link = *it->second;
                if (WaitForSingleObject(link.process, 0) != WAIT_TIMEOUT) {
                    it = links.erase(it);
                    continue;
                }
                if (InterlockedCompareExchange(&link.control->adapterState, 0, 0) == 1) {
                    if (!fs::exists(directory / "engine-ready"))
                        std::ofstream(directory / "engine-ready") << pid << '\n';
                    const auto count = InterlockedCompareExchange64(&link.control->hits, 0, 0);
                    if (count != link.reported) {
                        std::ofstream(directory / (L"engine-" + std::to_wstring(pid) + L".stats")) << count << '\n';
                        link.reported = count;
                    }
                }
                ++it;
            }
            Sleep(30);
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "BwARA engine: " << error.what() << '\n';
        return 1;
    }
}

