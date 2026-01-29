cbuffer PerDraw : register(b0)
{
    float4x4 uMVP;
    float4x4 uLightMVP;
    float4 uColor;

    int uUseTex;
    int uUseShadow;
    float uShadowBias;
    float _pad0;
};

Texture2D gAlbedo : register(t0);
Texture2D<float> gShadow : register(t1);

SamplerState gSamp : register(s0);
SamplerComparisonState gShadowSamp : register(s1);

struct VSIn
{
    float3 pos : POSITION0;
    float3 nrm : NORMAL0;
    float2 uv : TEXCOORD0;
};

struct VSOut
{
    float4 posH : SV_Position;
    float2 uv : TEXCOORD0;
    float4 shadowPos : TEXCOORD1;
};

VSOut VSMain(VSIn vin)
{
    VSOut o;

    const float4 p = float4(vin.pos, 1.0f);

    // IMPORTANT:
    // HLSL matrices are column-major by default, matching glm::value_ptr() layout.
    // Therefore we must multiply as M * v (not v * M), otherwise you'll effectively use a transposed matrix
    // and the object can get clipped (often with negative z) and "disappear".
    o.posH = mul(uMVP, p);
    o.shadowPos = mul(uLightMVP, p);
    o.uv = vin.uv;

    return o;
}

float SampleShadow(float4 shadowPos)
{
    // Project to NDC
    const float invW = 1.0f / max(shadowPos.w, 1e-6f);
    const float3 proj = shadowPos.xyz * invW;

    // NDC xy -> UV
    // In D3D, texture V axis goes down, so we flip Y when converting from NDC.
    const float2 uv = proj.xy * float2(0.5f, -0.5f) + float2(0.5f, 0.5f);

    // With *_ZO projection matrices, proj.z is already expected to be in [0..1]
    const float depth = proj.z;

    // Outside = fully lit
    if (uv.x < 0.0f || uv.x > 1.0f || uv.y < 0.0f || uv.y > 1.0f || depth < 0.0f || depth > 1.0f)
        return 1.0f;

    // Compare sampler: returns [0..1]
    return gShadow.SampleCmpLevelZero(gShadowSamp, uv, depth - uShadowBias);
}

float4 PSMain(VSOut pin) : SV_Target0
{
    float4 base = uColor;

    if (uUseTex != 0)
        base *= gAlbedo.Sample(gSamp, pin.uv);

    if (uUseShadow != 0)
    {
        const float sh = SampleShadow(pin.shadowPos);
        // Keep it visible even if the shadow map is empty/misaligned.
        base.rgb *= (0.25f + 0.75f * sh);
    }

    return base;
}
