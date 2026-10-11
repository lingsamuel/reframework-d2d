#pragma once

inline constexpr const char* D3D12_VERT_SHADER = R"(
cbuffer vert_buffer : register(b0) {
    float4x4 mvp;
};

struct VS_INPUT {
    float2 pos : POSITION;
    float4 color : COLOR;
    float2 uv : TEXCOORD;
};

struct PS_INPUT {
    float4 pos : SV_POSITION;
    float4 color : COLOR;
    float2 uv : TEXCOORD;
};

PS_INPUT main(VS_INPUT input) {
    PS_INPUT output;
    output.pos = mul(mvp, float4(input.pos.xy, 0.0f, 1.0f));
    output.color = input.color;
    output.uv = input.uv;
    return output;
}
)";

inline constexpr const char* D3D12_PIX_SHADER = R"(
struct PS_INPUT {
    float4 pos : SV_POSITION;
    float4 color : COLOR;
    float2 uv : TEXCOORD;
};

cbuffer pix_buffer : register(b1) {
    float output_mode;
    float paper_white_nits;
    float sc_rgb_scale;
    float _pad;
};

SamplerState sampler0 : register(s0);
Texture2D overlay_tex : register(t0);
Texture2D scene_tex : register(t1);

float3 rec709_to_rec2020(float3 c) {
    return float3(
        0.6274f * c.r + 0.3293f * c.g + 0.0433f * c.b,
        0.0691f * c.r + 0.9195f * c.g + 0.0114f * c.b,
        0.0164f * c.r + 0.0880f * c.g + 0.8956f * c.b
    );
}

float3 linear_nits_to_pq(float3 nits) {
    static const float m1 = 2610.0f / 16384.0f;
    static const float m2 = 2523.0f / 32.0f;
    static const float c1 = 3424.0f / 4096.0f;
    static const float c2 = 2413.0f / 128.0f;
    static const float c3 = 2392.0f / 128.0f;

    float3 normalized_nits = saturate(nits / 10000.0f);
    float3 lm1 = pow(normalized_nits, m1);
    float3 num = c1 + c2 * lm1;
    float3 den = 1.0f + c3 * lm1;
    return pow(num / den, m2);
}

float3 pq_to_linear_nits(float3 pq) {
    static const float m1 = 2610.0f / 16384.0f;
    static const float m2 = 2523.0f / 32.0f;
    static const float c1 = 3424.0f / 4096.0f;
    static const float c2 = 2413.0f / 128.0f;
    static const float c3 = 2392.0f / 128.0f;

    float3 p = pow(saturate(pq), 1.0f / m2);
    float3 num = max(p - c1, 0.0f);
    float3 den = c2 - c3 * p;
    float3 normalized_nits = pow(max(num / max(den, 1e-6f), 0.0f), 1.0f / m1);
    return normalized_nits * 10000.0f;
}

float4 main(PS_INPUT input) : SV_TARGET {
    float4 overlay = input.color * overlay_tex.Sample(sampler0, input.uv);
    float4 scene = scene_tex.Load(int3(int2(input.pos.xy), 0));
    float alpha = saturate(overlay.a);
    // Preserve scene pixels in transparent areas to avoid resampling and PQ round-trip errors.
    if (alpha == 0.0f) {
        return scene;
    }
    float out_alpha = alpha + scene.a * (1.0f - alpha);

    // Preserve legacy premultiplied blending for SDR; HDR overlays are already premultiplied in linear space.
    if (output_mode < 0.5f) {
        return float4(overlay.rgb + scene.rgb * (1.0f - alpha), out_alpha);
    }
    float3 overlay_linear = overlay.rgb;

    // 1 = HDR10 (PQ, Rec.2020)
    if (output_mode < 1.5f) {
        float3 scene_nits = pq_to_linear_nits(scene.rgb);
        float3 overlay_nits = rec709_to_rec2020(overlay_linear) * paper_white_nits;
        float3 out_nits = overlay_nits + scene_nits * (1.0f - alpha);
        return float4(linear_nits_to_pq(out_nits), out_alpha);
    }

    // 2 = scRGB (linear FP16; 1.0 ~= 80 nits)
    float3 overlay_scrgb = overlay_linear * sc_rgb_scale;
    float3 out_scrgb = overlay_scrgb + scene.rgb * (1.0f - alpha);
    return float4(out_scrgb, out_alpha);
}
)";
