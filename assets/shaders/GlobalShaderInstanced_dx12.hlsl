SamplerState gLinear : register(s0);
SamplerComparisonState gShadowCmp : register(s1);
SamplerState gPointClamp : register(s2);
SamplerState gLinearClamp : register(s3);

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
Texture2DArray<float> gPointShadow0 : register(t7);
Texture2DArray<float> gPointShadow1 : register(t8);
Texture2DArray<float> gPointShadow2 : register(t9);
Texture2DArray<float> gPointShadow3 : register(t10);

// Shadow metadata buffer (one element)
struct ShadowDataSB
{
    // ---------------- Directional CSM (atlas) ----------------
    // 3 cascades packed into one atlas (tileSize x tileSize each, laid out horizontally).
    // dirVPRows: row-major VP matrices (4 rows per cascade).
    float4 dirVPRows[12];

    // x=split1, y=split2, z=split3 (max distance), w=fadeFraction
    float4 dirSplits;

    // x=invAtlasW, y=invAtlasH, z=invTileRes, w=cascadeCount
    float4 dirInfo;

    float4 spotVPRows[16]; // 4 matrices * 4 rows (row-major rows)
    float4 spotInfo[4]; // { lightIndexBits, 0, extraBiasTexels, 0 }

    float4 pointPosRange[4]; // { pos.xyz, range }
    float4 pointInfo[4]; // { lightIndexBits, 0, extraBiasTexels, 0 }
};
StructuredBuffer<ShadowDataSB> gShadowData : register(t11);

// DX12 PBR maps
Texture2D gNormal : register(t12);
Texture2D gMetalness : register(t13);
Texture2D gRoughness : register(t14);
Texture2D gAO : register(t15);
Texture2D gEmissive : register(t16);
TextureCube gEnv : register(t17);

cbuffer PerBatchCB : register(b0)
{
    float4x4 uViewProj;
    float4x4 uLightViewProj; // directional shadow VP (rows)

    float4 uCameraAmbient; // xyz + ambientStrength
    float4 uCameraForward; // xyz + 0
    float4 uBaseColor; // fallback baseColor

    // x=shininess, y=specStrength, z=materialShadowBiasTexels, w=flags (bitpacked as float)
    float4 uMaterialFlags;

    // x=metallic, y=roughness, z=ao, w=emissiveStrength
    float4 uPbrParams;

    // x=lightCount, y=spotShadowCount, z=pointShadowCount, w=unused
    float4 uCounts;

    // x=dirBaseBiasTexels, y=spotBaseBiasTexels, z=pointBaseBiasTexels, w=slopeScaleTexels
    float4 uShadowBias;
};

// Flags (must match C++)
static const uint FLAG_USE_TEX = 1u << 0;
static const uint FLAG_USE_SHADOW = 1u << 1;
static const uint FLAG_USE_NORMAL = 1u << 2;
static const uint FLAG_USE_METAL_TEX = 1u << 3;
static const uint FLAG_USE_ROUGH_TEX = 1u << 4;
static const uint FLAG_USE_AO_TEX = 1u << 5;
static const uint FLAG_USE_EMISSIVE_TEX = 1u << 6;
static const uint FLAG_USE_ENV = 1u << 7;
static const uint FLAG_ENV_FLIP_Z = 1u << 8;
// When enabled, treat gEnv as an unfiltered dynamic cubemap: sample mip0 only.
// This avoids seams/garbage when the cubemap has multiple mips but only mip0 is rendered.
static const uint FLAG_ENV_FORCE_MIP0 = 1u << 9;

static const uint kMaxSpotShadows = 4;
static const uint kMaxPointShadows = 4;

// Light types (must match rendern::LightType in C++)
static const int LIGHT_DIR = 0;
static const int LIGHT_POINT = 1;
static const int LIGHT_SPOT = 2;

float4x4 LoadDirVP(ShadowDataSB sd, uint cascade)
{
    const uint base = cascade * 4u;
    return float4x4(
        sd.dirVPRows[base + 0u],
        sd.dirVPRows[base + 1u],
        sd.dirVPRows[base + 2u],
        sd.dirVPRows[base + 3u]
    );
}

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

static const float PI = 3.14159265359f;

float Pow5(float x)
{
    float x2 = x * x;
    return x2 * x2 * x;
}

float3 FresnelSchlick(float cosTheta, float3 F0)
{
    return F0 + (1.0f - F0) * Pow5(1.0f - saturate(cosTheta));
}

