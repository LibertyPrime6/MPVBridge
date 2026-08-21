#include <winsock2.h>
#include <ws2tcpip.h>

#include "PlaybackFeedback.h"

#include "AppCore.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cctype>
#include <iomanip>
#include <limits>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr size_t kMaximumCookieJarBytes = 2 * 1024 * 1024;
constexpr size_t kMaximumHttpHeaderBytes = 16 * 1024;

std::string JsonEscape(std::string_view value) {
    std::string result;
    result.reserve(value.size() + 16);
    constexpr char hex[] = "0123456789abcdef";
    for (const unsigned char ch : value) {
        switch (ch) {
        case '"': result.append("\\\""); break;
        case '\\': result.append("\\\\"); break;
        case '\b': result.append("\\b"); break;
        case '\f': result.append("\\f"); break;
        case '\n': result.append("\\n"); break;
        case '\r': result.append("\\r"); break;
        case '\t': result.append("\\t"); break;
        default:
            if (ch < 0x20) {
                result.append("\\u00");
                result.push_back(hex[ch >> 4]);
                result.push_back(hex[ch & 0x0f]);
            } else {
                result.push_back(static_cast<char>(ch));
            }
        }
    }
    return result;
}

long long UnixMilliseconds() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string OptionalNumber(const std::optional<double>& value) {
    if (!value.has_value() || !std::isfinite(*value)) return "null";
    std::ostringstream stream;
    stream << std::setprecision(15) << *value;
    return stream.str();
}

std::string OptionalBoolean(const std::optional<bool>& value) {
    if (!value.has_value()) return "null";
    return *value ? "true" : "false";
}

std::string HttpResponse(int status, std::string_view statusText,
                         std::string_view body, std::string_view contentType) {
    std::ostringstream response;
    response << "HTTP/1.1 " << status << ' ' << statusText << "\r\n"
             << "Content-Type: " << contentType << "; charset=utf-8\r\n"
             << "Content-Length: " << body.size() << "\r\n"
             << "Access-Control-Allow-Origin: *\r\n"
             << "Access-Control-Allow-Methods: GET, OPTIONS\r\n"
             << "Access-Control-Allow-Headers: Accept, Content-Type\r\n"
             << "Cache-Control: no-store\r\n"
             << "Connection: close\r\n\r\n"
             << body;
    return response.str();
}

std::optional<size_t> ContentLength(std::string_view headers) {
    std::string lower(headers);
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    constexpr std::string_view name = "\r\ncontent-length:";
    const size_t position = lower.find(name);
    if (position == std::string::npos) return size_t{0};
    size_t cursor = position + name.size();
    while (cursor < lower.size() && (lower[cursor] == ' ' || lower[cursor] == '\t')) {
        ++cursor;
    }
    size_t end = cursor;
    while (end < lower.size() && lower[end] >= '0' && lower[end] <= '9') ++end;
    if (end == cursor) return std::nullopt;
    try {
        const unsigned long long value = std::stoull(lower.substr(cursor, end - cursor));
        if (value > (std::numeric_limits<size_t>::max)()) return std::nullopt;
        return static_cast<size_t>(value);
    } catch (...) {
        return std::nullopt;
    }
}

bool ReceiveHttpRequest(SOCKET client, std::string& request) {
    DWORD timeout = 3000;
    setsockopt(client, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    std::optional<size_t> expectedSize;
    char buffer[8192];
    for (;;) {
        const int received = recv(client, buffer, sizeof(buffer), 0);
        if (received <= 0) return expectedSize.has_value() && request.size() >= *expectedSize;
        request.append(buffer, static_cast<size_t>(received));
        const size_t headerEnd = request.find("\r\n\r\n");
        if (headerEnd == std::string::npos) {
            if (request.size() > kMaximumHttpHeaderBytes) return false;
            continue;
        }
        if (!expectedSize.has_value()) {
            const auto contentLength = ContentLength(
                std::string_view(request).substr(0, headerEnd + 2));
            if (!contentLength.has_value() || *contentLength > kMaximumCookieJarBytes) {
                return false;
            }
            expectedSize = headerEnd + 4 + *contentLength;
        }
        if (request.size() >= *expectedSize) {
            request.resize(*expectedSize);
            return true;
        }
    }
}

} // namespace

