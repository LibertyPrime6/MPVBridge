#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

class PlaybackFeedbackServer {
public:
    PlaybackFeedbackServer();
    ~PlaybackFeedbackServer();

    PlaybackFeedbackServer(const PlaybackFeedbackServer&) = delete;
    PlaybackFeedbackServer& operator=(const PlaybackFeedbackServer&) = delete;

    bool Start(uint16_t port, std::string token, std::wstring& error);
    void Stop();
    bool WaitForCookieJar(unsigned long timeoutMilliseconds, std::string& cookieJar,
                          std::wstring& error);

    void SetPhase(std::string phase);
    void SetError(std::string error);
    void SetPreflightRunning();
    void SetPreflightSkipped(std::string reason);
    void SetPreflightSucceeded(std::string id, std::string title,
                               std::string formatId, std::string resolution);
    void SetPreflightFailed(std::string error);
    void SetIpcConnecting();
    void SetIpcConnected();
    void SetIpcFailed(std::string error);
    void SetPlaybackNumber(std::string_view property, double value);
    void SetPlaybackBoolean(std::string_view property, bool value);
    void SetPlaybackString(std::string_view property, std::string value);
    void SetExitCode(unsigned long exitCode);

    bool IsRunning() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
