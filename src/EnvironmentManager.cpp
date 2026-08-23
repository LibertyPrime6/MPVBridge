#include "EnvironmentManager.h"

#include "PortableEnvironment.h"
#include "resource.h"
#include "UiCommon.h"

#include <commctrl.h>
#include <shellapi.h>
#include <winhttp.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr wchar_t kWindowClass[] = L"MPVBridge.EnvironmentManager";
constexpr UINT kWorkerMessage = WM_APP + 71;
constexpr UINT_PTR kProgressAnimationTimer = 9;
constexpr DWORD kNetworkTimeoutMilliseconds = 30000;
constexpr ULONGLONG kOverallDownloadTimeoutMilliseconds = 10ULL * 60ULL * 1000ULL;

enum ControlId : int {
    IdStatusYtdlp = 500,
    IdPathYtdlp,
    IdInstallYtdlp,
    IdStatusFfmpeg,
    IdPathFfmpeg,
    IdInstallFfmpeg,
    IdStatusNode,
    IdPathNode,
    IdInstallNode,
    IdRefresh,
    IdOpenFolder,
    IdActivity,
};

struct WorkerUpdate {
    EnvironmentTool tool{EnvironmentTool::Ytdlp};
    bool scanResult{};
    bool operationFinished{};
    bool success{};
    bool cancelled{};
    int progress{-2};
    EnvironmentToolDetection detection;
    std::wstring version;
    std::wstring text;
};

struct WindowState {
    HINSTANCE instance{};
    HWND window{};
    HWND owner{};
    UINT dpi{USER_DEFAULT_SCREEN_DPI};
    bool done{};
    bool scanBusy{};
    int pendingScans{};
    HFONT font{};
    HFONT smallFont{};
    HFONT titleFont{};
    HFONT sectionFont{};
    HBRUSH backgroundBrush{};
    HBRUSH surfaceBrush{};
    std::array<EnvironmentToolDetection, 3> detections{};
    std::array<HWND, 3> names{};
    std::array<HWND, 3> descriptions{};
    std::array<HWND, 3> progressBars{};
    std::array<bool, 3> installing{};
    std::array<bool, 3> cancelRequested{};
    std::array<std::shared_ptr<std::atomic_bool>, 3> cancellation{};
    std::array<int, 3> displayedProgress{};
    std::array<int, 3> targetProgress{};
};

int ToolIndex(EnvironmentTool tool) {
    return static_cast<int>(tool);
}

int S(const WindowState& state, int value) {
    return ui::Scale(value, state.dpi);
}

const wchar_t* ToolName(EnvironmentTool tool) {
    switch (tool) {
    case EnvironmentTool::Ytdlp: return L"yt-dlp";
    case EnvironmentTool::Ffmpeg: return L"FFmpeg";
    case EnvironmentTool::Node: return L"Node.js";
    }
    return L"工具";
}

int StatusId(EnvironmentTool tool) {
    switch (tool) {
    case EnvironmentTool::Ytdlp: return IdStatusYtdlp;
    case EnvironmentTool::Ffmpeg: return IdStatusFfmpeg;
    case EnvironmentTool::Node: return IdStatusNode;
    }
    return 0;
}

int PathId(EnvironmentTool tool) {
    switch (tool) {
    case EnvironmentTool::Ytdlp: return IdPathYtdlp;
    case EnvironmentTool::Ffmpeg: return IdPathFfmpeg;
    case EnvironmentTool::Node: return IdPathNode;
    }
    return 0;
}

int InstallId(EnvironmentTool tool) {
    switch (tool) {
    case EnvironmentTool::Ytdlp: return IdInstallYtdlp;
    case EnvironmentTool::Ffmpeg: return IdInstallFfmpeg;
    case EnvironmentTool::Node: return IdInstallNode;
    }
    return 0;
}

EnvironmentTool ToolFromInstallId(int id) {
    if (id == IdInstallFfmpeg) return EnvironmentTool::Ffmpeg;
    if (id == IdInstallNode) return EnvironmentTool::Node;
    return EnvironmentTool::Ytdlp;
}

void SendUpdate(HWND window, std::unique_ptr<WorkerUpdate> update) {
    WorkerUpdate* raw = update.release();
    if (PostMessageW(window, kWorkerMessage, 0,
                     reinterpret_cast<LPARAM>(raw)) == FALSE) {
        delete raw;
    }
}

std::wstring FirstLine(std::wstring value) {
    const size_t end = value.find_first_of(L"\r\n");
    if (end != std::wstring::npos) value.resize(end);
    if (value.size() > 110) value.resize(110);
    return Trim(std::move(value));
}

