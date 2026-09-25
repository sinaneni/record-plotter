// Themed modal dialogs drawn with Direct2D, run in a nested message loop.
#include "dialogs.h"

#include <shlobj.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>

#include "../resource_ids.h"
#include "a11y.h"
#include "controls.h"
#include "d2d.h"
#include "theme.h"

namespace ui {
namespace {

const wchar_t kClass[] = L"MbDialog";

// Metrics at 96 DPI.
constexpr int kPad      = 18;
constexpr int kBtnH     = 30;
constexpr int kBtnMinW  = 92;
constexpr int kBtnGap   = 8;
constexpr int kIconW    = 34;
constexpr int kMinW     = 330;
constexpr int kMaxW     = 560;
constexpr int kInputH   = 28;

constexpr int kFirstButtonId = 200;

// The progress variant's bar and its poll.
constexpr float kBarH           = 8.0f;
constexpr float kBarRadius      = 4.0f;
constexpr int   kProgressMinW   = 420;   // keeps the detail on one line
constexpr UINT  kProgressTimer  = 1;
constexpr UINT  kProgressPollMs = 100;

struct DialogState {
    MessageOptions opts;
    bool           running = false;
    int            result = 0;
    UINT           dpi = 96;
    HWND           owner = nullptr;

    // About variant.
    bool      is_about = false;
    AboutInfo about;

    // Input variant.
    bool         is_input = false;
    std::wstring prompt;
    std::wstring value;
    Input        edit;

    // Progress variant; detail and bar come from the latest snapshot.
    bool             is_progress = false;
    std::function<ProgressSnapshot()> poll;
    ProgressSnapshot snap;
    unsigned         anim = 0;          // ticks, for the indeterminate bar's moving segment
    int              announced = -2;    // last tenth announced to screen readers
    // Geometry in DIPs, computed once at layout.
    float detail_top = 0.0f;
    float detail_h = 0.0f;
    float bar_top = 0.0f;
    bool  progress_detail = true;   // ProgressOptions::detail_line

    std::vector<Button> buttons;
    float text_top = 0;   // DIPs, filled during layout
    float text_h = 0;
    // Height of the hero icon plus its gap, in DIPs; 0 when none.
    float hero_h = 0.0f;

