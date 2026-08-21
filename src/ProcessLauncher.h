#pragma once

#include "Profiles.h"

#include <string_view>

DWORD LaunchAndWait(const ProfileStore& store, const Profile& profile,
                    std::wstring_view passThroughTail);
