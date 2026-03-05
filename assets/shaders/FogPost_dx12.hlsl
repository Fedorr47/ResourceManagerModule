// FogPost_dx12.hlsl
// Fullscreen fog post effect for deferred DX12 path.
// Applies fog based on reconstructed world position from depth.

SamplerState gLinear : register(s0);
SamplerComparisonState gShadowCmp : register(s1);
SamplerState gPointClamp : register(s2);
SamplerState gLinearClamp : register(s3);

Texture2D gSceneColor : register(t0);
Texture2D<float> gDepth : register(t1);

cbuffer FogConstants : register(b0)
{
    float4x4 uInvViewProj; // row-major in C++ (transposed before upload)
    float4 uCameraPos; // xyz + pad

    // x=start, y=end, z=density, w=mode (0=Linear, 1=Exp, 2=Exp2)
    float4 uFogParams;

    // rgb=color, a=enabled (0/1)
    float4 uFogColor;
};

struct VSOut
{
    float4 svPos : SV_POSITION;
    float2 uv : TEXCOORD0;
};

VSOut VS_Fullscreen(uint vid : SV_VertexID)
{
    // Fullscreen triangle (same convention as DeferredLighting)
    float2 pos = (vid == 0) ? float2(-1.0, -1.0) : (vid == 1) ? float2(-1.0, 3.0) : float2(3.0, -1.0);
    // Texture-space UV (0,0 top-left)
    float2 uv = float2((pos.x + 1.0f) * 0.5f, 1.0f - (pos.y + 1.0f) * 0.5f); // Texture-space UV (0,0 top-left)

    VSOut o;
    o.svPos = float4(pos, 0.0, 1.0);
    o.uv = uv;
    return o;
}

float3 ReconstructWorldPos(float2 uv, float depth01)
{
    // NDC in DX12: x,y in [-1..1], z in [0..1]
    float4 clip;
    clip.x = uv.x * 2.0f - 1.0f;
    clip.y = 1.0f - uv.y * 2.0f;
    clip.z = depth01;
    clip.w = 1.0f;

    float4 wpos = mul(clip, uInvViewProj);
    return wpos.xyz / max(wpos.w, 1e-6f);
}

float FogFactor(float dist)
{
    if (uFogColor.a <= 0.0f)
        return 0.0f;

    const float start = uFogParams.x;
    const float endV = uFogParams.y;
    const float density = max(uFogParams.z, 0.0f);
    const int mode = (int) uFogParams.w;

    if (mode == 0)
    {
        const float denom = max(endV - start, 1e-3f);
        return saturate((dist - start) / denom);
    }
    else if (mode == 1)
    {
        return saturate(1.0f - exp(-density * dist));
    }
    else
    {
        const float d = density * dist;
        return saturate(1.0f - exp(-(d * d)));
    }
}

float4 PS_Fog(VSOut IN) : SV_Target0
{
    float4 scene = gSceneColor.Sample(gLinearClamp, IN.uv);

    // If fog disabled, return original.
    if (uFogColor.a <= 0.0f)
        return scene;

    const float depth = gDepth.Sample(gPointClamp, IN.uv);

    // Background / skybox is depth==1 in your pipeline. Don't fog it.
    if (depth >= 0.999999f)
        return scene;

    const float3 worldPos = ReconstructWorldPos(IN.uv, depth);
    const float dist = length(worldPos - uCameraPos.xyz);

    const float f = FogFactor(dist);
    const float3 fogged = lerp(scene.rgb, uFogColor.rgb, f);
    return float4(fogged, scene.a);
}