    // Input row geometry in DIPs, shared by the painted frame and the edit control.
    float in_x = 0.0f;
    float in_w = 0.0f;
    float in_top = 0.0f;
    float prompt_h = 0.0f;
};

// DIPs to physical pixels, rounded.
int Px(float dip, UINT dpi) {
    return static_cast<int>(std::lround(dip * static_cast<double>(theme::UiDpiF(dpi)) / 96.0));
}

// Number of open modal dialogs.
int g_open_dialogs = 0;

struct OpenDialogScope {
    OpenDialogScope() { ++g_open_dialogs; }
    ~OpenDialogScope() { --g_open_dialogs; }
    OpenDialogScope(const OpenDialogScope&) = delete;
    OpenDialogScope& operator=(const OpenDialogScope&) = delete;
};

DialogState* State(HWND h) {
    return reinterpret_cast<DialogState*>(GetWindowLongPtrW(h, GWLP_USERDATA));
}

theme::Color IconColor(MsgIcon i) {
    switch (i) {
        case MsgIcon::Warning:  return theme::Warning;
        case MsgIcon::Error:    return theme::Error;
        case MsgIcon::Question: return theme::Accent;
        case MsgIcon::Info:
        case MsgIcon::None:
        default:                return theme::Accent;
    }
}

const wchar_t* IconGlyph(MsgIcon i) {
    switch (i) {
        case MsgIcon::Warning:  return L"!";
        case MsgIcon::Error:    return L"×";  // multiplication sign
        case MsgIcon::Question: return L"?";
        case MsgIcon::Info:     return L"i";
        case MsgIcon::None:
        default:                return L"";
    }
}

// Height of `s` wrapped to `width` DIPs.
float MeasureText(const std::wstring& s, theme::Font font, float width) {
    if (s.empty()) return 0.0f;
    IDWriteFactory* dw = d2d::WriteFactory();
    // Formats are shared, so wrapping is requested per call.
    IDWriteTextFormat* fmt = theme::Text(font, DWRITE_TEXT_ALIGNMENT_LEADING, true,
                                         DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    if (!dw || !fmt) return 20.0f;
    IDWriteTextLayout* layout = nullptr;
    float h = 20.0f;
    if (SUCCEEDED(dw->CreateTextLayout(s.c_str(), static_cast<UINT32>(s.size()), fmt,
                                       width, 4000.0f, &layout))) {
        DWRITE_TEXT_METRICS m = {};
        if (SUCCEEDED(layout->GetMetrics(&m))) h = m.height;
        layout->Release();
    }
    return h;
}

void DrawWrapped(ID2D1RenderTarget* rt, const std::wstring& s, theme::Font font,
                 ID2D1Brush* brush, const D2D1_RECT_F& r) {
    if (s.empty() || !brush) return;
    IDWriteTextFormat* fmt = theme::Text(font, DWRITE_TEXT_ALIGNMENT_LEADING, true,
                                         DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    if (!fmt) return;
    rt->DrawTextW(s.c_str(), static_cast<UINT32>(s.size()), fmt, r, brush,
                  D2D1_DRAW_TEXT_OPTIONS_NONE, DWRITE_MEASURING_MODE_NATURAL);
}

void DrawCentered(ID2D1RenderTarget* rt, const std::wstring& s, theme::Font font,
                  ID2D1Brush* brush, const D2D1_RECT_F& r) {
    if (s.empty() || !brush) return;
    IDWriteTextFormat* fmt = theme::Text(font, DWRITE_TEXT_ALIGNMENT_CENTER, true,
                                         DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    if (!fmt) return;
    rt->DrawTextW(s.c_str(), static_cast<UINT32>(s.size()), fmt, r, brush,
                  D2D1_DRAW_TEXT_OPTIONS_NONE, DWRITE_MEASURING_MODE_NATURAL);
}

// The application icon at a given pixel size, cached per size.
HICON HeroIcon(int px) {
    static HICON cached = nullptr;
    static int   cached_px = 0;
    if (px <= 0) return nullptr;
    if (cached && cached_px == px) return cached;
    HICON h = static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr),
                                            MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON,
                                            px, px, LR_DEFAULTCOLOR));
    if (!h) return cached;   // keep the previous icon
    // Sized icons are not shared, so release the old one.
    if (cached) DestroyIcon(cached);
    cached = h;
    cached_px = px;
    return cached;
}

// --------------------------------------------------------------- About layout
// Metrics for the About box, at 96 DPI.
constexpr float kAboutIconGap   = 16.0f;  // icon bottom to the product name
constexpr float kAboutRuleGap   = 16.0f;  // space either side of a hairline
constexpr float kAboutSectGap   = 10.0f;  // heading to the rows beneath it
constexpr float kAboutRowH      = 19.0f;  // one capability or meta row
constexpr float kAboutCapLabelW = 86.0f;  // capability label column
constexpr float kAboutMetaLabelW = 62.0f; // footer label column

// Lays out the About content: measures only when `rt` is null, otherwise also draws.
// Returns the content height in DIPs.
float AboutLayout(const AboutInfo& a, float left, float width, float top,
                  ID2D1RenderTarget* rt, d2d::Brushes* b) {
    float y = top;
    const float right = left + width;

    auto line = [&](const std::wstring& s, theme::Font font, theme::Color c,
                    DWRITE_TEXT_ALIGNMENT align, bool wrap) {
        if (s.empty()) return;
        const float h = MeasureText(s, font, width);
        if (rt && b) {
            IDWriteTextFormat* fmt = theme::Text(font, align, wrap,
                                                 DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
            if (fmt) {
                rt->DrawTextW(s.c_str(), static_cast<UINT32>(s.size()), fmt,
                              D2D1::RectF(left, y, right, y + h), b->Brush(c),
                              D2D1_DRAW_TEXT_OPTIONS_NONE, DWRITE_MEASURING_MODE_NATURAL);
            }
        }
        y += h;
    };

    auto rule = [&]() {
        y += kAboutRuleGap;
        if (rt && b) {
            rt->DrawLine(D2D1::Point2F(left, y), D2D1::Point2F(right, y),
                         b->Brush(theme::FgSub, 0.25f), 1.0f);
        }
        y += kAboutRuleGap;
    };

    // --- masthead: icon space (icon drawn later in GDI), name, version, tagline
    y += static_cast<float>(a.icon96) + kAboutIconGap;
    line(a.name, theme::Font::Title, theme::FgText, DWRITE_TEXT_ALIGNMENT_CENTER, false);
    y += 2.0f;
    line(a.version, theme::Font::UiBold, theme::Accent, DWRITE_TEXT_ALIGNMENT_CENTER,
         false);
    y += 4.0f;
    line(a.tagline, theme::Font::Subtitle, theme::FgSub, DWRITE_TEXT_ALIGNMENT_CENTER,
         false);

    if (!a.blurb.empty()) {
        rule();
        line(a.blurb, theme::Font::Ui, theme::FgSub, DWRITE_TEXT_ALIGNMENT_LEADING, true);
    }

    // --- capability table ---------------------------------------------------
    if (!a.capabilities.empty()) {
        rule();
        line(a.section, theme::Font::UiBold, theme::FgText,
             DWRITE_TEXT_ALIGNMENT_LEADING, false);
        y += kAboutSectGap;
        for (const auto& cap : a.capabilities) {
            if (rt && b) {
                IDWriteTextFormat* lf = theme::Text(theme::Font::Ui,
                                                    DWRITE_TEXT_ALIGNMENT_LEADING, false,
                                                    DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
                IDWriteTextFormat* vf = theme::Text(theme::Font::Mono,
                                                    DWRITE_TEXT_ALIGNMENT_LEADING, false,
                                                    DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
                if (lf) {
                    rt->DrawTextW(cap.first.c_str(),
                                  static_cast<UINT32>(cap.first.size()), lf,
                                  D2D1::RectF(left + 4.0f, y, left + kAboutCapLabelW,
                                              y + kAboutRowH),
                                  b->Brush(theme::FgSub), D2D1_DRAW_TEXT_OPTIONS_NONE,
                                  DWRITE_MEASURING_MODE_NATURAL);
                }
                if (vf) {
                    rt->DrawTextW(cap.second.c_str(),
                                  static_cast<UINT32>(cap.second.size()), vf,
                                  D2D1::RectF(left + kAboutCapLabelW, y, right,
                                              y + kAboutRowH),
                                  b->Brush(theme::Accent), D2D1_DRAW_TEXT_OPTIONS_NONE,
                                  DWRITE_MEASURING_MODE_NATURAL);
                }
            }
            y += kAboutRowH;
        }
        if (!a.caveat.empty()) {
            y += 8.0f;
            line(a.caveat, theme::Font::Subtitle, theme::FgSub,
                 DWRITE_TEXT_ALIGNMENT_LEADING, true);
        }
    }

    // --- footer rows --------------------------------------------------------
    if (!a.meta.empty()) {
        rule();
        for (const auto& m : a.meta) {
            const float h = (std::max)(kAboutRowH,
                                       MeasureText(m.second, theme::Font::Mono,
                                                   width - kAboutMetaLabelW));
            if (rt && b) {
                IDWriteTextFormat* lf = theme::Text(theme::Font::Subtitle,
                                                    DWRITE_TEXT_ALIGNMENT_LEADING, false,
                                                    DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
                IDWriteTextFormat* vf = theme::Text(theme::Font::Mono,
                                                    DWRITE_TEXT_ALIGNMENT_LEADING, true,
                                                    DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
                if (lf) {
                    rt->DrawTextW(m.first.c_str(), static_cast<UINT32>(m.first.size()),
                                  lf,
                                  D2D1::RectF(left + 4.0f, y + 1.0f,
                                              left + kAboutMetaLabelW, y + h),
                                  b->Brush(theme::FgSub), D2D1_DRAW_TEXT_OPTIONS_NONE,
                                  DWRITE_MEASURING_MODE_NATURAL);
                }
                if (vf) {
                    rt->DrawTextW(m.second.c_str(), static_cast<UINT32>(m.second.size()),
                                  vf,
                                  D2D1::RectF(left + kAboutMetaLabelW, y, right, y + h),
                                  b->Brush(theme::DisabledFg),
                                  D2D1_DRAW_TEXT_OPTIONS_NONE,
                                  DWRITE_MEASURING_MODE_NATURAL);
                }
            }
            y += h + 2.0f;
        }
    }

    return y - top;
}

void Paint(HWND hwnd) {
    DialogState* st = State(hwnd);
    if (!st) return;

    d2d::Frame frame(hwnd, st->dpi);
    ID2D1RenderTarget* rt = frame.rt();
    if (!rt) return;
    d2d::DcTarget& b = frame.brushes();

    const D2D1_SIZE_F size = frame.size();
    rt->Clear(theme::D2d(theme::BgPanel));

    const float left = static_cast<float>(kPad);
    float x = left;

    if (st->is_about) {
        AboutLayout(st->about, left, size.width - left * 2,
                    static_cast<float>(kPad), rt, &b);
        // Icon drawn last, in GDI.
        const int px = theme::Scale(st->about.icon96, st->dpi);
        if (HICON ico = HeroIcon(px)) {
            if (HDC dc = frame.EndAndGetDc()) {
                RECT rc;
                GetClientRect(hwnd, &rc);
                DrawIconEx(dc, (rc.right - rc.left - px) / 2, theme::Scale(kPad, st->dpi),
                           ico, px, px, 0, nullptr, DI_NORMAL);
            }
        }
        return;
    }

    if (st->is_progress) {
        const float w = size.width - left * 2;
        const float th = MeasureText(st->opts.text, theme::Font::UiBold, w);
        DrawWrapped(rt, st->opts.text, theme::Font::UiBold, b.Brush(theme::FgText),
                    D2D1::RectF(left, static_cast<float>(kPad), left + w,
                                static_cast<float>(kPad) + th));
        // Detail clipped to its single reserved row.
        const D2D1_RECT_F detail = D2D1::RectF(left, st->detail_top, left + w,
                                               st->detail_top + st->detail_h);
        rt->PushAxisAlignedClip(detail, D2D1_ANTIALIAS_MODE_ALIASED);
        DrawWrapped(rt, st->snap.detail, theme::Font::Ui, b.Brush(theme::FgSub), detail);
        rt->PopAxisAlignedClip();
        // Bar track, styled like an input field.
        const D2D1_RECT_F track = D2D1::RectF(left, st->bar_top, left + w, st->bar_top + kBarH);
        rt->FillRoundedRectangle(D2D1::RoundedRect(track, kBarRadius, kBarRadius),
                                 b.Brush(theme::EntryBg));
        rt->DrawRoundedRectangle(
            D2D1::RoundedRect(D2D1::RectF(track.left + 0.5f, track.top + 0.5f,
                                          track.right - 0.5f, track.bottom - 0.5f),
                              kBarRadius, kBarRadius),
            b.Brush(theme::Border), 1.0f);
        rt->PushAxisAlignedClip(track, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        if (st->snap.fraction >= 0.0) {
            const float f = static_cast<float>((std::min)(1.0, st->snap.fraction));
            if (f > 0.0f) {
                rt->FillRoundedRectangle(
                    D2D1::RoundedRect(D2D1::RectF(track.left, track.top,
                                                  track.left + w * f, track.bottom),
                                      kBarRadius, kBarRadius),
                    b.Brush(theme::Accent));
            }
        } else {
            // Unknown progress: a moving segment, one pass every 3 s.
            const float seg = w * 0.3f;
            const float t = static_cast<float>(st->anim % 30u) / 30.0f;
            const float x0 = track.left - seg + (w + seg) * t;
            rt->FillRoundedRectangle(
                D2D1::RoundedRect(D2D1::RectF(x0, track.top, x0 + seg, track.bottom),
                                  kBarRadius, kBarRadius),
                b.Brush(theme::Accent));
        }
        rt->PopAxisAlignedClip();
        return;
    }

    // Ringed status glyph, unless there is a hero icon.
    if (st->opts.icon != MsgIcon::None && !st->is_input && st->hero_h == 0.0f) {
        const theme::Color c = IconColor(st->opts.icon);
        const float r = 11.0f;
        const float cy = static_cast<float>(kPad) + r;
        rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(left + r, cy), r, r), b.Brush(c), 2.0f);
        IDWriteTextFormat* fmt = theme::Text(theme::Font::UiBold,
                                             DWRITE_TEXT_ALIGNMENT_CENTER);
        if (fmt) {
            const wchar_t* g = IconGlyph(st->opts.icon);
            rt->DrawTextW(g, static_cast<UINT32>(wcslen(g)), fmt,
                          D2D1::RectF(left, cy - r, left + r * 2, cy + r), b.Brush(c),
                          D2D1_DRAW_TEXT_OPTIONS_NONE, DWRITE_MEASURING_MODE_NATURAL);
        }
        x = left + kIconW;
    }

    const float text_w = size.width - x - kPad;
    float y = static_cast<float>(kPad);

    if (st->is_input) {
        // Uses the stored layout.
        DrawWrapped(rt, st->prompt, theme::Font::Ui, b.Brush(theme::FgText),
                    D2D1::RectF(st->in_x, static_cast<float>(kPad),
                                st->in_x + st->in_w,
                                static_cast<float>(kPad) + st->prompt_h));
        DrawInputFrame(rt, b,
                       D2D1::RectF(st->in_x, st->in_top, st->in_x + st->in_w,
                                   st->in_top + kInputH),
                       GetFocus() == st->edit.hwnd(), true);
        y = st->in_top + kInputH;
    } else {
        // Space reserved for the hero icon.
        y += st->hero_h;
        if (!st->opts.text.empty()) {
            const float th = MeasureText(st->opts.text, theme::Font::UiBold, text_w);
            // Centred under a hero icon, otherwise left-aligned.
            if (st->hero_h > 0.0f) {
                DrawCentered(rt, st->opts.text, theme::Font::UiBold,
                             b.Brush(theme::FgText),
                             D2D1::RectF(x, y, x + text_w, y + th));
            } else {
                DrawWrapped(rt, st->opts.text, theme::Font::UiBold, b.Brush(theme::FgText),
                            D2D1::RectF(x, y, x + text_w, y + th));
            }
            y += th + 8.0f;
        }
        if (!st->opts.detail.empty()) {
            const float dh = MeasureText(st->opts.detail, theme::Font::Ui, text_w);
            DrawWrapped(rt, st->opts.detail, theme::Font::Ui, b.Brush(theme::FgSub),
                        D2D1::RectF(x, y, x + text_w, y + dh));
        }
    }

    // ---------------------------------------------------------- hero icon overlay
    // Drawn in GDI after the D2D frame ends; `rt` must not be used after this.
    if (st->hero_h > 0.0f) {
        const int px = theme::Scale(st->opts.hero_icon96, st->dpi);
        if (HICON ico = HeroIcon(px)) {
            if (HDC dc = frame.EndAndGetDc()) {
                RECT rc;
                GetClientRect(hwnd, &rc);
                const int left_px = (rc.right - rc.left - px) / 2;
                const int top_px = theme::Scale(kPad, st->dpi);
                DrawIconEx(dc, left_px, top_px, ico, px, px, 0, nullptr, DI_NORMAL);
            }
        }
    }
}

void Close(HWND hwnd, int result) {
    DialogState* st = State(hwnd);
    if (!st || !st->running) return;
    if (st->is_input) st->value = st->edit.Text();
    st->result = result;
    st->running = false;
    PostMessageW(hwnd, WM_NULL, 0, 0);  // make sure the nested loop wakes
}

LRESULT CALLBACK DialogProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    DialogState* st = State(hwnd);

    switch (msg) {
        case WM_NCCREATE: {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                              reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
            return DefWindowProcW(hwnd, msg, wp, lp);
        }
        case WM_PAINT:
            Paint(hwnd);
            return 0;
        case WM_ERASEBKGND:
            return 1;

        case WM_CTLCOLOREDIT: {
            HDC dc = reinterpret_cast<HDC>(wp);
            SetTextColor(dc, theme::Ref(theme::EntryFg));
            SetBkColor(dc, theme::Ref(theme::EntryBg));
            return reinterpret_cast<LRESULT>(theme::SolidBrush(theme::EntryBg));
        }

        case WM_COMMAND: {
            if (!st) break;
            // Enter is handled by the modal loop, not here.
            const int id = LOWORD(wp);
            const int index = id - kFirstButtonId;
            if (index >= 0 && index < static_cast<int>(st->opts.buttons.size())) {
                Close(hwnd, st->opts.buttons[index].first);
                return 0;
            }
            break;
        }

        case WM_CLOSE:
            if (st) Close(hwnd, st->opts.cancel_result);
            return 0;

        // Destroyed from outside (e.g. with its owner): report as cancelled.
        case WM_DESTROY:
            if (st && st->running) {
                st->result = st->opts.cancel_result;
                st->running = false;
            }
            break;

        case WM_TIMER:
            if (st && st->is_progress && wp == kProgressTimer && st->running) {
                const std::wstring before = st->snap.detail;
                st->snap = st->poll();
                if (st->snap.finished) {
                    Close(hwnd, 1);
                    return 0;
                }
                ++st->anim;
                // Client area only, so the button does not flicker.
                InvalidateRect(hwnd, nullptr, FALSE);
                if (st->snap.detail != before) {
                    // Expose the detail to screen readers; announce at most once per tenth.
                    a11y::SetValue(hwnd, st->snap.detail);
                    const int tenth = st->snap.fraction < 0.0
                                          ? -1 : static_cast<int>(st->snap.fraction * 10.0);
                    if (tenth != st->announced) {
                        st->announced = tenth;
                        a11y::NotifyStateChanged(hwnd);
                    }
                }
                return 0;
            }
            break;

        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void RegisterDialogClass() {
    static bool done = false;
    if (done) return;
    done = true;
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DialogProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpszClassName = kClass;
        // A class brush avoids a white flash before the first paint.
    wc.hbrBackground = theme::SolidBrush(theme::BgPanel);
    RegisterClassExW(&wc);
}

int RunDialog(HWND owner, DialogState& st) {
    const OpenDialogScope open;
    RegisterDialogClass();
    RegisterControlClasses();

    st.dpi = theme::WindowDpi(owner ? owner : GetDesktopWindow());
    st.owner = owner;

    // --- size to content ---------------------------------------------------
    // A hero icon replaces the glyph and sits above the text.
    constexpr float kHeroGap = 14.0f;
    st.hero_h = (st.opts.hero_icon96 > 0 && !st.is_input)
                    ? static_cast<float>(st.opts.hero_icon96) + kHeroGap : 0.0f;
    const float icon_w = (st.opts.icon != MsgIcon::None && !st.is_input &&
                          st.hero_h == 0.0f)
                             ? static_cast<float>(kIconW) : 0.0f;
    // Settle the width first, then measure the text against it.
    float width96 = static_cast<float>(kMinW);
    // Buttons need room; widen the dialog if the row would not fit.
    float buttons_w = 0;
    for (size_t i = 0; i < st.opts.buttons.size(); ++i) {
        buttons_w += kBtnMinW + (i ? kBtnGap : 0);
    }
    width96 = (std::max)(width96, buttons_w + kPad * 2);
    // Caller-requested minimum width.
    width96 = (std::max)(width96, static_cast<float>(st.opts.min_width96));
    width96 = (std::min)(width96, static_cast<float>(kMaxW));

    const float text_w = width96 - kPad * 2 - icon_w;
    float content_h = 0;
    if (st.is_about) {
        // Measured by the same function that draws it.
        content_h = AboutLayout(st.about, static_cast<float>(kPad), text_w, 0.0f,
                                nullptr, nullptr);
    } else if (st.is_input) {
        content_h = MeasureText(st.prompt, theme::Font::Ui, text_w) + 10.0f + kInputH;
    } else if (st.is_progress) {
        // One line reserved for the detail, measured with sample text.
        const float th = MeasureText(st.opts.text, theme::Font::UiBold, text_w);
        st.detail_top = static_cast<float>(kPad) + th + 8.0f;
        st.detail_h = st.progress_detail ? MeasureText(L"0 MB", theme::Font::Ui, text_w) : 0.0f;
        st.bar_top = st.detail_top + st.detail_h + (st.progress_detail ? 12.0f : 4.0f);
        content_h = st.bar_top + kBarH - static_cast<float>(kPad);
    } else {
        content_h = st.hero_h + MeasureText(st.opts.text, theme::Font::UiBold, text_w);
        if (!st.opts.detail.empty()) {
            content_h += 8.0f + MeasureText(st.opts.detail, theme::Font::Ui, text_w);
        }
    }

    const int client_w = theme::Scale(static_cast<int>(width96), st.dpi);
    const int client_h = theme::Scale(
        static_cast<int>(kPad + content_h + kPad + kBtnH + kPad), st.dpi);

    RECT rc = {0, 0, client_w, client_h};
    // Window size for the computed client area, at the owner's DPI.
    theme::AdjustWindowRect(&rc, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, st.dpi);
    const int win_w = rc.right - rc.left;
    const int win_h = rc.bottom - rc.top;

    // Centre on the owner, or on the work area when there is none.
    int x = 0, y = 0;
    RECT orc;
    if (owner && GetWindowRect(owner, &orc)) {
        x = orc.left + ((orc.right - orc.left) - win_w) / 2;
        y = orc.top + ((orc.bottom - orc.top) - win_h) / 2;
    }

    HWND hwnd = CreateWindowExW(WS_EX_DLGMODALFRAME, kClass, st.opts.title.c_str(),
                                WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN,
                                x, y, win_w, win_h, owner, nullptr,
                                GetModuleHandleW(nullptr), &st);
    if (!hwnd) return st.opts.cancel_result;
    theme::ApplyTitleBar(hwnd);

    // --- children ----------------------------------------------------------
    const int pad = theme::Scale(kPad, st.dpi);
    const int btn_h = theme::Scale(kBtnH, st.dpi);
    const int btn_w = theme::Scale(kBtnMinW, st.dpi);
    const int gap = theme::Scale(kBtnGap, st.dpi);

    if (st.is_input) {
        // Input row layout in DIPs, shared with Paint().
        st.in_x = static_cast<float>(kPad) + icon_w;
        st.in_w = width96 - st.in_x - static_cast<float>(kPad);
        st.prompt_h = MeasureText(st.prompt, theme::Font::Ui, st.in_w);
        st.in_top = static_cast<float>(kPad) + st.prompt_h + 10.0f;

        st.edit.Create(hwnd, 100);
        // Edit control placed from the same layout, inset on all sides.
        const int fi = theme::Scale(kFieldInset96, st.dpi);
        const int fx = Px(st.in_x, st.dpi);
        const int fy = Px(st.in_top, st.dpi);
        const int fw = Px(st.in_x + st.in_w, st.dpi) - fx;
        const int fh = Px(st.in_top + kInputH, st.dpi) - fy;
        st.edit.Move(fx + fi, fy + fi, fw - fi * 2, fh - fi * 2);
        st.edit.SetText(st.value);
        SendMessageW(st.edit.hwnd(), EM_SETSEL, 0, -1);
    }

    // Buttons in the given order, right-aligned; the default one is accented.
    st.buttons.resize(st.opts.buttons.size());
    const int n = static_cast<int>(st.opts.buttons.size());
    const int total = n * btn_w + (n - 1) * gap;
    int bx = client_w - pad - total;
    for (int i = 0; i < n; ++i) {
        const Button::Variant v = (i == st.opts.default_button) ? Button::Variant::Accent
                                                                : Button::Variant::Neutral;
        st.buttons[i].Create(hwnd, kFirstButtonId + i, st.opts.buttons[i].second, v);
        st.buttons[i].Move(bx, client_h - pad - btn_h, btn_w, btn_h);
        bx += btn_w + gap;
    }

    // --- modal loop --------------------------------------------------------
    // Disable the owner; re-enable it only if this call disabled it (dialogs can nest).
    const bool owned_disable = owner && IsWindowEnabled(owner);
    if (owned_disable) EnableWindow(owner, FALSE);
    if (st.is_progress) {
        a11y::SetValue(hwnd, st.snap.detail);
        SetTimer(hwnd, kProgressTimer, kProgressPollMs, nullptr);
    }
    theme::ShowPainted(hwnd);
    if (st.is_input) SetFocus(st.edit.hwnd());
    else if (!st.buttons.empty()) SetFocus(st.buttons[st.opts.default_button].hwnd());

    st.running = true;
    MSG msg;
    while (st.running) {
        const BOOL got = GetMessageW(&msg, nullptr, 0, 0);
        if (got <= 0) {
            // WM_QUIT: re-post it for the outer message loop.
            if (got == 0) PostQuitMessage(static_cast<int>(msg.wParam));
            break;
        }
        // Escape cancels; Enter activates the focused button, or the default one.
        // Handled before IsDialogMessage, which would turn Enter into IDOK.
        if (msg.message == WM_KEYDOWN && (msg.hwnd == hwnd || IsChild(hwnd, msg.hwnd))) {
            if (msg.wParam == VK_ESCAPE) {
                Close(hwnd, st.opts.cancel_result);
                continue;
            }
            if (msg.wParam == VK_RETURN && !st.opts.buttons.empty()) {
                int index = st.opts.default_button;
                for (size_t i = 0; i < st.buttons.size(); ++i) {
                    if (st.buttons[i].hwnd() == msg.hwnd) {
                        index = static_cast<int>(i);
                        break;
                    }
                }
                Close(hwnd, st.opts.buttons[static_cast<size_t>(index)].first);
                continue;
            }
        }
        if (IsDialogMessageW(hwnd, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (owned_disable) EnableWindow(owner, TRUE);
    if (IsWindow(hwnd)) DestroyWindow(hwnd);
    // Hand activation back to the owner.
    if (owned_disable) SetActiveWindow(owner);
    return st.result;
}

}  // namespace

bool AnyDialogOpen() { return g_open_dialogs > 0; }

int ShowMessage(HWND owner, const MessageOptions& opts) {
    DialogState st;
    st.opts = opts;
    if (st.opts.buttons.empty()) st.opts.buttons.push_back({1, L"OK"});
    if (st.opts.default_button < 0 ||
        st.opts.default_button >= static_cast<int>(st.opts.buttons.size())) {
        st.opts.default_button = 0;
    }
    return RunDialog(owner, st);
}

int ShowAboutDialog(HWND owner, const AboutInfo& info) {
    DialogState st;
    st.is_about = true;
    st.about = info;
    st.opts.title = L"About";
    st.opts.icon = MsgIcon::None;
    st.opts.buttons = {{2, L"Open Folder"},
                       {1, L"Copy Details"},
                       {3, L"Updates"},
                       {0, L"Close"}};
    st.opts.default_button = 1;
    st.opts.cancel_result = 0;
    st.opts.min_width96 = 552;
    return RunDialog(owner, st);
}

void ShowInfo(HWND owner, const std::wstring& title, const std::wstring& text,
              const std::wstring& detail) {
    MessageOptions o;
    o.title = title;
    o.text = text;
    o.detail = detail;
    o.icon = MsgIcon::Info;
    o.buttons = {{1, L"OK"}};
    ShowMessage(owner, o);
}

void ShowWarning(HWND owner, const std::wstring& title, const std::wstring& text,
                 const std::wstring& detail) {
    MessageOptions o;
    o.title = title;
    o.text = text;
    o.detail = detail;
    o.icon = MsgIcon::Warning;
    o.buttons = {{1, L"OK"}};
    ShowMessage(owner, o);
}

void ShowError(HWND owner, const std::wstring& title, const std::wstring& text,
               const std::wstring& detail) {
    MessageOptions o;
    o.title = title;
    o.text = text;
    o.detail = detail;
    o.icon = MsgIcon::Error;
    o.buttons = {{1, L"OK"}};
    ShowMessage(owner, o);
}

bool AskYesNo(HWND owner, const std::wstring& title, const std::wstring& text,
              const std::wstring& detail) {
    MessageOptions o;
    o.title = title;
    o.text = text;
    o.detail = detail;
    o.icon = MsgIcon::Question;
    // No is the default.
    o.buttons = {{1, L"Yes"}, {0, L"No"}};
    o.default_button = 1;
    o.cancel_result = 0;
    return ShowMessage(owner, o) == 1;
}

bool PickFolder(HWND owner, const std::wstring& title, std::wstring* dir) {
    IFileDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&dlg)))) {
        return false;
    }
    bool ok = false;
    DWORD flags = 0;
    if (SUCCEEDED(dlg->GetOptions(&flags))) {
        // Folder-picking mode.
        dlg->SetOptions(flags | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM |
                        FOS_PATHMUSTEXIST);
    }
    if (!title.empty()) dlg->SetTitle(title.c_str());
    if (dir && !dir->empty()) {
        IShellItem* start = nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(dir->c_str(), nullptr,
                                                  IID_PPV_ARGS(&start)))) {
            dlg->SetFolder(start);
            start->Release();
        }
    }
    const OpenDialogScope open;
    if (SUCCEEDED(dlg->Show(owner))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item))) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
                if (dir) *dir = path;
                CoTaskMemFree(path);
                ok = true;
            }
            item->Release();
        }
    }
    dlg->Release();
    return ok;
}

