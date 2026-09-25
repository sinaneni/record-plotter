// Main window: layout, painting, file handling and commands.
#include "ui/main_window.h"

#include <shellapi.h>
#include <windowsx.h>

#include <algorithm>

#include "core/util.h"
#include "ui/d2d.h"
#include "ui/dialogs.h"
#include "ui/theme.h"
#include "version.h"

namespace ui {
namespace {

const wchar_t kClass[] = L"RecordPlotterMainWindow";

// Layout metrics, in DIPs at 96 DPI.
constexpr float kMargin      = 16.0f;
constexpr float kSpacing     = 12.0f;
constexpr float kPanelRadius = 8.0f;
constexpr float kPanelPad    = 14.0f;
constexpr float kHeaderH     = 36.0f;
constexpr float kSidebarW    = 300.0f;
constexpr float kLabelW      = 92.0f;
constexpr float kInfoRowH    = 22.0f;
constexpr int   kInfoRows    = 6;
constexpr float kHeadingH    = 24.0f;
constexpr float kSectionGap  = 18.0f;
constexpr float kPlotInset   = 6.0f;

constexpr float kOpenBtnW = 120.0f;
constexpr float kPngBtnW  = 120.0f;
constexpr float kFitBtnW  = 90.0f;
constexpr float kBtnH     = 32.0f;
constexpr float kSmallBtnW = 64.0f;
constexpr float kSmallBtnH = 24.0f;

constexpr int kDefaultW = 1200;
constexpr int kDefaultH = 780;
constexpr int kMinW = 860;
constexpr int kMinH = 560;
constexpr DWORD kStyle = WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN;

// Number of series ticked when a file opens.
constexpr size_t kInitiallyChecked = 8;

enum : int { ID_OPEN = 100, ID_PNG, ID_FIT, ID_XCOL, ID_ALL, ID_NONE, ID_LIST, ID_PLOT, ID_AUTO };

constexpr float kAutoRowH = 26.0f;

// The automatic-axes setting, stored under HKCU; defaults to on.
const wchar_t kSettingsKey[] = L"Software\\SinaNeni\\RecordPlot";
const wchar_t kAutoAxesValue[] = L"AutoAxes";

bool LoadAutoAxes() {
    DWORD v = 1, size = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, kAutoAxesValue, RRF_RT_REG_DWORD, nullptr,
                     &v, &size) != ERROR_SUCCESS) {
        return true;
    }
    return v != 0;
}

void SaveAutoAxes(bool on) {
    const DWORD v = on ? 1 : 0;
    RegSetKeyValueW(HKEY_CURRENT_USER, kSettingsKey, kAutoAxesValue, REG_DWORD, &v, sizeof(v));
}

void DrawLabel(ID2D1RenderTarget* rt, const std::wstring& text, theme::Font font,
               ID2D1Brush* brush, const D2D1_RECT_F& r,
               DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING) {
    IDWriteTextFormat* fmt = theme::Text(font, align);
    if (!fmt || !brush || text.empty()) return;
    rt->DrawTextW(text.c_str(), static_cast<UINT32>(text.size()), fmt, r, brush,
                  D2D1_DRAW_TEXT_OPTIONS_CLIP, DWRITE_MEASURING_MODE_NATURAL);
}

std::wstring FileName(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

// 12345 -> "12.345".
std::wstring Grouped(size_t n) {
    std::wstring s = core::Fmt(L"%zu", n);
    for (int i = static_cast<int>(s.size()) - 3; i > 0; i -= 3) s.insert(static_cast<size_t>(i), 1, L',');
    return s;
}

}  // namespace

