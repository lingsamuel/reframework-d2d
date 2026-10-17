#include <cmath>
#include <cstring>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <vector>

#include <d3dcompiler.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>

#include "D2DPainter.hpp"
#include "D3D12Shaders.hpp"
#include "ColorSpace.hpp"

using Microsoft::WRL::ComPtr;

void test_on12_composition();
void test_on12_scrgb_white();
void test_context_pending_timeout();
void test_on12_callback_exception();
void test_on12_resolution_reset();
void test_renderer_retirement();
void test_live_renderer_does_not_pin_backbuffers();
void test_plugin_lifecycle();
void test_failed_frame_signal();
void test_failed_tail_signal();
void test_font_cache_concurrency();
void test_device_removal_retirement();
void test_many_backbuffers();
void test_retired_renderer_does_not_pin_swapchain();
void test_cached_overlay_during_graphics_access();
void test_drawlist_batch_ownership();
void test_direct_composition_resources();
void test_direct_composition_pixels();
void test_hdr10_shared_scene_in_flight();

namespace {
struct Pixel { float r, g, b, a; };

void check(HRESULT hr) {
    if (FAILED(hr)) {
        throw std::runtime_error("DirectX HRESULT: " + std::to_string(static_cast<unsigned long>(hr)));
    }
}

void expect_near(float actual, float expected, float tolerance = 0.002f) {
    if (!std::isfinite(actual) || std::abs(actual - expected) > tolerance) {
        throw std::runtime_error("expected " + std::to_string(expected) + ", got " + std::to_string(actual));
    }
}

class Fixture {
public:
    Fixture() {
        check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context));
        overlay = texture(DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
        ComPtr<IDXGISurface> surface;
        check(overlay.As(&surface));
        painter = std::make_unique<D2DPainter>(device.Get(), surface.Get());
        check(device->CreateShaderResourceView(overlay.Get(), nullptr, &overlay_view));

        // The test supplies a full-screen triangle and runs the project pixel shader for composition.
        const char* vertex_source = R"(
struct Output { float4 pos : SV_POSITION; float4 color : COLOR; float2 uv : TEXCOORD; };
Output main(uint id : SV_VertexID) {
    Output o;
    o.uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    o.color = 1;
    return o;
})";
        auto vs = compile(vertex_source, "vs_5_0");
        auto ps = compile(D3D12_PIX_SHADER, "ps_5_0");
        check(device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &vertex_shader));
        check(device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &pixel_shader));

        D3D11_BUFFER_DESC constants_desc{};
        constants_desc.ByteWidth = 16;
        constants_desc.Usage = D3D11_USAGE_DEFAULT;
        constants_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        check(device->CreateBuffer(&constants_desc, nullptr, &constants));

        D3D11_SAMPLER_DESC sampler_desc{};
        sampler_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sampler_desc.AddressU = sampler_desc.AddressV = sampler_desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler_desc.MaxLOD = D3D11_FLOAT32_MAX;
        sampler_desc.MaxAnisotropy = 1;
        sampler_desc.ComparisonFunc = D3D11_COMPARISON_ALWAYS;
        check(device->CreateSamplerState(&sampler_desc, &sampler));

        D3D11_RASTERIZER_DESC raster_desc{};
        raster_desc.FillMode = D3D11_FILL_SOLID;
        raster_desc.CullMode = D3D11_CULL_NONE;
        check(device->CreateRasterizerState(&raster_desc, &rasterizer));
    }

    Pixel render(float mode, Pixel scene, float scale = 2.5f) {
        auto scene_texture = texture(DXGI_FORMAT_R32G32B32A32_FLOAT, D3D11_BIND_SHADER_RESOURCE, &scene);
        auto result_texture = texture(DXGI_FORMAT_R32G32B32A32_FLOAT, D3D11_BIND_RENDER_TARGET);
        ComPtr<ID3D11ShaderResourceView> scene_view;
        ComPtr<ID3D11RenderTargetView> result_view;
        check(device->CreateShaderResourceView(scene_texture.Get(), nullptr, &scene_view));
        check(device->CreateRenderTargetView(result_texture.Get(), nullptr, &result_view));
        float values[4] = {mode, 200.0f, scale, 0.0f};
        context->UpdateSubresource(constants.Get(), 0, nullptr, values, 0, 0);
        context->VSSetShader(vertex_shader.Get(), nullptr, 0);
        context->PSSetShader(pixel_shader.Get(), nullptr, 0);
        context->PSSetConstantBuffers(1, 1, constants.GetAddressOf());
        ID3D11ShaderResourceView* views[] = {overlay_view.Get(), scene_view.Get()};
        context->PSSetShaderResources(0, 2, views);
        context->PSSetSamplers(0, 1, sampler.GetAddressOf());
        context->OMSetRenderTargets(1, result_view.GetAddressOf(), nullptr);
        context->OMSetBlendState(nullptr, nullptr, ~0u);
        context->RSSetState(rasterizer.Get());
        D3D11_VIEWPORT viewport{0, 0, 1, 1, 0, 1};
        context->RSSetViewports(1, &viewport);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->Draw(3, 0);
        ID3D11ShaderResourceView* empty[] = {nullptr, nullptr};
        context->PSSetShaderResources(0, 2, empty);
        context->OMSetRenderTargets(0, nullptr, nullptr);

        D3D11_TEXTURE2D_DESC staging_desc;
        result_texture->GetDesc(&staging_desc);
        staging_desc.BindFlags = 0;
        staging_desc.Usage = D3D11_USAGE_STAGING;
        staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> staging;
        check(device->CreateTexture2D(&staging_desc, nullptr, &staging));
        context->CopyResource(staging.Get(), result_texture.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        check(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped));
        Pixel result;
        std::memcpy(&result, mapped.pData, sizeof(result));
        context->Unmap(staging.Get(), 0);
        return result;
    }

    void white(bool opaque_background) {
        painter->begin();
        if (opaque_background) painter->fill_rect(0, 0, 1, 1, 0xFF000000);
        painter->fill_rect(0, 0, 1, 1, 0x80FFFFFF);
        painter->end();
    }

    std::unique_ptr<D2DPainter> painter;

