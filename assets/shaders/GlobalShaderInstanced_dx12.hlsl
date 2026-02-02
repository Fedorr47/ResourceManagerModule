// Resource slots must match DirectX12RHI root signature (t0..t11, s0..s1).

SamplerState gLinear : register(s0);
SamplerComparisonState gShadowCmp : register(s1);

Texture2D gAlbedo : register(t0);

// Directional shadow map (depth)
Texture2D<float> gDirShadow : register(t1);

// Lights buffer (StructuredBuffer<GPULight>)
struct GPULight
{
    float4 p0; // pos.xyz, type
    float4 p1; // dir.xyz, intensity
    float4 p2; // color.rgb, range
    float4 p3; // cosInner, cosOuter, attLin, attQuad
};
StructuredBuffer<GPULight> gLights : register(t2);

// Spot shadow maps (depth)
Texture2D<float> gSpotShadow0 : register(t3);
Texture2D<float> gSpotShadow1 : register(t4);
Texture2D<float> gSpotShadow2 : register(t5);
Texture2D<float> gSpotShadow3 : register(t6);

// Point shadow distance cubemaps (R32_FLOAT)
TextureCube<float> gPointShadow0 : register(t7);
TextureCube<float> gPointShadow1 : register(t8);
TextureCube<float> gPointShadow2 : register(t9);
TextureCube<float> gPointShadow3 : register(t10);

// Shadow metadata buffer (one element).
struct ShadowDataSB
{
    float4 spotVPRows[16]; // 4 matrices * 4 rows
    float4 spotInfo[4]; // { lightIndexBits, bias, 0, 0 }

    float4 pointPosRange[4]; // { pos.xyz, range }
    float4 pointInfo[4]; // { lightIndexBits, bias, 0, 0 }
};
StructuredBuffer<ShadowDataSB> gShadowData : register(t11);

cbuffer PerBatchCB : register(b0)
{
    float4x4 uViewProj;
    float4x4 uLightViewProj; // directional shadow VP (rows)

    float4 uCameraAmbient; // xyz + ambientStrength
    float4 uBaseColor;     // fallback baseColor

    // x=shininess, y=specStrength, z=materialShadowBiasTexels, w=flags (bitpacked as float)
    float4 uMaterialFlags;

    // x=lightCount, y=spotShadowCount, z=pointShadowCount, w=unused
    float4 uCounts;

    // x=dirBaseBiasTexels, y=spotBaseBiasTexels, z=pointBaseBiasTexels, w=slopeScaleTexels
    float4 uShadowBias;
};


// Flags (must match C++).
static const uint FLAG_USE_TEX = 1u << 0;
static const uint FLAG_USE_SHADOW = 1u << 1;

float4x4 MakeMatRows(float4 r0, float4 r1, float4 r2, float4 r3)
{
    return float4x4(r0, r1, r2, r3);
}

float4 MulRows(float4 v, float4 r0, float4 r1, float4 r2, float4 r3)
{
    return float4(dot(v, r0), dot(v, r1), dot(v, r2), dot(v, r3));
}

float SmoothStep01(float t)
{
	t = saturate(t);
	return t * t * (3.0f - 2.0f * t); // classic smoothstep
}

