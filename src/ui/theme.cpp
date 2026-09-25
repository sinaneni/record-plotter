// Theme implementation: palettes, DPI helpers, fonts, GDI caches and window chrome.
#include "theme.h"

#include <dwmapi.h>

#include "../resource_ids.h"
#include "d2d.h"

namespace theme {
namespace {

// One Dark, in Color enum order, as 0xRRGGBB. FgSub is lightened for readability.
const UINT32 kDark[ColorCount] = {
    0x21252b,  // BgMain
    0x282c34,  // BgPanel
    0xabb2bf,  // FgText
    0x8f97a6,  // FgSub
    0x61afef,  // Accent
    0x98c379,  // Success
    0xe06c75,  // Error
    0xe5c07b,  // Warning
    0x3b4048,  // EntryBg
    0xffffff,  // EntryFg
    0x5c6370,  // DisabledFg
    0x181a1f,  // Border
    0x3e4451,  // SelectBg
    0x394f65,  // SelectRow
    0x2c313a,  // ListBg
    0x2a2e37,  // RowOdd
    0x21252b,  // RowEven
    0x3b4048,  // GridLines
    0x282c34,  // PlotBg
    0x4d9cd6,  // AccentHover
};

// One Light, with text colours darkened for contrast.
const UINT32 kLight[ColorCount] = {
    0xfafafa,  // BgMain
    0xffffff,  // BgPanel
    0x383a42,  // FgText
    0x6a6c75,  // FgSub
    0x2a5fcc,  // Accent
    0x2d6b2c,  // Success
    0xc23127,  // Error
    0x8a5c00,  // Warning
    0xffffff,  // EntryBg
    0x1f2126,  // EntryFg
    0xa0a1a7,  // DisabledFg
    0xd0d0d3,  // Border
    0xe5e5e6,  // SelectBg
    0xd3e2fb,  // SelectRow
    0xffffff,  // ListBg
    0xf5f5f6,  // RowOdd
    0xffffff,  // RowEven
    0xe2e2e4,  // GridLines
    0xffffff,  // PlotBg
    0x2f66d8,  // AccentHover
};

// Stored preference and the active palette.
Pref g_pref = Pref::System;
const UINT32* g_palette = kDark;

IDWriteTextFormat* g_formats[6] = {};

// Points to DIPs.
constexpr float Pt(float pt) { return pt * 4.0f / 3.0f; }

struct FontSpec { const wchar_t* family; float size; DWRITE_FONT_WEIGHT weight; };

const FontSpec kFonts[6] = {
    {L"Segoe UI",  Pt(9),  DWRITE_FONT_WEIGHT_NORMAL},     // Ui
    {L"Segoe UI",  Pt(9),  DWRITE_FONT_WEIGHT_SEMI_BOLD},  // UiBold
    {L"Segoe UI",  Pt(13), DWRITE_FONT_WEIGHT_SEMI_BOLD},  // Title
    {L"Segoe UI",  Pt(8),  DWRITE_FONT_WEIGHT_NORMAL},     // Subtitle
    {L"Consolas",  Pt(9),  DWRITE_FONT_WEIGHT_NORMAL},     // Mono
    {L"Consolas",  Pt(10), DWRITE_FONT_WEIGHT_NORMAL},     // MonoLog
};

}  // namespace

UINT32 Rgb(Color c) { return g_palette[c]; }

COLORREF Ref(Color c) {
    const UINT32 v = g_palette[c];
    // 0x00BBGGRR
    return RGB((v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF);
}

D2D1_COLOR_F D2d(Color c, float alpha) {
    return D2D1::ColorF(g_palette[c], alpha);
}

Mode SystemMode() {
    // Reads AppsUseLightTheme; a missing value means dark.
    DWORD light = 0, size = sizeof(light), type = 0;
    if (RegGetValueW(HKEY_CURRENT_USER,
                     L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"AppsUseLightTheme", RRF_RT_REG_DWORD, &type, &light,
                     &size) != ERROR_SUCCESS) {
        return Mode::Dark;
    }
    return light ? Mode::Light : Mode::Dark;
}

Pref CurrentPref() { return g_pref; }

// Parses a stored preference; empty or unknown means System.
Pref PrefFromCode(const std::wstring& code) {
    if (code == L"light")  return Pref::Light;
    if (code == L"dark")   return Pref::Dark;
    return Pref::System;
}

const wchar_t* PrefCode(Pref p) {
    switch (p) {
        case Pref::Light:  return L"light";
        case Pref::System: return L"system";
        case Pref::Dark:   break;
    }
    return L"dark";
}
Mode CurrentMode() { return (g_palette == kLight) ? Mode::Light : Mode::Dark; }

UINT WindowDpi(HWND hwnd) {
    // Bound dynamically so older Windows without GetDpiForWindow still runs.
    using GetDpiForWindowFn = UINT(WINAPI*)(HWND);
    static GetDpiForWindowFn fn = reinterpret_cast<GetDpiForWindowFn>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"));
    if (fn) {
        const UINT dpi = fn(hwnd);
        if (dpi) return dpi;
    }
    return 96;
}

void AdjustWindowRect(RECT* rc, DWORD style, UINT dpi) {
    // Prefer the per-DPI variant; fall back to the system-DPI one.
    using AdjustForDpiFn = BOOL(WINAPI*)(LPRECT, DWORD, BOOL, DWORD, UINT);
    static AdjustForDpiFn fn = reinterpret_cast<AdjustForDpiFn>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "AdjustWindowRectExForDpi"));
    if (fn && fn(rc, style, FALSE, 0, dpi)) return;
    AdjustWindowRectEx(rc, style, FALSE, 0);
}

