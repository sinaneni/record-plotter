// Plot view: data ranges, axes, decimated drawing, PNG export and mouse input.
#include "ui/plot_view.h"

#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <limits>

#include "core/util.h"
#include "core/values.h"
#include "ui/a11y.h"
#include "ui/theme.h"

namespace ui {
namespace {

const wchar_t kClass[] = L"RpPlotView";

// Margins around the plot area, in DIPs.
constexpr float kLeft = 62.0f;
constexpr float kTop = 14.0f;
constexpr float kRight = 18.0f;
constexpr float kRightAxis = kLeft;   // right margin with a second axis
constexpr float kBottom = 40.0f;

constexpr double kWheelFactor = 1.25;   // one notch
constexpr int    kDragSlopPx = 4;
constexpr float  kTraceW = 1.5f;

const double kNaN = std::numeric_limits<double>::quiet_NaN();

// HLS -> RGB.
void HlsToRgb(double h, double l, double s, double* r, double* g, double* b) {
    auto v = [](double m1, double m2, double hue) {
        hue = hue - std::floor(hue);
        if (hue < 1.0 / 6.0) return m1 + (m2 - m1) * hue * 6.0;
        if (hue < 0.5)       return m2;
        if (hue < 2.0 / 3.0) return m1 + (m2 - m1) * (2.0 / 3.0 - hue) * 6.0;
        return m1;
    };
    if (s == 0.0) { *r = *g = *b = l; return; }
    const double m2 = (l <= 0.5) ? l * (1.0 + s) : l + s - l * s;
    const double m1 = 2.0 * l - m2;
    *r = v(m1, m2, h + 1.0 / 3.0);
    *g = v(m1, m2, h);
    *b = v(m1, m2, h - 1.0 / 3.0);
}

// The smallest 1/2/2.5/5 x 10^k at or above `raw`.
double NiceStep(double raw) {
    if (!(raw > 0.0) || !std::isfinite(raw)) return 1.0;
    const double exp10 = std::pow(10.0, std::floor(std::log10(raw)));
    const double f = raw / exp10;
    double m = 10.0;
    if      (f <= 1.0) m = 1.0;
    else if (f <= 2.0) m = 2.0;
    else if (f <= 2.5) m = 2.5;
    else if (f <= 5.0) m = 5.0;
    return m * exp10;
}

// Time axis step on round clock intervals.
double TimeStep(double raw) {
    static const double kSteps[] = {
        0.001, 0.002, 0.005, 0.01, 0.02, 0.05, 0.1, 0.2, 0.5,
        1, 2, 5, 10, 15, 30,
        60, 120, 300, 600, 900, 1800,
        3600, 7200, 10800, 21600, 43200,
        86400, 172800, 604800};
    for (double s : kSteps) {
        if (s >= raw) return s;
    }
    return NiceStep(raw / 86400.0) * 86400.0;
}

// Multiples of `step` inside [lo, hi].
void Ticks(double lo, double hi, double step, std::vector<double>* out) {
    out->clear();
    if (!(hi > lo) || !(step > 0.0)) return;
    const double first = std::ceil(lo / step - 1e-9) * step;
    const double eps = step * 1e-6;
    for (int k = 0; k < 200; ++k) {
        const double v = first + k * step;
        if (v > hi + eps) break;
        out->push_back(std::fabs(v) < eps ? 0.0 : v);
    }
}

// Formats a tick with as many decimals as the step needs.
std::wstring FormatTick(double v, double step) {
    const double a = std::fabs(v);
    if (a >= 1e7 || (a > 0.0 && a < 1e-4 && step < 1e-4)) return core::Fmt(L"%.4g", v);
    int dec = 0;
    double s = step;
    while (dec < 9 && std::fabs(s - std::round(s)) > 1e-7 * (std::max)(1.0, std::fabs(s))) {
        s *= 10.0;
        ++dec;
    }
    return core::Fmt(L"%.*f", dec, v);
}

std::wstring FormatValue(double v) {
    if (std::isnan(v)) return L"—";
    return core::Fmt(L"%.6g", v);
}

// Text width in DIPs.
float TextWidth(const std::wstring& s, theme::Font font) {
    IDWriteTextFormat* fmt = theme::Text(font);
    IDWriteFactory* f = d2d::WriteFactory();
    if (!fmt || !f || s.empty()) return 0.0f;
    IDWriteTextLayout* layout = nullptr;
    if (FAILED(f->CreateTextLayout(s.c_str(), static_cast<UINT32>(s.size()), fmt, 4000.0f,
                                   100.0f, &layout))) {
        return 0.0f;
    }
    DWRITE_TEXT_METRICS m = {};
    layout->GetMetrics(&m);
    layout->Release();
    return m.widthIncludingTrailingWhitespace;
}

void DrawText(ID2D1RenderTarget* rt, const std::wstring& s, theme::Font font, ID2D1Brush* b,
              const D2D1_RECT_F& r, DWRITE_TEXT_ALIGNMENT align,
              DWRITE_PARAGRAPH_ALIGNMENT vertical = DWRITE_PARAGRAPH_ALIGNMENT_CENTER) {
    IDWriteTextFormat* fmt = theme::Text(font, align, false, vertical);
    if (!fmt || !b || s.empty()) return;
    rt->DrawTextW(s.c_str(), static_cast<UINT32>(s.size()), fmt, r, b,
                  D2D1_DRAW_TEXT_OPTIONS_CLIP, DWRITE_MEASURING_MODE_NATURAL);
}

// Builds one series' path, one figure per unbroken run.
class PathBuilder {
public:
    explicit PathBuilder(ID2D1RenderTarget* rt) : rt_(rt) {
        if (SUCCEEDED(d2d::Factory()->CreatePathGeometry(&geo_))) {
            if (FAILED(geo_->Open(&sink_))) sink_ = nullptr;
        }
    }
    ~PathBuilder() {
        if (sink_) sink_->Release();
        if (geo_) geo_->Release();
    }
    PathBuilder(const PathBuilder&) = delete;
    PathBuilder& operator=(const PathBuilder&) = delete;

