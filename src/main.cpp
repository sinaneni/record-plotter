// Record Plotter - entry point.
//
//   RecordPlot.exe                      the window
//   RecordPlot.exe file.csv             the window, with the file open
//   RecordPlot.exe --render in out.png [width height] [--hover fraction] [--right 1,3]
//                                       no window: plot the file to a PNG and exit.
//                                       --hover draws the readout at that x fraction,
//                                       --right puts those series (0-based) on the right axis.
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shobjidl.h>

#include <string>
#include <vector>

#include "core/table.h"
#include "ui/a11y.h"
#include "ui/d2d.h"
#include "ui/main_window.h"
#include "ui/plot_view.h"
#include "ui/theme.h"
#include "version.h"

#pragma comment(lib, "comctl32.lib")

namespace {

const wchar_t kAppUserModelId[] = L"SinaNeni.RecordPlot";

std::vector<std::wstring> Args() {
    std::vector<std::wstring> out;
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return out;
    for (int i = 1; i < argc; ++i) out.push_back(argv[i]);
    LocalFree(argv);
    return out;
}

int Render(const std::vector<std::wstring>& a) {
    if (a.size() < 3) return 2;
    core::Table t;
    std::wstring err;
    if (!core::LoadTableFile(a[1], &t, &err)) return 3;
    UINT w = 1600, h = 900;
    if (a.size() >= 5 && a[3][0] != L'-') {
        w = static_cast<UINT>(_wtoi(a[3].c_str()));
        h = static_cast<UINT>(_wtoi(a[4].c_str()));
        if (w < 200 || h < 150) return 2;
    }
    std::vector<ui::PlotView::Series> series;
    for (size_t i = 0; i < t.series.size() && i < 8; ++i) {
        series.push_back({t.series[i], ui::SeriesColour(static_cast<int>(i))});
    }
    float hover = -1.0f;
    bool manual = false;
    for (size_t i = 3; i + 1 < a.size(); ++i) {
        if (a[i] == L"--right") manual = true;
    }
    if (!manual) {
        std::vector<std::pair<double, double>> ranges;
        for (const auto& s : series) {
            const core::Column& c = t.columns[static_cast<size_t>(s.column)];
            ranges.push_back({c.lo, c.hi});
        }
        const std::vector<bool> right = core::ChooseAxes(ranges);
        for (size_t i = 0; i < series.size(); ++i) series[i].right = right[i];
    }
    for (size_t i = 3; i + 1 < a.size(); ++i) {
        if (a[i] == L"--hover") hover = static_cast<float>(_wtof(a[i + 1].c_str()));
        if (a[i] == L"--right") {
            const wchar_t* p = a[i + 1].c_str();
            while (*p) {
                const size_t k = static_cast<size_t>(wcstoul(p, const_cast<wchar_t**>(&p), 10));
                if (k < series.size()) series[k].right = true;
                if (*p) ++p;
            }
        }
    }
    return ui::PlotView::RenderToPng(&t, t.x_column, series, w, h, a[2], hover) ? 0 : 4;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR, int) {
    SetCurrentProcessExplicitAppUserModelID(kAppUserModelId);

    INITCOMMONCONTROLSEX icc = {sizeof(icc), ICC_BAR_CLASSES | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);
    // COM for the file dialogs and WIC.
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    if (!d2d::Factory() || !d2d::WriteFactory()) {
        MessageBoxW(nullptr, L"Direct2D or DirectWrite could not be initialised.", APP_NAME_STRW,
                    MB_ICONERROR);
        return 1;
    }

    const std::vector<std::wstring> args = Args();
    int exit_code = 0;
    if (!args.empty() && args[0] == L"--render") {
        theme::ReresolveSystem();
        exit_code = Render(args);
    } else {
        ui::MainWindow win;
        if (!win.Create(inst)) {
            MessageBoxW(nullptr, L"The main window could not be created.", APP_NAME_STRW, MB_ICONERROR);
            return 1;
        }
        win.Show();
        if (!args.empty()) win.OpenFile(args[0]);

        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            if (win.PreTranslate(msg)) continue;
            // Dialog-style Tab navigation.
            const HWND root = GetAncestor(msg.hwnd, GA_ROOT);
            if (root && IsDialogMessageW(root, &msg)) continue;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        exit_code = static_cast<int>(msg.wParam);
    }

    ui::a11y::Shutdown();
    theme::Shutdown();
    d2d::Shutdown();
    if (SUCCEEDED(com)) CoUninitialize();
    return exit_code;
}
