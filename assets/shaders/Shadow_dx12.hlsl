cbuffer PerDraw : register(b0)
{
    float4x4 uMVP; // lightMVP
};

struct VSIn
{
    float3 pos : POSITION0;
    float3 nrm : NORMAL0;
    float2 uv : TEXCOORD0;
};

struct VSOut
{
    float4 posH : SV_Position;
};

VSOut VSMain(VSIn vin)
{
    VSOut o;
    const float4 p = float4(vin.pos, 1.0f);

    // column-major (glm) => M * v
    o.posH = mul(uMVP, p);
    return o;
}

// Depth-only: no color output required
void PSMain(VSOut pin)
{
    // intentionally empty
}
