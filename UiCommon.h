#pragma once

#include "AppCore.h"

#include <string>

namespace ui {

inline constexpr COLORREF kBackground = RGB(243, 246, 251);
inline constexpr COLORREF kSurface = RGB(255, 255, 255);
inline constexpr COLORREF kHeader = RGB(22, 31, 49);
inline constexpr COLORREF kAccent = RGB(45, 108, 223);
inline constexpr COLORREF kAccentSoft = RGB(232, 240, 255);
inline constexpr COLORREF kText = RGB(28, 36, 50);
inline constexpr COLORREF kMuted = RGB(104, 116, 139);
inline constexpr COLORREF kBorder = RGB(218, 225, 236);
inline constexpr COLORREF kSuccess = RGB(23, 133, 86);
inline constexpr COLORREF kDanger = RGB(194, 57, 52);
inline constexpr COLORREF kWarning = RGB(218, 151, 36);

enum class ButtonStyle { Primary, Secondary, Danger, Toggle };
enum class ChromeButton { None, Minimize, Maximize, Close };

struct ChromeRects {
    RECT minimize{};
    RECT maximize{};
    RECT close{};
};

int Scale(int value, UINT dpi);
HFONT CreateFont(UINT dpi, int points, int weight = FW_NORMAL);
void SetFont(HWND control, HFONT font);
void ApplyControlTheme(HWND control);
void ApplyModernWindowFrame(HWND window);
void ApplyRoundedWindowRegion(HWND window, UINT dpi);
void ApplyMonitorWorkArea(HWND window, MINMAXINFO& info);
LRESULT FramelessHitTest(HWND window, POINT screenPoint, UINT dpi,
                         int titleHeight, const ChromeRects& chrome);
ChromeRects GetChromeRects(const RECT& client, UINT dpi);
ChromeButton HitChromeButton(POINT clientPoint, const ChromeRects& chrome);
void DrawChromeButtons(HDC dc, const ChromeRects& chrome, ChromeButton hot,
                       ChromeButton pressed, bool maximized, UINT dpi);
void InvalidateChrome(HWND window, const ChromeRects& chrome);
void ToggleMaximize(HWND window);
void CenterWindow(HWND window, HWND owner = nullptr);
std::wstring GetWindowTextString(HWND control);
void SetWindowTextString(HWND control, const std::wstring& text);
bool BrowseForExecutable(HWND owner, std::wstring& selectedPath);
void StyleButton(HWND button, ButtonStyle style,
                 COLORREF parentBackground = kSurface);
void StyleEdit(HWND edit);
void DrawRoundedEditFrame(HDC dc, HWND parent, HWND edit, UINT dpi);
void FillSolid(HDC dc, const RECT& rect, COLORREF color);
void FillRounded(HDC dc, const RECT& rect, int radius, COLORREF color,
                 COLORREF border = CLR_INVALID);
void DrawTextLine(HDC dc, const std::wstring& text, RECT rect, HFONT font,
                  COLORREF color, UINT format);

} // namespace ui
