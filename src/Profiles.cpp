#include "Profiles.h"

#include "AppCore.h"

#include <shellapi.h>

#include <algorithm>
#include <cwchar>
#include <iterator>
#include <limits>
#include <vector>

namespace fs = std::filesystem;

namespace {

std::wstring ReadIniValue(const fs::path& iniPath, const wchar_t* section,
                          const wchar_t* key) {
    std::vector<wchar_t> buffer(512);
    for (;;) {
        const DWORD copied = GetPrivateProfileStringW(
            section, key, L"", buffer.data(), static_cast<DWORD>(buffer.size()),
            iniPath.c_str());
        if (copied < buffer.size() - 1) {
            return std::wstring(buffer.data(), copied);
        }
        buffer.resize(buffer.size() * 2);
    }
}

std::vector<std::wstring> ReadIniSections(const fs::path& iniPath) {
    std::vector<wchar_t> buffer(1024);
    DWORD copied = 0;
    for (;;) {
        copied = GetPrivateProfileSectionNamesW(
            buffer.data(), static_cast<DWORD>(buffer.size()), iniPath.c_str());
        if (copied < buffer.size() - 2) {
            break;
        }
        buffer.resize(buffer.size() * 2);
    }
    std::vector<std::wstring> sections;
    const wchar_t* current = buffer.data();
    const wchar_t* const finish = buffer.data() + copied;
    while (current < finish && *current != L'\0') {
        const size_t length = std::wcslen(current);
        sections.emplace_back(current, length);
        current += length + 1;
    }
    return sections;
}

bool WriteValue(const fs::path& ini, const wchar_t* section, const wchar_t* key,
                const wchar_t* value, std::wstring& error) {
    if (WritePrivateProfileStringW(section, key, value, ini.c_str()) == FALSE) {
        error = FormatSystemError(GetLastError());
        return false;
    }
    return true;
}

void FlushIni(const fs::path& ini) {
    WritePrivateProfileStringW(nullptr, nullptr, nullptr, ini.c_str());
}

} // namespace

ProfileStore::ProfileStore(fs::path iniPath, fs::path applicationDirectory)
    : iniPath_(std::move(iniPath)),
      applicationDirectory_(std::move(applicationDirectory)) {}

const fs::path& ProfileStore::IniPath() const noexcept { return iniPath_; }
const fs::path& ProfileStore::ApplicationDirectory() const noexcept {
    return applicationDirectory_;
}

std::vector<Profile> ProfileStore::Load() const {
    std::vector<Profile> profiles;
    int sectionIndex = 0;
    for (const std::wstring& section : ReadIniSections(iniPath_)) {
        if (EqualsInsensitive(section, L"General")) {
            continue;
        }
        std::wstring name = Trim(ReadIniValue(iniPath_, section.c_str(), L"Name"));
        if (name.empty()) {
            name = section;
        }
        std::wstring pathText = Trim(ReadIniValue(iniPath_, section.c_str(), L"Path"));
        fs::path executable;
        if (!pathText.empty()) {
            executable = fs::path(pathText);
            if (executable.is_relative()) {
                executable = applicationDirectory_ / executable;
            }
            executable = executable.lexically_normal();
        }
        const std::wstring orderText =
            Trim(ReadIniValue(iniPath_, section.c_str(), L"Order"));
        int order = 1'000'000 + sectionIndex;
        if (!orderText.empty()) {
            wchar_t* end = nullptr;
            const long parsed = std::wcstol(orderText.c_str(), &end, 10);
            if (end != orderText.c_str() && *end == L'\0' && parsed >= 0 &&
                parsed <= std::numeric_limits<int>::max()) {
                order = static_cast<int>(parsed);
            }
        }
        profiles.push_back({section, name, executable, order});
        ++sectionIndex;
    }
    std::stable_sort(profiles.begin(), profiles.end(),
                     [](const Profile& left, const Profile& right) {
                         return left.order < right.order;
                     });
    return profiles;
}

std::wstring ProfileStore::DefaultId() const {
    return Trim(ReadIniValue(iniPath_, L"General", L"DefaultProfile"));
}

bool ProfileStore::SkipProfilePicker() const {
    const std::wstring value =
        Trim(ReadIniValue(iniPath_, L"General", L"SkipProfilePicker"));
    return EqualsInsensitive(value, L"1") || EqualsInsensitive(value, L"true") ||
           EqualsInsensitive(value, L"yes");
}

