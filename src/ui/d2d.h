// Direct2D helpers: process-wide factories and a shared render target that handles device loss.
#pragma once

#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>

#include <functional>
#include <string>

#include "theme.h"

namespace d2d {

// Created on first use; single-threaded.
ID2D1Factory*   Factory();
IDWriteFactory* WriteFactory();
void            Shutdown();

// A source of palette brushes for a specific render target.
class Brushes {
public:
    virtual ~Brushes() = default;
    virtual ID2D1SolidColorBrush* Brush(theme::Color c) = 0;
    virtual ID2D1SolidColorBrush* Brush(theme::Color c, float alpha) = 0;
};

// A render target bound to a GDI device context; every window paints through it.
class DcTarget : public Brushes {
public:
    ~DcTarget() override { Discard(); }

    // Binds to `dc` over `rc` (device pixels) and begins a frame; drawing is in DIPs.
    // Returns nullptr on failure.
    ID2D1RenderTarget* Begin(HDC dc, const RECT& rc, UINT dpi);
    // False when the device was lost; the target is dropped and the window repainted.
    bool End();

    ID2D1SolidColorBrush* Brush(theme::Color c) override;
    ID2D1SolidColorBrush* Brush(theme::Color c, float alpha) override;
    void Discard();

private:
    ID2D1DCRenderTarget*  rt_ = nullptr;
    ID2D1SolidColorBrush* brushes_[theme::ColorCount] = {};
    ID2D1SolidColorBrush* scratch_ = nullptr;
    UINT                  dpi_ = 0;
    // Window being painted, and consecutive device losses.
    HWND                  painting_ = nullptr;
    int                   losses_ = 0;
};

// The process-wide DC target.
DcTarget& SharedDc();

// Palette brush cache for any other render target (e.g. off-screen export).
class TargetBrushes : public Brushes {
public:
    explicit TargetBrushes(ID2D1RenderTarget* rt) : rt_(rt) {}
    ~TargetBrushes() override;

    TargetBrushes(const TargetBrushes&) = delete;
    TargetBrushes& operator=(const TargetBrushes&) = delete;

    ID2D1SolidColorBrush* Brush(theme::Color c) override;
    ID2D1SolidColorBrush* Brush(theme::Color c, float alpha) override;

private:
    ID2D1RenderTarget*    rt_ = nullptr;
    ID2D1SolidColorBrush* brushes_[theme::ColorCount] = {};
    ID2D1SolidColorBrush* scratch_ = nullptr;
};

// Renders `draw` into an off-screen bitmap and saves it as a PNG at the given DPI.
bool SavePng(const std::wstring& path, UINT width, UINT height, float dpi,
             const std::function<void(ID2D1RenderTarget*, Brushes&)>& draw);

// WM_PAINT helper: BeginPaint, bind, and end the paint in the destructor.
// rt() is null if the frame should be skipped.
class Frame {
public:
    Frame(HWND hwnd, UINT dpi);
    ~Frame();

    Frame(const Frame&) = delete;
    Frame& operator=(const Frame&) = delete;

    ID2D1RenderTarget* rt() const { return rt_; }
    DcTarget&          brushes() const { return SharedDc(); }
    D2D1_SIZE_F        size() const;

    // Ends the Direct2D frame and returns the paint DC for GDI drawing on top.
    HDC EndAndGetDc();

private:
    HWND               hwnd_;
    PAINTSTRUCT        ps_ = {};
    ID2D1RenderTarget* rt_ = nullptr;
};

}  // namespace d2d
