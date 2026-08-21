#include "YtdlpPreflight.h"

#include "AppCore.h"

#include <shellapi.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

constexpr DWORD kPreflightTimeoutMilliseconds = 120000;
constexpr size_t kMaximumCapturedOutput = 1024 * 1024;

bool StartsWithInsensitive(std::wstring_view value, std::wstring_view prefix) {
    return value.size() >= prefix.size() &&
           EqualsInsensitive(value.substr(0, prefix.size()), prefix);
}

bool ContainsInsensitiveAscii(std::string_view value, std::string_view needle) {
    if (needle.empty()) return true;
    return std::search(value.begin(), value.end(), needle.begin(), needle.end(),
                       [](char left, char right) {
                           return std::tolower(static_cast<unsigned char>(left)) ==
                                  std::tolower(static_cast<unsigned char>(right));
                       }) != value.end();
}

std::wstring QuoteCommandArgument(std::wstring_view value) {
    std::wstring result = L"\"";
    size_t backslashes = 0;
    for (const wchar_t ch : value) {
        if (ch == L'\\') {
            ++backslashes;
            continue;
        }
        if (ch == L'\"') {
            result.append(backslashes * 2 + 1, L'\\');
            result.push_back(L'\"');
            backslashes = 0;
            continue;
        }
        result.append(backslashes, L'\\');
        backslashes = 0;
        result.push_back(ch);
    }
    result.append(backslashes * 2, L'\\');
    result.push_back(L'\"');
    return result;
}

std::string WideToUtf8(std::wstring_view value) {
    if (value.empty()) return {};
    const int required = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
        nullptr, 0, nullptr, nullptr);
    if (required <= 0) return "Windows error";
    std::string result(static_cast<size_t>(required), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                        static_cast<int>(value.size()), result.data(), required,
                        nullptr, nullptr);
    return result;
}

std::vector<std::wstring> ParseArguments(std::wstring_view tail) {
    const std::wstring parseLine = L"mpvbridge-preflight.exe" + std::wstring(tail);
    int count = 0;
    wchar_t** values = CommandLineToArgvW(parseLine.c_str(), &count);
    if (values == nullptr) return {};
    std::vector<std::wstring> result;
    for (int index = 1; index < count; ++index) result.emplace_back(values[index]);
    LocalFree(values);
    return result;
}

std::wstring OptionValue(const std::vector<std::wstring>& arguments,
                         std::wstring_view prefix) {
    for (auto iterator = arguments.rbegin(); iterator != arguments.rend(); ++iterator) {
        if (StartsWithInsensitive(*iterator, prefix)) {
            return iterator->substr(prefix.size());
        }
    }
    return {};
}

std::vector<std::wstring> ParseM3uEntries(std::wstring_view value) {
    constexpr std::wstring_view memoryPrefix = L"memory://";
    if (StartsWithInsensitive(value, memoryPrefix)) value.remove_prefix(memoryPrefix.size());
    std::vector<std::wstring> entries;
    size_t cursor = 0;
    while (cursor <= value.size()) {
        const size_t end = value.find(L'\n', cursor);
        std::wstring line(value.substr(cursor, end == std::wstring_view::npos ?
                                                  value.size() - cursor : end - cursor));
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        line = Trim(std::move(line));
        if (!line.empty() && line.front() != L'#') entries.push_back(std::move(line));
        if (end == std::wstring_view::npos) break;
        cursor = end + 1;
    }
    return entries;
}

std::wstring FindYtdlpTarget(const std::vector<std::wstring>& arguments) {
    size_t selectedIndex = 0;
    const std::wstring selected = OptionValue(arguments, L"--playlist-start=");
    if (!selected.empty()) {
        try {
            selectedIndex = static_cast<size_t>(std::stoull(selected));
        } catch (...) {
            selectedIndex = 0;
        }
    }
    const std::wstring playlist = OptionValue(arguments, L"--playlist=");
    if (!playlist.empty()) {
        const std::vector<std::wstring> entries = ParseM3uEntries(playlist);
        if (!entries.empty()) {
            size_t index = selectedIndex;
            if (index >= entries.size()) index = entries.size() - 1;
            if (StartsWithInsensitive(entries[index], L"ytdl://")) {
                return entries[index].substr(7);
            }
            return {};
        }
    }
    std::vector<std::wstring> targets;
    for (const std::wstring& argument : arguments) {
        if (StartsWithInsensitive(argument, L"ytdl://")) {
            targets.push_back(argument.substr(7));
        }
    }
    if (targets.empty()) return {};
    if (selectedIndex >= targets.size()) selectedIndex = targets.size() - 1;
    return targets[selectedIndex];
}