bool ProfileStore::LoggingEnabled() const {
    const std::wstring value =
        Trim(ReadIniValue(iniPath_, L"General", L"EnableLogging"));
    return EqualsInsensitive(value, L"1") || EqualsInsensitive(value, L"true") ||
           EqualsInsensitive(value, L"yes");
}

int ProfileStore::AutoLaunchSeconds() const {
    const std::wstring value =
        Trim(ReadIniValue(iniPath_, L"General", L"AutoLaunchSeconds"));
    if (value.empty()) return 3;
    wchar_t* end = nullptr;
    const long parsed = std::wcstol(value.c_str(), &end, 10);
    if (end == value.c_str() || *end != L'\0' || parsed < 0 || parsed > 3600) {
        return 3;
    }
    return static_cast<int>(parsed);
}

bool ProfileStore::Save(const Profile& profile, std::wstring_view originalId,
                        std::wstring& error) const {
    int order = profile.order;
    const std::vector<Profile> existingProfiles = Load();
    const bool isFirstProfile = originalId.empty() && existingProfiles.empty();
    if (!originalId.empty()) {
        if (const Profile* existing = FindProfile(existingProfiles, originalId)) {
            order = existing->order;
        }
    } else {
        order = existingProfiles.empty()
                    ? 0
                    : existingProfiles.back().order + 1;
    }
    if (!WriteValue(iniPath_, profile.id.c_str(), L"Name", profile.name.c_str(), error) ||
        !WriteValue(iniPath_, profile.id.c_str(), L"Path",
                    profile.executable.c_str(), error) ||
        !WriteValue(iniPath_, profile.id.c_str(), L"Order",
                    std::to_wstring(order).c_str(), error)) {
        return false;
    }

    const std::wstring oldId(originalId);
    if (!oldId.empty() && !EqualsInsensitive(oldId, profile.id)) {
        const bool wasDefault = EqualsInsensitive(DefaultId(), oldId);
        if (WritePrivateProfileStringW(oldId.c_str(), nullptr, nullptr,
                                       iniPath_.c_str()) == FALSE) {
            error = FormatSystemError(GetLastError());
            return false;
        }
        if (wasDefault && !SetDefault(profile.id, error)) {
            return false;
        }
    }
    if (isFirstProfile && !SetDefault(profile.id, error)) {
        return false;
    }
    FlushIni(iniPath_);
    return true;
}

bool ProfileStore::Delete(std::wstring_view id, std::wstring& error) const {
    const std::vector<Profile> existingProfiles = Load();
    const auto deleting = std::find_if(
        existingProfiles.begin(), existingProfiles.end(),
        [id](const Profile& profile) {
            return EqualsInsensitive(profile.id, id);
        });
    const size_t deletedIndex = deleting == existingProfiles.end()
                                    ? 0
                                    : static_cast<size_t>(
                                          std::distance(existingProfiles.begin(),
                                                        deleting));
    const bool wasDefault = EqualsInsensitive(DefaultId(), id);
    const std::wstring section(id);
    if (WritePrivateProfileStringW(section.c_str(), nullptr, nullptr,
                                   iniPath_.c_str()) == FALSE) {
        error = FormatSystemError(GetLastError());
        return false;
    }
    const std::vector<Profile> remainingProfiles = Load();
    if (wasDefault) {
        std::wstring nextDefault;
        if (!remainingProfiles.empty()) {
            const size_t nextIndex = deletedIndex < remainingProfiles.size()
                                         ? deletedIndex
                                         : 0;
            nextDefault = remainingProfiles[nextIndex].id;
        }
        if (!SetDefault(nextDefault, error)) {
            return false;
        }
    }
    std::vector<std::wstring> remainingIds;
    for (const Profile& profile : remainingProfiles) {
        remainingIds.push_back(profile.id);
    }
    if (!SaveOrder(remainingIds, error)) {
        return false;
    }
    FlushIni(iniPath_);
    return true;
}

bool ProfileStore::SetDefault(std::wstring_view id, std::wstring& error) const {
    const std::wstring value(id);
    if (!WriteValue(iniPath_, L"General", L"DefaultProfile", value.c_str(), error)) {
        return false;
    }
    FlushIni(iniPath_);
    return true;
}

bool ProfileStore::SetSkipProfilePicker(bool enabled,
                                        std::wstring& error) const {
    if (!WriteValue(iniPath_, L"General", L"SkipProfilePicker",
                    enabled ? L"1" : L"0", error)) {
        return false;
    }
    FlushIni(iniPath_);
    return true;
}