    // Adds a point, skipping one on the same pixel as the last.
    void Add(float x, float y) {
        if (!run_.empty()) {
            const D2D1_POINT_2F& l = run_.back();
            if (std::fabs(l.x - x) < 0.5f && std::fabs(l.y - y) < 0.5f) return;
        }
        run_.push_back(D2D1::Point2F(x, y));
    }
    void Break() {
        if (run_.size() >= 2 && sink_) {
            sink_->BeginFigure(run_[0], D2D1_FIGURE_BEGIN_HOLLOW);
            sink_->AddLines(run_.data() + 1, static_cast<UINT32>(run_.size() - 1));
            sink_->EndFigure(D2D1_FIGURE_END_OPEN);
        } else if (run_.size() == 1) {
            dots_.push_back(run_[0]);   // lone sample: draw a dot
        }
        run_.clear();
    }
    void Draw(ID2D1Brush* brush, float width) {
        Break();
        if (sink_) {
            sink_->Close();
            sink_->Release();
            sink_ = nullptr;
            rt_->DrawGeometry(geo_, brush, width);
        }
        for (const D2D1_POINT_2F& p : dots_) {
            rt_->FillEllipse(D2D1::Ellipse(p, width * 1.2f, width * 1.2f), brush);
        }
    }

private:
    ID2D1RenderTarget*          rt_;
    ID2D1PathGeometry*          geo_ = nullptr;
    ID2D1GeometrySink*          sink_ = nullptr;
    std::vector<D2D1_POINT_2F>  run_;
    std::vector<D2D1_POINT_2F>  dots_;
};

}  // namespace

uint32_t SeriesColour(int i) {
    // Okabe-Ito palette, with darker variants for the light theme.
    static const uint32_t kBaseDark[] = {0xE69F00, 0x56B4E9, 0x009E73, 0xF0E442,
                                         0x0072B2, 0xD55E00, 0xCC79A7};
    static const uint32_t kBaseLight[] = {0xA57200, 0x3F85AC, 0x008E67, 0x867F24,
                                          0x0072B2, 0xD05C00, 0xAF688F};
    const bool light = theme::CurrentMode() == theme::Mode::Light;
    constexpr int kBaseCount = 7;
    if (i < kBaseCount) return light ? kBaseLight[i] : kBaseDark[i];
    const int k = i - kBaseCount;
    const double hue = std::fmod(k * 0.618033988749895, 1.0);
    const double s = 0.7 + (k % 3) * 0.10;
    const double l = light ? 0.32 + (k % 2) * 0.08 : 0.5 + (k % 2) * 0.15;
    double r = 0, g = 0, b = 0;
    HlsToRgb(hue, l, s, &r, &g, &b);
    auto q = [](double x) {
        return static_cast<uint32_t>((std::max)(0.0, (std::min)(1.0, x)) * 255.0 + 0.5);
    };
    return (q(r) << 16) | (q(g) << 8) | q(b);
}

// ------------------------------------------------------------------ window
bool PlotView::Create(HWND parent, int id) {
    static bool registered = false;
    HINSTANCE inst = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(parent, GWLP_HINSTANCE));
    if (!registered) {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.style = CS_DBLCLKS;
        wc.lpfnWndProc = Proc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursorW(nullptr, IDC_CROSS);
        wc.hbrBackground = theme::SolidBrush(theme::BgPanel);
        wc.lpszClassName = kClass;
        if (!RegisterClassExW(&wc)) return false;
        registered = true;
    }
    hwnd_ = CreateWindowExW(0, kClass, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, 0, 0, 0,
                            parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), inst,
                            this);
    if (!hwnd_) return false;
    dpi_ = theme::WindowDpi(hwnd_);
    a11y::Annotate(hwnd_, L"Plot", a11y::Role::Graphic);
    return true;
}

