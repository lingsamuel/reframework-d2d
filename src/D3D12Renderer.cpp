#include <stdexcept>
#include <algorithm>

#include <d3d11on12.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>

#include "D3D12Shaders.hpp"

#include "D3D12Renderer.hpp"

D3D12Renderer::D3D12Renderer(IDXGISwapChain* swapchain_, ID3D12Device* device_, ID3D12CommandQueue* cmd_queue_)
    : m_swapchain{(IDXGISwapChain3*)swapchain_}
    , m_device{device_}
    , m_cmd_queue{cmd_queue_} {

    if (FAILED(D3D11On12CreateDevice(m_device.Get(), D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, (IUnknown**)m_cmd_queue.GetAddressOf(),
            1, 0, &m_d3d11_device, &m_d3d11_context, nullptr))) {
        throw std::runtime_error{"Failed to create D3D11On12 device"};
    }

    if (FAILED(m_d3d11_device.As(&m_d3d11on12_device))) {
        throw std::runtime_error{"Failed to query D3D11On12 device"};
    }

    // Create RTV descriptor heap
    D3D12_DESCRIPTOR_HEAP_DESC rtv_desc = {};
    rtv_desc.NumDescriptors = (int)RTV::COUNT;
    rtv_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtv_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;

    if (FAILED(m_device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&m_rtv_heap)))) {
        throw std::runtime_error{"Failed to create RTV descriptor heap"};
    }

    DXGI_SWAP_CHAIN_DESC swapchain_desc{};

    if (FAILED(m_swapchain->GetDesc(&swapchain_desc))) {
        throw std::runtime_error{"Failed to get swapchain description"};
    }

    // Reserve two SRVs per buffered frame: overlay(t0) + scene copy(t1).
    D3D12_DESCRIPTOR_HEAP_DESC srv_desc = {};
    srv_desc.NumDescriptors = swapchain_desc.BufferCount * SRV_SLOTS_PER_FRAME;
    srv_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srv_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

    if (FAILED(m_device->CreateDescriptorHeap(&srv_desc, IID_PPV_ARGS(&m_srv_heap)))) {
        throw std::runtime_error{"Failed to create SRV descriptor heap"};
    }

    for (int i = 0; i < swapchain_desc.BufferCount; i++) {
        // Create a command context for each back buffer.
        // We create one for each because the GPU could be doing work on one while we're recording commands on another,
        // before we submit them to the command queue. If we did not do this, we would run into some race conditions.
        auto cmd_context = std::make_unique<D3D12CommandContext>(m_device.Get());

        if (cmd_context->is_setup()) {
            m_cmd_contexts.push_back(std::move(cmd_context));
        } else {
            throw std::runtime_error{"Failed to create command context"};
        }

        if (SUCCEEDED(m_swapchain->GetBuffer((UINT)i, IID_PPV_ARGS(&m_rts[i])))) {
            m_device->CreateRenderTargetView(m_rts[i].Get(), nullptr, get_cpu_rtv((RTV)i));
        }
    }

    m_frames_in_flight = m_cmd_contexts.size();

    // Create D2D render target
    auto& backbuffer = get_rt(RTV::BACKBUFFER_0);
    auto backbuffer_desc = backbuffer->GetDesc();
    refresh_output_mode();

    m_width = backbuffer_desc.Width;
    m_height = backbuffer_desc.Height;

    D3D12_RESOURCE_DESC d2d_desc{};
    d2d_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d2d_desc.Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
    d2d_desc.Width = backbuffer_desc.Width;
    d2d_desc.Height = backbuffer_desc.Height;
    d2d_desc.DepthOrArraySize = 1;
    d2d_desc.MipLevels = 1;
    d2d_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d2d_desc.SampleDesc.Count = 1;
    d2d_desc.SampleDesc.Quality = 0;
    d2d_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    D3D12_HEAP_PROPERTIES d2d_heap_props = {};
    d2d_heap_props.Type = D3D12_HEAP_TYPE_DEFAULT;
    d2d_heap_props.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    d2d_heap_props.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;

    D3D12_CLEAR_VALUE clear_value{};
    clear_value.Format = d2d_desc.Format;

    if (FAILED(m_device->CreateCommittedResource(&d2d_heap_props, D3D12_HEAP_FLAG_NONE, &d2d_desc,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &clear_value, IID_PPV_ARGS(&m_rts[(int)RTV::D2D])))) {
        throw std::runtime_error{"Failed to create D2D render target"};
    }

    m_device->CreateRenderTargetView(m_rts[(int)RTV::D2D].Get(), nullptr, get_cpu_rtv(RTV::D2D));

    D3D11_RESOURCE_FLAGS res_flags{D3D11_BIND_RENDER_TARGET};
    if (FAILED(m_d3d11on12_device->CreateWrappedResource(get_rt(RTV::D2D).Get(), &res_flags, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, IID_PPV_ARGS(&m_wrapped_rt)))) {
        throw std::runtime_error{"Failed to create wrapped render target"};
    }

    ComPtr<IDXGISurface> dxgi_surface{};

    if (FAILED(m_wrapped_rt.As(&dxgi_surface))) {
        throw std::runtime_error{"Failed to query DXGI surface"};
    }

    m_d2d = std::make_unique<D2DPainter>(m_d3d11_device.Get(), dxgi_surface.Get());

    // Create root signature.
    D3D12_DESCRIPTOR_RANGE desc_range{};
    desc_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    desc_range.NumDescriptors = 2;
    desc_range.BaseShaderRegister = 0;
    desc_range.RegisterSpace = 0;
    desc_range.OffsetInDescriptorsFromTableStart = 0;

    D3D12_ROOT_PARAMETER root_params[3]{};
    root_params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    root_params[0].Constants.ShaderRegister = 0;
    root_params[0].Constants.RegisterSpace = 0;
    root_params[0].Constants.Num32BitValues = 16;
    root_params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;

    root_params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    root_params[1].Constants.ShaderRegister = 1;
    root_params[1].Constants.RegisterSpace = 0;
    root_params[1].Constants.Num32BitValues = 4;
    root_params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    root_params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    root_params[2].DescriptorTable.NumDescriptorRanges = 1;
    root_params[2].DescriptorTable.pDescriptorRanges = &desc_range;
    root_params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC sampler_desc{};
    sampler_desc.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler_desc.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler_desc.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler_desc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler_desc.MipLODBias = 0.0f;
    sampler_desc.MaxAnisotropy = 0;
    sampler_desc.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    sampler_desc.BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
    sampler_desc.MinLOD = 0.0f;
    sampler_desc.MaxLOD = 0.0f;
    sampler_desc.ShaderRegister = 0;
    sampler_desc.RegisterSpace = 0;

    D3D12_ROOT_SIGNATURE_DESC sig_desc{};
    sig_desc.NumParameters = 3;
    sig_desc.pParameters = root_params;
    sig_desc.NumStaticSamplers = 1;
    sig_desc.pStaticSamplers = &sampler_desc;
    sig_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT | D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
                     D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS | D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;

    ComPtr<ID3DBlob> blob{};
    if (FAILED(D3D12SerializeRootSignature(&sig_desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, nullptr))) {
        throw std::runtime_error{"Failed to serialize root signature"};
    }

    if (FAILED(m_device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&m_root_signature)))) {
        throw std::runtime_error{"Failed to create root signature"};
    }

    // Create pipeline state object.
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso_desc{};
    pso_desc.NodeMask = 1;
    pso_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso_desc.pRootSignature = m_root_signature.Get();
    pso_desc.SampleMask = UINT_MAX;
    pso_desc.NumRenderTargets = 1;
    pso_desc.RTVFormats[0] = backbuffer_desc.Format;
    pso_desc.SampleDesc.Count = 1;
    pso_desc.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;

    ComPtr<ID3DBlob> vertshader_blob{};
    ComPtr<ID3DBlob> pixshader_blob{};

    if (FAILED(D3DCompile(
            D3D12_VERT_SHADER, strlen(D3D12_VERT_SHADER), nullptr, nullptr, nullptr, "main", "vs_5_0", 0, 0, &vertshader_blob, nullptr))) {
        throw std::runtime_error{"Failed to compile vertex shader"};
    }

    if (FAILED(D3DCompile(
            D3D12_PIX_SHADER, strlen(D3D12_PIX_SHADER), nullptr, nullptr, nullptr, "main", "ps_5_0", 0, 0, &pixshader_blob, nullptr))) {
        throw std::runtime_error{"Failed to compile pixel shader"};
    }

    pso_desc.VS = {vertshader_blob->GetBufferPointer(), vertshader_blob->GetBufferSize()};
    pso_desc.PS = {pixshader_blob->GetBufferPointer(), pixshader_blob->GetBufferSize()};

    static D3D12_INPUT_ELEMENT_DESC input_layout[]{
        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 16, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    };

    pso_desc.InputLayout = {input_layout, 3};

    auto& blend = pso_desc.BlendState;
    blend.AlphaToCoverageEnable = false;
    blend.IndependentBlendEnable = false;
    blend.RenderTarget[0].BlendEnable = false;
    blend.RenderTarget[0].LogicOpEnable = false;
    blend.RenderTarget[0].SrcBlend = D3D12_BLEND_ONE;
    blend.RenderTarget[0].DestBlend = D3D12_BLEND_ZERO;
    blend.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
    blend.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
    blend.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ZERO;
    blend.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
    blend.RenderTarget[0].LogicOp = D3D12_LOGIC_OP_NOOP;
    blend.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    auto& raster = pso_desc.RasterizerState;
    raster.FillMode = D3D12_FILL_MODE_SOLID;
    raster.CullMode = D3D12_CULL_MODE_NONE;
    raster.FrontCounterClockwise = false;
    raster.DepthBias = D3D12_DEFAULT_DEPTH_BIAS;
    raster.DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
    raster.SlopeScaledDepthBias = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
    raster.DepthClipEnable = true;
    raster.MultisampleEnable = false;
    raster.AntialiasedLineEnable = false;
    raster.ForcedSampleCount = 0;
    raster.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;

    auto& depth = pso_desc.DepthStencilState;
    depth.DepthEnable = false;
    depth.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    depth.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    depth.StencilEnable = false;
    depth.FrontFace.StencilFailOp = depth.FrontFace.StencilDepthFailOp = depth.FrontFace.StencilPassOp = D3D12_STENCIL_OP_KEEP;
    depth.FrontFace.StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    depth.BackFace = depth.FrontFace;

    if (FAILED(m_device->CreateGraphicsPipelineState(&pso_desc, IID_PPV_ARGS(&m_pipeline_state)))) {
        throw std::runtime_error{"Failed to create pipeline state"};
    }

    D3D12_RESOURCE_DESC scene_desc{};
    scene_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    scene_desc.Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
    scene_desc.Width = backbuffer_desc.Width;
    scene_desc.Height = backbuffer_desc.Height;
    scene_desc.DepthOrArraySize = 1;
    scene_desc.MipLevels = 1;
    scene_desc.Format = backbuffer_desc.Format;
    scene_desc.SampleDesc.Count = 1;
    scene_desc.SampleDesc.Quality = 0;
    scene_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    scene_desc.Flags = D3D12_RESOURCE_FLAG_NONE;

    D3D12_SHADER_RESOURCE_VIEW_DESC d2d_srv_desc{};
    d2d_srv_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d2d_srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    d2d_srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    d2d_srv_desc.Texture2D.MipLevels = 1;

    D3D12_SHADER_RESOURCE_VIEW_DESC scene_srv_desc{};
    scene_srv_desc.Format = backbuffer_desc.Format;
    scene_srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    scene_srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    scene_srv_desc.Texture2D.MipLevels = 1;

    for (uint32_t i = 0; i < m_frames_in_flight; i++) {
        auto resources = std::make_unique<RenderResources>();
        auto& vert_buffer = resources->vert_buffer;

        D3D12_HEAP_PROPERTIES vertbuf_heap_props{};
        vertbuf_heap_props.Type = D3D12_HEAP_TYPE_UPLOAD;
        vertbuf_heap_props.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
        vertbuf_heap_props.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;

        D3D12_RESOURCE_DESC vertbuf_desc{};
        vertbuf_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        vertbuf_desc.Width = sizeof(Vert) * 6;
        vertbuf_desc.Height = 1;
        vertbuf_desc.DepthOrArraySize = 1;
        vertbuf_desc.MipLevels = 1;
        vertbuf_desc.Format = DXGI_FORMAT_UNKNOWN;
        vertbuf_desc.SampleDesc.Count = 1;
        vertbuf_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        vertbuf_desc.Flags = D3D12_RESOURCE_FLAG_NONE;

        if (FAILED(m_device->CreateCommittedResource(&vertbuf_heap_props, D3D12_HEAP_FLAG_NONE, &vertbuf_desc,
                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&vert_buffer)))) {
            throw std::runtime_error{"Failed to create vertex buffer"};
        }

        // Upload the verticies.
        D3D12_RANGE range{};
        Vert* verts{};

        if (FAILED(vert_buffer->Map(0, &range, (void**)&verts))) {
            throw std::runtime_error{"Failed to map vertex buffer"};
        }

        auto w = (float)m_width;
        auto h = (float)m_height;

        // First triangle (top-left of screen).
        verts[0] = {0.0f, 0.0f, 0.0f, 0.0f, 0xFFFFFFFF};
        verts[1] = {w, 0.0f, 1.0f, 0.0f, 0xFFFFFFFF};
        verts[2] = {0.0f, h, 0.0f, 1.0f, 0xFFFFFFFF};

        // Second triangle (bottom-right of screen).
        verts[3] = {w, 0.0f, 1.0f, 0.0f, 0xFFFFFFFF};
        verts[4] = {w, h, 1.0f, 1.0f, 0xFFFFFFFF};
        verts[5] = {0.0f, h, 0.0f, 1.0f, 0xFFFFFFFF};

        vert_buffer->Unmap(0, &range);

        if (FAILED(m_device->CreateCommittedResource(&d2d_heap_props, D3D12_HEAP_FLAG_NONE, &scene_desc,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&resources->scene_copy)))) {
            throw std::runtime_error{"Failed to create scene copy render target"};
        }

        // Write this frame's two SRV descriptors contiguously in the global heap.
        m_device->CreateShaderResourceView(
            m_rts[(int)RTV::D2D].Get(), &d2d_srv_desc, get_cpu_srv(get_srv_index(i, SRV_D2D_SLOT)));
        m_device->CreateShaderResourceView(
            resources->scene_copy.Get(), &scene_srv_desc, get_cpu_srv(get_srv_index(i, SRV_SCENE_SLOT)));
        m_render_resources.push_back(std::move(resources));
    }
}

