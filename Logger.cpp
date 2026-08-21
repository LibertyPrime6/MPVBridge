#include "Logger.h"

#include "AppCore.h"

#include <shellapi.h>

#include <string>
#include <vector>

namespace {

std::filesystem::path LogPath(const ProfileStore& store) {
    return store.ApplicationDirectory() / L"MPVBridge.log";
}

std::string ToUtf8(std::wstring_view text) {
    if (text.empty()) {
        return {};
    }
    const int length = WideCharToMultiByte(CP_UTF8, 0, text.data(),
                                           static_cast<int>(text.size()), nullptr,
                                           0, nullptr, nullptr);
    std::string result(static_cast<size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                        result.data(), length, nullptr, nullptr);
    return result;
}

bool EnsureLogFile(const std::filesystem::path& path) {
    HANDLE file = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ |
                              FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    LARGE_INTEGER size{};
    if (GetFileSizeEx(file, &size) != FALSE && size.QuadPart == 0) {
        static constexpr BYTE bom[] = {0xEF, 0xBB, 0xBF};
        DWORD written = 0;
        WriteFile(file, bom, static_cast<DWORD>(sizeof(bom)), &written, nullptr);
    }
    CloseHandle(file);
    return true;
}

} // namespace

void WriteDiagnosticLog(const ProfileStore& store, std::wstring_view message) {
    if (!store.LoggingEnabled()) {
        return;
    }
    const std::filesystem::path path = LogPath(store);
    HANDLE file = CreateFileW(path.c_str(), FILE_APPEND_DATA,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                              OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return;
    }
    LARGE_INTEGER size{};
    if (GetFileSizeEx(file, &size) != FALSE && size.QuadPart == 0) {
        static constexpr BYTE bom[] = {0xEF, 0xBB, 0xBF};
        DWORD written = 0;
        WriteFile(file, bom, static_cast<DWORD>(sizeof(bom)), &written, nullptr);
    }

    SYSTEMTIME time{};
    GetLocalTime(&time);
    wchar_t prefix[64]{};
    swprintf_s(prefix, L"%04u-%02u-%02u %02u:%02u:%02u [INFO] ",
               time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute,
               time.wSecond);
    const std::string line = ToUtf8(std::wstring(prefix) + std::wstring(message) +
                                    L"\r\n");
    DWORD written = 0;
    WriteFile(file, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
    CloseHandle(file);
}

void OpenDiagnosticLog(const ProfileStore& store, HWND owner) {
    const std::filesystem::path path = LogPath(store);
    if (!EnsureLogFile(path)) {
        ShowError(L"无法创建日志文件：\n" + path.wstring(), owner);
        return;
    }
    const std::wstring parameters = L"\"" + path.wstring() + L"\"";
    const HINSTANCE result = ShellExecuteW(owner, L"open", L"notepad.exe",
                                           parameters.c_str(),
                                           path.parent_path().c_str(), SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(result) <= 32) {
        ShowError(L"无法打开日志文件：\n" + path.wstring(), owner);
    }
}