bool OpenFileDialog(HWND owner, const std::wstring& title,
                    const std::vector<std::pair<std::wstring, std::wstring>>& filters,
                    std::wstring* path) {
    IFileDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&dlg)))) {
        return false;
    }
    bool ok = false;
    // Specs point at the caller's strings.
    std::vector<COMDLG_FILTERSPEC> specs;
    specs.reserve(filters.size());
    for (const auto& f : filters) specs.push_back({f.first.c_str(), f.second.c_str()});
    if (!title.empty()) dlg->SetTitle(title.c_str());
    if (!specs.empty()) {
        dlg->SetFileTypes(static_cast<UINT>(specs.size()), specs.data());
        dlg->SetFileTypeIndex(1);
    }
    // Start in the previous file's folder.
    if (path && !path->empty()) {
        const size_t slash = path->find_last_of(L"\\/");
        const std::wstring folder = (slash == std::wstring::npos) ? *path : path->substr(0, slash);
        IShellItem* start = nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(folder.c_str(), nullptr,
                                                  IID_PPV_ARGS(&start)))) {
            dlg->SetFolder(start);
            start->Release();
        }
    }
    DWORD flags = 0;
    if (SUCCEEDED(dlg->GetOptions(&flags))) {
        dlg->SetOptions(flags | FOS_FILEMUSTEXIST | FOS_FORCEFILESYSTEM);
    }
    const OpenDialogScope open;
    if (SUCCEEDED(dlg->Show(owner))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item))) {
            PWSTR p = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p)) && p) {
                if (path) *path = p;
                CoTaskMemFree(p);
                ok = true;
            }
            item->Release();
        }
    }
    dlg->Release();
    return ok;
}