float DistributionGGX(float NdotH, float alpha)
{
    float a2 = alpha * alpha;
    float denom = (NdotH * NdotH) * (a2 - 1.0f) + 1.0f;
    return a2 / max(PI * denom * denom, 1e-6f);
}

float GeometrySchlickGGX(float NdotX, float k)
{
    return NdotX / max(NdotX * (1.0f - k) + k, 1e-6f);
}

float GeometrySmith(float NdotV, float NdotL, float roughness)
{
    float r = roughness + 1.0f;
    float k = (r * r) / 8.0f;
    float ggxV = GeometrySchlickGGX(NdotV, k);
    float ggxL = GeometrySchlickGGX(NdotL, k);
    return ggxV * ggxL;
}

float3 GetNormalMapped(float3 N, float3 worldPos, float2 uv)
{
    // Derivative-based TBN (no explicit tangents in the mesh).
    float3 dp1 = ddx(worldPos);
    float3 dp2 = ddy(worldPos);
    float2 duv1 = ddx(uv);
    float2 duv2 = ddy(uv);

    float3 T = dp1 * duv2.y - dp2 * duv1.y;
    float3 B = -dp1 * duv2.x + dp2 * duv1.x;

    T = normalize(T - N * dot(N, T));
    B = normalize(B - N * dot(N, B));

    float3 nTS = gNormal.Sample(gLinear, uv).xyz * 2.0f - 1.0f;
    float3x3 TBN = float3x3(T, B, N);
    return normalize(mul(nTS, TBN));
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
    float4 shadowPos : TEXCOORD3; // directional shadow clip (legacy / not used for CSM)
};

// Shadow sampling (generic 2D depth compare, used by spots and legacy dir)
float Shadow2D(Texture2D<float> shadowMap, float4 shadowClip, float biasTexels)
{
    float3 p = shadowClip.xyz / max(shadowClip.w, 1e-6f);

    // Only reject invalid depth. For XY we rely on BORDER addressing on the comparison sampler,
    // so PCF near the frustum edge fades naturally instead of producing a hard cut.
    if (p.z < 0.0f || p.z > 1.0f)
        return 1.0f;

    // NDC -> UV (note: flip Y)
    float2 uv = float2(p.x, -p.y) * 0.5f + 0.5f;

    uint w, h;
    shadowMap.GetDimensions(w, h);
    float2 texel = 1.0f / float2(max(w, 1u), max(h, 1u));

    float biasDepth = biasTexels * max(texel.x, texel.y);
    float z = p.z - biasDepth;

    // 3x3 PCF
    float s = 0.0f;
    [unroll]
    for (int y = -1; y <= 1; ++y)
    {
        [unroll]
        for (int x = -1; x <= 1; ++x)
        {
            s += shadowMap.SampleCmpLevelZero(gShadowCmp, uv + texel * float2(x, y), z);
        }
    }

    float shadow = s / 9.0f;

    // Edge guard-band: smoothly fade out the shadow near the shadow-map boundary to avoid a visible seam
    float edge = min(min(uv.x, uv.y), min(1.0f - uv.x, 1.0f - uv.y));
    float fade = saturate(edge / (2.0f * max(texel.x, texel.y))); // ~2 texels

    return lerp(1.0f, shadow, fade);
}

// Wrapper that matches the CPU call-site signature (shadowClip, materialBiasTexels, baseBiasTexels, slopeScaleTexels).
// NOTE: slopeScaleTexels is currently ignored here; kept for ABI stability with C++.
float Shadow2D(float4 shadowClip, float materialBiasTexels, float baseBiasTexels, float slopeScaleTexels)
{
    const float biasTexels = materialBiasTexels + baseBiasTexels;
    return Shadow2D(gDirShadow, shadowClip, biasTexels);
}

// ---------------- Directional CSM (atlas) ----------------
//
// Atlas layout: `dirCascadeCount` tiles laid out horizontally (no gaps).
// Tile size is constant (2048), atlas is 6144x2048 for 3 cascades.
//
// Key fixes / choices:
// - Cascade selection uses view-depth (dot with camera forward), not Euclidean distance,
//   otherwise cascade borders look "curved" / crooked.
// - PCF taps are clamped to the tile (branchless) to avoid cross-cascade bleeding.

