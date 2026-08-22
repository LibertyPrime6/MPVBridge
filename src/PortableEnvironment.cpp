#include "PortableEnvironment.h"

#include "AppCore.h"

#include <array>
#include <cwctype>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;

namespace {

bool IsFile(const fs::path& path) {
    std::error_code error;
    return fs::is_regular_file(path, error);
}

fs::path SearchSystemExecutable(const wchar_t* name) {
    const DWORD pathRequired = GetEnvironmentVariableW(L"PATH", nullptr, 0);
    if (pathRequired == 0) return {};
    std::vector<wchar_t> pathBuffer(pathRequired, L'\0');
    const DWORD pathCopied = GetEnvironmentVariableW(
        L"PATH", pathBuffer.data(), static_cast<DWORD>(pathBuffer.size()));
    if (pathCopied == 0 || pathCopied >= pathBuffer.size()) return {};
    std::vector<wchar_t> buffer(32768, L'\0');
    // Supplying PATH explicitly avoids the implicit application/current-folder
    // probes performed by SearchPathW. This keeps the promised priority clear:
    // system PATH first, MPVBridge-local fallback second.
    const DWORD length = SearchPathW(pathBuffer.data(), name, nullptr,
                                     static_cast<DWORD>(buffer.size()),
                                     buffer.data(), nullptr);
    if (length == 0 || length >= buffer.size()) return {};
    return fs::path(buffer.data());
}

EnvironmentToolDetection FindFirst(
    const std::vector<std::pair<fs::path, bool>>& candidates,
    const wchar_t* searchName,
    bool (*supported)(const fs::path&) = nullptr) {
    const auto isSupported = [supported](const fs::path& path) {
        return supported == nullptr || supported(path);
    };
    bool portableInstalled = false;
    for (const auto& [path, portable] : candidates) {
        if (portable && IsFile(path) && isSupported(path)) {
            portableInstalled = true;
        }
    }
    const fs::path system = SearchSystemExecutable(searchName);
    if (!system.empty() && isSupported(system)) {
        return {true, false, portableInstalled, system};
    }
    for (const auto& [path, portable] : candidates) {
        if (IsFile(path) && isSupported(path)) {
            return {true, portable, portableInstalled, path};
        }
    }
    return {};
}

bool IsSupportedNode(const fs::path& executable) {
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    HANDLE readPipe = nullptr;
    HANDLE writePipe = nullptr;
    if (CreatePipe(&readPipe, &writePipe, &security, 0) == FALSE) return false;
    SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);
    std::wstring command = L"\"" + executable.wstring() + L"\" --version";
    std::vector<wchar_t> writable(command.begin(), command.end());
    writable.push_back(L'\0');
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = writePipe;
    startup.hStdError = writePipe;
    PROCESS_INFORMATION process{};
    const BOOL created = CreateProcessW(
        executable.c_str(), writable.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW, nullptr, executable.parent_path().c_str(), &startup,
        &process);
    CloseHandle(writePipe);
    if (created == FALSE) {
        CloseHandle(readPipe);
        return false;
    }
    CloseHandle(process.hThread);
    const DWORD wait = WaitForSingleObject(process.hProcess, 5000);
    if (wait == WAIT_TIMEOUT) {
        TerminateProcess(process.hProcess, ERROR_TIMEOUT);
        WaitForSingleObject(process.hProcess, 1000);
    }
    CloseHandle(process.hProcess);
    char buffer[128]{};
    DWORD read = 0;
    const BOOL readResult = ReadFile(readPipe, buffer, sizeof(buffer) - 1, &read,
                                     nullptr);
    CloseHandle(readPipe);
    if (wait != WAIT_OBJECT_0 || readResult == FALSE || read == 0) return false;
    size_t cursor = buffer[0] == 'v' || buffer[0] == 'V' ? 1 : 0;
    unsigned long major = 0;
    bool foundDigit = false;
    while (cursor < read && buffer[cursor] >= '0' && buffer[cursor] <= '9') {
        foundDigit = true;
        major = major * 10 + static_cast<unsigned long>(buffer[cursor] - '0');
        ++cursor;
    }
    // yt-dlp's current EJS documentation requires Node.js 22 or newer.
    return foundDigit && major >= 22;
}