void PlotView::Move(int x, int y, int w, int h) {
    MoveWindow(hwnd_, x, y, w, h, TRUE);
}

LRESULT CALLBACK PlotView::Proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    PlotView* self = nullptr;
    if (msg == WM_NCCREATE) {
        self = static_cast<PlotView*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        self->hwnd_ = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<PlotView*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (self) return self->Handle(msg, wp, lp);
    return DefWindowProcW(hwnd, msg, wp, lp);
}

D2D1_SIZE_F PlotView::SizeDip() const {
    RECT rc = {};
    GetClientRect(hwnd_, &rc);
    const float k = 96.0f / theme::UiDpiF(dpi_);
    return D2D1::SizeF(static_cast<float>(rc.right) * k, static_cast<float>(rc.bottom) * k);
}

D2D1_RECT_F PlotView::PlotRect(D2D1_SIZE_F size) const {
    const float right = HasRight() ? kRightAxis : kRight;
    return D2D1::RectF(kLeft, kTop, (std::max)(kLeft + 10.0f, size.width - right),
                       (std::max)(kTop + 10.0f, size.height - kBottom));
}

// ------------------------------------------------------------------ data
void PlotView::SetData(const core::Table* table, int x_column, std::vector<Series> series) {
    table_ = table;
    x_column_ = -1;
    xs_.clear();
    runs_.clear();
    x_kind_ = core::ColumnKind::Number;
    series_ = std::move(series);
    if (table_ && x_column >= 0 && static_cast<size_t>(x_column) < table_->columns.size()) {
        const core::Column& c = table_->columns[static_cast<size_t>(x_column)];
        // Forward-fill X gaps and split the rows into non-decreasing runs.
        double first = kNaN;
        for (double v : c.values) {
            if (!std::isnan(v)) { first = v; break; }
        }
        if (!std::isnan(first)) {
            x_column_ = x_column;
            x_kind_ = c.kind;
            xs_.resize(c.values.size());
            double prev = first;
            size_t b = 0;
            for (size_t i = 0; i < c.values.size(); ++i) {
                const double v = std::isnan(c.values[i]) ? prev : c.values[i];
                if (v < prev) {
                    runs_.push_back({b, i});
                    b = i;
                }
                xs_[i] = v;
                prev = v;
            }
            runs_.push_back({b, c.values.size()});
        }
    }
    if (table_ && runs_.empty()) runs_.push_back({0, table_->rows});
    FitAll();
}

