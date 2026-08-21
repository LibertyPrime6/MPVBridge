#include "ProtocolHandler.h"

#include "AppCore.h"

#include <shellapi.h>
#include <shlobj.h>

#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr wchar_t kClassesKey[] = L"Software\\Classes\\mpvbridge";
constexpr wchar_t kCommandKey[] =
    L"Software\\Classes\\mpvbridge\\shell\\open\\command";
constexpr size_t kMaximumPayloadCharacters = 4 * 1024 * 1024;

bool StartsWithInsensitive(std::wstring_view value, std::wstring_view prefix) {
    return value.size() >= prefix.size() &&
           EqualsInsensitive(value.substr(0, prefix.size()), prefix);
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

std::wstring ExpectedOpenCommand() {
    return QuoteCommandArgument(GetModulePath().wstring()) + L" \"%1\"";
}

std::optional<std::wstring> ReadRegistryString(HKEY root,
                                               const wchar_t* subKey,
                                               const wchar_t* valueName) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(root, subKey, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) {
        return std::nullopt;
    }
    DWORD type = 0;
    DWORD bytes = 0;
    const LSTATUS measured =
        RegQueryValueExW(key, valueName, nullptr, &type, nullptr, &bytes);
    if (measured != ERROR_SUCCESS ||
        (type != REG_SZ && type != REG_EXPAND_SZ) || bytes < sizeof(wchar_t)) {
        RegCloseKey(key);
        return std::nullopt;
    }
    std::vector<wchar_t> buffer(bytes / sizeof(wchar_t) + 1, L'\0');
    const LSTATUS read = RegQueryValueExW(
        key, valueName, nullptr, &type,
        reinterpret_cast<BYTE*>(buffer.data()), &bytes);
    RegCloseKey(key);
    if (read != ERROR_SUCCESS) {
        return std::nullopt;
    }
    return std::wstring(buffer.data());
}

bool SetRegistryString(const wchar_t* subKey, const wchar_t* valueName,
                       std::wstring_view value, std::wstring& error) {
    HKEY key = nullptr;
    DWORD disposition = 0;
    const LSTATUS created = RegCreateKeyExW(
        HKEY_CURRENT_USER, subKey, 0, nullptr, REG_OPTION_NON_VOLATILE,
        KEY_SET_VALUE, nullptr, &key, &disposition);
    if (created != ERROR_SUCCESS) {
        error = FormatSystemError(static_cast<DWORD>(created));
        return false;
    }
    const std::wstring valueCopy(value);
    const DWORD bytes =
        static_cast<DWORD>((valueCopy.size() + 1) * sizeof(wchar_t));
    const LSTATUS written = RegSetValueExW(
        key, valueName, 0, REG_SZ,
        reinterpret_cast<const BYTE*>(valueCopy.c_str()), bytes);
    RegCloseKey(key);
    if (written != ERROR_SUCCESS) {
        error = FormatSystemError(static_cast<DWORD>(written));
        return false;
    }
    return true;
}

int Base64Value(wchar_t ch) {
    if (ch >= L'A' && ch <= L'Z') return ch - L'A';
    if (ch >= L'a' && ch <= L'z') return ch - L'a' + 26;
    if (ch >= L'0' && ch <= L'9') return ch - L'0' + 52;
    if (ch == L'-' || ch == L'+') return 62;
    if (ch == L'_' || ch == L'/') return 63;
    return -1;
}

bool DecodeBase64Url(std::wstring_view encoded, std::vector<unsigned char>& bytes,
                     std::wstring& error) {
    if (encoded.empty()) {
        error = L"mpvbridge:// 缺少播放参数。";
        return false;
    }
    if (encoded.size() > kMaximumPayloadCharacters) {
        error = L"mpvbridge:// 播放参数过长。";
        return false;
    }
    bytes.clear();
    bytes.reserve(encoded.size() * 3 / 4 + 3);
    uint32_t accumulator = 0;
    int bits = 0;
    bool paddingStarted = false;
    for (const wchar_t ch : encoded) {
        if (ch == L'=') {
            paddingStarted = true;
            continue;
        }
        if (paddingStarted) {
            error = L"mpvbridge:// 参数的 Base64 填充无效。";
            return false;
        }
        const int value = Base64Value(ch);
        if (value < 0) {
            error = L"mpvbridge:// 参数不是有效的 Base64URL。";
            return false;
        }
        accumulator = (accumulator << 6) | static_cast<uint32_t>(value);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            bytes.push_back(
                static_cast<unsigned char>((accumulator >> bits) & 0xff));
        }
    }
    if (bits == 6 || (bits > 0 && (accumulator & ((1u << bits) - 1u)) != 0)) {
        error = L"mpvbridge:// 参数的 Base64URL 长度无效。";
        return false;
    }
    return true;
}

