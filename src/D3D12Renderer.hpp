#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>

#include <d3d11.h>
#include <d3d11on12.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl.h>

#include "D3D12CommandContext.hpp"

#include "D2DPainter.hpp"
#include "HDRDisplayInfo.hpp"

class D3D12Renderer {
public:
    enum class OutputMode : uint32_t {
        SDR = 0,
        HDR10_PQ = 1,
        SCRGB = 2,
    };

    D3D12Renderer(IDXGISwapChain* swapchain_, ID3D12Device* device_, ID3D12CommandQueue* cmd_queue_);
    // Destruction follows ready_to_destroy(), or an explicit successful wait_for_idle().
    ~D3D12Renderer() noexcept = default;
    // Called only after the last CPU lease has gone away. Retries a failed tail Signal.
    bool ready_to_destroy() noexcept;
    bool matches(IDXGISwapChain* swapchain, ID3D12Device* device, ID3D12CommandQueue* queue) const;
    void wait_for_idle();
    auto try_lock() { return std::unique_lock<std::recursive_mutex>{m_mutex, std::try_to_lock}; }
    auto lock() { return std::unique_lock<std::recursive_mutex>{m_mutex}; }

    // Returns true only when the overlay was repainted; deferred requests remain pending.
    bool render(IDXGISwapChain* swapchain, std::function<void(D2DPainter&)> draw_fn, bool update_d2d);

    auto& get_d2d() { return m_d2d; }
    bool is_hdr_output() const {
        return m_output_mode == OutputMode::HDR10_PQ || (m_output_mode == OutputMode::SCRGB && m_hdr_display);
    }
    uint32_t get_output_mode() const { return static_cast<uint32_t>(m_output_mode); }
    float get_paper_white_nits() const { return m_paper_white_nits; }
    void set_paper_white_nits(float nits);

private:
    template <typename T> using ComPtr = Microsoft::WRL::ComPtr<T>;
    // Per-frame SRV layout:
    //   slot 0 -> D2D overlay texture (t0)
    //   slot 1 -> copied scene backbuffer (t1)
    static constexpr uint32_t SRV_D2D_SLOT = 0;
    static constexpr uint32_t SRV_SCENE_SLOT = 1;
    static constexpr uint32_t SRV_SLOTS_PER_FRAME = 2;

    struct Vert {
        float x, y;
        float u, v;
        unsigned int color;
    };

    // Identity only, never dereferenced or AddRef'd across host callbacks.
    uintptr_t m_swapchain_identity{};
    ComPtr<ID3D12Device> m_device{};
    ComPtr<ID3D12CommandQueue> m_cmd_queue{};

    ComPtr<ID3D11Device> m_d3d11_device{};
    ComPtr<ID3D11DeviceContext> m_d3d11_context{};
    ComPtr<ID3D11On12Device> m_d3d11on12_device{};
    ComPtr<ID3D11Resource> m_wrapped_rt{};

    ComPtr<ID3D12DescriptorHeap> m_rtv_heap{};
    ComPtr<ID3D12DescriptorHeap> m_srv_heap{};
    ComPtr<ID3D12Resource> m_overlay;
    std::recursive_mutex m_mutex;
    std::unique_ptr<D3D12CommandContext> m_tail;
    bool m_tail_signalled{};

    ComPtr<ID3D12RootSignature> m_root_signature{};
    ComPtr<ID3D12PipelineState> m_pipeline_state{};

    struct RenderResources {
        ComPtr<ID3D12Resource> vert_buffer{};
        ComPtr<ID3D12Resource> scene_copy{};
    };

    uint32_t m_frames_in_flight{1};
    std::vector<std::unique_ptr<D3D12CommandContext>> m_cmd_contexts{};
    std::vector<std::unique_ptr<RenderResources>> m_render_resources{};

    int m_width{};
    int m_height{};
    OutputMode m_output_mode{OutputMode::SDR};
    float m_paper_white_nits{200.0f};
    // Present's copy remains stable when a Lua resource operation owns m_mutex.
    float m_composite_paper_white_nits{200.0f};
    bool m_overlay_valid{};
    HWND m_output_window{};
    HDRDisplayInfo m_display_info{};
    bool m_hdr_display{};

    std::unique_ptr<D2DPainter> m_d2d{};
    void refresh_output_mode(IDXGISwapChain3* swapchain);
    DXGI_FORMAT m_backbuffer_format{};
    uint32_t get_srv_index(uint32_t frame_index, uint32_t slot) const { return frame_index * SRV_SLOTS_PER_FRAME + slot; }

    D3D12_CPU_DESCRIPTOR_HANDLE get_cpu_rtv() {
        return m_rtv_heap->GetCPUDescriptorHandleForHeapStart();
    }

    D3D12_CPU_DESCRIPTOR_HANDLE get_cpu_srv(uint32_t index) {
        return {m_srv_heap->GetCPUDescriptorHandleForHeapStart().ptr +
                index * m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV)};
    }

    D3D12_GPU_DESCRIPTOR_HANDLE get_gpu_srv(uint32_t index) {
        return {m_srv_heap->GetGPUDescriptorHandleForHeapStart().ptr +
                index * m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV)};
    }
};
