#include "AppCore.h"

#include <algorithm>
#include <cwctype>
#include <stdexcept>
#include <vector>

std::wstring FormatSystemError(DWORD error) {
    wchar_t* buffer = nullptr;
    const DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER |
                        FORMAT_MESSAGE_FROM_SYSTEM |
                        FORMAT_MESSAGE_IGNORE_INSERTS;
    const DWORD length = FormatMessageW(flags, nullptr, error, 0,
                                        reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
    std::wstring message = length != 0 && buffer != nullptr
                               ? std::wstring(buffer, length)
                               : L"未知系统错误";
    if (buffer != nullptr) {
        LocalFree(buffer);
    }
    while (!message.empty() && std::iswspace(message.back()) != 0) {
        message.pop_back();
    }
    return message;
}

void ShowError(std::wstring_view message, HWND owner) {
    const std::wstring copy(message);
    MessageBoxW(owner, copy.c_str(), kAppTitle,
                MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
}

void ShowInfo(std::wstring_view message, HWND owner) {
    const std::wstring copy(message);
    MessageBoxW(owner, copy.c_str(), kAppTitle,
                MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND);
}

std::filesystem::path GetModulePath() {
    std::vector<wchar_t> buffer(512);
    for (;;) {
        const DWORD copied = GetModuleFileNameW(nullptr, buffer.data(),
                                                static_cast<DWORD>(buffer.size()));
        if (copied == 0) {
            throw std::runtime_error("GetModuleFileNameW failed");
        }
        if (copied < buffer.size() - 1) {
            return std::filesystem::path(std::wstring(buffer.data(), copied));
        }
        buffer.resize(buffer.size() * 2);
    }
}

bool EqualsInsensitive(std::wstring_view left, std::wstring_view right) {
    return left.size() == right.size() &&
           CompareStringOrdinal(left.data(), static_cast<int>(left.size()),
                                right.data(), static_cast<int>(right.size()), TRUE) ==
               CSTR_EQUAL;
}

std::wstring Trim(std::wstring value) {
    const auto isSpace = [](wchar_t ch) { return std::iswspace(ch) != 0; };
    const auto first = std::find_if_not(value.begin(), value.end(), isSpace);
    const auto last = std::find_if_not(value.rbegin(), value.rend(), isSpace).base();
    if (first >= last) {
        return {};
    }
    return std::wstring(first, last);
}

bool HasVisibleText(std::wstring_view value) {
    return std::any_of(value.begin(), value.end(),
                       [](wchar_t ch) { return std::iswspace(ch) == 0; });
}