struct PlaybackFeedbackServer::Impl {
    mutable std::mutex mutex;
    std::string token;
    std::string phase = "starting";
    std::string error;
    std::string preflightStatus = "not-run";
    std::string preflightError;
    std::string preflightId;
    std::string preflightTitle;
    std::string preflightFormatId;
    std::string preflightResolution;
    std::string ipcStatus = "not-run";
    std::string ipcError;
    std::optional<double> timePos;
    std::optional<double> duration;
    std::optional<double> playlistPos;
    std::optional<bool> pause;
    std::string mediaTitle;
    std::string path;
    long long playbackUpdatedAt = 0;
    std::optional<unsigned long> exitCode;
    long long updatedAt = UnixMilliseconds();
    std::condition_variable cookieCondition;
    bool cookieReceived = false;
    std::string cookieJar;

    std::atomic<bool> stopping = false;
    SOCKET listeningSocket = INVALID_SOCKET;
    std::thread serverThread;
    bool winsockStarted = false;

    void Touch() { updatedAt = UnixMilliseconds(); }

    std::string SnapshotJson() const {
        std::lock_guard lock(mutex);
        std::ostringstream json;
        json << "{\"phase\":\"" << JsonEscape(phase) << "\","
             << "\"error\":\"" << JsonEscape(error) << "\","
             << "\"preflight\":{"
             << "\"status\":\"" << JsonEscape(preflightStatus) << "\","
             << "\"error\":\"" << JsonEscape(preflightError) << "\","
             << "\"id\":\"" << JsonEscape(preflightId) << "\","
             << "\"title\":\"" << JsonEscape(preflightTitle) << "\","
             << "\"formatId\":\"" << JsonEscape(preflightFormatId) << "\","
             << "\"resolution\":\"" << JsonEscape(preflightResolution) << "\"},"
             << "\"ipc\":{"
             << "\"status\":\"" << JsonEscape(ipcStatus) << "\","
             << "\"error\":\"" << JsonEscape(ipcError) << "\"},"
             << "\"playback\":{"
             << "\"timePos\":" << OptionalNumber(timePos) << ','
             << "\"duration\":" << OptionalNumber(duration) << ','
             << "\"playlistPos\":" << OptionalNumber(playlistPos) << ','
             << "\"pause\":" << OptionalBoolean(pause) << ','
             << "\"mediaTitle\":\"" << JsonEscape(mediaTitle) << "\","
             << "\"path\":\"" << JsonEscape(path) << "\","
             << "\"updatedAt\":" << playbackUpdatedAt << "},"
             << "\"exitCode\":";
        if (exitCode.has_value()) json << *exitCode;
        else json << "null";
        json << ",\"updatedAt\":" << updatedAt << '}';
        return json.str();
    }

