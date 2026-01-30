cbuffer PerDraw : register(b0)
{
    float4x4 uLightViewProj; // (LightProj * LightView)
};

struct VSIn
{
    float3 pos : POSITION;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD0;

    // Instance matrix rows (как в GlobalShaderInstanced_dx12.hlsl)
    float4 i0 : TEXCOORD1;
    float4 i1 : TEXCOORD2;
    float4 i2 : TEXCOORD3;
    float4 i3 : TEXCOORD4;
};

struct VSOut
{
    float4 posH : SV_POSITION;
};

VSOut VSMain(VSIn IN)
{
    VSOut OUT;

    float4x4 model = float4x4(IN.i0, IN.i1, IN.i2, IN.i3);

    float4 wpos = mul(model, float4(IN.pos, 1.0));
    OUT.posH = mul(uLightViewProj, wpos);
    return OUT;
}

// Depth-only
void PSMain()
{
}