void D3D12Renderer::render(std::function<void(D2DPainter&)> draw_fn, bool update_d2d) {
    refresh_output_mode();
    auto frame_index = m_swapchain->GetCurrentBackBufferIndex() % m_cmd_contexts.size();
    auto& cmd_context = m_cmd_contexts[frame_index];
    auto& resources = m_render_resources[frame_index];
    auto& cmd_list = cmd_context->begin();
    auto& vert_buffer = resources->vert_buffer;
    auto& scene_copy = resources->scene_copy;

    if (update_d2d) {
        m_d3d11on12_device->AcquireWrappedResources(m_wrapped_rt.GetAddressOf(), 1);
        m_d2d->begin();
        draw_fn(*m_d2d);
        m_d2d->end();
        m_d3d11on12_device->ReleaseWrappedResources(m_wrapped_rt.GetAddressOf(), 1);
        m_d3d11_context->Flush();
    }

    auto L = 0.0f;
    auto R = (float)m_width;
    auto T = 0.0f;
    auto B = (float)m_height;
    float mvp[4][4]{
        {2.0f / (R - L), 0.0f, 0.0f, 0.0f},
        {0.0f, 2.0f / (T - B), 0.0f, 0.0f},
        {0.0f, 0.0f, 0.5f, 0.0f},
        {(R + L) / (L - R), (T + B) / (B - T), 0.5f, 1.0f},
    };

    D3D12_VIEWPORT vp{};
    vp.Width = m_width;
    vp.Height = m_height;
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    vp.TopLeftX = vp.TopLeftY = 0;
    cmd_list->RSSetViewports(1, &vp);

    D3D12_RECT sr{};
    sr.left = 0;
    sr.top = 0;
    sr.right = m_width;
    sr.bottom = m_height;
    cmd_list->RSSetScissorRects(1, &sr);

    D3D12_VERTEX_BUFFER_VIEW vbv{};
    vbv.BufferLocation = vert_buffer->GetGPUVirtualAddress();
    vbv.SizeInBytes = sizeof(Vert) * 6;
    vbv.StrideInBytes = sizeof(Vert);
    cmd_list->IASetVertexBuffers(0, 1, &vbv);
    cmd_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmd_list->SetPipelineState(m_pipeline_state.Get());
    cmd_list->SetGraphicsRootSignature(m_root_signature.Get());
    cmd_list->SetGraphicsRoot32BitConstants(0, 16, mvp, 0);
    float pix_consts[4] = {static_cast<float>(static_cast<uint32_t>(m_output_mode)), m_paper_white_nits, 0.0f, 0.0f};
    cmd_list->SetGraphicsRoot32BitConstants(1, 4, pix_consts, 0);

    // Copy the scene backbuffer, then run color-managed composition in shader.
    auto bb_index = m_swapchain->GetCurrentBackBufferIndex();
    D3D12_RESOURCE_BARRIER pre_copy_barriers[2]{};
    pre_copy_barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    pre_copy_barriers[0].Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    pre_copy_barriers[0].Transition.pResource = m_rts[bb_index].Get();
    pre_copy_barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    pre_copy_barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    pre_copy_barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    pre_copy_barriers[1].Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    pre_copy_barriers[1].Transition.pResource = scene_copy.Get();
    pre_copy_barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    pre_copy_barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    cmd_list->ResourceBarrier(2, pre_copy_barriers);

    cmd_list->CopyResource(scene_copy.Get(), m_rts[bb_index].Get());

    D3D12_RESOURCE_BARRIER post_copy_barriers[2]{};
    post_copy_barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    post_copy_barriers[0].Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    post_copy_barriers[0].Transition.pResource = m_rts[bb_index].Get();
    post_copy_barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    post_copy_barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    post_copy_barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    post_copy_barriers[1].Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    post_copy_barriers[1].Transition.pResource = scene_copy.Get();
    post_copy_barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    post_copy_barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    cmd_list->ResourceBarrier(2, post_copy_barriers);

    D3D12_CPU_DESCRIPTOR_HANDLE rts[1]{};
    rts[0] = get_cpu_rtv((RTV)bb_index);
    cmd_list->OMSetRenderTargets(1, rts, FALSE, NULL);
    cmd_list->SetDescriptorHeaps(1, m_srv_heap.GetAddressOf());

    // Bind this frame's SRV pair (t0 = D2D overlay, t1 = scene copy) and composite.
    cmd_list->SetGraphicsRootDescriptorTable(2, get_gpu_srv(get_srv_index(frame_index, SRV_D2D_SLOT)));
    cmd_list->DrawInstanced(6, 1, 0, 0);

    D3D12_RESOURCE_BARRIER present_barrier{};
    present_barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    present_barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    present_barrier.Transition.pResource = m_rts[bb_index].Get();
    present_barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    present_barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    cmd_list->ResourceBarrier(1, &present_barrier);

    // end(...) calls Close() on the command list.
    cmd_context->end(m_cmd_queue.Get());
}