    void Serve() {
        while (!stopping.load()) {
            fd_set readSet;
            FD_ZERO(&readSet);
            FD_SET(listeningSocket, &readSet);
            timeval timeout{};
            timeout.tv_sec = 0;
            timeout.tv_usec = 200000;
            const int selected = select(0, &readSet, nullptr, nullptr, &timeout);
            if (selected <= 0 || stopping.load()) continue;
            SOCKET client = accept(listeningSocket, nullptr, nullptr);
            if (client == INVALID_SOCKET) continue;

            std::string request;
            const bool requestValid = ReceiveHttpRequest(client, request);
            const size_t lineEnd = request.find("\r\n");
            const std::string_view requestLine(
                request.data(), lineEnd == std::string::npos ? request.size() : lineEnd);
            const std::string expectedPath = "/v1/session/" + token;
            std::string response;
            if (!requestValid) {
                response = HttpResponse(400, "Bad Request", "invalid request", "text/plain");
            } else if (requestLine.starts_with("OPTIONS ")) {
                response = HttpResponse(204, "No Content", "", "text/plain");
            } else if (requestLine == "GET " + expectedPath + " HTTP/1.1" ||
                       requestLine == "GET " + expectedPath + " HTTP/1.0") {
                const std::string body = SnapshotJson();
                response = HttpResponse(200, "OK", body, "application/json");
            } else if (requestLine == "POST " + expectedPath + "/cookie HTTP/1.1" ||
                       requestLine == "POST " + expectedPath + "/cookie HTTP/1.0") {
                const size_t headerEnd = request.find("\r\n\r\n");
                const std::string body = request.substr(headerEnd + 4);
                {
                    std::lock_guard lock(mutex);
                    cookieJar = body;
                    cookieReceived = true;
                    Touch();
                }
                cookieCondition.notify_all();
                response = HttpResponse(204, "No Content", "", "text/plain");
            } else {
                response = HttpResponse(404, "Not Found", "not found", "text/plain");
            }
            size_t sent = 0;
            while (sent < response.size()) {
                const int count = send(client, response.data() + sent,
                                       static_cast<int>(response.size() - sent), 0);
                if (count <= 0) break;
                sent += static_cast<size_t>(count);
            }
            shutdown(client, SD_BOTH);
            closesocket(client);
        }
    }
};

PlaybackFeedbackServer::PlaybackFeedbackServer() : impl_(std::make_unique<Impl>()) {}

PlaybackFeedbackServer::~PlaybackFeedbackServer() { Stop(); }

bool PlaybackFeedbackServer::Start(uint16_t port, std::string token,
                                   std::wstring& error) {
    if (port < 1024 || token.size() < 16 || token.size() > 128) {
        error = L"MPVBridge 回传端口或会话令牌无效。";
        return false;
    }
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
        error = L"无法初始化本机回传网络。";
        return false;
    }
    impl_->winsockStarted = true;
    impl_->listeningSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (impl_->listeningSocket == INVALID_SOCKET) {
        error = L"无法创建本机回传套接字。";
        Stop();
        return false;
    }
    BOOL exclusive = TRUE;
    setsockopt(impl_->listeningSocket, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
               reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);
    if (bind(impl_->listeningSocket, reinterpret_cast<sockaddr*>(&address),
             sizeof(address)) == SOCKET_ERROR ||
        listen(impl_->listeningSocket, SOMAXCONN) == SOCKET_ERROR) {
        error = L"无法监听本机回传端口 " + std::to_wstring(port) + L"。";
        Stop();
        return false;
    }
    impl_->token = std::move(token);
    impl_->stopping.store(false);
    impl_->serverThread = std::thread([this] { impl_->Serve(); });
    return true;
}

void PlaybackFeedbackServer::Stop() {
    if (!impl_) return;
    impl_->stopping.store(true);
    impl_->cookieCondition.notify_all();
    if (impl_->listeningSocket != INVALID_SOCKET) {
        shutdown(impl_->listeningSocket, SD_BOTH);
        closesocket(impl_->listeningSocket);
        impl_->listeningSocket = INVALID_SOCKET;
    }
    if (impl_->serverThread.joinable()) impl_->serverThread.join();
    if (impl_->winsockStarted) {
        WSACleanup();
        impl_->winsockStarted = false;
    }
}

bool PlaybackFeedbackServer::WaitForCookieJar(unsigned long timeoutMilliseconds,
                                              std::string& cookieJar,
                                              std::wstring& error) {
    std::unique_lock lock(impl_->mutex);
    const bool received = impl_->cookieCondition.wait_for(
        lock, std::chrono::milliseconds(timeoutMilliseconds),
        [this] { return impl_->cookieReceived || impl_->stopping.load(); });
    if (!received || !impl_->cookieReceived) {
        error = L"等待油猴脚本传送 Cookie 超时。";
        return false;
    }
    cookieJar = std::move(impl_->cookieJar);
    impl_->cookieJar.clear();
    return true;
}

