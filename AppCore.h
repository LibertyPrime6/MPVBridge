#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define _WIN32_WINNT 0x0A00

#include <windows.h>

#include <filesystem>
#include <string>
#include <string_view>

inline constexpr wchar_t kAppTitle[] = L"MPVBridge";

std::wstring FormatSystemError(DWORD error);
void ShowError(std::wstring_view message, HWND owner = nullptr);
void ShowInfo(std::wstring_view message, HWND owner = nullptr);
std::filesystem::path GetModulePath();
bool EqualsInsensitive(std::wstring_view left, std::wstring_view right);
std::wstring Trim(std::wstring value);
bool HasVisibleText(std::wstring_view value);