private:
    ComPtr<ID3DBlob> compile(const char* source, const char* profile) {
        ComPtr<ID3DBlob> code, errors;
        auto hr = D3DCompile(source, std::strlen(source), nullptr, nullptr, nullptr, "main", profile, 0, 0, &code, &errors);
        if (FAILED(hr) && errors) {
            throw std::runtime_error(static_cast<const char*>(errors->GetBufferPointer()));
        }
        check(hr);
        return code;
    }

    ComPtr<ID3D11Texture2D> texture(DXGI_FORMAT format, UINT bind, const Pixel* pixel = nullptr) {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = desc.Height = desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
        desc.Format = format;
        desc.BindFlags = bind;
        D3D11_SUBRESOURCE_DATA data{};
        data.pSysMem = pixel;
        data.SysMemPitch = sizeof(Pixel);
        ComPtr<ID3D11Texture2D> result;
        check(device->CreateTexture2D(&desc, pixel ? &data : nullptr, &result));
        return result;
    }

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11Texture2D> overlay;
    ComPtr<ID3D11ShaderResourceView> overlay_view;
    ComPtr<ID3D11VertexShader> vertex_shader;
    ComPtr<ID3D11PixelShader> pixel_shader;
    ComPtr<ID3D11Buffer> constants;
    ComPtr<ID3D11SamplerState> sampler;
    ComPtr<ID3D11RasterizerState> rasterizer;
};

class TestImage {
public:
    TestImage(D2DPainter& painter, unsigned int color) : TestImage(painter, std::vector<unsigned int>{color}) {}