bool SaveFileDialog(HWND owner, const std::wstring& title,
                    const std::vector<std::pair<std::wstring, std::wstring>>& filters,
                    const std::wstring& default_name, std::wstring* path,
                    int* filter_index) {
    IFileDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&dlg)))) {
        return false;
    }
    bool ok = false;
    // Specs point at the caller's strings, which must outlive Show().
    std::vector<COMDLG_FILTERSPEC> specs;
    specs.reserve(filters.size());
    for (const auto& f : filters) specs.push_back({f.first.c_str(), f.second.c_str()});

    if (!title.empty()) dlg->SetTitle(title.c_str());
    if (!specs.empty()) {
        dlg->SetFileTypes(static_cast<UINT>(specs.size()), specs.data());
        dlg->SetFileTypeIndex(1);   // 1-based
        // Appends the selected filter's extension when the user types none.
        const std::wstring& first = filters[0].second;
        const size_t dot = first.find(L'.');
        if (dot != std::wstring::npos) dlg->SetDefaultExtension(first.substr(dot + 1).c_str());
    }
    if (!default_name.empty()) dlg->SetFileName(default_name.c_str());
    DWORD flags = 0;
    if (SUCCEEDED(dlg->GetOptions(&flags))) {
        dlg->SetOptions(flags | FOS_OVERWRITEPROMPT | FOS_FORCEFILESYSTEM);
    }

    const OpenDialogScope open;
    if (SUCCEEDED(dlg->Show(owner))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item))) {
            PWSTR p = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p)) && p) {
                if (path) *path = p;
                CoTaskMemFree(p);
                UINT idx = 1;
                dlg->GetFileTypeIndex(&idx);
                if (filter_index) *filter_index = static_cast<int>(idx) - 1;
                ok = true;
            }
            item->Release();
        }
    }
    dlg->Release();
    return ok;
}