std::wstring UnwrapRawValue(std::wstring value) {
    if (value.size() >= 2 && value.front() == L'[' && value.back() == L']') {
        value = value.substr(1, value.size() - 2);
    }
    std::wstring result;
    result.reserve(value.size());
    for (size_t index = 0; index < value.size(); ++index) {
        const wchar_t ch = value[index];
        // MPV's bracket syntax only needs a closing bracket escaped here.
        // Preserve ordinary Windows path separators (for example C:\\Users).
        if (ch == L'\\' && index + 1 < value.size() &&
            value[index + 1] == L']') {
            result.push_back(L']');
            ++index;
        } else {
            result.push_back(ch);
        }
    }
    return result;
}

std::vector<std::pair<std::wstring, std::wstring>> ParseRawOptions(
    std::wstring_view raw) {
    std::vector<std::pair<std::wstring, std::wstring>> result;
    size_t start = 0;
    int bracketDepth = 0;
    bool escaped = false;
    for (size_t index = 0; index <= raw.size(); ++index) {
        const wchar_t ch = index < raw.size() ? raw[index] : L',';
        if (escaped) {
            escaped = false;
            continue;
        }
        if (ch == L'\\') {
            escaped = true;
            continue;
        }
        if (ch == L'[') ++bracketDepth;
        else if (ch == L']' && bracketDepth > 0) --bracketDepth;
        else if (ch == L',' && bracketDepth == 0) {
            const std::wstring item(raw.substr(start, index - start));
            const size_t separator = item.find(L'=');
            if (separator != std::wstring::npos && separator > 0) {
                result.emplace_back(item.substr(0, separator),
                                    UnwrapRawValue(item.substr(separator + 1)));
            }
            start = index + 1;
        }
    }
    return result;
}

std::optional<std::wstring> MapRawOption(std::wstring_view key) {
    static constexpr std::pair<std::wstring_view, std::wstring_view> mappings[] = {
        {L"cookies", L"--cookies"},
        {L"proxy", L"--proxy"},
        {L"format-sort", L"--format-sort"},
        {L"extractor-args", L"--extractor-args"},
        {L"user-agent", L"--user-agent"},
        {L"referer", L"--referer"},
        {L"add-headers", L"--add-headers"},
        {L"impersonate", L"--impersonate"},
        {L"js-runtimes", L"--js-runtimes"},
        {L"remote-components", L"--remote-components"},
        {L"source-address", L"--source-address"}
    };
    for (const auto& [candidate, option] : mappings) {
        if (EqualsInsensitive(key, candidate)) return std::wstring(option);
    }
    return std::nullopt;
}

std::filesystem::path FindYtdlp(const Profile& profile) {
    const std::filesystem::path moduleDirectory = GetModulePath().parent_path();
    const std::filesystem::path playerDirectory = profile.executable.parent_path();
    const std::filesystem::path candidates[] = {
        moduleDirectory / L"yt-dlp.exe",
        moduleDirectory.parent_path() / L"Tools" / L"yt-dlp" / L"yt-dlp.exe",
        playerDirectory / L"yt-dlp.exe",
        playerDirectory.parent_path() / L"Tools" / L"yt-dlp" / L"yt-dlp.exe"
    };
    std::error_code error;
    for (const auto& candidate : candidates) {
        if (std::filesystem::is_regular_file(candidate, error)) return candidate;
        error.clear();
    }
    std::vector<wchar_t> buffer(32768, L'\0');
    const DWORD length = SearchPathW(nullptr, L"yt-dlp.exe", nullptr,
                                     static_cast<DWORD>(buffer.size()),
                                     buffer.data(), nullptr);
    if (length > 0 && length < buffer.size()) return std::filesystem::path(buffer.data());
    return {};
}

