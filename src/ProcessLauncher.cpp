#include "ProcessLauncher.h"

#include "AppCore.h"
#include "Logger.h"
#include "MpvIpc.h"
#include "PlaybackFeedback.h"
#include "PortableEnvironment.h"
#include "YtdlpPreflight.h"
#include "resource.h"

#include <shellapi.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cwctype>
#include <filesystem>
#include <limits>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

constexpr std::wstring_view kCookieJarArgument =
    L"--mpvbridge-ytdl-cookie-jar=";
constexpr std::wstring_view kCookieTransferArgument =
    L"--mpvbridge-cookie-transfer=";
constexpr std::wstring_view kSessionArgument = L"--mpvbridge-session=";
constexpr std::wstring_view kFeedbackPortArgument =
    L"--mpvbridge-feedback-port=";
constexpr std::wstring_view kPreflightArgument = L"--mpvbridge-preflight=";
constexpr std::wstring_view kValidationOnlyArgument =
    L"--mpvbridge-validation-only=";
constexpr std::wstring_view kYtdlLabelScriptArgument =
    L"--mpvbridge-ytdl-label-script=";
constexpr std::wstring_view kYtdlLabelScriptFileName =
    L"External Player-YTDL Labels.lua";
constexpr size_t kMaximumCookiePayloadCharacters = 2 * 1024 * 1024;

struct WebIntegrationControl {
    bool enabled = false;
    std::wstring session;
    uint16_t feedbackPort = 0;
    bool expectsCookieTransfer = false;
    bool validationOnly = false;
};

bool StartsWithInsensitive(std::wstring_view value, std::wstring_view prefix) {
    return value.size() >= prefix.size() &&
           EqualsInsensitive(value.substr(0, prefix.size()), prefix);
}

