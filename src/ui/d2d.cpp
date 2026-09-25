// Direct2D factories, shared DC render target, brush caches and PNG export.
#include "d2d.h"

#include <wincodec.h>

#pragma comment(lib, "windowscodecs.lib")

namespace d2d {
namespace {

ID2D1Factory*   g_factory = nullptr;
IDWriteFactory* g_write = nullptr;

}  // namespace

ID2D1Factory* Factory() {
    if (!g_factory) {
        D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &g_factory);
    }
    return g_factory;
}

IDWriteFactory* WriteFactory() {
    if (!g_write) {
        DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                            reinterpret_cast<IUnknown**>(&g_write));
    }
    return g_write;
}

void Shutdown() {
    SharedDc().Discard();
    if (g_write)   { g_write->Release();   g_write = nullptr; }
    if (g_factory) { g_factory->Release(); g_factory = nullptr; }
}

// ------------------------------------------------------------------ DcTarget
DcTarget& SharedDc() {
    static DcTarget t;
    return t;
}

ID2D1RenderTarget* DcTarget::Begin(HDC dc, const RECT& rc, UINT dpi) {
    ID2D1Factory* factory = Factory();
    if (!factory || !dc) return nullptr;

    if (!rt_) {
        // Opaque painting, so alpha is ignored.
        const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE),
            0.0f, 0.0f, D2D1_RENDER_TARGET_USAGE_NONE, D2D1_FEATURE_LEVEL_DEFAULT);
        if (FAILED(factory->CreateDCRenderTarget(&props, &rt_))) {
            rt_ = nullptr;
            return nullptr;
        }
        dpi_ = 0;  // force the SetDpi below
    }
    if (dpi != dpi_) {
        // Use the app's DPI, which includes the UI scale.
        const float ui = theme::UiDpiF(dpi);
        rt_->SetDpi(ui, ui);
        dpi_ = dpi;
    }
    if (FAILED(rt_->BindDC(dc, &rc))) return nullptr;
    // Remembered so End can repaint a lost frame (null for memory DCs).
    painting_ = WindowFromDC(dc);
    rt_->BeginDraw();
    // BindDC does not reset the transform.
    rt_->SetTransform(D2D1::Matrix3x2F::Identity());
    return rt_;
}

bool DcTarget::End() {
    if (!rt_) return true;
    const HWND painted = painting_;
    painting_ = nullptr;
    if (rt_->EndDraw() == D2DERR_RECREATE_TARGET) {
        Discard();
        // Repaint the lost frame, a limited number of times in a row.
        constexpr int kMaxRepaintsAfterLoss = 3;
        if (painted && ++losses_ <= kMaxRepaintsAfterLoss) {
            InvalidateRect(painted, nullptr, FALSE);
        }
        return false;
    }
    losses_ = 0;
    return true;
}

ID2D1SolidColorBrush* DcTarget::Brush(theme::Color c) {
    if (!rt_) return nullptr;
    if (!brushes_[c]) {
        if (FAILED(rt_->CreateSolidColorBrush(theme::D2d(c), &brushes_[c]))) {
            brushes_[c] = nullptr;
        }
    }
    return brushes_[c];
}

ID2D1SolidColorBrush* DcTarget::Brush(theme::Color c, float alpha) {
    if (!rt_) return nullptr;
    if (!scratch_) {
        if (FAILED(rt_->CreateSolidColorBrush(theme::D2d(c, alpha), &scratch_))) {
            return nullptr;
        }
        return scratch_;
    }
    scratch_->SetColor(theme::D2d(c, alpha));
    return scratch_;
}

void DcTarget::Discard() {
    for (auto& b : brushes_) {
        if (b) { b->Release(); b = nullptr; }
    }
    if (scratch_) { scratch_->Release(); scratch_ = nullptr; }
    if (rt_)      { rt_->Release();      rt_ = nullptr; }
    dpi_ = 0;
}

// ------------------------------------------------------------- TargetBrushes
TargetBrushes::~TargetBrushes() {
    for (auto& b : brushes_) {
        if (b) { b->Release(); b = nullptr; }
    }
    if (scratch_) { scratch_->Release(); scratch_ = nullptr; }
}

