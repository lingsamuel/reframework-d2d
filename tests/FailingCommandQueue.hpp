#pragma once
#include <atomic>
#include <d3d12.h>
#include <wrl.h>

// Delegate all real GPU work; fail only the caller's next Signal. Driver-internal
// interfaces are queried from the real queue so D3D11On12 still uses a native queue.
class FailingCommandQueue final : public ID3D12CommandQueue {
public:
    explicit FailingCommandQueue(ID3D12CommandQueue* queue) : m_queue{queue} {}
    void fail_next_signal() { m_fail.store(true); }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** result) override {
        if (id == __uuidof(IUnknown) || id == __uuidof(ID3D12Object) || id == __uuidof(ID3D12DeviceChild) ||
            id == __uuidof(ID3D12Pageable) || id == __uuidof(ID3D12CommandQueue)) {
            *result = static_cast<ID3D12CommandQueue*>(this); AddRef(); return S_OK;
        }
        return m_queue->QueryInterface(id, result);
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++m_refs; }
    ULONG STDMETHODCALLTYPE Release() override { const auto n = --m_refs; if (!n) delete this; return n; }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID id, UINT* size, void* data) override { return m_queue->GetPrivateData(id, size, data); }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID id, UINT size, const void* data) override { return m_queue->SetPrivateData(id, size, data); }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID id, const IUnknown* data) override { return m_queue->SetPrivateDataInterface(id, data); }
    HRESULT STDMETHODCALLTYPE SetName(LPCWSTR name) override { return m_queue->SetName(name); }
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID id, void** device) override { return m_queue->GetDevice(id, device); }
    void STDMETHODCALLTYPE UpdateTileMappings(ID3D12Resource* r, UINT n, const D3D12_TILED_RESOURCE_COORDINATE* c,
        const D3D12_TILE_REGION_SIZE* s, ID3D12Heap* h, UINT nr, const D3D12_TILE_RANGE_FLAGS* f,
        const UINT* offsets, const UINT* counts, D3D12_TILE_MAPPING_FLAGS flags) override {
        m_queue->UpdateTileMappings(r, n, c, s, h, nr, f, offsets, counts, flags);
    }
    void STDMETHODCALLTYPE CopyTileMappings(ID3D12Resource* dst, const D3D12_TILED_RESOURCE_COORDINATE* dc,
        ID3D12Resource* src, const D3D12_TILED_RESOURCE_COORDINATE* sc, const D3D12_TILE_REGION_SIZE* s,
        D3D12_TILE_MAPPING_FLAGS flags) override { m_queue->CopyTileMappings(dst, dc, src, sc, s, flags); }
    void STDMETHODCALLTYPE ExecuteCommandLists(UINT n, ID3D12CommandList* const* lists) override { m_queue->ExecuteCommandLists(n, lists); }
    void STDMETHODCALLTYPE SetMarker(UINT m, const void* p, UINT size) override { m_queue->SetMarker(m, p, size); }
    void STDMETHODCALLTYPE BeginEvent(UINT m, const void* p, UINT size) override { m_queue->BeginEvent(m, p, size); }
    void STDMETHODCALLTYPE EndEvent() override { m_queue->EndEvent(); }
    HRESULT STDMETHODCALLTYPE Signal(ID3D12Fence* fence, UINT64 value) override {
        if (m_fail.exchange(false)) return E_FAIL;
        return m_queue->Signal(fence, value);
    }
    HRESULT STDMETHODCALLTYPE Wait(ID3D12Fence* fence, UINT64 value) override { return m_queue->Wait(fence, value); }
    HRESULT STDMETHODCALLTYPE GetTimestampFrequency(UINT64* v) override { return m_queue->GetTimestampFrequency(v); }
    HRESULT STDMETHODCALLTYPE GetClockCalibration(UINT64* gpu, UINT64* cpu) override { return m_queue->GetClockCalibration(gpu, cpu); }
    D3D12_COMMAND_QUEUE_DESC STDMETHODCALLTYPE GetDesc() override { return m_queue->GetDesc(); }
private:
    std::atomic<ULONG> m_refs{1};
    std::atomic<bool> m_fail{false};
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> m_queue;
};
