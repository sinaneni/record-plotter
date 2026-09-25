// Plot view: a Direct2D child window that draws the table's series against its X column.
#pragma once

#include <windows.h>
#include <d2d1.h>

#include <cstdint>
#include <string>
#include <vector>

#include "core/table.h"
#include "d2d.h"

namespace ui {

// Colour of the i-th series.
uint32_t SeriesColour(int i);

class PlotView {
public:
    bool Create(HWND parent, int id);
    HWND hwnd() const { return hwnd_; }
    void Move(int x, int y, int w, int h);

    struct Series {
        int      column = -1;
        uint32_t rgb = 0;
        bool     right = false;   // on the right Y axis
    };
    // Sets the data and fits the view; null clears the plot.
    void SetData(const core::Table* table, int x_column, std::vector<Series> series);
    // Changes the drawn series, keeping the X view.
    void SetSeries(std::vector<Series> series);
    void FitAll();

    // Saves the current view as a PNG at `scale` times the window size.
    bool SavePng(const std::wstring& path, float scale);

    // Renders a PNG without a window; `hover_fx` in [0, 1] draws the readout there.
    static bool RenderToPng(const core::Table* table, int x_column,
                            const std::vector<Series>& series, UINT width, UINT height,
                            const std::wstring& path, float hover_fx = -1.0f);

private:
    static LRESULT CALLBACK Proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT Handle(UINT msg, WPARAM wp, LPARAM lp);

    void Paint();
    void Draw(ID2D1RenderTarget* rt, d2d::Brushes& brushes, D2D1_SIZE_F size, bool live);
    D2D1_RECT_F PlotRect(D2D1_SIZE_F size) const;
    D2D1_SIZE_F SizeDip() const;

    // X of row i, or the row number when there is no X column.
    double X(size_t i) const;
    void   DataXRange(double* lo, double* hi) const;
    void   FitY();
    // Padded value range of the series on one axis.
    void   FitRange(bool right, double* lo, double* hi) const;
    bool   HasRight() const;
    void   ZoomAt(float px, float py, double fx, double fy);
    void   SetXRange(double lo, double hi);
    // A stretch of rows over which X never decreases.
    struct Run { size_t b = 0, e = 0; };
    size_t LowerIndex(const Run& r, double x) const;   // first row of r with X >= x
    void   InView(const Run& r, size_t* i0, size_t* i1) const;
    bool   Nearest(double x, size_t* row) const;

    double PxToX(float px, const D2D1_RECT_F& p) const;
    double PxToY(float py, const D2D1_RECT_F& p, bool right = false) const;
    float  XToPx(double x, const D2D1_RECT_F& p) const;
    float  YToPx(double y, const D2D1_RECT_F& p, bool right = false) const;

    std::wstring XLabel(double x, double step) const;

    HWND hwnd_ = nullptr;
    UINT dpi_ = 96;

    const core::Table*  table_ = nullptr;
    int                 x_column_ = -1;
    std::vector<double> xs_;            // X values; empty = row number
    std::vector<Run>    runs_;
    core::ColumnKind    x_kind_ = core::ColumnKind::Number;
    std::vector<Series> series_;

    double x_lo_ = 0.0, x_hi_ = 1.0;
    double y_lo_ = 0.0, y_hi_ = 1.0;
    // Right Y axis range, moved together with the left one.
    double y2_lo_ = 0.0, y2_hi_ = 1.0;
    bool   y_auto_ = true;

    // Mouse.
    bool   hover_ = false;
    POINT  hover_pt_ = {};
    bool   maybe_drag_ = false, maybe_box_ = false, panning_ = false, boxing_ = false;
    POINT  drag_start_ = {}, drag_now_ = {};
    double pan_x_lo_ = 0, pan_x_hi_ = 0, pan_y_lo_ = 0, pan_y_hi_ = 0;
    double pan_y2_lo_ = 0, pan_y2_hi_ = 0;
};

}  // namespace ui
