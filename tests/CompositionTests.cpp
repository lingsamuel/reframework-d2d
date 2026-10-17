#include "D3D12TestFixture.hpp"
#include <algorithm>
#include <cmath>
#include <d3d12sdklayers.h>

// Inspect real GPU allocations without adding a runtime diagnostic API.
struct D3D12RendererTestAccess {
    static size_t scene_copy_count(const D3D12Renderer& renderer) {
        return renderer.m_scene_copy ? 1 : 0;
    }
    static ID3D12Resource* scene_copy(const D3D12Renderer& renderer) { return renderer.m_scene_copy.Get(); }

    // Exercise the production composition stage independently of the physical monitor's HDR capability.
    // The HUD is white, so its SDR and linear encodings are identical before premultiplication.
    static void record(D3D12Renderer& renderer, ID3D12GraphicsCommandList* list, ID3D12Resource* target,
        D3D12Renderer::OutputMode mode, bool hdr_display = false) {
        renderer.m_output_mode = mode;
        renderer.m_hdr_display = hdr_display;
        renderer.m_composite_paper_white_nits = renderer.m_paper_white_nits;
        renderer.record_composition(list, target, renderer.m_render_resources.front()->vert_buffer.Get());
    }
};

namespace {
void expect_pixel(const std::array<float, 4>& actual, const std::array<float, 4>& expected, float tolerance) {
    for (size_t i = 0; i < 4; ++i) {
        if (!std::isfinite(actual[i]) || std::abs(actual[i] - expected[i]) > tolerance)
            throw std::runtime_error("Composition channel " + std::to_string(i) + ": expected " +
                std::to_string(expected[i]) + ", got " + std::to_string(actual[i]));
    }
}

double pq_encode(double nits) {
    const double p = std::pow(nits / 10000.0, 2610.0 / 16384.0);
    return std::pow((3424.0 / 4096.0 + (2413.0 / 128.0) * p) / (1 + (2392.0 / 128.0) * p), 2523.0 / 32.0);
}

double pq_decode(double code) {
    const double p = std::pow(code, 32.0 / 2523.0);
    return 10000.0 * std::pow((std::max)(p - 3424.0 / 4096.0, 0.0) / (2413.0 / 128.0 - (2392.0 / 128.0) * p),
        16384.0 / 2610.0);
}

void expect_no_debug_errors(ID3D12Device* device, bool initialization = false) {
    Microsoft::WRL::ComPtr<ID3D12InfoQueue> info;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&info)))) return;
    for (UINT64 i = 0; i < info->GetNumStoredMessagesAllowedByRetrievalFilter(); ++i) {
        SIZE_T size{};
        test::check_hr(info->GetMessage(i, nullptr, &size));
        std::vector<unsigned char> storage(size);
        auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
        test::check_hr(info->GetMessage(i, message, &size));
        // CreateWrappedResource probes native resources for D3D11 sharing metadata.
        // This runtime reports the failed probe even when wrapping succeeds. Exclude
        // only that constructor diagnostic; all composition errors remain failures.
        if (initialization && message->ID == D3D12_MESSAGE_ID_REFLECTSHAREDPROPERTIES_INVALIDOBJECT) continue;
        if (message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR)
            throw std::runtime_error(message->pDescription);
    }
    info->ClearStoredMessages();
}
}

void test_direct_composition_resources() {
    for (const auto format : {DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R16G16B16A16_FLOAT}) {
        test::D3D12TestFixture f(format, 3);
        expect_no_debug_errors(f.device.Get(), true);
        if (D3D12RendererTestAccess::scene_copy_count(*f.renderer) != 0)
            throw std::runtime_error("SDR/scRGB allocated full-screen scene copies");
        for (int i = 0; i < 6; ++i) {
            f.clear_scene();
            f.renderer->render(f.swapchain.Get(), [](D2DPainter& p) { p.fill_rect(0, 0, 2, 2, 0x80FFFFFF); }, i == 0);
            if (f.read_red() < 0.49f) throw std::runtime_error("Direct composition lost the cached overlay");
            test::check_hr(f.swapchain->Present(0, 0));
        }
        if (D3D12RendererTestAccess::scene_copy_count(*f.renderer) != 0)
            throw std::runtime_error("Direct composition created a scene copy while drawing");
        expect_no_debug_errors(f.device.Get());
    }
}

