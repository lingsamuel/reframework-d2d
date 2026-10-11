#pragma once

#include <dxgi1_6.h>
#include <wrl.h>

class HDRDisplayInfo {
public:
    bool is_hdr(HWND window);

private:
    Microsoft::WRL::ComPtr<IDXGIFactory1> m_factory{};
    Microsoft::WRL::ComPtr<IDXGIOutput6> m_output{};
    HMONITOR m_monitor{};
};
