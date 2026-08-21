#pragma once

#include "Profiles.h"

#include <string>
#include <string_view>

struct YtdlpPreflightResult {
    bool attempted = false;
    bool success = false;
    std::string id;
    std::string title;
    std::string formatId;
    std::string resolution;
    std::string error;
};

YtdlpPreflightResult RunYtdlpPreflight(const Profile& profile,
                                       std::wstring_view passThroughTail);