void D3D12Renderer::set_paper_white_nits(float nits) {
    m_paper_white_nits = std::clamp(nits, 80.0f, 1000.0f);
}

void D3D12Renderer::refresh_output_mode() {
    auto bb_index = m_swapchain->GetCurrentBackBufferIndex();
    const auto format = m_rts[bb_index]->GetDesc().Format;

    if (format == DXGI_FORMAT_R16G16B16A16_FLOAT) {
        m_output_mode = OutputMode::SCRGB;
        return;
    }

    if (format != DXGI_FORMAT_R10G10B10A2_UNORM) {
        m_output_mode = OutputMode::SDR;
        return;
    }

    bool output_is_hdr = false;
    ComPtr<IDXGIOutput> output{};
    if (SUCCEEDED(m_swapchain->GetContainingOutput(&output)) && output != nullptr) {
        ComPtr<IDXGIOutput6> output6{};
        if (SUCCEEDED(output.As(&output6)) && output6 != nullptr) {
            DXGI_OUTPUT_DESC1 output_desc{};
            if (SUCCEEDED(output6->GetDesc1(&output_desc))) {
                switch (output_desc.ColorSpace) {
                case DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020:
                case DXGI_COLOR_SPACE_RGB_STUDIO_G2084_NONE_P2020:
                    output_is_hdr = true;
                    break;
                default:
                    break;
                }
            }
        }
    }

    // require HDR output and PQ present support before choosing HDR10.
    UINT pq_support = 0;
    bool can_present_pq = false;

    if (SUCCEEDED(m_swapchain->CheckColorSpaceSupport(DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020, &pq_support))) {
        can_present_pq = (pq_support & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT) != 0;
    }

    if (!can_present_pq && SUCCEEDED(m_swapchain->CheckColorSpaceSupport(DXGI_COLOR_SPACE_RGB_STUDIO_G2084_NONE_P2020, &pq_support))) {
        can_present_pq = (pq_support & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT) != 0;
    }

    m_output_mode = (output_is_hdr && can_present_pq) ? OutputMode::HDR10_PQ : OutputMode::SDR;
}
