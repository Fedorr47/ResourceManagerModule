#ifndef PARTICLE_TEXTURED
#define PARTICLE_TEXTURED 0
#endif

SamplerState gLinear : register(s0);
#if PARTICLE_TEXTURED
Texture2D gParticleTex : register(t0);
#endif

cbuffer ParticleCB : register(b0)
{
    float4x4 uViewProj;
    float4 uCameraRight;
    float4 uCameraUp;
};

struct VSIn
{
    float3 localPos   : POSITION;
    float2 uv         : TEXCOORD0;
    float4 centerSize : TEXCOORD1;
    float4 color      : TEXCOORD2;
    float4 params0    : TEXCOORD3;
    float4 params1    : TEXCOORD4;
};

struct VSOut
{
    float4 pos   : SV_POSITION;
    float2 uv    : TEXCOORD0;
    float4 color : TEXCOORD1;
};

VSOut VSMain(VSIn IN)
{
    const float size = IN.centerSize.w;
    const float angle = IN.params0.x;

    float2 local = IN.localPos.xy;
    const float s = sin(angle);
    const float c = cos(angle);
    local = float2(local.x * c - local.y * s, local.x * s + local.y * c);

    const float3 worldPos = IN.centerSize.xyz
        + uCameraRight.xyz * (local.x * size)
        + uCameraUp.xyz * (local.y * size);

    VSOut OUT;
    OUT.pos = mul(float4(worldPos, 1.0f), uViewProj);
    OUT.uv = IN.uv;
    OUT.color = IN.color;
    return OUT;
}

float4 PSMain(VSOut IN) : SV_Target
{
    const float2 d = IN.uv * 2.0f - 1.0f;
    const float r2 = dot(d, d);
    const float falloff = saturate(1.0f - r2);
    const float intensity = falloff * falloff;

#if PARTICLE_TEXTURED
    const float4 texel = gParticleTex.Sample(gLinear, IN.uv);
    const float alpha = texel.a * intensity * IN.color.a;
    return float4(texel.rgb * IN.color.rgb * alpha, alpha);
#else
    return float4(IN.color.rgb * intensity * IN.color.a, intensity * IN.color.a);
#endif
}