void AppendPath(std::wstring& value, const fs::path& path) {
    std::error_code error;
    if (!fs::is_directory(path, error)) return;
    if (!value.empty()) value.push_back(L';');
    value.append(path.wstring());
}

} // namespace

fs::path PortableToolsDirectory() {
    return GetModulePath().parent_path() / L"Tools";
}

fs::path PortableYtdlpPath() {
    return PortableToolsDirectory() / L"yt-dlp" / L"yt-dlp.exe";
}

fs::path PortableFfmpegPath() {
    return PortableToolsDirectory() / L"ffmpeg" / L"bin" / L"ffmpeg.exe";
}

fs::path PortableFfprobePath() {
    return PortableToolsDirectory() / L"ffmpeg" / L"bin" / L"ffprobe.exe";
}

fs::path PortableNodePath() {
    return PortableToolsDirectory() / L"node" / L"node.exe";
}

EnvironmentToolDetection DetectEnvironmentTool(EnvironmentTool tool) {
    const fs::path applicationDirectory = GetModulePath().parent_path();
    switch (tool) {
    case EnvironmentTool::Ytdlp:
        return FindFirst({{PortableYtdlpPath(), true},
                          {applicationDirectory / L"yt-dlp.exe", true}},
                         L"yt-dlp.exe");
    case EnvironmentTool::Ffmpeg:
    {
        const bool toolsInstalled = IsFile(PortableFfmpegPath()) &&
                                    IsFile(PortableFfprobePath());
        const fs::path legacyFfmpeg = applicationDirectory / L"ffmpeg.exe";
        const bool legacyInstalled = IsFile(legacyFfmpeg) &&
            IsFile(applicationDirectory / L"ffprobe.exe");
        const bool portableInstalled = toolsInstalled || legacyInstalled;
        const fs::path systemFfmpeg = SearchSystemExecutable(L"ffmpeg.exe");
        const fs::path systemFfprobe = SearchSystemExecutable(L"ffprobe.exe");
        if (!systemFfmpeg.empty() && !systemFfprobe.empty()) {
            return {true, false, portableInstalled, systemFfmpeg};
        }
        if (toolsInstalled) {
            return {true, true, true, PortableFfmpegPath()};
        }
        if (legacyInstalled) {
            return {true, true, true, legacyFfmpeg};
        }
        return {};
    }
    case EnvironmentTool::Node:
        return FindFirst({{PortableNodePath(), true},
                          {applicationDirectory / L"node.exe", true}},
                         L"node.exe", IsSupportedNode);
    }
    return {};
}

std::wstring PortableChildPathValue() {
    std::wstring value;
    const DWORD required = GetEnvironmentVariableW(L"PATH", nullptr, 0);
    if (required > 0) {
        std::vector<wchar_t> buffer(required, L'\0');
        const DWORD copied = GetEnvironmentVariableW(
            L"PATH", buffer.data(), static_cast<DWORD>(buffer.size()));
        if (copied > 0 && copied < buffer.size()) {
            if (!value.empty()) value.push_back(L';');
            value.append(buffer.data(), copied);
        }
    }
    // Append rather than prepend: a supported system installation wins, while
    // the relative Tools layout remains a self-contained fallback after the
    // whole MPVBridge folder is copied to another PC.
    AppendPath(value, PortableToolsDirectory() / L"yt-dlp");
    AppendPath(value, PortableToolsDirectory() / L"ffmpeg" / L"bin");
    AppendPath(value, PortableToolsDirectory() / L"node");
    return value;
}

ScopedPortableChildPath::ScopedPortableChildPath() {
    const DWORD required = GetEnvironmentVariableW(L"PATH", nullptr, 0);
    if (required > 0) {
        std::vector<wchar_t> buffer(required, L'\0');
        const DWORD copied = GetEnvironmentVariableW(
            L"PATH", buffer.data(), static_cast<DWORD>(buffer.size()));
        if (copied > 0 && copied < buffer.size()) {
            existed_ = true;
            previous_.assign(buffer.data(), copied);
        }
    }
    const std::wstring childPath = PortableChildPathValue();
    SetEnvironmentVariableW(L"PATH", childPath.c_str());
}

ScopedPortableChildPath::~ScopedPortableChildPath() {
    SetEnvironmentVariableW(L"PATH", existed_ ? previous_.c_str() : nullptr);
}