struct ProcessResult {
    DWORD exitCode = 1;
    bool timedOut = false;
    std::string output;
    std::string error;
};

class ScopedEnvironmentVariable {
public:
    ScopedEnvironmentVariable(std::wstring name, std::wstring_view value)
        : name_(std::move(name)) {
        SetLastError(ERROR_SUCCESS);
        const DWORD required = GetEnvironmentVariableW(name_.c_str(), nullptr, 0);
        if (required > 0) {
            std::vector<wchar_t> buffer(required, L'\0');
            const DWORD copied = GetEnvironmentVariableW(
                name_.c_str(), buffer.data(), static_cast<DWORD>(buffer.size()));
            if (copied < buffer.size()) {
                existed_ = true;
                previous_.assign(buffer.data(), copied);
            }
        }
        SetEnvironmentVariableW(name_.c_str(), std::wstring(value).c_str());
    }

    ~ScopedEnvironmentVariable() {
        SetEnvironmentVariableW(name_.c_str(),
                                existed_ ? previous_.c_str() : nullptr);
    }

    ScopedEnvironmentVariable(const ScopedEnvironmentVariable&) = delete;
    ScopedEnvironmentVariable& operator=(const ScopedEnvironmentVariable&) = delete;

private:
    std::wstring name_;
    std::wstring previous_;
    bool existed_ = false;
};

ProcessResult RunProcess(const std::filesystem::path& executable,
                         const std::vector<std::wstring>& arguments) {
    ProcessResult result;
    SECURITY_ATTRIBUTES security{};
    security.nLength = sizeof(security);
    security.bInheritHandle = TRUE;
    HANDLE readPipe = nullptr;
    HANDLE writePipe = nullptr;
    if (CreatePipe(&readPipe, &writePipe, &security, 0) == FALSE ||
        SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0) == FALSE) {
        if (readPipe) CloseHandle(readPipe);
        if (writePipe) CloseHandle(writePipe);
        result.error = "Unable to create yt-dlp output pipe";
        return result;
    }

    std::wstring commandLine = QuoteCommandArgument(executable.wstring());
    for (const std::wstring& argument : arguments) {
        commandLine.push_back(L' ');
        commandLine.append(QuoteCommandArgument(argument));
    }
    std::vector<wchar_t> writable(commandLine.begin(), commandLine.end());
    writable.push_back(L'\0');
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = writePipe;
    startup.hStdError = writePipe;
    PROCESS_INFORMATION process{};
    BOOL created = FALSE;
    {
        ScopedEnvironmentVariable pythonIoEncoding(L"PYTHONIOENCODING", L"utf-8");
        ScopedEnvironmentVariable pythonUtf8(L"PYTHONUTF8", L"1");
        created = CreateProcessW(executable.c_str(), writable.data(), nullptr, nullptr,
                                 TRUE, CREATE_NO_WINDOW, nullptr,
                                 executable.parent_path().c_str(), &startup, &process);
    }
    if (created == FALSE) {
        result.error = WideToUtf8(FormatSystemError(GetLastError()));
        CloseHandle(readPipe);
        CloseHandle(writePipe);
        return result;
    }
    CloseHandle(writePipe);
    CloseHandle(process.hThread);

    std::thread reader([&] {
        char buffer[8192];
        for (;;) {
            DWORD read = 0;
            if (ReadFile(readPipe, buffer, sizeof(buffer), &read, nullptr) == FALSE || read == 0) break;
            if (result.output.size() < kMaximumCapturedOutput) {
                const size_t accepted = std::min<size_t>(
                    read, kMaximumCapturedOutput - result.output.size());
                result.output.append(buffer, accepted);
            }
        }
    });
    const DWORD wait = WaitForSingleObject(process.hProcess, kPreflightTimeoutMilliseconds);
    if (wait == WAIT_TIMEOUT) {
        result.timedOut = true;
        TerminateProcess(process.hProcess, ERROR_TIMEOUT);
        WaitForSingleObject(process.hProcess, 5000);
    }
    GetExitCodeProcess(process.hProcess, &result.exitCode);
    CloseHandle(process.hProcess);
    reader.join();
    CloseHandle(readPipe);
    return result;
}