    TestImage(D2DPainter& painter, std::vector<unsigned int> pixels) {
        wchar_t executable[MAX_PATH]{};
        if (GetModuleFileNameW(nullptr, executable, MAX_PATH) == 0) {
            throw std::runtime_error("Could not locate test output directory");
        }
        static unsigned int counter = 0;
        path = std::filesystem::path(executable).parent_path() /
            ("hdr-image-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(++counter) + ".png");
        auto wic = painter.wic();
        ComPtr<IWICStream> stream;
        ComPtr<IWICBitmapEncoder> encoder;
        ComPtr<IWICBitmapFrameEncode> frame;
        check(wic->CreateStream(&stream));
        check(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE));
        check(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder));
        check(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache));
        check(encoder->CreateNewFrame(&frame, nullptr));
        check(frame->Initialize(nullptr));
        const auto width = static_cast<UINT>(pixels.size());
        check(frame->SetSize(width, 1));
        auto format = GUID_WICPixelFormat32bppBGRA;
        check(frame->SetPixelFormat(&format));
        if (format != GUID_WICPixelFormat32bppBGRA) throw std::runtime_error("PNG encoder changed pixel format");
        check(frame->WritePixels(1, width * 4, width * 4, reinterpret_cast<BYTE*>(pixels.data())));
        check(frame->Commit());
        check(encoder->Commit());
        frame.Reset(); encoder.Reset(); stream.Reset();
        image = std::make_shared<D2DImage>(wic, painter.context(), path);
    }

    ~TestImage() {
        image.reset();
        std::error_code error;
        std::filesystem::remove(path, error);
    }

    std::shared_ptr<D2DImage> image;
private:
    std::filesystem::path path;
};
}