void PlaybackFeedbackServer::SetPhase(std::string phase) {
    std::lock_guard lock(impl_->mutex);
    impl_->phase = std::move(phase);
    impl_->Touch();
}

void PlaybackFeedbackServer::SetError(std::string error) {
    std::lock_guard lock(impl_->mutex);
    impl_->phase = "error";
    impl_->error = std::move(error);
    impl_->Touch();
}

void PlaybackFeedbackServer::SetPreflightRunning() {
    std::lock_guard lock(impl_->mutex);
    impl_->phase = "preflight";
    impl_->preflightStatus = "running";
    impl_->Touch();
}

void PlaybackFeedbackServer::SetPreflightSkipped(std::string reason) {
    std::lock_guard lock(impl_->mutex);
    impl_->preflightStatus = "skipped";
    impl_->preflightError = std::move(reason);
    impl_->Touch();
}

void PlaybackFeedbackServer::SetPreflightSucceeded(
    std::string id, std::string title, std::string formatId,
    std::string resolution) {
    std::lock_guard lock(impl_->mutex);
    impl_->preflightStatus = "ok";
    impl_->preflightId = std::move(id);
    impl_->preflightTitle = std::move(title);
    impl_->preflightFormatId = std::move(formatId);
    impl_->preflightResolution = std::move(resolution);
    impl_->Touch();
}

void PlaybackFeedbackServer::SetPreflightFailed(std::string error) {
    std::lock_guard lock(impl_->mutex);
    impl_->phase = "error";
    impl_->preflightStatus = "failed";
    impl_->preflightError = error;
    impl_->error = std::move(error);
    impl_->Touch();
}

void PlaybackFeedbackServer::SetIpcConnecting() {
    std::lock_guard lock(impl_->mutex);
    impl_->ipcStatus = "connecting";
    impl_->ipcError.clear();
    impl_->Touch();
}

void PlaybackFeedbackServer::SetIpcConnected() {
    std::lock_guard lock(impl_->mutex);
    impl_->ipcStatus = "connected";
    impl_->ipcError.clear();
    impl_->Touch();
}

void PlaybackFeedbackServer::SetIpcFailed(std::string error) {
    std::lock_guard lock(impl_->mutex);
    impl_->ipcStatus = "failed";
    impl_->ipcError = std::move(error);
    impl_->Touch();
}

void PlaybackFeedbackServer::SetPlaybackNumber(std::string_view property,
                                               double value) {
    if (!std::isfinite(value)) return;
    std::lock_guard lock(impl_->mutex);
    if (property == "time-pos") impl_->timePos = value;
    else if (property == "duration") impl_->duration = value;
    else if (property == "playlist-pos") impl_->playlistPos = value;
    impl_->Touch();
    impl_->playbackUpdatedAt = impl_->updatedAt;
}

void PlaybackFeedbackServer::SetPlaybackBoolean(std::string_view property,
                                                bool value) {
    std::lock_guard lock(impl_->mutex);
    if (property == "pause") impl_->pause = value;
    impl_->Touch();
    impl_->playbackUpdatedAt = impl_->updatedAt;
}

void PlaybackFeedbackServer::SetPlaybackString(std::string_view property,
                                               std::string value) {
    std::lock_guard lock(impl_->mutex);
    if (property == "media-title") impl_->mediaTitle = std::move(value);
    else if (property == "path") impl_->path = std::move(value);
    impl_->Touch();
    impl_->playbackUpdatedAt = impl_->updatedAt;
}

void PlaybackFeedbackServer::SetExitCode(unsigned long exitCode) {
    std::lock_guard lock(impl_->mutex);
    impl_->exitCode = exitCode;
    impl_->phase = "ended";
    impl_->Touch();
}

bool PlaybackFeedbackServer::IsRunning() const {
    return impl_ && impl_->listeningSocket != INVALID_SOCKET &&
           !impl_->stopping.load();
}
