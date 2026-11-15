#include "revision_file.h"

#include <windows.h>
#include <cstring>
#include <fstream>
#include <system_error>
#include <vector>

namespace bwara {

// 文件是两个进程之间的小信箱。先写完，再换门牌，读者只会看到旧版或完整新版。
// 临时文件和目标必须在同一目录；每个信箱只由 Melodyne 的 UI 线程写。
void publishRevision(const std::filesystem::path& marker, unsigned long long revision)
{
    auto temporary = marker;
    temporary += L".tmp";
    std::ofstream output(temporary, std::ios::binary);
    output.exceptions(std::ios::badbit | std::ios::failbit);
    output << revision << '\n';
    output.close();
    // 普通 MoveFileEx 在读者仍拿着旧文件时会失败。POSIX 替换让旧读者读完旧版，
    // 新读者打开新版，无需暂停对方，也不用靠重试碰运气。
    const auto name = std::filesystem::absolute(marker).native();
    const auto bytes = static_cast<DWORD>(name.size() * sizeof(wchar_t));
    std::vector<unsigned char> storage(sizeof(FILE_RENAME_INFO) + bytes, 0);
    auto* rename = reinterpret_cast<FILE_RENAME_INFO*>(storage.data());
    rename->Flags = FILE_RENAME_FLAG_REPLACE_IF_EXISTS | FILE_RENAME_FLAG_POSIX_SEMANTICS;
    rename->FileNameLength = bytes;
    std::memcpy(rename->FileName, name.data(), bytes);
    const auto file = CreateFileW(temporary.c_str(), DELETE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                 nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        throw std::system_error(GetLastError(), std::system_category(), "Could not open the Melodyne revision");
    const bool published = SetFileInformationByHandle(file, FileRenameInfoEx, rename, static_cast<DWORD>(storage.size()));
    const auto error = GetLastError();
    CloseHandle(file);
    if (!published)
        throw std::system_error(error, std::system_category(), "Could not publish the Melodyne revision");
}

}