int main(int argc, char** argv) {
    ComPtr<ID3D12Debug> debug;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) debug->EnableDebugLayer();
    check(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
    int failures = 0;
    const std::vector<std::pair<const char*, std::function<void()>>> tests{
        {"HDR white blends identically inside D2D and over the scene", [] {
            Fixture f;
            f.painter->set_linear_blending(true);
            f.white(false);
            auto direct = f.render(1, {0, 0, 0, 1});
            f.white(true);
            auto layered = f.render(1, {0, 0, 0, 1});
            expect_near(layered.r, direct.r, 0.0005f);
        }},
        {"SDR keeps legacy encoded alpha blending", [] {
            Fixture f;
            f.white(false);
            expect_near(f.render(0, {0, 0, 0, 1}).r, 128.0f / 255.0f);
        }},
        {"SDR scRGB does not boost reference white", [] {
            Fixture f;
            f.painter->begin();
            f.painter->fill_rect(0, 0, 1, 1, 0xFFFFFFFF);
            f.painter->end();
            expect_near(f.render(2, {0, 0, 0, 1}, ColorSpace::scrgb_white_scale(200, false)).r, 1);
        }},
        {"HDR scRGB uses absolute paper white", [] {
            Fixture f;
            f.painter->set_linear_blending(true);
            f.painter->begin();
            f.painter->fill_rect(0, 0, 1, 1, 0xFFFFFFFF);
            f.painter->end();
            expect_near(f.render(2, {0, 0, 0, 1}, ColorSpace::scrgb_white_scale(200, true)).r, 2.5f);
        }},
        {"HDR gray is decoded before blending", [] {
            Fixture f;
            f.painter->set_linear_blending(true);
            f.painter->begin();
            f.painter->fill_rect(0, 0, 1, 1, 0xFF808080);
            f.painter->end();
            expect_near(f.render(2, {0, 0, 0, 1}, 1).r, 0.2158605f, 0.0005f);
        }},
        {"Switching HDR to SDR and back updates brush colors", [] {
            Fixture f;
            if (!f.painter->set_linear_blending(true) || f.painter->set_linear_blending(true)) {
                throw std::runtime_error("Encoding change was not reported");
            }
            f.painter->begin();
            f.painter->fill_rect(0, 0, 1, 1, 0xFF808080);
            f.painter->end();
            expect_near(f.render(2, {0, 0, 0, 1}, 1).r, 0.2158605f, 0.0005f);
            if (!f.painter->set_linear_blending(false)) throw std::runtime_error("SDR change was not reported");
            f.painter->begin();
            f.painter->fill_rect(0, 0, 1, 1, 0xFF808080);
            f.painter->end();
            expect_near(f.render(0, {0, 0, 0, 1}).r, 128.0f / 255.0f);
            if (!f.painter->set_linear_blending(true)) throw std::runtime_error("HDR change was not reported");
            f.painter->begin();
            f.painter->fill_rect(0, 0, 1, 1, 0xFF808080);
            f.painter->end();
            expect_near(f.render(2, {0, 0, 0, 1}, 1).r, 0.2158605f, 0.0005f);
        }},
        {"HDR scaled image matches linear gray brush with opacity", [] {
            Fixture f;
            f.painter->set_linear_blending(true);
            TestImage image(*f.painter, 0xFF808080);
            f.painter->begin();
            f.painter->image(image.image, 0, 0, 1, 1, 0.5f);
            f.painter->end();
            expect_near(f.render(2, {0, 0, 0, 1}, 1).r, 0.2158605f * 0.5f, 0.0005f);
        }},
        {"HDR image preserves intrinsic alpha and draw opacity", [] {
            Fixture f;
            f.painter->set_linear_blending(true);
            TestImage image(*f.painter, 0x80FFFFFF);
            f.painter->begin();
            f.painter->image(image.image, 0, 0, 0.5f);
            f.painter->end();
            auto pixel = f.render(2, {0, 0, 0, 0}, 1);
            expect_near(pixel.r, 128.0f / 255.0f * 0.5f, 0.0005f);
            expect_near(pixel.a, pixel.r, 0.0005f);
        }},
        {"HDR image is linearized before downsampling", [] {
            Fixture f;
            f.painter->set_linear_blending(true);
            TestImage image(*f.painter, std::vector<unsigned int>{0xFF000000, 0xFFFFFFFF});
            f.painter->begin();
            f.painter->image(image.image, 0, 0, 1, 1, 1);
            f.painter->end();
            expect_near(f.render(2, {0, 0, 0, 1}, 1).r, 0.5f, 0.0005f);
        }},
        {"HDR gray image correctly decodes premultiplied input", [] {
            Fixture f;
            f.painter->set_linear_blending(true);
            TestImage image(*f.painter, 0x80808080);
            f.painter->begin();
            f.painter->image(image.image, 0, 0, 1);
            f.painter->end();
            // WIC rounds the 8-bit premultiplied gray channel to 64; unpremultiply to 64/128 before decoding.
            expect_near(f.render(2, {0, 0, 0, 1}, 1).r, 0.2140411f * (128.0f / 255.0f), 0.001f);
        }},
        {"SDR image keeps encoded gray", [] {
            Fixture f;
            TestImage image(*f.painter, 0xFF808080);
            f.painter->begin();
            f.painter->image(image.image, 0, 0, 0.5f);
            f.painter->end();
            expect_near(f.render(0, {0, 0, 0, 1}).r, 128.0f / 255.0f * 0.5f);
        }},
        {"Image loading during drawing preserves the active batch", [] {
            Fixture f;
            f.painter->begin();
            TestImage image(*f.painter, 0xFFFFFFFF);
            f.painter->fill_rect(0, 0, 1, 1, 0xFFFFFFFF);
            f.painter->end();
            expect_near(f.render(0, {0, 0, 0, 1}).r, 1.0f);
        }},
        {"Images survive replacement of the D2D device", [] {
            Fixture first, second;
            TestImage image(*first.painter, 0xFFFFFFFF);
            second.painter->begin();
            second.painter->image(image.image, 0, 0, 1);
            second.painter->end();
            expect_near(second.render(0, {0, 0, 0, 1}).r, 1.0f);
        }},
        {"Image conversion restores the previous D2D target", [] {
            Fixture f;
            f.painter->begin(); f.painter->end();
            ComPtr<ID2D1Image> before, after;
            f.painter->context()->GetTarget(&before);
            TestImage image(*f.painter, 0xFFFFFFFF);
            f.painter->context()->GetTarget(&after);
            if (before.Get() != after.Get()) throw std::runtime_error("D2D target was replaced by image conversion");
        }},
        {"Transparent overlay preserves the scene exactly", [] {
            Fixture f;
            f.painter->begin(); f.painter->end();
            const Pixel scene{0.1234567f, 0.75f, 0.2345678f, 0.5f};
            for (float mode : {0.0f, 1.0f, 2.0f}) {
                auto result = f.render(mode, scene);
                if (std::memcmp(&scene, &result, sizeof(scene)) != 0) throw std::runtime_error("Scene pixel changed");
            }
        }},
        {"scRGB blending preserves extended scene values", [] {
            Fixture f;
            f.painter->set_linear_blending(true);
            f.white(false);
            auto result = f.render(2, {-0.5f, 12.5f, 0.0f, 1}, 1);
            const float alpha = 128.0f / 255.0f;
            expect_near(result.r, alpha - 0.5f * (1 - alpha));
            expect_near(result.g, alpha + 12.5f * (1 - alpha), 0.004f);
        }},
        {"Project vertex shader compiles as Shader Model 5", [] {
            ComPtr<ID3DBlob> code, errors;
            check(D3DCompile(D3D12_VERT_SHADER, std::strlen(D3D12_VERT_SHADER), nullptr, nullptr, nullptr,
                "main", "vs_5_0", 0, 0, &code, &errors));
        }},
        {"Retirement waits for CPU leases and the GPU tail", test_renderer_retirement},
        {"Live renderer does not pin backbuffers", test_live_renderer_does_not_pin_backbuffers},
        {"Plugin Lua lifecycle and concurrent reset", test_plugin_lifecycle},
        {"Failed frame Signal prevents allocator reuse", test_failed_frame_signal},
        {"Failed tail Signal retries retirement", test_failed_tail_signal},
        {"Font cache concurrent measurement and layout", test_font_cache_concurrency},
        {"Device removal retires resources without Reset", test_device_removal_retirement},
        {"Six backbuffers have no fixed-array limit", test_many_backbuffers},
        {"Retired renderer does not pin swapchain", test_retired_renderer_does_not_pin_swapchain},
        {"Cached overlay survives busy graphics access", test_cached_overlay_during_graphics_access},
        {"Command batch ownership preserves storage and text", test_drawlist_batch_ownership},
        {"Direct composition allocates no scene copies", test_direct_composition_resources},
        {"Direct composition preserves SDR and extended scRGB pixels", test_direct_composition_pixels},
        {"HDR10 reuses one scene with several GPU frames in flight", test_hdr10_shared_scene_in_flight},
        {"GPU pending timeout", test_context_pending_timeout},
        {"D3D11On12 callback exception", test_on12_callback_exception},
        {"D3D11On12 resolution reset", test_on12_resolution_reset},
        {"D3D11On12 FP16 overlay and cached SDR composition", test_on12_composition},
        {"D3D11On12 scRGB paper white follows display HDR state", test_on12_scrgb_white},
    };
    int matched = 0;
    for (const auto& [name, run] : tests) {
        if (argc > 1 && std::string(name).find(argv[1]) == std::string::npos) continue;
        ++matched;
        try { run(); std::cout << "PASS: " << name << '\n'; }
        catch (const std::exception& e) { ++failures; std::cerr << "FAIL: " << name << ": " << e.what() << '\n'; }
    }
    if (!matched) { ++failures; std::cerr << "FAIL: no test matched the filter\n"; }
    CoUninitialize();
    return failures == 0 ? 0 : 1;
}
