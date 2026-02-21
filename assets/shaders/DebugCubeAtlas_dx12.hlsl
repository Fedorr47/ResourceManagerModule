// DebugCubeAtlas_dx12.hlsl
// NOTE: Save as UTF-8 without BOM.

SamplerState gPointClamp : register(s2);
TextureCube<float> gCube : register(t0);

cbuffer DebugCubeAtlasCB : register(b0)
{
    float uInvRange; // 1/range if stored is world-distance, or 1 if stored is already normalized [0..1]
    float uGamma; // 1.0 = linear
    uint uInvert; // 1 -> invert (near=white)
    uint uShowGrid; // 1 -> draw tile borders
    float2 uInvViewport; // (1/width, 1/height)  <-- важно!
    float2 _pad;
};

struct VSOut
{
    float4 pos : SV_Position;
};

// Fullscreen triangle
VSOut VSMain(uint vid : SV_VertexID)
{
    VSOut o;
    if (vid == 0)
        o.pos = float4(-1.0, -1.0, 0.0, 1.0);
    if (vid == 1)
        o.pos = float4(-1.0, 3.0, 0.0, 1.0);
    if (vid == 2)
        o.pos = float4(3.0, -1.0, 0.0, 1.0);
    return o;
}

// st in [-1..1], Y up.
// Face order: +X, -X, +Y, -Y, +Z, -Z
float3 DirFromFaceST(uint face, float2 st)
{
    if (face == 0)
        return float3(1.0, st.y, -st.x); // +X
    if (face == 1)
        return float3(-1.0, st.y, st.x); // -X
    if (face == 2)
        return float3(st.x, 1.0, -st.y); // +Y
    if (face == 3)
        return float3(st.x, -1.0, st.y); // -Y
    if (face == 4)
        return float3(st.x, st.y, 1.0); // +Z
    return float3(-st.x, st.y, -1.0); // -Z
}

float4 PSMain(VSOut i) : SV_Target
{
    // SV_Position in pixel coords (top-left origin in D3D)
    float2 uv = i.pos.xy * uInvViewport;
    uv = saturate(uv);

    // atlas 3x2 tiles
    uint tileX = min((uint) (uv.x * 3.0), 2u);
    uint tileY = min((uint) (uv.y * 2.0), 1u);
    uint face = tileY * 3u + tileX; // 0..5

    float2 uvLocal = frac(uv * float2(3.0, 2.0));

    // borders
    if (uShowGrid != 0)
    {
        float2 fw = fwidth(uvLocal);
        float gx = step(uvLocal.x, fw.x * 1.5) + step(1.0 - uvLocal.x, fw.x * 1.5);
        float gy = step(uvLocal.y, fw.y * 1.5) + step(1.0 - uvLocal.y, fw.y * 1.5);
        if (gx + gy > 0.0)
            return float4(1, 1, 1, 1);
    }

    // local face coords: st in [-1..1], with +Y up
    float2 st;
    st.x = uvLocal.x * 2.0 - 1.0;
    st.y = (1.0 - uvLocal.y) * 2.0 - 1.0;

    float3 dir = normalize(DirFromFaceST(face, st));

    float stored = gCube.SampleLevel(gPointClamp, dir, 0).r;

    // show as grayscale
    float v = saturate(stored * uInvRange);
    if (uInvert != 0)
        v = 1.0 - v;
    if (abs(uGamma - 1.0) > 1e-3)
        v = pow(v, 1.0 / max(uGamma, 1e-3));

    return float4(v, v, v, 1.0);
}