// Theme: palette (One Dark / One Light), DPI helpers, DirectWrite text formats, title bar.
#pragma once

#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>

#include <string>

namespace theme {

// ------------------------------------------------------------------ palette
enum Color {
    BgMain, BgPanel, FgText, FgSub, Accent, Success, Error, Warning,
    EntryBg, EntryFg, DisabledFg, Border, SelectBg, SelectRow, ListBg,
    RowOdd, RowEven, GridLines, PlotBg, AccentHover,
    ColorCount
};

// ------------------------------------------------------------------- mode
// Pref is the stored choice (may be System); Mode is the resolved palette.
enum class Pref { Dark, Light, System };
enum class Mode { Dark, Light };

// The Windows app theme from the registry; Dark if the value is missing.
Mode SystemMode();

// String form of a Pref for storage.
Pref PrefFromCode(const std::wstring& code);
const wchar_t* PrefCode(Pref p);

Pref CurrentPref();
Mode CurrentMode();

// Sets the preference; returns whether the effective mode changed. Does not repaint.
bool SetPref(Pref p);

// Re-resolves a System preference (call on WM_SETTINGCHANGE); returns whether the mode changed.
bool ReresolveSystem();

// Drops cached GDI and Direct2D colour resources.
void InvalidateColorCaches();

UINT32       Rgb(Color c);                            // 0x00RRGGBB
COLORREF     Ref(Color c);                            // 0x00BBGGRR (GDI/DWM order)
D2D1_COLOR_F D2d(Color c, float alpha = 1.0f);

// ---------------------------------------------------------------------- DPI
// Layout constants are authored at 96 DPI. Direct2D scales itself; Scale() is for
// values Win32 needs in physical pixels (control rects, window sizes).

// ----------------------------------------------------------- the UI scale
// Global UI scale factor applied to layout, Direct2D drawing and GDI fonts.
// The window frame is not scaled.
inline constexpr int kUiScaleNum = 7;
inline constexpr int kUiScaleDen = 8;

// The DPI the app draws at: the window DPI times the UI scale.
inline float UiDpiF(UINT dpi) {
    return static_cast<float>(dpi) * static_cast<float>(kUiScaleNum) /
           static_cast<float>(kUiScaleDen);
}

inline int Scale(int value96, UINT dpi) {
    return MulDiv(value96 * kUiScaleNum, static_cast<int>(dpi), 96 * kUiScaleDen);
}

// Inverse of Scale(): physical pixels back to authored units.
inline int Unscale(int value_px, UINT dpi) {
    if (dpi == 0) dpi = 96;
    return MulDiv(value_px * kUiScaleDen, 96, static_cast<int>(dpi) * kUiScaleNum);
}

UINT WindowDpi(HWND hwnd);

// Per-monitor DPI versions of AdjustWindowRectEx and GetSystemMetrics.
void AdjustWindowRect(RECT* rc, DWORD style, UINT dpi);
int  SystemMetric(int index, UINT dpi);

// Sets the minimum track size to the given client size, clamped to the work area.
// `hwnd` may be null to skip the clamp.
void ClampMinTrackSize(MINMAXINFO* mmi, HWND hwnd, int client_w96, int client_h96,
                       DWORD style, UINT dpi);

// Sizes a new window for its client size and keeps it inside the monitor's work area.
void FitToWorkArea(HWND hwnd, int client_w96, int client_h96, DWORD style, UINT dpi);

// --------------------------------------------------------------------- text
// Sizes are authored in points (DIP = pt * 4/3).
enum class Font {
    Ui,        //  9pt  body text
    UiBold,    //  9pt  headings, button labels
    Title,     // 13pt  window title
    Subtitle,  //  8pt  secondary text
    Mono,      //  9pt  monospace
    MonoLog,   // 10pt  monospace, larger
};

// Shared, process-lifetime text formats. Alignment and wrapping are set on every call.
IDWriteTextFormat* Text(Font f,
                        DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING,
                        bool wrap = false,
                        DWRITE_PARAGRAPH_ALIGNMENT vertical =
                            DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

// --------------------------------------------------------------- GDI resources
// HFONTs for native controls, cached per DPI.
HFONT UiFontHandle(UINT dpi);
HFONT MonoFontHandle(UINT dpi);

// The application icon at the system large / small icon size (shared handles).
HICON AppIconLarge();
HICON AppIconSmall();

// Cached solid brush, e.g. for WM_CTLCOLOR* replies.
HBRUSH SolidBrush(Color c);
// Cached 1px pen.
HPEN   SolidPen(Color c);

void Shutdown();

// ----------------------------------------------------------- window chrome
// Colours the title bar for the current mode via DWM. Re-callable.
void ApplyTitleBar(HWND hwnd);

// Applies the dark or light visual style to a control's scrollbar.
void ApplyScrollbarTheme(HWND control);

// Scrollbar theme plus ListView item colours for the current mode.
void ApplyListViewTheme(HWND list);

// Hides a window from the compositor without hiding it from Windows.
void SetCloaked(HWND hwnd, bool cloaked);

// Shows a window only after its first frame has been painted.
void ShowPainted(HWND hwnd);

// Whether the compositor is not showing this window.
bool IsCloaked(HWND hwnd);

// The two halves of ShowPainted, for work between showing and revealing.
bool BeginShowPainted(HWND hwnd);
void EndShowPainted(HWND hwnd, bool was_visible);

}  // namespace theme
