#pragma once

#include <string>
#include <string_view>

inline constexpr wchar_t kMpvBridgeScheme[] = L"mpvbridge";

struct ProtocolLaunchRequest {
    bool matched{};
    std::wstring passThroughTail;
    std::wstring error;
};

struct ProtocolRegistrationStatus {
    bool registered{};
    bool ownedByCurrentExecutable{};
    std::wstring command;
};

ProtocolLaunchRequest ParseMpvBridgeProtocolCommandLine(
    std::wstring_view commandLine);
ProtocolRegistrationStatus GetMpvBridgeProtocolStatus();
bool RegisterMpvBridgeProtocol(std::wstring& error);
bool UnregisterMpvBridgeProtocol(std::wstring& error);

