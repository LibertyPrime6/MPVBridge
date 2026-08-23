#include "ProfileManager.h"

#include "AppCore.h"
#include "EnvironmentManager.h"
#include "Logger.h"
#include "MediaAssociations.h"
#include "ProtocolHandler.h"
#include "resource.h"
#include "UiCommon.h"

#include <commctrl.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iterator>
#include <string>
#include <vector>

namespace {

constexpr wchar_t kManagerClass[] = L"MPVBridge.ProfileManager";
constexpr wchar_t kProfileBoardClass[] = L"MPVBridge.ProfileBoard";
constexpr UINT_PTR kBoardAnimationTimer = 1;
constexpr UINT kSelectProfileMessage = WM_APP + 41;

enum ControlId : int {
    IdProfileList = 100,
    IdNewProfile,
    IdRefresh,
    IdOpenIni,
    IdProfileId,
    IdProfileName,
    IdProfilePath,
    IdBrowse,
    IdSave,
    IdSetDefault,
    IdDelete,
    IdLogging,
    IdOpenLog,
    IdStatus,
    IdMediaAssociations,
    IdEnvironmentManager,
    IdSkipProfilePicker,
    IdAutoLaunchLabel,
    IdAutoLaunchSeconds,
    IdAutoLaunchSuffix,
    IdSaveAutoLaunch,
    IdProtocolStatus,
    IdRegisterProtocol,
    IdUnregisterProtocol,
    IdMoveUp = 180,
    IdMoveDown,
};

struct ManagerState {
    HINSTANCE instance{};
    HWND window{};
    HWND owner{};
    ProfileStore* store{};
    UINT dpi{USER_DEFAULT_SCREEN_DPI};
    std::vector<Profile> profiles;
    std::wstring defaultId;
    int selectedIndex{-1};
    std::wstring originalId;
    bool loading{};
    bool dirty{};
    bool done{};
    HFONT font{};
    HFONT smallFont{};
    HFONT titleFont{};
    HFONT sectionFont{};
    HBRUSH surfaceBrush{};
    ui::ChromeRects chrome{};
    ui::ChromeButton hotChrome{ui::ChromeButton::None};
    ui::ChromeButton pressedChrome{ui::ChromeButton::None};
    bool trackingMouse{};
    bool pointerDown{};
    bool dragging{};
    bool dragAnimating{};
    bool dragWillCommit{};
    POINT dragStart{};
    POINT dragMouse{};
    int dragSource{-1};
    int dragInsertion{-1};
    int dragGrabX{};
    int dragGrabY{};
    int pendingDestination{-1};
    int scrollOffset{};
    float floatingX{};
    float floatingY{};
    float animationFromX{};
    float animationFromY{};
    float animationTargetX{};
    float animationTargetY{};
    ULONGLONG animationStarted{};
    std::vector<float> cardPositions;
    HWND list{};
    HWND idEdit{};
    HWND nameEdit{};
    HWND pathEdit{};
    HWND status{};
    HWND skipProfilePicker{};
    HWND logging{};
    HWND autoLaunchEdit{};
    HWND protocolStatus{};
};

int S(const ManagerState& state, int value) { return ui::Scale(value, state.dpi); }

HWND Control(ManagerState& state, DWORD exStyle, const wchar_t* type,
             const wchar_t* text, DWORD style, int id) {
    HWND control = CreateWindowExW(
        exStyle, type, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0,
        state.window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
        state.instance, nullptr);
    ui::SetFont(control, state.font);
    ui::ApplyControlTheme(control);
    return control;
}

void RecreateFonts(ManagerState& state) {
    if (state.font != nullptr) DeleteObject(state.font);
    if (state.smallFont != nullptr) DeleteObject(state.smallFont);
    if (state.titleFont != nullptr) DeleteObject(state.titleFont);
    if (state.sectionFont != nullptr) DeleteObject(state.sectionFont);
    state.font = ui::CreateFont(state.dpi, 10);
    state.smallFont = ui::CreateFont(state.dpi, 9);
    state.titleFont = ui::CreateFont(state.dpi, 22, FW_SEMIBOLD);
    state.sectionFont = ui::CreateFont(state.dpi, 12, FW_SEMIBOLD);
    EnumChildWindows(state.window,
                     [](HWND child, LPARAM parameter) -> BOOL {
                         ui::SetFont(child, reinterpret_cast<HFONT>(parameter));
                         return TRUE;
                     },
                     reinterpret_cast<LPARAM>(state.font));
    if (state.status != nullptr) ui::SetFont(state.status, state.smallFont);
    if (state.protocolStatus != nullptr) {
        ui::SetFont(state.protocolStatus, state.smallFont);
    }
    if (state.list != nullptr) InvalidateRect(state.list, nullptr, TRUE);
}

void Layout(ManagerState& state) {
    RECT client{};
    GetClientRect(state.window, &client);
    const int margin = S(state, 24);
    const int top = S(state, 126);
    const int bottom = client.bottom - S(state, 24);
    const int leftWidth = std::max(S(state, 270),
                                   (static_cast<int>(client.right) - S(state, 72)) *
                                       34 / 100);
    const int gap = S(state, 22);
    const int rightX = margin + leftWidth + gap;
    const int rightWidth = client.right - rightX - margin;
    state.chrome = ui::GetChromeRects(client, state.dpi);

    SetWindowPos(state.list, nullptr, margin, top + S(state, 40), leftWidth,
                 bottom - top - S(state, 86), SWP_NOZORDER);
    SetWindowPos(GetDlgItem(state.window, IdNewProfile), nullptr, margin, top,
                 S(state, 92), S(state, 30), SWP_NOZORDER);
    SetWindowPos(GetDlgItem(state.window, IdRefresh), nullptr, margin + S(state, 100),
                 top, S(state, 78), S(state, 30), SWP_NOZORDER);
    SetWindowPos(GetDlgItem(state.window, IdOpenIni), nullptr,
                 margin + S(state, 186), top, S(state, 94), S(state, 30),
                 SWP_NOZORDER);

    const int fieldX = rightX + S(state, 22);
    const int fieldWidth = rightWidth - S(state, 44);
    SetWindowPos(state.idEdit, nullptr, fieldX, top + S(state, 50), fieldWidth,
                 S(state, 24), SWP_NOZORDER);
    SetWindowPos(state.nameEdit, nullptr, fieldX, top + S(state, 120), fieldWidth,
                 S(state, 24), SWP_NOZORDER);
    SetWindowPos(state.pathEdit, nullptr, fieldX, top + S(state, 190),
                 fieldWidth - S(state, 92), S(state, 24), SWP_NOZORDER);
    SetWindowPos(GetDlgItem(state.window, IdBrowse), nullptr,
                 fieldX + fieldWidth - S(state, 84), top + S(state, 186),
                 S(state, 84), S(state, 32), SWP_NOZORDER);
    SetWindowPos(state.status, nullptr, fieldX, top + S(state, 224), fieldWidth,
                 S(state, 24), SWP_NOZORDER);

    const int actionY = top + S(state, 268);
    SetWindowPos(GetDlgItem(state.window, IdSave), nullptr, fieldX, actionY,
                 S(state, 100), S(state, 34), SWP_NOZORDER);
    SetWindowPos(GetDlgItem(state.window, IdSetDefault), nullptr,
                 fieldX + S(state, 110), actionY, S(state, 112), S(state, 34),
                 SWP_NOZORDER);
    SetWindowPos(GetDlgItem(state.window, IdDelete), nullptr,
                 fieldX + S(state, 232), actionY, S(state, 84), S(state, 34),
                 SWP_NOZORDER);

    const int integrationY = bottom - S(state, 182);
    SetWindowPos(state.skipProfilePicker, nullptr,
                 fieldX + fieldWidth - S(state, 230), bottom - S(state, 240),
                 S(state, 230), S(state, 28), SWP_NOZORDER);
    SetWindowPos(GetDlgItem(state.window, IdMediaAssociations), nullptr,
                 fieldX, integrationY, S(state, 112), S(state, 32),
                 SWP_NOZORDER);
    SetWindowPos(GetDlgItem(state.window, IdEnvironmentManager), nullptr,
                 fieldX + S(state, 120), integrationY, S(state, 112),
                 S(state, 32), SWP_NOZORDER);
    SetWindowPos(GetDlgItem(state.window, IdAutoLaunchLabel), nullptr,
                 fieldX + S(state, 242), integrationY, S(state, 86),
                 S(state, 32), SWP_NOZORDER);
    SetWindowPos(state.autoLaunchEdit, nullptr,
                 fieldX + S(state, 334), integrationY + S(state, 4),
                 S(state, 42), S(state, 24), SWP_NOZORDER);
    SetWindowPos(GetDlgItem(state.window, IdAutoLaunchSuffix), nullptr,
                 fieldX + S(state, 382), integrationY, S(state, 18),
                 S(state, 32), SWP_NOZORDER);
    SetWindowPos(GetDlgItem(state.window, IdSaveAutoLaunch), nullptr,
                 fieldX + S(state, 408), integrationY, S(state, 58),
                 S(state, 32), SWP_NOZORDER);

    const int protocolY = bottom - S(state, 140);
    SetWindowPos(state.protocolStatus, nullptr, fieldX, protocolY,
                 S(state, 190), S(state, 32), SWP_NOZORDER);
    SetWindowPos(GetDlgItem(state.window, IdRegisterProtocol), nullptr,
                 fieldX + S(state, 204), protocolY, S(state, 136), S(state, 32),
                 SWP_NOZORDER);
    SetWindowPos(GetDlgItem(state.window, IdUnregisterProtocol), nullptr,
                 fieldX + S(state, 350), protocolY, S(state, 136), S(state, 32),
                 SWP_NOZORDER);

    const int settingsY = bottom - S(state, 66);
    SetWindowPos(state.logging, nullptr, fieldX, settingsY, S(state, 190),
                 S(state, 28), SWP_NOZORDER);
    SetWindowPos(GetDlgItem(state.window, IdOpenLog), nullptr,
                 fieldX + S(state, 198), settingsY - S(state, 2), S(state, 90),
                 S(state, 30), SWP_NOZORDER);
}

void UpdatePathStatus(ManagerState& state) {
    const std::filesystem::path path(
        Trim(ui::GetWindowTextString(state.pathEdit)));
    const bool valid = IsUsableExecutable(path);
    ui::SetWindowTextString(
        state.status,
        path.empty() ? L"请选择 mpv.exe" :
        valid ? L"✓ 路径有效，可以启动" : L"⚠ 路径不存在或不是文件");
    InvalidateRect(state.status, nullptr, TRUE);
}

void UpdateProtocolStatus(ManagerState& state) {
    const ProtocolRegistrationStatus status = GetMpvBridgeProtocolStatus();
    const std::wstring label =
        !status.registered
            ? L"mpvbridge:// 未注册"
            : status.ownedByCurrentExecutable
                  ? L"✓ mpvbridge:// 已注册"
                  : L"⚠ mpvbridge:// 已被其他程序注册";
    ui::SetWindowTextString(state.protocolStatus, label);
    EnableWindow(GetDlgItem(state.window, IdUnregisterProtocol),
                 status.ownedByCurrentExecutable ? TRUE : FALSE);
    InvalidateRect(state.protocolStatus, nullptr, TRUE);
}

void PopulateList(ManagerState& state, std::wstring_view selectId = {}) {
    state.loading = true;
    state.profiles = state.store->Load();
    state.defaultId = state.store->DefaultId();
    int selectIndex = -1;
    for (size_t index = 0; index < state.profiles.size(); ++index) {
        if (!selectId.empty() &&
            EqualsInsensitive(state.profiles[index].id, selectId)) {
            selectIndex = static_cast<int>(index);
        }
    }
    if (selectIndex < 0 && !state.profiles.empty()) {
        selectIndex = 0;
    }
    state.selectedIndex = selectIndex;
    const int pitch = S(state, 74);
    if (state.list != nullptr) {
        RECT client{};
        GetClientRect(state.list, &client);
        const int content = S(state, 10) +
                            static_cast<int>(state.profiles.size()) * pitch;
        state.scrollOffset = std::clamp(
            state.scrollOffset, 0,
            std::max(0, content - static_cast<int>(client.bottom)));
        SCROLLINFO scroll{sizeof(scroll), SIF_RANGE | SIF_PAGE | SIF_POS};
        scroll.nMin = 0;
        scroll.nMax = std::max(0, content - 1);
        scroll.nPage = static_cast<UINT>(std::max<LONG>(0, client.bottom));
        scroll.nPos = state.scrollOffset;
        SetScrollInfo(state.list, SB_VERT, &scroll, TRUE);
    }
    state.cardPositions.resize(state.profiles.size());
    const int top = S(state, 5) - state.scrollOffset;
    for (size_t index = 0; index < state.cardPositions.size(); ++index) {
        state.cardPositions[index] =
            static_cast<float>(top + static_cast<int>(index) * pitch);
    }
    if (state.list != nullptr) InvalidateRect(state.list, nullptr, TRUE);
    state.loading = false;
}

bool MoveProfileTo(ManagerState& state, int from, int to) {
    if (from < 0 || to < 0 || from == to ||
        static_cast<size_t>(from) >= state.profiles.size() ||
        static_cast<size_t>(to) >= state.profiles.size()) {
        return false;
    }
    Profile moved = state.profiles[static_cast<size_t>(from)];
    state.profiles.erase(state.profiles.begin() + from);
    state.profiles.insert(state.profiles.begin() + to, std::move(moved));
    std::vector<std::wstring> ids;
    ids.reserve(state.profiles.size());
    for (const Profile& profile : state.profiles) ids.push_back(profile.id);
    std::wstring error;
    if (!state.store->SaveOrder(ids, error)) {
        ShowError(L"无法保存 Profile 顺序：\n" + error, state.window);
        PopulateList(state, state.originalId);
        return false;
    }
    const std::wstring selectedId = state.profiles[static_cast<size_t>(to)].id;
    WriteDiagnosticLog(*state.store, L"已调整 Profile 顺序：" + selectedId +
                                         L" → " + std::to_wstring(to + 1));
    PopulateList(state, selectedId);
    state.selectedIndex = to;
    return true;
}

int BoardPitch(const ManagerState& state) { return S(state, 74); }
int BoardCardHeight(const ManagerState& state) { return S(state, 66); }
int BoardPadding(const ManagerState& state) { return S(state, 5); }

void DrawProfileName(ManagerState& state, HDC dc, const Profile& profile,
                     RECT name) {
    if (EqualsInsensitive(profile.id, state.defaultId)) {
        const int badgeWidth = S(state, 44);
        const int badgeGap = S(state, 8);
        RECT badge{name.right - badgeWidth, name.top + S(state, 1), name.right,
                   name.bottom - S(state, 1)};
        name.right = std::max(name.left, badge.left - badgeGap);
        ui::FillRounded(dc, badge, S(state, 7), ui::kAccent);
        ui::DrawTextLine(dc, L"默认", badge, state.smallFont,
                         RGB(255, 255, 255),
                         DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    }
    ui::DrawTextLine(dc, profile.name, name, state.font, ui::kText,
                     DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS | DT_VCENTER);
}

bool DragVisualActive(const ManagerState& state) {
    return state.dragging || state.dragAnimating;
}

void UpdateBoardScroll(ManagerState& state) {
    if (state.list == nullptr) return;
    RECT client{};
    GetClientRect(state.list, &client);
    const int content = S(state, 10) +
                        static_cast<int>(state.profiles.size()) * BoardPitch(state);
    state.scrollOffset = std::clamp(
        state.scrollOffset, 0,
        std::max(0, content - static_cast<int>(client.bottom)));
    SCROLLINFO scroll{sizeof(scroll), SIF_RANGE | SIF_PAGE | SIF_POS};
    scroll.nMin = 0;
    scroll.nMax = std::max(0, content - 1);
    scroll.nPage = static_cast<UINT>(std::max<LONG>(0, client.bottom));
    scroll.nPos = state.scrollOffset;
    SetScrollInfo(state.list, SB_VERT, &scroll, TRUE);
}

int BoardItemAt(const ManagerState& state, POINT point) {
    const int pitch = BoardPitch(state);
    const int logicalY = point.y + state.scrollOffset - BoardPadding(state);
    if (logicalY < 0) return -1;
    const int index = logicalY / pitch;
    if (index < 0 || static_cast<size_t>(index) >= state.profiles.size() ||
        logicalY % pitch >= BoardCardHeight(state)) {
        return -1;
    }
    return index;
}

std::vector<float> BoardTargetPositions(const ManagerState& state) {
    std::vector<float> targets(state.profiles.size());
    const int top = BoardPadding(state) - state.scrollOffset;
    const int pitch = BoardPitch(state);
    if (!DragVisualActive(state) || state.dragSource < 0) {
        for (size_t index = 0; index < targets.size(); ++index) {
            targets[index] = static_cast<float>(top + static_cast<int>(index) * pitch);
        }
        return targets;
    }

    int remainingSlot = 0;
    for (size_t index = 0; index < targets.size(); ++index) {
        if (static_cast<int>(index) == state.dragSource) continue;
        int slot = remainingSlot;
        if (state.dragInsertion >= 0 && slot >= state.dragInsertion) ++slot;
        targets[index] = static_cast<float>(top + slot * pitch);
        ++remainingSlot;
    }
    return targets;
}

void DrawBoardCard(ManagerState& state, HDC dc, int index, RECT card,
                   bool floating) {
    if (index < 0 || static_cast<size_t>(index) >= state.profiles.size()) return;
    const Profile& profile = state.profiles[static_cast<size_t>(index)];
    const bool selected = index == state.selectedIndex;
    const int border = selected || floating ? S(state, 2) : S(state, 1);
    if (floating) {
        RECT shadow = card;
        OffsetRect(&shadow, S(state, 3), S(state, 5));
        ui::FillRounded(dc, shadow, S(state, 13), RGB(205, 213, 226));
    }
    ui::FillRounded(dc, card, S(state, 12),
                    selected || floating ? ui::kAccent : ui::kBorder);
    RECT inner = card;
    InflateRect(&inner, -border, -border);
    ui::FillRounded(dc, inner, std::max(1, S(state, 12) - border),
                    floating ? ui::kAccentSoft : ui::kSurface);

    const int dotSize = S(state, 9);
    RECT dot{card.left + S(state, 16), card.top + S(state, 18),
             card.left + S(state, 16) + dotSize,
             card.top + S(state, 18) + dotSize};
    const COLORREF dotColor = IsUsableExecutable(profile.executable)
                                  ? ui::kSuccess
                                  : ui::kDanger;
    HBRUSH dotBrush = CreateSolidBrush(dotColor);
    HPEN dotPen = CreatePen(PS_SOLID, 1, dotColor);
    HGDIOBJ oldBrush = SelectObject(dc, dotBrush);
    HGDIOBJ oldPen = SelectObject(dc, dotPen);
    Ellipse(dc, dot.left, dot.top, dot.right, dot.bottom);
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
    DeleteObject(dotPen);
    DeleteObject(dotBrush);

    RECT name{card.left + S(state, 40), card.top + S(state, 10),
              card.right - S(state, 14), card.top + S(state, 34)};
    DrawProfileName(state, dc, profile, name);
    RECT idRect{card.left + S(state, 40), card.top + S(state, 35),
                card.right - S(state, 14), card.bottom - S(state, 7)};
    ui::DrawTextLine(dc, profile.id, idRect, state.smallFont, ui::kMuted,
                     DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS | DT_VCENTER);
}

void PaintProfileBoard(ManagerState& state, HWND board) {
    PAINTSTRUCT paint{};
    HDC target = BeginPaint(board, &paint);
    RECT client{};
    GetClientRect(board, &client);
    HDC dc = CreateCompatibleDC(target);
    HBITMAP bitmap = CreateCompatibleBitmap(target, std::max<LONG>(1, client.right),
                                             std::max<LONG>(1, client.bottom));
    HGDIOBJ oldBitmap = SelectObject(dc, bitmap);
    ui::FillSolid(dc, client, ui::kSurface);

    if (state.cardPositions.size() != state.profiles.size()) {
        state.cardPositions = BoardTargetPositions(state);
    }
    const int cardHeight = BoardCardHeight(state);
    const int horizontalPadding = BoardPadding(state);
    for (size_t index = 0; index < state.profiles.size(); ++index) {
        if (DragVisualActive(state) && static_cast<int>(index) == state.dragSource) {
            continue;
        }
        const int y = static_cast<int>(std::lround(state.cardPositions[index]));
        RECT card{horizontalPadding, y, client.right - horizontalPadding,
                  y + cardHeight};
        if (card.bottom >= 0 && card.top <= client.bottom) {
            DrawBoardCard(state, dc, static_cast<int>(index), card, false);
        }
    }
    if (DragVisualActive(state) && state.dragSource >= 0) {
        RECT floating{static_cast<int>(std::lround(state.floatingX)),
                      static_cast<int>(std::lround(state.floatingY)), 0, 0};
        floating.right = floating.left + client.right - horizontalPadding * 2;
        floating.bottom = floating.top + cardHeight;
        DrawBoardCard(state, dc, state.dragSource, floating, true);
    }
    BitBlt(target, paint.rcPaint.left, paint.rcPaint.top,
           paint.rcPaint.right - paint.rcPaint.left,
           paint.rcPaint.bottom - paint.rcPaint.top, dc,
           paint.rcPaint.left, paint.rcPaint.top, SRCCOPY);
    SelectObject(dc, oldBitmap);
    DeleteObject(bitmap);
    DeleteDC(dc);
    EndPaint(board, &paint);
}

int FindDragInsertion(const ManagerState& state, HWND board) {
    if (!state.dragging || state.dragSource < 0) return -1;
    RECT client{};
    GetClientRect(board, &client);
    const int allowance = S(state, 18);
    if (state.dragMouse.x < -allowance || state.dragMouse.x > client.right + allowance ||
        state.dragMouse.y < -allowance || state.dragMouse.y > client.bottom + allowance) {
        return -1;
    }
    const int remainingCount = static_cast<int>(state.profiles.size()) - 1;
    const float floatingCenter = state.floatingY + BoardCardHeight(state) / 2.0F;
    const int top = BoardPadding(state) - state.scrollOffset;
    const int pitch = BoardPitch(state);
    int closest = -1;
    float closestDistance = static_cast<float>(S(state, 30));
    for (int slot = 0; slot <= remainingCount; ++slot) {
        const float targetCenter =
            static_cast<float>(top + slot * pitch + BoardCardHeight(state) / 2);
        const float distance = std::abs(floatingCenter - targetCenter);
        if (distance <= closestDistance) {
            closestDistance = distance;
            closest = slot;
        }
    }
    return closest;
}

void BeginDropAnimation(ManagerState& state, HWND board, bool commit) {
    state.pointerDown = false;
    state.dragging = false;
    state.dragAnimating = true;
    state.dragWillCommit = commit;
    state.pendingDestination = commit ? state.dragInsertion : state.dragSource;
    state.dragInsertion = state.pendingDestination;
    state.animationFromX = state.floatingX;
    state.animationFromY = state.floatingY;
    state.animationTargetX = static_cast<float>(BoardPadding(state));
    state.animationTargetY = static_cast<float>(
        BoardPadding(state) - state.scrollOffset +
        state.pendingDestination * BoardPitch(state));
    state.animationStarted = GetTickCount64();
    SetTimer(board, kBoardAnimationTimer, 16, nullptr);
    if (GetCapture() == board) ReleaseCapture();
}

void TickBoardAnimation(ManagerState& state, HWND board) {
    if (state.dragging) {
        RECT client{};
        GetClientRect(board, &client);
        const int edge = S(state, 34);
        const int maxScroll = std::max(
            0, S(state, 10) + static_cast<int>(state.profiles.size()) *
                                   BoardPitch(state) -
                   static_cast<int>(client.bottom));
        int delta = 0;
        if (state.dragMouse.y < edge) delta = -S(state, 6);
        if (state.dragMouse.y > client.bottom - edge) delta = S(state, 6);
        const int nextScroll = std::clamp(state.scrollOffset + delta, 0, maxScroll);
        if (nextScroll != state.scrollOffset) {
            state.scrollOffset = nextScroll;
            state.dragInsertion = FindDragInsertion(state, board);
            UpdateBoardScroll(state);
        }
    }

    const std::vector<float> targets = BoardTargetPositions(state);
    if (state.cardPositions.size() != targets.size()) state.cardPositions = targets;
    bool moving = false;
    for (size_t index = 0; index < targets.size(); ++index) {
        if (DragVisualActive(state) && static_cast<int>(index) == state.dragSource) continue;
        const float difference = targets[index] - state.cardPositions[index];
        if (std::abs(difference) > 0.4F) {
            state.cardPositions[index] += difference * 0.28F;
            moving = true;
        } else {
            state.cardPositions[index] = targets[index];
        }
    }

    if (state.dragAnimating) {
        const float elapsed = static_cast<float>(GetTickCount64() - state.animationStarted);
        const float t = std::clamp(elapsed / 150.0F, 0.0F, 1.0F);
        const float eased = 1.0F - (1.0F - t) * (1.0F - t) * (1.0F - t);
        state.floatingX = state.animationFromX +
                          (state.animationTargetX - state.animationFromX) * eased;
        state.floatingY = state.animationFromY +
                          (state.animationTargetY - state.animationFromY) * eased;
        if (t >= 1.0F) {
            const int source = state.dragSource;
            const int destination = state.pendingDestination;
            const bool commit = state.dragWillCommit && source != destination;
            state.dragAnimating = false;
            state.dragWillCommit = false;
            state.dragSource = -1;
            state.dragInsertion = -1;
            state.pendingDestination = -1;
            KillTimer(board, kBoardAnimationTimer);
            if (commit) {
                MoveProfileTo(state, source, destination);
            } else {
                state.cardPositions = BoardTargetPositions(state);
            }
        }
    } else if (!state.dragging && !moving) {
        KillTimer(board, kBoardAnimationTimer);
    }
    InvalidateRect(board, nullptr, FALSE);
}

LRESULT CALLBACK ProfileBoardProc(HWND board, UINT message, WPARAM wParam,
                                  LPARAM lParam) {
    auto* state = reinterpret_cast<ManagerState*>(
        GetWindowLongPtrW(board, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        state = static_cast<ManagerState*>(create->lpCreateParams);
        SetWindowLongPtrW(board, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }
    if (state == nullptr) return DefWindowProcW(board, message, wParam, lParam);

    switch (message) {
    case WM_SIZE:
        UpdateBoardScroll(*state);
        state->cardPositions = BoardTargetPositions(*state);
        InvalidateRect(board, nullptr, TRUE);
        return 0;
    case WM_PAINT:
        PaintProfileBoard(*state, board);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_LBUTTONDOWN: {
        const POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        const int index = BoardItemAt(*state, point);
        if (index < 0 || state->dragAnimating) return 0;
        if (SendMessageW(state->window, kSelectProfileMessage,
                         static_cast<WPARAM>(index), 0) == FALSE) {
            return 0;
        }
        SetFocus(board);
        state->pointerDown = true;
        state->dragging = false;
        state->dragStart = point;
        state->dragMouse = point;
        state->dragSource = index;
        const int cardTop = BoardPadding(*state) - state->scrollOffset +
                            index * BoardPitch(*state);
        state->dragGrabX = point.x - BoardPadding(*state);
        state->dragGrabY = point.y - cardTop;
        state->floatingX = static_cast<float>(BoardPadding(*state));
        state->floatingY = static_cast<float>(cardTop);
        SetCapture(board);
        return 0;
    }
    case WM_MOUSEMOVE:
        if (state->pointerDown && (wParam & MK_LBUTTON) != 0) {
            const POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            state->dragMouse = point;
            if (!state->dragging &&
                (std::abs(point.x - state->dragStart.x) >= GetSystemMetrics(SM_CXDRAG) ||
                 std::abs(point.y - state->dragStart.y) >= GetSystemMetrics(SM_CYDRAG))) {
                state->dragging = true;
                state->dragInsertion = state->dragSource;
                SetTimer(board, kBoardAnimationTimer, 16, nullptr);
            }
            if (state->dragging) {
                const int horizontalTravel = std::clamp(
                    static_cast<int>(point.x) - state->dragGrabX,
                    -S(*state, 16), S(*state, 24));
                state->floatingX = static_cast<float>(horizontalTravel);
                state->floatingY = static_cast<float>(point.y - state->dragGrabY);
                state->dragInsertion = FindDragInsertion(*state, board);
                InvalidateRect(board, nullptr, FALSE);
                SetCursor(LoadCursorW(nullptr, IDC_SIZEALL));
            }
            return 0;
        }
        break;
    case WM_LBUTTONUP:
        if (!state->pointerDown) return 0;
        if (state->dragging) {
            BeginDropAnimation(*state, board, state->dragInsertion >= 0);
        } else {
            state->pointerDown = false;
            state->dragSource = -1;
            if (GetCapture() == board) ReleaseCapture();
        }
        return 0;
    case WM_CAPTURECHANGED:
        if (state->pointerDown && reinterpret_cast<HWND>(lParam) != board) {
            if (state->dragging) BeginDropAnimation(*state, board, false);
            else {
                state->pointerDown = false;
                state->dragSource = -1;
            }
        }
        return 0;
    case WM_CANCELMODE:
    case WM_KEYDOWN:
        if (message == WM_KEYDOWN && wParam != VK_ESCAPE) {
            if (state->profiles.empty()) return 0;
            int next = state->selectedIndex;
            if (wParam == VK_UP) --next;
            else if (wParam == VK_DOWN) ++next;
            else return DefWindowProcW(board, message, wParam, lParam);
            next = std::clamp(next, 0,
                              static_cast<int>(state->profiles.size()) - 1);
            SendMessageW(state->window, kSelectProfileMessage,
                         static_cast<WPARAM>(next), 0);
            return 0;
        }
        if (state->dragging) BeginDropAnimation(*state, board, false);
        else {
            state->pointerDown = false;
            state->dragSource = -1;
            if (GetCapture() == board) ReleaseCapture();
        }
        return 0;
    case WM_MOUSEWHEEL: {
        RECT client{};
        GetClientRect(board, &client);
        const int maxScroll = std::max(
            0, S(*state, 10) + static_cast<int>(state->profiles.size()) *
                                   BoardPitch(*state) -
                   static_cast<int>(client.bottom));
        state->scrollOffset = std::clamp(
            state->scrollOffset - GET_WHEEL_DELTA_WPARAM(wParam) /
                                      WHEEL_DELTA * BoardPitch(*state),
            0, maxScroll);
        UpdateBoardScroll(*state);
        state->cardPositions = BoardTargetPositions(*state);
        InvalidateRect(board, nullptr, FALSE);
        return 0;
    }
    case WM_VSCROLL: {
        SCROLLINFO scroll{sizeof(scroll), SIF_ALL};
        GetScrollInfo(board, SB_VERT, &scroll);
        int next = state->scrollOffset;
        switch (LOWORD(wParam)) {
        case SB_LINEUP: next -= BoardPitch(*state); break;
        case SB_LINEDOWN: next += BoardPitch(*state); break;
        case SB_PAGEUP: next -= static_cast<int>(scroll.nPage); break;
        case SB_PAGEDOWN: next += static_cast<int>(scroll.nPage); break;
        case SB_THUMBTRACK: next = scroll.nTrackPos; break;
        default: return 0;
        }
        state->scrollOffset = std::clamp(next, scroll.nMin,
                                         std::max(scroll.nMin,
                                                  scroll.nMax -
                                                      static_cast<int>(scroll.nPage) + 1));
        UpdateBoardScroll(*state);
        state->cardPositions = BoardTargetPositions(*state);
        InvalidateRect(board, nullptr, FALSE);
        return 0;
    }
    case WM_TIMER:
        if (wParam == kBoardAnimationTimer) {
            TickBoardAnimation(*state, board);
            return 0;
        }
        break;
    case WM_SETCURSOR:
        if (state->dragging || state->dragAnimating) {
            SetCursor(LoadCursorW(nullptr, IDC_SIZEALL));
            return TRUE;
        }
        SetCursor(LoadCursorW(nullptr, IDC_HAND));
        return TRUE;
    }
    return DefWindowProcW(board, message, wParam, lParam);
}

void LoadEditor(ManagerState& state, int index) {
    state.loading = true;
    state.selectedIndex = index;
    if (index >= 0 && static_cast<size_t>(index) < state.profiles.size()) {
        const Profile& profile = state.profiles[static_cast<size_t>(index)];
        state.originalId = profile.id;
        ui::SetWindowTextString(state.idEdit, profile.id);
        ui::SetWindowTextString(state.nameEdit, profile.name);
        ui::SetWindowTextString(state.pathEdit, profile.executable.wstring());
    } else {
        state.originalId.clear();
        ui::SetWindowTextString(state.idEdit, L"");
        ui::SetWindowTextString(state.nameEdit, L"");
        ui::SetWindowTextString(state.pathEdit, L"");
    }
    state.dirty = false;
    state.loading = false;
    UpdatePathStatus(state);
    InvalidateRect(state.window, nullptr, FALSE);
}

bool ConfirmDiscard(ManagerState& state) {
    if (!state.dirty) {
        return true;
    }
    return MessageBoxW(state.window,
                       L"当前修改尚未保存。要放弃这些修改吗？",
                       kAppTitle, MB_YESNO | MB_ICONQUESTION) == IDYES;
}

bool IsValidProfileId(std::wstring_view id) {
    return !id.empty() && !EqualsInsensitive(id, L"General") &&
           id.find_first_of(L"[]\r\n") == std::wstring_view::npos;
}

bool SaveEditor(ManagerState& state) {
    Profile profile;
    profile.id = Trim(ui::GetWindowTextString(state.idEdit));
    profile.name = Trim(ui::GetWindowTextString(state.nameEdit));
    profile.executable =
        std::filesystem::path(Trim(ui::GetWindowTextString(state.pathEdit)));

    if (!IsValidProfileId(profile.id)) {
        ShowError(L"Profile ID 不能为空，不能使用 General，也不能包含 [ ] 或换行。",
                  state.window);
        SetFocus(state.idEdit);
        return false;
    }
    if (profile.name.empty()) {
        ShowError(L"请填写便于识别的 Profile 名称。", state.window);
        SetFocus(state.nameEdit);
        return false;
    }
    if (!IsUsableExecutable(profile.executable)) {
        ShowError(L"请选择一个实际存在的 mpv.exe 文件。", state.window);
        SetFocus(state.pathEdit);
        return false;
    }
    for (const Profile& existing : state.profiles) {
        if (EqualsInsensitive(existing.id, profile.id) &&
            !EqualsInsensitive(existing.id, state.originalId)) {
            ShowError(L"该 Profile ID 已存在，请使用其他 ID。", state.window);
            SetFocus(state.idEdit);
            return false;
        }
    }
    std::wstring error;
    if (!state.store->Save(profile, state.originalId, error)) {
        ShowError(L"保存 Profile 失败：\n" + error, state.window);
        return false;
    }
    WriteDiagnosticLog(*state.store, L"已保存 Profile：" + profile.id);
    state.dirty = false;
    PopulateList(state, profile.id);
    LoadEditor(state, state.selectedIndex);
    return true;
}

void CreateControls(ManagerState& state) {
    state.list = CreateWindowExW(
        0, kProfileBoardClass, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                                            WS_VSCROLL | WS_CLIPSIBLINGS,
        0, 0, 0, 0, state.window,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IdProfileList)),
        state.instance, &state);
    Control(state, 0, L"BUTTON", L"＋ 新建", WS_TABSTOP, IdNewProfile);
    Control(state, 0, L"BUTTON", L"刷新", WS_TABSTOP, IdRefresh);
    Control(state, 0, L"BUTTON", L"编辑 INI", WS_TABSTOP, IdOpenIni);
    state.idEdit = Control(state, 0, L"EDIT", L"",
                           WS_TABSTOP | ES_AUTOHSCROLL, IdProfileId);
    state.nameEdit = Control(state, 0, L"EDIT", L"",
                             WS_TABSTOP | ES_AUTOHSCROLL, IdProfileName);
    state.pathEdit = Control(state, 0, L"EDIT", L"",
                             WS_TABSTOP | ES_AUTOHSCROLL, IdProfilePath);
    Control(state, 0, L"BUTTON", L"浏览…", WS_TABSTOP, IdBrowse);
    Control(state, 0, L"BUTTON", L"保存修改",
            WS_TABSTOP | BS_DEFPUSHBUTTON, IdSave);
    Control(state, 0, L"BUTTON", L"设为默认", WS_TABSTOP, IdSetDefault);
    Control(state, 0, L"BUTTON", L"删除", WS_TABSTOP, IdDelete);
    state.status = Control(state, 0, L"STATIC", L"请选择 mpv.exe",
                           SS_LEFT, IdStatus);
    state.logging = Control(state, 0, L"BUTTON", L"启用诊断日志",
                            WS_TABSTOP | BS_AUTOCHECKBOX, IdLogging);
    Control(state, 0, L"BUTTON", L"打开日志", WS_TABSTOP, IdOpenLog);
    Control(state, 0, L"BUTTON", L"媒体文件关联…", WS_TABSTOP,
            IdMediaAssociations);
    Control(state, 0, L"BUTTON", L"运行环境检测…", WS_TABSTOP,
            IdEnvironmentManager);
    state.skipProfilePicker = Control(
        state, 0, L"BUTTON", L"多 Profile 时直接使用默认项",
        WS_TABSTOP | BS_AUTOCHECKBOX, IdSkipProfilePicker);
    Control(state, 0, L"STATIC", L"默认自动进入", SS_LEFT,
            IdAutoLaunchLabel);
    state.autoLaunchEdit = Control(
        state, 0, L"EDIT", L"", WS_TABSTOP | ES_NUMBER | ES_CENTER |
                                      ES_AUTOHSCROLL,
        IdAutoLaunchSeconds);
    Control(state, 0, L"STATIC", L"秒", SS_LEFT, IdAutoLaunchSuffix);
    Control(state, 0, L"BUTTON", L"保存", WS_TABSTOP, IdSaveAutoLaunch);
    state.protocolStatus = Control(state, 0, L"STATIC", L"mpvbridge:// 未注册",
                                   SS_LEFT | SS_CENTERIMAGE, IdProtocolStatus);
    Control(state, 0, L"BUTTON", L"注册 mpvbridge://", WS_TABSTOP,
            IdRegisterProtocol);
    Control(state, 0, L"BUTTON", L"注销 mpvbridge://", WS_TABSTOP,
            IdUnregisterProtocol);
    ui::StyleButton(GetDlgItem(state.window, IdNewProfile), ui::ButtonStyle::Primary,
                    ui::kBackground);
    ui::StyleButton(GetDlgItem(state.window, IdRefresh), ui::ButtonStyle::Secondary,
                    ui::kBackground);
    ui::StyleButton(GetDlgItem(state.window, IdOpenIni), ui::ButtonStyle::Secondary,
                    ui::kBackground);
    ui::StyleButton(GetDlgItem(state.window, IdBrowse), ui::ButtonStyle::Secondary);
    ui::StyleButton(GetDlgItem(state.window, IdSave), ui::ButtonStyle::Primary);
    ui::StyleButton(GetDlgItem(state.window, IdSetDefault), ui::ButtonStyle::Secondary);
    ui::StyleButton(GetDlgItem(state.window, IdDelete), ui::ButtonStyle::Danger);
    ui::StyleButton(state.logging, ui::ButtonStyle::Toggle);
    ui::StyleButton(GetDlgItem(state.window, IdOpenLog), ui::ButtonStyle::Secondary);
    ui::StyleButton(GetDlgItem(state.window, IdMediaAssociations),
                    ui::ButtonStyle::Secondary);
    ui::StyleButton(GetDlgItem(state.window, IdEnvironmentManager),
                    ui::ButtonStyle::Secondary);
    ui::StyleButton(state.skipProfilePicker, ui::ButtonStyle::Toggle);
    ui::StyleButton(GetDlgItem(state.window, IdSaveAutoLaunch),
                     ui::ButtonStyle::Primary);
    ui::StyleButton(GetDlgItem(state.window, IdRegisterProtocol),
                    ui::ButtonStyle::Primary);
    ui::StyleButton(GetDlgItem(state.window, IdUnregisterProtocol),
                    ui::ButtonStyle::Danger);
    ui::StyleEdit(state.idEdit);
    ui::StyleEdit(state.nameEdit);
    ui::StyleEdit(state.pathEdit);
    ui::StyleEdit(state.autoLaunchEdit);
    SendMessageW(state.autoLaunchEdit, EM_SETLIMITTEXT, 4, 0);
    ui::SetWindowTextString(
        state.autoLaunchEdit,
        std::to_wstring(state.store->AutoLaunchSeconds()));
    SendMessageW(state.logging, BM_SETCHECK,
                  state.store->LoggingEnabled() ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(state.skipProfilePicker, BM_SETCHECK,
                  state.store->SkipProfilePicker() ? BST_CHECKED : BST_UNCHECKED,
                  0);
    UpdateProtocolStatus(state);
    RecreateFonts(state);
}

void DrawManagerListItem(ManagerState& state, const DRAWITEMSTRUCT& item) {
    if (item.itemID == static_cast<UINT>(-1) ||
        item.itemID >= state.profiles.size()) {
        return;
    }
    const Profile& profile = state.profiles[item.itemID];
    const bool selected = (item.itemState & ODS_SELECTED) != 0;
    ui::FillSolid(item.hDC, item.rcItem, ui::kSurface);
    RECT row = item.rcItem;
    InflateRect(&row, -S(state, 5), -S(state, 4));
    ui::FillRounded(item.hDC, row, S(state, 10),
                    selected ? ui::kAccentSoft : ui::kSurface,
                    selected ? RGB(185, 207, 247) : ui::kBorder);
    const int dotSize = S(state, 9);
    RECT dot{row.left + S(state, 14), row.top + S(state, 16),
             row.left + S(state, 14) + dotSize, row.top + S(state, 16) + dotSize};
    HBRUSH dotBrush = CreateSolidBrush(IsUsableExecutable(profile.executable)
                                           ? ui::kSuccess
                                           : ui::kDanger);
    HPEN dotPen = CreatePen(PS_SOLID, 1, IsUsableExecutable(profile.executable)
                                             ? ui::kSuccess
                                             : ui::kDanger);
    HGDIOBJ oldBrush = SelectObject(item.hDC, dotBrush);
    HGDIOBJ oldPen = SelectObject(item.hDC, dotPen);
    Ellipse(item.hDC, dot.left, dot.top, dot.right, dot.bottom);
    SelectObject(item.hDC, oldPen);
    SelectObject(item.hDC, oldBrush);
    DeleteObject(dotPen);
    DeleteObject(dotBrush);

    RECT name{row.left + S(state, 34), row.top + S(state, 9),
              row.right - S(state, 10), row.top + S(state, 31)};
    DrawProfileName(state, item.hDC, profile, name);
    RECT idRect{row.left + S(state, 34), row.top + S(state, 32),
                row.right - S(state, 10), row.bottom - S(state, 6)};
    ui::DrawTextLine(item.hDC, profile.id, idRect, state.smallFont, ui::kMuted,
                     DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS | DT_VCENTER);
    if (state.dragging) {
        HPEN pen = CreatePen(PS_SOLID, S(state, 3), ui::kAccent);
        HGDIOBJ insertionOldPen = SelectObject(item.hDC, pen);
        if (state.dragInsertion == static_cast<int>(item.itemID)) {
            MoveToEx(item.hDC, row.left + S(state, 5), item.rcItem.top, nullptr);
            LineTo(item.hDC, row.right - S(state, 5), item.rcItem.top);
        }
        if (state.dragInsertion == static_cast<int>(state.profiles.size()) &&
            item.itemID + 1 == state.profiles.size()) {
            MoveToEx(item.hDC, row.left + S(state, 5), item.rcItem.bottom - 1, nullptr);
            LineTo(item.hDC, row.right - S(state, 5), item.rcItem.bottom - 1);
        }
        SelectObject(item.hDC, insertionOldPen);
        DeleteObject(pen);
    }
}

void Paint(ManagerState& state) {
    PAINTSTRUCT paint{};
    HDC target = BeginPaint(state.window, &paint);
    RECT client{};
    GetClientRect(state.window, &client);
    HDC dc = CreateCompatibleDC(target);
    HBITMAP bitmap = CreateCompatibleBitmap(target, std::max<LONG>(1, client.right),
                                             std::max<LONG>(1, client.bottom));
    HGDIOBJ oldBitmap = SelectObject(dc, bitmap);
    ui::FillSolid(dc, client, ui::kBackground);
    RECT header{0, 0, client.right, S(state, 96)};
    ui::FillSolid(dc, header, ui::kHeader);
    RECT accent{0, S(state, 92), client.right, S(state, 96)};
    ui::FillSolid(dc, accent, ui::kAccent);

    RECT title{S(state, 28), S(state, 17), client.right - S(state, 28), S(state, 54)};
    ui::DrawTextLine(dc, L"Profile 管理", title, state.titleFont, RGB(255, 255, 255),
                     DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    RECT subtitle{S(state, 30), S(state, 53), client.right - S(state, 28), S(state, 79)};
    ui::DrawTextLine(dc, L"为不同 MPV 整合包配置固定、可靠的播放环境", subtitle,
                     state.font, RGB(190, 202, 222),
                     DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    ui::DrawChromeButtons(dc, state.chrome, state.hotChrome, state.pressedChrome,
                          IsZoomed(state.window) != FALSE, state.dpi);

    RECT clientArea{};
    GetClientRect(state.window, &clientArea);
    const int margin = S(state, 24);
    const int top = S(state, 126);
    const int leftWidth = std::max(
        S(state, 270),
        (static_cast<int>(clientArea.right) - S(state, 72)) * 34 / 100);
    const int rightX = margin + leftWidth + S(state, 22);
    RECT card{rightX, S(state, 112), clientArea.right - margin,
              clientArea.bottom - margin};
    ui::FillRounded(dc, card, S(state, 14), ui::kSurface, ui::kBorder);

    RECT section{rightX + S(state, 22), top - S(state, 6), card.right - S(state, 22),
                 top + S(state, 24)};
    ui::DrawTextLine(dc,
                     state.originalId.empty() ? L"创建新 Profile" : L"Profile 详情",
                     section, state.sectionFont, ui::kText,
                     DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    const int x = rightX + S(state, 22);
    const int width = card.right - x - S(state, 22);
    RECT label{x, top + S(state, 22), x + width, top + S(state, 45)};
    ui::DrawTextLine(dc, L"Profile ID", label, state.smallFont, ui::kMuted,
                     DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    OffsetRect(&label, 0, S(state, 70));
    ui::DrawTextLine(dc, L"显示名称", label, state.smallFont, ui::kMuted,
                     DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    OffsetRect(&label, 0, S(state, 70));
    ui::DrawTextLine(dc, L"mpv.exe 路径", label, state.smallFont, ui::kMuted,
                     DT_LEFT | DT_SINGLELINE | DT_VCENTER);

    RECT settings{x, clientArea.bottom - S(state, 120), x + width,
                  clientArea.bottom - S(state, 93)};
    ui::DrawTextLine(dc, L"诊断与日志", settings, state.sectionFont, ui::kText,
                     DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    RECT integration{x, clientArea.bottom - S(state, 236), x + width,
                      clientArea.bottom - S(state, 209)};
    ui::DrawTextLine(dc, L"系统集成", integration, state.sectionFont,
                     ui::kText, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    ui::DrawRoundedEditFrame(dc, state.window, state.idEdit, state.dpi);
    ui::DrawRoundedEditFrame(dc, state.window, state.nameEdit, state.dpi);
    ui::DrawRoundedEditFrame(dc, state.window, state.pathEdit, state.dpi);
    ui::DrawRoundedEditFrame(dc, state.window, state.autoLaunchEdit, state.dpi);
    BitBlt(target, paint.rcPaint.left, paint.rcPaint.top,
           paint.rcPaint.right - paint.rcPaint.left,
           paint.rcPaint.bottom - paint.rcPaint.top, dc,
           paint.rcPaint.left, paint.rcPaint.top, SRCCOPY);
    SelectObject(dc, oldBitmap);
    DeleteObject(bitmap);
    DeleteDC(dc);
    EndPaint(state.window, &paint);
}

LRESULT HandleCommand(ManagerState& state, WPARAM wParam, LPARAM lParam) {
    const int id = LOWORD(wParam);
    const int notification = HIWORD(wParam);
    if ((id == IdProfileId || id == IdProfileName || id == IdProfilePath) &&
        notification == EN_CHANGE && !state.loading) {
        state.dirty = true;
        if (id == IdProfilePath) UpdatePathStatus(state);
        return 0;
    }
    switch (id) {
    case IdNewProfile:
        if (ConfirmDiscard(state)) {
            LoadEditor(state, -1);
            InvalidateRect(state.list, nullptr, FALSE);
            ui::SetWindowTextString(state.idEdit, L"New_Profile");
            ui::SetWindowTextString(state.nameEdit, L"新 MPV Profile");
            state.dirty = true;
            SetFocus(state.idEdit);
            SendMessageW(state.idEdit, EM_SETSEL, 0, -1);
        }
        return 0;
    case IdRefresh:
        if (ConfirmDiscard(state)) {
            PopulateList(state, state.originalId);
            LoadEditor(state, state.selectedIndex);
            ui::SetWindowTextString(
                state.autoLaunchEdit,
                std::to_wstring(state.store->AutoLaunchSeconds()));
            SendMessageW(
                state.skipProfilePicker, BM_SETCHECK,
                state.store->SkipProfilePicker() ? BST_CHECKED : BST_UNCHECKED,
                0);
            SendMessageW(state.logging, BM_SETCHECK,
                         state.store->LoggingEnabled() ? BST_CHECKED
                                                       : BST_UNCHECKED,
                         0);
            UpdateProtocolStatus(state);
        }
        return 0;
    case IdOpenIni:
        OpenIniWithDefaultApp(state.store->IniPath(), state.window);
        return 0;
    case IdBrowse: {
        std::wstring selectedPath;
        if (ui::BrowseForExecutable(state.window, selectedPath)) {
            ui::SetWindowTextString(state.pathEdit, selectedPath);
            state.dirty = true;
            UpdatePathStatus(state);
        }
        return 0;
    }
    case IdSave:
        SaveEditor(state);
        return 0;
    case IdSetDefault: {
        if (state.dirty && !SaveEditor(state)) return 0;
        const int index = state.selectedIndex;
        if (index < 0 || static_cast<size_t>(index) >= state.profiles.size()) {
            ShowError(L"请先选择并保存一个 Profile。", state.window);
            return 0;
        }
        std::wstring error;
        const std::wstring idValue = state.profiles[static_cast<size_t>(index)].id;
        if (!state.store->SetDefault(idValue, error)) {
            ShowError(L"设置默认 Profile 失败：\n" + error, state.window);
        } else {
            state.defaultId = idValue;
            WriteDiagnosticLog(*state.store, L"默认 Profile 已设为：" + idValue);
            InvalidateRect(state.list, nullptr, TRUE);
        }
        return 0;
    }
    case IdDelete: {
        if (state.originalId.empty()) return 0;
        if (MessageBoxW(state.window,
                        (L"确定删除 Profile “" + state.originalId + L"” 吗？").c_str(),
                        kAppTitle, MB_YESNO | MB_ICONWARNING) != IDYES) {
            return 0;
        }
        const std::wstring deleting = state.originalId;
        std::wstring error;
        if (!state.store->Delete(deleting, error)) {
            ShowError(L"删除 Profile 失败：\n" + error, state.window);
        } else {
            WriteDiagnosticLog(*state.store, L"已删除 Profile：" + deleting);
            PopulateList(state);
            LoadEditor(state, state.selectedIndex);
        }
        return 0;
    }
    case IdLogging: {
        const bool enabled =
            SendMessageW(state.logging, BM_GETCHECK, 0, 0) == BST_CHECKED;
        if (!enabled) {
            WriteDiagnosticLog(*state.store, L"诊断日志已关闭");
        }
        std::wstring error;
        if (!state.store->SetLoggingEnabled(enabled, error)) {
            ShowError(L"无法更新日志设置：\n" + error, state.window);
            SendMessageW(state.logging, BM_SETCHECK,
                         enabled ? BST_UNCHECKED : BST_CHECKED, 0);
        } else if (enabled) {
            WriteDiagnosticLog(*state.store, L"诊断日志已启用");
        }
        return 0;
    }
    case IdSkipProfilePicker: {
        const bool enabled = SendMessageW(state.skipProfilePicker, BM_GETCHECK,
                                          0, 0) == BST_CHECKED;
        std::wstring error;
        if (!state.store->SetSkipProfilePicker(enabled, error)) {
            ShowError(L"无法更新直接播放设置：\n" + error, state.window);
            SendMessageW(state.skipProfilePicker, BM_SETCHECK,
                         enabled ? BST_UNCHECKED : BST_CHECKED, 0);
        } else {
            WriteDiagnosticLog(
                *state.store,
                enabled ? L"已启用：多 Profile 时直接使用默认项"
                        : L"已关闭：多 Profile 时显示 Profile 选择窗口");
        }
        return 0;
    }
    case IdOpenLog:
        OpenDiagnosticLog(*state.store, state.window);
        return 0;
    case IdMediaAssociations:
        RunMediaAssociationsDialog(state.instance, state.window);
        return 0;
    case IdEnvironmentManager:
        RunEnvironmentManagerDialog(state.instance, state.window);
        return 0;
    case IdRegisterProtocol: {
        std::wstring error;
        if (!RegisterMpvBridgeProtocol(error)) {
            ShowError(L"注册 mpvbridge:// 失败：\n" + error, state.window);
        } else {
            WriteDiagnosticLog(*state.store, L"已注册 mpvbridge:// 网页调用协议");
            ShowInfo(L"mpvbridge:// 已注册到当前 MPVBridge。\n\n"
                     L"浏览器协议调用将按当前 Profile 设置进入播放流程。",
                     state.window);
        }
        UpdateProtocolStatus(state);
        return 0;
    }
    case IdUnregisterProtocol: {
        if (MessageBoxW(state.window,
                        L"确定注销 mpvbridge:// 吗？\n\n"
                        L"此操作不会修改或删除现有 ush:// 协议。",
                        kAppTitle, MB_YESNO | MB_ICONWARNING) != IDYES) {
            return 0;
        }
        std::wstring error;
        if (!UnregisterMpvBridgeProtocol(error)) {
            ShowError(L"注销 mpvbridge:// 失败：\n" + error, state.window);
        } else {
            WriteDiagnosticLog(*state.store, L"已注销 mpvbridge:// 网页调用协议");
            ShowInfo(L"mpvbridge:// 已注销。\n现有 ush:// 未作任何更改。",
                     state.window);
        }
        UpdateProtocolStatus(state);
        return 0;
    }
    case IdSaveAutoLaunch: {
        const std::wstring text =
            Trim(ui::GetWindowTextString(state.autoLaunchEdit));
        wchar_t* end = nullptr;
        const long seconds = std::wcstol(text.c_str(), &end, 10);
        if (text.empty() || end == text.c_str() || *end != L'\0' ||
            seconds < 0 || seconds > 3600) {
            ShowError(L"请输入 0–3600 之间的整数秒数；0 表示关闭自动进入。",
                      state.window);
            SetFocus(state.autoLaunchEdit);
            SendMessageW(state.autoLaunchEdit, EM_SETSEL, 0, -1);
            return 0;
        }
        std::wstring error;
        if (!state.store->SetAutoLaunchSeconds(static_cast<int>(seconds),
                                               error)) {
            ShowError(L"无法保存自动进入延迟：\n" + error, state.window);
            return 0;
        }
        WriteDiagnosticLog(
            *state.store,
            seconds == 0
                ? L"默认 Profile 自动进入已关闭"
                : L"默认 Profile 自动进入延迟已设为 " +
                      std::to_wstring(seconds) + L" 秒");
        ShowInfo(seconds == 0
                     ? L"已关闭默认 Profile 自动进入。"
                     : L"默认 Profile 将在选择窗口显示 " +
                           std::to_wstring(seconds) + L" 秒后自动进入。",
                 state.window);
        return 0;
    }
    case IdMoveUp:
    case IdMoveDown: {
        const int current = state.selectedIndex;
        const int target = current + (id == IdMoveUp ? -1 : 1);
        MoveProfileTo(state, current, target);
        return 0;
    }
    default:
        break;
    }
    return DefWindowProcW(state.window, WM_COMMAND, wParam, lParam);
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* state = reinterpret_cast<ManagerState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        state = static_cast<ManagerState*>(create->lpCreateParams);
        state->window = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }
    if (state == nullptr) return DefWindowProcW(window, message, wParam, lParam);

    switch (message) {
    case WM_CREATE:
        state->dpi = GetDpiForWindow(window);
        state->surfaceBrush = CreateSolidBrush(ui::kSurface);
        ui::ApplyModernWindowFrame(window);
        CreateControls(*state);
        PopulateList(*state);
        LoadEditor(*state, state->selectedIndex);
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
        if (wParam == HTCAPTION) {
            ui::ToggleMaximize(window);
            return 0;
        }
        break;
    case kSelectProfileMessage: {
        const int chosen = static_cast<int>(wParam);
        if (chosen < 0 || static_cast<size_t>(chosen) >= state->profiles.size()) {
            return FALSE;
        }
        if (chosen == state->selectedIndex) return TRUE;
        if (!ConfirmDiscard(*state)) return FALSE;
        LoadEditor(*state, chosen);
        InvalidateRect(state->list, nullptr, FALSE);
        return TRUE;
    }
    case WM_COMMAND:
        return HandleCommand(*state, wParam, lParam);
    case WM_MEASUREITEM: {
        auto* measure = reinterpret_cast<MEASUREITEMSTRUCT*>(lParam);
        if (measure->CtlID == IdProfileList) {
            measure->itemHeight = static_cast<UINT>(S(*state, 66));
            return TRUE;
        }
        break;
    }
    case WM_DRAWITEM: {
        const auto* item = reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
        if (item->CtlID == IdProfileList) {
            DrawManagerListItem(*state, *item);
            return TRUE;
        }
        break;
    }
    case WM_CTLCOLORSTATIC: {
        HDC dc = reinterpret_cast<HDC>(wParam);
        const HWND control = reinterpret_cast<HWND>(lParam);
        if (control == state->status || control == state->protocolStatus) {
            SetBkMode(dc, OPAQUE);
            SetBkColor(dc, ui::kSurface);
            if (control == state->protocolStatus) {
                const ProtocolRegistrationStatus protocol =
                    GetMpvBridgeProtocolStatus();
                SetTextColor(dc, protocol.ownedByCurrentExecutable
                                     ? ui::kSuccess
                                     : protocol.registered ? ui::kWarning
                                                           : ui::kMuted);
            } else {
                SetTextColor(dc,
                             IsUsableExecutable(std::filesystem::path(
                                 Trim(ui::GetWindowTextString(state->pathEdit))))
                                 ? ui::kSuccess
                                 : ui::kMuted);
            }
            return reinterpret_cast<LRESULT>(state->surfaceBrush);
        }
        // These native STATIC controls overlap a parent window that uses
        // WS_CLIPCHILDREN.  A transparent brush leaves their pixels untouched,
        // allowing text from an earlier layout/paint to show through.  Paint an
        // opaque card-coloured background before drawing every static label.
        SetBkMode(dc, OPAQUE);
        SetBkColor(dc, ui::kSurface);
        SetTextColor(dc, ui::kText);
        return reinterpret_cast<LRESULT>(state->surfaceBrush);
    }
    case WM_CTLCOLORBTN: {
        HDC dc = reinterpret_cast<HDC>(wParam);
        SetBkMode(dc, OPAQUE);
        SetBkColor(dc, ui::kSurface);
        return reinterpret_cast<LRESULT>(state->surfaceBrush);
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        Paint(*state);
        return 0;
    case WM_GETMINMAXINFO: {
        auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
        ui::ApplyMonitorWorkArea(window, *info);
        info->ptMinTrackSize = POINT{S(*state, 880), S(*state, 700)};
        return 0;
    }
    case WM_CLOSE:
        if (ConfirmDiscard(*state)) DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        state->done = true;
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

bool RegisterManagerClass(HINSTANCE instance) {
    WNDCLASSEXW boardClass{sizeof(boardClass)};
    boardClass.style = CS_DBLCLKS;
    boardClass.lpfnWndProc = ProfileBoardProc;
    boardClass.hInstance = instance;
    boardClass.hCursor = LoadCursorW(nullptr, IDC_HAND);
    boardClass.hbrBackground = nullptr;
    boardClass.lpszClassName = kProfileBoardClass;
    if (RegisterClassExW(&boardClass) == 0 &&
        GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return false;
    }

    WNDCLASSEXW windowClass{sizeof(windowClass)};
    windowClass.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    windowClass.lpfnWndProc = WindowProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_APP_ICON));
    windowClass.hIconSm = windowClass.hIcon;
    windowClass.hbrBackground = nullptr;
    windowClass.lpszClassName = kManagerClass;
    return RegisterClassExW(&windowClass) != 0 ||
           GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

} // namespace

int RunProfileManager(HINSTANCE instance, ProfileStore& store, HWND owner) {
    if (!RegisterManagerClass(instance)) {
        ShowError(L"无法创建 Profile 管理窗口。", owner);
        return 1;
    }
    ManagerState state{};
    state.instance = instance;
    state.owner = owner;
    state.store = &store;
    const UINT dpi = owner != nullptr ? GetDpiForWindow(owner) : GetDpiForSystem();
    const int width = ui::Scale(940, dpi);
    const int height = ui::Scale(700, dpi);
    HWND window = CreateWindowExW(
        WS_EX_CONTROLPARENT | WS_EX_APPWINDOW, kManagerClass,
        L"MPVBridge · Profile 管理",
        WS_POPUP | WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX |
            WS_MAXIMIZEBOX | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT,
        width, height, owner, nullptr, instance, &state);
    if (window == nullptr) {
        ShowError(L"无法创建 Profile 管理窗口：\n" +
                  FormatSystemError(GetLastError()), owner);
        return 1;
    }
    if (owner != nullptr) EnableWindow(owner, FALSE);
    ui::CenterWindow(window, owner);
    ShowWindow(window, SW_SHOW);
    UpdateWindow(window);

    const ACCEL accelerators[] = {
        {FVIRTKEY | FALT, VK_UP, IdMoveUp},
        {FVIRTKEY | FALT, VK_DOWN, IdMoveDown},
    };
    HACCEL acceleratorTable = CreateAcceleratorTableW(
        const_cast<LPACCEL>(accelerators), static_cast<int>(std::size(accelerators)));
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
    if (state.sectionFont != nullptr) DeleteObject(state.sectionFont);
    if (state.surfaceBrush != nullptr) DeleteObject(state.surfaceBrush);
    return 0;
}
