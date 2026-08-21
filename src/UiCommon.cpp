#include "UiCommon.h"

#include <commctrl.h>
#include <dwmapi.h>
#include <shobjidl.h>
#include <uxtheme.h>

#include <algorithm>
#include <cmath>
#include <iterator>
#include <memory>
#include <vector>

namespace ui {

namespace {

constexpr UINT_PTR kButtonSubclass = 0x4D504201;
constexpr UINT_PTR kEditSubclass = 0x4D504202;
constexpr UINT_PTR kButtonAnimationTimer = 0x4D504203;
constexpr float kToggleAnimationDurationMs = 150.0F;

struct ButtonVisualState {
    ButtonStyle style{ButtonStyle::Secondary};
    COLORREF parentBackground{kSurface};
    bool hot{};
    float togglePosition{};
    float toggleTarget{};
    ULONGLONG animationTick{};
};

float SmoothStep(float value) {
    value = std::clamp(value, 0.0F, 1.0F);
    return value * value * (3.0F - 2.0F * value);
}

COLORREF BlendColor(COLORREF from, COLORREF to, float amount) {
    amount = std::clamp(amount, 0.0F, 1.0F);
    const auto blend = [amount](BYTE first, BYTE second) {
        return static_cast<BYTE>(std::lround(
            static_cast<float>(first) +
            (static_cast<float>(second) - static_cast<float>(first)) * amount));
    };
    return RGB(blend(GetRValue(from), GetRValue(to)),
               blend(GetGValue(from), GetGValue(to)),
               blend(GetBValue(from), GetBValue(to)));
}

void StartToggleAnimation(HWND window, ButtonVisualState& state) {
    if (state.style != ButtonStyle::Toggle) return;
    state.toggleTarget =
        SendMessageW(window, BM_GETCHECK, 0, 0) == BST_CHECKED ? 1.0F : 0.0F;
    if (std::abs(state.toggleTarget - state.togglePosition) < 0.001F) {
        state.togglePosition = state.toggleTarget;
        KillTimer(window, kButtonAnimationTimer);
        InvalidateRect(window, nullptr, FALSE);
        return;
    }
    state.animationTick = GetTickCount64();
    SetTimer(window, kButtonAnimationTimer, 15, nullptr);
    InvalidateRect(window, nullptr, FALSE);
}

bool Contains(const RECT& rect, POINT point) {
    return PtInRect(&rect, point) != FALSE;
}

void DrawCenteredIcon(HDC dc, const RECT& rect, ChromeButton button,
                      COLORREF color, bool maximized, int stroke) {
    HPEN pen = CreatePen(PS_SOLID, stroke, color);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    const int centerX = (rect.left + rect.right) / 2;
    const int centerY = (rect.top + rect.bottom) / 2;
    const int half = std::max(4, static_cast<int>(rect.right - rect.left) / 7);
    if (button == ChromeButton::Minimize) {
        MoveToEx(dc, centerX - half, centerY + half / 2, nullptr);
        LineTo(dc, centerX + half + 1, centerY + half / 2);
    } else if (button == ChromeButton::Maximize && !maximized) {
        Rectangle(dc, centerX - half, centerY - half,
                  centerX + half + 1, centerY + half + 1);
    } else if (button == ChromeButton::Maximize) {
        Rectangle(dc, centerX - half + 2, centerY - half - 2,
                  centerX + half + 2, centerY + half - 1);
        Rectangle(dc, centerX - half - 2, centerY - half + 2,
                  centerX + half - 1, centerY + half + 2);
    } else if (button == ChromeButton::Close) {
        MoveToEx(dc, centerX - half, centerY - half, nullptr);
        LineTo(dc, centerX + half + 1, centerY + half + 1);
        MoveToEx(dc, centerX + half, centerY - half, nullptr);
        LineTo(dc, centerX - half - 1, centerY + half + 1);
    }
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(pen);
}

void PaintButton(HWND window, ButtonVisualState& state) {
    PAINTSTRUCT paint{};
    HDC target = BeginPaint(window, &paint);
    RECT client{};
    GetClientRect(window, &client);
    const int width = client.right;
    const int height = client.bottom;
    HDC dc = CreateCompatibleDC(target);
    HBITMAP bitmap = CreateCompatibleBitmap(target, std::max(1, width),
                                             std::max(1, height));
    HGDIOBJ oldBitmap = SelectObject(dc, bitmap);
    FillSolid(dc, client, state.parentBackground);

    const bool enabled = IsWindowEnabled(window) != FALSE;
    const LRESULT buttonState = SendMessageW(window, BM_GETSTATE, 0, 0);
    const bool pressed = (buttonState & BST_PUSHED) != 0;
    const UINT dpi = GetDpiForWindow(window);

    if (state.style == ButtonStyle::Toggle) {
        const int trackWidth = Scale(40, dpi);
        const int trackHeight = Scale(23, dpi);
        RECT track{Scale(1, dpi), (height - trackHeight) / 2,
                   Scale(1, dpi) + trackWidth, (height + trackHeight) / 2};
        const float position = SmoothStep(state.togglePosition);
        FillRounded(dc, track, trackHeight,
                    BlendColor(RGB(198, 207, 220), kAccent, position));
        const int knob = trackHeight - Scale(6, dpi);
        const int uncheckedLeft = track.left + Scale(3, dpi);
        const int checkedLeft = track.right - knob - Scale(3, dpi);
        const int knobLeft = static_cast<int>(std::lround(
            static_cast<float>(uncheckedLeft) +
            static_cast<float>(checkedLeft - uncheckedLeft) * position));
        RECT circle{knobLeft, track.top + Scale(3, dpi), knobLeft + knob,
                    track.top + Scale(3, dpi) + knob};
        HBRUSH brush = CreateSolidBrush(RGB(255, 255, 255));
        HPEN pen = CreatePen(PS_SOLID, 1, RGB(255, 255, 255));
        HGDIOBJ oldBrush = SelectObject(dc, brush);
        HGDIOBJ oldPen = SelectObject(dc, pen);
        Ellipse(dc, circle.left, circle.top, circle.right, circle.bottom);
        SelectObject(dc, oldPen);
        SelectObject(dc, oldBrush);
        DeleteObject(pen);
        DeleteObject(brush);
        RECT text{track.right + Scale(10, dpi), 0, client.right, client.bottom};
        wchar_t label[256]{};
        GetWindowTextW(window, label, static_cast<int>(std::size(label)));
        DrawTextLine(dc, label, text,
                     reinterpret_cast<HFONT>(SendMessageW(window, WM_GETFONT, 0, 0)),
                     enabled ? kText : RGB(156, 164, 177),
                     DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
    } else {
        RECT face = client;
        InflateRect(&face, -Scale(1, dpi), -Scale(1, dpi));
        COLORREF fill = kSurface;
        COLORREF border = kBorder;
        COLORREF textColor = kText;
        if (state.style == ButtonStyle::Primary) {
            fill = pressed ? RGB(29, 82, 181)
                           : state.hot ? RGB(55, 119, 230) : kAccent;
            border = fill;
            textColor = RGB(255, 255, 255);
        } else if (state.style == ButtonStyle::Danger) {
            fill = pressed ? RGB(247, 218, 215)
                           : state.hot ? RGB(255, 235, 233) : kSurface;
            border = state.hot ? RGB(224, 137, 132) : kBorder;
            textColor = kDanger;
        } else if (state.hot || pressed) {
            fill = pressed ? RGB(225, 232, 243) : RGB(246, 248, 252);
            border = RGB(190, 202, 220);
        }
        if (!enabled) {
            fill = RGB(240, 242, 246);
            border = RGB(225, 229, 236);
            textColor = RGB(156, 164, 177);
        }
        FillRounded(dc, face, Scale(10, dpi), fill, border);
        wchar_t label[256]{};
        GetWindowTextW(window, label, static_cast<int>(std::size(label)));
        DrawTextLine(dc, label, face,
                     reinterpret_cast<HFONT>(SendMessageW(window, WM_GETFONT, 0, 0)),
                     textColor, DT_CENTER | DT_SINGLELINE | DT_VCENTER |
                                    DT_END_ELLIPSIS);
    }
    BitBlt(target, 0, 0, width, height, dc, 0, 0, SRCCOPY);
    SelectObject(dc, oldBitmap);
    DeleteObject(bitmap);
    DeleteDC(dc);
    EndPaint(window, &paint);
}

LRESULT CALLBACK ButtonSubclassProc(HWND window, UINT message, WPARAM wParam,
                                    LPARAM lParam, UINT_PTR subclassId,
                                    DWORD_PTR referenceData) {
    auto* state = reinterpret_cast<ButtonVisualState*>(referenceData);
    switch (message) {
    case WM_MOUSEMOVE:
        if (!state->hot) {
            state->hot = true;
            TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, window, 0};
            TrackMouseEvent(&track);
            InvalidateRect(window, nullptr, FALSE);
        }
        break;
    case WM_MOUSELEAVE:
        state->hot = false;
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    case WM_TIMER:
        if (wParam == kButtonAnimationTimer &&
            state->style == ButtonStyle::Toggle) {
            const ULONGLONG now = GetTickCount64();
            const float elapsed = static_cast<float>(std::min<ULONGLONG>(
                now - state->animationTick, 50));
            state->animationTick = now;
            const float distance = state->toggleTarget - state->togglePosition;
            const float step = elapsed / kToggleAnimationDurationMs;
            if (std::abs(distance) <= step) {
                state->togglePosition = state->toggleTarget;
                KillTimer(window, kButtonAnimationTimer);
            } else {
                state->togglePosition += std::copysign(step, distance);
            }
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        }
        break;
    case WM_PAINT:
        PaintButton(window, *state);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_ENABLE:
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
    case WM_SETTEXT:
    case BM_SETCHECK: {
        const LRESULT result = DefSubclassProc(window, message, wParam, lParam);
        if (message == BM_SETCHECK) {
            StartToggleAnimation(window, *state);
        } else {
            InvalidateRect(window, nullptr, FALSE);
        }
        return result;
    }
    case WM_NCDESTROY:
        KillTimer(window, kButtonAnimationTimer);
        RemoveWindowSubclass(window, ButtonSubclassProc, subclassId);
        delete state;
        break;
    }
    const LRESULT result = DefSubclassProc(window, message, wParam, lParam);
    if (message == WM_LBUTTONUP || message == WM_KEYUP || message == BM_CLICK) {
        StartToggleAnimation(window, *state);
    } else if (message == WM_LBUTTONDOWN) {
        InvalidateRect(window, nullptr, FALSE);
    }
    return result;
}

LRESULT CALLBACK EditSubclassProc(HWND window, UINT message, WPARAM wParam,
                                  LPARAM lParam, UINT_PTR subclassId, DWORD_PTR) {
    if (message == WM_SETFOCUS || message == WM_KILLFOCUS ||
        message == WM_ENABLE) {
        InvalidateRect(GetParent(window), nullptr, FALSE);
    }
    if (message == WM_NCDESTROY) {
        RemoveWindowSubclass(window, EditSubclassProc, subclassId);
    }
    return DefSubclassProc(window, message, wParam, lParam);
}

} // namespace

int Scale(int value, UINT dpi) {
    return MulDiv(value, static_cast<int>(dpi), USER_DEFAULT_SCREEN_DPI);
}

HFONT CreateFont(UINT dpi, int points, int weight) {
    LOGFONTW font{};
    font.lfHeight = -MulDiv(points, static_cast<int>(dpi), 72);
    font.lfWeight = weight;
    font.lfQuality = CLEARTYPE_QUALITY;
    wcscpy_s(font.lfFaceName, L"Segoe UI Variable Text");
    HFONT created = CreateFontIndirectW(&font);
    if (created == nullptr) {
        wcscpy_s(font.lfFaceName, L"Segoe UI");
        created = CreateFontIndirectW(&font);
    }
    return created;
}

void SetFont(HWND control, HFONT font) {
    SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
}

void ApplyControlTheme(HWND control) {
    SetWindowTheme(control, L"Explorer", nullptr);
}

void ApplyModernWindowFrame(HWND window) {
    BOOL dark = FALSE;
    DwmSetWindowAttribute(window, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark,
                          sizeof(dark));
#ifdef DWMWA_WINDOW_CORNER_PREFERENCE
    const DWM_WINDOW_CORNER_PREFERENCE corners = DWMWCP_ROUND;
    DwmSetWindowAttribute(window, DWMWA_WINDOW_CORNER_PREFERENCE, &corners,
                          sizeof(corners));
#endif
}

void ApplyRoundedWindowRegion(HWND window, UINT dpi) {
    if (IsZoomed(window) != FALSE) {
        SetWindowRgn(window, nullptr, TRUE);
        return;
    }
    RECT rect{};
    GetWindowRect(window, &rect);
    const int width = rect.right - rect.left;
    const int height = rect.bottom - rect.top;
    const int radius = Scale(18, dpi);
    HRGN region = CreateRoundRectRgn(0, 0, width + 1, height + 1,
                                     radius, radius);
    if (SetWindowRgn(window, region, TRUE) == 0) {
        DeleteObject(region);
    }
}

void ApplyMonitorWorkArea(HWND window, MINMAXINFO& info) {
    const HMONITOR monitor = MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
    MONITORINFO monitorInfo{sizeof(monitorInfo)};
    if (GetMonitorInfoW(monitor, &monitorInfo) != FALSE) {
        info.ptMaxPosition.x = monitorInfo.rcWork.left - monitorInfo.rcMonitor.left;
        info.ptMaxPosition.y = monitorInfo.rcWork.top - monitorInfo.rcMonitor.top;
        info.ptMaxSize.x = monitorInfo.rcWork.right - monitorInfo.rcWork.left;
        info.ptMaxSize.y = monitorInfo.rcWork.bottom - monitorInfo.rcWork.top;
    }
}

ChromeRects GetChromeRects(const RECT& client, UINT dpi) {
    const int size = Scale(34, dpi);
    const int gap = Scale(7, dpi);
    const int right = client.right - Scale(18, dpi);
    const int top = Scale(14, dpi);
    ChromeRects result{};
    result.close = RECT{right - size, top, right, top + size};
    result.maximize = RECT{result.close.left - gap - size, top,
                           result.close.left - gap, top + size};
    result.minimize = RECT{result.maximize.left - gap - size, top,
                           result.maximize.left - gap, top + size};
    return result;
}

ChromeButton HitChromeButton(POINT point, const ChromeRects& chrome) {
    if (Contains(chrome.close, point)) return ChromeButton::Close;
    if (Contains(chrome.maximize, point)) return ChromeButton::Maximize;
    if (Contains(chrome.minimize, point)) return ChromeButton::Minimize;
    return ChromeButton::None;
}

LRESULT FramelessHitTest(HWND window, POINT screenPoint, UINT dpi,
                         int titleHeight, const ChromeRects& chrome) {
    RECT bounds{};
    GetWindowRect(window, &bounds);
    const int border = Scale(8, dpi);
    if (IsZoomed(window) == FALSE) {
        const bool left = screenPoint.x < bounds.left + border;
        const bool right = screenPoint.x >= bounds.right - border;
        const bool top = screenPoint.y < bounds.top + border;
        const bool bottom = screenPoint.y >= bounds.bottom - border;
        if (top && left) return HTTOPLEFT;
        if (top && right) return HTTOPRIGHT;
        if (bottom && left) return HTBOTTOMLEFT;
        if (bottom && right) return HTBOTTOMRIGHT;
        if (left) return HTLEFT;
        if (right) return HTRIGHT;
        if (top) return HTTOP;
        if (bottom) return HTBOTTOM;
    }
    POINT clientPoint = screenPoint;
    ScreenToClient(window, &clientPoint);
    if (Contains(chrome.maximize, clientPoint)) return HTMAXBUTTON;
    if (!Contains(chrome.minimize, clientPoint) &&
        !Contains(chrome.close, clientPoint) &&
        clientPoint.y >= 0 && clientPoint.y < titleHeight) {
        return HTCAPTION;
    }
    return HTCLIENT;
}

void DrawChromeButtons(HDC dc, const ChromeRects& chrome, ChromeButton hot,
                       ChromeButton pressed, bool maximized, UINT dpi) {
    const ChromeButton buttons[] = {ChromeButton::Minimize,
                                    ChromeButton::Maximize,
                                    ChromeButton::Close};
    for (ChromeButton button : buttons) {
        const RECT& rect = button == ChromeButton::Minimize
                               ? chrome.minimize
                               : button == ChromeButton::Maximize
                                     ? chrome.maximize
                                     : chrome.close;
        COLORREF fill = kHeader;
        if (button == pressed) {
            fill = button == ChromeButton::Close ? RGB(185, 54, 52)
                                                  : RGB(54, 68, 93);
        } else if (button == hot) {
            fill = button == ChromeButton::Close ? RGB(218, 74, 71)
                                                  : RGB(43, 56, 80);
        }
        FillRounded(dc, rect, Scale(11, dpi), fill);
        DrawCenteredIcon(dc, rect, button, RGB(225, 232, 243), maximized,
                         std::max(1, Scale(1, dpi)));
    }
}

void InvalidateChrome(HWND window, const ChromeRects& chrome) {
    RECT dirty{chrome.minimize.left - 2, chrome.minimize.top - 2,
               chrome.close.right + 2, chrome.close.bottom + 2};
    InvalidateRect(window, &dirty, FALSE);
}

void ToggleMaximize(HWND window) {
    ShowWindow(window, IsZoomed(window) != FALSE ? SW_RESTORE : SW_MAXIMIZE);
}

void CenterWindow(HWND window, HWND owner) {
    RECT bounds{};
    GetWindowRect(window, &bounds);
    HMONITOR monitor = owner != nullptr
                           ? MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST)
                           : MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    if (owner == nullptr) {
        POINT cursor{};
        if (GetCursorPos(&cursor) != FALSE) {
            monitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
        }
    }
    MONITORINFO info{sizeof(info)};
    GetMonitorInfoW(monitor, &info);
    const int width = bounds.right - bounds.left;
    const int height = bounds.bottom - bounds.top;
    const int x = info.rcWork.left + (info.rcWork.right - info.rcWork.left - width) / 2;
    const int y = info.rcWork.top + (info.rcWork.bottom - info.rcWork.top - height) / 2;
    SetWindowPos(window, nullptr, x, y, 0, 0,
                 SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

std::wstring GetWindowTextString(HWND control) {
    const int length = GetWindowTextLengthW(control);
    std::vector<wchar_t> text(static_cast<size_t>(length) + 1);
    GetWindowTextW(control, text.data(), static_cast<int>(text.size()));
    return std::wstring(text.data(), static_cast<size_t>(length));
}

void SetWindowTextString(HWND control, const std::wstring& text) {
    SetWindowTextW(control, text.c_str());
}

bool BrowseForExecutable(HWND owner, std::wstring& selectedPath) {
    IFileOpenDialog* dialog = nullptr;
    HRESULT result = CoCreateInstance(CLSID_FileOpenDialog, nullptr,
                                      CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog));
    if (FAILED(result)) {
        return false;
    }
    const COMDLG_FILTERSPEC filters[] = {
        {L"MPV executable", L"mpv.exe"},
        {L"Executable files", L"*.exe"},
        {L"All files", L"*.*"},
    };
    dialog->SetFileTypes(static_cast<UINT>(std::size(filters)), filters);
    dialog->SetFileTypeIndex(1);
    dialog->SetTitle(L"选择此 Profile 使用的 mpv.exe");
    dialog->SetOptions(FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST |
                       FOS_PATHMUSTEXIST | FOS_DONTADDTORECENT);
    result = dialog->Show(owner);
    if (SUCCEEDED(result)) {
        IShellItem* item = nullptr;
        result = dialog->GetResult(&item);
        if (SUCCEEDED(result)) {
            PWSTR path = nullptr;
            result = item->GetDisplayName(SIGDN_FILESYSPATH, &path);
            if (SUCCEEDED(result) && path != nullptr) {
                selectedPath = path;
                CoTaskMemFree(path);
            }
            item->Release();
        }
    }
    dialog->Release();
    return SUCCEEDED(result) && !selectedPath.empty();
}

void StyleButton(HWND button, ButtonStyle style, COLORREF parentBackground) {
    auto* state = new ButtonVisualState{style, parentBackground};
    if (style == ButtonStyle::Toggle) {
        state->togglePosition =
            SendMessageW(button, BM_GETCHECK, 0, 0) == BST_CHECKED ? 1.0F : 0.0F;
        state->toggleTarget = state->togglePosition;
    }
    SetWindowSubclass(button, ButtonSubclassProc, kButtonSubclass,
                      reinterpret_cast<DWORD_PTR>(state));
}

void StyleEdit(HWND edit) {
    SendMessageW(edit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN,
                 MAKELPARAM(Scale(7, GetDpiForWindow(edit)),
                            Scale(7, GetDpiForWindow(edit))));
    SetWindowSubclass(edit, EditSubclassProc, kEditSubclass, 0);
}

void DrawRoundedEditFrame(HDC dc, HWND parent, HWND edit, UINT dpi) {
    RECT rect{};
    GetWindowRect(edit, &rect);
    MapWindowPoints(nullptr, parent, reinterpret_cast<POINT*>(&rect), 2);
    InflateRect(&rect, Scale(5, dpi), Scale(4, dpi));
    const bool focused = GetFocus() == edit;
    FillRounded(dc, rect, Scale(9, dpi), kSurface,
                focused ? kAccent : kBorder);
}

void FillSolid(HDC dc, const RECT& rect, COLORREF color) {
    HBRUSH brush = CreateSolidBrush(color);
    FillRect(dc, &rect, brush);
    DeleteObject(brush);
}

void FillRounded(HDC dc, const RECT& rect, int radius, COLORREF color,
                 COLORREF border) {
    HBRUSH brush = CreateSolidBrush(color);
    HPEN pen = CreatePen(PS_SOLID, 1, border == CLR_INVALID ? color : border);
    HGDIOBJ oldBrush = SelectObject(dc, brush);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, radius, radius);
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
    DeleteObject(pen);
    DeleteObject(brush);
}

void DrawTextLine(HDC dc, const std::wstring& text, RECT rect, HFONT font,
                  COLORREF color, UINT format) {
    HGDIOBJ oldFont = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, color);
    DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &rect, format);
    SelectObject(dc, oldFont);
}

} // namespace ui