void ClampMinTrackSize(MINMAXINFO* mmi, HWND hwnd, int client_w96, int client_h96,
                       DWORD style, UINT dpi) {
    if (!mmi) return;
    RECT want = {0, 0, Scale(client_w96, dpi), Scale(client_h96, dpi)};
    AdjustWindowRect(&want, style, dpi);
    long w = want.right - want.left;
    long h = want.bottom - want.top;

    MONITORINFO mi = {sizeof(mi)};
    if (hwnd && GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi)) {
        const long avail_w = mi.rcWork.right - mi.rcWork.left;
        const long avail_h = mi.rcWork.bottom - mi.rcWork.top;
        if (avail_w > 0 && w > avail_w) w = avail_w;
        if (avail_h > 0 && h > avail_h) h = avail_h;
    }
    mmi->ptMinTrackSize.x = w;
    mmi->ptMinTrackSize.y = h;
}

void FitToWorkArea(HWND hwnd, int client_w96, int client_h96, DWORD style, UINT dpi) {
    if (!hwnd) return;
    RECT want = {0, 0, Scale(client_w96, dpi), Scale(client_h96, dpi)};
    AdjustWindowRect(&want, style, dpi);
    long w = want.right - want.left;
    long h = want.bottom - want.top;

    // Start from the current position; only move if it hangs off an edge.
    RECT now = {};
    GetWindowRect(hwnd, &now);
    long x = now.left;
    long y = now.top;

    MONITORINFO mi = {sizeof(mi)};
    if (GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi)) {
        const long avail_w = mi.rcWork.right - mi.rcWork.left;
        const long avail_h = mi.rcWork.bottom - mi.rcWork.top;
        if (avail_w > 0 && w > avail_w) w = avail_w;
        if (avail_h > 0 && h > avail_h) h = avail_h;
        // Right/bottom first so the top-left edge wins.
        if (x + w > mi.rcWork.right) x = mi.rcWork.right - w;
        if (y + h > mi.rcWork.bottom) y = mi.rcWork.bottom - h;
        if (x < mi.rcWork.left) x = mi.rcWork.left;
        if (y < mi.rcWork.top) y = mi.rcWork.top;
    }

    SetWindowPos(hwnd, nullptr, static_cast<int>(x), static_cast<int>(y),
                 static_cast<int>(w), static_cast<int>(h), SWP_NOZORDER | SWP_NOACTIVATE);
}

int SystemMetric(int index, UINT dpi) {
    // Prefer the per-DPI variant; fall back to GetSystemMetrics.
    using MetricForDpiFn = int(WINAPI*)(int, UINT);
    static MetricForDpiFn fn = reinterpret_cast<MetricForDpiFn>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetSystemMetricsForDpi"));
    if (fn) {
        const int v = fn(index, dpi);
        if (v) return v;
    }
    return GetSystemMetrics(index);
}

