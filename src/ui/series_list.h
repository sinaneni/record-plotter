// Scrolling list of plottable columns: colour swatch, checkbox and name per row.
#pragma once

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

namespace ui {

// WM_COMMAND notification codes sent to the parent.
constexpr WORD kSeriesToggled = 0x7A10;
constexpr WORD kSeriesAxisChanged = 0x7A11;

class SeriesList {
public:
    struct Row {
        std::wstring name;
        uint32_t     rgb = 0;
        bool         checked = false;
        bool         right = false;   // on the right Y axis
    };

    bool Create(HWND parent, int id);
    HWND hwnd() const { return hwnd_; }
    void Move(int x, int y, int w, int h);

    void SetRows(std::vector<Row> rows);
    const std::vector<Row>& rows() const { return rows_; }
    void SetAll(bool checked);   // does not notify
    // Sets which rows are on the right axis; does not notify.
    void SetAxes(const std::vector<bool>& right);

private:
    static LRESULT CALLBACK Proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT Handle(UINT msg, WPARAM wp, LPARAM lp);
    void Paint();
    int  RowAt(int y_px) const;
    int  RowPx() const;
    void ScrollTo(int top_px);
    void Toggle(int row);
    void SetAxis(int row, bool right);
    void UpdateTip();   // full name of a shortened hot row
    // Left edge of the axis badge, in DIPs.
    float BadgeLeft() const;

    HWND             hwnd_ = nullptr;
    int              id_ = 0;
    UINT             dpi_ = 96;
    std::vector<Row> rows_;
    int              scroll_px_ = 0;
    int              hot_ = -1;
    int              focus_row_ = 0;
    // Rows drawn shortened at the last paint.
    std::vector<bool> clipped_;
    HWND             tip_ = nullptr;
    int              tip_row_ = -2;
};

}  // namespace ui