std::wstring DocumentsFolder() {
    PWSTR path = nullptr;
    std::wstring out;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &path)) && path) {
        out = path;
    }
    if (path) CoTaskMemFree(path);
    if (out.empty() &&
        SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Profile, 0, nullptr, &path)) && path) {
        out = path;
        CoTaskMemFree(path);
    }
    return out;
}

void OpenFolderInShell(const std::wstring& dir) {
    if (dir.empty()) return;
    ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

int ShowProgress(HWND owner, const ProgressOptions& opts,
                 const std::function<ProgressSnapshot()>& poll) {
    DialogState st;
    st.is_progress = true;
    st.progress_detail = opts.detail_line;
    st.poll = poll;
    st.opts.title = opts.title;
    st.opts.text = opts.text;
    st.opts.icon = MsgIcon::None;
    st.opts.buttons = {{0, opts.cancel_label}};
    st.opts.default_button = 0;
    st.opts.cancel_result = 0;
    st.opts.min_width96 = kProgressMinW;
    if (!st.poll) return 0;
    // First poll before the window exists; skip the dialog if already finished.
    st.snap = st.poll();
    if (st.snap.finished) return 1;
    return RunDialog(owner, st);
}

bool ShowInput(HWND owner, const std::wstring& title, const std::wstring& prompt,
               std::wstring* value) {
    DialogState st;
    st.is_input = true;
    st.prompt = prompt;
    st.value = value ? *value : std::wstring();
    st.opts.title = title;
    st.opts.icon = MsgIcon::None;
    st.opts.buttons = {{1, L"OK"}, {0, L"Cancel"}};
    st.opts.default_button = 0;
    st.opts.cancel_result = 0;
    const int r = RunDialog(owner, st);
    if (r == 1 && value) *value = st.value;
    return r == 1;
}

}  // namespace ui
