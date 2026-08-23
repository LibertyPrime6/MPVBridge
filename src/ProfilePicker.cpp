#include "ProfilePicker.h"

#include "AppCore.h"
#include "Logger.h"
#include "ProfileManager.h"
#include "resource.h"
#include "UiCommon.h"

#include <commctrl.h>
#include <windowsx.h>

#include <algorithm>
#include <string>
#include <vector>

namespace {

constexpr wchar_t kPickerClass[] = L"MPVBridge.ProfilePicker";
constexpr UINT_PTR kAutoLaunchTimer = 2;
constexpr UINT kCancelAutoLaunchMessage = WM_APP + 51;
constexpr UINT_PTR kPickerListSubclass = 0x4D505601;

enum ControlId : int {
    IdProfileList = 300,
    IdLaunch,
    IdManage,
    IdCancel,
    IdRemember,
    IdStatus,
    IdQuickBase = 400,
};

struct PickerState {
    HINSTANCE instance{};
    HWND window{};
    HWND owner{};
    ProfileStore* store{};
    UINT dpi{USER_DEFAULT_SCREEN_DPI};
    std::vector<Profile> profiles;
    std::wstring defaultId;
    ProfilePickerResult result;
    bool done{};
    HFONT font{};
    HFONT smallFont{};
    HFONT titleFont{};
    HFONT nameFont{};
    HBRUSH backgroundBrush{};
    ui::ChromeRects chrome{};
    ui::ChromeButton hotChrome{ui::ChromeButton::None};
    ui::ChromeButton pressedChrome{ui::ChromeButton::None};
    bool trackingMouse{};
    bool autoLaunchActive{};
    bool userInteracted{};
    int autoLaunchSeconds{};
    ULONGLONG autoLaunchDeadline{};
    HWND list{};
    HWND status{};
    HWND launch{};
    HWND remember{};
    bool positionSelectionAfterLayout{};
};

int S(const PickerState& state, int value) { return ui::Scale(value, state.dpi); }

void UpdateSelectionState(PickerState& state);

void PositionSelectedProfile(PickerState& state) {
    if (!state.positionSelectionAfterLayout || state.list == nullptr) return;

    const int selected =
        static_cast<int>(SendMessageW(state.list, LB_GETCURSEL, 0, 0));
    if (selected < 0) {
        state.positionSelectionAfterLayout = false;
        return;
    }

    RECT client{};
    GetClientRect(state.list, &client);
    const int itemHeight =
        static_cast<int>(SendMessageW(state.list, LB_GETITEMHEIGHT, 0, 0));
    const int clientHeight = client.bottom - client.top;
    if (itemHeight <= 0 || clientHeight <= 0) return;

    const int visibleItems = std::max(1, clientHeight / itemHeight);
    const int itemCount =
        static_cast<int>(SendMessageW(state.list, LB_GETCOUNT, 0, 0));
    const int maxTopIndex = std::max(0, itemCount - visibleItems);
    const int topIndex =
        std::clamp(selected - visibleItems / 2, 0, maxTopIndex);
    SendMessageW(state.list, LB_SETTOPINDEX, topIndex, 0);
    state.positionSelectionAfterLayout = false;
}

void CancelAutoLaunch(PickerState& state, bool userInteraction = true) {
    if (userInteraction) state.userInteracted = true;
    if (!state.autoLaunchActive) return;
    state.autoLaunchActive = false;
    KillTimer(state.window, kAutoLaunchTimer);
    if (userInteraction) {
        WriteDiagnosticLog(*state.store,
                           L"用户操作取消了默认 Profile 自动进入");
    }
    UpdateSelectionState(state);
}

LRESULT CALLBACK PickerListSubclassProc(HWND window, UINT message,
                                        WPARAM wParam, LPARAM lParam,
                                        UINT_PTR subclassId, DWORD_PTR) {
    if (message == WM_LBUTTONDOWN || message == WM_MOUSEWHEEL ||
        message == WM_KEYDOWN) {
        SendMessageW(GetParent(window), kCancelAutoLaunchMessage, 0, 0);
    }
    if (message == WM_NCDESTROY) {
        RemoveWindowSubclass(window, PickerListSubclassProc, subclassId);
    }
    return DefSubclassProc(window, message, wParam, lParam);
}

HWND Control(PickerState& state, const wchar_t* type, const wchar_t* text,
             DWORD style, int id) {
    HWND control = CreateWindowExW(
        0, type, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0,
        state.window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
        state.instance, nullptr);
    ui::SetFont(control, state.font);
    ui::ApplyControlTheme(control);
    return control;
}

void RecreateFonts(PickerState& state) {
    if (state.font != nullptr) DeleteObject(state.font);
    if (state.smallFont != nullptr) DeleteObject(state.smallFont);
    if (state.titleFont != nullptr) DeleteObject(state.titleFont);
    if (state.nameFont != nullptr) DeleteObject(state.nameFont);
    state.font = ui::CreateFont(state.dpi, 10);
    state.smallFont = ui::CreateFont(state.dpi, 9);
    state.titleFont = ui::CreateFont(state.dpi, 21, FW_SEMIBOLD);
    state.nameFont = ui::CreateFont(state.dpi, 11, FW_SEMIBOLD);
    EnumChildWindows(state.window,
                     [](HWND child, LPARAM parameter) -> BOOL {
                         ui::SetFont(child, reinterpret_cast<HFONT>(parameter));
                         return TRUE;
                     },
                     reinterpret_cast<LPARAM>(state.font));
    if (state.status != nullptr) ui::SetFont(state.status, state.smallFont);
    if (state.list != nullptr) {
        SendMessageW(state.list, LB_SETITEMHEIGHT, 0, S(state, 78));
    }
}

void Layout(PickerState& state) {
    RECT client{};
    GetClientRect(state.window, &client);
    const int margin = S(state, 28);
    const int listTop = S(state, 132);
    const int footerHeight = S(state, 112);
    state.chrome = ui::GetChromeRects(client, state.dpi);
    SetWindowPos(state.list, nullptr, margin, listTop,
                 client.right - margin * 2,
                 client.bottom - listTop - footerHeight, SWP_NOZORDER);
    SetWindowPos(state.status, nullptr, margin, client.bottom - S(state, 101),
                 client.right - margin * 2, S(state, 24), SWP_NOZORDER);
    SetWindowPos(state.remember, nullptr, margin, client.bottom - S(state, 67),
                 S(state, 210), S(state, 28), SWP_NOZORDER);
    SetWindowPos(GetDlgItem(state.window, IdManage), nullptr,
                 client.right - margin - S(state, 286), client.bottom - S(state, 70),
                 S(state, 90), S(state, 34), SWP_NOZORDER);
    SetWindowPos(GetDlgItem(state.window, IdCancel), nullptr,
                 client.right - margin - S(state, 186), client.bottom - S(state, 70),
                 S(state, 78), S(state, 34), SWP_NOZORDER);
    SetWindowPos(state.launch, nullptr,
                 client.right - margin - S(state, 98), client.bottom - S(state, 70),
                 S(state, 98), S(state, 34), SWP_NOZORDER);
    PositionSelectedProfile(state);
}

void UpdateSelectionState(PickerState& state) {
    const int index = static_cast<int>(SendMessageW(state.list, LB_GETCURSEL, 0, 0));
    bool valid = index >= 0 && static_cast<size_t>(index) < state.profiles.size() &&
                 IsUsableExecutable(state.profiles[static_cast<size_t>(index)].executable);
    EnableWindow(state.launch, valid ? TRUE : FALSE);
    if (index < 0 || static_cast<size_t>(index) >= state.profiles.size()) {
        ui::SetWindowTextString(state.status, L"没有可选择的 Profile，请先打开管理界面添加。" );
    } else if (valid && state.autoLaunchActive) {
        const ULONGLONG now = GetTickCount64();
        const ULONGLONG remainingMilliseconds =
            state.autoLaunchDeadline > now ? state.autoLaunchDeadline - now : 0;
        const int remainingSeconds = std::max(
            1, static_cast<int>((remainingMilliseconds + 999) / 1000));
        ui::SetWindowTextString(
            state.status,
            std::to_wstring(remainingSeconds) +
                L" 秒后自动使用默认 Profile · 任意操作取消");
    } else if (valid) {
        ui::SetWindowTextString(state.status,
                                L"✓ 已就绪 · Enter 或双击开始本次播放会话");
    } else {
        ui::SetWindowTextString(state.status,
                                L"⚠ 此 Profile 的 mpv.exe 路径无效，请先管理 Profile");
    }
    InvalidateRect(state.status, nullptr, TRUE);
}

void StartAutoLaunch(PickerState& state) {
    if (state.userInteracted) return;
    state.autoLaunchSeconds = state.store->AutoLaunchSeconds();
    if (state.autoLaunchSeconds <= 0 || state.defaultId.empty()) return;
    const int index =
        static_cast<int>(SendMessageW(state.list, LB_GETCURSEL, 0, 0));
    if (index < 0 || static_cast<size_t>(index) >= state.profiles.size()) return;
    const Profile& profile = state.profiles[static_cast<size_t>(index)];
    if (!EqualsInsensitive(profile.id, state.defaultId) ||
        !IsUsableExecutable(profile.executable)) {
        return;
    }
    state.autoLaunchActive = true;
    state.autoLaunchDeadline =
        GetTickCount64() + static_cast<ULONGLONG>(state.autoLaunchSeconds) * 1000;
    if (SetTimer(state.window, kAutoLaunchTimer, 100, nullptr) == 0) {
        state.autoLaunchActive = false;
        return;
    }
    WriteDiagnosticLog(
        *state.store,
        L"默认 Profile 自动进入倒计时已启动：" + profile.id + L"，" +
            std::to_wstring(state.autoLaunchSeconds) + L" 秒");
    UpdateSelectionState(state);
}

void LoadProfiles(PickerState& state, std::wstring_view preferred = {}) {
    state.profiles = state.store->Load();
    state.defaultId = state.store->DefaultId();
    SendMessageW(state.list, LB_RESETCONTENT, 0, 0);
    int selected = -1;
    for (size_t index = 0; index < state.profiles.size(); ++index) {
        const int item = static_cast<int>(SendMessageW(
            state.list, LB_ADDSTRING, 0,
            reinterpret_cast<LPARAM>(state.profiles[index].name.c_str())));
        SendMessageW(state.list, LB_SETITEMDATA, item, static_cast<LPARAM>(index));
        const std::wstring_view wanted = preferred.empty()
                                             ? std::wstring_view(state.defaultId)
                                             : preferred;
        if (!wanted.empty() && EqualsInsensitive(state.profiles[index].id, wanted)) {
            selected = item;
        }
    }
    if (selected < 0 && !state.profiles.empty()) selected = 0;
    SendMessageW(state.list, LB_SETCURSEL, selected, 0);
    state.positionSelectionAfterLayout = true;
    PositionSelectedProfile(state);
    UpdateSelectionState(state);
    InvalidateRect(state.list, nullptr, TRUE);
    StartAutoLaunch(state);
}

void AcceptSelection(PickerState& state) {
    if (state.autoLaunchActive) {
        state.autoLaunchActive = false;
        KillTimer(state.window, kAutoLaunchTimer);
    }
    const int index = static_cast<int>(SendMessageW(state.list, LB_GETCURSEL, 0, 0));
    if (index < 0 || static_cast<size_t>(index) >= state.profiles.size()) return;
    const Profile& profile = state.profiles[static_cast<size_t>(index)];
    if (!IsUsableExecutable(profile.executable)) {
        ShowError(L"该 Profile 的 mpv.exe 路径无效。请先点击“管理”进行修复。",
                  state.window);
        return;
    }
    state.result.profileId = profile.id;
    state.result.setAsDefault =
        SendMessageW(state.remember, BM_GETCHECK, 0, 0) == BST_CHECKED;
    DestroyWindow(state.window);
}

void DrawPickerItem(PickerState& state, const DRAWITEMSTRUCT& item) {
    if (item.itemID == static_cast<UINT>(-1) ||
        item.itemID >= state.profiles.size()) return;
    const Profile& profile = state.profiles[item.itemID];
    const bool selected = (item.itemState & ODS_SELECTED) != 0;
    const bool valid = IsUsableExecutable(profile.executable);
    RECT card = item.rcItem;
    InflateRect(&card, -S(state, 5), -S(state, 5));
    ui::FillRounded(item.hDC, card, S(state, 12),
                    selected ? ui::kAccentSoft : ui::kSurface,
                    selected ? RGB(174, 201, 247) : ui::kBorder);
    if (selected) {
        RECT stripe{card.left, card.top + S(state, 10), card.left + S(state, 4),
                    card.bottom - S(state, 10)};
        ui::FillRounded(item.hDC, stripe, S(state, 3), ui::kAccent);
    }

    const int badgeSize = S(state, 34);
    RECT badge{card.left + S(state, 16), card.top + S(state, 17),
               card.left + S(state, 16) + badgeSize,
               card.top + S(state, 17) + badgeSize};
    ui::FillRounded(item.hDC, badge, badgeSize, selected ? ui::kAccent : RGB(232, 237, 246));
    RECT badgeText = badge;
    ui::DrawTextLine(item.hDC,
                     item.itemID < 9 ? std::to_wstring(item.itemID + 1) : L"·",
                     badgeText, state.nameFont,
                     selected ? RGB(255, 255, 255) : ui::kMuted,
                     DT_CENTER | DT_SINGLELINE | DT_VCENTER);

    const int textLeft = badge.right + S(state, 14);
    RECT name{textLeft, card.top + S(state, 10), card.right - S(state, 92),
              card.top + S(state, 35)};
    ui::DrawTextLine(item.hDC, profile.name, name, state.nameFont, ui::kText,
                     DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS | DT_VCENTER);
    std::wstring detail = profile.id + L"  ·  " + profile.executable.wstring();
    RECT path{textLeft, card.top + S(state, 37), card.right - S(state, 16),
              card.bottom - S(state, 8)};
    ui::DrawTextLine(item.hDC, detail, path, state.smallFont, ui::kMuted,
                     DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS | DT_VCENTER);

    std::wstring tag = EqualsInsensitive(profile.id, state.defaultId) ? L"默认" :
                       valid ? L"可用" : L"无效";
    RECT tagRect{card.right - S(state, 76), card.top + S(state, 12),
                 card.right - S(state, 16), card.top + S(state, 34)};
    ui::FillRounded(item.hDC, tagRect, S(state, 10),
                    !valid ? RGB(255, 235, 233) :
                    EqualsInsensitive(profile.id, state.defaultId)
                        ? RGB(232, 240, 255)
                        : RGB(231, 247, 240));
    ui::DrawTextLine(item.hDC, tag, tagRect, state.smallFont,
                     !valid ? ui::kDanger :
                     EqualsInsensitive(profile.id, state.defaultId)
                         ? ui::kAccent
                         : ui::kSuccess,
                     DT_CENTER | DT_SINGLELINE | DT_VCENTER);
}

void Paint(PickerState& state) {
    PAINTSTRUCT paint{};
    HDC target = BeginPaint(state.window, &paint);
    RECT client{};
    GetClientRect(state.window, &client);
    HDC dc = CreateCompatibleDC(target);
    HBITMAP bitmap = CreateCompatibleBitmap(target, std::max<LONG>(1, client.right),
                                             std::max<LONG>(1, client.bottom));
    HGDIOBJ oldBitmap = SelectObject(dc, bitmap);
    ui::FillSolid(dc, client, ui::kBackground);
    RECT header{0, 0, client.right, S(state, 108)};
    ui::FillSolid(dc, header, ui::kHeader);
    RECT accent{0, S(state, 104), client.right, S(state, 108)};
    ui::FillSolid(dc, accent, ui::kAccent);
    RECT title{S(state, 28), S(state, 18), client.right - S(state, 28), S(state, 55)};
    ui::DrawTextLine(dc, L"选择本次播放使用的 MPV", title, state.titleFont,
                     RGB(255, 255, 255), DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    RECT subtitle{S(state, 30), S(state, 57), client.right - S(state, 28), S(state, 86)};
    ui::DrawTextLine(dc,
                     L"Profile 将锁定到会话结束 · 数字键 1–9 快选 · Esc 取消",
                     subtitle, state.font, RGB(190, 202, 222),
                     DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    ui::DrawChromeButtons(dc, state.chrome, state.hotChrome, state.pressedChrome,
                          IsZoomed(state.window) != FALSE, state.dpi);
    BitBlt(target, paint.rcPaint.left, paint.rcPaint.top,
           paint.rcPaint.right - paint.rcPaint.left,
           paint.rcPaint.bottom - paint.rcPaint.top, dc,
           paint.rcPaint.left, paint.rcPaint.top, SRCCOPY);
    SelectObject(dc, oldBitmap);
    DeleteObject(bitmap);
    DeleteDC(dc);
    EndPaint(state.window, &paint);
}

void CreateControls(PickerState& state) {
    state.list = Control(state, L"LISTBOX", L"",
                         WS_TABSTOP | WS_VSCROLL | LBS_NOTIFY | LBS_OWNERDRAWFIXED |
                             LBS_NOINTEGRALHEIGHT,
                         IdProfileList);
    SetWindowSubclass(state.list, PickerListSubclassProc,
                      kPickerListSubclass, 0);
    state.status = Control(state, L"STATIC", L"", SS_LEFT, IdStatus);
    state.remember = Control(state, L"BUTTON", L"设为默认 Profile",
                             WS_TABSTOP | BS_AUTOCHECKBOX, IdRemember);
    Control(state, L"BUTTON", L"管理", WS_TABSTOP, IdManage);
    Control(state, L"BUTTON", L"取消", WS_TABSTOP, IdCancel);
    state.launch = Control(state, L"BUTTON", L"开始播放",
                           WS_TABSTOP | BS_DEFPUSHBUTTON, IdLaunch);
    ui::StyleButton(state.remember, ui::ButtonStyle::Toggle,
                    ui::kBackground);
    ui::StyleButton(GetDlgItem(state.window, IdManage),
                    ui::ButtonStyle::Secondary, ui::kBackground);
    ui::StyleButton(GetDlgItem(state.window, IdCancel),
                    ui::ButtonStyle::Secondary, ui::kBackground);
    ui::StyleButton(state.launch, ui::ButtonStyle::Primary,
                    ui::kBackground);
    RecreateFonts(state);
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* state = reinterpret_cast<PickerState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        state = static_cast<PickerState*>(create->lpCreateParams);
        state->window = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }
    if (state == nullptr) return DefWindowProcW(window, message, wParam, lParam);

    switch (message) {
    case WM_CREATE:
        state->dpi = GetDpiForWindow(window);
        state->backgroundBrush = CreateSolidBrush(ui::kBackground);
        ui::ApplyModernWindowFrame(window);
        CreateControls(*state);
        LoadProfiles(*state);
        return 0;
    case WM_NCCALCSIZE:
        return 0;
    case WM_NCHITTEST: {
        const POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        return ui::FramelessHitTest(window, point, state->dpi, S(*state, 108),
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
        CancelAutoLaunch(*state);
        const POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        const ui::ChromeButton hit = ui::HitChromeButton(point, state->chrome);
        if (hit == ui::ChromeButton::Minimize || hit == ui::ChromeButton::Close) {
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
                if (pressed == ui::ChromeButton::Minimize) ShowWindow(window, SW_MINIMIZE);
                if (pressed == ui::ChromeButton::Close) SendMessageW(window, WM_CLOSE, 0, 0);
            }
            return 0;
        }
        break;
    }
    case WM_NCMOUSEMOVE:
        if (wParam == HTMAXBUTTON) {
            if (state->hotChrome == ui::ChromeButton::Maximize) return 0;
            state->hotChrome = ui::ChromeButton::Maximize;
            TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE | TME_NONCLIENT, window, 0};
            TrackMouseEvent(&track);
            ui::InvalidateChrome(window, state->chrome);
            return 0;
        }
        break;
    case WM_NCMOUSELEAVE:
        if (state->hotChrome == ui::ChromeButton::None &&
            state->pressedChrome == ui::ChromeButton::None) return 0;
        state->hotChrome = ui::ChromeButton::None;
        state->pressedChrome = ui::ChromeButton::None;
        ui::InvalidateChrome(window, state->chrome);
        return 0;
    case WM_NCLBUTTONDOWN:
        CancelAutoLaunch(*state);
        if (wParam == HTMAXBUTTON) {
            if (state->pressedChrome == ui::ChromeButton::Maximize) return 0;
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
        CancelAutoLaunch(*state);
        if (wParam == HTCAPTION) {
            ui::ToggleMaximize(window);
            return 0;
        }
        break;
    case kCancelAutoLaunchMessage:
        CancelAutoLaunch(*state);
        return 0;
    case WM_TIMER:
        if (wParam == kAutoLaunchTimer && state->autoLaunchActive) {
            if (GetTickCount64() >= state->autoLaunchDeadline) {
                state->autoLaunchActive = false;
                KillTimer(window, kAutoLaunchTimer);
                WriteDiagnosticLog(*state->store,
                                   L"默认 Profile 自动进入倒计时完成");
                AcceptSelection(*state);
            } else {
                UpdateSelectionState(*state);
            }
            return 0;
        }
        break;
    case WM_COMMAND: {
        const int id = LOWORD(wParam);
        CancelAutoLaunch(*state);
        if (id == IdProfileList && HIWORD(wParam) == LBN_SELCHANGE) {
            UpdateSelectionState(*state);
            return 0;
        }
        if (id == IdProfileList && HIWORD(wParam) == LBN_DBLCLK) {
            AcceptSelection(*state);
            return 0;
        }
        if (id == IdLaunch) {
            AcceptSelection(*state);
            return 0;
        }
        if (id == IdManage) {
            const int current = static_cast<int>(SendMessageW(state->list, LB_GETCURSEL, 0, 0));
            std::wstring selected;
            if (current >= 0 && static_cast<size_t>(current) < state->profiles.size()) {
                selected = state->profiles[static_cast<size_t>(current)].id;
            }
            RunProfileManager(state->instance, *state->store, window);
            LoadProfiles(*state, selected);
            return 0;
        }
        if (id == IdCancel) {
            DestroyWindow(window);
            return 0;
        }
        if (id >= IdQuickBase && id < IdQuickBase + 9) {
            const int index = id - IdQuickBase;
            if (static_cast<size_t>(index) < state->profiles.size()) {
                SendMessageW(state->list, LB_SETCURSEL, index, 0);
                UpdateSelectionState(*state);
                AcceptSelection(*state);
            }
            return 0;
        }
        break;
    }
    case WM_MEASUREITEM: {
        auto* measure = reinterpret_cast<MEASUREITEMSTRUCT*>(lParam);
        if (measure->CtlID == IdProfileList) {
            measure->itemHeight = static_cast<UINT>(S(*state, 78));
            return TRUE;
        }
        break;
    }
    case WM_DRAWITEM: {
        const auto* item = reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
        if (item->CtlID == IdProfileList) {
            DrawPickerItem(*state, *item);
            return TRUE;
        }
        break;
    }
    case WM_CTLCOLORSTATIC: {
        HDC dc = reinterpret_cast<HDC>(wParam);
        SetBkMode(dc, OPAQUE);
        SetBkColor(dc, ui::kBackground);
        const int index = static_cast<int>(SendMessageW(state->list, LB_GETCURSEL, 0, 0));
        const bool valid = index >= 0 && static_cast<size_t>(index) < state->profiles.size() &&
                           IsUsableExecutable(state->profiles[static_cast<size_t>(index)].executable);
        SetTextColor(dc, valid ? ui::kSuccess : ui::kMuted);
        return reinterpret_cast<LRESULT>(state->backgroundBrush);
    }
    case WM_CTLCOLORBTN: {
        HDC dc = reinterpret_cast<HDC>(wParam);
        SetBkMode(dc, OPAQUE);
        SetBkColor(dc, ui::kBackground);
        return reinterpret_cast<LRESULT>(state->backgroundBrush);
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        Paint(*state);
        return 0;
    case WM_GETMINMAXINFO: {
        auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
        ui::ApplyMonitorWorkArea(window, *info);
        info->ptMinTrackSize = POINT{S(*state, 620), S(*state, 480)};
        return 0;
    }
    case WM_CLOSE:
        DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        state->done = true;
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

bool RegisterPickerClass(HINSTANCE instance) {
    WNDCLASSEXW windowClass{sizeof(windowClass)};
    windowClass.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    windowClass.lpfnWndProc = WindowProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_APP_ICON));
    windowClass.hIconSm = windowClass.hIcon;
    windowClass.hbrBackground = nullptr;
    windowClass.lpszClassName = kPickerClass;
    return RegisterClassExW(&windowClass) != 0 ||
           GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

} // namespace

ProfilePickerResult RunProfilePicker(HINSTANCE instance, ProfileStore& store,
                                     HWND owner) {
    if (!RegisterPickerClass(instance)) {
        ShowError(L"无法创建 Profile 选择窗口。", owner);
        return {};
    }
    PickerState state{};
    state.instance = instance;
    state.owner = owner;
    state.store = &store;
    const UINT dpi = owner != nullptr ? GetDpiForWindow(owner) : GetDpiForSystem();
    HWND window = CreateWindowExW(
        WS_EX_CONTROLPARENT | WS_EX_APPWINDOW, kPickerClass,
        L"MPVBridge · 选择 Profile",
        WS_POPUP | WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX |
            WS_MAXIMIZEBOX | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT, ui::Scale(720, dpi), ui::Scale(570, dpi),
        owner, nullptr, instance, &state);
    if (window == nullptr) {
        ShowError(L"无法创建 Profile 选择窗口：\n" +
                  FormatSystemError(GetLastError()), owner);
        return {};
    }

    std::vector<ACCEL> accelerators;
    for (int index = 0; index < 9; ++index) {
        accelerators.push_back(
            ACCEL{FVIRTKEY, static_cast<WORD>(L'1' + index),
                  static_cast<WORD>(IdQuickBase + index)});
    }
    accelerators.push_back(ACCEL{FVIRTKEY, VK_RETURN, IdLaunch});
    accelerators.push_back(ACCEL{FVIRTKEY, VK_ESCAPE, IdCancel});
    HACCEL acceleratorTable = CreateAcceleratorTableW(
        accelerators.data(), static_cast<int>(accelerators.size()));

    if (owner != nullptr) EnableWindow(owner, FALSE);
    ui::CenterWindow(window, owner);
    ShowWindow(window, SW_SHOW);
    UpdateWindow(window);
    SetForegroundWindow(window);

    WriteDiagnosticLog(store, L"已显示外部调用 Profile 选择窗口");
    MSG message{};
    while (!state.done && GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (TranslateAcceleratorW(window, acceleratorTable, &message) == 0 &&
            IsDialogMessageW(window, &message) == FALSE) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    if (acceleratorTable != nullptr) DestroyAcceleratorTable(acceleratorTable);
    if (owner != nullptr) {
        EnableWindow(owner, TRUE);
        SetForegroundWindow(owner);
    }
    if (state.font != nullptr) DeleteObject(state.font);
    if (state.smallFont != nullptr) DeleteObject(state.smallFont);
    if (state.titleFont != nullptr) DeleteObject(state.titleFont);
    if (state.nameFont != nullptr) DeleteObject(state.nameFont);
    if (state.backgroundBrush != nullptr) DeleteObject(state.backgroundBrush);
    return state.result;
}