float ShadowAtlasCSM(ShadowDataSB sd,
                     float4 clip,
                     uint cascade,
                     float biasTexels,
                     float kernelScale)
{
    float3 p = clip.xyz / max(clip.w, 1e-6f);

    // Only reject invalid depth. XY is handled with tile bounds below.
    if (p.z < 0.0f || p.z > 1.0f)
        return 1.0f;

    // NDC -> tile-local UV (flip Y)
    float2 uvLocal = float2(p.x, -p.y) * 0.5f + 0.5f;

    // Outside the cascade frustum => fully lit.
    if (uvLocal.x < 0.0f || uvLocal.x > 1.0f || uvLocal.y < 0.0f || uvLocal.y > 1.0f)
        return 1.0f;

    // dirInfo:
    //  x = 1/atlasW, y = 1/atlasH, z = 1/tileSize, w = cascadeCount
    const uint cascadeCount = (uint) sd.dirInfo.w;
    const float scaleX = 1.0f / max((float) cascadeCount, 1.0f);

    const float2 texelAtlas = float2(sd.dirInfo.x, sd.dirInfo.y); // 1/atlas dims
    const float invTile = sd.dirInfo.z; // 1/tileSize (for depth bias)

    // Convert the per-material bias expressed in "tile texels" into normalized depth units.
    const float z = p.z - (biasTexels * invTile);

    // Tile bounds in atlas UV.
    const float2 uvMin = float2((float) cascade * scaleX, 0.0f);
    const float2 uvMax = uvMin + float2(scaleX, 1.0f);

    // Map [0..1] tile-local UV into atlas UV.
    const float2 uv = uvMin + float2(uvLocal.x * scaleX, uvLocal.y);

    // PCF step (in atlas UV). kernelScale is in *tile texels*.
    const float2 stepUV = texelAtlas * kernelScale;

    // Guard band against cross-tile taps (scale with kernel).
    const float2 pad = stepUV * 1.5f;
    const float2 uvMinP = uvMin + pad;
    const float2 uvMaxP = uvMax - pad;

    float s = 0.0f;

    [unroll]
    for (int y = -1; y <= 1; ++y)
    {
        [unroll]
        for (int x = -1; x <= 1; ++x)
        {
            const float2 uvTap = uv + stepUV * float2((float) x, (float) y);

            // Branchless inside test (tile bounds, padded).
            const float inX = step(uvMinP.x, uvTap.x) * step(uvTap.x, uvMaxP.x);
            const float inY = step(uvMinP.y, uvTap.y) * step(uvTap.y, uvMaxP.y);
            const float inside = inX * inY;

            const float sample = gDirShadow.SampleCmpLevelZero(gShadowCmp, uvTap, z);
            s += lerp(1.0f, sample, inside);
        }
    }

    return s / 9.0f;
}

