cbuffer PerDraw : register(b0)
{
    float4x4 uMVP; // lightMVP (already includes Model)
};

struct VSIn
{
    float3 pos : POSITION;
    float3 nrm : NORMAL;
    float2 uv : TEXCOORD0;
};

struct VSOut
{
    float4 posH : SV_Position;
};

VSOut VSMain(VSIn vin)
{
    VSOut o;
    // column-major (glm) => M * v
    o.posH = mul(uMVP, float4(vin.pos, 1.0f));
    return o;
}

// Depth-only: no color output required
void PSMain(VSOut pin)
{
    // intentionally empty
}
