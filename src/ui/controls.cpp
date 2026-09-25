// Themed controls: button, text input, checkbox, dropdown.

#include "controls.h"

#include <commctrl.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>

#include "a11y.h"
#include "core/util.h"
#include "d2d.h"

#pragma comment(lib, "comctl32.lib")

namespace ui {
namespace {

const wchar_t kButtonClass[]   = L"MbButton";
const wchar_t kDropdownClass[] = L"MbDropdown";
const wchar_t kPopupClass[]    = L"MbDropdownPopup";
const wchar_t kCheckClass[]    = L"MbCheckbox";


// Per-button state, hung off the window.
struct ButtonState {
    std::wstring       text;
    Button::Variant    variant = Button::Variant::Accent;
    int                id = 0;
    bool               hot = false;
    bool               pressed = false;
    bool               icon = false;
    Button::Glyph      glyph = Button::Glyph::None;
    HWND               tip = nullptr;
    bool               tip_added = false;
    theme::Color       backdrop = theme::BgPanel;
};

// Draws a stroked gear of radius `r` centred on `c`.
void DrawGear(ID2D1RenderTarget* rt, D2D1_POINT_2F c, float r, ID2D1Brush* brush) {
    if (!brush || r <= 0.0f) return;
    constexpr float kStroke = 1.6f;
    constexpr int   kTeeth = 8;
    const float ring = r * 0.66f;     // the body the teeth stand on
    const float hub  = r * 0.26f;     // the hole in the middle
    rt->DrawEllipse(D2D1::Ellipse(c, ring, ring), brush, kStroke);
    rt->DrawEllipse(D2D1::Ellipse(c, hub, hub), brush, kStroke);
    for (int i = 0; i < kTeeth; ++i) {
        // Offset by half a tooth so one tooth points straight up.
        const float a = 6.2831853f * (static_cast<float>(i) / kTeeth) + 3.1415927f / kTeeth;
        const float cs = std::cos(a);
        const float sn = std::sin(a);
        rt->DrawLine(D2D1::Point2F(c.x + cs * (ring - kStroke * 0.5f),
                                   c.y + sn * (ring - kStroke * 0.5f)),
                     D2D1::Point2F(c.x + cs * r, c.y + sn * r), brush, kStroke);
    }
}

// Per-dropdown state.
struct DropdownState {
    std::vector<Dropdown::Item> items;
    theme::Color backdrop = theme::BgPanel;
    int  index = 0;
    int  id = 0;
    bool hot = false;
    bool open = false;
    // The open list window, used to forward keys to it.
    HWND popup = nullptr;
};

// Corner radius of the dropdown list.
constexpr float kDropdownRadius = kPopupRadius;

// Clips a window to a rounded rectangle.
void ApplyRoundedRgn(HWND hwnd, int w, int h, UINT dpi) {
    const int r = theme::Scale(static_cast<int>(kDropdownRadius), dpi);
    if (HRGN rgn = CreateRoundRectRgn(0, 0, w + 1, h + 1, r * 2, r * 2)) {
        if (!SetWindowRgn(hwnd, rgn, FALSE)) DeleteObject(rgn);
    }
}

ButtonState* BtnState(HWND h) {
    return reinterpret_cast<ButtonState*>(GetWindowLongPtrW(h, GWLP_USERDATA));
}
DropdownState* DropState(HWND h) {
    return reinterpret_cast<DropdownState*>(GetWindowLongPtrW(h, GWLP_USERDATA));
}

theme::Color VariantColor(Button::Variant v) {
    switch (v) {
        case Button::Variant::Success: return theme::Success;
        case Button::Variant::Danger:  return theme::Error;
        // FgText, so a Neutral button does not look disabled.
        case Button::Variant::Neutral: return theme::FgText;
        case Button::Variant::Accent:
        default:                       return theme::Accent;
    }
}

void TrackLeave(HWND h) {
    TRACKMOUSEEVENT t = {sizeof(t), TME_LEAVE, h, 0};
    TrackMouseEvent(&t);
}

// Draws text in a rect with the given alignment.
void DrawLabel(ID2D1RenderTarget* rt, const std::wstring& text, theme::Font font,
               ID2D1Brush* brush, const D2D1_RECT_F& r,
               DWRITE_TEXT_ALIGNMENT align) {
    IDWriteTextFormat* fmt = theme::Text(font, align);
    if (!fmt || !brush || text.empty()) return;
    rt->DrawTextW(text.c_str(), static_cast<UINT32>(text.size()), fmt, r, brush,
                  D2D1_DRAW_TEXT_OPTIONS_NONE, DWRITE_MEASURING_MODE_NATURAL);
}

// Draws a single glyph centred on its ink rather than its line box. For symbols only.
void DrawGlyphInkCentred(ID2D1RenderTarget* rt, const std::wstring& text, theme::Font font,
                         ID2D1Brush* brush, const D2D1_RECT_F& r) {
    IDWriteTextFormat* fmt = theme::Text(font, DWRITE_TEXT_ALIGNMENT_CENTER);
    IDWriteFactory* dw = d2d::WriteFactory();
    if (!fmt || !brush || text.empty()) return;
    const float w = r.right - r.left;
    const float h = r.bottom - r.top;
    IDWriteTextLayout* layout = nullptr;
    if (!dw || FAILED(dw->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), fmt,
                                           w, h, &layout)) ||
        !layout) {
        // Fall back to normal centring.
        DrawLabel(rt, text, font, brush, r, DWRITE_TEXT_ALIGNMENT_CENTER);
        return;
    }
    DWRITE_OVERHANG_METRICS om = {};
    float dx = 0.0f;
    float dy = 0.0f;
    if (SUCCEEDED(layout->GetOverhangMetrics(&om))) {
        dx = (om.left - om.right) * 0.5f;
        dy = (om.top - om.bottom) * 0.5f;
    }
    rt->DrawTextLayout(D2D1::Point2F(r.left + dx, r.top + dy), layout, brush);
    layout->Release();
}

