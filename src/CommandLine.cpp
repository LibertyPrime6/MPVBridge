#include "CommandLine.h"

#include "AppCore.h"

#include <cwctype>
#include <iterator>
#include <utility>
#include <vector>

namespace {

constexpr wchar_t kBridgePrefix[] = L"--bridge-profile=";

struct RawArgument {
    size_t begin{};
    size_t end{};
    std::wstring decoded;
};

size_t SkipExecutableToken(std::wstring_view commandLine) {
    size_t index = 0;
    while (index < commandLine.size() && std::iswspace(commandLine[index]) != 0) {
        ++index;
    }
    if (index < commandLine.size() && commandLine[index] == L'"') {
        ++index;
        while (index < commandLine.size() && commandLine[index] != L'"') {
            ++index;
        }
        if (index < commandLine.size()) {
            ++index;
        }
    } else {
        while (index < commandLine.size() && std::iswspace(commandLine[index]) == 0) {
            ++index;
        }
    }
    return index;
}

RawArgument ScanArgument(std::wstring_view text, size_t begin) {
    RawArgument argument;
    argument.begin = begin;
    size_t index = begin;
    bool inQuotes = false;

    while (index < text.size()) {
        if (!inQuotes && std::iswspace(text[index]) != 0) {
            break;
        }
        size_t backslashes = 0;
        while (index < text.size() && text[index] == L'\\') {
            ++backslashes;
            ++index;
        }
        if (index < text.size() && text[index] == L'"') {
            argument.decoded.append(backslashes / 2, L'\\');
            if ((backslashes % 2) != 0) {
                argument.decoded.push_back(L'"');
                ++index;
            } else if (inQuotes && index + 1 < text.size() &&
                       text[index + 1] == L'"') {
                argument.decoded.push_back(L'"');
                index += 2;
            } else {
                inQuotes = !inQuotes;
                ++index;
            }
            continue;
        }
        argument.decoded.append(backslashes, L'\\');
        if (index < text.size() && (inQuotes || std::iswspace(text[index]) == 0)) {
            argument.decoded.push_back(text[index++]);
        }
    }
    argument.end = index;
    return argument;
}

bool StartsWithInsensitive(std::wstring_view value, std::wstring_view prefix) {
    return value.size() >= prefix.size() &&
           EqualsInsensitive(value.substr(0, prefix.size()), prefix);
}

} // namespace

ParsedCommandLine ParseCommandLine(std::wstring_view commandLine) {
    ParsedCommandLine parsed;
    const size_t executableEnd = SkipExecutableToken(commandLine);
    const std::wstring_view tail = commandLine.substr(executableEnd);
    std::vector<RawArgument> bridgeArguments;

    size_t index = 0;
    while (index < tail.size()) {
        while (index < tail.size() && std::iswspace(tail[index]) != 0) {
            ++index;
        }
        if (index == tail.size()) {
            break;
        }
        RawArgument argument = ScanArgument(tail, index);
        if (StartsWithInsensitive(argument.decoded, kBridgePrefix)) {
            std::wstring id = argument.decoded.substr(std::size(kBridgePrefix) - 1);
            if (id.empty()) {
                parsed.error = L"--bridge-profile 参数缺少 ProfileID。";
                return parsed;
            }
            if (parsed.requestedProfile.has_value()) {
                parsed.error = L"--bridge-profile 只能指定一次。";
                return parsed;
            }
            parsed.requestedProfile = std::move(id);
            bridgeArguments.push_back(std::move(argument));
        }
        index = argument.end;
    }

    // Preserve all original characters other than the Bridge-only token.
    size_t cursor = 0;
    for (const RawArgument& argument : bridgeArguments) {
        parsed.passThroughTail.append(tail.substr(cursor, argument.begin - cursor));
        cursor = argument.end;
    }
    parsed.passThroughTail.append(tail.substr(cursor));
    return parsed;
}

