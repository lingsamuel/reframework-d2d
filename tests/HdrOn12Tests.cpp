#include <stdexcept>
#include <cmath>
#include <future>
#include <thread>
#include <d3d12sdklayers.h>
#include <d3d12.h>
#include "D3D12TestFixture.hpp"
#include "RendererLifecycle.hpp"
#include "FailingCommandQueue.hpp"
#include <DirectXPackedVector.h>
#include "D3D12Renderer.hpp"

using Microsoft::WRL::ComPtr;

namespace {
void check(HRESULT hr) {
    if (FAILED(hr)) throw std::runtime_error("D3D12 HRESULT: " + std::to_string(static_cast<unsigned long>(hr)));
}

using Fixture = test::D3D12TestFixture;
}

void test_on12_composition() {
    Fixture f;
    f.renderer->render(f.swapchain.Get(), [](D2DPainter& painter) {
        painter.fill_rect(0, 0, 2, 2, 0xFF000000);
        painter.fill_rect(0, 0, 2, 2, 0x80FFFFFF);
    }, true);
    if (f.read_red() != 128.0f / 255.0f) throw std::runtime_error("D3D11On12 SDR composition did not preserve the expected encoded value");
    bool redrawn = false;
    f.renderer->render(f.swapchain.Get(), [&](D2DPainter&) { redrawn = true; }, false);
    if (redrawn || f.read_red() != 128.0f / 255.0f) throw std::runtime_error("Cached D2D overlay was not preserved");
}

void test_on12_scrgb_white() {
    Fixture f(DXGI_FORMAT_R16G16B16A16_FLOAT);
    if (f.renderer->get_output_mode() != 2) throw std::runtime_error("FP16 output is not scRGB");
    f.renderer->render(f.swapchain.Get(), [](D2DPainter& painter) { painter.fill_rect(0, 0, 2, 2, 0xFFFFFFFF); }, true);
    const float expected = f.renderer->is_hdr_output() ? 2.5f : 1.0f;
    if (std::abs(f.read_red() - expected) > 0.001f) throw std::runtime_error("Renderer scRGB white scale disagrees with display HDR state");
}


void test_context_pending_timeout() {
    Fixture f;
    ComPtr<ID3D12Fence> gate;
    check(f.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate)));
    check(f.queue->Wait(gate.Get(), 1));
    D3D12CommandContext context(f.device.Get());
    context.begin();
    context.end(f.queue.Get());
    bool timed_out = false;
    try { context.wait(1); }
    catch (const std::exception& e) {
        timed_out = std::string(e.what()).find("timed out") != std::string::npos;
    }
    // Always release the real GPU queue before an assertion or cleanup can throw.
    check(gate->Signal(1));
    context.wait(10000);
    if (!timed_out) throw std::runtime_error("A pending GPU submission was not reported as a timeout");
}

void test_on12_callback_exception() {
    Fixture f;
    bool caught = false;
    try {
        f.renderer->render(f.swapchain.Get(), [](D2DPainter&) { throw std::runtime_error("injected draw failure"); }, true);
    } catch (const std::exception&) { caught = true; }
    if (!caught) throw std::runtime_error("The injected draw failure was lost");
    // A leaked recursive-mutex lock is only visible when the next Present uses another thread.
    auto next = std::async(std::launch::async, [&] {
        f.renderer->render(f.swapchain.Get(), [](D2DPainter& p) { p.fill_rect(0, 0, 2, 2, 0xFFFFFFFF); }, true);
    });
    next.get();
    if (f.read_red() < 0.99f) throw std::runtime_error("Drawing did not recover after a callback exception");
}

void test_on12_resolution_reset() {
    Fixture f;
    for (UINT size : {4u, 8u, 2u, 16u, 4u}) {
        f.renderer->render(f.swapchain.Get(), [](D2DPainter& p) { p.fill_rect(0, 0, 32, 32, 0xFFFFFFFF); }, true);
        f.renderer->wait_for_idle();
        f.renderer.reset();
        check(f.swapchain->ResizeBuffers(0, size, size, DXGI_FORMAT_UNKNOWN, 0));
        f.renderer = std::make_unique<D3D12Renderer>(f.swapchain.Get(), f.device.Get(), f.queue.Get());
        auto [width, height] = f.renderer->get_d2d()->surface_size();
        if (width != size || height != size) throw std::runtime_error("Old surface size survived reset");
        f.renderer->render(f.swapchain.Get(), [](D2DPainter& p) { p.fill_rect(0, 0, 32, 32, 0xFFFFFFFF); }, true);
        if (f.read_red() < 0.99f) throw std::runtime_error("Resized surface did not render");
    }
}