// ------------------------------------------------------------ button painting
void PaintButton(HWND hwnd) {
    ButtonState* st = BtnState(hwnd);
    if (!st) return;

    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd, &ps);
    RECT rc;
    GetClientRect(hwnd, &rc);
    const UINT dpi = theme::WindowDpi(hwnd);

    d2d::DcTarget& t = d2d::SharedDc();
    ID2D1RenderTarget* rt = t.Begin(dc, rc, dpi);
    if (rt) {
        const bool on = IsWindowEnabled(hwnd) != FALSE;
        const theme::Color accent = on ? VariantColor(st->variant) : theme::DisabledFg;
        // Only an enabled button fills on hover or press.
        const bool fill = on && (st->hot || st->pressed);

        const D2D1_SIZE_F size = rt->GetSize();
        rt->Clear(theme::D2d(st->backdrop));

        // Inset by half the 2 px stroke so the border lands fully inside.
        const D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(
            D2D1::RectF(1.0f, 1.0f, size.width - 1.0f, size.height - 1.0f), 8.0f, 8.0f);
        if (fill) {
            // Pressed uses a slightly deeper fill than hover.
            rt->FillRoundedRectangle(rr, t.Brush(accent, st->pressed ? 0.85f : 1.0f));
        }
        rt->DrawRoundedRectangle(rr, t.Brush(accent), 2.0f);

        // Keyboard focus ring, inset inside the border.
        if (on && GetFocus() == hwnd) {
            const D2D1_ROUNDED_RECT ring = D2D1::RoundedRect(
                D2D1::RectF(4.0f, 4.0f, size.width - 4.0f, size.height - 4.0f), 5.0f, 5.0f);
            rt->DrawRoundedRectangle(ring, t.Brush(fill ? theme::EntryFg : accent, 0.55f),
                                     1.0f);
        }

        if (st->glyph == Button::Glyph::Gear) {
            DrawGear(rt, D2D1::Point2F(size.width * 0.5f, size.height * 0.5f),
                     (std::min)(size.width, size.height) * 0.30f,
                     fill ? t.Brush(theme::EntryFg) : t.Brush(accent));
        } else if (st->icon) {
            // Lone symbol: centre it on its ink.
            DrawGlyphInkCentred(rt, st->text, theme::Font::Title,
                                fill ? t.Brush(theme::EntryFg) : t.Brush(accent),
                                D2D1::RectF(0, 0, size.width, size.height));
        } else {
            DrawLabel(rt, st->text, theme::Font::UiBold,
                      fill ? t.Brush(theme::EntryFg) : t.Brush(accent),
                      D2D1::RectF(0, 0, size.width, size.height),
                      DWRITE_TEXT_ALIGNMENT_CENTER);
        }
        t.End();
    }
    EndPaint(hwnd, &ps);
}

