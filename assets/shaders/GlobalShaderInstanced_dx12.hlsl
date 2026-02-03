// GlobalShaderInstanced_dx12.hlsl
// NOTE: Save as UTF-8 without BOM to keep FXC happy.

// Samplers (must match root signature)
SamplerState gLinear : register(s0);
SamplerComparisonState gShadowCmp : register(s1);
SamplerState gPointClamp : register(s2);

// Textures / SRVs (must match root signature tables per register)
Texture2D gAlbedo : register(t0);

// Directional shadow map (depth SRV)
Texture2D<float> gDirShadow : register(t1);

// Lights buffer
struct GPULight
{
    float4 p0; // pos.xyz, type
    float4 p1; // dir.xyz, intensity
    float4 p2; // color.rgb, range
    float4 p3; // cosInner, cosOuter, attLin, attQuad
};
StructuredBuffer<GPULight> gLights : register(t2);

// Spot shadow maps (depth) - NO ARRAYS (root sig uses 1-descriptor tables per tN)
Texture2D<float> gSpotShadow0 : register(t3);
Texture2D<float> gSpotShadow1 : register(t4);
Texture2D<float> gSpotShadow2 : register(t5);
Texture2D<float> gSpotShadow3 : register(t6);

// Point distance cubemaps (normalized distance)
TextureCube<float> gPointShadow0 : register(t7);
TextureCube<float> gPointShadow1 : register(t8);
TextureCube<float> gPointShadow2 : register(t9);
TextureCube<float> gPointShadow3 : register(t10);

// Shadow metadata buffer (one element)
struct ShadowDataSB
{
    float4 spotVPRows[16]; // 4 matrices * 4 rows (row-major rows)
    float4 spotInfo[4]; // { lightIndexBits, 0, extraBiasTexels, 0 }

    float4 pointPosRange[4]; // { pos.xyz, range }
    float4 pointInfo[4]; // { lightIndexBits, 0, extraBiasTexels, 0 }
};
StructuredBuffer<ShadowDataSB> gShadowData : register(t11);

cbuffer PerBatchCB : register(b0)
{
    float4x4 uViewProj;
    float4x4 uLightViewProj; // directional shadow VP (rows)

    float4 uCameraAmbient; // xyz + ambientStrength
    float4 uBaseColor; // fallback baseColor

    // x=shininess, y=specStrength, z=materialShadowBiasTexels, w=flags (bitpacked as float)
    float4 uMaterialFlags;

    // x=lightCount, y=spotShadowCount, z=pointShadowCount, w=unused
    float4 uCounts;

    // x=dirBaseBiasTexels, y=spotBaseBiasTexels, z=pointBaseBiasTexels, w=slopeScaleTexels
    float4 uShadowBias;
};

// Flags (must match C++)
static const uint FLAG_USE_TEX = 1u << 0;
static const uint FLAG_USE_SHADOW = 1u << 1;

static const uint kMaxSpotShadows = 4;
static const uint kMaxPointShadows = 4;

// Helpers
float4x4 MakeMatRows(float4 r0, float4 r1, float4 r2, float4 r3)
{
    return float4x4(r0, r1, r2, r3);
}

float SmoothStep01(float t)
{
    t = saturate(t);
    return t * t * (3.0f - 2.0f * t);
}

float SlopeScaleTerm(float NdotL)
{
    NdotL = max(NdotL, 1e-4f);
    return sqrt(max(1.0f - NdotL * NdotL, 0.0f)) / NdotL;
}

float ComputeBiasTexels(float NdotL,
                        float baseBiasTexels,
                        float slopeScaleTexels,
                        float materialBiasTexels,
                        float extraBiasTexels)
{
    return baseBiasTexels
         + SlopeScaleTerm(NdotL) * slopeScaleTexels
         + materialBiasTexels
         + extraBiasTexels;
}

int FindSpotShadowSlot(uint lightIndex, uint spotShadowCount)
{
    [unroll]
    for (uint s = 0; s < kMaxSpotShadows; ++s)
    {
        if (s >= spotShadowCount)
            break;
        uint stored = asuint(gShadowData[0].spotInfo[s].x);
        if (stored == lightIndex)
            return (int) s;
    }
    return -1;
}

int FindPointShadowSlot(uint lightIndex, uint pointShadowCount)
{
    [unroll]
    for (uint s = 0; s < kMaxPointShadows; ++s)
    {
        if (s >= pointShadowCount)
            break;
        uint stored = asuint(gShadowData[0].pointInfo[s].x);
        if (stored == lightIndex)
            return (int) s;
    }
    return -1;
}

// Vertex IO
struct VSIn
{
    float3 pos : POSITION;
    float3 nrm : NORMAL;
    float2 uv : TEXCOORD0;