void test_renderer_retirement() {
    Fixture f;
    f.renderer->wait_for_idle();
    f.renderer.reset();
    RendererLifecycle lifecycle;
    auto renderer = std::make_shared<D3D12Renderer>(f.swapchain.Get(), f.device.Get(), f.queue.Get());
    std::weak_ptr<D3D12Renderer> old = renderer;
    lifecycle.publish(std::move(renderer));
    auto lease = lifecycle.snapshot();
    lease.renderer->render(f.swapchain.Get(), [](D2DPainter& p) { p.fill_rect(0, 0, 2, 2, 0xFFFFFFFF); }, true);
    ComPtr<ID3D12Fence> gate;
    check(f.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate)));
    check(f.queue->Wait(gate.Get(), 1));
    lifecycle.reset();
    lifecycle.collect();
    if (old.expired()) throw std::runtime_error("An old CPU lease was invalidated");
    lease = {};
    lifecycle.collect();
    const bool retained_pending_gpu = !old.expired();
    check(gate->Signal(1));
    // The host must drain the same game queue before ResizeBuffers; do not resize a busy swapchain.
    D3D12CommandContext drain(f.device.Get());
    drain.signal(f.queue.Get());
    drain.wait(10000);
    check(f.swapchain->ResizeBuffers(0, 4, 4, DXGI_FORMAT_UNKNOWN, 0));
    lifecycle.collect();
    if (!retained_pending_gpu || !old.expired()) throw std::runtime_error("Renderer retirement did not follow the tail fence");
}

void test_live_renderer_does_not_pin_backbuffers() {
    Fixture f;
    f.renderer->render(f.swapchain.Get(), [](D2DPainter& p) { p.fill_rect(0, 0, 2, 2, 0xFFFFFFFF); }, true);
    f.renderer->wait_for_idle();
    check(f.swapchain->ResizeBuffers(0, 4, 4, DXGI_FORMAT_UNKNOWN, 0));
    if (f.renderer->matches(f.swapchain.Get(), f.device.Get(), f.queue.Get()))
        throw std::runtime_error("An old renderer accepted the resized surface");
}


void test_failed_frame_signal() {
    Fixture f;
    ComPtr<FailingCommandQueue> queue;
    queue.Attach(new FailingCommandQueue(f.queue.Get()));
    D3D12CommandContext context(f.device.Get());
    context.begin();
    queue->fail_next_signal();
    bool failed = false;
    try { context.end(queue.Get()); } catch (const std::exception&) { failed = true; }
    const bool claimed_complete = context.completed();
    bool refused_reuse = false;
    try { context.begin(); } catch (const std::exception&) { refused_reuse = true; }
    D3D12CommandContext drain(f.device.Get());
    drain.signal(f.queue.Get());
    drain.wait(10000);
    if (!failed || !refused_reuse || claimed_complete) throw std::runtime_error("Unfenced GPU work was reported complete or allowed allocator reuse");
}

void test_failed_tail_signal() {
    Fixture f;
    f.renderer->wait_for_idle();
    f.renderer.reset();
    ComPtr<FailingCommandQueue> queue;
    queue.Attach(new FailingCommandQueue(f.queue.Get()));
    RendererLifecycle lifecycle;
    auto renderer = std::make_shared<D3D12Renderer>(f.swapchain.Get(), f.device.Get(), queue.Get());
    renderer->render(f.swapchain.Get(), [](D2DPainter& p) { p.fill_rect(0, 0, 2, 2, 0xFFFFFFFF); }, true);
    std::weak_ptr<D3D12Renderer> old = renderer;
    lifecycle.publish(std::move(renderer));
    lifecycle.reset();
    queue->fail_next_signal();
    lifecycle.collect();
    const bool retained = !old.expired();
    lifecycle.collect();
    D3D12CommandContext drain(f.device.Get());
    drain.signal(f.queue.Get());
    drain.wait(10000);
    lifecycle.collect();
    if (!retained || !old.expired()) throw std::runtime_error("Failed tail Signal did not retain and retry retirement");
}

void test_font_cache_concurrency() {
    Fixture f;
    auto font = std::make_shared<D2DFont>(f.renderer->get_d2d()->dwrite(), "Arial", 12, false, false);
    auto measure = std::async(std::launch::async, [&] {
        for (int i = 0; i < 2000; ++i) {
            auto [w, h] = font->measure(std::to_string(i % 173));
            if (!(w > 0 && h > 0)) throw std::runtime_error("Concurrent measurement failed");
        }
    });
    for (int i = 0; i < 2000; ++i) {
        if (!font->layout(std::to_string(i % 157))) throw std::runtime_error("Concurrent layout failed");
    }
    measure.get();
}


