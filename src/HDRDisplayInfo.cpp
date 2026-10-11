#include "HDRDisplayInfo.hpp"

bool HDRDisplayInfo::is_hdr(HWND window) {
    if (window == nullptr) {
        return false;
    }

    auto monitor = MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
    if (m_factory == nullptr || !m_factory->IsCurrent() || monitor != m_monitor) {
        // A swap chain can retain a stale output; enumerate again when the monitor or display configuration changes.
        m_output.Reset();
        m_factory.Reset();
        m_monitor = monitor;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&m_factory)))) {
            return false;
        }

        for (UINT adapter_index = 0; m_output == nullptr; ++adapter_index) {
            Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
            if (FAILED(m_factory->EnumAdapters1(adapter_index, &adapter))) {
                break;
            }
            for (UINT output_index = 0; ; ++output_index) {
                Microsoft::WRL::ComPtr<IDXGIOutput> output;
                if (FAILED(adapter->EnumOutputs(output_index, &output))) {
                    break;
                }
                DXGI_OUTPUT_DESC desc{};
                if (SUCCEEDED(output->GetDesc(&desc)) && desc.Monitor == monitor) {
                    output.As(&m_output);
                    break;
                }
            }
        }
    }

    DXGI_OUTPUT_DESC1 desc{};
    return m_output != nullptr && SUCCEEDED(m_output->GetDesc1(&desc)) &&
        (desc.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 ||
            desc.ColorSpace == DXGI_COLOR_SPACE_RGB_STUDIO_G2084_NONE_P2020);
}