float SlopeScaleTerm(float NdotL)
{
    // slope-scale bias term ~ tan(theta), theta = acos(NdotL)
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
    float4 shadowPos : TEXCOORD3; // directional shadow
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

float Shadow2D(Texture2D<float> shadowMap, float4 shadowPos, float biasTexels)
{
    float3 p = shadowPos.xyz / max(shadowPos.w, 1e-6f);

    if (p.x < -1.0f || p.x > 1.0f || p.y < -1.0f || p.y > 1.0f || p.z < 0.0f || p.z > 1.0f)
        return 1.0f;

    float2 uv = float2(p.x, -p.y) * 0.5f + 0.5f;

    uint w, h;
    shadowMap.GetDimensions(w, h);
    float2 texel = 1.0f / float2(max(w, 1u), max(h, 1u));

    float biasDepth = biasTexels * max(texel.x, texel.y);

    float z = p.z - biasDepth;
float s = 0.0f;
    s += shadowMap.SampleCmpLevelZero(gShadowCmp, uv + texel * float2(-0.5f, -0.5f), z);
    s += shadowMap.SampleCmpLevelZero(gShadowCmp, uv + texel * float2(0.5f, -0.5f), z);
    s += shadowMap.SampleCmpLevelZero(gShadowCmp, uv + texel * float2(-0.5f, 0.5f), z);
    s += shadowMap.SampleCmpLevelZero(gShadowCmp, uv + texel * float2(0.5f, 0.5f), z);
    return s * 0.25f;
}

float ShadowPoint(TextureCube<float> distCube, float3 lightPos, float range, float3 worldPos, float biasTexels)
{
    float3 v = worldPos - lightPos;
    float d = length(v);
    float nd = saturate(d / max(range, 1e-3f));

    float3 dir = (d > 1e-5f) ? (v / d) : float3(0, 0, 1);
    float stored = distCube.SampleLevel(gLinear, dir, 0).r;

	uint w, h, levels;
	distCube.GetDimensions(0, w, h, levels); // mip 0
	float biasNorm = biasTexels / float(max(w, h));

    return (nd - biasNorm <= stored) ? 1.0f : 0.0f;
}

float SpotShadowFactor(uint si, ShadowDataSB sd, float3 worldPos, float biasTexels)
{
    float4 r0 = sd.spotVPRows[si * 4 + 0];
    float4 r1 = sd.spotVPRows[si * 4 + 1];
    float4 r2 = sd.spotVPRows[si * 4 + 2];
    float4 r3 = sd.spotVPRows[si * 4 + 3];
    // Build clip-space position without relying on matrix constructor row/col conventions.
    float4 sp = MulRows(float4(worldPos, 1.0f), r0, r1, r2, r3);
    
    if (si == 0)
        return Shadow2D(gSpotShadow0, sp, biasTexels);
    if (si == 1)
        return Shadow2D(gSpotShadow1, sp, biasTexels);
    if (si == 2)
        return Shadow2D(gSpotShadow2, sp, biasTexels);
    return Shadow2D(gSpotShadow3, sp, biasTexels);
}

float PointShadowFactor(uint pi, ShadowDataSB sd, float3 worldPos, float biasTexels)
{
    float3 lp = sd.pointPosRange[pi].xyz;
    float range = sd.pointPosRange[pi].w;
    if (pi == 0)
        return ShadowPoint(gPointShadow0, lp, range, worldPos, biasTexels);
    if (pi == 1)
        return ShadowPoint(gPointShadow1, lp, range, worldPos, biasTexels);
    if (pi == 2)
        return ShadowPoint(gPointShadow2, lp, range, worldPos, biasTexels);
    return ShadowPoint(gPointShadow3, lp, range, worldPos, biasTexels);
}

float4 PSMain(VSOut IN) : SV_Target0
{
    const uint flags = asuint(uMaterialFlags.w);
    const bool useTex = (flags & FLAG_USE_TEX) != 0;
    const bool useShadow = (flags & FLAG_USE_SHADOW) != 0;

    float3 base = uBaseColor.rgb;
    if (useTex)
    {
        base *= gAlbedo.Sample(gLinear, IN.uv).rgb;
    }

    float3 N = normalize(IN.nrmW);
    float3 V = normalize(uCameraAmbient.xyz - IN.worldPos);

    const float shininess = uMaterialFlags.x;
    const float specStrength = uMaterialFlags.y;
    const float materialBiasTexels = uMaterialFlags.z;

    float3 color = base * uCameraAmbient.w;

    const uint lightCount = (uint)uCounts.x;
    const uint spotShadowCount = (uint)uCounts.y;
    const uint pointShadowCount = (uint)uCounts.z;

    ShadowDataSB sd = gShadowData[0];

    // Bias controls are in "texel units": larger maps => smaller depth bias.
    const float dirBaseBiasTexels   = uShadowBias.x;
    const float spotBaseBiasTexels  = uShadowBias.y;
    const float pointBaseBiasTexels = uShadowBias.z;
    const float slopeScaleTexels    = uShadowBias.w;

    for (uint i = 0; i < lightCount; ++i)
    {
        GPULight Ld = gLights[i];
        const uint type = (uint)Ld.p0.w;

        float3 Lpos = Ld.p0.xyz;
        float3 LdirFromLight = normalize(Ld.p1.xyz); // Directional/Spot: FROM light towards scene
        float intensity = Ld.p1.w;
        float3 Lcolor = Ld.p2.rgb;
        float range = Ld.p2.w;

        float3 L = float3(0, 0, 1); // to light
        float att = 1.0f;

        if (type == 0)
        {
            // Directional: LdirFromLight is FROM light to scene, so -dir is FROM point to light.
            L = normalize(-LdirFromLight);
        }
        else
        {
            float3 toL = Lpos - IN.worldPos;
            float dist = length(toL);
            if (dist > range)
                continue;

            L = toL / max(dist, 1e-6f);

            const float attLin  = Ld.p3.z;
            const float attQuad = Ld.p3.w;

            // Range fade (artist-friendly) + physical-ish attenuation.
            float rangeFade = saturate(1.0f - dist / max(range, 1e-3f));
            float denom = 1.0f + attLin * dist + attQuad * dist * dist;
            att = rangeFade / max(denom, 1e-3f);

            if (type == 2)
            {
                // Spot cone attenuation (cos angles are precomputed on CPU).
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
                // Match this spot light to its shadow map by light index.
                for (uint si = 0; si < spotShadowCount; ++si)
                {
                    uint idx = asuint(sd.spotInfo[si].x);
                    if (idx == i)
                    {
                        float extraBiasTexels = sd.spotInfo[si].y; // optional per-light add
                        float biasTexels = ComputeBiasTexels(NdotL, spotBaseBiasTexels, slopeScaleTexels, materialBiasTexels, extraBiasTexels);
                        shadow = SpotShadowFactor(si, sd, IN.worldPos, biasTexels);
                        break;
                    }
                }
            }
            else if (type == 1) // Point
            {
                for (uint pi = 0; pi < pointShadowCount; ++pi)
                {
                    uint idx = asuint(sd.pointInfo[pi].x);
                    if (idx == i)
                    {
                        float extraBiasTexels = sd.pointInfo[pi].y; // optional per-light add
                        float biasTexels = ComputeBiasTexels(NdotL, pointBaseBiasTexels, slopeScaleTexels, materialBiasTexels, extraBiasTexels);
                        shadow = PointShadowFactor(pi, sd, IN.worldPos, biasTexels);
                        break;
                    }
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
