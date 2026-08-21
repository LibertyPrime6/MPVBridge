#pragma once

#include <optional>
#include <string>
#include <string_view>

struct ParsedCommandLine {
    std::wstring passThroughTail;
    std::optional<std::wstring> requestedProfile;
    std::wstring error;
};

ParsedCommandLine ParseCommandLine(std::wstring_view commandLine);

