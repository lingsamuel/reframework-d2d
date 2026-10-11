#include <stdexcept>
#include <cmath>
#include <DirectXPackedVector.h>
#include "D3D12Renderer.hpp"

using Microsoft::WRL::ComPtr;

namespace {
void check(HRESULT hr) {
    if (FAILED(hr)) throw std::runtime_error("D3D12 HRESULT: " + std::to_string(static_cast<unsigned long>(hr)));
}

class Fixture {
public:
    Fixture(DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM) {
        window = CreateWindowExW(0, L"STATIC", L"D2D regression test", WS_POPUP, 0, 0, 2, 2,
            nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (!window) throw std::runtime_error("Could not create hidden test window");
        ComPtr<IDXGIFactory4> factory;
        ComPtr<IDXGIAdapter> adapter;
        check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
        check(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)));
        check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
        D3D12_COMMAND_QUEUE_DESC queue_desc{};
        check(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
        DXGI_SWAP_CHAIN_DESC1 desc{};
        desc.Width = desc.Height = 2;
        desc.Format = format;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 2;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        ComPtr<IDXGISwapChain1> chain;
        check(factory->CreateSwapChainForHwnd(queue.Get(), window, &desc, nullptr, nullptr, &chain));
        check(chain.As(&swapchain));
        renderer = std::make_unique<D3D12Renderer>(swapchain.Get(), device.Get(), queue.Get());
    }

    ~Fixture() {
        renderer.reset();
        DestroyWindow(window);
    }

    float read_red() {
        ComPtr<ID3D12Resource> backbuffer, readback;
        check(swapchain->GetBuffer(swapchain->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&backbuffer)));
        auto texture_desc = backbuffer->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
        UINT64 size{};
        device->GetCopyableFootprints(&texture_desc, 0, 1, 0, &footprint, nullptr, nullptr, &size);
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC buffer_desc{};
        buffer_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buffer_desc.Width = size;
        buffer_desc.Height = buffer_desc.DepthOrArraySize = buffer_desc.MipLevels = buffer_desc.SampleDesc.Count = 1;
        buffer_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer_desc,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback)));
        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12GraphicsCommandList> commands;
        check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
        check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&commands)));
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = backbuffer.Get();
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        commands->ResourceBarrier(1, &barrier);
        D3D12_TEXTURE_COPY_LOCATION source{}, destination{};
        source.pResource = backbuffer.Get();
        source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        destination.pResource = readback.Get();
        destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        destination.PlacedFootprint = footprint;
        commands->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        commands->ResourceBarrier(1, &barrier);
        check(commands->Close());
        ID3D12CommandList* lists[] = {commands.Get()};
        queue->ExecuteCommandLists(1, lists);
        ComPtr<ID3D12Fence> fence;
        check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
        HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!event) throw std::runtime_error("Could not create readback fence event");
        check(queue->Signal(fence.Get(), 1));
        check(fence->SetEventOnCompletion(1, event));
        auto wait_result = WaitForSingleObject(event, 10000);
        CloseHandle(event);
        if (wait_result != WAIT_OBJECT_0) throw std::runtime_error("Readback fence timed out");
        void* pixels{};
        D3D12_RANGE read_range{0, 4};
        check(readback->Map(0, &read_range, &pixels));
        auto red = texture_desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT ?
            DirectX::PackedVector::XMConvertHalfToFloat(*static_cast<unsigned short*>(pixels)) :
            *static_cast<unsigned char*>(pixels) / 255.0f;
        D3D12_RANGE written_range{0, 0};
        readback->Unmap(0, &written_range);
        return red;
    }

    std::unique_ptr<D3D12Renderer> renderer;
private:
    HWND window{};
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<IDXGISwapChain3> swapchain;
};
}

void test_on12_composition() {
    Fixture f;
    f.renderer->render([](D2DPainter& painter) {
        painter.fill_rect(0, 0, 2, 2, 0xFF000000);
        painter.fill_rect(0, 0, 2, 2, 0x80FFFFFF);
    }, true);
    if (f.read_red() != 128.0f / 255.0f) throw std::runtime_error("D3D11On12 SDR composition did not preserve the expected encoded value");
    bool redrawn = false;
    f.renderer->render([&](D2DPainter&) { redrawn = true; }, false);
    if (redrawn || f.read_red() != 128.0f / 255.0f) throw std::runtime_error("Cached D2D overlay was not preserved");
}

void test_on12_scrgb_white() {
    Fixture f(DXGI_FORMAT_R16G16B16A16_FLOAT);
    if (f.renderer->get_output_mode() != 2) throw std::runtime_error("FP16 output is not scRGB");
    f.renderer->render([](D2DPainter& painter) { painter.fill_rect(0, 0, 2, 2, 0xFFFFFFFF); }, true);
    const float expected = f.renderer->is_hdr_output() ? 2.5f : 1.0f;
    if (std::abs(f.read_red() - expected) > 0.001f) throw std::runtime_error("Renderer scRGB white scale disagrees with display HDR state");
}