void PlotView::SetSeries(std::vector<Series> series) {
    series_ = std::move(series);
    if (y_auto_) FitY();
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

double PlotView::X(size_t i) const {
    return xs_.empty() ? static_cast<double>(i) : xs_[i];
}

void PlotView::DataXRange(double* lo, double* hi) const {
    *lo = 0.0;
    *hi = 1.0;
    if (!table_ || table_->rows == 0) return;
    if (xs_.empty()) {
        *lo = 0.0;
        *hi = static_cast<double>(table_->rows - 1);
    } else {
        // Runs are sorted, so their ends are the extremes.
        *lo = std::numeric_limits<double>::infinity();
        *hi = -*lo;
        for (const Run& r : runs_) {
            *lo = (std::min)(*lo, xs_[r.b]);
            *hi = (std::max)(*hi, xs_[r.e - 1]);
        }
    }
    if (!(*hi > *lo)) {
        *lo -= 0.5;
        *hi += 0.5;
    }
}

void PlotView::FitAll() {
    DataXRange(&x_lo_, &x_hi_);
    y_auto_ = true;
    FitY();
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

size_t PlotView::LowerIndex(const Run& r, double x) const {
    if (xs_.empty()) {
        if (x <= static_cast<double>(r.b)) return r.b;
        return (std::min)(r.e, static_cast<size_t>(std::ceil(x)));
    }
    const auto first = xs_.begin() + static_cast<std::ptrdiff_t>(r.b);
    const auto last = xs_.begin() + static_cast<std::ptrdiff_t>(r.e);
    return static_cast<size_t>(std::lower_bound(first, last, x) - xs_.begin());
}

// Rows of `r` inside the X view, plus one on each side.
void PlotView::InView(const Run& r, size_t* i0, size_t* i1) const {
    *i0 = LowerIndex(r, x_lo_);
    if (*i0 > r.b) --*i0;
    *i1 = (std::min)(r.e, LowerIndex(r, x_hi_) + 1);
}

bool PlotView::HasRight() const {
    for (const Series& s : series_) {
        if (s.right) return true;
    }
    return false;
}

void PlotView::FitY() {
    FitRange(false, &y_lo_, &y_hi_);
    FitRange(true, &y2_lo_, &y2_hi_);
}

void PlotView::FitRange(bool right, double* out_lo, double* out_hi) const {
    double lo = std::numeric_limits<double>::infinity();
    double hi = -lo;
    if (table_) {
        for (const Series& s : series_) {
            if (s.right != right) continue;
            const std::vector<double>& v = table_->columns[static_cast<size_t>(s.column)].values;
            for (const Run& r : runs_) {
                size_t i0 = 0, i1 = 0;
                InView(r, &i0, &i1);
                for (size_t i = i0; i < i1 && i < v.size(); ++i) {
                    const double x = X(i);
                    if (x < x_lo_ || x > x_hi_) continue;   // skip the margin rows
                    const double y = v[i];
                    if (std::isnan(y)) continue;
                    if (y < lo) lo = y;
                    if (y > hi) hi = y;
                }
            }
        }
    }
    if (!std::isfinite(lo)) { lo = 0.0; hi = 1.0; }
    if (hi <= lo) {
        // Pad a flat line into a band.
        const double pad = (lo == 0.0) ? 1.0 : std::fabs(lo) * 0.05;
        lo -= pad;
        hi += pad;
    } else {
        const double pad = (hi - lo) * 0.05;
        lo -= pad;
        hi += pad;
    }
    *out_lo = lo;
    *out_hi = hi;
}

void PlotView::SetXRange(double lo, double hi) {
    if (!(hi > lo)) return;
    // Limit the zoom to a millionth of the data span.
    double dlo = 0, dhi = 1;
    DataXRange(&dlo, &dhi);
    const double min_span = (std::max)((dhi - dlo) * 1e-6, 1e-9);
    if (hi - lo < min_span) {
        const double mid = (lo + hi) * 0.5;
        lo = mid - min_span * 0.5;
        hi = mid + min_span * 0.5;
    }
    x_lo_ = lo;
    x_hi_ = hi;
    if (y_auto_) FitY();
}

bool PlotView::Nearest(double x, size_t* row) const {
    if (!table_ || table_->rows == 0) return false;
    size_t best = 0;
    double best_d = std::numeric_limits<double>::infinity();
    for (const Run& r : runs_) {
        size_t i = LowerIndex(r, x);
        if (i >= r.e) i = r.e - 1;
        if (i > r.b && std::fabs(X(i - 1) - x) <= std::fabs(X(i) - x)) --i;
        const double d = std::fabs(X(i) - x);
        if (d < best_d) { best_d = d; best = i; }
    }
    *row = best;
    return true;
}

double PlotView::PxToX(float px, const D2D1_RECT_F& p) const {
    return x_lo_ + (static_cast<double>(px) - p.left) / (p.right - p.left) * (x_hi_ - x_lo_);
}
double PlotView::PxToY(float py, const D2D1_RECT_F& p, bool right) const {
    const double lo = right ? y2_lo_ : y_lo_, hi = right ? y2_hi_ : y_hi_;
    return lo + (p.bottom - static_cast<double>(py)) / (p.bottom - p.top) * (hi - lo);
}
float PlotView::XToPx(double x, const D2D1_RECT_F& p) const {
    return static_cast<float>(p.left + (x - x_lo_) / (x_hi_ - x_lo_) * (p.right - p.left));
}
float PlotView::YToPx(double y, const D2D1_RECT_F& p, bool right) const {
    const double lo = right ? y2_lo_ : y_lo_, hi = right ? y2_hi_ : y_hi_;
    return static_cast<float>(p.bottom - (y - lo) / (hi - lo) * (p.bottom - p.top));
}

std::wstring PlotView::XLabel(double x, double step) const {
    switch (x_kind_) {
        case core::ColumnKind::DateTime:
            if (step >= 86400.0) return core::FormatDateTime(x, true, step).substr(0, 10);
            return core::FormatDateTime(x, false, step);
        case core::ColumnKind::Time:
            return core::FormatDateTime(x, false, step);
        default:
            return FormatTick(x, step);
    }
}

// ------------------------------------------------------------------ drawing
void PlotView::Paint() {
    d2d::Frame frame(hwnd_, dpi_);
    ID2D1RenderTarget* rt = frame.rt();
    if (!rt) return;
    Draw(rt, frame.brushes(), frame.size(), true);
}

void PlotView::Draw(ID2D1RenderTarget* rt, d2d::Brushes& br, D2D1_SIZE_F size, bool live) {
    rt->Clear(theme::D2d(theme::BgPanel));
    const D2D1_RECT_F p = PlotRect(size);
    const float pw = p.right - p.left, ph = p.bottom - p.top;

    if (!table_ || table_->rows == 0) {
        DrawText(rt, L"Open a CSV or fixed-width record file,\n"
                     L"or drop one onto this window.",
                 theme::Font::Ui, br.Brush(theme::FgSub), D2D1::RectF(0, 0, size.width, size.height),
                 DWRITE_TEXT_ALIGNMENT_CENTER);
        return;
    }

    rt->FillRectangle(p, br.Brush(theme::PlotBg));

    // ------------------------------------------------------------ grid
    std::vector<double> yt, xt;
    const int y_intervals = (std::max)(3, (std::min)(10, static_cast<int>(ph / 55.0f)));
    const double y_step = NiceStep((y_hi_ - y_lo_) / y_intervals);
    Ticks(y_lo_, y_hi_, y_step, &yt);
    const int x_intervals = (std::max)(2, (std::min)(14, static_cast<int>(pw / 120.0f)));
    const bool timeish = x_kind_ == core::ColumnKind::DateTime || x_kind_ == core::ColumnKind::Time;
    const double x_step = timeish ? TimeStep((x_hi_ - x_lo_) / x_intervals)
                                  : NiceStep((x_hi_ - x_lo_) / x_intervals);
    Ticks(x_lo_, x_hi_, x_step, &xt);

    ID2D1Brush* grid = br.Brush(theme::GridLines);
    ID2D1Brush* sub = br.Brush(theme::FgSub);
    for (double v : yt) {
        const float y = std::round(YToPx(v, p)) + 0.5f;
        rt->DrawLine(D2D1::Point2F(p.left, y), D2D1::Point2F(p.right, y), grid, 1.0f);
        DrawText(rt, FormatTick(v, y_step), theme::Font::Subtitle, sub,
                 D2D1::RectF(0, y - 9, p.left - 8, y + 9), DWRITE_TEXT_ALIGNMENT_TRAILING);
    }
    if (HasRight()) {
        // Right axis: tick marks and labels only, no grid.
        std::vector<double> yt2;
        const double y2_step = NiceStep((y2_hi_ - y2_lo_) / y_intervals);
        Ticks(y2_lo_, y2_hi_, y2_step, &yt2);
        for (double v : yt2) {
            const float y = std::round(YToPx(v, p, true)) + 0.5f;
            rt->DrawLine(D2D1::Point2F(p.right - 5, y), D2D1::Point2F(p.right, y), sub, 1.0f);
            DrawText(rt, FormatTick(v, y2_step), theme::Font::Subtitle, sub,
                     D2D1::RectF(p.right + 8, y - 9, size.width, y + 9),
                     DWRITE_TEXT_ALIGNMENT_LEADING);
        }
        // Colour keys above each axis for its series.
        float lx = p.left - 8, rx = p.right + 8;
        for (const Series& s : series_) {
            ID2D1SolidColorBrush* key = nullptr;
            rt->CreateSolidColorBrush(D2D1::ColorF(s.rgb), &key);
            if (!key) continue;
            if (s.right) {
                if (rx + 10 <= size.width - 2) {
                    rt->FillRectangle(D2D1::RectF(rx, 4, rx + 10, 7), key);
                    rx += 13;
                }
            } else if (lx - 10 >= 2) {
                rt->FillRectangle(D2D1::RectF(lx - 10, 4, lx, 7), key);
                lx -= 13;
            }
            key->Release();
        }
    }
    long long prev_day = (std::numeric_limits<long long>::min)();
    for (double v : xt) {
        const float x = std::round(XToPx(v, p)) + 0.5f;
        rt->DrawLine(D2D1::Point2F(x, p.top), D2D1::Point2F(x, p.bottom), grid, 1.0f);
        std::wstring label = XLabel(v, x_step);
        // Add the date under the first tick and wherever the day changes.
        if (x_kind_ == core::ColumnKind::DateTime && x_step < 86400.0) {
            const long long day = static_cast<long long>(std::floor(v / 86400.0));
            if (day != prev_day) {
                label += L"\n" + core::FormatDateTime(v, true, 1.0).substr(0, 10);
                prev_day = day;
            }
        }
        DrawText(rt, label, theme::Font::Subtitle, sub,
                 D2D1::RectF(x - 70, p.bottom + 5, x + 70, size.height),
                 DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    }
    rt->DrawRectangle(D2D1::RectF(p.left + 0.5f, p.top + 0.5f, p.right - 0.5f, p.bottom - 0.5f),
                      br.Brush(theme::GridLines), 1.0f);

    // ------------------------------------------------------------ series
    rt->PushAxisAlignedClip(p, D2D1_ANTIALIAS_MODE_ALIASED);
    // Decimate per DIP column.
    const int columns = (std::max)(1, static_cast<int>(pw));
    for (const Series& s : series_) {
        const std::vector<double>& v = table_->columns[static_cast<size_t>(s.column)].values;
        ID2D1SolidColorBrush* brush = nullptr;
        rt->CreateSolidColorBrush(D2D1::ColorF(s.rgb), &brush);
        if (!brush) continue;
        PathBuilder path(rt);
        for (const Run& r : runs_) {
            size_t i0 = 0, i1 = 0;
            InView(r, &i0, &i1);
            if (i1 - i0 <= static_cast<size_t>(columns) * 3) {
                for (size_t i = i0; i < i1; ++i) {
                    if (std::isnan(v[i])) { path.Break(); continue; }
                    path.Add(XToPx(X(i), p), YToPx(v[i], p, s.right));
                }
            } else {
                // Keep first, min, max and last sample per screen column.
                int col = -1;
                size_t first = 0, last = 0, lo = 0, hi = 0;
                bool have = false;
                auto flush = [&] {
                    if (!have) return;
                    size_t idx[4] = {first, (std::min)(lo, hi), (std::max)(lo, hi), last};
                    size_t prev = static_cast<size_t>(-1);
                    for (size_t k : idx) {
                        if (k == prev) continue;
                        path.Add(XToPx(X(k), p), YToPx(v[k], p, s.right));
                        prev = k;
                    }
                    have = false;
                };
                const double scale = columns / (x_hi_ - x_lo_);
                for (size_t i = i0; i < i1; ++i) {
                    if (std::isnan(v[i])) { flush(); path.Break(); col = -1; continue; }
                    const int c = static_cast<int>(std::floor((X(i) - x_lo_) * scale));
                    if (c != col || !have) {
                        flush();
                        col = c;
                        first = last = lo = hi = i;
                        have = true;
                        continue;
                    }
                    last = i;
                    if (v[i] < v[lo]) lo = i;
                    if (v[i] > v[hi]) hi = i;
                }
                flush();
            }
            path.Break();   // no line between runs
        }
        path.Draw(brush, kTraceW);
        brush->Release();
    }
    rt->PopAxisAlignedClip();

    // ------------------------------------------------------------ box zoom
    const float k = 96.0f / theme::UiDpiF(dpi_);
    if (live && boxing_) {
        const D2D1_RECT_F box = D2D1::RectF(
            (std::min)(drag_start_.x, drag_now_.x) * k, (std::min)(drag_start_.y, drag_now_.y) * k,
            (std::max)(drag_start_.x, drag_now_.x) * k, (std::max)(drag_start_.y, drag_now_.y) * k);
        rt->FillRectangle(box, br.Brush(theme::Accent, 0.12f));
        rt->DrawRectangle(box, br.Brush(theme::Accent), 1.0f);
    }

    // ------------------------------------------------------------ hover readout
    if (!live || !hover_ || panning_ || boxing_ || series_.empty()) return;
    const float mx = hover_pt_.x * k, my = hover_pt_.y * k;
    if (mx < p.left || mx > p.right || my < p.top || my > p.bottom) return;
    size_t row = 0;
    if (!Nearest(PxToX(mx, p), &row)) return;
    const float gx = XToPx(X(row), p);
    if (gx < p.left || gx > p.right) return;
    rt->DrawLine(D2D1::Point2F(gx, p.top), D2D1::Point2F(gx, p.bottom),
                 br.Brush(theme::FgSub, 0.6f), 1.0f);

    std::vector<std::wstring> lines;
    // Row's exact X, with milliseconds when present.
    const double xr = X(row);
    const double resolution = (std::fabs(xr - std::round(xr)) > 1e-6) ? 0.001 : 1.0;
    std::wstring head;
    if (x_kind_ == core::ColumnKind::DateTime)
        head = core::FormatDateTime(xr, true, resolution);
    else if (x_kind_ == core::ColumnKind::Time)
        head = core::FormatDateTime(xr, false, resolution);
    else if (x_column_ >= 0)
        head = FormatValue(X(row));
    else
        head = core::Fmt(L"Row %zu", row + 1);
    if (x_column_ >= 0) head = table_->columns[static_cast<size_t>(x_column_)].name + L": " + head;
    float wmax = TextWidth(head, theme::Font::UiBold);
    for (const Series& s : series_) {
        const core::Column& c = table_->columns[static_cast<size_t>(s.column)];
        const double y = c.values[row];
        lines.push_back(c.name + (s.right ? L" (right)" : L"") + L": " + FormatValue(y));
        wmax = (std::max)(wmax, TextWidth(lines.back(), theme::Font::Ui) + 16.0f);
        if (!std::isnan(y)) {
            const float py = YToPx(y, p, s.right);
            if (py >= p.top && py <= p.bottom) {
                ID2D1SolidColorBrush* dot = nullptr;
                rt->CreateSolidColorBrush(D2D1::ColorF(s.rgb), &dot);
                if (dot) {
                    rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(gx, py), 3.5f, 3.5f), dot);
                    rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(gx, py), 3.5f, 3.5f),
                                    br.Brush(theme::PlotBg), 1.0f);
                    dot->Release();
                }
            }
        }
    }
    constexpr float kLineH = 18.0f, kPad = 8.0f;
    const int room = static_cast<int>((ph - 2 * kPad) / kLineH) - 1;
    const size_t shown = (std::min)(lines.size(), static_cast<size_t>((std::max)(room, 0)));
    const float bw = wmax + 2 * kPad;
    const float bh = kLineH * static_cast<float>(shown + 1) + 2 * kPad;
    float bx = gx + 14.0f;
    if (bx + bw > p.right - 4) bx = gx - 14.0f - bw;
    if (bx < p.left + 4) bx = p.left + 4;
    float by = my - bh * 0.5f;
    by = (std::max)(p.top + 4, (std::min)(by, p.bottom - 4 - bh));
    const D2D1_ROUNDED_RECT box = D2D1::RoundedRect(D2D1::RectF(bx, by, bx + bw, by + bh), 6, 6);
    rt->FillRoundedRectangle(box, br.Brush(theme::BgMain, 0.94f));
    rt->DrawRoundedRectangle(box, br.Brush(theme::Border), 1.0f);
    float ty = by + kPad;
    DrawText(rt, head, theme::Font::UiBold, br.Brush(theme::FgText),
             D2D1::RectF(bx + kPad, ty, bx + bw - kPad, ty + kLineH), DWRITE_TEXT_ALIGNMENT_LEADING);
    ty += kLineH;
    for (size_t i = 0; i < shown; ++i) {
        ID2D1SolidColorBrush* sw = nullptr;
        rt->CreateSolidColorBrush(D2D1::ColorF(series_[i].rgb), &sw);
        if (sw) {
            rt->FillRoundedRectangle(
                D2D1::RoundedRect(D2D1::RectF(bx + kPad, ty + 5, bx + kPad + 9, ty + 14), 2, 2), sw);
            sw->Release();
        }
        DrawText(rt, lines[i], theme::Font::Ui, br.Brush(theme::FgText),
                 D2D1::RectF(bx + kPad + 16, ty, bx + bw - kPad, ty + kLineH),
                 DWRITE_TEXT_ALIGNMENT_LEADING);
        ty += kLineH;
    }
}