void test_direct_composition_pixels() {
    for (const auto format : {DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R16G16B16A16_FLOAT}) {
        test::D3D12TestFixture f(format, 3);
        expect_no_debug_errors(f.device.Get(), true);
        const bool scrgb = format == DXGI_FORMAT_R16G16B16A16_FLOAT;
        const float alpha = 128.0f / 255.0f;
        const float scale = scrgb && f.renderer->is_hdr_output() ? 2.5f : 1.0f;
        const std::array<float, 4> scene = scrgb ? std::array<float, 4>{-0.5f, 12.5f, 0.25f, 1.0f} :
            std::array<float, 4>{0.2f, 0.6f, 0.8f, 1.0f};
        for (int i = 0; i < 6; ++i) {
            f.clear_scene(scene);
            f.renderer->render(f.swapchain.Get(), [](D2DPainter& p) { p.fill_rect(0, 0, 2, 2, 0x80FFFFFF); }, i == 0);
            auto expected = scene;
            for (size_t channel = 0; channel < 3; ++channel) expected[channel] = alpha * scale + scene[channel] * (1 - alpha);
            expect_pixel(f.read_pixel(), expected, scrgb ? 0.008f : 1.0f / 255.0f);
            test::check_hr(f.swapchain->Present(0, 0));
        }
        if (scrgb) {
            // Exercise HDR paper-white scaling even on an SDR desktop.
            Microsoft::WRL::ComPtr<ID3D12Resource> target;
            test::check_hr(f.swapchain->GetBuffer(f.swapchain->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&target)));
            for (const float nits : {80.0f, 200.0f, 1000.0f}) {
                f.renderer->set_paper_white_nits(nits);
                f.clear_scene(scene);
                D3D12CommandContext frame(f.device.Get());
                D3D12RendererTestAccess::record(*f.renderer, frame.begin().Get(), target.Get(), D3D12Renderer::OutputMode::SCRGB, true);
                frame.end(f.queue.Get());
                frame.wait(10000);
                auto expected = scene;
                for (size_t channel = 0; channel < 3; ++channel) expected[channel] = alpha * nits / 80.0f + scene[channel] * (1 - alpha);
                expect_pixel(f.read_pixel(), expected, 0.01f);
            }
        }
        // Fully transparent HUD must leave extended values and destination alpha untouched.
        f.clear_scene(scene);
        f.renderer->render(f.swapchain.Get(), [](D2DPainter&) {}, true);
        expect_pixel(f.read_pixel(), scene, scrgb ? 0.001f : 1.0f / 255.0f);
        expect_no_debug_errors(f.device.Get());
    }
}

void test_hdr10_shared_scene_in_flight() {
    using Microsoft::WRL::ComPtr;
    using Mode = D3D12Renderer::OutputMode;
    test::D3D12TestFixture f(DXGI_FORMAT_R10G10B10A2_UNORM, 3);
    expect_no_debug_errors(f.device.Get(), true);
    if (D3D12RendererTestAccess::scene_copy_count(*f.renderer) != 0)
        throw std::runtime_error("The HDR10 scene was allocated before it was needed");
    f.renderer->render(f.swapchain.Get(), [](D2DPainter& p) { p.fill_rect(0, 0, 2, 2, 0x80FFFFFF); }, true);
    f.renderer->wait_for_idle();
    ComPtr<ID3D12Resource> target;
    test::check_hr(f.swapchain->GetBuffer(f.swapchain->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&target)));
    ComPtr<ID3D12Fence> gate;
    test::check_hr(f.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate)));
    const std::array<Mode, 6> modes{Mode::SDR, Mode::HDR10_PQ, Mode::HDR10_PQ, Mode::SDR, Mode::HDR10_PQ, Mode::HDR10_PQ};
    const std::array<float, 6> scenes{0.1f, 0.0f, 0.25f, 0.3f, 0.5f, 0.7f};
    std::vector<std::unique_ptr<D3D12CommandContext>> frames;
    std::vector<std::unique_ptr<test::PixelReadback>> pixels;
    frames.reserve(modes.size());
    pixels.reserve(modes.size());
    // Declared after the frame resources: even a failed assertion must drain before freeing them.
    struct ReleaseQueue {
        ID3D12Fence* gate;
        D3D12Renderer* renderer;
        ~ReleaseQueue() {
            gate->Signal(1);
            try { renderer->wait_for_idle(); } catch (...) {}
        }
    } release_queue{gate.Get(), f.renderer.get()};
    test::check_hr(f.queue->Wait(gate.Get(), 1));
    // Preparing the HUD above may already need PQ on an HDR monitor.
    ID3D12Resource* shared = D3D12RendererTestAccess::scene_copy(*f.renderer);
    for (size_t i = 0; i < modes.size(); ++i) {
        auto frame = std::make_unique<D3D12CommandContext>(f.device.Get());
        auto readback = std::make_unique<test::PixelReadback>(f.device.Get(), target.Get());
        auto* list = frame->begin().Get();
        f.record_clear_scene(list, target.Get(), {scenes[i], scenes[i], scenes[i], 1});
        D3D12RendererTestAccess::record(*f.renderer, list, target.Get(), modes[i]);
        auto* scene = D3D12RendererTestAccess::scene_copy(*f.renderer);
        if (i == 0 && scene != shared) throw std::runtime_error("SDR created a scene texture");
        if (i == 1) shared = scene;
        if (i > 0 && (!scene || scene != shared)) throw std::runtime_error("HDR10 did not reuse one stable scene texture");
        readback->record(list, target.Get());
        frame->end(f.queue.Get());
        pixels.push_back(std::move(readback));
        frames.push_back(std::move(frame));
    }
    // The queue gate proves none of the submitted frames has completed on the GPU yet.
    for (const auto& frame : frames) {
        if (frame->completed()) throw std::runtime_error("The in-flight test unexpectedly drained the GPU");
    }
    test::check_hr(gate->Signal(1));
    for (auto& frame : frames) frame->wait(10000);
    const double alpha = 128.0 / 255.0;
    for (size_t i = 0; i < modes.size(); ++i) {
        // Account for the game's R10 storage before the scene is read by the shader.
        const double scene_code = std::round(scenes[i] * 1023.0) / 1023.0;
        const float expected = static_cast<float>(modes[i] == Mode::HDR10_PQ ?
            pq_encode(200.0 * alpha + pq_decode(scene_code) * (1 - alpha)) : alpha + scene_code * (1 - alpha));
        expect_pixel(pixels[i]->pixel(), {expected, expected, expected, 1}, 2.0f / 1023.0f);
    }
    if (D3D12RendererTestAccess::scene_copy_count(*f.renderer) != 1)
        throw std::runtime_error("HDR10 retained more than one scene copy");
    expect_no_debug_errors(f.device.Get());
}
