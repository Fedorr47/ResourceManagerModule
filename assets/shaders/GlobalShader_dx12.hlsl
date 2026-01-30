// Stage-1 DX12 forward shader with:
//  - Phong/Blinn lighting for Directional / Point / Spot
//  - lights in StructuredBuffer SRV (t2)
//  - optional albedo texture (t0, can be null SRV)
//  - directional shadow map (t1) with comparison sampler (s1)
//
// Root signature (see DirectX12RHI.cppm):
//   b0 : PerDraw constants
//   t0 : albedo SRV
//   t1 : shadow map SRV (R32_FLOAT from typeless D32 resource)
//   t2 : StructuredBuffer<GPULight> lights
//   s0 : linear wrap
//   s1 : comparison sampler for shadow map

struct VSIn
{
    float3 pos    : POSITION0;
    float3 normal : NORMAL0;
    float2 uv     : TEXCOORD0;
};

struct VSOut
{
    float4 posH       : SV_Position;
    float3 worldPos   : TEXCOORD0;
    float3 normalW    : TEXCOORD1;
    float2 uv         : TEXCOORD2;
    float4 shadowPosH : TEXCOORD3;
};

cbuffer PerDraw : register(b0)
{
    float4x4 uMVP;
    float4x4 uLightMVP;

    float4 uModelRow0;
    float4 uModelRow1;
    float4 uModelRow2;

    float4 uCameraAmbient; // camPos.xyz, ambientStrength
    float4 uBaseColor;     // rgba

    // x=shininess, y=specStrength, z=shadowBias, w=flags (bitpacked as float)
    float4 uMaterialFlags;

    // x = lightCount
    float4 uCounts;
};

struct GPULight
{
    float4 p0; // pos.xyz, type
    float4 p1; // dir.xyz (FROM light), intensity
    float4 p2; // color.rgb, range
    float4 p3; // cosInner, cosOuter, attLin, attQuad
};

Texture2D        gAlbedo     : register(t0);
Texture2D<float> gShadowMap  : register(t1);
StructuredBuffer<GPULight> gLights : register(t2);

SamplerState             gSampler      : register(s0);
SamplerComparisonState   gShadowSampler : register(s1);

static const uint LIGHT_DIR   = 0;
static const uint LIGHT_POINT = 1;
static const uint LIGHT_SPOT  = 2;

static const uint FLAG_USE_TEX    = 1u << 0;
static const uint FLAG_USE_SHADOW = 1u << 1;

float3 WorldFromLocal(float3 localPos)
{
    float4 p = float4(localPos, 1.0f);
    return float3(dot(uModelRow0, p), dot(uModelRow1, p), dot(uModelRow2, p));
}

float3 NormalWorld(float3 localN)
{
    // Stage-1: assumes uniform scale or pure rotation for best results.
    float3 n;
    n.x = dot(uModelRow0.xyz, localN);
    n.y = dot(uModelRow1.xyz, localN);
    n.z = dot(uModelRow2.xyz, localN);
    return normalize(n);
}

float ShadowVisibility(float4 shadowPosH, float bias)
{
    // Homogeneous clip -> NDC
    float3 ndc = shadowPosH.xyz / shadowPosH.w;

    // If behind the light frustum or invalid, treat as lit
    if (ndc.z < 0.0f || ndc.z > 1.0f)
        return 1.0f;

    // NDC -> UV (D3D viewport maps y with (1-ndc.y))
    float2 uv;
    uv.x = ndc.x * 0.5f + 0.5f;
    uv.y = 1.0f - (ndc.y * 0.5f + 0.5f);

    // Outside shadow map => lit
    if (uv.x < 0.0f || uv.x > 1.0f || uv.y < 0.0f || uv.y > 1.0f)
        return 1.0f;

    // Compare (LESS_EQUAL). Ref value is our depth with bias applied.
    const float ref = ndc.z - bias;

    // SampleCmpLevelZero returns [0..1] (PCF if filter set).
    return gShadowMap.SampleCmpLevelZero(gShadowSampler, uv, ref);
}

VSOut VSMain(VSIn vin)
{
    VSOut o;

    const float4 pLocal = float4(vin.pos, 1.0f);

    o.posH = mul(uMVP, pLocal);
    o.shadowPosH = mul(uLightMVP, pLocal);

    o.worldPos = WorldFromLocal(vin.pos);
    o.normalW = NormalWorld(vin.normal);
    o.uv = vin.uv;

    return o;
}

float3 ShadeDirectional(float3 N, float3 V, float3 albedo, GPULight Lgt)
{
    const float3 dirFromLight = normalize(Lgt.p1.xyz);
    const float3 L = normalize(-dirFromLight); // from point -> light
    const float intensity = Lgt.p1.w;
    const float3 color = Lgt.p2.xyz;

    const float NdotL = saturate(dot(N, L));
    if (NdotL <= 0.0f) return 0.0f;

    const float3 H = normalize(L + V);

    const float shininess    = uMaterialFlags.x;
    const float specStrength = uMaterialFlags.y;

    const float spec = pow(saturate(dot(N, H)), shininess) * specStrength;

    const float3 diff = albedo * NdotL;
    const float3 specC = spec.xxx;

    return (diff + specC) * (intensity * color);
}

