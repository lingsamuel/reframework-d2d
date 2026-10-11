#include <stdexcept>

#include "D2DImage.hpp"
#include <d2d1effects.h>

D2DImage::D2DImage(ComPtr<IWICImagingFactory> wic, ComPtr<ID2D1DeviceContext> context, std::filesystem::path filepath) {
    ComPtr<IWICBitmapDecoder> decoder{};

    if (FAILED(wic->CreateDecoderFromFilename(filepath.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &decoder))) {
        throw std::runtime_error{"Failed to create WIC decoder"};
    }

    ComPtr<IWICBitmapFrameDecode> frame{};

    if (FAILED(decoder->GetFrame(0, &frame))) {
        throw std::runtime_error{"Failed to get frame from WIC decoder"};
    }

    ComPtr<IWICFormatConverter> converter{};

    if (FAILED(wic->CreateFormatConverter(&converter))) {
        throw std::runtime_error{"Failed to create WIC format converter"};
    }

    if (FAILED(converter->Initialize(
            frame.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0.0f, WICBitmapPaletteTypeMedianCut))) {
        throw std::runtime_error{"Failed to initialize WIC format converter"};
    }

    if (FAILED(context->CreateBitmapFromWicBitmap(converter.Get(), nullptr, &m_bitmap))) {
        throw std::runtime_error{"Failed to create D2D bitmap from WIC bitmap"};
    }

    m_size = m_bitmap->GetPixelSize();

    ComPtr<ID2D1ColorContext> srgb, linear;
    if (FAILED(context->CreateColorContext(D2D1_COLOR_SPACE_SRGB, nullptr, 0, &srgb)) ||
        FAILED(context->CreateColorContext(D2D1_COLOR_SPACE_SCRGB, nullptr, 0, &linear))) {
        throw std::runtime_error{"Failed to create image color contexts"};
    }

    ComPtr<ID2D1Effect> conversion;
    if (FAILED(context->CreateEffect(CLSID_D2D1ColorManagement, &conversion)) ||
        FAILED(conversion->SetValue(D2D1_COLORMANAGEMENT_PROP_SOURCE_COLOR_CONTEXT, srgb.Get())) ||
        FAILED(conversion->SetValue(D2D1_COLORMANAGEMENT_PROP_DESTINATION_COLOR_CONTEXT, linear.Get())) ||
        FAILED(conversion->SetValue(D2D1_PROPERTY_PRECISION, D2D1_BUFFER_PRECISION_16BPC_FLOAT))) {
        throw std::runtime_error{"Failed to configure linear image conversion"};
    }
    conversion->SetInput(0, m_bitmap.Get());

    auto props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET,
        D2D1::PixelFormat(DXGI_FORMAT_R16G16B16A16_FLOAT, D2D1_ALPHA_MODE_PREMULTIPLIED),
        96.0f, 96.0f, linear.Get());
    if (FAILED(context->CreateBitmap(m_size, nullptr, 0, &props, &m_linear_bitmap))) {
        throw std::runtime_error{"Failed to create linear image bitmap"};
    }

    // Convert once at load time; keep using DrawBitmap for scaling and opacity during rendering.
    ComPtr<ID2D1Image> previous_target;
    context->GetTarget(&previous_target);
    context->SetTarget(m_linear_bitmap.Get());
    context->BeginDraw();
    context->Clear(D2D1::ColorF(0, 0, 0, 0));
    context->DrawImage(conversion.Get());
    auto hr = context->EndDraw();
    context->SetTarget(previous_target.Get());
    if (FAILED(hr)) {
        throw std::runtime_error{"Failed to convert image to linear color"};
    }
}