bool Utf8ToWide(const std::vector<unsigned char>& bytes, std::wstring& output,
                std::wstring& error) {
    if (bytes.empty()) {
        error = L"mpvbridge:// 解码后没有播放参数。";
        return false;
    }
    if (bytes.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
        error = L"mpvbridge:// 播放参数过长。";
        return false;
    }
    const int inputLength = static_cast<int>(bytes.size());
    const int required = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS,
        reinterpret_cast<const char*>(bytes.data()), inputLength, nullptr, 0);
    if (required <= 0) {
        error = L"mpvbridge:// 参数不是有效的 UTF-8 文本。";
        return false;
    }
    output.resize(static_cast<size_t>(required));
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                        reinterpret_cast<const char*>(bytes.data()), inputLength,
                        output.data(), required);
    if (output.find(L'\0') != std::wstring::npos || !HasVisibleText(output)) {
        error = L"mpvbridge:// 解码后的播放参数无效。";
        output.clear();
        return false;
    }
    return true;
}

ProtocolLaunchRequest DecodeUri(std::wstring_view uri) {
    ProtocolLaunchRequest result;
    constexpr std::wstring_view prefix = L"mpvbridge://";
    if (!StartsWithInsensitive(uri, prefix)) {
        return result;
    }
    result.matched = true;
    const size_t query = uri.find(L'?', prefix.size());
    if (query == std::wstring_view::npos || query + 1 >= uri.size()) {
        result.error = L"mpvbridge:// 链接缺少播放载荷。";
        return result;
    }
    std::wstring_view payload = uri.substr(query + 1);
    if (StartsWithInsensitive(payload, L"payload=")) {
        payload.remove_prefix(8);
    }
    const size_t fragment = payload.find(L'#');
    if (fragment != std::wstring_view::npos) {
        payload = payload.substr(0, fragment);
    }
    std::vector<unsigned char> decoded;
    if (!DecodeBase64Url(payload, decoded, result.error)) {
        return result;
    }
    Utf8ToWide(decoded, result.passThroughTail, result.error);
    return result;
}

} // namespace

ProtocolLaunchRequest ParseMpvBridgeProtocolCommandLine(
    std::wstring_view commandLine) {
    ProtocolLaunchRequest result;
    const std::wstring commandLineCopy(commandLine);
    int argumentCount = 0;
    wchar_t** arguments =
        CommandLineToArgvW(commandLineCopy.c_str(), &argumentCount);
    if (arguments == nullptr) {
        result.error = L"无法读取 mpvbridge:// 启动参数：" +
                       FormatSystemError(GetLastError());
        return result;
    }
    for (int index = 1; index < argumentCount; ++index) {
        ProtocolLaunchRequest candidate = DecodeUri(arguments[index]);
        if (!candidate.matched) continue;
        if (result.matched) {
            result.error = L"一次只能处理一个 mpvbridge:// 链接。";
            result.passThroughTail.clear();
            break;
        }
        result = std::move(candidate);
    }
    LocalFree(arguments);
    return result;
}

ProtocolRegistrationStatus GetMpvBridgeProtocolStatus() {
    ProtocolRegistrationStatus status;
    const std::optional<std::wstring> command =
        ReadRegistryString(HKEY_CURRENT_USER, kCommandKey, nullptr);
    if (!command.has_value()) {
        return status;
    }
    status.registered = true;
    status.command = *command;
    status.ownedByCurrentExecutable =
        EqualsInsensitive(status.command, ExpectedOpenCommand());
    return status;
}

bool RegisterMpvBridgeProtocol(std::wstring& error) {
    error.clear();
    const std::wstring module = GetModulePath().wstring();
    if (!SetRegistryString(kClassesKey, nullptr, L"URL:MPVBridge Protocol", error) ||
        !SetRegistryString(kClassesKey, L"URL Protocol", L"", error) ||
        !SetRegistryString((std::wstring(kClassesKey) + L"\\DefaultIcon").c_str(),
                           nullptr, QuoteCommandArgument(module) + L",0", error) ||
        !SetRegistryString(kCommandKey, nullptr, ExpectedOpenCommand(), error)) {
        return false;
    }
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return true;
}

bool UnregisterMpvBridgeProtocol(std::wstring& error) {
    error.clear();
    const ProtocolRegistrationStatus status = GetMpvBridgeProtocolStatus();
    if (!status.registered) {
        return true;
    }
    if (!status.ownedByCurrentExecutable) {
        error = L"mpvbridge:// 当前由其他程序或另一位置的 MPVBridge 注册，"
                L"为避免误删，已拒绝注销。\n\n当前命令：\n" + status.command;
        return false;
    }
    const LSTATUS deleted = RegDeleteTreeW(HKEY_CURRENT_USER, kClassesKey);
    if (deleted != ERROR_SUCCESS && deleted != ERROR_FILE_NOT_FOUND) {
        error = FormatSystemError(static_cast<DWORD>(deleted));
        return false;
    }
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return true;
}