IDWriteTextFormat* Text(Font f, DWRITE_TEXT_ALIGNMENT align, bool wrap,
                        DWRITE_PARAGRAPH_ALIGNMENT vertical) {
    const int i = static_cast<int>(f);
    if (!g_formats[i]) {
        IDWriteFactory* dw = d2d::WriteFactory();
        if (!dw) return nullptr;
        const FontSpec& s = kFonts[i];
        if (FAILED(dw->CreateTextFormat(s.family, nullptr, s.weight,
                                        DWRITE_FONT_STYLE_NORMAL,
                                        DWRITE_FONT_STRETCH_NORMAL, s.size, L"",
                                        &g_formats[i]))) {
            return nullptr;
        }
    }
    // Always set: the format is shared.
    g_formats[i]->SetTextAlignment(align);
    g_formats[i]->SetParagraphAlignment(vertical);
    g_formats[i]->SetWordWrapping(wrap ? DWRITE_WORD_WRAPPING_WRAP
                                       : DWRITE_WORD_WRAPPING_NO_WRAP);
    return g_formats[i];
}

namespace {

// One cached HFONT per (family, dpi).
struct FontCacheEntry { UINT dpi; HFONT font; };
FontCacheEntry g_ui_fonts[8] = {};
FontCacheEntry g_mono_fonts[8] = {};
// GDI brushes and pens per mode; kept until Shutdown because window classes hold them.
HBRUSH g_brushes[2][ColorCount] = {};
HPEN   g_pens[2][ColorCount] = {};

HFONT CachedFont(FontCacheEntry* cache, const wchar_t* family, float pt, UINT dpi) {
    for (int i = 0; i < 8; ++i) {
        if (cache[i].font && cache[i].dpi == dpi) return cache[i].font;
    }
    LOGFONTW lf = {};
    // Negative height = character height; includes the UI scale.
    lf.lfHeight = -MulDiv(static_cast<int>(pt) * kUiScaleNum, static_cast<int>(dpi),
                          72 * kUiScaleDen);
    lf.lfWeight = FW_NORMAL;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfQuality = CLEARTYPE_QUALITY;
    wcsncpy_s(lf.lfFaceName, family, _TRUNCATE);
    HFONT f = CreateFontIndirectW(&lf);
    if (!f) return nullptr;
    for (int i = 0; i < 8; ++i) {
        if (!cache[i].font) { cache[i].dpi = dpi; cache[i].font = f; return f; }
    }
    // Cache full: reuse slot 0.
    DeleteObject(f);
    return cache[0].font;
}

}  // namespace

HFONT UiFontHandle(UINT dpi)   { return CachedFont(g_ui_fonts, L"Segoe UI", 9, dpi); }
HFONT MonoFontHandle(UINT dpi) { return CachedFont(g_mono_fonts, L"Consolas", 9, dpi); }

namespace {

// Loads the app icon at the given pixel size.
HICON LoadAppIcon(int px) {
    return static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr),
                                         MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON,
                                         px, px, LR_DEFAULTCOLOR | LR_SHARED));
}

}  // namespace

HICON AppIconLarge() {
    static HICON icon = LoadAppIcon(GetSystemMetrics(SM_CXICON));
    return icon;
}

HICON AppIconSmall() {
    static HICON icon = LoadAppIcon(GetSystemMetrics(SM_CXSMICON));
    return icon;
}

HBRUSH SolidBrush(Color c) {
    const int m = (CurrentMode() == Mode::Light) ? 1 : 0;
    if (!g_brushes[m][c]) g_brushes[m][c] = CreateSolidBrush(Ref(c));
    return g_brushes[m][c];
}

HPEN SolidPen(Color c) {
    const int m = (CurrentMode() == Mode::Light) ? 1 : 0;
    if (!g_pens[m][c]) g_pens[m][c] = CreatePen(PS_SOLID, 1, Ref(c));
    return g_pens[m][c];
}

void InvalidateColorCaches() {
    // GDI caches are per mode and need nothing; the shared D2D target is rebuilt.
    d2d::SharedDc().Discard();
}