    // Instance matrix rows in TEXCOORD1..4
    float4 i0 : TEXCOORD1;
    float4 i1 : TEXCOORD2;
    float4 i2 : TEXCOORD3;
    float4 i3 : TEXCOORD4;
};

struct VSOut
{
    float4 posH : SV_Position;
    float3 worldPos : TEXCOORD0;
    float3 nrmW : TEXCOORD1;
    float2 uv : TEXCOORD2;
    float4 shadowPos : TEXCOORD3; // directional shadow clip
};

VSOut VSMain(VSIn IN)
{
    VSOut OUT;

    float4x4 model = MakeMatRows(IN.i0, IN.i1, IN.i2, IN.i3);

    float4 world = mul(float4(IN.pos, 1.0f), model);
    OUT.worldPos = world.xyz;

    float3 nrmW = mul(float4(IN.nrm, 0.0f), model).xyz;
    OUT.nrmW = normalize(nrmW);

    OUT.uv = IN.uv;
    OUT.posH = mul(world, uViewProj);

    OUT.shadowPos = mul(world, uLightViewProj);
    return OUT;
}

// Shadow sampling
float Shadow2D(Texture2D<float> shadowMap, float4 shadowClip, float biasTexels)
{
    float3 p = shadowClip.xyz / max(shadowClip.w, 1e-6f);

    // clip -> ndc bounds check
    if (p.x < -1.0f || p.x > 1.0f || p.y < -1.0f || p.y > 1.0f || p.z < 0.0f || p.z > 1.0f)
        return 1.0f;

    // ndc -> uv (note: flip Y)
    float2 uv = float2(p.x, -p.y) * 0.5f + 0.5f;

    uint w, h;
    shadowMap.GetDimensions(w, h);
    float2 texel = 1.0f / float2(max(w, 1u), max(h, 1u));

    float biasDepth = biasTexels * max(texel.x, texel.y);
    float z = p.z - biasDepth;

    // 2x2 PCF
    float s = 0.0f;
    s += shadowMap.SampleCmpLevelZero(gShadowCmp, uv + texel * float2(-0.5f, -0.5f), z);
    s += shadowMap.SampleCmpLevelZero(gShadowCmp, uv + texel * float2(0.5f, -0.5f), z);
    s += shadowMap.SampleCmpLevelZero(gShadowCmp, uv + texel * float2(-0.5f, 0.5f), z);
    s += shadowMap.SampleCmpLevelZero(gShadowCmp, uv + texel * float2(0.5f, 0.5f), z);
    return s * 0.25f;
}

float ShadowPoint(TextureCube<float> distCube,
                  float3 lightPos, float range,
                  float3 worldPos, float biasTexels)
{
    float3 v = worldPos - lightPos;
    float d = length(v);
    float3 dir = v / max(d, 1e-6f);

    float nd = saturate(d / max(range, 1e-3f));

    // distance cube expects clamp sampler (no wrap)
    float stored = distCube.SampleLevel(gPointClamp, dir, 0).r;

    uint w, h, levels;
    distCube.GetDimensions(0, w, h, levels);
    float biasNorm = biasTexels / float(max(w, h));

    return (nd - biasNorm <= stored) ? 1.0f : 0.0f;
}

float SpotShadowFactor(uint slot, ShadowDataSB sd, float3 worldPos, float biasTexels)
{
    if (slot >= 4)
        return 1.0f;

    float4 r0 = sd.spotVPRows[slot * 4 + 0];
    float4 r1 = sd.spotVPRows[slot * 4 + 1];
    float4 r2 = sd.spotVPRows[slot * 4 + 2];
    float4 r3 = sd.spotVPRows[slot * 4 + 3];
    float4x4 VP = float4x4(r0, r1, r2, r3);

    float4 clip = mul(float4(worldPos, 1.0f), VP);

    if (slot == 0)
        return Shadow2D(gSpotShadow0, clip, biasTexels);
    if (slot == 1)
        return Shadow2D(gSpotShadow1, clip, biasTexels);
    if (slot == 2)
        return Shadow2D(gSpotShadow2, clip, biasTexels);
    return Shadow2D(gSpotShadow3, clip, biasTexels);
}

float PointShadowFactor(uint slot, ShadowDataSB sd, float3 worldPos, float biasTexels)
{
    if (slot >= 4)
        return 1.0f;

    float3 lp = sd.pointPosRange[slot].xyz;
    float range = sd.pointPosRange[slot].w;

    if (slot == 0)
        return ShadowPoint(gPointShadow0, lp, range, worldPos, biasTexels);
    if (slot == 1)
        return ShadowPoint(gPointShadow1, lp, range, worldPos, biasTexels);
    if (slot == 2)
        return ShadowPoint(gPointShadow2, lp, range, worldPos, biasTexels);
    return ShadowPoint(gPointShadow3, lp, range, worldPos, biasTexels);
}