// ------------------------------------------------------------------ creation
bool MainWindow::Create(HINSTANCE inst) {
    RegisterControlClasses();
    theme::ReresolveSystem();

    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = Proc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        // App background, so the first frame is not white.
        wc.hbrBackground = theme::SolidBrush(theme::BgMain);
        wc.lpszClassName = kClass;
        wc.hIcon = theme::AppIconLarge();
        wc.hIconSm = theme::AppIconSmall();
        if (!RegisterClassExW(&wc)) return false;
        registered = true;
    }

    RECT rc = {0, 0, theme::Scale(kDefaultW, 96), theme::Scale(kDefaultH, 96)};
    theme::AdjustWindowRect(&rc, kStyle, 96);
    hwnd_ = CreateWindowExW(WS_EX_ACCEPTFILES, kClass, APP_NAME_STRW, kStyle, CW_USEDEFAULT,
                            CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top, nullptr,
                            nullptr, inst, this);
    if (!hwnd_) return false;
    dpi_ = theme::WindowDpi(hwnd_);
    theme::FitToWorkArea(hwnd_, kDefaultW, kDefaultH, kStyle, dpi_);
    theme::ApplyTitleBar(hwnd_);

    btn_open_.Create(hwnd_, ID_OPEN, L"OPEN FILE", Button::Variant::Accent);
    btn_open_.SetBackdrop(theme::BgMain);
    btn_open_.SetToolTip(L"Opens a CSV or fixed-width record file (Ctrl+O)");
    btn_png_.Create(hwnd_, ID_PNG, L"SAVE PNG", Button::Variant::Neutral);
    btn_png_.SetBackdrop(theme::BgMain);
    btn_png_.SetToolTip(L"Saves the plot as an image (Ctrl+S)");
    btn_fit_.Create(hwnd_, ID_FIT, L"FIT", Button::Variant::Neutral);
    btn_fit_.SetBackdrop(theme::BgMain);
    btn_fit_.SetToolTip(L"Shows all data (Home, or double-click the plot)");
    dd_x_.Create(hwnd_, ID_XCOL, {{L"Row number", -1}});
    btn_all_.Create(hwnd_, ID_ALL, L"All", Button::Variant::Neutral);
    btn_none_.Create(hwnd_, ID_NONE, L"None", Button::Variant::Neutral);
    chk_auto_.Create(hwnd_, ID_AUTO, L"Assign axes automatically", LoadAutoAxes());
    list_.Create(hwnd_, ID_LIST);
    plot_.Create(hwnd_, ID_PLOT);

    btn_png_.SetEnabled(false);
    btn_fit_.SetEnabled(false);
    dd_x_.SetEnabled(false);
    btn_all_.SetEnabled(false);
    btn_none_.SetEnabled(false);
    Layout();
    return true;
}

void MainWindow::Show() {
    theme::ShowPainted(hwnd_);
}