std::wstring Utf8ToWide(std::string_view value) {
    if (value.empty()) return {};
    const int required = MultiByteToWideChar(
        CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (required <= 0) return {};
    std::wstring result(static_cast<size_t>(required), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                        result.data(), required);
    return result;
}

std::wstring QuoteCommandArgument(std::wstring_view value) {
    std::wstring result = L"\"";
    size_t backslashes = 0;
    for (const wchar_t ch : value) {
        if (ch == L'\\') {
            ++backslashes;
        } else if (ch == L'\"') {
            result.append(backslashes * 2 + 1, L'\\');
            result.push_back(L'\"');
            backslashes = 0;
        } else {
            result.append(backslashes, L'\\');
            backslashes = 0;
            result.push_back(ch);
        }
    }
    result.append(backslashes * 2, L'\\');
    result.push_back(L'\"');
    return result;
}

std::wstring QueryVersion(EnvironmentTool tool, const fs::path& executable) {
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    HANDLE readPipe = nullptr;
    HANDLE writePipe = nullptr;
    if (CreatePipe(&readPipe, &writePipe, &security, 0) == FALSE) return {};
    SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

    std::wstring command = QuoteCommandArgument(executable.wstring());
    command.append(tool == EnvironmentTool::Ffmpeg ? L" -version" : L" --version");
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
        return {};
    }
    CloseHandle(process.hThread);
    const DWORD wait = WaitForSingleObject(process.hProcess, 8000);
    if (wait == WAIT_TIMEOUT) {
        TerminateProcess(process.hProcess, ERROR_TIMEOUT);
        WaitForSingleObject(process.hProcess, 1000);
    }
    CloseHandle(process.hProcess);
    std::string output;
    char buffer[2048];
    for (;;) {
        DWORD read = 0;
        if (ReadFile(readPipe, buffer, sizeof(buffer), &read, nullptr) == FALSE ||
            read == 0) {
            break;
        }
        output.append(buffer, read);
        if (output.size() >= 8192) break;
    }
    CloseHandle(readPipe);
    return FirstLine(Utf8ToWide(output));
}

struct InternetHandle {
    HINTERNET value{};
    ~InternetHandle() {
        if (value != nullptr) WinHttpCloseHandle(value);
    }
    InternetHandle() = default;
    InternetHandle(const InternetHandle&) = delete;
    InternetHandle& operator=(const InternetHandle&) = delete;
};