LRESULT CALLBACK ButtonProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    ButtonState* st = BtnState(hwnd);

    switch (msg) {
        case WM_NCCREATE: {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                              reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
            return TRUE;
        }
        case WM_PAINT:
            PaintButton(hwnd);
            return 0;
        case WM_ERASEBKGND:
            return 1;

        case WM_MOUSEMOVE:
            if (st && !st->hot) {
                st->hot = true;
                TrackLeave(hwnd);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        case WM_MOUSELEAVE:
            if (st && st->hot) {
                st->hot = false;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;

        case WM_LBUTTONDOWN:
            if (st && IsWindowEnabled(hwnd)) {
                st->pressed = true;
                SetCapture(hwnd);
                SetFocus(hwnd);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        case WM_LBUTTONUP:
            if (st && st->pressed) {
                st->pressed = false;
                ReleaseCapture();
                InvalidateRect(hwnd, nullptr, FALSE);
                // Only a release inside the button counts as a click.
                POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
                RECT rc;
                GetClientRect(hwnd, &rc);
                if (PtInRect(&rc, pt) && IsWindowEnabled(hwnd)) {
                    SendMessageW(GetParent(hwnd), WM_COMMAND,
                                 MAKEWPARAM(st->id, BN_CLICKED),
                                 reinterpret_cast<LPARAM>(hwnd));
                }
            }
            return 0;

        case WM_KEYDOWN:
            // Space and Enter activate, as a native button does.
            if ((wp == VK_SPACE || wp == VK_RETURN) && st && IsWindowEnabled(hwnd)) {
                SendMessageW(GetParent(hwnd), WM_COMMAND, MAKEWPARAM(st->id, BN_CLICKED),
                             reinterpret_cast<LPARAM>(hwnd));
                return 0;
            }
            break;

        case WM_SETFOCUS:
        case WM_KILLFOCUS:
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;

        // Let IsDialogMessage route keyboard navigation.
        case WM_GETDLGCODE:
            return DLGC_BUTTON | DLGC_WANTCHARS;

        case WM_ENABLE:
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;

        case WM_NCDESTROY:
            // Drop the accessibility annotation; HWNDs get reused.
            a11y::Clear(hwnd);
            if (st) {
                if (st->tip) DestroyWindow(st->tip);
                delete st;
                SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            }
            return 0;

        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// --------------------------------------------------------- checkbox painting
struct CheckState {
    theme::Color backdrop = theme::BgPanel;
    std::wstring label;
    int  id = 0;
    bool checked = false;
    bool hot = false;
    // Set on the button-DOWN this control received; see WM_LBUTTONUP.
    bool armed = false;
};

CheckState* ChkState(HWND h) {
    return reinterpret_cast<CheckState*>(GetWindowLongPtrW(h, GWLP_USERDATA));
}

void PaintCheckbox(HWND hwnd) {
    CheckState* st = ChkState(hwnd);
    if (!st) return;

    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd, &ps);
    RECT rc;
    GetClientRect(hwnd, &rc);
    const UINT dpi = theme::WindowDpi(hwnd);

    d2d::DcTarget& t = d2d::SharedDc();
    ID2D1RenderTarget* rt = t.Begin(dc, rc, dpi);
    if (rt) {
        const bool on = IsWindowEnabled(hwnd) != FALSE;
        const bool focus = (GetFocus() == hwnd);
        const D2D1_SIZE_F size = rt->GetSize();
        rt->Clear(theme::D2d(st->backdrop));

        constexpr float kBox = 14.0f;
        const float top = (size.height - kBox) * 0.5f;
        const D2D1_ROUNDED_RECT box = D2D1::RoundedRect(
            D2D1::RectF(0.5f, top + 0.5f, kBox + 0.5f, top + kBox + 0.5f), 3.0f, 3.0f);

        const theme::Color edge = !on ? theme::DisabledFg
                                      : ((st->hot || focus) ? theme::Accent : theme::FgSub);
        if (st->checked && on) {
            rt->FillRoundedRectangle(box, t.Brush(theme::Accent));
            // Tick drawn as two strokes so it scales with DPI and needs no font.
            ID2D1Brush* mark = t.Brush(theme::EntryFg);
            rt->DrawLine(D2D1::Point2F(3.5f, top + 7.5f), D2D1::Point2F(6.0f, top + 10.5f),
                         mark, 2.0f);
            rt->DrawLine(D2D1::Point2F(6.0f, top + 10.5f), D2D1::Point2F(11.0f, top + 4.0f),
                         mark, 2.0f);
        } else {
            rt->FillRoundedRectangle(box, t.Brush(theme::EntryBg));
        }
        rt->DrawRoundedRectangle(box, t.Brush(edge), 1.0f);

        // Separate focus ring, drawn within the box's bounds.
        if (on && focus) {
            const D2D1_ROUNDED_RECT ring = D2D1::RoundedRect(
                D2D1::RectF(1.0f, top + 1.0f, kBox, top + kBox), 2.5f, 2.5f);
            rt->DrawRoundedRectangle(
                ring, t.Brush(st->checked ? theme::EntryFg : theme::Accent), 2.0f);
        }

        DrawLabel(rt, st->label, theme::Font::Ui,
                  t.Brush(on ? theme::FgText : theme::DisabledFg),
                  D2D1::RectF(kBox + 7.0f, 0, size.width, size.height),
                  DWRITE_TEXT_ALIGNMENT_LEADING);
        t.End();
    }
    EndPaint(hwnd, &ps);
}

LRESULT CALLBACK CheckboxProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    CheckState* st = ChkState(hwnd);

    switch (msg) {
        case WM_NCCREATE: {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                              reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
            return TRUE;
        }
        case WM_PAINT:
            PaintCheckbox(hwnd);
            return 0;
        case WM_ERASEBKGND:
            return 1;

        case WM_MOUSEMOVE:
            if (st && !st->hot) {
                st->hot = true;
                TrackLeave(hwnd);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        case WM_MOUSELEAVE:
            if (st && st->hot) {
                st->hot = false;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;

        case WM_LBUTTONDOWN:
            // Arm, so only a press that started here toggles on release.
            if (st && IsWindowEnabled(hwnd)) {
                st->armed = true;
                SetCapture(hwnd);
                SetFocus(hwnd);
            }
            return 0;

        case WM_LBUTTONUP:
        case WM_KEYDOWN: {
            if (msg == WM_KEYDOWN && wp != VK_SPACE) break;
            // Ignore key auto-repeat so a held Space toggles once.
            if (msg == WM_KEYDOWN && (HIWORD(lp) & KF_REPEAT)) return 0;
            if (msg == WM_LBUTTONUP) {
                // Read `armed` before ReleaseCapture: WM_CAPTURECHANGED clears it.
                const bool was_armed = st && st->armed;
                if (st) st->armed = false;
                if (GetCapture() == hwnd) ReleaseCapture();
                if (!was_armed) return 0;
                // Released outside the control is a cancelled click, as on a push button.
                RECT rc;
                GetClientRect(hwnd, &rc);
                const POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
                if (!PtInRect(&rc, pt)) return 0;
            }
            if (st && IsWindowEnabled(hwnd)) {
                st->checked = !st->checked;
                if (msg == WM_LBUTTONUP) SetFocus(hwnd);
                a11y::SetChecked(hwnd, st->checked);
                a11y::NotifyStateChanged(hwnd);
                InvalidateRect(hwnd, nullptr, FALSE);
                SendMessageW(GetParent(hwnd), WM_COMMAND,
                             MAKEWPARAM(st->id, kCheckboxToggled),
                             reinterpret_cast<LPARAM>(hwnd));
            }
            return 0;
        }

        case WM_CAPTURECHANGED:
            if (st) st->armed = false;
            return 0;

        case WM_SETFOCUS:
        case WM_KILLFOCUS:
        case WM_ENABLE:
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;

        case WM_GETDLGCODE:
            return DLGC_BUTTON | DLGC_WANTCHARS;

        case WM_NCDESTROY:
            a11y::Clear(hwnd);
            if (st) {
                delete st;
                SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            }
            return 0;

        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---------------------------------------------------------- dropdown popup
// The dropdown list: a top-level popup that holds capture and closes on outside clicks.
struct PopupState {
    HWND owner = nullptr;   // the Dropdown window
    int  hot = -1;
    int  row_h = 0;
    // Guards against re-entry from WM_CAPTURECHANGED while closing.
    bool closing = false;
};

PopupState* PopState(HWND h) {
    return reinterpret_cast<PopupState*>(GetWindowLongPtrW(h, GWLP_USERDATA));
}

void PaintPopup(HWND hwnd) {
    PopupState* ps = PopState(hwnd);
    if (!ps) return;
    DropdownState* ds = DropState(ps->owner);
    if (!ds) return;

    PAINTSTRUCT p;
    HDC dc = BeginPaint(hwnd, &p);
    RECT rc;
    GetClientRect(hwnd, &rc);
    const UINT dpi = theme::WindowDpi(hwnd);

    d2d::DcTarget& t = d2d::SharedDc();
    ID2D1RenderTarget* rt = t.Begin(dc, rc, dpi);
    if (rt) {
        const D2D1_SIZE_F size = rt->GetSize();
        // Fill fully; the window region rounds the corners.
        constexpr float kPillRadius = 5.0f;
        constexpr float kPillInsetX = 4.0f;
        rt->Clear(theme::D2d(theme::ListBg));
        const D2D1_ROUNDED_RECT panel = D2D1::RoundedRect(
            D2D1::RectF(0.5f, 0.5f, size.width - 0.5f, size.height - 0.5f),
            kDropdownRadius, kDropdownRadius);
        rt->DrawRoundedRectangle(panel, t.Brush(theme::Border), 1.0f);

        const float row = static_cast<float>(ps->row_h) * 96.0f / theme::UiDpiF(dpi);
        for (size_t i = 0; i < ds->items.size(); ++i) {
            const float y = static_cast<float>(kPopupPadY96) + row * static_cast<float>(i);
            const bool sel = (static_cast<int>(i) == ds->index);
            const bool hot = (static_cast<int>(i) == ps->hot);

            // Hover gets a filled pill; the current selection a small bar.
            if (hot) {
                const D2D1_ROUNDED_RECT pill = D2D1::RoundedRect(
                    D2D1::RectF(kPillInsetX, y + 1.0f, size.width - kPillInsetX,
                                y + row - 1.0f),
                    kPillRadius, kPillRadius);
                rt->FillRoundedRectangle(pill, t.Brush(theme::Accent));
            } else if (sel) {
                const D2D1_ROUNDED_RECT bar = D2D1::RoundedRect(
                    D2D1::RectF(kPillInsetX, y + 5.0f, kPillInsetX + 3.0f,
                                y + row - 5.0f),
                    1.5f, 1.5f);
                rt->FillRoundedRectangle(bar, t.Brush(theme::Accent));
            }
            DrawLabel(rt, ds->items[i].label, theme::Font::Ui,
                      t.Brush(hot ? theme::EntryFg : (sel ? theme::EntryFg : theme::FgText)),
                      D2D1::RectF(static_cast<float>(kPopupPadX96), y,
                                  size.width - static_cast<float>(kPopupPadX96), y + row),
                      DWRITE_TEXT_ALIGNMENT_LEADING);
        }
        t.End();
    }
    EndPaint(hwnd, &p);
}

// Publishes the selected item's label as the accessible value.
void PublishDropdownValue(HWND hwnd, const DropdownState* ds) {
    if (!ds || ds->index < 0 || ds->index >= static_cast<int>(ds->items.size())) {
        a11y::SetValue(hwnd, std::wstring());
        return;
    }
    a11y::SetValue(hwnd, ds->items[ds->index].label);
}

// Changes the selection and notifies the parent, as a click would.
void SelectDropdownIndex(HWND hwnd, DropdownState* ds, int index) {
    if (!ds || index == ds->index || index < 0 ||
        index >= static_cast<int>(ds->items.size())) {
        return;
    }
    ds->index = index;
    PublishDropdownValue(hwnd, ds);
    a11y::NotifyValueChanged(hwnd);
    InvalidateRect(hwnd, nullptr, FALSE);
    SendMessageW(GetParent(hwnd), WM_COMMAND, MAKEWPARAM(ds->id, kDropdownChanged),
                 reinterpret_cast<LPARAM>(hwnd));
}

void ClosePopup(HWND popup, bool commit) {
    PopupState* ps = PopState(popup);
    if (!ps || ps->closing) return;
    ps->closing = true;
    HWND owner = ps->owner;
    DropdownState* ds = DropState(owner);
    const int choice = ps->hot;

    ReleaseCapture();
    DestroyWindow(popup);

    if (ds) {
        ds->open = false;
        ds->popup = nullptr;
        // Commit through SelectDropdownIndex, which also notifies.
        if (commit) SelectDropdownIndex(owner, ds, choice);
        InvalidateRect(owner, nullptr, FALSE);
    }
}

LRESULT CALLBACK PopupProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    PopupState* ps = PopState(hwnd);

    switch (msg) {
        case WM_NCCREATE: {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                              reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
            return TRUE;
        }
        case WM_PAINT:
            PaintPopup(hwnd);
            return 0;
        case WM_ERASEBKGND:
            return 1;

        case WM_MOUSEMOVE: {
            if (!ps || ps->row_h <= 0) return 0;
            // Offset by the same top padding the painter uses.
            const int y = GET_Y_LPARAM(lp) -
                          theme::Scale(kPopupPadY96, theme::WindowDpi(hwnd));
            const int idx = (y < 0) ? -1 : y / ps->row_h;
            DropdownState* ds = DropState(ps->owner);
            const int count = ds ? static_cast<int>(ds->items.size()) : 0;
            const int clamped = (idx >= 0 && idx < count) ? idx : -1;
            if (clamped != ps->hot) {
                ps->hot = clamped;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }

        // Only a release inside the list selects an item.
        case WM_LBUTTONUP: {
            POINT pt;
            GetCursorPos(&pt);
            RECT rc;
            GetWindowRect(hwnd, &rc);
            if (PtInRect(&rc, pt)) ClosePopup(hwnd, true);
            return 0;
        }

        // Capture is held, so a click outside the list dismisses it.
        case WM_LBUTTONDOWN:
        case WM_NCLBUTTONDOWN:
        case WM_RBUTTONDOWN: {
            POINT pt;
            GetCursorPos(&pt);
            RECT rc;
            GetWindowRect(hwnd, &rc);
            if (!PtInRect(&rc, pt)) ClosePopup(hwnd, false);
            return 0;
        }

        case WM_CAPTURECHANGED:
            // Lost the mouse capture: close without choosing.
            ClosePopup(hwnd, false);
            return 0;

        case WM_KEYDOWN:
            if (wp == VK_ESCAPE) { ClosePopup(hwnd, false); return 0; }
            if (wp == VK_RETURN) { ClosePopup(hwnd, true); return 0; }
            if ((wp == VK_DOWN || wp == VK_UP) && ps) {
                DropdownState* ds = DropState(ps->owner);
                const int count = ds ? static_cast<int>(ds->items.size()) : 0;
                if (count) {
                    int next = (ps->hot < 0) ? (ds ? ds->index : 0)
                                             : ps->hot + (wp == VK_DOWN ? 1 : -1);
                    ps->hot = (std::max)(0, (std::min)(count - 1, next));
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
                return 0;
            }
            return 0;

        case WM_NCDESTROY:
            if (ps) {
                delete ps;
                SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            }
            return 0;

        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---------------------------------------------------------- dropdown painting
void PaintDropdown(HWND hwnd) {
    DropdownState* ds = DropState(hwnd);
    if (!ds) return;

    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd, &ps);
    RECT rc;
    GetClientRect(hwnd, &rc);
    const UINT dpi = theme::WindowDpi(hwnd);

    d2d::DcTarget& t = d2d::SharedDc();
    ID2D1RenderTarget* rt = t.Begin(dc, rc, dpi);
    if (rt) {
        const bool on = IsWindowEnabled(hwnd) != FALSE;
        const bool focus = (GetFocus() == hwnd);
        const D2D1_SIZE_F size = rt->GetSize();
        rt->Clear(theme::D2d(ds->backdrop));

        DrawInputFrame(rt, t, D2D1::RectF(0, 0, size.width, size.height),
                       focus || ds->hot || ds->open, on);

        const std::wstring& label =
            (ds->index >= 0 && ds->index < static_cast<int>(ds->items.size()))
                ? ds->items[ds->index].label
                : std::wstring();
        DrawLabel(rt, label, theme::Font::Ui,
                  t.Brush(on ? theme::EntryFg : theme::DisabledFg),
                  D2D1::RectF(8.0f, 0, size.width - 24.0f, size.height),
                  DWRITE_TEXT_ALIGNMENT_LEADING);

        // Chevron, drawn as two strokes so it scales cleanly with DPI.
        const float cx = size.width - 14.0f;
        const float cy = size.height * 0.5f;
        ID2D1Brush* arrow = t.Brush(on ? theme::FgText : theme::DisabledFg);
        rt->DrawLine(D2D1::Point2F(cx - 4.0f, cy - 2.0f), D2D1::Point2F(cx, cy + 2.5f),
                     arrow, 1.6f);
        rt->DrawLine(D2D1::Point2F(cx, cy + 2.5f), D2D1::Point2F(cx + 4.0f, cy - 2.0f),
                     arrow, 1.6f);
        t.End();
    }
    EndPaint(hwnd, &ps);
}

void OpenPopup(HWND hwnd) {
    DropdownState* ds = DropState(hwnd);
    if (!ds || ds->items.empty() || ds->open) return;

    RECT rc;
    GetWindowRect(hwnd, &rc);
    const UINT dpi = theme::WindowDpi(hwnd);
    // Row height and padding shared with the context menu.
    const int row_h = theme::Scale(kPopupItemH96, dpi);
    const int height = static_cast<int>(ds->items.size()) * row_h +
                       theme::Scale(kPopupPadY96, dpi) * 2;

    auto* ps = new PopupState();
    ps->owner = hwnd;
    ps->hot = ds->index;
    ps->row_h = row_h;

    // Keep the list on the monitor; open above the control if it does not fit below.
    int px = rc.left;
    int py = rc.bottom + 2;
    const int pw = rc.right - rc.left;
    MONITORINFO mi = {sizeof(mi)};
    if (GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi)) {
        if (py + height > mi.rcWork.bottom) {
            const int above = rc.top - 2 - height;
            py = (above >= mi.rcWork.top) ? above
                                          : (std::max)(mi.rcWork.top, mi.rcWork.bottom - height);
        }
        if (px + pw > mi.rcWork.right) px = (std::max)(mi.rcWork.left, mi.rcWork.right - pw);
        if (px < mi.rcWork.left) px = mi.rcWork.left;
    }

    // Tool window: no taskbar entry, and showing it does not activate it.
    HWND popup = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST, kPopupClass, L"", WS_POPUP,
        px, py, pw, height,
        GetParent(hwnd), nullptr, GetModuleHandleW(nullptr), ps);
    if (!popup) { delete ps; return; }

    ds->open = true;
    ds->popup = popup;
    ApplyRoundedRgn(popup, rc.right - rc.left, height, dpi);
    ShowWindow(popup, SW_SHOWNOACTIVATE);
    SetCapture(popup);
    InvalidateRect(hwnd, nullptr, FALSE);
}

LRESULT CALLBACK DropdownProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    DropdownState* ds = DropState(hwnd);

    switch (msg) {
        case WM_NCCREATE: {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                              reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
            return TRUE;
        }
        case WM_PAINT:
            PaintDropdown(hwnd);
            return 0;
        case WM_ERASEBKGND:
            return 1;

        case WM_MOUSEMOVE:
            if (ds && !ds->hot) {
                ds->hot = true;
                TrackLeave(hwnd);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        case WM_MOUSELEAVE:
            if (ds && ds->hot) {
                ds->hot = false;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;

        case WM_LBUTTONDOWN:
            if (IsWindowEnabled(hwnd)) {
                SetFocus(hwnd);
                OpenPopup(hwnd);
            }
            return 0;

        case WM_KEYDOWN:
        case WM_CHAR: {
            if (!ds || !IsWindowEnabled(hwnd)) break;

            // While the list is open, forward keys to it.
            if (ds->open && ds->popup && IsWindow(ds->popup)) {
                SendMessageW(ds->popup, msg, wp, lp);
                return 0;
            }
            if (msg == WM_CHAR) {
                // Type-to-select: jump to the next item starting with that letter.
                const wchar_t ch = static_cast<wchar_t>(wp);
                if (ch > L' ') {
                    const int count = static_cast<int>(ds->items.size());
                    for (int k = 1; k <= count; ++k) {
                        const int cand = (ds->index + k) % count;
                        const std::wstring& label = ds->items[cand].label;
                        if (!label.empty() && towlower(label[0]) == towlower(ch)) {
                            SelectDropdownIndex(hwnd, ds, cand);
                            break;
                        }
                    }
                    return 0;
                }
                break;
            }
            if (wp == VK_SPACE || wp == VK_RETURN ||
                (wp == VK_DOWN && (GetKeyState(VK_MENU) & 0x8000))) {
                OpenPopup(hwnd);
                return 0;
            }
            // Arrow keys step the selection while the list is closed.
            if (wp == VK_DOWN || wp == VK_UP || wp == VK_HOME || wp == VK_END) {
                const int count = static_cast<int>(ds->items.size());
                if (count > 0) {
                    int next = ds->index;
                    if (wp == VK_DOWN)      next = (std::min)(count - 1, ds->index + 1);
                    else if (wp == VK_UP)   next = (std::max)(0, ds->index - 1);
                    else if (wp == VK_HOME) next = 0;
                    else                    next = count - 1;
                    SelectDropdownIndex(hwnd, ds, next);
                }
                return 0;
            }
            break;
        }

        case WM_SETFOCUS:
        case WM_KILLFOCUS:
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;

        case WM_GETDLGCODE: {
            // While the list is open, claim Escape and Enter for it.
            LRESULT code = DLGC_WANTARROWS | DLGC_WANTCHARS;
            const MSG* m = reinterpret_cast<const MSG*>(lp);
            if (ds && ds->open && m && m->message == WM_KEYDOWN &&
                (m->wParam == VK_ESCAPE || m->wParam == VK_RETURN)) {
                code |= DLGC_WANTMESSAGE;
            }
            return code;
        }

        case WM_ENABLE:
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;

        case WM_NCDESTROY:
            a11y::Clear(hwnd);
            if (ds) {
                delete ds;
                SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            }
            return 0;

        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ------------------------------------------------------------ Input subclass
// Rewrites a numeric field to the plain spelling of its clamped value.
void SettleNumeric(HWND hwnd, const Input* owner) {
    if (!owner || !owner->numeric()) return;
    wchar_t buf[32] = {};
    GetWindowTextW(hwnd, buf, 32);
    const long v = wcstol(buf, nullptr, 10);
    const long lo = owner->lo();
    const long hi = owner->hi();
    const long c = (v < lo) ? lo : ((v > hi) ? hi : v);
    wchar_t out[32] = {};
    _snwprintf_s(out, _TRUNCATE, L"%ld", c);
    // Only rewrite when it differs, to keep the caret.
    if (wcscmp(buf, out) != 0) {
        SetWindowTextW(hwnd, out);
        SendMessageW(GetParent(hwnd), WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(hwnd), EN_CHANGE),
                     reinterpret_cast<LPARAM>(hwnd));
    }
}

// EDIT subclass: reports Enter to the parent and repaints the parent's frame
// on focus changes.
LRESULT CALLBACK InputProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                           UINT_PTR, DWORD_PTR ref) {
    // The owning Input, passed as the subclass reference data.
    Input* owner = reinterpret_cast<Input*>(ref);
    switch (msg) {
        case WM_KEYDOWN:
            if (wp == VK_RETURN) {
                // Settle first, so the parent sees the corrected value before Enter.
                SettleNumeric(hwnd, owner);
                SendMessageW(GetParent(hwnd), WM_COMMAND,
                             MAKEWPARAM(GetDlgCtrlID(hwnd), kInputAccept),
                             reinterpret_cast<LPARAM>(hwnd));
                return 0;
            }
            break;
        case WM_CHAR:
            // Swallow the Enter character so the EDIT does not beep.
            if (wp == VK_RETURN) return 0;
            break;
        case WM_SETFOCUS:
        case WM_KILLFOCUS: {
            // The frame is painted by the parent, so repaint the whole parent.
            const LRESULT r = DefSubclassProc(hwnd, msg, wp, lp);

            // Leaving a numeric field corrects it.
            if (msg == WM_KILLFOCUS) SettleNumeric(hwnd, owner);

            if (HWND parent = GetParent(hwnd)) InvalidateRect(parent, nullptr, FALSE);
            return r;
        }
        default:
            break;
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

}  // namespace

void RegisterControlClasses() {
    static bool done = false;
    if (done) return;
    done = true;

    HINSTANCE inst = GetModuleHandleW(nullptr);

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.style = CS_HREDRAW | CS_VREDRAW;

    wc.lpfnWndProc = ButtonProc;
    wc.lpszClassName = kButtonClass;
    RegisterClassExW(&wc);

    wc.lpfnWndProc = DropdownProc;
    wc.lpszClassName = kDropdownClass;
    RegisterClassExW(&wc);

    wc.lpfnWndProc = CheckboxProc;
    wc.lpszClassName = kCheckClass;
    RegisterClassExW(&wc);

    wc.lpfnWndProc = PopupProc;
    wc.lpszClassName = kPopupClass;
    // CS_DROPSHADOW gives the list the same lift a native menu has.
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DROPSHADOW;
    RegisterClassExW(&wc);
}

// ------------------------------------------------------------------- Button
bool Button::Create(HWND parent, int id, const std::wstring& text, Variant v) {
    RegisterControlClasses();
    auto* st = new ButtonState();
    st->text = text;
    st->variant = v;
    st->id = id;
    hwnd_ = CreateWindowExW(0, kButtonClass, text.c_str(),
                            WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, 0, 10, 10,
                            parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                            GetModuleHandleW(nullptr), st);
    if (!hwnd_) { delete st; return false; }
    // Custom classes get no accessible name from window text, so set it explicitly.
    a11y::Annotate(hwnd_, text, a11y::Role::PushButton);
    return true;
}

void Button::SetText(const std::wstring& text) {
    if (ButtonState* st = BtnState(hwnd_)) {
        // Skip when unchanged, avoiding a repaint and a screen-reader announcement.
        if (st->text == text) return;
        st->text = text;
        // Keep the window text and accessible name in sync with the caption.
        SetWindowTextW(hwnd_, text.c_str());
        a11y::SetName(hwnd_, text);
        a11y::NotifyNameChanged(hwnd_);
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void Button::SetVariant(Variant v) {
    if (ButtonState* st = BtnState(hwnd_)) {
        st->variant = v;
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void Button::SetGlyph(Glyph g) {
    ButtonState* st = BtnState(hwnd_);
    if (!st || st->glyph == g) return;
    st->glyph = g;
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void Button::SetIconStyle(bool on) {
    if (ButtonState* st = BtnState(hwnd_)) {
        st->icon = on;
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void Button::SetEnabled(bool on) {
    if (!hwnd_) return;
    // Clear hover and press state when disabling.
    if (!on) {
        if (ButtonState* st = BtnState(hwnd_)) { st->hot = false; st->pressed = false; }
    }
    EnableWindow(hwnd_, on);
}

bool Button::enabled() const { return hwnd_ && IsWindowEnabled(hwnd_); }

void Button::SetToolTip(const std::wstring& text) {
    ButtonState* st = BtnState(hwnd_);
    if (!st) return;
    if (!st->tip) {
        st->tip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr,
                                  WS_POPUP | TTS_ALWAYSTIP, 0, 0, 0, 0,
                                  hwnd_, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (!st->tip) return;
    }
    TOOLINFOW ti = {};
    ti.cbSize = sizeof(ti);
    ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
    ti.hwnd = GetParent(hwnd_);
    ti.uId = reinterpret_cast<UINT_PTR>(hwnd_);
    ti.lpszText = const_cast<LPWSTR>(text.c_str());
    // Add the tool once, update its text afterwards.
    if (st->tip_added) {
        SendMessageW(st->tip, TTM_UPDATETIPTEXTW, 0, reinterpret_cast<LPARAM>(&ti));
    } else {
        SendMessageW(st->tip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&ti));
        st->tip_added = true;
    }

    // For icon buttons the tooltip also serves as the accessible name.
    if (st->icon) a11y::SetName(hwnd_, text);
}

// SWP_NOCOPYBITS repaints a moved control instead of copying its old pixels.
constexpr UINT kMoveFlags = SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS;

void Button::Move(int x, int y, int w, int h) {
    if (hwnd_) SetWindowPos(hwnd_, nullptr, x, y, w, h, kMoveFlags);
}

// -------------------------------------------------------------------- Input
bool Input::Create(HWND parent, int id, bool numeric, int lo, int hi) {
    numeric_ = numeric;
    lo_ = lo;
    hi_ = hi;
    // No WS_BORDER: the parent paints the rounded frame.
    DWORD style = WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | ES_LEFT;
    hwnd_ = CreateWindowExW(0, L"EDIT", L"", style, 0, 0, 10, 10, parent,
                            reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                            GetModuleHandleW(nullptr), nullptr);
    if (!hwnd_) return false;
    SetWindowSubclass(hwnd_, InputProc, 0, reinterpret_cast<DWORD_PTR>(this));
    SendMessageW(hwnd_, WM_SETFONT,
                 reinterpret_cast<WPARAM>(theme::UiFontHandle(theme::WindowDpi(parent))),
                 TRUE);
    return true;
}

void Input::SetRange(int lo, int hi) {
    lo_ = lo;
    hi_ = hi;
    if (!numeric_ || !hwnd_) return;
    // Clamp the current text into the new range; an empty field is left alone.
    if (GetWindowTextLengthW(hwnd_) == 0) return;
    SetValue(Value());
}

bool Input::IsInputEdit(HWND hwnd) {
    if (!hwnd) return false;
    // Any EDIT in this app is an Input field.
    wchar_t cls[16] = {};
    if (!GetClassNameW(hwnd, cls, static_cast<int>(std::size(cls)))) return false;
    return _wcsicmp(cls, L"Edit") == 0;
}

void Button::SetBackdrop(theme::Color c) {
    if (ButtonState* st = BtnState(hwnd_)) {
        st->backdrop = c;
        if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void Checkbox::SetBackdrop(theme::Color c) {
    if (CheckState* st = ChkState(hwnd_)) {
        st->backdrop = c;
        if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void Input::Move(int x, int y, int w, int h) {
    if (hwnd_) SetWindowPos(hwnd_, nullptr, x, y, w, h, kMoveFlags);
}

void Input::SetEnabled(bool on) {
    if (hwnd_) EnableWindow(hwnd_, on);
}

std::wstring Input::Text() const {
    if (!hwnd_) return std::wstring();
    const int n = GetWindowTextLengthW(hwnd_);
    if (n <= 0) return std::wstring();
    std::wstring s(static_cast<size_t>(n), L'\0');
    GetWindowTextW(hwnd_, &s[0], n + 1);
    return s;
}

void Input::SetText(const std::wstring& s) {
    if (hwnd_) SetWindowTextW(hwnd_, s.c_str());
}

int Input::Value() const {
    const std::wstring s = Text();
    // Lenient: partial or empty text falls back to the low bound.
    const long v = wcstol(s.c_str(), nullptr, 10);
    return static_cast<int>((std::max)(static_cast<long>(lo_),
                                       (std::min)(static_cast<long>(hi_), v)));
}

void Input::SetValue(int v) {
    SetText(core::Fmt(L"%d", (std::max)(lo_, (std::min)(hi_, v))));
}

void Input::Settle() {
    if (hwnd_) SettleNumeric(hwnd_, this);
}

// ----------------------------------------------------------------- Dropdown
bool Dropdown::Create(HWND parent, int id, std::vector<Item> items) {
    RegisterControlClasses();
    auto* ds = new DropdownState();
    ds->items = std::move(items);
    ds->id = id;
    hwnd_ = CreateWindowExW(0, kDropdownClass, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                            0, 0, 10, 10, parent,
                            reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                            GetModuleHandleW(nullptr), ds);
    if (!hwnd_) { delete ds; return false; }
    // Accessible role; the value tracks the selection.
    a11y::SetRole(hwnd_, a11y::Role::ComboBox);
    PublishDropdownValue(hwnd_, ds);
    return true;
}

void Dropdown::Move(int x, int y, int w, int h) {
    if (hwnd_) SetWindowPos(hwnd_, nullptr, x, y, w, h, kMoveFlags);
}

void Dropdown::SetEnabled(bool on) {
    if (hwnd_) EnableWindow(hwnd_, on);
}

int Dropdown::SelectedData() const {
    DropdownState* ds = DropState(hwnd_);
    if (!ds || ds->index < 0 || ds->index >= static_cast<int>(ds->items.size())) return 0;
    return ds->items[ds->index].data;
}

void Dropdown::SetBackdrop(theme::Color c) {
    DropdownState* ds = DropState(hwnd_);
    if (!ds || ds->backdrop == c) return;
    ds->backdrop = c;
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void Dropdown::SetItems(std::vector<Item> items) {
    DropdownState* ds = DropState(hwnd_);
    if (!ds) return;
    const int keep = SelectedData();
    ds->items = std::move(items);
    // Restore the selection by payload, or fall back to the first item.
    ds->index = 0;
    for (size_t i = 0; i < ds->items.size(); ++i) {
        if (ds->items[i].data == keep) { ds->index = static_cast<int>(i); break; }
    }
    if (ds->items.empty()) ds->index = -1;
    PublishDropdownValue(hwnd_, ds);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void Dropdown::CloseList() {
    DropdownState* ds = DropState(hwnd_);
    // Close without committing.
    if (ds && ds->open && ds->popup && IsWindow(ds->popup)) ClosePopup(ds->popup, false);
}

void Dropdown::SelectData(int data) {
    DropdownState* ds = DropState(hwnd_);
    if (!ds) return;
    for (size_t i = 0; i < ds->items.size(); ++i) {
        if (ds->items[i].data == data) {
            if (ds->index != static_cast<int>(i)) {
                ds->index = static_cast<int>(i);
                PublishDropdownValue(hwnd_, ds);
                a11y::NotifyValueChanged(hwnd_);
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            return;
        }
    }
}

// ----------------------------------------------------------------- Checkbox
bool Checkbox::Create(HWND parent, int id, const std::wstring& label, bool checked) {
    RegisterControlClasses();
    auto* st = new CheckState();
    st->label = label;
    st->id = id;
    st->checked = checked;
    hwnd_ = CreateWindowExW(0, kCheckClass, label.c_str(),
                            WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, 0, 10, 10, parent,
                            reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                            GetModuleHandleW(nullptr), st);
    if (!hwnd_) { delete st; return false; }
    // Custom class: set the accessible name and role explicitly.
    a11y::Annotate(hwnd_, label, a11y::Role::CheckBox);
    // Expose the checked state to assistive technology.
    a11y::SetChecked(hwnd_, checked);
    return true;
}

void Checkbox::Move(int x, int y, int w, int h) {
    if (hwnd_) SetWindowPos(hwnd_, nullptr, x, y, w, h, kMoveFlags);
}

void Checkbox::SetEnabled(bool on) {
    if (!hwnd_) return;
    // Clear hover when disabling.
    if (!on) {
        if (CheckState* st = ChkState(hwnd_)) st->hot = false;
    }
    EnableWindow(hwnd_, on);
}

bool Checkbox::checked() const {
    CheckState* st = ChkState(hwnd_);
    return st && st->checked;
}

void Checkbox::SetChecked(bool on) {
    CheckState* st = ChkState(hwnd_);
    if (!st || st->checked == on) return;
    st->checked = on;
    // Keep the accessible checked state in sync.
    a11y::SetChecked(hwnd_, on);
    a11y::NotifyStateChanged(hwnd_);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void Checkbox::SetLabel(const std::wstring& label) {
    CheckState* st = ChkState(hwnd_);
    if (!st || st->label == label) return;
    st->label = label;
    // Update window text, accessible name and painting.
    SetWindowTextW(hwnd_, label.c_str());
    a11y::Annotate(hwnd_, label, a11y::Role::CheckBox);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

std::wstring ShownCaption(HWND hwnd) {
    if (!hwnd) return std::wstring();
    // Check the class first; each stores a different state struct.
    wchar_t cls[32] = {};
    GetClassNameW(hwnd, cls, static_cast<int>(std::size(cls)));
    if (wcscmp(cls, kButtonClass) == 0) {
        const ButtonState* st = BtnState(hwnd);
        return st ? st->text : std::wstring();
    }
    if (wcscmp(cls, kCheckClass) == 0) {
        const CheckState* st = ChkState(hwnd);
        return st ? st->label : std::wstring();
    }
    if (wcscmp(cls, kDropdownClass) == 0) {
        const DropdownState* ds = DropState(hwnd);
        if (!ds || ds->index < 0 || ds->index >= static_cast<int>(ds->items.size())) {
            return std::wstring();
        }
        return ds->items[ds->index].label;
    }
    const int n = GetWindowTextLengthW(hwnd);
    if (n <= 0) return std::wstring();
    std::wstring s(static_cast<size_t>(n), L'\0');
    GetWindowTextW(hwnd, &s[0], n + 1);
    return s;
}

// --------------------------------------------------------- shared paint bits
bool SetClipboardText(HWND owner, const std::wstring& text) {
    if (text.empty()) return false;
    if (!OpenClipboard(owner)) return false;
    bool ok = false;
    EmptyClipboard();
    const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    if (HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes)) {
        if (void* dst = GlobalLock(mem)) {
            memcpy(dst, text.c_str(), bytes);
            GlobalUnlock(mem);
            // The clipboard owns mem on success; free it only on failure.
            ok = SetClipboardData(CF_UNICODETEXT, mem) != nullptr;
            if (!ok) GlobalFree(mem);
        } else {
            GlobalFree(mem);
        }
    }
    CloseClipboard();
    return ok;
}

void DrawInputFrame(ID2D1RenderTarget* rt, d2d::Brushes& brushes,
                    const D2D1_RECT_F& r, bool focused, bool enabled) {
    // Inset by one pixel so the stroke stays inside the rect.
    const float inset = 1.0f;
    const D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(
        D2D1::RectF(r.left + inset, r.top + inset, r.right - inset, r.bottom - inset),
        kFieldRadius, kFieldRadius);
    // Disabled fields use the panel colour.
    rt->FillRoundedRectangle(rr, brushes.Brush(enabled ? theme::EntryBg : theme::BgPanel));
    rt->DrawRoundedRectangle(rr, brushes.Brush(focused ? theme::Accent : theme::Border),
                             1.0f);
}

}  // namespace ui