bool ContainsInsensitive(std::wstring_view value, std::wstring_view needle) {
    if (needle.empty()) return true;
    return std::search(value.begin(), value.end(), needle.begin(), needle.end(),
                       [](wchar_t left, wchar_t right) {
                           return towlower(left) == towlower(right);
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
        CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (required <= 0) return "Windows error";
    std::string result(static_cast<size_t>(required), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                        static_cast<int>(value.size()), result.data(), required,
                        nullptr, nullptr);
    return result;
}

bool IsValidSessionToken(std::wstring_view value) {
    if (value.size() < 16 || value.size() > 128) return false;
    return std::all_of(value.begin(), value.end(), [](wchar_t ch) {
        return (ch >= L'a' && ch <= L'z') || (ch >= L'A' && ch <= L'Z') ||
               (ch >= L'0' && ch <= L'9') || ch == L'-' || ch == L'_';
    });
}

bool ParseFeedbackPort(std::wstring_view value, uint16_t& port) {
    if (value.empty()) return false;
    unsigned long parsed = 0;
    try {
        size_t consumed = 0;
        parsed = std::stoul(std::wstring(value), &consumed, 10);
        if (consumed != value.size()) return false;
    } catch (...) {
        return false;
    }
    if (parsed < 1024 || parsed > 65535) return false;
    port = static_cast<uint16_t>(parsed);
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

bool DecodeCookiePayload(std::wstring_view encoded,
                         std::vector<unsigned char>& bytes,
                         std::wstring& error) {
    if (encoded.empty() || encoded.size() > kMaximumCookiePayloadCharacters) {
        error = L"网页传入的 Cookie 文件为空或过长。";
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
            error = L"网页传入的 Cookie Base64 填充无效。";
            return false;
        }
        const int value = Base64Value(ch);
        if (value < 0) {
            error = L"网页传入的 Cookie 不是有效的 Base64URL。";
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
    if (bits == 6 ||
        (bits > 0 && (accumulator & ((1u << bits) - 1u)) != 0)) {
        error = L"网页传入的 Cookie Base64URL 长度无效。";
        return false;
    }
    constexpr std::string_view header = "# Netscape HTTP Cookie File";
    if (bytes.size() <= header.size() ||
        !std::equal(header.begin(), header.end(), bytes.begin()) ||
        std::find(bytes.begin(), bytes.end(), 0) != bytes.end() ||
        std::find(bytes.begin(), bytes.end(), '\t') == bytes.end()) {
        error = L"网页传入的内容不是有效的 Netscape Cookie 文件。";
        bytes.clear();
        return false;
    }
    return true;
}

bool ValidateCookieJar(std::string_view value,
                       std::vector<unsigned char>& bytes,
                       std::wstring& error) {
    if (value.empty() || value.size() > kMaximumCookiePayloadCharacters) {
        error = L"油猴脚本传送的 Cookie 文件为空或过长。";
        return false;
    }
    bytes.assign(value.begin(), value.end());
    constexpr std::string_view header = "# Netscape HTTP Cookie File";
    if (bytes.size() <= header.size() ||
        !std::equal(header.begin(), header.end(), bytes.begin()) ||
        std::find(bytes.begin(), bytes.end(), 0) != bytes.end() ||
        std::find(bytes.begin(), bytes.end(), '\t') == bytes.end()) {
        error = L"油猴脚本传送的内容不是有效的 Netscape Cookie 文件。";
        bytes.clear();
        return false;
    }
    return true;
}

bool WriteTemporaryCookieFile(const std::vector<unsigned char>& bytes,
                              std::filesystem::path& path,
                              std::wstring& error) {
    std::vector<wchar_t> tempBuffer(32768, L'\0');
    const DWORD tempLength =
        GetTempPathW(static_cast<DWORD>(tempBuffer.size()), tempBuffer.data());
    if (tempLength == 0 || tempLength >= tempBuffer.size()) {
        error = L"无法取得临时目录：" + FormatSystemError(GetLastError());
        return false;
    }
    const std::filesystem::path directory =
        std::filesystem::path(tempBuffer.data()) / L"MPVBridge";
    std::error_code directoryError;
    std::filesystem::create_directories(directory, directoryError);
    if (directoryError) {
        error = L"无法创建 MPVBridge 临时目录。";
        return false;
    }

    std::vector<wchar_t> fileBuffer(32768, L'\0');
    if (GetTempFileNameW(directory.c_str(), L"MBC", 0, fileBuffer.data()) == 0) {
        error = L"无法创建临时 Cookie 文件名：" +
                FormatSystemError(GetLastError());
        return false;
    }
    path = fileBuffer.data();
    const HANDLE file = CreateFileW(
        path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_TEMPORARY, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        error = L"无法创建临时 Cookie 文件：" +
                FormatSystemError(GetLastError());
        DeleteFileW(path.c_str());
        path.clear();
        return false;
    }
    size_t offset = 0;
    bool writtenSuccessfully = true;
    while (offset < bytes.size()) {
        const size_t remaining = bytes.size() - offset;
        const DWORD chunk = static_cast<DWORD>(std::min<size_t>(
            remaining, static_cast<size_t>(std::numeric_limits<DWORD>::max())));
        DWORD written = 0;
        if (WriteFile(file, bytes.data() + offset, chunk, &written, nullptr) ==
                FALSE ||
            written != chunk) {
            writtenSuccessfully = false;
            error = L"无法写入临时 Cookie 文件：" +
                    FormatSystemError(GetLastError());
            break;
        }
        offset += written;
    }
    CloseHandle(file);
    if (!writtenSuccessfully) {
        DeleteFileW(path.c_str());
        path.clear();
        return false;
    }
    return true;
}

bool PrepareYtdlLabelScript(std::filesystem::path& path, bool& isTemporary,
                            std::wstring& error) {
    path.clear();
    isTemporary = false;
    const auto useSiblingScript = [&]() {
        const std::filesystem::path sibling =
            GetModulePath().parent_path() / kYtdlLabelScriptFileName;
        std::error_code statusError;
        if (std::filesystem::is_regular_file(sibling, statusError) &&
            std::filesystem::file_size(sibling, statusError) > 0 &&
            !statusError) {
            path = sibling;
            return true;
        }
        return false;
    };

    const HMODULE module = GetModuleHandleW(nullptr);
    const HRSRC resource =
        module == nullptr
            ? nullptr
            : FindResourceW(module, MAKEINTRESOURCEW(IDR_YTDL_LABEL_SCRIPT),
                            RT_RCDATA);
    if (resource == nullptr) {
        if (useSiblingScript()) return true;
        error = L"MPVBridge 未内嵌 yt-dlp 轨道命名 Lua，且程序同目录也找不到“" +
                std::wstring(kYtdlLabelScriptFileName) + L"”。";
        return false;
    }
    const HGLOBAL loaded = LoadResource(module, resource);
    const DWORD resourceSize = SizeofResource(module, resource);
    const void* resourceData = loaded == nullptr ? nullptr : LockResource(loaded);
    if (resourceData == nullptr || resourceSize == 0) {
        if (useSiblingScript()) return true;
        error = L"无法读取 MPVBridge 内嵌的 yt-dlp 轨道命名 Lua。";
        return false;
    }

    std::vector<wchar_t> tempBuffer(32768, L'\0');
    const DWORD tempLength =
        GetTempPathW(static_cast<DWORD>(tempBuffer.size()), tempBuffer.data());
    if (tempLength == 0 || tempLength >= tempBuffer.size()) {
        if (useSiblingScript()) return true;
        error = L"无法取得 Lua 临时目录：" + FormatSystemError(GetLastError());
        return false;
    }
    const std::filesystem::path directory =
        std::filesystem::path(tempBuffer.data()) / L"MPVBridge";
    std::error_code directoryError;
    std::filesystem::create_directories(directory, directoryError);
    if (directoryError) {
        if (useSiblingScript()) return true;
        error = L"无法创建 MPVBridge Lua 临时目录。";
        return false;
    }

    HANDLE file = INVALID_HANDLE_VALUE;
    for (unsigned int attempt = 0; attempt < 8; ++attempt) {
        path = directory /
               (L"YtdlLabels-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                std::to_wstring(GetTickCount64()) + L"-" +
                std::to_wstring(attempt) + L".lua");
        file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                           FILE_ATTRIBUTE_TEMPORARY, nullptr);
        if (file != INVALID_HANDLE_VALUE || GetLastError() != ERROR_FILE_EXISTS) {
            break;
        }
    }
    if (file == INVALID_HANDLE_VALUE) {
        path.clear();
        if (useSiblingScript()) return true;
        error = L"无法创建临时 Lua 文件：" + FormatSystemError(GetLastError());
        return false;
    }

    const auto* bytes = static_cast<const unsigned char*>(resourceData);
    size_t offset = 0;
    bool writtenSuccessfully = true;
    while (offset < resourceSize) {
        const DWORD chunk = static_cast<DWORD>(std::min<size_t>(
            resourceSize - offset,
            static_cast<size_t>(std::numeric_limits<DWORD>::max())));
        DWORD written = 0;
        if (WriteFile(file, bytes + offset, chunk, &written, nullptr) == FALSE ||
            written != chunk) {
            writtenSuccessfully = false;
            error = L"无法写入临时 Lua 文件：" +
                    FormatSystemError(GetLastError());
            break;
        }
        offset += written;
    }
    CloseHandle(file);
    if (!writtenSuccessfully) {
        DeleteFileW(path.c_str());
        path.clear();
        if (useSiblingScript()) {
            error.clear();
            return true;
        }
        return false;
    }
    isTemporary = true;
    return true;
}

std::wstring EscapeYtdlpRawValue(std::wstring_view value) {
    std::wstring result;
    result.reserve(value.size());
    for (const wchar_t ch : value) {
        if (ch == L']') result.push_back(L'\\');
        result.push_back(ch);
    }
    return L"[" + result + L"]";
}

void AddCookieRawOption(std::vector<std::wstring>& arguments,
                        const std::filesystem::path& temporaryCookieFile) {
    const std::wstring cookieOption =
        L"cookies=" + EscapeYtdlpRawValue(temporaryCookieFile.wstring());
    bool appended = false;
    constexpr std::wstring_view rawOptionsPrefix = L"--ytdl-raw-options=";
    for (std::wstring& argument : arguments) {
        if (!appended && StartsWithInsensitive(argument, rawOptionsPrefix)) {
            if (argument.size() > rawOptionsPrefix.size()) argument.push_back(L',');
            argument.append(cookieOption);
            appended = true;
        }
    }
    if (!appended) {
        arguments.emplace_back(std::wstring(rawOptionsPrefix) + cookieOption);
    }
}

void EnsureNodeJsRuntimeOption(std::vector<std::wstring>& arguments) {
    if (!DetectEnvironmentTool(EnvironmentTool::Node).available) return;
    constexpr std::wstring_view rawOptionsPrefix = L"--ytdl-raw-options=";
    for (std::wstring& argument : arguments) {
        if (!StartsWithInsensitive(argument, rawOptionsPrefix)) continue;
        if (ContainsInsensitive(argument.substr(rawOptionsPrefix.size()),
                                L"js-runtimes=")) {
            return;
        }
        if (argument.size() > rawOptionsPrefix.size()) argument.push_back(L',');
        argument.append(L"js-runtimes=node");
        return;
    }
    arguments.emplace_back(L"--ytdl-raw-options=js-runtimes=node");
}

void RebuildTail(const std::vector<std::wstring>& arguments,
                 std::wstring& preparedTail) {
    preparedTail.clear();
    for (const std::wstring& argument : arguments) {
        preparedTail.push_back(L' ');
        preparedTail.append(QuoteCommandArgument(argument));
    }
}

bool InjectCookieRawOption(std::wstring& preparedTail,
                           const std::filesystem::path& temporaryCookieFile,
                           std::wstring& error) {
    const std::wstring parseLine = L"mpvbridge-cookie.exe" + preparedTail;
    int count = 0;
    wchar_t** values = CommandLineToArgvW(parseLine.c_str(), &count);
    if (values == nullptr) {
        error = L"无法为 yt-dlp 加入临时 Cookie 文件：" +
                FormatSystemError(GetLastError());
        return false;
    }
    std::vector<std::wstring> arguments;
    for (int index = 1; index < count; ++index) arguments.emplace_back(values[index]);
    LocalFree(values);
    AddCookieRawOption(arguments, temporaryCookieFile);
    RebuildTail(arguments, preparedTail);
    return true;
}

bool PrepareLaunchTail(std::wstring_view originalTail, std::wstring& preparedTail,
                       std::filesystem::path& temporaryCookieFile,
                       std::filesystem::path& ytdlLabelScript,
                       bool& ytdlLabelScriptIsTemporary,
                       WebIntegrationControl& webControl, std::wstring& error) {
    const std::wstring parseLine = L"mpvbridge-arguments.exe" +
                                   std::wstring(originalTail);
    int argumentCount = 0;
    wchar_t** parsed = CommandLineToArgvW(parseLine.c_str(), &argumentCount);
    if (parsed == nullptr) {
        error = L"无法读取 MPV 播放参数：" + FormatSystemError(GetLastError());
        return false;
    }

    std::vector<std::wstring> arguments;
    std::wstring cookiePayload;
    bool foundBridgeArgument = false;
    bool sessionSeen = false;
    bool portSeen = false;
    bool preflightSeen = false;
    bool cookieTransferSeen = false;
    bool validationOnlySeen = false;
    bool ytdlLabelScriptSeen = false;
    bool ytdlLabelScriptRequested = false;
    for (int index = 1; index < argumentCount; ++index) {
        const std::wstring_view argument(parsed[index]);
        if (StartsWithInsensitive(argument, kCookieJarArgument)) {
            foundBridgeArgument = true;
            if (!cookiePayload.empty()) {
                LocalFree(parsed);
                error = L"一次播放只能传入一个临时 Cookie 文件。";
                return false;
            }
            cookiePayload = argument.substr(kCookieJarArgument.size());
            continue;
        }
        if (StartsWithInsensitive(argument, kSessionArgument)) {
            foundBridgeArgument = true;
            if (sessionSeen) {
                LocalFree(parsed);
                error = L"油猴网页会话令牌只能指定一次。";
                return false;
            }
            sessionSeen = true;
            webControl.session = argument.substr(kSessionArgument.size());
            continue;
        }
        if (StartsWithInsensitive(argument, kCookieTransferArgument)) {
            foundBridgeArgument = true;
            if (cookieTransferSeen ||
                argument.substr(kCookieTransferArgument.size()) != L"1") {
                LocalFree(parsed);
                error = L"油猴网页 Cookie 传输参数无效或重复。";
                return false;
            }
            cookieTransferSeen = true;
            webControl.expectsCookieTransfer = true;
            continue;
        }
        if (StartsWithInsensitive(argument, kFeedbackPortArgument)) {
            foundBridgeArgument = true;
            if (portSeen ||
                !ParseFeedbackPort(argument.substr(kFeedbackPortArgument.size()),
                                   webControl.feedbackPort)) {
                LocalFree(parsed);
                error = L"油猴网页回传端口无效或重复。";
                return false;
            }
            portSeen = true;
            continue;
        }
        if (StartsWithInsensitive(argument, kPreflightArgument)) {
            foundBridgeArgument = true;
            if (preflightSeen ||
                argument.substr(kPreflightArgument.size()) != L"1") {
                LocalFree(parsed);
                error = L"油猴网页预检参数无效或重复。";
                return false;
            }
            preflightSeen = true;
            continue;
        }
        if (StartsWithInsensitive(argument, kValidationOnlyArgument)) {
            foundBridgeArgument = true;
            if (validationOnlySeen ||
                argument.substr(kValidationOnlyArgument.size()) != L"1") {
                LocalFree(parsed);
                error = L"油猴 Cookie 预检模式参数无效或重复。";
                return false;
            }
            validationOnlySeen = true;
            webControl.validationOnly = true;
            continue;
        }
        if (StartsWithInsensitive(argument, kYtdlLabelScriptArgument)) {
            foundBridgeArgument = true;
            if (ytdlLabelScriptSeen ||
                argument.substr(kYtdlLabelScriptArgument.size()) != L"1") {
                LocalFree(parsed);
                error = L"yt-dlp 轨道命名 Lua 参数无效或重复。";
                return false;
            }
            ytdlLabelScriptSeen = true;
            ytdlLabelScriptRequested = true;
            continue;
        }
        arguments.emplace_back(argument);
    }
    LocalFree(parsed);

    if (cookieTransferSeen && !cookiePayload.empty()) {
        error = L"Cookie 只能通过网页 POST 或旧版命令参数传入一种。";
        return false;
    }
    const bool anyWebControl = sessionSeen || portSeen || preflightSeen;
    if (anyWebControl && !(sessionSeen && portSeen && preflightSeen)) {
        error = L"油猴网页集成参数不完整，已拒绝启用 JSON IPC。";
        return false;
    }
    if (anyWebControl && !IsValidSessionToken(webControl.session)) {
        error = L"油猴网页会话令牌无效。";
        return false;
    }
    if (cookieTransferSeen && !anyWebControl) {
        error = L"Cookie 传输只能用于完整的油猴网页会话。";
        return false;
    }
    if (validationOnlySeen &&
        (!anyWebControl || !cookieTransferSeen)) {
        error = L"Cookie 预检模式需要完整网页会话和临时 Cookie 传输。";
        return false;
    }
    webControl.enabled = anyWebControl;

    // The companion userscript already requests Node for YouTube. Keep this
    // guarantee inside the Bridge as well so older script copies still work.
    // Only the runtime name is persisted in the command; PATH resolves either
    // the system installation or the relative Tools fallback at launch time.
    if (webControl.enabled) EnsureNodeJsRuntimeOption(arguments);

    // The common path remains byte-for-byte pass-through. In particular, an
    // unrelated application that supplies its own --input-ipc-server is never
    // inspected or rewritten by MPVBridge.
    if (!foundBridgeArgument) {
        preparedTail = originalTail;
        return true;
    }

    if (!cookiePayload.empty()) {
        std::vector<unsigned char> cookieBytes;
        if (!DecodeCookiePayload(cookiePayload, cookieBytes, error) ||
            !WriteTemporaryCookieFile(cookieBytes, temporaryCookieFile, error)) {
            return false;
        }

        AddCookieRawOption(arguments, temporaryCookieFile);
    }

    if (ytdlLabelScriptRequested) {
        if (!PrepareYtdlLabelScript(ytdlLabelScript,
                                    ytdlLabelScriptIsTemporary, error)) {
            return false;
        }
        arguments.emplace_back(L"--script=" + ytdlLabelScript.wstring());
    }

    RebuildTail(arguments, preparedTail);
    return true;
}

void DeleteTemporaryCookieFile(const ProfileStore& store,
                               const std::filesystem::path& path) {
    if (path.empty()) return;
    if (DeleteFileW(path.c_str()) == FALSE) {
        WriteDiagnosticLog(store, L"临时 Cookie 文件删除失败：" +
                                      FormatSystemError(GetLastError()));
    } else {
        WriteDiagnosticLog(store, L"临时 Cookie 文件已删除");
    }
}

void DeleteTemporaryYtdlLabelScript(const ProfileStore& store,
                                    const std::filesystem::path& path,
                                    bool isTemporary) {
    if (!isTemporary || path.empty()) return;
    if (DeleteFileW(path.c_str()) == FALSE) {
        WriteDiagnosticLog(store, L"临时 yt-dlp 轨道命名 Lua 删除失败：" +
                                      FormatSystemError(GetLastError()));
    } else {
        WriteDiagnosticLog(store, L"临时 yt-dlp 轨道命名 Lua 已删除");
    }
}

} // namespace

DWORD LaunchAndWait(const ProfileStore& store, const Profile& profile,
                    std::wstring_view passThroughTail) {
    std::wstring preparedTail;
    std::filesystem::path temporaryCookieFile;
    std::filesystem::path ytdlLabelScript;
    bool ytdlLabelScriptIsTemporary = false;
    WebIntegrationControl webControl;
    std::wstring prepareError;
    if (!PrepareLaunchTail(passThroughTail, preparedTail, temporaryCookieFile,
                           ytdlLabelScript, ytdlLabelScriptIsTemporary,
                           webControl, prepareError)) {
        WriteDiagnosticLog(store, L"准备网页播放参数失败");
        ShowError(L"无法准备网页播放参数：\n" + prepareError);
        DeleteTemporaryCookieFile(store, temporaryCookieFile);
        DeleteTemporaryYtdlLabelScript(store, ytdlLabelScript,
                                       ytdlLabelScriptIsTemporary);
        return 1;
    }
    if (!temporaryCookieFile.empty()) {
        WriteDiagnosticLog(store, L"已为 yt-dlp 创建临时 Cookie 文件");
    }
    if (!ytdlLabelScript.empty()) {
        WriteDiagnosticLog(
            store, ytdlLabelScriptIsTemporary
                       ? L"已从 MPVBridge 内嵌资源释放 yt-dlp 轨道命名 Lua"
                       : L"正在使用 MPVBridge 同目录的 yt-dlp 轨道命名 Lua");
    }

    PlaybackFeedbackServer feedback;
    if (webControl.enabled) {
        std::wstring feedbackError;
        if (!feedback.Start(webControl.feedbackPort,
                            WideToUtf8(webControl.session), feedbackError)) {
            WriteDiagnosticLog(store, L"无法启动油猴网页回传服务：" + feedbackError);
            ShowError(L"无法启动油猴网页回传服务：\n" + feedbackError);
            DeleteTemporaryCookieFile(store, temporaryCookieFile);
            DeleteTemporaryYtdlLabelScript(store, ytdlLabelScript,
                                           ytdlLabelScriptIsTemporary);
            return 1;
        }

        if (webControl.expectsCookieTransfer) {
            feedback.SetPhase("waiting-cookie");
            std::string transferredCookieJar;
            std::wstring transferError;
            std::vector<unsigned char> cookieBytes;
            if (!feedback.WaitForCookieJar(30000, transferredCookieJar,
                                           transferError) ||
                !ValidateCookieJar(transferredCookieJar, cookieBytes,
                                   transferError) ||
                !WriteTemporaryCookieFile(cookieBytes, temporaryCookieFile,
                                          transferError) ||
                !InjectCookieRawOption(preparedTail, temporaryCookieFile,
                                       transferError)) {
                feedback.SetError(WideToUtf8(transferError));
                WriteDiagnosticLog(store, L"接收油猴 Cookie 失败：" + transferError);
                std::this_thread::sleep_for(std::chrono::seconds(5));
                feedback.Stop();
                DeleteTemporaryCookieFile(store, temporaryCookieFile);
                DeleteTemporaryYtdlLabelScript(store, ytdlLabelScript,
                                               ytdlLabelScriptIsTemporary);
                return 1;
            }
            transferredCookieJar.clear();
            WriteDiagnosticLog(store, L"已接收油猴 Cookie 临时副本");
        }

        if (webControl.validationOnly) {
            // 导入 Cookie 时仍需要独立 yt-dlp 预检来确认 Cookie jar 真能被
            // 提取器使用；普通播放已经由 MPV 自己调用 yt-dlp，若这里再模拟
            // 一遍会造成完全重复的网络解析，Bilibili 风控或网络波动时甚至会
            // 先阻塞到 120 秒才启动 MPV。
            feedback.SetPreflightRunning();
            const YtdlpPreflightResult preflight =
                RunYtdlpPreflight(profile, preparedTail);
            if (!preflight.attempted || !preflight.success) {
                const std::string preflightError = preflight.error.empty()
                    ? "yt-dlp validation preflight failed"
                    : preflight.error;
                feedback.SetPreflightFailed(preflightError);
                WriteDiagnosticLog(store, L"Cookie yt-dlp 预检失败");
                std::this_thread::sleep_for(std::chrono::seconds(5));
                feedback.Stop();
                DeleteTemporaryCookieFile(store, temporaryCookieFile);
                DeleteTemporaryYtdlLabelScript(store, ytdlLabelScript,
                                               ytdlLabelScriptIsTemporary);
                return 1;
            }
            feedback.SetPreflightSucceeded(
                preflight.id, preflight.title, preflight.formatId,
                preflight.resolution);
            feedback.SetPhase("validated");
            WriteDiagnosticLog(store, L"油猴 Cookie yt-dlp 预检成功；未启动 MPV");
            std::this_thread::sleep_for(std::chrono::seconds(3));
            feedback.Stop();
            DeleteTemporaryCookieFile(store, temporaryCookieFile);
            DeleteTemporaryYtdlLabelScript(store, ytdlLabelScript,
                                           ytdlLabelScriptIsTemporary);
            return 0;
        }

        feedback.SetPreflightSkipped(
            "Normal playback delegates extraction directly to MPV/yt-dlp");
        WriteDiagnosticLog(store, L"普通播放跳过重复 yt-dlp 预检，直接启动 MPV");

        const std::wstring pipeName =
            L"\\\\.\\pipe\\MPVBridge-" + webControl.session;
        preparedTail.push_back(L' ');
        preparedTail.append(
            QuoteCommandArgument(L"--input-ipc-server=" + pipeName));
        feedback.SetPhase("launching");
    }

    std::wstring commandLine = L"\"" + profile.executable.wstring() + L"\"";
    commandLine.append(preparedTail);
    std::vector<wchar_t> writable(commandLine.begin(), commandLine.end());
    writable.push_back(L'\0');

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);

    PROCESS_INFORMATION process{};
    const std::filesystem::path workingDirectory =
        profile.executable.parent_path();
    BOOL processCreated = FALSE;
    {
        // System PATH entries stay first. MPVBridge-local Tools folders are
        // appended only as a portable fallback and never persisted.
        ScopedPortableChildPath portablePath;
        processCreated = CreateProcessW(
            profile.executable.c_str(), writable.data(), nullptr, nullptr, TRUE,
            0, nullptr, workingDirectory.c_str(), &startup, &process);
    }
    if (processCreated == FALSE) {
        WriteDiagnosticLog(store, L"CreateProcessW 失败：" +
                                      FormatSystemError(GetLastError()));
        ShowError(L"无法启动目标 MPV：\n" + profile.executable.wstring() +
                  L"\n\n系统错误：" + FormatSystemError(GetLastError()));
        if (webControl.enabled) {
            feedback.SetError(WideToUtf8(FormatSystemError(GetLastError())));
            std::this_thread::sleep_for(std::chrono::seconds(3));
            feedback.Stop();
        }
        DeleteTemporaryCookieFile(store, temporaryCookieFile);
        DeleteTemporaryYtdlLabelScript(store, ytdlLabelScript,
                                       ytdlLabelScriptIsTemporary);
        return 1;
    }

    WriteDiagnosticLog(store, L"MPV 进程已创建，PID=" +
                                  std::to_wstring(process.dwProcessId));

    CloseHandle(process.hThread);
    std::thread ipcMonitor;
    if (webControl.enabled) {
        const std::wstring pipeName =
            L"\\\\.\\pipe\\MPVBridge-" + webControl.session;
        ipcMonitor = std::thread([&, pipeName] {
            MonitorMpvJsonIpc(pipeName, process.hProcess, feedback);
        });
    }
    DWORD exitCode = 1;
    const DWORD waitResult = WaitForSingleObject(process.hProcess, INFINITE);
    if (waitResult == WAIT_OBJECT_0) {
        if (GetExitCodeProcess(process.hProcess, &exitCode) == FALSE) {
            WriteDiagnosticLog(store, L"GetExitCodeProcess 失败：" +
                                          FormatSystemError(GetLastError()));
            ShowError(L"无法读取 MPV 的退出码：\n" +
                      FormatSystemError(GetLastError()));
        }
    } else {
        WriteDiagnosticLog(store, L"WaitForSingleObject 失败：" +
                                      FormatSystemError(GetLastError()));
        ShowError(L"等待 MPV 进程结束时发生错误：\n" +
                  FormatSystemError(GetLastError()));
    }
    if (ipcMonitor.joinable()) ipcMonitor.join();
    if (webControl.enabled) {
        feedback.SetExitCode(exitCode);
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));
        feedback.Stop();
    }
    CloseHandle(process.hProcess);
    DeleteTemporaryCookieFile(store, temporaryCookieFile);
    DeleteTemporaryYtdlLabelScript(store, ytdlLabelScript,
                                   ytdlLabelScriptIsTemporary);
    WriteDiagnosticLog(store, L"MPV 进程已结束，退出码=" +
                                  std::to_wstring(exitCode));
    return exitCode;
}