float DirShadowFactor(float3 worldPos, float NdotL,
                      float materialBiasTexels,
                      float baseBiasTexels,
                      float slopeScaleTexels)
{
    ShadowDataSB sd = gShadowData[0];

    const float3 camPos = uCameraAmbient.xyz;
    const float3 camF = uCameraForward.xyz; // must be normalized on CPU

    float distV = dot(worldPos - camPos, camF);
    distV = max(distV, 0.0f);

    // x=split1, y=split2, z=split3(max), w=fadeFraction
    const float split1 = sd.dirSplits.x;
    const float split2 = sd.dirSplits.y;
    const float split3 = sd.dirSplits.z;
    const float fadeFrac = sd.dirSplits.w;

    if (distV >= split3)
        return 1.0f;

    const float biasTexels = ComputeBiasTexels(
        NdotL, baseBiasTexels, slopeScaleTexels, materialBiasTexels, 0.0f);

    uint c = (distV < split1) ? 0u : ((distV < split2) ? 1u : 2u);

    // Slightly increase the filter radius for farther cascades.
    const float kernelScale = 1.0f + 0.5f * (float) c; // 1.0, 1.5, 2.0 (tile texels)

    float4x4 VP = LoadDirVP(sd, c);
    float4 clip = mul(float4(worldPos, 1.0f), VP);
    float s = ShadowAtlasCSM(sd, clip, c, biasTexels, kernelScale);

    // Cross-fade band near split plane.
    const float kMinBlend = 2.0f;
    const float f = saturate(fadeFrac);

    if (c == 0u)
    {
        const float prevSplit = 0.0f;
        const float nextSplit = split1;
        const float blend = max(kMinBlend, f * (nextSplit - prevSplit));

        const float t = saturate((distV - (nextSplit - blend)) / max(blend, 1e-3f));
        if (t > 0.0f)
        {
            float4x4 VP1 = LoadDirVP(sd, 1u);
            float4 clip1 = mul(float4(worldPos, 1.0f), VP1);
            const float s1 = ShadowAtlasCSM(sd, clip1, 1u, biasTexels, 1.5f);
            s = lerp(s, s1, t);
        }
    }
    else if (c == 1u)
    {
        const float prevSplit = split1;
        const float nextSplit = split2;
        const float blend = max(kMinBlend, f * (nextSplit - prevSplit));

        const float t = saturate((distV - (nextSplit - blend)) / max(blend, 1e-3f));
        if (t > 0.0f)
        {
            float4x4 VP2 = LoadDirVP(sd, 2u);
            float4 clip2 = mul(float4(worldPos, 1.0f), VP2);
            const float s2 = ShadowAtlasCSM(sd, clip2, 2u, biasTexels, 2.0f);
            s = lerp(s, s2, t);
        }
    }

    return s;
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

// ---------------- Point shadow sampling without TextureCube face selection ----------------
// We bind point shadows as Texture2DArray[6] (cubemap faces), and do explicit dir->face+UV mapping.

struct CubeFaceUV
{
    uint face; // 0..5 : +X,-X,+Y,-Y,+Z,-Z
    float2 uv; // [0..1]
};

CubeFaceUV CubeDirToFaceUV(float3 dir)
{
    float3 a = abs(dir);
    CubeFaceUV o;
    float2 st; // [-1..1], +Y up

    if (a.x >= a.y && a.x >= a.z)
    {
        float inv = 1.0f / max(a.x, 1e-6f);
        if (dir.x > 0.0f)
        {
            o.face = 0u; // +X
            // Inverse of DebugCubeAtlas DirFromFaceST(+X): dir=(1, st.y, -st.x)
            st = float2(-dir.z, dir.y) * inv;
        }
        else
        {
            o.face = 1u; // -X
            // Inverse of DebugCubeAtlas DirFromFaceST(-X): dir=(-1, st.y, st.x)
            st = float2(dir.z, dir.y) * inv;
        }
    }
    else if (a.y >= a.x && a.y >= a.z)
    {
        float inv = 1.0f / max(a.y, 1e-6f);
        if (dir.y > 0.0f)
        {
            o.face = 2u; // +Y
            // Inverse of DebugCubeAtlas DirFromFaceST(+Y): dir=(st.x, 1, -st.y)
            st = float2(dir.x, -dir.z) * inv;
        }
        else
        {
            o.face = 3u; // -Y
            // Inverse of DebugCubeAtlas DirFromFaceST(-Y): dir=(st.x, -1, st.y)
            st = float2(dir.x, dir.z) * inv;
        }
    }
    else
    {
        float inv = 1.0f / max(a.z, 1e-6f);
        if (dir.z > 0.0f)
        {
            o.face = 4u; // +Z
            // Inverse of DebugCubeAtlas DirFromFaceST(+Z): dir=(st.x, st.y, 1)
            st = float2(dir.x, dir.y) * inv;
        }
        else
        {
            o.face = 5u; // -Z
            // Inverse of DebugCubeAtlas DirFromFaceST(-Z): dir=(-st.x, st.y, -1)
            st = float2(-dir.x, dir.y) * inv;
        }
    }

    // st.y is +up; texture UV is +down in D3D. Also flip X to match FaceView() basis.
    o.uv = float2(-st.x, -st.y) * 0.5f + 0.5f;
    return o;
}

float SamplePointShadow(Texture2DArray<float> distArr, float3 dir)
{
    CubeFaceUV fu = CubeDirToFaceUV(dir);

    // If we are outside the face, treat as "far" (fully lit).
    if (fu.uv.x < 0.0f || fu.uv.x > 1.0f || fu.uv.y < 0.0f || fu.uv.y > 1.0f)
        return 1.0f;

    uint w, h, layers, mips;
    distArr.GetDimensions(0, w, h, layers, mips);

    // Map [0..1] -> [0..w-1], [0..h-1]
    int2 xy = int2(fu.uv * float2((float) w, (float) h));
    xy = clamp(xy, int2(0, 0), int2((int) w - 1, (int) h - 1));

    return distArr.Load(int4(xy, (int) fu.face, 0)).r;
}

float ShadowPoint(Texture2DArray<float> distArr,
                  float3 lightPos, float range,
                  float3 worldPos, float biasTexels)
{
    float3 v = worldPos - lightPos;
    float d = length(v);
    
    if (d >= range)
        return 1.0f;

    float3 dir = v / max(d, 1e-6f);

    float nd = saturate(d / max(range, 1e-3f));

    uint w, h, layers, levels;
    distArr.GetDimensions(0, w, h, layers, levels);

    const float invRes = 1.0f / float(max(w, h));
    const float biasNorm = biasTexels * invRes;
    const float compare = max(nd - biasNorm, 0.0f);
    
    const float stored = SamplePointShadow(distArr, dir);
    return (compare <= stored) ? 1.0f : 0.0f;
}

float ShadowPoint_SimpleSeam(Texture2DArray<float> distArr,
                             float3 lightPos, float range,
                             float3 worldPos,
                             float biasTexels)
{
    float3 v = worldPos - lightPos;
    float d = length(v);
    if (d >= range)
        return 1.0f;

    float3 dir = v / max(d, 1e-6f);
    float nd = d / max(range, 1e-6f);

    uint w, h, layers, mips;
    distArr.GetDimensions(0, w, h, layers, mips);

    const float invRes = 1.0f / float(max(w, h));
    const float biasNorm = biasTexels * invRes;

    float stored = SamplePointShadow(distArr, dir);

    const float compare = max(nd - biasNorm, 0.0f);

    return (compare <= stored) ? 1.0f : 0.0f;
}

// Wrapper matching CPU call-site signature: (slot, worldPos, materialBiasTexels, baseBiasTexels, slopeScaleTexels)
// NOTE: slopeScaleTexels is currently ignored here; kept for ABI stability with C++.
float SpotShadowFactor(uint slot, float3 worldPos, float NdotL, float materialBiasTexels, float baseBiasTexels, float slopeScaleTexels)
{
    if (slot >= 4)
        return 1.0f;
    ShadowDataSB sd = gShadowData[0];
    const float extraBiasTexels = sd.spotInfo[slot].z;
    const float biasTexels = ComputeBiasTexels(NdotL, baseBiasTexels, slopeScaleTexels, materialBiasTexels, extraBiasTexels);
    return SpotShadowFactor(slot, sd, worldPos, biasTexels);
}

float PointShadowFactor(uint slot, ShadowDataSB sd, float3 worldPos, float biasTexels)
{
    if (slot >= 4)
        return 1.0f;

    float3 lp = sd.pointPosRange[slot].xyz;
    float range = sd.pointPosRange[slot].w;

    if (slot == 0)
        return ShadowPoint_SimpleSeam(gPointShadow0, lp, range, worldPos, biasTexels);
    if (slot == 1)
        return ShadowPoint_SimpleSeam(gPointShadow1, lp, range, worldPos, biasTexels);
    if (slot == 2)
        return ShadowPoint_SimpleSeam(gPointShadow2, lp, range, worldPos, biasTexels);
    return ShadowPoint_SimpleSeam(gPointShadow3, lp, range, worldPos, biasTexels);
}

// Wrapper matching CPU call-site signature: (slot, worldPos, materialBiasTexels, baseBiasTexels, slopeScaleTexels)
// NOTE: slopeScaleTexels is currently ignored here; kept for ABI stability with C++.
float PointShadowFactor(uint slot, float3 worldPos, float NdotL, float materialBiasTexels, float baseBiasTexels, float slopeScaleTexels)
{
    if (slot >= 4)
        return 1.0f;
    ShadowDataSB sd = gShadowData[0];
    const float extraBiasTexels = sd.pointInfo[slot].z;
    const float biasTexels = ComputeBiasTexels(NdotL, baseBiasTexels, slopeScaleTexels, materialBiasTexels, extraBiasTexels);
    return PointShadowFactor(slot, sd, worldPos, biasTexels);
}

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

// Pixel Shader
float4 PSMain(VSOut IN) : SV_Target0
{
    const uint flags = asuint(uMaterialFlags.w);

    const bool useTex = (flags & FLAG_USE_TEX) != 0;
    const bool useShadow = (flags & FLAG_USE_SHADOW) != 0;
    const bool useNormal = (flags & FLAG_USE_NORMAL) != 0;
    const bool useMetalTex = (flags & FLAG_USE_METAL_TEX) != 0;
    const bool useRoughTex = (flags & FLAG_USE_ROUGH_TEX) != 0;
    const bool useAOTex = (flags & FLAG_USE_AO_TEX) != 0;
    const bool useEmissiveTex = (flags & FLAG_USE_EMISSIVE_TEX) != 0;
    const bool useEnv = (flags & FLAG_USE_ENV) != 0;
	const bool envFlipZ = (flags & FLAG_ENV_FLIP_Z) != 0;
	const bool envForceMip0 = (flags & FLAG_ENV_FORCE_MIP0) != 0;

    float3 baseColor = uBaseColor.rgb;
    float alphaOut = uBaseColor.a;

    if (useTex)
    {
        const float4 tex = gAlbedo.Sample(gLinear, IN.uv);
        baseColor *= tex.rgb;
        alphaOut *= tex.a;
    }

    float metallic = saturate(uPbrParams.x);
    float roughness = saturate(uPbrParams.y);
    float ao = saturate(uPbrParams.z);
    const float emissiveStrength = max(uPbrParams.w, 0.0f);

    if (useMetalTex)
        metallic *= gMetalness.Sample(gLinear, IN.uv).r;
    if (useRoughTex)
        roughness *= gRoughness.Sample(gLinear, IN.uv).r;
    if (useAOTex)
        ao *= gAO.Sample(gLinear, IN.uv).r;

    roughness = clamp(roughness, 0.04f, 1.0f);

    float3 N = normalize(IN.nrmW);
    if (useNormal)
    {
        N = GetNormalMapped(N, IN.worldPos, IN.uv);
    }

    const float3 V = normalize(uCameraAmbient.xyz - IN.worldPos);
    const float NdotV = max(dot(N, V), 0.0f);

    // Fresnel reflectance at normal incidence
    const float3 F0 = lerp(float3(0.04f, 0.04f, 0.04f), baseColor, metallic);

    float3 Lo = 0.0f;

    const int lightCount = (int) uCounts.x;
    const int spotShadowCount = (int) uCounts.y;
    const int pointShadowCount = (int) uCounts.z;

    [loop]
    for (int i = 0; i < lightCount; ++i)
    {
        const GPULight Ld = gLights[i];
        const int type = (int) Ld.p0.w;

        float3 L = 0.0f;
        float attenuation = 1.0f;
        float shadowFactor = 1.0f;

        // Build L and attenuation first.
        if (type == LIGHT_DIR)
        {
            L = normalize(-Ld.p1.xyz);
        }
        else if (type == LIGHT_POINT)
        {
            const float3 toLight = Ld.p0.xyz - IN.worldPos;
            const float dist = length(toLight);
            // Soft range cutoff (prevents a hard circle boundary).
            const float range = max(Ld.p2.w, 1e-3f);
            const float fade = max(range * 0.10f, 0.05f);
            float rangeFade = saturate((range - dist) / fade);
            rangeFade = rangeFade * rangeFade;
            if (rangeFade <= 0.0f)
                continue;

            L = toLight / max(dist, 1e-6f);

            const float attLin = Ld.p3.z;
            const float attQuad = Ld.p3.w;
            attenuation = rangeFade / max(1.0f + attLin * dist + attQuad * dist * dist, 1e-6f);
        }
        else if (type == LIGHT_SPOT)
        {
            const float3 toLight = Ld.p0.xyz - IN.worldPos;
            const float dist = length(toLight);
            // Soft range cutoff (prevents a hard circle boundary).
            const float range = max(Ld.p2.w, 1e-3f);
            const float fade = max(range * 0.10f, 0.05f);
            float rangeFade = saturate((range - dist) / fade);
            rangeFade = rangeFade * rangeFade;
            if (rangeFade <= 0.0f)
                continue;

            L = toLight / max(dist, 1e-6f);

            // Angular falloff
            const float3 spotDir = normalize(Ld.p1.xyz); // FROM light
            const float cosTheta = dot(-L, spotDir);
            const float cosInner = Ld.p3.x;
            const float cosOuter = Ld.p3.y;
            const float spotT = saturate((cosTheta - cosOuter) / max(cosInner - cosOuter, 1e-5f));
            const float spotAtt = SmoothStep01(spotT);

            const float attLin = Ld.p3.z;
            const float attQuad = Ld.p3.w;
            attenuation = (spotAtt * rangeFade) / max(1.0f + attLin * dist + attQuad * dist * dist, 1e-6f);
        }
        else
        {
            continue;
        }

        // Now NdotL is valid.
        const float NdotL = saturate(dot(N, L));
        if (NdotL <= 0.0f)
            continue;

        // Shadows (dir uses NdotL for bias).
        if (useShadow)
        {
            if (type == LIGHT_DIR)
            {
                shadowFactor = DirShadowFactor(IN.worldPos, NdotL, uMaterialFlags.z, uShadowBias.x, uShadowBias.w);
            }
            else if (type == LIGHT_POINT)
            {
                int slot = FindPointShadowSlot((uint) i, (uint) pointShadowCount);
                if (slot >= 0)
                    shadowFactor = PointShadowFactor((uint) slot, IN.worldPos, NdotL, uMaterialFlags.z, uShadowBias.z, uShadowBias.w);
            }
            else // SPOT
            {
                int slot = FindSpotShadowSlot((uint) i, (uint) spotShadowCount);
                if (slot >= 0)
                    shadowFactor = SpotShadowFactor((uint) slot, IN.worldPos, NdotL, uMaterialFlags.z, uShadowBias.y, uShadowBias.w);
            }
        }

        const float3 H = normalize(V + L);
        const float NdotH = saturate(dot(N, H));
        const float VdotH = saturate(dot(V, H));

        const float alphaR = roughness * roughness;
        const float D = DistributionGGX(NdotH, alphaR);
        const float G = GeometrySmith(NdotV, NdotL, roughness);
        const float3 F = FresnelSchlick(VdotH, F0);

        const float3 numerator = D * G * F;
        const float denom = max(4.0f * NdotV * NdotL, 1e-6f);
        const float3 specular = numerator / denom;

        const float3 kS = F;
        const float3 kD = (1.0f - kS) * (1.0f - metallic);
        const float3 diffuse = kD * baseColor / PI;

        const float3 radiance = Ld.p2.xyz * (Ld.p1.w * attenuation);

        Lo += (diffuse + specular) * radiance * NdotL * shadowFactor;
    }

    // Ambient / IBL
    float3 ambient = 0.0f;
    if (useEnv)
    {
       // Compute mipMax from actual env mip levels (important for dynamic cubemaps that may have 1 mip).
        uint w, h, levels;
        gEnv.GetDimensions(0, w, h, levels);
        float mipMax = (levels > 0u) ? (float) (levels - 1u) : 0.0f;
        
        // Dynamic reflection captures typically render only mip0. If the texture was created with
        // a full mip chain, sampling higher mips will read uninitialized data and show face seams.
		if (envForceMip0)
			mipMax = 0.0f;

        const float3 R = reflect(-V, N);
        
        // NOTE: Skybox cubemaps may use a different convention (see Skybox_dx12.hlsl which flips Z when sampling).
        // To keep IBL consistent with the visible skybox, optionally flip Z for env sampling.
		float3 envN = N;
		float3 envR = R;
		if (envFlipZ)
		{
			envN.z = -envN.z;
			envR.z = -envR.z;
		}

        // For dynamic captures (mip0 only), keep both diffuse and specular on mip0.
		const float lodDiffuse = envForceMip0 ? 0.0f : mipMax;
		const float lodSpec = envForceMip0 ? 0.0f : (roughness * mipMax);
        
        // "Diffuse irradiance" approximation: very blurry env sample (or mip0 for dynamic captures).
        const float3 envDiffuse = gEnv.SampleLevel(gLinearClamp, envN, lodDiffuse).rgb;
        
        // Specular IBL approximation: env prefiltered by mip level (or mip0 for dynamic captures).
        const float3 envSpec = gEnv.SampleLevel(gLinearClamp, envR, lodSpec).rgb;

        const float3 F = FresnelSchlick(NdotV, F0);
        const float3 kS = F;
        const float3 kD = (1.0f - kS) * (1.0f - metallic);

        ambient = (kD * baseColor * envDiffuse) + (envSpec * kS);
        ambient *= (ao * uCameraAmbient.w);
    }
    else
    {
        ambient = baseColor * (ao * uCameraAmbient.w);
    }

    float3 emissive = 0.0f;
    if (useEmissiveTex)
    {
        emissive = gEmissive.Sample(gLinear, IN.uv).rgb * emissiveStrength;
    }

    const float3 color = ambient + Lo + emissive;
    return float4(color, saturate(alphaOut));
}