// ------------------------------------------------------------------ export
bool PlotView::SavePng(const std::wstring& path, float scale) {
    RECT rc = {};
    GetClientRect(hwnd_, &rc);
    const UINT w = static_cast<UINT>(rc.right * scale), h = static_cast<UINT>(rc.bottom * scale);
    const float dpi = theme::UiDpiF(dpi_) * scale;
    const D2D1_SIZE_F size = SizeDip();
    return d2d::SavePng(path, w, h, dpi, [&](ID2D1RenderTarget* rt, d2d::Brushes& b) {
        Draw(rt, b, size, false);
    });
}

bool PlotView::RenderToPng(const core::Table* table, int x_column,
                           const std::vector<Series>& series, UINT width, UINT height,
                           const std::wstring& path, float hover_fx) {
    PlotView v;
    v.SetData(table, x_column, series);
    const D2D1_SIZE_F size = D2D1::SizeF(static_cast<float>(width), static_cast<float>(height));
    const bool hover = hover_fx >= 0.0f && hover_fx <= 1.0f;
    if (hover) {
        // Convert the readout position from DIPs to window pixels.
        const D2D1_RECT_F p = v.PlotRect(size);
        const float k = theme::UiDpiF(v.dpi_) / 96.0f;
        v.hover_ = true;
        v.hover_pt_ = {static_cast<LONG>((p.left + (p.right - p.left) * hover_fx) * k),
                       static_cast<LONG>((p.top + (p.bottom - p.top) * 0.35f) * k)};
    }
    return d2d::SavePng(path, width, height, 96.0f, [&](ID2D1RenderTarget* rt, d2d::Brushes& b) {
        v.Draw(rt, b, size, hover);
    });
}

