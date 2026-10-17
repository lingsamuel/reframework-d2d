#include "D3D12CommandContext.hpp"
#include <stdexcept>

D3D12CommandContext::D3D12CommandContext(ID3D12Device* device, const wchar_t* name) : m_device{device} {
    if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_cmd_allocator))) ||
        FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_cmd_allocator.Get(), nullptr, IID_PPV_ARGS(&m_cmd_list))) ||
        FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence)))) {
        throw std::runtime_error{"Failed to create command context"};
    }
    if (FAILED(m_cmd_list->Close())) throw std::runtime_error{"Failed to close initial command list"};
    m_cmd_allocator->SetName(name);
    m_cmd_list->SetName(name);
    m_fence->SetName(name);
    m_fence_event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (!m_fence_event) throw std::runtime_error{"Failed to create fence event"};
    m_is_setup = true;
}

D3D12CommandContext::~D3D12CommandContext() noexcept {
    // Resetting an allocator/list is a recording operation, never a cleanup operation.
    if (m_fence_event) CloseHandle(m_fence_event);
}

bool D3D12CommandContext::completed() const noexcept {
    if (FAILED(m_device->GetDeviceRemovedReason())) return true;
    return !m_untracked_submission && m_fence->GetCompletedValue() >= m_fence_value;
}

void D3D12CommandContext::wait(uint32_t ms) {
    std::scoped_lock lock{m_mtx};
    if (FAILED(m_device->GetDeviceRemovedReason())) throw std::runtime_error{"D3D12 device was removed"};
    if (completed()) return;
    const auto deadline = GetTickCount64() + ms;
    for (;;) {
        // A previous timed-out wait may have left an old event signal behind.
        ResetEvent(m_fence_event);
        if (FAILED(m_fence->SetEventOnCompletion(m_fence_value, m_fence_event)))
            throw std::runtime_error{"Failed to arm GPU fence event"};
        if (completed()) break;
        const auto now = GetTickCount64();
        if (now >= deadline) throw GpuWaitTimeout{};
        const auto result = WaitForSingleObject(m_fence_event, static_cast<DWORD>(deadline - now));
        if (result == WAIT_FAILED) throw std::runtime_error{"GPU wait failed"};
        if (completed()) break;
        if (result == WAIT_TIMEOUT) throw GpuWaitTimeout{};
    }
    if (FAILED(m_device->GetDeviceRemovedReason())) throw std::runtime_error{"D3D12 device was removed"};
}

D3D12CommandContext::ComPtr<ID3D12GraphicsCommandList>& D3D12CommandContext::begin() {
    std::scoped_lock lock{m_mtx};
    if (m_untracked_submission) throw std::runtime_error{"Untracked GPU submission must be retired"};
    wait(100);
    cancel();
    if (FAILED(m_cmd_allocator->Reset()) || FAILED(m_cmd_list->Reset(m_cmd_allocator.Get(), nullptr)))
        throw std::runtime_error{"Failed to reset command recording"};
    m_recording = true;
    return m_cmd_list;
}

void D3D12CommandContext::cancel() noexcept {
    std::scoped_lock lock{m_mtx};
    if (m_recording) m_cmd_list->Close();
    m_recording = false;
}

void D3D12CommandContext::signal(ID3D12CommandQueue* queue) {
    // Publish the value only after Signal succeeds. A failed submission is retired using
    // a separate tail fence; the renderer must never reuse that submission's allocator.
    const auto value = m_fence_value + 1;
    if (FAILED(queue->Signal(m_fence.Get(), value))) throw std::runtime_error{"Failed to signal GPU fence"};
    m_fence_value = value;
}

void D3D12CommandContext::end(ID3D12CommandQueue* queue) {
    std::scoped_lock lock{m_mtx};
    const auto hr = m_cmd_list->Close();
    m_recording = false;
    if (FAILED(hr)) throw std::runtime_error{"Failed to close command list"};
    ID3D12CommandList* lists[] = {m_cmd_list.Get()};
    m_untracked_submission = true;
    queue->ExecuteCommandLists(1, lists);
    signal(queue);
    m_untracked_submission = false;
}