float3 ShadePoint(float3 worldPos, float3 N, float3 V, float3 albedo, GPULight Lgt)
{
    const float3 lightPos = Lgt.p0.xyz;
    const float intensity = Lgt.p1.w;
    const float3 color = Lgt.p2.xyz;
    const float range = Lgt.p2.w;

    float3 Lvec = lightPos - worldPos;
    const float dist = length(Lvec);
    if (range > 0.0f && dist > range) return 0.0f;

    const float3 L = Lvec / max(dist, 1e-6f);

    const float NdotL = saturate(dot(N, L));
    if (NdotL <= 0.0f) return 0.0f;

    const float attLin = Lgt.p3.z;
    const float attQuad = Lgt.p3.w;
    const float att = 1.0f / (1.0f + attLin * dist + attQuad * dist * dist);

    const float3 H = normalize(L + V);

    const float shininess    = uMaterialFlags.x;
    const float specStrength = uMaterialFlags.y;

    const float spec = pow(saturate(dot(N, H)), shininess) * specStrength;

    const float3 diff = albedo * NdotL;
    const float3 specC = spec.xxx;

    return (diff + specC) * (att * intensity * color);
}

float3 ShadeSpot(float3 worldPos, float3 N, float3 V, float3 albedo, GPULight Lgt)
{
    const float3 lightPos = Lgt.p0.xyz;
    const float intensity = Lgt.p1.w;
    const float3 color = Lgt.p2.xyz;
    const float range = Lgt.p2.w;

    const float3 dirFromLight = normalize(Lgt.p1.xyz); // spotlight axis (from light)

    float3 Lvec = lightPos - worldPos;
    const float dist = length(Lvec);
    if (range > 0.0f && dist > range) return 0.0f;

    const float3 L = Lvec / max(dist, 1e-6f); // point->light

    const float NdotL = saturate(dot(N, L));
    if (NdotL <= 0.0f) return 0.0f;

    // Spot factor: compare axis vs (light->point)
    const float3 toPointFromLight = normalize(worldPos - lightPos);
    const float cosAngle = dot(dirFromLight, toPointFromLight);

    const float cosInner = Lgt.p3.x;
    const float cosOuter = Lgt.p3.y;

    const float spotT = saturate((cosAngle - cosOuter) / max(cosInner - cosOuter, 1e-5f));

    if (spotT <= 0.0f) return 0.0f;

    const float attLin = Lgt.p3.z;
    const float attQuad = Lgt.p3.w;
    const float att = 1.0f / (1.0f + attLin * dist + attQuad * dist * dist);

    const float3 H = normalize(L + V);

    const float shininess    = uMaterialFlags.x;
    const float specStrength = uMaterialFlags.y;

    const float spec = pow(saturate(dot(N, H)), shininess) * specStrength;

    const float3 diff = albedo * NdotL;
    const float3 specC = spec.xxx;

    return (diff + specC) * (spotT * att * intensity * color);
}

float4 PSMain(VSOut pin) : SV_Target
{
    const uint flags = (uint)uMaterialFlags.w;

    float3 albedo = uBaseColor.rgb;
    if ((flags & FLAG_USE_TEX) != 0)
    {
        albedo *= gAlbedo.Sample(gSampler, pin.uv).rgb;
    }

    const float3 N = normalize(pin.normalW);
    const float3 V = normalize(uCameraAmbient.xyz - pin.worldPos);

    const float ambientStrength = uCameraAmbient.w;
    float3 lighting = albedo * ambientStrength;

    const uint lightCount = (uint)uCounts.x;
    [loop]
    for (uint i = 0; i < lightCount; ++i)
    {
        GPULight l = gLights[i];
        const uint type = (uint)l.p0.w;

        if (type == LIGHT_DIR)
        {
            lighting += ShadeDirectional(N, V, albedo, l);
        }
        else if (type == LIGHT_POINT)
        {
            lighting += ShadePoint(pin.worldPos, N, V, albedo, l);
        }
        else if (type == LIGHT_SPOT)
        {
            lighting += ShadeSpot(pin.worldPos, N, V, albedo, l);
        }
    }

    if ((flags & FLAG_USE_SHADOW) != 0)
    {
        const float bias = uMaterialFlags.z;
        const float vis = ShadowVisibility(pin.shadowPosH, bias);

        // Ambient is unshadowed; shadow only affects direct lighting.
        // We approximated by scaling everything above ambient:
        const float3 amb = albedo * ambientStrength;
        const float3 direct = lighting - amb;
        lighting = amb + direct * vis;
    }

    return float4(lighting, uBaseColor.a);
}
