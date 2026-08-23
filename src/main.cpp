#include "AppCore.h"
#include "CommandLine.h"
#include "Logger.h"
#include "ProcessLauncher.h"
#include "ProfileManager.h"
#include "ProfilePicker.h"
#include "ProtocolHandler.h"
#include "Profiles.h"

#include <commctrl.h>
#include <objbase.h>
#include <shellapi.h>

#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

bool HasExactArgument(std::wstring_view tail, std::wstring_view expected) {
    const std::wstring commandLine = L"mpvbridge-check.exe" + std::wstring(tail);
    int count = 0;
    wchar_t** arguments = CommandLineToArgvW(commandLine.c_str(), &count);
    if (arguments == nullptr) return false;
    bool found = false;
    for (int index = 1; index < count; ++index) {
        if (EqualsInsensitive(arguments[index], expected)) {
            found = true;
            break;
        }
    }
    LocalFree(arguments);
    return found;
}

class ComApartment {
public:
    ComApartment() : result_(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED |
                                                     COINIT_DISABLE_OLE1DDE)) {}
    ~ComApartment() {
        if (SUCCEEDED(result_)) CoUninitialize();
    }
    bool Available() const noexcept {
        return SUCCEEDED(result_) || result_ == RPC_E_CHANGED_MODE;
    }

private:
    HRESULT result_;
};

int Run(HINSTANCE instance) {
    INITCOMMONCONTROLSEX commonControls{
        sizeof(commonControls),
        ICC_STANDARD_CLASSES | ICC_LISTVIEW_CLASSES | ICC_WIN95_CLASSES};
    if (InitCommonControlsEx(&commonControls) == FALSE) {
        ShowError(L"无法初始化 Windows 公共控件。" );
        return 2;
    }

    ComApartment com;
    if (!com.Available()) {
        ShowError(L"无法初始化 Windows COM 环境。" );
        return 2;
    }

    const fs::path applicationDirectory = GetModulePath().parent_path();
    const fs::path iniPath = applicationDirectory / L"profiles.ini";
    std::wstring error;
    if (!EnsureProfilesIni(iniPath, error)) {
        ShowError(L"无法准备 profiles.ini：\n" + iniPath.wstring() +
                  L"\n\n" + error);
        return 2;
    }

    ProfileStore store(iniPath, applicationDirectory);
    const ProtocolLaunchRequest protocolRequest =
        ParseMpvBridgeProtocolCommandLine(GetCommandLineW());
    ParsedCommandLine parsed;
    if (protocolRequest.matched) {
        parsed.passThroughTail = L" " + protocolRequest.passThroughTail;
        parsed.error = protocolRequest.error;
    } else if (!protocolRequest.error.empty()) {
        parsed.error = protocolRequest.error;
    } else {
        parsed = ParseCommandLine(GetCommandLineW());
    }
    if (!parsed.error.empty()) {
        WriteDiagnosticLog(store, L"命令行解析失败：" + parsed.error);
        ShowError(parsed.error);
        return 2;
    }
    const bool cookieValidationOnly = protocolRequest.matched &&
        HasExactArgument(parsed.passThroughTail,
                         L"--mpvbridge-validation-only=1");

    // Only a plain double-click has no media arguments and opens management.
    // A decoded mpvbridge:// payload always contains MPV arguments, so it goes
    // directly to the playback flow below (with selection only when needed).
    if (!parsed.requestedProfile.has_value() &&
        !HasVisibleText(parsed.passThroughTail)) {
        WriteDiagnosticLog(store, L"启动模式：Profile 管理");
        return RunProfileManager(instance, store);
    }

    if (store.Load().empty()) {
        WriteDiagnosticLog(store, L"无可用 Profile，无法开始播放");
        ShowError(L"无可用 MPV 播放器。\n\n"
                  L"请双击 MPVBridge 打开 Profile 管理并添加 mpv.exe。");
        return 2;
    }

    std::wstring selectedId;
    if (parsed.requestedProfile.has_value()) {
        selectedId = *parsed.requestedProfile;
        WriteDiagnosticLog(store, L"启动模式：命令行锁定 Profile=" + selectedId);
    } else if (cookieValidationOnly) {
        selectedId = store.DefaultId();
        if (selectedId.empty()) {
            const std::vector<Profile> candidates = store.Load();
            if (!candidates.empty()) selectedId = candidates.front().id;
        }
        WriteDiagnosticLog(store, L"启动模式：油猴 Cookie yt-dlp 预检；默认 Profile=" +
                                      selectedId);
    } else {
        WriteDiagnosticLog(
            store,
            (protocolRequest.matched ? L"启动模式：mpvbridge:// 网页调用；参数字符数="
                                     : L"启动模式：外部媒体调用；参数字符数=") +
                std::to_wstring(parsed.passThroughTail.size()));
        const std::vector<Profile> candidates = store.Load();
        if (candidates.size() == 1) {
            selectedId = candidates.front().id;
            WriteDiagnosticLog(store,
                               L"仅有一个 Profile，跳过选择窗口：" +
                                   selectedId);
        } else if (store.SkipProfilePicker()) {
            const std::wstring defaultId = store.DefaultId();
            if (FindProfile(candidates, defaultId) != nullptr) {
                selectedId = defaultId;
                WriteDiagnosticLog(
                    store, L"已启用直接播放，跳过选择窗口并使用默认 Profile：" +
                               selectedId);
            } else {
                WriteDiagnosticLog(
                    store, L"已启用直接播放，但默认 Profile 不存在；显示选择窗口");
            }
        }
        if (selectedId.empty()) {
            const ProfilePickerResult choice = RunProfilePicker(instance, store);
            if (!choice.profileId.has_value()) {
                WriteDiagnosticLog(store, L"用户取消了外部媒体调用");
                return ERROR_CANCELLED;
            }
            selectedId = *choice.profileId;
        }
    }

    const std::vector<Profile> profiles = store.Load();
    const Profile* selected = FindProfile(profiles, selectedId);
    if (selected == nullptr) {
        WriteDiagnosticLog(store, L"指定的 Profile 不存在：" + selectedId);
        ShowError(L"指定的 Profile 不存在：\n" + selectedId +
                  L"\n\n请打开 MPVBridge 管理配置。" );
        return 2;
    }
    if (!IsUsableExecutable(selected->executable)) {
        WriteDiagnosticLog(store, L"Profile 路径无效：" + selected->id);
        ShowError(L"Profile “" + selected->id +
                  L"” 的 mpv.exe 路径无效：\n" +
                  selected->executable.wstring() +
                  L"\n\n请双击 MPVBridge 打开管理界面修复。" );
        return 2;
    }

    WriteDiagnosticLog(store, L"会话已锁定 Profile=" + selected->id +
                                  L"；目标=" + selected->executable.wstring());
    return static_cast<int>(
        LaunchAndWait(store, *selected, parsed.passThroughTail));
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    try {
        return Run(instance);
    } catch (const fs::filesystem_error& error) {
        ShowError(L"处理文件路径时发生错误：\n" + error.path1().wstring());
        return 2;
    } catch (...) {
        ShowError(L"MPVBridge 遇到未预期的错误。请检查 profiles.ini。" );
        return 2;
    }
}
