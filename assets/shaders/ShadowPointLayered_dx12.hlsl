// ShadowPointLayered_dx12.hlsl
// Shader Model 6.1 (DXC) vertex+pixel shader for point light shadow cubemap.
// Single-pass layered rendering into a Texture2DArray (6 slices) using SV_RenderTargetArrayIndex.
// Requires: D3D12_OPTIONS3.VPAndRTArrayIndexFromAnyShaderFeedingRasterizer.

cbuffer PointShadowCB : register(b0)
{
    float4x4 uFaceViewProj[6];
    float4   uLightPosRange; // xyz + range
    float4   uMisc;          // x = bias (reserved)
};

struct VSIn
{
    float3 pos : POSITION;
    float3 nrm : NORMAL;
    float2 uv  : TEXCOORD0;

    float4 i0  : TEXCOORD1;
    float4 i1  : TEXCOORD2;
    float4 i2  : TEXCOORD3;
    float4 i3  : TEXCOORD4;

    uint   instanceId : SV_InstanceID;
};

float4x4 MakeMatRows(float4 r0, float4 r1, float4 r2, float4 r3)
{
    return float4x4(r0, r1, r2, r3);
}

struct VSOut
{
    float4 posH     : SV_Position;
    float3 worldPos : TEXCOORD0;
    uint   rtIndex  : SV_RenderTargetArrayIndex;
};

VSOut VS_ShadowPointLayered(VSIn IN)
{
    VSOut OUT;

    const float4x4 model = MakeMatRows(IN.i0, IN.i1, IN.i2, IN.i3);
    const float4 world   = mul(float4(IN.pos, 1.0f), model);

    OUT.worldPos = world.xyz;

    // Assumes instance data is duplicated 6 times per original instance: for each object instance,
    // we emit faces 0..5 in that order. Then face = instanceId % 6.
    const uint face = IN.instanceId % 6u;

    OUT.posH   = mul(world, uFaceViewProj[face]);
    OUT.rtIndex = face;
    return OUT;
}

// R32_FLOAT distance (normalized by range).
float PS_ShadowPointLayered(VSOut IN) : SV_Target0
{
    const float3 L = IN.worldPos - uLightPosRange.xyz;
    const float dist = length(L);
    const float norm = saturate(dist / max(uLightPosRange.w, 0.001f));
    return norm;
}
