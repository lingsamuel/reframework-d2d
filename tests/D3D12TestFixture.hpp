#pragma once
#include <stdexcept>
#include <array>
#include <cstring>
#include <cstdint>
#include <DirectXPackedVector.h>
#include "D3D12Renderer.hpp"

namespace test {
using Microsoft::WRL::ComPtr;
inline void check_hr(HRESULT hr) {
    if (FAILED(hr)) throw std::runtime_error("D3D12 HRESULT: " + std::to_string(static_cast<unsigned long>(hr)));
}
// Queue a snapshot now and read it only after the caller's fence completes.
// Keeping readback separate from waiting allows tests to leave several frames in flight.
class PixelReadback {
public:
    PixelReadback(ID3D12Device* device, ID3D12Resource* texture) {
        auto desc = texture->GetDesc();
        m_format = desc.Format;
        UINT64 size{};
        device->GetCopyableFootprints(&desc, 0, 1, 0, &m_footprint, nullptr, nullptr, &size);
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC buffer{};
        buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buffer.Width = size;
        buffer.Height = buffer.DepthOrArraySize = buffer.MipLevels = buffer.SampleDesc.Count = 1;
        buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        check_hr(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&m_buffer)));
    }

    void record(ID3D12GraphicsCommandList* commands, ID3D12Resource* texture) {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = texture;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        commands->ResourceBarrier(1, &barrier);
        D3D12_TEXTURE_COPY_LOCATION source{}, destination{};
        source.pResource = texture;
        source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        destination.pResource = m_buffer.Get();
        destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        destination.PlacedFootprint = m_footprint;
        commands->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        commands->ResourceBarrier(1, &barrier);
    }

    std::array<float, 4> pixel() const {
        void* pixels{};
        D3D12_RANGE range{0, m_format == DXGI_FORMAT_R16G16B16A16_FLOAT ? 8u : 4u};
        check_hr(m_buffer->Map(0, &range, &pixels));
        std::array<float, 4> result{};
        if (m_format == DXGI_FORMAT_R16G16B16A16_FLOAT) {
            unsigned short channels[4];
            std::memcpy(channels, pixels, sizeof(channels));
            for (size_t i = 0; i < 4; ++i) result[i] = DirectX::PackedVector::XMConvertHalfToFloat(channels[i]);
        } else if (m_format == DXGI_FORMAT_R10G10B10A2_UNORM) {
            uint32_t packed;
            std::memcpy(&packed, pixels, sizeof(packed));
            for (size_t i = 0; i < 3; ++i) result[i] = ((packed >> (i * 10)) & 1023) / 1023.0f;
            result[3] = (packed >> 30) / 3.0f;
        } else {
            const auto* channels = static_cast<const unsigned char*>(pixels);
            for (size_t i = 0; i < 4; ++i) result[i] = channels[i] / 255.0f;
        }
        D3D12_RANGE written{0, 0};
        m_buffer->Unmap(0, &written);
        return result;
    }

private:
    ComPtr<ID3D12Resource> m_buffer;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT m_footprint{};
    DXGI_FORMAT m_format{};
};

class D3D12TestFixture {
public:
    D3D12TestFixture(DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM, UINT buffer_count = 2) {
        window = CreateWindowExW(0, L"STATIC", L"D2D regression test", WS_POPUP, 0, 0, 2, 2,
            nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (!window) throw std::runtime_error("Could not create hidden test window");
        ComPtr<IDXGIFactory4> factory;
        ComPtr<IDXGIAdapter> adapter;
        check_hr(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
        check_hr(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)));
        check_hr(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
        D3D12_COMMAND_QUEUE_DESC queue_desc{};
        check_hr(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
        DXGI_SWAP_CHAIN_DESC1 desc{};
        desc.Width = desc.Height = 2;
        desc.Format = format;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = buffer_count;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        ComPtr<IDXGISwapChain1> chain;
        check_hr(factory->CreateSwapChainForHwnd(queue.Get(), window, &desc, nullptr, nullptr, &chain));
        check_hr(chain.As(&swapchain));
        renderer = std::make_unique<D3D12Renderer>(swapchain.Get(), device.Get(), queue.Get());
    }

    ~D3D12TestFixture() {
        if (renderer) renderer->wait_for_idle();
        renderer.reset();
        DestroyWindow(window);
    }

    void record_clear_scene(ID3D12GraphicsCommandList* list, ID3D12Resource* backbuffer,
        const std::array<float, 4>& color) {
        if (!m_clear_heap) {
            D3D12_DESCRIPTOR_HEAP_DESC desc{};
            desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
            desc.NumDescriptors = 1;
            check_hr(device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&m_clear_heap)));
        }
        const auto rtv = m_clear_heap->GetCPUDescriptorHandleForHeapStart();
        device->CreateRenderTargetView(backbuffer, nullptr, rtv);
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = backbuffer;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        list->ResourceBarrier(1, &barrier);
        list->ClearRenderTargetView(rtv, color.data(), 0, nullptr);
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        list->ResourceBarrier(1, &barrier);
    }

    void clear_scene(const std::array<float, 4>& color = {0, 0, 0, 1}) {
        ComPtr<ID3D12Resource> backbuffer;
        check_hr(swapchain->GetBuffer(swapchain->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&backbuffer)));
        D3D12CommandContext commands(device.Get());
        record_clear_scene(commands.begin().Get(), backbuffer.Get(), color);
        commands.end(queue.Get());
        commands.wait(10000);
    }

    std::array<float, 4> read_pixel() {
        ComPtr<ID3D12Resource> backbuffer;
        check_hr(swapchain->GetBuffer(swapchain->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&backbuffer)));
        PixelReadback readback(device.Get(), backbuffer.Get());
        D3D12CommandContext commands(device.Get());
        readback.record(commands.begin().Get(), backbuffer.Get());
        commands.end(queue.Get());
        commands.wait(10000);
        return readback.pixel();
    }

    float read_red() { return read_pixel()[0]; }

    ComPtr<ID3D12DescriptorHeap> m_clear_heap;
    std::unique_ptr<D3D12Renderer> renderer;
public:
    HWND window{};
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<IDXGISwapChain3> swapchain;
};
}