namespace {

// The palette a preference resolves to.
const UINT32* Resolve(Pref p) {
    switch (p) {
        case Pref::Light:  return kLight;
        case Pref::Dark:   return kDark;
        case Pref::System: return (SystemMode() == Mode::Light) ? kLight : kDark;
    }
    return kDark;
}

}  // namespace

bool SetPref(Pref p) {
    g_pref = p;
    const UINT32* want = Resolve(p);
    if (want == g_palette) return false;   // nothing visible to do
    g_palette = want;
    InvalidateColorCaches();
    return true;
}

bool ReresolveSystem() {
    if (g_pref != Pref::System) return false;
    const UINT32* want = Resolve(g_pref);
    if (want == g_palette) return false;
    g_palette = want;
    InvalidateColorCaches();
    return true;
}

void Shutdown() {
    for (auto& f : g_formats) {
        if (f) { f->Release(); f = nullptr; }
    }
    for (auto& e : g_ui_fonts)   { if (e.font) { DeleteObject(e.font); e.font = nullptr; } }
    for (auto& e : g_mono_fonts) { if (e.font) { DeleteObject(e.font); e.font = nullptr; } }
    for (auto& row : g_brushes) {
        for (auto& b : row) { if (b) { DeleteObject(b); b = nullptr; } }
    }
    for (auto& row : g_pens) {
        for (auto& p : row) { if (p) { DeleteObject(p); p = nullptr; } }
    }
}

void ApplyTitleBar(HWND hwnd) {
    // DWM attributes by number: 20 (19 on older Win10) = dark mode,
    // 35 = caption colour, 36 = caption text colour (Win11 only).
    const BOOL dark = (CurrentMode() == Mode::Dark) ? TRUE : FALSE;
    if (FAILED(DwmSetWindowAttribute(hwnd, 20, &dark, sizeof(dark)))) {
        DwmSetWindowAttribute(hwnd, 19, &dark, sizeof(dark));
    }
    const COLORREF caption = Ref(BgMain);
    const COLORREF text = Ref(FgText);
    DwmSetWindowAttribute(hwnd, 35, &caption, sizeof(caption));
    DwmSetWindowAttribute(hwnd, 36, &text, sizeof(text));
}

void ApplyScrollbarTheme(HWND control) {
    if (!control) return;
    SetWindowTheme(control, (CurrentMode() == Mode::Dark) ? L"DarkMode_Explorer" : L"Explorer",
                   nullptr);
}

void ApplyListViewTheme(HWND list) {
    if (!list) return;
    ApplyScrollbarTheme(list);
    ListView_SetBkColor(list, Ref(BgMain));
    ListView_SetTextBkColor(list, CLR_NONE);
    ListView_SetTextColor(list, Ref(FgText));
    InvalidateRect(list, nullptr, TRUE);
}

void SetCloaked(HWND hwnd, bool cloaked) {
    // 13 = DWMWA_CLOAK
    const BOOL on = cloaked ? TRUE : FALSE;
    DwmSetWindowAttribute(hwnd, 13, &on, sizeof(on));
}

bool IsCloaked(HWND hwnd) {
    if (!hwnd) return false;
    // 14 = DWMWA_CLOAKED; non-zero means not on screen.
    DWORD why = 0;
    if (FAILED(DwmGetWindowAttribute(hwnd, 14, &why, sizeof(why)))) return false;
    return why != 0;
}

bool BeginShowPainted(HWND hwnd) {
    if (!hwnd) return true;
    // On screen means visible and not cloaked.
    const bool on_screen = IsWindowVisible(hwnd) != FALSE && !IsCloaked(hwnd);
    // Only cloak a window that is not already on screen.
    if (!on_screen) SetCloaked(hwnd, true);
    ShowWindow(hwnd, SW_SHOW);
    return on_screen;
}

void EndShowPainted(HWND hwnd, bool on_screen) {
    if (!hwnd) return;
    // Paint now, then always uncloak.
    UpdateWindow(hwnd);
    (void)on_screen;
    SetCloaked(hwnd, false);
}

void ShowPainted(HWND hwnd) {
    EndShowPainted(hwnd, BeginShowPainted(hwnd));
}

}  // namespace theme
