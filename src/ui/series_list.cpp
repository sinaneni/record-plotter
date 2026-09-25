// Series list: drawing, scrolling, tooltips and input.
#include "ui/series_list.h"

#include <commctrl.h>
#include <windowsx.h>

#include <algorithm>

#include "ui/a11y.h"
#include "ui/d2d.h"
#include "ui/theme.h"

namespace ui {
namespace {

const wchar_t kClass[] = L"RpSeriesList";
constexpr int   kRowH96 = 26;
constexpr float kBox = 14.0f;   // checkbox size
constexpr float kBadgeW = 40.0f;
constexpr float kBadgeH = 16.0f;
constexpr float kScrollW = 8.0f;   // scroll indicator room

}  // namespace

bool SeriesList::Create(HWND parent, int id) {
    static bool registered = false;
    HINSTANCE inst = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(parent, GWLP_HINSTANCE));
    if (!registered) {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = Proc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = theme::SolidBrush(theme::BgPanel);
        wc.lpszClassName = kClass;
        if (!RegisterClassExW(&wc)) return false;
        registered = true;
    }
    id_ = id;
    hwnd_ = CreateWindowExW(0, kClass, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, 0, 0, 0,
                            parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), inst,
                            this);
    if (!hwnd_) return false;
    dpi_ = theme::WindowDpi(hwnd_);
    a11y::Annotate(hwnd_, L"Series", a11y::Role::List);
    return true;
}

void SeriesList::Move(int x, int y, int w, int h) {
    MoveWindow(hwnd_, x, y, w, h, TRUE);
    ScrollTo(scroll_px_);
}

void SeriesList::SetRows(std::vector<Row> rows) {
    rows_ = std::move(rows);
    scroll_px_ = 0;
    hot_ = -1;
    tip_row_ = -2;
    focus_row_ = 0;
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void SeriesList::SetAxes(const std::vector<bool>& right) {
    for (size_t i = 0; i < rows_.size() && i < right.size(); ++i) rows_[i].right = right[i];
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void SeriesList::UpdateTip() {
    const bool show = hot_ >= 0 && static_cast<size_t>(hot_) < clipped_.size() &&
                      clipped_[static_cast<size_t>(hot_)];
    const int want = show ? hot_ : -1;
    if (want == tip_row_) return;
    tip_row_ = want;
    if (!tip_) {
        tip_ = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP |
                               TTS_NOPREFIX, 0, 0, 0, 0, hwnd_, nullptr, GetModuleHandleW(nullptr),
                               nullptr);
        if (!tip_) return;
        TOOLINFOW ti = {};
        ti.cbSize = sizeof(ti);
        ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
        ti.hwnd = hwnd_;
        ti.uId = reinterpret_cast<UINT_PTR>(hwnd_);
        ti.lpszText = const_cast<LPWSTR>(L"");
        SendMessageW(tip_, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&ti));
        SendMessageW(tip_, TTM_SETMAXTIPWIDTH, 0, 600);
    }
    // Show the new row's name, re-popping the tip over it.
    TOOLINFOW ti = {};
    ti.cbSize = sizeof(ti);
    ti.hwnd = hwnd_;
    ti.uId = reinterpret_cast<UINT_PTR>(hwnd_);
    ti.lpszText = const_cast<LPWSTR>(show ? rows_[static_cast<size_t>(hot_)].name.c_str() : L"");
    SendMessageW(tip_, TTM_POP, 0, 0);
    SendMessageW(tip_, TTM_UPDATETIPTEXTW, 0, reinterpret_cast<LPARAM>(&ti));
}

void SeriesList::SetAll(bool checked) {
    for (Row& r : rows_) r.checked = checked;
    InvalidateRect(hwnd_, nullptr, FALSE);
}

int SeriesList::RowPx() const { return theme::Scale(kRowH96, dpi_); }

