#include "MpvIpc.h"

#include "PlaybackFeedback.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

namespace {

constexpr DWORD kPipeConnectTimeoutMilliseconds = 30000;
constexpr auto kPlaybackSnapshotInterval = std::chrono::seconds(10);

bool SendAll(HANDLE pipe, std::string_view command);

bool ProcessIsRunning(HANDLE process) {
    return WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
}

std::optional<std::string> ParseJsonString(std::string_view json,
                                           size_t position) {
    while (position < json.size() &&
           (json[position] == ' ' || json[position] == '\t')) {
        ++position;
    }
    if (position >= json.size() || json[position] != '"') return std::nullopt;
    ++position;
    std::string result;
    while (position < json.size()) {
        const char ch = json[position++];
        if (ch == '"') return result;
        if (ch != '\\') {
            result.push_back(ch);
            continue;
        }
        if (position >= json.size()) return std::nullopt;
        const char escaped = json[position++];
        switch (escaped) {
        case '"': result.push_back('"'); break;
        case '\\': result.push_back('\\'); break;
        case '/': result.push_back('/'); break;
        case 'b': result.push_back('\b'); break;
        case 'f': result.push_back('\f'); break;
        case 'n': result.push_back('\n'); break;
        case 'r': result.push_back('\r'); break;
        case 't': result.push_back('\t'); break;
        case 'u':
            // Property names are ASCII and MPV normally emits UTF-8 values.
            // Keep uncommon escaped Unicode lossless enough for diagnostics.
            result.append("\\u");
            if (position + 4 > json.size()) return std::nullopt;
            result.append(json.substr(position, 4));
            position += 4;
            break;
        default: return std::nullopt;
        }
    }
    return std::nullopt;
}

std::optional<size_t> FindValue(std::string_view json, std::string_view key) {
    const std::string needle = "\"" + std::string(key) + "\"";
    const size_t keyPosition = json.find(needle);
    if (keyPosition == std::string_view::npos) return std::nullopt;
    const size_t colon = json.find(':', keyPosition + needle.size());
    if (colon == std::string_view::npos) return std::nullopt;
    size_t value = colon + 1;
    while (value < json.size() &&
           (json[value] == ' ' || json[value] == '\t')) {
        ++value;
    }
    return value;
}

std::optional<std::string> StringValue(std::string_view json,
                                       std::string_view key) {
    const auto position = FindValue(json, key);
    return position.has_value() ? ParseJsonString(json, *position) : std::nullopt;
}

std::optional<double> NumberValue(std::string_view json, std::string_view key) {
    const auto position = FindValue(json, key);
    if (!position.has_value() || *position >= json.size() ||
        json.substr(*position, 4) == "null") {
        return std::nullopt;
    }
    std::string remaining(json.substr(*position));
    char* end = nullptr;
    const double value = std::strtod(remaining.c_str(), &end);
    if (end == remaining.c_str()) return std::nullopt;
    return value;
}

std::optional<bool> BooleanValue(std::string_view json, std::string_view key) {
    const auto position = FindValue(json, key);
    if (!position.has_value()) return std::nullopt;
    if (json.substr(*position, 4) == "true") return true;
    if (json.substr(*position, 5) == "false") return false;
    return std::nullopt;
}

bool ProcessEvent(std::string_view line, PlaybackFeedbackServer& feedback) {
    const auto requestId = NumberValue(line, "request_id");
    if (!requestId.has_value()) return false;
    const int id = static_cast<int>(*requestId);
    const std::string_view property = id == 101 ? "time-pos" :
        id == 102 ? "duration" : id == 103 ? "pause" :
        id == 104 ? "path" : id == 105 ? "playlist-pos" :
        id == 106 ? "media-title" : "";
    if (property.empty()) return false;

    bool playbackPathReady = false;

    if (property == "time-pos" || property == "duration" ||
        property == "playlist-pos") {
        const auto value = NumberValue(line, "data");
        if (value.has_value()) feedback.SetPlaybackNumber(property, *value);
    } else if (property == "pause") {
        const auto value = BooleanValue(line, "data");
        if (value.has_value()) feedback.SetPlaybackBoolean(property, *value);
    } else if (property == "media-title" || property == "path") {
        const auto valuePosition = FindValue(line, "data");
        if (valuePosition.has_value()) {
            const auto value = ParseJsonString(line, *valuePosition);
            if (value.has_value()) {
                feedback.SetPlaybackString(property, *value);
                playbackPathReady = property == "path" && !value->empty();
            }
        }
    }
    feedback.SetPhase("playing");
    return playbackPathReady;
}

bool RequestPlaybackSnapshot(HANDLE pipe) {
    constexpr std::string_view commands[] = {
        "{\"command\":[\"get_property\",\"time-pos\"],\"request_id\":101}\n",
        "{\"command\":[\"get_property\",\"duration\"],\"request_id\":102}\n",
        "{\"command\":[\"get_property\",\"pause\"],\"request_id\":103}\n",
        "{\"command\":[\"get_property\",\"path\"],\"request_id\":104}\n",
        "{\"command\":[\"get_property\",\"playlist-pos\"],\"request_id\":105}\n",
        "{\"command\":[\"get_property\",\"media-title\"],\"request_id\":106}\n"
    };
    for (const std::string_view command : commands) {
        if (!SendAll(pipe, command)) return false;
    }
    return true;
}

bool SendAll(HANDLE pipe, std::string_view command) {
    size_t offset = 0;
    while (offset < command.size()) {
        DWORD written = 0;
        if (WriteFile(pipe, command.data() + offset,
                      static_cast<DWORD>(command.size() - offset), &written,
                      nullptr) == FALSE || written == 0) {
            return false;
        }
        offset += written;
    }
    return true;
}

} // namespace