void test_device_removal_retirement() {
    Fixture f;
    f.renderer->wait_for_idle();
    f.renderer.reset();
    RendererLifecycle lifecycle;
    auto renderer = std::make_shared<D3D12Renderer>(f.swapchain.Get(), f.device.Get(), f.queue.Get());
    renderer->render(f.swapchain.Get(), [](D2DPainter& p) { p.fill_rect(0, 0, 2, 2, 0xFFFFFFFF); }, true);
    std::weak_ptr<D3D12Renderer> old = renderer;
    lifecycle.publish(std::move(renderer));
    ComPtr<ID3D12Device5> removable;
    check(f.device.As(&removable));
    removable->RemoveDevice();
    lifecycle.reset();
    lifecycle.collect();
    if (!old.expired()) throw std::runtime_error("Removed-device resources were not released");
}

void test_many_backbuffers() {
    Fixture f(DXGI_FORMAT_R8G8B8A8_UNORM, 6);
    for (int i = 0; i < 8; ++i) {
        f.renderer->render(f.swapchain.Get(), [](D2DPainter& p) { p.fill_rect(0, 0, 2, 2, 0xFFFFFFFF); }, true);
        if (f.read_red() < 0.99f) throw std::runtime_error("Six-buffer rendering failed");
        check(f.swapchain->Present(0, 0));
    }
}

void test_retired_renderer_does_not_pin_swapchain() {
    Fixture f;
    f.renderer->wait_for_idle();
    f.renderer.reset();
    RendererLifecycle lifecycle;
    lifecycle.publish(std::make_shared<D3D12Renderer>(f.swapchain.Get(), f.device.Get(), f.queue.Get()));
    // A Lua callback may retain the renderer through reset. This must not retain
    // DXGI's one-flip-chain-per-window association until a future Present.
    auto old_cpu_lease = lifecycle.snapshot();
    lifecycle.reset();
    lifecycle.collect();
    DXGI_SWAP_CHAIN_DESC1 desc{};
    check(f.swapchain->GetDesc1(&desc));
    f.swapchain.Reset();
    ComPtr<IDXGIFactory4> factory;
    check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
    ComPtr<IDXGISwapChain1> replacement;
    const HRESULT hr = factory->CreateSwapChainForHwnd(f.queue.Get(), f.window, &desc, nullptr, nullptr, &replacement);
    if (FAILED(hr)) throw std::runtime_error("Recreating the same HWND swapchain failed: " + std::to_string(static_cast<unsigned long>(hr)));
    check(replacement.As(&f.swapchain));
    old_cpu_lease = {};
    D3D12CommandContext drain(f.device.Get());
    lifecycle.collect();
    drain.signal(f.queue.Get());
    drain.wait(10000);
    lifecycle.collect();
}

void test_cached_overlay_during_graphics_access() {
    using namespace std::chrono_literals;
    Fixture f;
    f.renderer->render(f.swapchain.Get(), [](D2DPainter& p) { p.fill_rect(0, 0, 2, 2, 0xFFFFFFFF); }, true);
    if (f.read_red() < 0.99f) throw std::runtime_error("Initial cached overlay was not painted");
    std::promise<void> entered, release;
    auto entered_future = entered.get_future();
    auto release_future = release.get_future();
    auto resource_access = std::async(std::launch::async, [&] {
        auto graphics_lock = f.renderer->lock();
        entered.set_value();
        release_future.wait();
    });
    entered_future.wait();
    auto drawing = std::async(std::launch::async, [&] {
        bool repainted = false;
        for (int i = 0; i < 4; ++i) {
            f.clear_scene();
            f.renderer->render(f.swapchain.Get(), [&](D2DPainter&) { repainted = true; }, true);
            if (f.read_red() < 0.99f) throw std::runtime_error("Graphics access made the cached overlay disappear");
            check(f.swapchain->Present(0, 0));
        }
        if (repainted) throw std::runtime_error("Busy graphics resources were accessed during cached composition");
    });
    const bool returned = drawing.wait_for(1s) == std::future_status::ready;
    release.set_value();
    resource_access.get();
    drawing.get();
    if (!returned) throw std::runtime_error("Cached composition waited for the Lua graphics access lock");
    const bool repainted = f.renderer->render(f.swapchain.Get(), [](D2DPainter& p) {
        p.fill_rect(0, 0, 2, 2, 0xFF808080);
    }, true);
    if (!repainted || std::abs(f.read_red() - 128.0f / 255.0f) > 0.002f)
        throw std::runtime_error("Deferred repaint did not resume after graphics access completed");
}