int SeriesList::RowAt(int y_px) const {
    const int r = (y_px + scroll_px_) / (std::max)(1, RowPx());
    return (r >= 0 && r < static_cast<int>(rows_.size())) ? r : -1;
}

void SeriesList::ScrollTo(int top_px) {
    RECT rc = {};
    GetClientRect(hwnd_, &rc);
    const int content = RowPx() * static_cast<int>(rows_.size());
    const int max_top = (std::max)(0, content - static_cast<int>(rc.bottom));
    scroll_px_ = (std::max)(0, (std::min)(top_px, max_top));
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void SeriesList::Toggle(int row) {
    if (row < 0 || row >= static_cast<int>(rows_.size())) return;
    rows_[static_cast<size_t>(row)].checked = !rows_[static_cast<size_t>(row)].checked;
    focus_row_ = row;
    InvalidateRect(hwnd_, nullptr, FALSE);
    SendMessageW(GetParent(hwnd_), WM_COMMAND, MAKEWPARAM(id_, kSeriesToggled),
                 reinterpret_cast<LPARAM>(hwnd_));
}

void SeriesList::SetAxis(int row, bool right) {
    if (row < 0 || row >= static_cast<int>(rows_.size())) return;
    Row& r = rows_[static_cast<size_t>(row)];
    focus_row_ = row;
    if (r.right == right) return;
    r.right = right;
    InvalidateRect(hwnd_, nullptr, FALSE);
    SendMessageW(GetParent(hwnd_), WM_COMMAND, MAKEWPARAM(id_, kSeriesAxisChanged),
                 reinterpret_cast<LPARAM>(hwnd_));
}

float SeriesList::BadgeLeft() const {
    RECT rc = {};
    GetClientRect(hwnd_, &rc);
    const float width = static_cast<float>(rc.right) * 96.0f / theme::UiDpiF(dpi_);
    return width - kScrollW - 4.0f - kBadgeW;
}

void SeriesList::Paint() {
    d2d::Frame frame(hwnd_, dpi_);
    ID2D1RenderTarget* rt = frame.rt();
    if (!rt) return;
    d2d::DcTarget& t = frame.brushes();
    const D2D1_SIZE_F size = frame.size();
    rt->Clear(theme::D2d(theme::BgPanel));

    const float scale = 96.0f / theme::UiDpiF(dpi_);
    const float row_h = static_cast<float>(kRowH96);
    const float scroll = static_cast<float>(scroll_px_) * scale;
    const bool focused = GetFocus() == hwnd_;

    if (rows_.empty()) {
        IDWriteTextFormat* fmt = theme::Text(theme::Font::Ui);
        const wchar_t* msg = L"No numeric columns to plot.";
        rt->DrawTextW(msg, static_cast<UINT32>(wcslen(msg)), fmt,
                      D2D1::RectF(0, 0, size.width, row_h), t.Brush(theme::FgSub));
        return;
    }

    const bool has_scrollbar = row_h * static_cast<float>(rows_.size()) > size.height + 0.5f;
    clipped_.assign(rows_.size(), false);
    const float right = size.width - (has_scrollbar ? 8.0f : 0.0f);
    for (size_t i = 0; i < rows_.size(); ++i) {
        const float top = row_h * static_cast<float>(i) - scroll;
        if (top + row_h < 0) continue;
        if (top > size.height) break;
        const Row& r = rows_[i];
        if (static_cast<int>(i) == hot_) {
            rt->FillRoundedRectangle(
                D2D1::RoundedRect(D2D1::RectF(0, top + 1, right, top + row_h - 1), 5, 5),
                t.Brush(theme::SelectBg));
        }
        const float bt = top + (row_h - kBox) * 0.5f;
        const float bx = 6.0f;
        const D2D1_ROUNDED_RECT box = D2D1::RoundedRect(
            D2D1::RectF(bx + 0.5f, bt + 0.5f, bx + kBox + 0.5f, bt + kBox + 0.5f), 3.0f, 3.0f);
        const bool ring = focused && static_cast<int>(i) == focus_row_;
        if (r.checked) {
            rt->FillRoundedRectangle(box, t.Brush(theme::Accent));
            ID2D1Brush* mark = t.Brush(theme::EntryFg);
            rt->DrawLine(D2D1::Point2F(bx + 3.5f, bt + 7.5f), D2D1::Point2F(bx + 6.0f, bt + 10.5f),
                         mark, 2.0f);
            rt->DrawLine(D2D1::Point2F(bx + 6.0f, bt + 10.5f), D2D1::Point2F(bx + 11.0f, bt + 4.0f),
                         mark, 2.0f);
        } else {
            rt->FillRoundedRectangle(box, t.Brush(theme::EntryBg));
        }
        rt->DrawRoundedRectangle(
            box, t.Brush((ring || static_cast<int>(i) == hot_) ? theme::Accent : theme::FgSub), 1.0f);

        // Colour swatch.
        ID2D1SolidColorBrush* sw = nullptr;
        rt->CreateSolidColorBrush(D2D1::ColorF(r.rgb, r.checked ? 1.0f : 0.35f), &sw);
        if (sw) {
            const float sx = bx + kBox + 9.0f;
            rt->FillRoundedRectangle(
                D2D1::RoundedRect(D2D1::RectF(sx, top + row_h * 0.5f - 2.0f, sx + 16.0f,
                                              top + row_h * 0.5f + 2.0f), 2.0f, 2.0f), sw);
            sw->Release();
        }
        // Axis badge; the right axis is drawn in the accent colour.
        const float bl = BadgeLeft();
        const D2D1_ROUNDED_RECT badge = D2D1::RoundedRect(
            D2D1::RectF(bl, top + (row_h - kBadgeH) * 0.5f, bl + kBadgeW,
                        top + (row_h + kBadgeH) * 0.5f), 4.0f, 4.0f);
        const theme::Color bc = r.right ? theme::Accent : theme::FgSub;
        rt->DrawRoundedRectangle(badge, t.Brush(bc, r.right ? 1.0f : 0.45f), 1.0f);
        IDWriteTextFormat* tag = theme::Text(theme::Font::Subtitle, DWRITE_TEXT_ALIGNMENT_CENTER);
        const wchar_t* side = r.right ? L"RIGHT" : L"LEFT";
        rt->DrawTextW(side, static_cast<UINT32>(wcslen(side)), tag, badge.rect,
                      t.Brush(bc, r.right ? 1.0f : 0.7f));

        // Name, trimmed with an ellipsis on its own text layout.
        IDWriteTextFormat* fmt = theme::Text(theme::Font::Ui);
        const float tx = bx + kBox + 33.0f;
        const float tw = (std::max)(1.0f, bl - 6.0f - tx);
        IDWriteTextLayout* layout = nullptr;
        if (fmt && SUCCEEDED(d2d::WriteFactory()->CreateTextLayout(
                       r.name.c_str(), static_cast<UINT32>(r.name.size()), fmt, tw, row_h,
                       &layout))) {
            IDWriteInlineObject* dots = nullptr;
            d2d::WriteFactory()->CreateEllipsisTrimmingSign(layout, &dots);
            const DWRITE_TRIMMING trim = {DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
            layout->SetTrimming(&trim, dots);
            // Measure the untrimmed width to see if the name was cut.
            IDWriteTextLayout* probe = nullptr;
            bool cut = false;
            if (SUCCEEDED(d2d::WriteFactory()->CreateTextLayout(
                    r.name.c_str(), static_cast<UINT32>(r.name.size()), fmt, 10000.0f, row_h,
                    &probe))) {
                DWRITE_TEXT_METRICS pm = {};
                probe->GetMetrics(&pm);
                cut = pm.width > tw + 0.5f;
                probe->Release();
            }
            if (i < clipped_.size()) clipped_[i] = cut;
            rt->DrawTextLayout(D2D1::Point2F(tx, top), layout,
                               t.Brush(r.checked ? theme::FgText : theme::FgSub),
                               D2D1_DRAW_TEXT_OPTIONS_CLIP);
            if (dots) dots->Release();
            layout->Release();
        }
    }

    // Thin scroll indicator.
    if (has_scrollbar) {
        const float content = row_h * static_cast<float>(rows_.size());
        const float h = (std::max)(24.0f, size.height * size.height / content);
        const float y = (size.height - h) * (scroll / (content - size.height));
        rt->FillRoundedRectangle(
            D2D1::RoundedRect(D2D1::RectF(size.width - 5.0f, y, size.width - 1.0f, y + h), 2, 2),
            t.Brush(theme::FgSub, 0.5f));
    }
}

LRESULT CALLBACK SeriesList::Proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    SeriesList* self = nullptr;
    if (msg == WM_NCCREATE) {
        self = static_cast<SeriesList*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        self->hwnd_ = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<SeriesList*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (self) return self->Handle(msg, wp, lp);
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT SeriesList::Handle(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_PAINT:
            Paint();
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_DPICHANGED_AFTERPARENT:
            dpi_ = theme::WindowDpi(hwnd_);
            ScrollTo(scroll_px_);
            return 0;
        case WM_SETFOCUS:
        case WM_KILLFOCUS:
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        case WM_GETDLGCODE:
            return DLGC_WANTARROWS | DLGC_WANTCHARS;
        case WM_LBUTTONDOWN:
        case WM_LBUTTONDBLCLK: {
            if (msg == WM_LBUTTONDOWN) SetFocus(hwnd_);
            const int row = RowAt(GET_Y_LPARAM(lp));
            const float x = GET_X_LPARAM(lp) * 96.0f / theme::UiDpiF(dpi_);
            // Badge click switches axis; elsewhere toggles the row.
            if (row >= 0 && x >= BadgeLeft() - 2.0f) {
                SetAxis(row, !rows_[static_cast<size_t>(row)].right);
            } else {
                Toggle(row);
            }
            return 0;
        }
        case WM_MOUSEMOVE: {
            const int r = RowAt(GET_Y_LPARAM(lp));
            if (r != hot_) {
                hot_ = r;
                InvalidateRect(hwnd_, nullptr, FALSE);
                TRACKMOUSEEVENT t = {sizeof(t), TME_LEAVE, hwnd_, 0};
                TrackMouseEvent(&t);
            }
            UpdateTip();
            return 0;
        }
        case WM_MOUSELEAVE:
            hot_ = -1;
            UpdateTip();
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        case WM_MOUSEWHEEL:
            ScrollTo(scroll_px_ - GET_WHEEL_DELTA_WPARAM(wp) * RowPx() * 3 / WHEEL_DELTA);
            return 0;
        case WM_KEYDOWN: {
            const int n = static_cast<int>(rows_.size());
            if (n == 0) break;
            if (wp == VK_DOWN || wp == VK_UP) {
                focus_row_ = (std::max)(0, (std::min)(n - 1, focus_row_ + (wp == VK_DOWN ? 1 : -1)));
                RECT rc = {};
                GetClientRect(hwnd_, &rc);
                const int top = focus_row_ * RowPx();
                if (top < scroll_px_) ScrollTo(top);
                else if (top + RowPx() > scroll_px_ + rc.bottom) ScrollTo(top + RowPx() - rc.bottom);
                InvalidateRect(hwnd_, nullptr, FALSE);
                return 0;
            }
            if (wp == VK_SPACE) {
                Toggle(focus_row_);
                return 0;
            }
            if (wp == VK_LEFT || wp == VK_RIGHT) {
                SetAxis(focus_row_, wp == VK_RIGHT);
                return 0;
            }
            break;
        }
        case WM_DESTROY:
            a11y::Clear(hwnd_);
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}

}  // namespace ui
