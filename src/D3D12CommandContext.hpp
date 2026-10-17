#pragma once

#include <d3d12.h>
#include <mutex>
#include <stdexcept>
#include <wrl.h>

class GpuWaitTimeout : public std::runtime_error {
public:
    GpuWaitTimeout() : std::runtime_error{"GPU wait timed out"} {}
};

class D3D12CommandContext {
public:
    template <typename T> using ComPtr = Microsoft::WRL::ComPtr<T>;

    D3D12CommandContext(ID3D12Device* device, const wchar_t* name = L"REFD2D D3D12CommandContext object");
    // The owner must retire submitted GPU work before destroying its resources.
    ~D3D12CommandContext() noexcept;

    void wait(uint32_t ms);
    ComPtr<ID3D12GraphicsCommandList>& begin();
    void end(ID3D12CommandQueue* queue);
    void cancel() noexcept;
    void signal(ID3D12CommandQueue* queue);
    bool completed() const noexcept;
    [[nodiscard]] bool is_setup() const { return m_is_setup; }

private:
    ComPtr<ID3D12Device> m_device;
    ComPtr<ID3D12CommandAllocator> m_cmd_allocator;
    ComPtr<ID3D12GraphicsCommandList> m_cmd_list;
    ComPtr<ID3D12Fence> m_fence;
    UINT64 m_fence_value{};
    HANDLE m_fence_event{};
    std::recursive_mutex m_mtx;
    bool m_recording{};
    bool m_untracked_submission{};
    bool m_is_setup{};
};
