#include "MediaAssociations.h"

#include "AppCore.h"
#include "resource.h"
#include "UiCommon.h"

#include <shellapi.h>
#include <shlobj.h>
#include <windowsx.h>

#include <algorithm>
#include <array>
#include <string>
#include <vector>

namespace {

constexpr wchar_t kAssociationClass[] = L"MPVBridge.MediaAssociations";

enum class MediaCategory { Video, Audio, Playlist };

struct MediaExtension {
    const wchar_t* extension;
    const wchar_t* label;
    MediaCategory category;
};

constexpr std::array kMediaExtensions{
    MediaExtension{L".mkv", L".mkv  MKV", MediaCategory::Video},
    MediaExtension{L".mp4", L".mp4  MP4", MediaCategory::Video},
    MediaExtension{L".webm", L".webm  WebM", MediaCategory::Video},
    MediaExtension{L".avi", L".avi  AVI", MediaCategory::Video},
    MediaExtension{L".mov", L".mov  MOV", MediaCategory::Video},
    MediaExtension{L".m4v", L".m4v  M4V", MediaCategory::Video},
    MediaExtension{L".wmv", L".wmv  WMV", MediaCategory::Video},
    MediaExtension{L".flv", L".flv  FLV", MediaCategory::Video},
    MediaExtension{L".ts", L".ts  MPEG-TS", MediaCategory::Video},
    MediaExtension{L".m2ts", L".m2ts  Blu-ray TS", MediaCategory::Video},
    MediaExtension{L".mts", L".mts  AVCHD", MediaCategory::Video},
    MediaExtension{L".mpg", L".mpg  MPEG", MediaCategory::Video},
    MediaExtension{L".mpeg", L".mpeg  MPEG", MediaCategory::Video},
    MediaExtension{L".vob", L".vob  DVD Video", MediaCategory::Video},
    MediaExtension{L".ogv", L".ogv  Ogg Video", MediaCategory::Video},
    MediaExtension{L".3gp", L".3gp  3GPP", MediaCategory::Video},
    MediaExtension{L".3g2", L".3g2  3GPP2", MediaCategory::Video},
    MediaExtension{L".rm", L".rm  RealMedia", MediaCategory::Video},
    MediaExtension{L".rmvb", L".rmvb  RealMedia", MediaCategory::Video},
    MediaExtension{L".asf", L".asf  ASF", MediaCategory::Video},

    MediaExtension{L".mp3", L".mp3  MP3", MediaCategory::Audio},
    MediaExtension{L".flac", L".flac  FLAC", MediaCategory::Audio},
    MediaExtension{L".m4a", L".m4a  M4A", MediaCategory::Audio},
    MediaExtension{L".aac", L".aac  AAC", MediaCategory::Audio},
    MediaExtension{L".ogg", L".ogg  Ogg Audio", MediaCategory::Audio},
    MediaExtension{L".opus", L".opus  Opus", MediaCategory::Audio},
    MediaExtension{L".wav", L".wav  WAV", MediaCategory::Audio},
    MediaExtension{L".wma", L".wma  WMA", MediaCategory::Audio},
    MediaExtension{L".ape", L".ape  Monkey's Audio", MediaCategory::Audio},
    MediaExtension{L".alac", L".alac  ALAC", MediaCategory::Audio},
    MediaExtension{L".ac3", L".ac3  Dolby AC-3", MediaCategory::Audio},
    MediaExtension{L".dts", L".dts  DTS", MediaCategory::Audio},
    MediaExtension{L".mka", L".mka  Matroska Audio", MediaCategory::Audio},
    MediaExtension{L".aiff", L".aiff  AIFF", MediaCategory::Audio},

    MediaExtension{L".m3u", L".m3u  Playlist", MediaCategory::Playlist},
    MediaExtension{L".m3u8", L".m3u8  UTF-8 Playlist", MediaCategory::Playlist},
    MediaExtension{L".pls", L".pls  Playlist", MediaCategory::Playlist},
    MediaExtension{L".cue", L".cue  Cue Sheet", MediaCategory::Playlist},
};

enum ControlId : int {
    IdSelectAll = 400,
    IdSelectNone,
    IdSelectVideo,
    IdSelectAudio,
    IdApply,
    IdClose,
    IdStatus,
    IdExtensionBase = 500,
};

struct AssociationState {
    HINSTANCE instance{};
    HWND owner{};
    HWND window{};
    HWND status{};
    UINT dpi{USER_DEFAULT_SCREEN_DPI};
    bool done{};
    bool ownerRestored{};
    bool trackingMouse{};
    ui::ChromeRects chrome{};
    ui::ChromeButton hotChrome{ui::ChromeButton::None};
    ui::ChromeButton pressedChrome{ui::ChromeButton::None};
    HBRUSH backgroundBrush{};
    HFONT font{};
    HFONT smallFont{};
    HFONT titleFont{};
    HFONT sectionFont{};
    std::vector<HWND> toggles;
};

int S(const AssociationState& state, int value) {
    return ui::Scale(value, state.dpi);
}

std::wstring ProgIdFor(std::wstring_view extension) {
    return L"MPVBridge" + std::wstring(extension);
}

std::wstring QuotePath(const std::filesystem::path& path) {
    return L"\"" + path.wstring() + L"\"";
}

std::wstring RegistryError(LSTATUS status) {
    return FormatSystemError(static_cast<DWORD>(status));
}

bool SetRegistryString(HKEY root, const std::wstring& subKey,
                       const wchar_t* valueName, const std::wstring& value,
                       std::wstring& error, DWORD type = REG_SZ) {
    HKEY key{};
    const LSTATUS created = RegCreateKeyExW(
        root, subKey.c_str(), 0, nullptr, REG_OPTION_NON_VOLATILE,
        KEY_SET_VALUE, nullptr, &key, nullptr);
    if (created != ERROR_SUCCESS) {
        error = RegistryError(created);
        return false;
    }
    const DWORD bytes = type == REG_NONE
                            ? 0
                            : static_cast<DWORD>((value.size() + 1) *
                                                 sizeof(wchar_t));
    const BYTE* data = type == REG_NONE
                           ? nullptr
                           : reinterpret_cast<const BYTE*>(value.c_str());
    const LSTATUS written =
        RegSetValueExW(key, valueName, 0, type, data, bytes);
    RegCloseKey(key);
    if (written != ERROR_SUCCESS) {
        error = RegistryError(written);
        return false;
    }
    return true;
}

void DeleteRegistryValue(HKEY root, const std::wstring& subKey,
                         const wchar_t* valueName) {
    HKEY key{};
    if (RegOpenKeyExW(root, subKey.c_str(), 0, KEY_SET_VALUE, &key) ==
        ERROR_SUCCESS) {
        RegDeleteValueW(key, valueName);
        RegCloseKey(key);
    }
}

std::wstring ReadRegistryString(HKEY root, const std::wstring& subKey,
                                const wchar_t* valueName) {
    DWORD type{};
    DWORD bytes{};
    if (RegGetValueW(root, subKey.c_str(), valueName,
                     RRF_RT_REG_SZ, &type, nullptr, &bytes) != ERROR_SUCCESS ||
        bytes < sizeof(wchar_t)) {
        return {};
    }
    std::vector<wchar_t> buffer(bytes / sizeof(wchar_t));
    if (RegGetValueW(root, subKey.c_str(), valueName,
                     RRF_RT_REG_SZ, &type, buffer.data(), &bytes) !=
        ERROR_SUCCESS) {
        return {};
    }
    return buffer.data();
}

bool IsRegisteredSelection(const MediaExtension& media) {
    const std::wstring value = ReadRegistryString(
        HKEY_CURRENT_USER,
        L"Software\\MPVBridge\\Capabilities\\FileAssociations",
        media.extension);
    return EqualsInsensitive(value, ProgIdFor(media.extension));
}

bool IsCurrentDefault(const MediaExtension& media) {
    const std::wstring choiceKey =
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\FileExts\\" +
        std::wstring(media.extension) + L"\\UserChoice";
    const std::wstring progId =
        ReadRegistryString(HKEY_CURRENT_USER, choiceKey, L"ProgId");
    return EqualsInsensitive(progId, ProgIdFor(media.extension)) ||
           EqualsInsensitive(progId, L"Applications\\MPVBridge.exe");
}

bool RegisterSelections(const std::vector<bool>& selected, std::wstring& error) {
    const std::filesystem::path executable = GetModulePath();
    const std::wstring icon = QuotePath(executable) + L",0";
    const std::wstring command = QuotePath(executable) + L" \"%1\"";
    const std::wstring applicationKey =
        L"Software\\Classes\\Applications\\MPVBridge.exe";
    const std::wstring capabilitiesKey =
        L"Software\\MPVBridge\\Capabilities";
    const std::wstring associationsKey = capabilitiesKey + L"\\FileAssociations";

    if (!SetRegistryString(HKEY_CURRENT_USER, applicationKey,
                           L"FriendlyAppName", L"MPVBridge", error) ||
        !SetRegistryString(HKEY_CURRENT_USER, applicationKey + L"\\DefaultIcon",
                           nullptr, icon, error) ||
        !SetRegistryString(HKEY_CURRENT_USER,
                           applicationKey + L"\\shell\\open\\command",
                           nullptr, command, error) ||
        !SetRegistryString(HKEY_CURRENT_USER, capabilitiesKey,
                           L"ApplicationName", L"MPVBridge", error) ||
        !SetRegistryString(HKEY_CURRENT_USER, capabilitiesKey,
                           L"ApplicationDescription", L"MPVBridge", error) ||
        !SetRegistryString(HKEY_CURRENT_USER, capabilitiesKey,
                           L"ApplicationIcon", icon, error) ||
        !SetRegistryString(HKEY_CURRENT_USER,
                           L"Software\\RegisteredApplications",
                           L"MPVBridge", L"Software\\MPVBridge\\Capabilities",
                           error)) {
        return false;
    }

    for (size_t index = 0; index < kMediaExtensions.size(); ++index) {
        const MediaExtension& media = kMediaExtensions[index];
        const std::wstring extension(media.extension);
        const std::wstring progId = ProgIdFor(extension);
        const std::wstring supportedKey = applicationKey + L"\\SupportedTypes";
        const std::wstring extensionKey =
            L"Software\\Classes\\" + extension + L"\\OpenWithProgids";
        if (!selected[index]) {
            DeleteRegistryValue(HKEY_CURRENT_USER, associationsKey,
                                media.extension);
            DeleteRegistryValue(HKEY_CURRENT_USER, supportedKey,
                                media.extension);
            DeleteRegistryValue(HKEY_CURRENT_USER, extensionKey,
                                progId.c_str());
            continue;
        }

        const std::wstring classKey = L"Software\\Classes\\" + progId;
        if (!SetRegistryString(HKEY_CURRENT_USER, associationsKey,
                               media.extension, progId, error) ||
            !SetRegistryString(HKEY_CURRENT_USER, supportedKey,
                               media.extension, L"", error, REG_NONE) ||
            !SetRegistryString(HKEY_CURRENT_USER, extensionKey,
                               progId.c_str(), L"", error, REG_NONE) ||
            !SetRegistryString(HKEY_CURRENT_USER, classKey,
                               nullptr, L"MPVBridge", error) ||
            !SetRegistryString(HKEY_CURRENT_USER, classKey,
                               L"FriendlyTypeName", L"MPVBridge", error) ||
            !SetRegistryString(HKEY_CURRENT_USER, classKey + L"\\DefaultIcon",
                               nullptr, icon, error) ||
            !SetRegistryString(HKEY_CURRENT_USER,
                               classKey + L"\\shell\\open\\command",
                               nullptr, command, error)) {
            return false;
        }
    }
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return true;
}

bool OpenDefaultAppsSettings(HWND owner) {
    HINSTANCE result = ShellExecuteW(
        owner, L"open",
        L"ms-settings:defaultapps?registeredAppUser=MPVBridge",
        nullptr, nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(result) > 32) return true;
    result = ShellExecuteW(owner, L"open", L"ms-settings:defaultapps",
                           nullptr, nullptr, SW_SHOWNORMAL);
    return reinterpret_cast<INT_PTR>(result) > 32;
}

HWND CreateControl(AssociationState& state, const wchar_t* type,
                   const wchar_t* text, DWORD style, int id) {
    HWND control = CreateWindowExW(
        0, type, text, WS_CHILD | WS_VISIBLE | style,
        0, 0, 0, 0, state.window,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
        state.instance, nullptr);
    ui::SetFont(control, state.font);
    ui::ApplyControlTheme(control);
    return control;
}

void RecreateFonts(AssociationState& state) {
    if (state.font != nullptr) DeleteObject(state.font);
    if (state.smallFont != nullptr) DeleteObject(state.smallFont);
    if (state.titleFont != nullptr) DeleteObject(state.titleFont);
    if (state.sectionFont != nullptr) DeleteObject(state.sectionFont);
    state.font = ui::CreateFont(state.dpi, 10);
    state.smallFont = ui::CreateFont(state.dpi, 9);
    state.titleFont = ui::CreateFont(state.dpi, 21, FW_SEMIBOLD);
    state.sectionFont = ui::CreateFont(state.dpi, 12, FW_SEMIBOLD);
    EnumChildWindows(
        state.window,
        [](HWND child, LPARAM value) -> BOOL {
            ui::SetFont(child, reinterpret_cast<HFONT>(value));
            return TRUE;
        },
        reinterpret_cast<LPARAM>(state.font));
    if (state.status != nullptr) ui::SetFont(state.status, state.smallFont);
}

std::vector<int> IndicesFor(MediaCategory category) {
    std::vector<int> result;
    for (size_t index = 0; index < kMediaExtensions.size(); ++index) {
        if (kMediaExtensions[index].category == category) {
            result.push_back(static_cast<int>(index));
        }
    }
    return result;
}

void LayoutGroup(AssociationState& state, const std::vector<int>& indices,
                 int left, int top, int width, int columns) {
    const int gap = S(state, 10);
    const int rowHeight = S(state, 29);
    const int columnWidth = (width - gap * (columns - 1)) / columns;
    const int rows =
        (static_cast<int>(indices.size()) + columns - 1) / columns;
    for (size_t offset = 0; offset < indices.size(); ++offset) {
        const int column = static_cast<int>(offset) / rows;
        const int row = static_cast<int>(offset) % rows;
        SetWindowPos(state.toggles[static_cast<size_t>(indices[offset])], nullptr,
                     left + column * (columnWidth + gap), top + row * rowHeight,
                     columnWidth, S(state, 25), SWP_NOZORDER);
    }
}

void Layout(AssociationState& state) {
    RECT client{};
    GetClientRect(state.window, &client);
    state.chrome = ui::GetChromeRects(client, state.dpi);
    const int margin = S(state, 26);
    const int toolbarY = S(state, 112);
    SetWindowPos(GetDlgItem(state.window, IdSelectAll), nullptr,
                 margin, toolbarY, S(state, 82), S(state, 30), SWP_NOZORDER);
    SetWindowPos(GetDlgItem(state.window, IdSelectNone), nullptr,
                 margin + S(state, 90), toolbarY, S(state, 82), S(state, 30),
                 SWP_NOZORDER);
    SetWindowPos(GetDlgItem(state.window, IdSelectVideo), nullptr,
                 margin + S(state, 180), toolbarY, S(state, 92), S(state, 30),
                 SWP_NOZORDER);
    SetWindowPos(GetDlgItem(state.window, IdSelectAudio), nullptr,
                 margin + S(state, 280), toolbarY, S(state, 92), S(state, 30),
                 SWP_NOZORDER);

    const int cardTop = S(state, 158);
    const int cardBottom = client.bottom - S(state, 108);
    const int gap = S(state, 16);
    const int cardWidth = (client.right - margin * 2 - gap) / 2;
    const int leftCard = margin;
    const int rightCard = margin + cardWidth + gap;
    LayoutGroup(state, IndicesFor(MediaCategory::Video),
                leftCard + S(state, 16), cardTop + S(state, 45),
                cardWidth - S(state, 32), 2);
    LayoutGroup(state, IndicesFor(MediaCategory::Audio),
                rightCard + S(state, 16), cardTop + S(state, 45),
                cardWidth - S(state, 32), 2);
    LayoutGroup(state, IndicesFor(MediaCategory::Playlist),
                rightCard + S(state, 16), cardBottom - S(state, 80),
                cardWidth - S(state, 32), 2);

    SetWindowPos(state.status, nullptr, margin, client.bottom - S(state, 82),
                 client.right - margin * 2 - S(state, 292), S(state, 42),
                 SWP_NOZORDER);
    SetWindowPos(GetDlgItem(state.window, IdApply), nullptr,
                 client.right - margin - S(state, 194),
                 client.bottom - S(state, 78), S(state, 194), S(state, 36),
                 SWP_NOZORDER);
    SetWindowPos(GetDlgItem(state.window, IdClose), nullptr,
                 client.right - margin - S(state, 286),
                 client.bottom - S(state, 78), S(state, 82), S(state, 36),
                 SWP_NOZORDER);
}

void UpdateStatus(AssociationState& state) {
    int selected = 0;
    int defaults = 0;
    for (size_t index = 0; index < state.toggles.size(); ++index) {
        if (SendMessageW(state.toggles[index], BM_GETCHECK, 0, 0) ==
            BST_CHECKED) {
            ++selected;
        }
        if (IsCurrentDefault(kMediaExtensions[index])) ++defaults;
    }
    ui::SetWindowTextString(
        state.status,
        L"已选择 " + std::to_wstring(selected) + L" 项 · 当前默认 " +
            std::to_wstring(defaults) + L" 项");
}

void SetSelection(AssociationState& state, bool all,
                  MediaCategory category = MediaCategory::Video,
                  bool categoryOnly = false) {
    for (size_t index = 0; index < state.toggles.size(); ++index) {
        const bool checked = categoryOnly
                                 ? kMediaExtensions[index].category == category
                                 : all;
        SendMessageW(state.toggles[index], BM_SETCHECK,
                     checked ? BST_CHECKED : BST_UNCHECKED, 0);
        InvalidateRect(state.toggles[index], nullptr, FALSE);
    }
    UpdateStatus(state);
}

void CreateControls(AssociationState& state) {
    CreateControl(state, L"BUTTON", L"全部", WS_TABSTOP, IdSelectAll);
    CreateControl(state, L"BUTTON", L"全不选", WS_TABSTOP, IdSelectNone);
    CreateControl(state, L"BUTTON", L"仅视频", WS_TABSTOP, IdSelectVideo);
    CreateControl(state, L"BUTTON", L"仅音频", WS_TABSTOP, IdSelectAudio);
    for (size_t index = 0; index < kMediaExtensions.size(); ++index) {
        HWND toggle = CreateControl(
            state, L"BUTTON", kMediaExtensions[index].label,
            WS_TABSTOP | BS_AUTOCHECKBOX,
            IdExtensionBase + static_cast<int>(index));
        state.toggles.push_back(toggle);
        ui::StyleButton(toggle, ui::ButtonStyle::Toggle);
        SendMessageW(toggle, BM_SETCHECK,
                     IsRegisteredSelection(kMediaExtensions[index])
                         ? BST_CHECKED
                         : BST_UNCHECKED,
                     0);
    }
    state.status = CreateControl(state, L"STATIC", L"", SS_LEFT, IdStatus);
    CreateControl(state, L"BUTTON", L"保存并打开系统设置",
                  WS_TABSTOP | BS_DEFPUSHBUTTON, IdApply);
    CreateControl(state, L"BUTTON", L"关闭", WS_TABSTOP, IdClose);

    ui::StyleButton(GetDlgItem(state.window, IdSelectAll),
                    ui::ButtonStyle::Secondary, ui::kBackground);
    ui::StyleButton(GetDlgItem(state.window, IdSelectNone),
                    ui::ButtonStyle::Secondary, ui::kBackground);
    ui::StyleButton(GetDlgItem(state.window, IdSelectVideo),
                    ui::ButtonStyle::Secondary, ui::kBackground);
    ui::StyleButton(GetDlgItem(state.window, IdSelectAudio),
                    ui::ButtonStyle::Secondary, ui::kBackground);
    ui::StyleButton(GetDlgItem(state.window, IdApply),
                    ui::ButtonStyle::Primary, ui::kBackground);
    ui::StyleButton(GetDlgItem(state.window, IdClose),
                    ui::ButtonStyle::Secondary, ui::kBackground);
    RecreateFonts(state);
    UpdateStatus(state);
}

void Paint(AssociationState& state) {
    PAINTSTRUCT paint{};
    HDC target = BeginPaint(state.window, &paint);
    RECT client{};
    GetClientRect(state.window, &client);
    HDC dc = CreateCompatibleDC(target);
    HBITMAP bitmap = CreateCompatibleBitmap(
        target, std::max<LONG>(1, client.right),
        std::max<LONG>(1, client.bottom));
    HGDIOBJ oldBitmap = SelectObject(dc, bitmap);
    ui::FillSolid(dc, client, ui::kBackground);
    RECT header{0, 0, client.right, S(state, 96)};
    ui::FillSolid(dc, header, ui::kHeader);
    RECT accent{0, S(state, 92), client.right, S(state, 96)};
    ui::FillSolid(dc, accent, ui::kAccent);
    RECT title{S(state, 28), S(state, 16), client.right - S(state, 170),
               S(state, 53)};
    ui::DrawTextLine(dc, L"媒体文件关联", title, state.titleFont,
                     RGB(255, 255, 255),
                     DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    RECT subtitle{S(state, 30), S(state, 52), client.right - S(state, 170),
                  S(state, 80)};
    ui::DrawTextLine(dc, L"选择希望由 MPVBridge 接管的媒体扩展名",
                     subtitle, state.font, RGB(190, 202, 222),
                     DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    ui::DrawChromeButtons(dc, state.chrome, state.hotChrome,
                          state.pressedChrome,
                          IsZoomed(state.window) != FALSE, state.dpi);

    const int margin = S(state, 26);
    const int cardTop = S(state, 158);
    const int cardBottom = client.bottom - S(state, 108);
    const int gap = S(state, 16);
    const int cardWidth = (client.right - margin * 2 - gap) / 2;
    RECT videoCard{margin, cardTop, margin + cardWidth, cardBottom};
    RECT audioCard{videoCard.right + gap, cardTop,
                   videoCard.right + gap + cardWidth, cardBottom};
    ui::FillRounded(dc, videoCard, S(state, 14), ui::kSurface, ui::kBorder);
    ui::FillRounded(dc, audioCard, S(state, 14), ui::kSurface, ui::kBorder);
    RECT videoTitle{videoCard.left + S(state, 16), cardTop + S(state, 8),
                    videoCard.right - S(state, 16), cardTop + S(state, 38)};
    RECT audioTitle{audioCard.left + S(state, 16), cardTop + S(state, 8),
                    audioCard.right - S(state, 16), cardTop + S(state, 38)};
    ui::DrawTextLine(dc, L"视频", videoTitle, state.sectionFont, ui::kText,
                     DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    ui::DrawTextLine(dc, L"音频", audioTitle, state.sectionFont, ui::kText,
                     DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    RECT playlistTitle{audioCard.left + S(state, 16),
                       cardBottom - S(state, 112),
                       audioCard.right - S(state, 16),
                       cardBottom - S(state, 84)};
    ui::DrawTextLine(dc, L"播放列表", playlistTitle, state.sectionFont,
                     ui::kText, DT_LEFT | DT_SINGLELINE | DT_VCENTER);

    BitBlt(target, paint.rcPaint.left, paint.rcPaint.top,
           paint.rcPaint.right - paint.rcPaint.left,
           paint.rcPaint.bottom - paint.rcPaint.top,
           dc, paint.rcPaint.left, paint.rcPaint.top, SRCCOPY);
    SelectObject(dc, oldBitmap);
    DeleteObject(bitmap);
    DeleteDC(dc);
    EndPaint(state.window, &paint);
}

void ApplySelections(AssociationState& state) {
    std::vector<bool> selected(state.toggles.size());
    int count = 0;
    for (size_t index = 0; index < state.toggles.size(); ++index) {
        selected[index] =
            SendMessageW(state.toggles[index], BM_GETCHECK, 0, 0) ==
            BST_CHECKED;
        if (selected[index]) ++count;
    }
    std::wstring error;
    if (!RegisterSelections(selected, error)) {
        ShowError(L"无法保存媒体文件关联：\n" + error, state.window);
        return;
    }
    UpdateStatus(state);
    ShowInfo(L"已注册 " + std::to_wstring(count) +
                 L" 个媒体扩展名。\n\nWindows 将打开默认应用页面；请确认由 MPVBridge 打开所需类型。",
             state.window);
    if (!OpenDefaultAppsSettings(state.window)) {
        ShowError(L"无法打开 Windows 默认应用设置。", state.window);
    }
}

void RestoreOwnerBeforeClose(AssociationState& state) {
    if (state.owner == nullptr || state.ownerRestored) return;
    SendMessageW(state.owner, WM_SETREDRAW, FALSE, 0);
    EnableWindow(state.owner, TRUE);
    SendMessageW(state.owner, WM_SETREDRAW, TRUE, 0);
    RedrawWindow(state.owner, nullptr, nullptr,
                 RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW);
    state.ownerRestored = true;
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam,
                            LPARAM lParam) {
    auto* state = reinterpret_cast<AssociationState*>(
        GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        state = static_cast<AssociationState*>(create->lpCreateParams);
        state->window = window;
        SetWindowLongPtrW(window, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(state));
    }
    if (state == nullptr) return DefWindowProcW(window, message, wParam, lParam);

    switch (message) {
    case WM_CREATE:
        state->dpi = GetDpiForWindow(window);
        state->backgroundBrush = CreateSolidBrush(ui::kBackground);
        ui::ApplyModernWindowFrame(window);
        CreateControls(*state);
        return 0;
    case WM_NCCALCSIZE:
        return 0;
    case WM_NCHITTEST: {
        const POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        return ui::FramelessHitTest(window, point, state->dpi, S(*state, 96),
                                    state->chrome);
    }
    case WM_NCACTIVATE:
        return TRUE;
    case WM_SIZE:
        Layout(*state);
        if (wParam != SIZE_MINIMIZED) {
            ui::ApplyRoundedWindowRegion(window, state->dpi);
        }
        InvalidateRect(window, nullptr, TRUE);
        return 0;
    case WM_DPICHANGED: {
        state->dpi = HIWORD(wParam);
        const RECT* suggested = reinterpret_cast<RECT*>(lParam);
        SetWindowPos(window, nullptr, suggested->left, suggested->top,
                     suggested->right - suggested->left,
                     suggested->bottom - suggested->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        RecreateFonts(*state);
        Layout(*state);
        ui::ApplyRoundedWindowRegion(window, state->dpi);
        return 0;
    }
    case WM_ACTIVATE:
        if (LOWORD(wParam) != WA_INACTIVE) UpdateStatus(*state);
        break;
    case WM_MOUSEMOVE: {
        const POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        const ui::ChromeButton hit = ui::HitChromeButton(point, state->chrome);
        if (hit != ui::ChromeButton::Maximize && hit != state->hotChrome) {
            state->hotChrome = hit;
            ui::InvalidateChrome(window, state->chrome);
        }
        if (!state->trackingMouse) {
            TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, window, 0};
            TrackMouseEvent(&track);
            state->trackingMouse = true;
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        state->trackingMouse = false;
        state->hotChrome = ui::ChromeButton::None;
        state->pressedChrome = ui::ChromeButton::None;
        ui::InvalidateChrome(window, state->chrome);
        return 0;
    case WM_LBUTTONDOWN: {
        const POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        const ui::ChromeButton hit = ui::HitChromeButton(point, state->chrome);
        if (hit == ui::ChromeButton::Minimize ||
            hit == ui::ChromeButton::Close) {
            state->pressedChrome = hit;
            SetCapture(window);
            ui::InvalidateChrome(window, state->chrome);
            return 0;
        }
        break;
    }
    case WM_LBUTTONUP: {
        const POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        const ui::ChromeButton hit = ui::HitChromeButton(point, state->chrome);
        const ui::ChromeButton pressed = state->pressedChrome;
        if (pressed != ui::ChromeButton::None) {
            ReleaseCapture();
            state->pressedChrome = ui::ChromeButton::None;
            ui::InvalidateChrome(window, state->chrome);
            if (hit == pressed) {
                if (pressed == ui::ChromeButton::Minimize) {
                    ShowWindow(window, SW_MINIMIZE);
                } else if (pressed == ui::ChromeButton::Close) {
                    SendMessageW(window, WM_CLOSE, 0, 0);
                }
            }
            return 0;
        }
        break;
    }
    case WM_NCMOUSEMOVE:
        if (wParam == HTMAXBUTTON) {
            if (state->hotChrome == ui::ChromeButton::Maximize) return 0;
            state->hotChrome = ui::ChromeButton::Maximize;
            TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE | TME_NONCLIENT,
                                  window, 0};
            TrackMouseEvent(&track);
            ui::InvalidateChrome(window, state->chrome);
            return 0;
        }
        break;
    case WM_NCMOUSELEAVE:
        state->hotChrome = ui::ChromeButton::None;
        state->pressedChrome = ui::ChromeButton::None;
        ui::InvalidateChrome(window, state->chrome);
        return 0;
    case WM_NCLBUTTONDOWN:
        if (wParam == HTMAXBUTTON) {
            state->pressedChrome = ui::ChromeButton::Maximize;
            ui::InvalidateChrome(window, state->chrome);
            return 0;
        }
        break;
    case WM_NCLBUTTONUP:
        if (wParam == HTMAXBUTTON &&
            state->pressedChrome == ui::ChromeButton::Maximize) {
            state->pressedChrome = ui::ChromeButton::None;
            state->hotChrome = ui::ChromeButton::None;
            ui::ToggleMaximize(window);
            ui::InvalidateChrome(window, state->chrome);
            return 0;
        }
        break;
    case WM_NCLBUTTONDBLCLK:
        if (wParam == HTCAPTION) {
            ui::ToggleMaximize(window);
            return 0;
        }
        break;
    case WM_COMMAND: {
        const int id = LOWORD(wParam);
        if (id >= IdExtensionBase &&
            id < IdExtensionBase + static_cast<int>(kMediaExtensions.size())) {
            UpdateStatus(*state);
            return 0;
        }
        if (id == IdSelectAll) {
            SetSelection(*state, true);
            return 0;
        }
        if (id == IdSelectNone) {
            SetSelection(*state, false);
            return 0;
        }
        if (id == IdSelectVideo) {
            SetSelection(*state, false, MediaCategory::Video, true);
            return 0;
        }
        if (id == IdSelectAudio) {
            SetSelection(*state, false, MediaCategory::Audio, true);
            return 0;
        }
        if (id == IdApply) {
            ApplySelections(*state);
            return 0;
        }
        if (id == IdClose) {
            DestroyWindow(window);
            return 0;
        }
        break;
    }
    case WM_CTLCOLORSTATIC: {
        HDC dc = reinterpret_cast<HDC>(wParam);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, ui::kMuted);
        return reinterpret_cast<LRESULT>(state->backgroundBrush);
    }
    case WM_CTLCOLORBTN: {
        HDC dc = reinterpret_cast<HDC>(wParam);
        SetBkMode(dc, TRANSPARENT);
        return reinterpret_cast<LRESULT>(GetStockObject(NULL_BRUSH));
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        Paint(*state);
        return 0;
    case WM_GETMINMAXINFO: {
        auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
        ui::ApplyMonitorWorkArea(window, *info);
        info->ptMinTrackSize = POINT{S(*state, 760), S(*state, 620)};
        return 0;
    }
    case WM_CLOSE:
        RestoreOwnerBeforeClose(*state);
        DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        state->done = true;
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

bool RegisterAssociationClass(HINSTANCE instance) {
    WNDCLASSEXW windowClass{sizeof(windowClass)};
    windowClass.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    windowClass.lpfnWndProc = WindowProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hIcon =
        LoadIconW(instance, MAKEINTRESOURCEW(IDI_APP_ICON));
    windowClass.hIconSm = windowClass.hIcon;
    windowClass.hbrBackground = nullptr;
    windowClass.lpszClassName = kAssociationClass;
    return RegisterClassExW(&windowClass) != 0 ||
           GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

} // namespace

int RunMediaAssociationsDialog(HINSTANCE instance, HWND owner) {
    if (!RegisterAssociationClass(instance)) {
        ShowError(L"无法创建媒体文件关联窗口。", owner);
        return 1;
    }
    AssociationState state{};
    state.instance = instance;
    state.owner = owner;
    const UINT dpi = owner != nullptr ? GetDpiForWindow(owner) : GetDpiForSystem();
    HWND window = CreateWindowExW(
        WS_EX_CONTROLPARENT | WS_EX_APPWINDOW, kAssociationClass,
        L"MPVBridge · 媒体文件关联",
        WS_POPUP | WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX |
            WS_MAXIMIZEBOX | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT, ui::Scale(820, dpi), ui::Scale(660, dpi),
        owner, nullptr, instance, &state);
    if (window == nullptr) {
        ShowError(L"无法创建媒体文件关联窗口：\n" +
                      FormatSystemError(GetLastError()),
                  owner);
        return 1;
    }

    if (owner != nullptr) EnableWindow(owner, FALSE);
    ui::CenterWindow(window, owner);
    ShowWindow(window, SW_SHOWNORMAL);
    UpdateWindow(window);

    MSG message{};
    while (!state.done && GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(window, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    if (owner != nullptr) {
        if (!state.ownerRestored) EnableWindow(owner, TRUE);
        SetForegroundWindow(owner);
    }
    if (state.backgroundBrush != nullptr) DeleteObject(state.backgroundBrush);
    if (state.font != nullptr) DeleteObject(state.font);
    if (state.smallFont != nullptr) DeleteObject(state.smallFont);
    if (state.titleFont != nullptr) DeleteObject(state.titleFont);
    if (state.sectionFont != nullptr) DeleteObject(state.sectionFont);
    return 0;
}
