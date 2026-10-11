#pragma once

#include <filesystem>
#include <tuple>

#include <d2d1_3.h>
#include <wincodec.h>
#include <wrl.h>

class D2DImage {
public:
    template <typename T> using ComPtr = Microsoft::WRL::ComPtr<T>;

    D2DImage(ComPtr<IWICImagingFactory> wic, ComPtr<ID2D1DeviceContext> context, std::filesystem::path filepath);

    ID2D1Bitmap* bitmap(bool linear = false) const { return linear ? m_linear_bitmap.Get() : m_bitmap.Get(); }
    auto size() const { return std::make_tuple(m_size.width, m_size.height); }

private:
    ComPtr<ID2D1Bitmap> m_bitmap{};
    ComPtr<ID2D1Bitmap1> m_linear_bitmap{};
    D2D1_SIZE_U m_size{};
};
