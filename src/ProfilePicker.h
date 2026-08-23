#pragma once

#include "Profiles.h"

#include <optional>
#include <string>

struct ProfilePickerResult {
    std::optional<std::wstring> profileId;
};

ProfilePickerResult RunProfilePicker(HINSTANCE instance, ProfileStore& store,
                                     HWND owner = nullptr);