// Pixel Shader
float4 PSMain(VSOut IN) : SV_Target0
{
    const uint flags = asuint(uMaterialFlags.w);
    const bool useTex = (flags & FLAG_USE_TEX) != 0;
    const bool useShadow = (flags & FLAG_USE_SHADOW) != 0;

    float3 base = uBaseColor.rgb;
    if (useTex)
        base *= gAlbedo.Sample(gLinear, IN.uv).rgb;

    float3 N = normalize(IN.nrmW);
    float3 V = normalize(uCameraAmbient.xyz - IN.worldPos);

    const float shininess = uMaterialFlags.x;
    const float specStrength = uMaterialFlags.y;
    const float materialBiasTexels = uMaterialFlags.z;

    float3 color = base * uCameraAmbient.w;

    const uint lightCount = (uint) uCounts.x;
    const uint spotShadowCount = (uint) uCounts.y;
    const uint pointShadowCount = (uint) uCounts.z;

    ShadowDataSB sd = gShadowData[0];

    const float dirBaseBiasTexels = uShadowBias.x;
    const float spotBaseBiasTexels = uShadowBias.y;
    const float pointBaseBiasTexels = uShadowBias.z;
    const float slopeScaleTexels = uShadowBias.w;

    [loop]
    for (uint i = 0; i < lightCount; ++i)
    {
        GPULight Ld = gLights[i];
        const uint type = (uint) Ld.p0.w;

        float3 Lpos = Ld.p0.xyz;
        float3 LdirFromLight = normalize(Ld.p1.xyz); // FROM light towards scene
        float intensity = Ld.p1.w;
        float3 Lcolor = Ld.p2.rgb;
        float range = Ld.p2.w;

        float3 L = float3(0, 0, 1);
        float att = 1.0f;

        if (type == 0)
        {
            // Directional: -dir is from point to light
            L = normalize(-LdirFromLight);
        }
        else
        {
            float3 toL = Lpos - IN.worldPos;
            float dist = length(toL);
            if (dist > range)
                continue;

            L = toL / max(dist, 1e-6f);

            const float attLin = Ld.p3.z;
            const float attQuad = Ld.p3.w;

            float rangeFade = saturate(1.0f - dist / max(range, 1e-3f));
            float denom = 1.0f + attLin * dist + attQuad * dist * dist;
            att = rangeFade / max(denom, 1e-3f);

            if (type == 2)
            {
                // Spot cone
                const float cosInner = Ld.p3.x;
                const float cosOuter = Ld.p3.y;

                float3 fromLightToP = normalize(IN.worldPos - Lpos);
                float cosAng = dot(fromLightToP, LdirFromLight);

                float t = (cosAng - cosOuter) / max(cosInner - cosOuter, 1e-4f);
                float cone = SmoothStep01(t);

                att *= cone;
                if (att <= 0.0f)
                    continue;
            }
        }

        float NdotL = max(dot(N, L), 0.0f);
        if (NdotL <= 0.0f)
            continue;

        float shadow = 1.0f;

        if (useShadow)
        {
            if (type == 0) // Directional
            {
                float biasTexels = ComputeBiasTexels(NdotL, dirBaseBiasTexels, slopeScaleTexels, materialBiasTexels, 0.0f);
                shadow = Shadow2D(gDirShadow, IN.shadowPos, biasTexels);
            }
            else if (type == 2) // Spot
            {
                int slot = FindSpotShadowSlot(i, spotShadowCount);
                if (slot >= 0)
                {
                    float extraBiasTexels = sd.spotInfo[slot].z;
                    float biasTexels = ComputeBiasTexels(NdotL, spotBaseBiasTexels, slopeScaleTexels, materialBiasTexels, extraBiasTexels);
                    shadow = SpotShadowFactor((uint) slot, sd, IN.worldPos, biasTexels);
                }
            }
            else if (type == 1) // Point
            {
                int slot = FindPointShadowSlot(i, pointShadowCount);
                if (slot >= 0)
                {
                    float extraBiasTexels = sd.pointInfo[slot].z;
                    float biasTexels = ComputeBiasTexels(NdotL, pointBaseBiasTexels, slopeScaleTexels, materialBiasTexels, extraBiasTexels);
                    shadow = PointShadowFactor((uint) slot, sd, IN.worldPos, biasTexels);
                }
            }
        }

        float3 diffuse = base * NdotL;

        float3 H = normalize(L + V);
        float spec = pow(max(dot(N, H), 0.0f), max(shininess, 1.0f));
        float3 specular = specStrength * spec;

        color += (diffuse + specular) * (Lcolor * intensity * att) * shadow;
    }

    return float4(color, 1.0f);
}
