#pragma once

#include <filesystem>
#include <mutex>
#include <tuple>

#include <d2d1_3.h>
#include <wincodec.h>
#include <wrl.h>

class D2DImage {
public:
    template <typename T> using ComPtr = Microsoft::WRL::ComPtr<T>;

    D2DImage(ComPtr<IWICImagingFactory> wic, ComPtr<ID2D1DeviceContext> context, std::filesystem::path filepath);

    ComPtr<ID2D1Bitmap> bitmap(ID2D1DeviceContext* context, bool linear = false);
    auto size() const { return std::make_tuple(m_size.width, m_size.height); }

private:
    void create_bitmaps(ID2D1Device* device);
    std::mutex m_mutex;
    ComPtr<IWICBitmap> m_pixels;
    ComPtr<ID2D1Device> m_device;
    ComPtr<ID2D1Bitmap> m_bitmap{};
    ComPtr<ID2D1Bitmap1> m_linear_bitmap{};
    D2D1_SIZE_U m_size{};
};