ID2D1SolidColorBrush* TargetBrushes::Brush(theme::Color c) {
    if (!rt_) return nullptr;
    if (!brushes_[c]) {
        if (FAILED(rt_->CreateSolidColorBrush(theme::D2d(c), &brushes_[c]))) {
            brushes_[c] = nullptr;
        }
    }
    return brushes_[c];
}

ID2D1SolidColorBrush* TargetBrushes::Brush(theme::Color c, float alpha) {
    if (!rt_) return nullptr;
    if (!scratch_) {
        if (FAILED(rt_->CreateSolidColorBrush(theme::D2d(c, alpha), &scratch_))) {
            return nullptr;
        }
        return scratch_;
    }
    scratch_->SetColor(theme::D2d(c, alpha));
    return scratch_;
}

// -------------------------------------------------------------------- SavePng
bool SavePng(const std::wstring& path, UINT width, UINT height, float dpi,
             const std::function<void(ID2D1RenderTarget*, Brushes&)>& draw) {
    ID2D1Factory* factory = Factory();
    if (!factory || width == 0 || height == 0) return false;

    IWICImagingFactory* wic = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&wic)))) {
        return false;
    }

    bool ok = false;
    IWICBitmap* bitmap = nullptr;
    IWICStream* stream = nullptr;
    IWICBitmapEncoder* encoder = nullptr;
    IWICBitmapFrameEncode* frame = nullptr;
    ID2D1RenderTarget* rt = nullptr;

    do {
        if (FAILED(wic->CreateBitmap(width, height, GUID_WICPixelFormat32bppPBGRA,
                                     WICBitmapCacheOnLoad, &bitmap))) {
            break;
        }
        const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
            dpi, dpi);
        if (FAILED(factory->CreateWicBitmapRenderTarget(bitmap, props, &rt))) break;

        {
            TargetBrushes brushes(rt);
            rt->BeginDraw();
            draw(rt, brushes);
            if (FAILED(rt->EndDraw())) break;
        }  // brushes must be released before the target

        if (FAILED(wic->CreateStream(&stream))) break;
        if (FAILED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE))) break;
        if (FAILED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder))) break;
        if (FAILED(encoder->Initialize(stream, WICBitmapEncoderNoCache))) break;
        if (FAILED(encoder->CreateNewFrame(&frame, nullptr))) break;
        if (FAILED(frame->Initialize(nullptr))) break;
        // Store the DPI in the file (pHYs); failure is not fatal.
        frame->SetResolution(static_cast<double>(dpi), static_cast<double>(dpi));
        if (FAILED(frame->WriteSource(bitmap, nullptr))) break;
        if (FAILED(frame->Commit())) break;
        if (FAILED(encoder->Commit())) break;
        ok = true;
    } while (false);

    if (frame)   frame->Release();
    if (encoder) encoder->Release();
    if (stream)  stream->Release();
    if (rt)      rt->Release();
    if (bitmap)  bitmap->Release();
    wic->Release();
    return ok;
}

// --------------------------------------------------------------------- Frame
Frame::Frame(HWND hwnd, UINT dpi) : hwnd_(hwnd) {
    HDC dc = BeginPaint(hwnd_, &ps_);
    RECT rc;
    GetClientRect(hwnd_, &rc);
    if (dc && rc.right > rc.left && rc.bottom > rc.top) {
        rt_ = SharedDc().Begin(dc, rc, dpi);
    }
}

Frame::~Frame() {
    if (rt_) SharedDc().End();
    EndPaint(hwnd_, &ps_);
}

D2D1_SIZE_F Frame::size() const {
    return rt_ ? rt_->GetSize() : D2D1::SizeF(0, 0);
}

HDC Frame::EndAndGetDc() {
    // Clearing rt_ stops the destructor ending the frame twice.
    if (rt_) {
        SharedDc().End();
        rt_ = nullptr;
    }
    return ps_.hdc;
}
}  // namespace d2d