// ------------------------------------------------------------------ input
void PlotView::ZoomAt(float px, float py, double fx, double fy) {
    const D2D1_RECT_F p = PlotRect(SizeDip());
    if (fx != 1.0) {
        const double a = PxToX(px, p);
        SetXRange(a + (x_lo_ - a) * fx, a + (x_hi_ - a) * fx);
    }
    if (fy != 1.0) {
        const double a = PxToY(py, p);
        const double lo = a + (y_lo_ - a) * fy, hi = a + (y_hi_ - a) * fy;
        const double mag = (std::max)(std::fabs(lo), std::fabs(hi));
        if (hi - lo >= (std::max)(mag * 1e-9, 1e-12)) {
            // Zoom the right axis about the same point.
            const double a2 = PxToY(py, p, true);
            y2_lo_ = a2 + (y2_lo_ - a2) * fy;
            y2_hi_ = a2 + (y2_hi_ - a2) * fy;
            y_lo_ = lo;
            y_hi_ = hi;
            y_auto_ = false;
        }
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

LRESULT PlotView::Handle(UINT msg, WPARAM wp, LPARAM lp) {
    const float k = 96.0f / theme::UiDpiF(dpi_);
    switch (msg) {
        case WM_PAINT:
            Paint();
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_SIZE:
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        case WM_DPICHANGED_AFTERPARENT:
            dpi_ = theme::WindowDpi(hwnd_);
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        case WM_GETDLGCODE:
            return DLGC_WANTARROWS;

        case WM_KEYDOWN:
            if (wp == VK_HOME) { FitAll(); return 0; }
            break;

        case WM_LBUTTONDBLCLK:
            FitAll();
            return 0;

        case WM_LBUTTONDOWN: {
            if (!table_) return 0;
            SetFocus(hwnd_);
            drag_start_ = drag_now_ = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            const bool ctrl = (wp & MK_CONTROL) != 0;
            maybe_box_ = ctrl;
            maybe_drag_ = !ctrl;
            pan_x_lo_ = x_lo_; pan_x_hi_ = x_hi_;
            pan_y_lo_ = y_lo_; pan_y_hi_ = y_hi_;
            pan_y2_lo_ = y2_lo_; pan_y2_hi_ = y2_hi_;
            SetCapture(hwnd_);
            return 0;
        }

        case WM_MOUSEMOVE: {
            drag_now_ = hover_pt_ = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            if (!hover_) {
                hover_ = true;
                TRACKMOUSEEVENT t = {sizeof(t), TME_LEAVE, hwnd_, 0};
                TrackMouseEvent(&t);
            }
            const int adx = std::abs(drag_now_.x - drag_start_.x);
            const int ady = std::abs(drag_now_.y - drag_start_.y);
            if ((maybe_drag_ || maybe_box_) && (adx >= kDragSlopPx || ady >= kDragSlopPx)) {
                panning_ = maybe_drag_;
                boxing_ = maybe_box_;
                maybe_drag_ = maybe_box_ = false;
            }
            if (panning_) {
                const D2D1_RECT_F p = PlotRect(SizeDip());
                const double dx = (drag_now_.x - drag_start_.x) * k / (p.right - p.left) *
                                  (pan_x_hi_ - pan_x_lo_);
                x_lo_ = pan_x_lo_ - dx;
                x_hi_ = pan_x_hi_ - dx;
                // A vertical drag pans Y too; otherwise Y stays auto-fitted.
                if (ady >= kDragSlopPx) {
                    const double f = (drag_now_.y - drag_start_.y) * k / (p.bottom - p.top);
                    const double dy = f * (pan_y_hi_ - pan_y_lo_);
                    const double dy2 = f * (pan_y2_hi_ - pan_y2_lo_);
                    y_lo_ = pan_y_lo_ + dy;
                    y_hi_ = pan_y_hi_ + dy;
                    y2_lo_ = pan_y2_lo_ + dy2;
                    y2_hi_ = pan_y2_hi_ + dy2;
                    y_auto_ = false;
                } else if (y_auto_) {
                    FitY();
                }
            }
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }

        case WM_MOUSELEAVE:
            hover_ = false;
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;

        case WM_LBUTTONUP: {
            if (boxing_) {
                const D2D1_RECT_F p = PlotRect(SizeDip());
                const float x0 = (std::min)(drag_start_.x, drag_now_.x) * k;
                const float x1 = (std::max)(drag_start_.x, drag_now_.x) * k;
                const float y0 = (std::min)(drag_start_.y, drag_now_.y) * k;
                const float y1 = (std::max)(drag_start_.y, drag_now_.y) * k;
                if (x1 - x0 > 3.0f && y1 - y0 > 3.0f) {
                    const double ny0 = PxToY(y1, p), ny1 = PxToY(y0, p);
                    const double n2y0 = PxToY(y1, p, true), n2y1 = PxToY(y0, p, true);
                    y_auto_ = false;
                    SetXRange(PxToX(x0, p), PxToX(x1, p));
                    y_lo_ = ny0;
                    y_hi_ = ny1;
                    y2_lo_ = n2y0;
                    y2_hi_ = n2y1;
                }
            }
            panning_ = boxing_ = maybe_drag_ = maybe_box_ = false;
            ReleaseCapture();
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }

        case WM_CAPTURECHANGED:
            panning_ = boxing_ = maybe_drag_ = maybe_box_ = false;
            return 0;

        case WM_MOUSEWHEEL: {
            if (!table_) return 0;
            POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ScreenToClient(hwnd_, &pt);
            const int delta = GET_WHEEL_DELTA_WPARAM(wp);
            const double f = (delta > 0) ? 1.0 / kWheelFactor : kWheelFactor;
            const bool shift = (GET_KEYSTATE_WPARAM(wp) & MK_SHIFT) != 0;
            const bool ctrl = (GET_KEYSTATE_WPARAM(wp) & MK_CONTROL) != 0;
            ZoomAt(pt.x * k, pt.y * k, ctrl ? 1.0 : f, shift ? 1.0 : f);
            return 0;
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