LRESULT CALLBACK MainWindow::Proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    MainWindow* self = nullptr;
    if (msg == WM_NCCREATE) {
        self = static_cast<MainWindow*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        self->hwnd_ = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (self) return self->Handle(msg, wp, lp);
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ------------------------------------------------------------------ layout
MainWindow::SidebarSlots MainWindow::Slots(float client_h) const {
    SidebarSlots s = {};
    const float top = kMargin + kHeaderH + kSpacing + kPanelPad;
    s.file_heading = top;
    s.file_name = s.file_heading + kHeadingH;
    s.info_row0 = s.file_name + kInfoRowH + 4.0f;
    s.x_heading = s.info_row0 + kInfoRowH * kInfoRows + kSectionGap;
    s.x_field = s.x_heading + kHeadingH;
    s.series_heading = s.x_field + static_cast<float>(kFieldH96) + kSectionGap;
    s.series_auto = s.series_heading + kHeadingH;
    s.series_list = s.series_auto + kAutoRowH + 4.0f;
    s.bottom = client_h - kMargin - kPanelPad;
    return s;
}

void MainWindow::Layout() {
    if (!hwnd_) return;
    RECT rc = {};
    GetClientRect(hwnd_, &rc);
    const float ui = theme::UiDpiF(dpi_);
    auto px = [ui](float dip) { return static_cast<int>(std::lround(dip * ui / 96.0f)); };
    const float W = static_cast<float>(rc.right) * 96.0f / ui;
    const float H = static_cast<float>(rc.bottom) * 96.0f / ui;

    // Header buttons, right-aligned.
    const float by = kMargin + (kHeaderH - kBtnH) * 0.5f;
    float x = W - kMargin - kFitBtnW;
    btn_fit_.Move(px(x), px(by), px(kFitBtnW), px(kBtnH));
    x -= kSpacing * 0.66f + kPngBtnW;
    btn_png_.Move(px(x), px(by), px(kPngBtnW), px(kBtnH));
    x -= kSpacing * 0.66f + kOpenBtnW;
    btn_open_.Move(px(x), px(by), px(kOpenBtnW), px(kBtnH));

    // Sidebar.
    const SidebarSlots s = Slots(H);
    const float sl = kMargin + kPanelPad;
    const float sw = kSidebarW - 2 * kPanelPad;
    dd_x_.Move(px(sl), px(s.x_field), px(sw), px(static_cast<float>(kFieldH96)));
    const float sby = s.series_heading + (kHeadingH - 6.0f - kSmallBtnH) * 0.5f - 1.0f;
    btn_none_.Move(px(sl + sw - kSmallBtnW), px(sby), px(kSmallBtnW), px(kSmallBtnH));
    btn_all_.Move(px(sl + sw - 2 * kSmallBtnW - 6.0f), px(sby), px(kSmallBtnW), px(kSmallBtnH));
    chk_auto_.Move(px(sl), px(s.series_auto), px(sw), px(kAutoRowH - 4.0f));
    list_.Move(px(sl - 4.0f), px(s.series_list), px(sw + 8.0f),
               (std::max)(0, px(s.bottom) - px(s.series_list)));

    // Plot, inside the content panel.
    const float cl = kMargin + kSidebarW + kSpacing + kPlotInset;
    const float ct = kMargin + kHeaderH + kSpacing + kPlotInset;
    plot_.Move(px(cl), px(ct), (std::max)(0, px(W - kMargin - kPlotInset) - px(cl)),
               (std::max)(0, px(H - kMargin - kPlotInset) - px(ct)));
}

void MainWindow::Paint() {
    d2d::Frame frame(hwnd_, dpi_);
    ID2D1RenderTarget* rt = frame.rt();
    if (!rt) return;
    d2d::DcTarget& host = frame.brushes();
    const D2D1_SIZE_F size = frame.size();
    rt->Clear(theme::D2d(theme::BgMain));

    // ---------------------------------------------------------------- header
    const float header_bottom = kMargin + kHeaderH;
    DrawLabel(rt, APP_NAME_STRW, theme::Font::Title, host.Brush(theme::Accent),
              D2D1::RectF(kMargin, kMargin, 500, header_bottom));

    // ------------------------------------------------------------------ body
    const float body_top = header_bottom + kSpacing;
    const float body_bottom = size.height - kMargin;
    const D2D1_RECT_F sidebar = D2D1::RectF(kMargin, body_top, kMargin + kSidebarW, body_bottom);
    const D2D1_RECT_F content =
        D2D1::RectF(sidebar.right + kSpacing, body_top, size.width - kMargin, body_bottom);
    rt->FillRoundedRectangle(D2D1::RoundedRect(sidebar, kPanelRadius, kPanelRadius),
                             host.Brush(theme::BgPanel));
    rt->FillRoundedRectangle(D2D1::RoundedRect(content, kPanelRadius, kPanelRadius),
                             host.Brush(theme::BgPanel));

    const SidebarSlots s = Slots(size.height);
    const float left = sidebar.left + kPanelPad;
    const float right = sidebar.right - kPanelPad;
    auto heading = [&](float y, const wchar_t* text) {
        DrawLabel(rt, text, theme::Font::UiBold, host.Brush(theme::FgText),
                  D2D1::RectF(left, y, right, y + 18));
    };
    auto divider = [&](float y) {
        rt->DrawLine(D2D1::Point2F(left, y), D2D1::Point2F(right, y),
                     host.Brush(theme::FgSub, 0.4f), 1.0f);
    };

    heading(s.file_heading, L"FILE");
    const core::Table* t = table_.get();
    DrawLabel(rt, t ? FileName(path_) : L"No file open", theme::Font::UiBold,
              host.Brush(t ? theme::Accent : theme::FgSub),
              D2D1::RectF(left, s.file_name, right, s.file_name + kInfoRowH));

    std::wstring values[kInfoRows];
    const wchar_t* labels[kInfoRows] = {L"Format", L"Encoding", L"Decimal", L"Header", L"Rows",
                                        L"Skipped"};
    if (t) {
        const core::Layout& l = t->layout;
        values[0] = core::DescribeLayout(l);
        values[1] = core::EncodingName(t->encoding, t->codepage);
        values[2] = l.decimal == L',' ? L"comma (12,5)" : L"point (12.5)";
        values[3] = !l.has_header ? L"none, columns numbered"
                  : l.unit_rows ? core::Fmt(L"yes, +%zu unit row(s)", l.unit_rows)
                                : L"yes";
        size_t cols = 0;
        for (const core::Column& c : t->columns) cols += c.merged_away ? 0 : 1;
        values[4] = Grouped(t->rows) + core::Fmt(L" rows, %zu columns", cols);
        values[5] = core::Fmt(L"%zu preamble, %zu non-data lines", l.preamble_lines,
                              t->skipped_rows);
    } else {
        for (std::wstring& v : values) v = L"—";
    }
    for (int i = 0; i < kInfoRows; ++i) {
        const float y = s.info_row0 + kInfoRowH * static_cast<float>(i);
        DrawLabel(rt, labels[i], theme::Font::Ui, host.Brush(theme::FgSub),
                  D2D1::RectF(left, y, left + kLabelW, y + kInfoRowH));
        DrawLabel(rt, values[i], theme::Font::Ui, host.Brush(theme::FgText),
                  D2D1::RectF(left + kLabelW, y, right, y + kInfoRowH));
    }

    divider(s.x_heading - kSectionGap * 0.5f);
    heading(s.x_heading, L"X AXIS");
    divider(s.series_heading - kSectionGap * 0.5f);
    heading(s.series_heading, L"SERIES");
}

// ------------------------------------------------------------------ files
bool MainWindow::OpenFile(const std::wstring& path) {
    HCURSOR old = SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    auto table = std::make_unique<core::Table>();
    std::wstring error;
    const bool ok = core::LoadTableFile(path, table.get(), &error);
    SetCursor(old);
    if (!ok) {
        ShowError(hwnd_, L"Could not open the file", error, path);
        return false;
    }

    // Release the plot's pointer to the old table first.
    plot_.SetData(nullptr, -1, {});
    table_ = std::move(table);
    path_ = path;
    x_column_ = table_->x_column;

    // List every numeric column, including the X column, so rows stay stable.
    list_columns_.clear();
    std::vector<SeriesList::Row> rows;
    size_t checked = 0;
    for (size_t j = 0; j < table_->columns.size(); ++j) {
        const core::Column& c = table_->columns[j];
        if (c.kind != core::ColumnKind::Number || c.merged_away) continue;
        const bool is_series = std::find(table_->series.begin(), table_->series.end(),
                                         static_cast<int>(j)) != table_->series.end();
        SeriesList::Row r;
        r.name = c.name;
        r.rgb = SeriesColour(static_cast<int>(rows.size()));
        r.checked = is_series && checked < kInitiallyChecked;
        if (r.checked) ++checked;
        rows.push_back(r);
        list_columns_.push_back(static_cast<int>(j));
    }
    list_.SetRows(std::move(rows));
    RebuildXChoices();
    AutoAssignAxes();

    btn_png_.SetEnabled(true);
    btn_fit_.SetEnabled(true);
    dd_x_.SetEnabled(true);
    btn_all_.SetEnabled(!list_columns_.empty());
    btn_none_.SetEnabled(!list_columns_.empty());

    plot_.SetData(table_.get(), x_column_, CheckedSeries());
    UpdateTitle();
    InvalidateRect(hwnd_, nullptr, FALSE);
    return true;
}

void MainWindow::RebuildXChoices() {
    std::vector<Dropdown::Item> items = {{L"Row number", -1}};
    if (table_) {
        for (size_t j = 0; j < table_->columns.size(); ++j) {
            const core::Column& c = table_->columns[j];
            if (c.merged_away || c.values.empty()) continue;
            if (c.kind == core::ColumnKind::Number || c.kind == core::ColumnKind::DateTime ||
                c.kind == core::ColumnKind::Time) {
                items.push_back({c.name, static_cast<int>(j)});
            }
        }
    }
    dd_x_.SetItems(std::move(items));
    dd_x_.SelectData(x_column_);
}

std::vector<PlotView::Series> MainWindow::CheckedSeries() const {
    std::vector<PlotView::Series> series;
    const auto& lr = list_.rows();
    for (size_t i = 0; i < lr.size(); ++i) {
        if (lr[i].checked && list_columns_[i] != x_column_) {
            series.push_back({list_columns_[i], lr[i].rgb, lr[i].right});
        }
    }
    return series;
}

void MainWindow::AutoAssignAxes() {
    if (!table_ || !chk_auto_.checked()) return;
    // Choose axes for the checked series; unchecked rows go to the left.
    std::vector<size_t> rows;
    std::vector<std::pair<double, double>> ranges;
    const auto& lr = list_.rows();
    for (size_t i = 0; i < lr.size(); ++i) {
        if (!lr[i].checked || list_columns_[i] == x_column_) continue;
        const core::Column& c = table_->columns[static_cast<size_t>(list_columns_[i])];
        rows.push_back(i);
        ranges.push_back({c.lo, c.hi});
    }
    const std::vector<bool> chosen = core::ChooseAxes(ranges);
    std::vector<bool> right(lr.size(), false);
    for (size_t k = 0; k < rows.size(); ++k) right[rows[k]] = chosen[k];
    list_.SetAxes(right);
}

void MainWindow::ApplySeries() {
    AutoAssignAxes();
    plot_.SetSeries(CheckedSeries());
}

void MainWindow::ChooseAndOpen() {
    std::wstring path = path_;
    if (OpenFileDialog(hwnd_, L"Open record file",
                       {{L"Record files (*.csv; *.txt; *.dat; *.log; *.tsv; *.prn)",
                         L"*.csv;*.txt;*.dat;*.log;*.tsv;*.prn"},
                        {L"All files (*.*)", L"*.*"}},
                       &path)) {
        OpenFile(path);
    }
}

void MainWindow::SaveImage() {
    if (!table_) return;
    std::wstring name = FileName(path_);
    const size_t dot = name.find_last_of(L'.');
    if (dot != std::wstring::npos) name.resize(dot);
    std::wstring out;
    int filter = 0;
    if (!SaveFileDialog(hwnd_, L"Save plot", {{L"PNG image (*.png)", L"*.png"}},
                        name + L".png", &out, &filter)) {
        return;
    }
    // Twice the window's resolution.
    if (!plot_.SavePng(out, 2.0f)) {
        ShowError(hwnd_, L"Could not save the image", L"The PNG file could not be written.", out);
    }
}

void MainWindow::UpdateTitle() {
    std::wstring title = APP_NAME_STRW;
    if (table_) title = FileName(path_) + L" — " + title;
    SetWindowTextW(hwnd_, title.c_str());
}

void MainWindow::Retheme() {
    theme::ApplyTitleBar(hwnd_);
    // Series colours are tuned per theme; re-issue them by row.
    if (table_) {
        std::vector<SeriesList::Row> rows = list_.rows();
        for (size_t i = 0; i < rows.size(); ++i) rows[i].rgb = SeriesColour(static_cast<int>(i));
        list_.SetRows(std::move(rows));
        ApplySeries();
    }
    RedrawWindow(hwnd_, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
}

// ------------------------------------------------------------------ messages
bool MainWindow::PreTranslate(const MSG& m) {
    // Catches Ctrl shortcuts before the focused control gets them.
    if (m.message != WM_KEYDOWN || GetAncestor(m.hwnd, GA_ROOT) != hwnd_) return false;
    if ((GetKeyState(VK_CONTROL) & 0x8000) == 0) return false;
    if (m.wParam == 'O') { ChooseAndOpen(); return true; }
    if (m.wParam == 'S') { SaveImage(); return true; }
    return false;
}

void MainWindow::OnCommand(int id, int code) {
    switch (id) {
        case ID_OPEN: ChooseAndOpen(); break;
        case ID_PNG:  SaveImage(); break;
        case ID_FIT:  plot_.FitAll(); break;
        case ID_ALL:
            list_.SetAll(true);
            ApplySeries();
            break;
        case ID_NONE:
            list_.SetAll(false);
            ApplySeries();
            break;
        case ID_LIST:
            // Moving a series by hand turns automatic axes off.
            if (code == kSeriesAxisChanged && chk_auto_.checked()) {
                chk_auto_.SetChecked(false);
                SaveAutoAxes(false);
            }
            if (code == kSeriesToggled || code == kSeriesAxisChanged) ApplySeries();
            break;
        case ID_AUTO:
            if (code == kCheckboxToggled) {
                SaveAutoAxes(chk_auto_.checked());
                ApplySeries();
            }
            break;
        case ID_XCOL:
            if (code == kDropdownChanged && table_) {
                x_column_ = dd_x_.SelectedData();
                AutoAssignAxes();
                plot_.SetData(table_.get(), x_column_, CheckedSeries());
            }
            break;
        default:
            break;
    }
}

LRESULT MainWindow::Handle(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_PAINT:
            Paint();
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_SIZE:
            Layout();
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        case WM_GETMINMAXINFO:
            theme::ClampMinTrackSize(reinterpret_cast<MINMAXINFO*>(lp), hwnd_, kMinW, kMinH,
                                     kStyle, dpi_);
            return 0;
        case WM_DPICHANGED: {
            const RECT* target = reinterpret_cast<const RECT*>(lp);
            dpi_ = HIWORD(wp);
            SetWindowPos(hwnd_, nullptr, target->left, target->top,
                         target->right - target->left, target->bottom - target->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            Layout();
            InvalidateRect(hwnd_, nullptr, TRUE);
            return 0;
        }
        case WM_SETTINGCHANGE: {
            const wchar_t* what = reinterpret_cast<const wchar_t*>(lp);
            if (what && wcscmp(what, L"ImmersiveColorSet") == 0 && theme::ReresolveSystem()) {
                Retheme();
            }
            return 0;
        }
        case WM_COMMAND:
            OnCommand(LOWORD(wp), HIWORD(wp));
            return 0;

        case WM_DROPFILES: {
            HDROP drop = reinterpret_cast<HDROP>(wp);
            wchar_t file[MAX_PATH * 4] = {};
            const bool got = DragQueryFileW(drop, 0, file, static_cast<UINT>(std::size(file))) > 0;
            DragFinish(drop);
            SetForegroundWindow(hwnd_);
            if (got) OpenFile(file);
            return 0;
        }

        // Send the wheel to the plot or list under the pointer.
        case WM_MOUSEWHEEL: {
            POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            const HWND under = WindowFromPoint(pt);
            if (under == plot_.hwnd() || under == list_.hwnd()) {
                return SendMessageW(under, msg, wp, lp);
            }
            break;
        }

        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}

}  // namespace ui