std::string FieldFromOutput(std::string_view output, std::string_view prefix) {
    size_t cursor = 0;
    std::string result;
    while (cursor <= output.size()) {
        const size_t end = output.find('\n', cursor);
        std::string_view line = output.substr(cursor, end == std::string_view::npos ?
                                                         output.size() - cursor : end - cursor);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (line.starts_with(prefix)) result = std::string(line.substr(prefix.size()));
        if (end == std::string_view::npos) break;
        cursor = end + 1;
    }
    return result;
}

std::string CompactError(std::string output) {
    output.erase(std::remove(output.begin(), output.end(), '\r'), output.end());
    while (!output.empty() && (output.back() == '\n' || output.back() == ' ')) output.pop_back();
    if (output.size() > 1600) output = output.substr(output.size() - 1600);
    return output;
}

} // namespace

YtdlpPreflightResult RunYtdlpPreflight(const Profile& profile,
                                       std::wstring_view passThroughTail) {
    YtdlpPreflightResult result;
    const std::vector<std::wstring> mpvArguments = ParseArguments(passThroughTail);
    const std::wstring target = FindYtdlpTarget(mpvArguments);
    if (target.empty()) {
        result.error = "Current media is a direct stream; yt-dlp preflight was skipped";
        return result;
    }
    result.attempted = true;
    const std::filesystem::path ytdlp = FindYtdlp(profile);
    if (ytdlp.empty()) {
        result.error = "yt-dlp.exe was not found";
        return result;
    }

    std::vector<std::wstring> arguments = {
        L"--no-config", L"--simulate", L"--no-playlist",
        L"--no-colors", L"--verbose", L"--encoding", L"utf-8",
        L"--print", L"__MPVBRIDGE_ID__=%(id)s",
        L"--print", L"__MPVBRIDGE_TITLE__=%(title)s",
        L"--print", L"__MPVBRIDGE_FORMAT__=%(format_id)s",
        L"--print", L"__MPVBRIDGE_RESOLUTION__=%(resolution)s"
    };
    const std::wstring format = OptionValue(mpvArguments, L"--ytdl-format=");
    if (!format.empty()) {
        arguments.emplace_back(L"--format");
        arguments.push_back(format);
    }
    const std::wstring rawOptions = OptionValue(mpvArguments, L"--ytdl-raw-options=");
    for (const auto& [key, value] : ParseRawOptions(rawOptions)) {
        const std::optional<std::wstring> mapped = MapRawOption(key);
        if (!mapped.has_value() || value.empty()) continue;
        arguments.push_back(*mapped);
        arguments.push_back(value);
    }
    arguments.push_back(target);

    const ProcessResult process = RunProcess(ytdlp, arguments);
    if (process.timedOut) {
        result.error = "yt-dlp preflight timed out after 120 seconds";
        return result;
    }
    if (!process.error.empty()) {
        result.error = process.error;
        return result;
    }
    if (process.exitCode != 0) {
        result.error = CompactError(process.output);
        if (result.error.empty()) result.error = "yt-dlp preflight failed";
        return result;
    }
    const bool youtubeTarget =
        target.find(L"youtube.com/") != std::wstring::npos ||
        target.find(L"youtu.be/") != std::wstring::npos;
    const bool suppliedCookies = std::find(arguments.begin(), arguments.end(),
                                           L"--cookies") != arguments.end();
    if (youtubeTarget && suppliedCookies) {
        if (ContainsInsensitiveAscii(
                process.output,
                "provided YouTube account cookies are no longer valid")) {
            result.error =
                "YouTube rejected the supplied account Cookie because the browser "
                "rotated the login session";
            return result;
        }
        if (!ContainsInsensitiveAscii(process.output,
                                      "Found YouTube account cookies")) {
            result.error =
                "yt-dlp parsed the public video but did not confirm a logged-in "
                "YouTube account Cookie";
            return result;
        }
    }
    result.id = FieldFromOutput(process.output, "__MPVBRIDGE_ID__=");
    result.title = FieldFromOutput(process.output, "__MPVBRIDGE_TITLE__=");
    result.formatId = FieldFromOutput(process.output, "__MPVBRIDGE_FORMAT__=");
    result.resolution = FieldFromOutput(process.output, "__MPVBRIDGE_RESOLUTION__=");
    result.success = true;
    return result;
}