bool DownloadLatest(HWND window, EnvironmentTool tool, std::wstring_view url,
                    const fs::path& destination,
                    const std::shared_ptr<std::atomic_bool>& cancellation,
                    std::wstring& error) {
    const auto cancelled = [&] {
        return cancellation != nullptr && cancellation->load();
    };
    if (cancelled()) {
        error = L"用户已取消下载。";
        return false;
    }
    URL_COMPONENTSW parts{};
    parts.dwStructSize = sizeof(parts);
    parts.dwSchemeLength = static_cast<DWORD>(-1);
    parts.dwHostNameLength = static_cast<DWORD>(-1);
    parts.dwUrlPathLength = static_cast<DWORD>(-1);
    parts.dwExtraInfoLength = static_cast<DWORD>(-1);
    std::wstring urlCopy(url);
    if (WinHttpCrackUrl(urlCopy.c_str(), 0, 0, &parts) == FALSE) {
        error = L"无法解析最新下载地址：" + FormatSystemError(GetLastError());
        return false;
    }
    const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
    std::wstring object(parts.lpszUrlPath, parts.dwUrlPathLength);
    if (parts.dwExtraInfoLength > 0) {
        object.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    }

    InternetHandle session;
    session.value = WinHttpOpen(L"MPVBridge/1.4.2 Environment Installer",
                                WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                WINHTTP_NO_PROXY_NAME,
                                WINHTTP_NO_PROXY_BYPASS, 0);
    if (session.value == nullptr) {
        error = L"无法初始化下载服务：" + FormatSystemError(GetLastError());
        return false;
    }
    WinHttpSetTimeouts(session.value, kNetworkTimeoutMilliseconds,
                       kNetworkTimeoutMilliseconds, kNetworkTimeoutMilliseconds,
                       kNetworkTimeoutMilliseconds);
    InternetHandle connection;
    connection.value = WinHttpConnect(session.value, host.c_str(), parts.nPort, 0);
    if (connection.value == nullptr) {
        error = L"无法连接下载服务器：" + FormatSystemError(GetLastError());
        return false;
    }
    if (cancelled()) {
        error = L"用户已取消下载。";
        return false;
    }
    InternetHandle request;
    const DWORD flags = parts.nScheme == INTERNET_SCHEME_HTTPS
                            ? WINHTTP_FLAG_SECURE
                            : 0;
    request.value = WinHttpOpenRequest(
        connection.value, L"GET", object.c_str(), nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (request.value == nullptr ||
        WinHttpSendRequest(request.value, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                           WINHTTP_NO_REQUEST_DATA, 0, 0, 0) == FALSE ||
        WinHttpReceiveResponse(request.value, nullptr) == FALSE) {
        const DWORD code = GetLastError();
        error = code == ERROR_WINHTTP_TIMEOUT
                    ? L"下载连接超时，请检查网络后重试。"
                    : L"下载请求失败：" + FormatSystemError(code);
        return false;
    }
    if (cancelled()) {
        error = L"用户已取消下载。";
        return false;
    }
    DWORD status = 0;
    DWORD statusSize = sizeof(status);
    WinHttpQueryHeaders(request.value,
                        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize,
                        WINHTTP_NO_HEADER_INDEX);
    if (status < 200 || status >= 300) {
        error = L"下载服务器返回 HTTP " + std::to_wstring(status) + L"。";
        return false;
    }
    ULONGLONG contentLength = 0;
    wchar_t lengthText[64]{};
    DWORD lengthSize = sizeof(lengthText);
    if (WinHttpQueryHeaders(request.value, WINHTTP_QUERY_CONTENT_LENGTH,
                            WINHTTP_HEADER_NAME_BY_INDEX, lengthText, &lengthSize,
                            WINHTTP_NO_HEADER_INDEX) != FALSE) {
        contentLength = _wcstoui64(lengthText, nullptr, 10);
    }
    {
        auto update = std::make_unique<WorkerUpdate>();
        update->tool = tool;
        update->progress = 4;
        update->text = L"已连接，正在下载最新版本…";
        SendUpdate(window, std::move(update));
    }

    HANDLE file = CreateFileW(destination.c_str(), GENERIC_WRITE, 0, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        error = L"无法创建下载文件：" + FormatSystemError(GetLastError());
        return false;
    }
    const ULONGLONG started = GetTickCount64();
    ULONGLONG downloaded = 0;
    int lastPercent = -1;
    bool succeeded = true;
    std::array<unsigned char, 64 * 1024> buffer{};
    for (;;) {
        if (cancelled()) {
            error = L"用户已取消下载。";
            succeeded = false;
            break;
        }
        if (GetTickCount64() - started > kOverallDownloadTimeoutMilliseconds) {
            error = L"下载总时长超过 10 分钟，已停止。请检查网络后重试。";
            succeeded = false;
            break;
        }
        DWORD read = 0;
        if (WinHttpReadData(request.value, buffer.data(),
                            static_cast<DWORD>(buffer.size()), &read) == FALSE) {
            const DWORD code = GetLastError();
            error = code == ERROR_WINHTTP_TIMEOUT
                        ? L"下载超过 30 秒没有收到数据，已超时。"
                        : L"下载中断：" + FormatSystemError(code);
            succeeded = false;
            break;
        }
        if (read == 0) break;
        if (cancelled()) {
            error = L"用户已取消下载。";
            succeeded = false;
            break;
        }
        DWORD written = 0;
        if (WriteFile(file, buffer.data(), read, &written, nullptr) == FALSE ||
            written != read) {
            error = L"写入下载文件失败：" + FormatSystemError(GetLastError());
            succeeded = false;
            break;
        }
        downloaded += read;
        const int percent = contentLength > 0
                                ? static_cast<int>(std::min<ULONGLONG>(
                                      100, downloaded * 100 / contentLength))
                                : static_cast<int>(downloaded / (1024 * 1024));
        if (percent != lastPercent) {
            lastPercent = percent;
            auto update = std::make_unique<WorkerUpdate>();
            update->tool = tool;
            update->progress = contentLength > 0
                                   ? percent
                                   : static_cast<int>(std::min<ULONGLONG>(
                                         90, 5 + downloaded / (1024 * 1024)));
            update->text = contentLength > 0
                               ? L"正在下载最新版本… " +
                                     std::to_wstring(percent) + L"%"
                               : L"正在下载最新版本… " +
                                     std::to_wstring(downloaded / (1024 * 1024)) +
                                     L" MB";
            SendUpdate(window, std::move(update));
        }
    }
    CloseHandle(file);
    if (!succeeded || downloaded == 0) {
        DeleteFileW(destination.c_str());
        if (error.empty()) error = L"下载内容为空。";
        return false;
    }
    auto complete = std::make_unique<WorkerUpdate>();
    complete->tool = tool;
    complete->progress = 100;
    complete->text = L"下载完成，正在配置…";
    SendUpdate(window, std::move(complete));
    return true;
}

std::wstring PowerShellLiteral(std::wstring value) {
    size_t cursor = 0;
    while ((cursor = value.find(L'\'', cursor)) != std::wstring::npos) {
        value.insert(cursor, 1, L'\'');
        cursor += 2;
    }
    return L"'" + value + L"'";
}

bool ExpandZip(const fs::path& archive, const fs::path& destination,
               const std::shared_ptr<std::atomic_bool>& cancellation,
               bool& cancelled, std::wstring& error) {
    const std::wstring script =
        L"& { $ErrorActionPreference='Stop'; Expand-Archive -LiteralPath " +
        PowerShellLiteral(archive.wstring()) + L" -DestinationPath " +
        PowerShellLiteral(destination.wstring()) + L" -Force }";
    std::wstring command = L"powershell.exe -NoLogo -NoProfile -NonInteractive "
                           L"-ExecutionPolicy Bypass -Command " +
                           QuoteCommandArgument(script);
    std::vector<wchar_t> writable(command.begin(), command.end());
    writable.push_back(L'\0');
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION process{};
    if (CreateProcessW(nullptr, writable.data(), nullptr, nullptr, FALSE,
                       CREATE_NO_WINDOW, nullptr, nullptr, &startup,
                       &process) == FALSE) {
        error = L"无法启动 Windows 解压组件：" + FormatSystemError(GetLastError());
        return false;
    }
    CloseHandle(process.hThread);
    const ULONGLONG started = GetTickCount64();
    DWORD wait = WAIT_TIMEOUT;
    for (;;) {
        wait = WaitForSingleObject(process.hProcess, 200);
        if (wait == WAIT_OBJECT_0) break;
        if (cancellation != nullptr && cancellation->load()) {
            cancelled = true;
            TerminateProcess(process.hProcess, ERROR_CANCELLED);
            WaitForSingleObject(process.hProcess, 2000);
            error = L"用户已取消下载和解压。";
            CloseHandle(process.hProcess);
            return false;
        }
        if (GetTickCount64() - started > 5ULL * 60ULL * 1000ULL) {
            TerminateProcess(process.hProcess, ERROR_TIMEOUT);
            WaitForSingleObject(process.hProcess, 2000);
            error = L"FFmpeg 解压超过 5 分钟，已超时。";
            CloseHandle(process.hProcess);
            return false;
        }
    }
    DWORD exitCode = 1;
    GetExitCodeProcess(process.hProcess, &exitCode);
    CloseHandle(process.hProcess);
    if (exitCode != 0) {
        error = L"Windows 解压组件返回错误码 " +
                std::to_wstring(exitCode) + L"。";
        return false;
    }
    return true;
}

fs::path FindExtractedFile(const fs::path& root, const wchar_t* name) {
    std::error_code error;
    for (fs::recursive_directory_iterator iterator(
             root, fs::directory_options::skip_permission_denied, error), end;
         iterator != end; iterator.increment(error)) {
        if (error) {
            error.clear();
            continue;
        }
        if (iterator->is_regular_file(error) &&
            EqualsInsensitive(iterator->path().filename().wstring(), name)) {
            return iterator->path();
        }
    }
    return {};
}

bool InstallTool(HWND window, EnvironmentTool tool,
                 const std::shared_ptr<std::atomic_bool>& cancellation,
                 bool& cancelled, std::wstring& error) {
    const auto cancellationRequested = [&] {
        return cancellation != nullptr && cancellation->load();
    };
    const fs::path tools = PortableToolsDirectory();
    const fs::path work = tools / L".downloads" /
        (std::to_wstring(GetCurrentProcessId()) + L"-" +
         std::to_wstring(ToolIndex(tool)) + L"-" +
         std::to_wstring(GetTickCount64()));
    std::error_code fileError;
    fs::create_directories(work, fileError);
    if (fileError) {
        error = L"无法创建便携工具目录：" +
                Utf8ToWide(fileError.message());
        return false;
    }

    std::wstring url;
    fs::path downloaded;
    fs::path target;
    if (tool == EnvironmentTool::Ytdlp) {
        url = L"https://github.com/yt-dlp/yt-dlp/releases/latest/download/yt-dlp.exe";
        downloaded = work / L"yt-dlp.exe";
        target = PortableYtdlpPath();
    } else if (tool == EnvironmentTool::Node) {
        url = L"https://nodejs.org/dist/latest/win-x64/node.exe";
        downloaded = work / L"node.exe";
        target = PortableNodePath();
    } else {
        url = L"https://github.com/BtbN/FFmpeg-Builds/releases/download/latest/ffmpeg-master-latest-win64-gpl.zip";
        downloaded = work / L"ffmpeg.zip";
        target = PortableFfmpegPath();
    }

    auto resolving = std::make_unique<WorkerUpdate>();
    resolving->tool = tool;
    resolving->progress = 2;
    resolving->text = L"正在解析上游最新版本下载地址…";
    SendUpdate(window, std::move(resolving));
    if (!DownloadLatest(window, tool, url, downloaded, cancellation, error)) {
        cancelled = cancellationRequested();
        fs::remove_all(work, fileError);
        return false;
    }
    if (cancellationRequested()) {
        cancelled = true;
        error = L"用户已取消下载。";
        fs::remove_all(work, fileError);
        return false;
    }

    auto installing = std::make_unique<WorkerUpdate>();
    installing->tool = tool;
    installing->text = tool == EnvironmentTool::Ffmpeg
                           ? L"下载完成，正在解压并配置 FFmpeg…"
                           : L"下载完成，正在写入便携工具目录…";
    SendUpdate(window, std::move(installing));

    fs::create_directories(target.parent_path(), fileError);
    if (fileError) {
        error = L"无法创建安装目录：" + Utf8ToWide(fileError.message());
        fs::remove_all(work, fileError);
        return false;
    }
    if (tool == EnvironmentTool::Ffmpeg) {
        const fs::path extracted = work / L"expanded";
        fs::create_directories(extracted, fileError);
        if (fileError || !ExpandZip(downloaded, extracted, cancellation,
                                    cancelled, error)) {
            fs::remove_all(work, fileError);
            return false;
        }
        if (cancellationRequested()) {
            cancelled = true;
            error = L"用户已取消下载。";
            fs::remove_all(work, fileError);
            return false;
        }
        const fs::path ffmpeg = FindExtractedFile(extracted, L"ffmpeg.exe");
        const fs::path ffprobe = FindExtractedFile(extracted, L"ffprobe.exe");
        if (ffmpeg.empty() || ffprobe.empty()) {
            error = L"下载包中没有找到 ffmpeg.exe 或 ffprobe.exe。";
            fs::remove_all(work, fileError);
            return false;
        }
        fs::copy_file(ffmpeg, PortableFfmpegPath(),
                      fs::copy_options::overwrite_existing, fileError);
        if (!fileError) {
            fs::copy_file(ffprobe, PortableFfprobePath(),
                          fs::copy_options::overwrite_existing, fileError);
        }
    } else {
        if (cancellationRequested()) {
            cancelled = true;
            error = L"用户已取消下载。";
            fs::remove_all(work, fileError);
            return false;
        }
        fs::copy_file(downloaded, target, fs::copy_options::overwrite_existing,
                      fileError);
    }
    if (fileError) {
        error = L"安装文件失败：" + Utf8ToWide(fileError.message());
        std::error_code cleanupError;
        fs::remove_all(work, cleanupError);
        return false;
    }
    std::error_code cleanupError;
    fs::remove_all(work, cleanupError);
    const EnvironmentToolDetection detection = DetectEnvironmentTool(tool);
    const fs::path installedPath = tool == EnvironmentTool::Ytdlp
                                       ? PortableYtdlpPath()
                                   : tool == EnvironmentTool::Ffmpeg
                                       ? PortableFfmpegPath()
                                       : PortableNodePath();
    if (!detection.portableInstalled ||
        QueryVersion(tool, installedPath).empty()) {
        error = L"文件已下载，但版本验证失败；请重试或检查安全软件拦截。";
        return false;
    }
    return true;
}

bool AnyInstallRunning(const WindowState& state) {
    return std::any_of(state.installing.begin(), state.installing.end(),
                       [](bool installing) { return installing; });
}

void UpdateActionControls(WindowState& state) {
    EnableWindow(GetDlgItem(state.window, IdRefresh),
                 !state.scanBusy && !AnyInstallRunning(state) ? TRUE : FALSE);
    for (EnvironmentTool tool : {EnvironmentTool::Ytdlp,
                                 EnvironmentTool::Ffmpeg,
                                 EnvironmentTool::Node}) {
        const int index = ToolIndex(tool);
        const bool portable = state.detections[index].portableInstalled;
        HWND button = GetDlgItem(state.window, InstallId(tool));
        if (state.installing[index]) {
            ui::SetWindowTextString(
                button, state.cancelRequested[index] ? L"正在取消…"
                                                     : L"取消下载");
            EnableWindow(button, state.cancelRequested[index] ? FALSE : TRUE);
        } else {
            ui::SetWindowTextString(button,
                                    portable ? L"便携版已安装" : L"安装便携版");
            EnableWindow(button, !state.scanBusy && !portable ? TRUE : FALSE);
        }
    }
}

void StartScan(WindowState& state) {
    state.scanBusy = true;
    state.pendingScans = 3;
    UpdateActionControls(state);
    ui::SetWindowTextString(GetDlgItem(state.window, IdActivity),
                            L"正在检测三个运行环境…");
    for (EnvironmentTool tool : {EnvironmentTool::Ytdlp,
                                 EnvironmentTool::Ffmpeg,
                                 EnvironmentTool::Node}) {
        ui::SetWindowTextString(GetDlgItem(state.window, StatusId(tool)),
                                L"正在检测…");
        ui::SetWindowTextString(GetDlgItem(state.window, PathId(tool)), L"");
        const HWND window = state.window;
        std::thread([window, tool] {
            auto update = std::make_unique<WorkerUpdate>();
            update->tool = tool;
            update->scanResult = true;
            update->detection = DetectEnvironmentTool(tool);
            if (update->detection.available) {
                update->version = QueryVersion(tool, update->detection.path);
                if (update->version.empty()) update->detection.available = false;
            }
            SendUpdate(window, std::move(update));
        }).detach();
    }
}

void StartInstall(WindowState& state, EnvironmentTool tool) {
    const int index = ToolIndex(tool);
    state.installing[index] = true;
    state.cancelRequested[index] = false;
    state.cancellation[index] = std::make_shared<std::atomic_bool>(false);
    ShowWindow(state.progressBars[index], SW_SHOW);
    SendMessageW(state.progressBars[index], PBM_SETPOS, 0, 0);
    state.displayedProgress[index] = 0;
    state.targetProgress[index] = 1;
    SetTimer(state.window, kProgressAnimationTimer, 35, nullptr);
    UpdateActionControls(state);
    ui::SetWindowTextString(GetDlgItem(state.window, IdActivity),
                            std::wstring(L"正在安装 ") + ToolName(tool) + L"…");
    const HWND window = state.window;
    const std::shared_ptr<std::atomic_bool> cancellation =
        state.cancellation[index];
    std::thread([window, tool, cancellation] {
        std::wstring error;
        bool cancelled = false;
        const bool success = InstallTool(window, tool, cancellation, cancelled,
                                         error);
        auto update = std::make_unique<WorkerUpdate>();
        update->tool = tool;
        update->operationFinished = true;
        update->success = success;
        update->cancelled = cancelled;
        update->text = success ? L"安装与配置完成。" : std::move(error);
        if (success) {
            update->detection = DetectEnvironmentTool(tool);
            if (update->detection.available) {
                update->version = QueryVersion(tool, update->detection.path);
            }
        }
        SendUpdate(window, std::move(update));
    }).detach();
}

void CancelInstall(WindowState& state, EnvironmentTool tool) {
    const int index = ToolIndex(tool);
    if (!state.installing[index] || state.cancelRequested[index]) return;
    state.cancelRequested[index] = true;
    if (state.cancellation[index] != nullptr) {
        state.cancellation[index]->store(true);
    }
    state.displayedProgress[index] = 0;
    state.targetProgress[index] = 0;
    SendMessageW(state.progressBars[index], PBM_SETPOS, 0, 0);
    ShowWindow(state.progressBars[index], SW_HIDE);
    ui::SetWindowTextString(GetDlgItem(state.window, StatusId(tool)),
                            L"正在取消下载并清理临时文件…");
    ui::SetWindowTextString(GetDlgItem(state.window, IdActivity),
                            std::wstring(ToolName(tool)) + L" 正在取消…");
    UpdateActionControls(state);
}

HWND CreateControl(WindowState& state, const wchar_t* type,
                   const wchar_t* text, DWORD style, int id) {
    HWND control = CreateWindowExW(
        0, type, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0,
        state.window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
        state.instance, nullptr);
    ui::SetFont(control, state.font);
    ui::ApplyControlTheme(control);
    return control;
}

void CreateControls(WindowState& state) {
    state.font = ui::CreateFont(state.dpi, 10);
    state.smallFont = ui::CreateFont(state.dpi, 9);
    state.titleFont = ui::CreateFont(state.dpi, 20, FW_SEMIBOLD);
    state.sectionFont = ui::CreateFont(state.dpi, 12, FW_SEMIBOLD);
    const struct Row {
        EnvironmentTool tool;
        const wchar_t* description;
    } rows[] = {
        {EnvironmentTool::Ytdlp, L"解析网页媒体地址与 YouTube 视频信息"},
        {EnvironmentTool::Ffmpeg, L"合并音视频轨道并完成格式处理（含 ffprobe）"},
        {EnvironmentTool::Node, L"通过 yt-dlp --js-runtimes 显式执行 YouTube JavaScript"},
    };
    for (const Row& row : rows) {
        const int index = ToolIndex(row.tool);
        state.names[index] = CreateControl(
            state, L"STATIC", ToolName(row.tool), SS_LEFT, 0);
        state.descriptions[index] = CreateControl(
            state, L"STATIC", row.description, SS_LEFT, 0);
        ui::SetFont(state.names[index], state.sectionFont);
        ui::SetFont(state.descriptions[index], state.smallFont);
        CreateControl(state, L"STATIC", L"等待检测", SS_LEFT,
                      StatusId(row.tool));
        CreateControl(state, L"STATIC", L"", SS_LEFT | SS_PATHELLIPSIS,
                      PathId(row.tool));
        HWND button = CreateControl(state, L"BUTTON", L"安装便携版",
                                    WS_TABSTOP, InstallId(row.tool));
        ui::StyleButton(button, ui::ButtonStyle::Primary);
        state.progressBars[index] = CreateWindowExW(
            0, PROGRESS_CLASSW, L"", WS_CHILD | PBS_SMOOTH,
            0, 0, 0, 0, state.window, nullptr, state.instance, nullptr);
        ui::ApplyControlTheme(state.progressBars[index]);
        SendMessageW(state.progressBars[index], PBM_SETRANGE32, 0, 100);
        SendMessageW(state.progressBars[index], PBM_SETBARCOLOR, 0,
                     ui::kAccent);
    }
    HWND refresh = CreateControl(state, L"BUTTON", L"重新检测", WS_TABSTOP,
                                 IdRefresh);
    HWND folder = CreateControl(state, L"BUTTON", L"打开工具目录", WS_TABSTOP,
                                IdOpenFolder);
    CreateControl(state, L"STATIC", L"", SS_LEFT, IdActivity);
    ui::StyleButton(refresh, ui::ButtonStyle::Primary, ui::kBackground);
    ui::StyleButton(folder, ui::ButtonStyle::Secondary, ui::kBackground);
}

void Layout(WindowState& state) {
    RECT client{};
    GetClientRect(state.window, &client);
    const int margin = S(state, 26);
    const int rowTop[] = {S(state, 105), S(state, 215), S(state, 325)};
    const EnvironmentTool tools[] = {EnvironmentTool::Ytdlp,
                                     EnvironmentTool::Ffmpeg,
                                     EnvironmentTool::Node};
    for (int index = 0; index < 3; ++index) {
        const int y = rowTop[index];
        SetWindowPos(state.names[index], nullptr, margin + S(state, 18),
                     y + S(state, 13), S(state, 120), S(state, 24), SWP_NOZORDER);
        SetWindowPos(state.descriptions[index], nullptr,
                     margin + S(state, 138), y + S(state, 13), S(state, 400),
                     S(state, 24), SWP_NOZORDER);
        SetWindowPos(GetDlgItem(state.window, StatusId(tools[index])), nullptr,
                     margin + S(state, 18), y + S(state, 49), S(state, 250),
                     S(state, 22), SWP_NOZORDER);
        SetWindowPos(GetDlgItem(state.window, PathId(tools[index])), nullptr,
                     margin + S(state, 275), y + S(state, 49),
                     client.right - margin * 2 - S(state, 430), S(state, 22),
                     SWP_NOZORDER);
        SetWindowPos(GetDlgItem(state.window, InstallId(tools[index])), nullptr,
                     client.right - margin - S(state, 132), y + S(state, 31),
                     S(state, 112), S(state, 34), SWP_NOZORDER);
        SetWindowPos(state.progressBars[index], nullptr, margin + S(state, 18),
                     y + S(state, 78),
                     client.right - margin * 2 - S(state, 36), S(state, 5),
                     SWP_NOZORDER);
    }
    SetWindowPos(GetDlgItem(state.window, IdRefresh), nullptr, margin,
                 client.bottom - S(state, 62), S(state, 104), S(state, 34),
                 SWP_NOZORDER);
    SetWindowPos(GetDlgItem(state.window, IdOpenFolder), nullptr,
                 margin + S(state, 116), client.bottom - S(state, 62),
                 S(state, 128), S(state, 34), SWP_NOZORDER);
    SetWindowPos(GetDlgItem(state.window, IdActivity), nullptr,
                 margin + S(state, 262), client.bottom - S(state, 60),
                 client.right - margin * 2 - S(state, 262), S(state, 30),
                 SWP_NOZORDER);
}

void Paint(WindowState& state) {
    PAINTSTRUCT paint{};
    HDC dc = BeginPaint(state.window, &paint);
    RECT client{};
    GetClientRect(state.window, &client);
    ui::FillSolid(dc, client, ui::kBackground);
    RECT header{0, 0, client.right, S(state, 82)};
    ui::FillSolid(dc, header, ui::kHeader);
    RECT accent{0, S(state, 78), client.right, S(state, 82)};
    ui::FillSolid(dc, accent, ui::kAccent);
    RECT title{S(state, 28), S(state, 10), client.right - S(state, 24), S(state, 45)};
    ui::DrawTextLine(dc, L"运行环境检测", title, state.titleFont,
                     RGB(255, 255, 255), DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    RECT subtitle{S(state, 30), S(state, 42), client.right - S(state, 24),
                  S(state, 70)};
    ui::DrawTextLine(dc, L"检测、下载并配置可随 MPVBridge 一起移动的工具",
                     subtitle, state.smallFont, RGB(190, 202, 222),
                     DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    const int margin = S(state, 26);
    for (int top : {S(state, 105), S(state, 215), S(state, 325)}) {
        RECT card{margin, top, client.right - margin, top + S(state, 88)};
        ui::FillRounded(dc, card, S(state, 12), ui::kSurface, ui::kBorder);
    }
    EndPaint(state.window, &paint);
}

void ApplyDetection(WindowState& state, const WorkerUpdate& update,
                    bool partOfFullScan = true) {
    const EnvironmentTool tool = update.tool;
    state.detections[ToolIndex(tool)] = update.detection;
    std::wstring status;
    if (!update.detection.available) {
        status = L"✕ 未检测到可用环境";
    } else {
        status = update.detection.portable ? L"✓ 便携环境可用" : L"✓ 系统环境可用";
        if (!update.detection.portable && update.detection.portableInstalled) {
            status.append(L"（便携副本已就绪）");
        }
        if (!update.version.empty()) status.append(L" · " + update.version);
    }
    ui::SetWindowTextString(GetDlgItem(state.window, StatusId(tool)), status);
    ui::SetWindowTextString(
        GetDlgItem(state.window, PathId(tool)),
        update.detection.available ? update.detection.path.wstring() : L"");
    if (partOfFullScan) --state.pendingScans;
    if (partOfFullScan && state.pendingScans == 0) {
        state.scanBusy = false;
        UpdateActionControls(state);
        ui::SetWindowTextString(GetDlgItem(state.window, IdActivity),
                                L"检测完成。系统环境优先；Tools 便携环境用于回退。" );
    }
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam,
                            LPARAM lParam) {
    auto* state = reinterpret_cast<WindowState*>(
        GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        state = static_cast<WindowState*>(create->lpCreateParams);
        state->window = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }
    if (state == nullptr) return DefWindowProcW(window, message, wParam, lParam);

    switch (message) {
    case WM_CREATE:
        state->dpi = GetDpiForWindow(window);
        state->backgroundBrush = CreateSolidBrush(ui::kBackground);
        state->surfaceBrush = CreateSolidBrush(ui::kSurface);
        ui::ApplyModernWindowFrame(window);
        CreateControls(*state);
        StartScan(*state);
        return 0;
    case WM_SIZE:
        Layout(*state);
        return 0;
    case WM_COMMAND: {
        const int id = LOWORD(wParam);
        if (id == IdRefresh && !state->scanBusy &&
            !AnyInstallRunning(*state)) {
            StartScan(*state);
            return 0;
        }
        if (id == IdOpenFolder) {
            std::error_code error;
            fs::create_directories(PortableToolsDirectory(), error);
            ShellExecuteW(window, L"open", PortableToolsDirectory().c_str(),
                          nullptr, nullptr, SW_SHOWNORMAL);
            return 0;
        }
        if (id == IdInstallYtdlp || id == IdInstallFfmpeg ||
            id == IdInstallNode) {
            const EnvironmentTool tool = ToolFromInstallId(id);
            const int index = ToolIndex(tool);
            if (state->installing[index]) {
                CancelInstall(*state, tool);
            } else if (!state->scanBusy &&
                       !state->detections[index].portableInstalled) {
                StartInstall(*state, tool);
            }
            return 0;
        }
        break;
    }
    case kWorkerMessage: {
        std::unique_ptr<WorkerUpdate> update(
            reinterpret_cast<WorkerUpdate*>(lParam));
        if (update->scanResult) {
            ApplyDetection(*state, *update);
        } else if (update->operationFinished) {
            const int index = ToolIndex(update->tool);
            state->installing[index] = false;
            state->cancelRequested[index] = false;
            state->cancellation[index].reset();
            if (update->cancelled) {
                SendMessageW(state->progressBars[index], PBM_SETPOS, 0, 0);
                state->displayedProgress[index] = 0;
                state->targetProgress[index] = 0;
                ShowWindow(state->progressBars[index], SW_HIDE);
                ui::SetWindowTextString(
                    GetDlgItem(window, StatusId(update->tool)),
                    L"已取消下载，临时文件已清理。" );
                ui::SetWindowTextString(
                    GetDlgItem(window, IdActivity),
                    std::wstring(ToolName(update->tool)) + L" 下载已取消。" );
            } else if (!update->success) {
                SendMessageW(state->progressBars[index], PBM_SETPOS, 0, 0);
                state->displayedProgress[index] = 0;
                state->targetProgress[index] = 0;
                ui::SetWindowTextString(GetDlgItem(window, IdActivity),
                                        L"安装失败：" + update->text);
                ShowError(std::wstring(ToolName(update->tool)) +
                              L" 安装失败：\n\n" + update->text,
                          window);
            } else {
                SendMessageW(state->progressBars[index], PBM_SETPOS, 0, 0);
                state->displayedProgress[index] = 0;
                state->targetProgress[index] = 0;
                ShowWindow(state->progressBars[index], SW_HIDE);
                ApplyDetection(*state, *update, false);
                ui::SetWindowTextString(GetDlgItem(window, IdActivity),
                                        std::wstring(ToolName(update->tool)) +
                                            L" 已安装并配置完成。" );
            }
            UpdateActionControls(*state);
        } else {
            const int index = ToolIndex(update->tool);
            if (update->progress != -2) {
                state->targetProgress[index] = std::max(
                    state->targetProgress[index],
                    std::clamp(update->progress, 0, 100));
                SetTimer(window, kProgressAnimationTimer, 35, nullptr);
            }
            ui::SetWindowTextString(GetDlgItem(window, StatusId(update->tool)),
                                    update->text);
            ui::SetWindowTextString(GetDlgItem(window, IdActivity), update->text);
        }
        return 0;
    }
    case WM_TIMER:
        if (wParam == kProgressAnimationTimer) {
            bool animationNeeded = false;
            for (int index = 0; index < 3; ++index) {
                if (state->displayedProgress[index] <
                    state->targetProgress[index]) {
                    const int difference = state->targetProgress[index] -
                                           state->displayedProgress[index];
                    // Keep the animation smooth without allowing it to drift
                    // far behind fast downloads. Large jumps catch up within a
                    // few frames; small changes still advance one percent at a
                    // time.
                    const int step = std::max(1, (difference + 3) / 4);
                    state->displayedProgress[index] = std::min(
                        state->targetProgress[index],
                        state->displayedProgress[index] + step);
                    SendMessageW(state->progressBars[index], PBM_SETPOS,
                                 state->displayedProgress[index], 0);
                } else if (state->displayedProgress[index] >
                           state->targetProgress[index]) {
                    state->displayedProgress[index] =
                        state->targetProgress[index];
                    SendMessageW(state->progressBars[index], PBM_SETPOS,
                                 state->displayedProgress[index], 0);
                }
                if (state->displayedProgress[index] !=
                    state->targetProgress[index]) {
                    animationNeeded = true;
                }
            }
            if (!animationNeeded && !AnyInstallRunning(*state)) {
                KillTimer(window, kProgressAnimationTimer);
            }
            return 0;
        }
        break;
    case WM_CTLCOLORSTATIC: {
        HDC dc = reinterpret_cast<HDC>(wParam);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, ui::kText);
        return reinterpret_cast<LRESULT>(
            GetDlgCtrlID(reinterpret_cast<HWND>(lParam)) == IdActivity
                ? state->backgroundBrush
                : state->surfaceBrush);
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        Paint(*state);
        return 0;
    case WM_CLOSE:
        if (state->scanBusy || AnyInstallRunning(*state)) {
            ShowInfo(L"环境检测或安装仍在进行，请等待完成后再关闭。", window);
        } else {
            DestroyWindow(window);
        }
        return 0;
    case WM_DESTROY:
        KillTimer(window, kProgressAnimationTimer);
        state->done = true;
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

bool RegisterWindowClass(HINSTANCE instance) {
    WNDCLASSEXW windowClass{sizeof(windowClass)};
    windowClass.style = CS_HREDRAW | CS_VREDRAW;
    windowClass.lpfnWndProc = WindowProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_APP_ICON));
    windowClass.hIconSm = windowClass.hIcon;
    windowClass.hbrBackground = nullptr;
    windowClass.lpszClassName = kWindowClass;
    return RegisterClassExW(&windowClass) != 0 ||
           GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

} // namespace

int RunEnvironmentManagerDialog(HINSTANCE instance, HWND owner) {
    if (!RegisterWindowClass(instance)) {
        ShowError(L"无法创建运行环境检测窗口。", owner);
        return 1;
    }
    WindowState state{};
    state.instance = instance;
    state.owner = owner;
    const UINT dpi = owner != nullptr ? GetDpiForWindow(owner) : GetDpiForSystem();
    HWND window = CreateWindowExW(
        WS_EX_CONTROLPARENT, kWindowClass, L"MPVBridge · 运行环境检测",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT, ui::Scale(800, dpi), ui::Scale(520, dpi),
        owner, nullptr, instance, &state);
    if (window == nullptr) {
        ShowError(L"无法创建运行环境检测窗口：\n" +
                      FormatSystemError(GetLastError()),
                  owner);
        return 1;
    }
    if (owner != nullptr) EnableWindow(owner, FALSE);
    ui::CenterWindow(window, owner);
    ShowWindow(window, SW_SHOW);
    UpdateWindow(window);
    MSG message{};
    while (!state.done && GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (IsDialogMessageW(window, &message) == FALSE) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    if (owner != nullptr) {
        EnableWindow(owner, TRUE);
        SetForegroundWindow(owner);
    }
    if (state.font != nullptr) DeleteObject(state.font);
    if (state.smallFont != nullptr) DeleteObject(state.smallFont);
    if (state.titleFont != nullptr) DeleteObject(state.titleFont);
    if (state.sectionFont != nullptr) DeleteObject(state.sectionFont);
    if (state.backgroundBrush != nullptr) DeleteObject(state.backgroundBrush);
    if (state.surfaceBrush != nullptr) DeleteObject(state.surfaceBrush);
    return 0;
}
