// Main window: header with file commands, a sidebar and the plot.
#pragma once

#include <windows.h>

#include <memory>
#include <string>
#include <vector>

#include "core/table.h"
#include "ui/controls.h"
#include "ui/plot_view.h"
#include "ui/series_list.h"

namespace ui {

class MainWindow {
public:
    bool Create(HINSTANCE inst);
    void Show();
    HWND hwnd() const { return hwnd_; }

    // Opens, detects and plots a file. Reports a failure in a dialog and returns false.
    bool OpenFile(const std::wstring& path);

    // Handles keyboard shortcuts; true if `m` was one.
    bool PreTranslate(const MSG& m);

private:
    static LRESULT CALLBACK Proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT Handle(UINT msg, WPARAM wp, LPARAM lp);

    // Vertical positions in the sidebar, in DIPs, shared by Layout and Paint.
    struct SidebarSlots {
        float file_heading, file_name, info_row0;
        float x_heading, x_field;
        float series_heading, series_auto, series_list;
        float bottom;
    };
    SidebarSlots Slots(float client_h) const;

    void Layout();
    void Paint();
    void OnCommand(int id, int code);
    void ChooseAndOpen();
    void SaveImage();
    void ApplySeries();       // checked rows -> plot
    // The checked rows, minus the X column, as plot series.
    std::vector<PlotView::Series> CheckedSeries() const;
    // Assigns the checked series to Y axes when automatic axes are on.
    void AutoAssignAxes();
    void RebuildXChoices();
    void Retheme();
    void UpdateTitle();

    HWND hwnd_ = nullptr;
    UINT dpi_ = 96;

    Button     btn_open_;
    Button     btn_png_;
    Button     btn_fit_;
    Dropdown   dd_x_;
    Button     btn_all_;
    Button     btn_none_;
    Checkbox   chk_auto_;
    SeriesList list_;
    PlotView   plot_;

    std::unique_ptr<core::Table> table_;
    std::wstring                 path_;
    int                          x_column_ = -1;
    std::vector<int>             list_columns_;   // row of list_ -> column of table_
};

}  // namespace ui