void MonitorMpvJsonIpc(std::wstring_view pipeName, HANDLE process,
                       PlaybackFeedbackServer& feedback,
                       bool requestBilibiliDanmaku) {
    feedback.SetIpcConnecting();
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(kPipeConnectTimeoutMilliseconds);
    HANDLE pipe = INVALID_HANDLE_VALUE;
    while (ProcessIsRunning(process) && std::chrono::steady_clock::now() < deadline) {
        pipe = CreateFileW(std::wstring(pipeName).c_str(),
                           GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                           OPEN_EXISTING, 0, nullptr);
        if (pipe != INVALID_HANDLE_VALUE) break;
        const DWORD error = GetLastError();
        if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PIPE_BUSY) {
            feedback.SetIpcFailed("Unable to open MPV JSON IPC pipe (Windows error " +
                                  std::to_string(error) + ")");
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (pipe == INVALID_HANDLE_VALUE) {
        feedback.SetIpcFailed(ProcessIsRunning(process)
                                  ? "Timed out waiting for MPV JSON IPC pipe"
                                  : "MPV exited before its JSON IPC pipe was ready");
        return;
    }

    feedback.SetIpcConnected();

    std::string pending;
    char buffer[8192];
    auto nextSnapshot = std::chrono::steady_clock::now();
    bool bilibiliDanmakuRequested = false;
    while (ProcessIsRunning(process)) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= nextSnapshot) {
            if (!RequestPlaybackSnapshot(pipe)) {
                feedback.SetIpcFailed("Unable to request MPV JSON IPC playback snapshot");
                break;
            }
            nextSnapshot = now +
                (requestBilibiliDanmaku && !bilibiliDanmakuRequested
                     ? std::chrono::milliseconds(250)
                     : std::chrono::duration_cast<std::chrono::milliseconds>(
                           kPlaybackSnapshotInterval));
        }
        DWORD available = 0;
        if (PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr) == FALSE) {
            break;
        }
        if (available == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }
        DWORD read = 0;
        const DWORD requested = std::min<DWORD>(available, sizeof(buffer));
        if (ReadFile(pipe, buffer, requested, &read, nullptr) == FALSE || read == 0) {
            break;
        }
        pending.append(buffer, read);
        size_t newline = 0;
        while ((newline = pending.find('\n')) != std::string::npos) {
            std::string line = pending.substr(0, newline);
            pending.erase(0, newline + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            const bool playbackPathReady = ProcessEvent(line, feedback);
            if (requestBilibiliDanmaku && !bilibiliDanmakuRequested &&
                playbackPathReady) {
                constexpr std::string_view command =
                    "{\"command\":[\"script-message-to\",\"uosc_danmaku\","
                    "\"set\",\"show_danmaku\",\"on\"],\"request_id\":201}\n";
                // script-message-to is intentionally best-effort. MPV accepts
                // the command even when the target script is absent, so a
                // profile without uosc_danmaku continues normal playback.
                bilibiliDanmakuRequested = SendAll(pipe, command);
            }
        }
        if (pending.size() > 1024 * 1024) pending.clear();
    }
    CloseHandle(pipe);
}