bool ProfileStore::SetLoggingEnabled(bool enabled, std::wstring& error) const {
    if (!WriteValue(iniPath_, L"General", L"EnableLogging",
                    enabled ? L"1" : L"0", error)) {
        return false;
    }
    FlushIni(iniPath_);
    return true;
}

bool ProfileStore::SetAutoLaunchSeconds(int seconds, std::wstring& error) const {
    if (seconds < 0 || seconds > 3600) {
        error = L"自动启动延迟必须在 0–3600 秒之间。";
        return false;
    }
    const std::wstring value = std::to_wstring(seconds);
    if (!WriteValue(iniPath_, L"General", L"AutoLaunchSeconds",
                    value.c_str(), error)) {
        return false;
    }
    FlushIni(iniPath_);
    return true;
}

bool ProfileStore::SaveOrder(const std::vector<std::wstring>& profileIds,
                             std::wstring& error) const {
    for (size_t index = 0; index < profileIds.size(); ++index) {
        const std::wstring order = std::to_wstring(index);
        if (!WriteValue(iniPath_, profileIds[index].c_str(), L"Order",
                        order.c_str(), error)) {
            return false;
        }
    }
    FlushIni(iniPath_);
    return true;
}

bool EnsureProfilesIni(const fs::path& iniPath, std::wstring& error) {
    std::error_code pathError;
    const bool exists = fs::exists(iniPath, pathError);
    if (pathError) {
        error = L"无法检查配置文件，错误代码：" +
                std::to_wstring(pathError.value());
        return false;
    }
    if (exists) {
        return true;
    }

    static constexpr wchar_t content[] =
        L"[General]\r\n"
        L"DefaultProfile=HQ_Anime\r\n"
        L"SkipProfilePicker=0\r\n"
        L"EnableLogging=0\r\n"
        L"AutoLaunchSeconds=3\r\n"
        L"\r\n"
        L"[HQ_Anime]\r\n"
        L"Order=0\r\n"
        L"Name=画质增强版 (Anime4K + Custom Shaders)\r\n"
        L"Path=C:\\Tools\\MPV_HQ\\mpv.exe\r\n"
        L"\r\n"
        L"[Light_Player]\r\n"
        L"Order=1\r\n"
        L"Name=原生轻量版 (无脚本/极速启动)\r\n"
        L"Path=C:\\Tools\\MPV_Light\\mpv.exe\r\n";

    HANDLE file = CreateFileW(iniPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                              nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        error = FormatSystemError(GetLastError());
        return false;
    }
    constexpr WORD bom = 0xFEFF;
    DWORD written = 0;
    bool ok = WriteFile(file, &bom, sizeof(bom), &written, nullptr) != FALSE &&
              written == sizeof(bom);
    if (ok) {
        const DWORD bytes =
            static_cast<DWORD>((std::size(content) - 1) * sizeof(wchar_t));
        written = 0;
        ok = WriteFile(file, content, bytes, &written, nullptr) != FALSE &&
             written == bytes;
    }
    if (!ok) {
        error = FormatSystemError(GetLastError());
    }
    CloseHandle(file);
    return ok;
}

void OpenIniInNotepad(const fs::path& iniPath, HWND owner) {
    const std::wstring parameters = L"\"" + iniPath.wstring() + L"\"";
    const HINSTANCE result = ShellExecuteW(owner, L"open", L"notepad.exe",
                                           parameters.c_str(),
                                           iniPath.parent_path().c_str(), SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(result) <= 32) {
        ShowError(L"无法启动记事本。请手动编辑：\n" + iniPath.wstring(), owner);
    }
}

void OpenIniWithDefaultApp(const fs::path& iniPath, HWND owner) {
    const HINSTANCE result = ShellExecuteW(owner, L"open", iniPath.c_str(),
                                           nullptr, iniPath.parent_path().c_str(),
                                           SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(result) <= 32) {
        ShowError(L"无法使用系统默认应用打开配置文件：\n" +
                      iniPath.wstring(),
                  owner);
    }
}

bool IsUsableExecutable(const fs::path& path) {
    if (path.empty()) {
        return false;
    }
    std::error_code error;
    return fs::is_regular_file(path, error) && !error;
}

const Profile* FindProfile(const std::vector<Profile>& profiles,
                           std::wstring_view id) {
    const auto found = std::find_if(profiles.begin(), profiles.end(),
                                    [id](const Profile& profile) {
                                        return EqualsInsensitive(profile.id, id);
                                    });
    return found == profiles.end() ? nullptr : &*found;
}
