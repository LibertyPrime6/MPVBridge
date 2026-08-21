#pragma once

#include "AppCore.h"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

struct Profile {
    std::wstring id;
    std::wstring name;
    std::filesystem::path executable;
    int order{};
};

class ProfileStore {
public:
    ProfileStore(std::filesystem::path iniPath,
                 std::filesystem::path applicationDirectory);

    const std::filesystem::path& IniPath() const noexcept;
    const std::filesystem::path& ApplicationDirectory() const noexcept;

    std::vector<Profile> Load() const;
    std::wstring DefaultId() const;
    bool LoggingEnabled() const;
    int AutoLaunchSeconds() const;
    bool Save(const Profile& profile, std::wstring_view originalId,
              std::wstring& error) const;
    bool Delete(std::wstring_view id, std::wstring& error) const;
    bool SetDefault(std::wstring_view id, std::wstring& error) const;
    bool SetLoggingEnabled(bool enabled, std::wstring& error) const;
    bool SetAutoLaunchSeconds(int seconds, std::wstring& error) const;
    bool SaveOrder(const std::vector<std::wstring>& profileIds,
                   std::wstring& error) const;

private:
    std::filesystem::path iniPath_;
    std::filesystem::path applicationDirectory_;
};

bool EnsureProfilesIni(const std::filesystem::path& iniPath, bool& created,
                       std::wstring& error);
void OpenIniInNotepad(const std::filesystem::path& iniPath, HWND owner = nullptr);
void OpenIniWithDefaultApp(const std::filesystem::path& iniPath,
                           HWND owner = nullptr);
bool IsUsableExecutable(const std::filesystem::path& path);
const Profile* FindProfile(const std::vector<Profile>& profiles,
                           std::wstring_view id);
