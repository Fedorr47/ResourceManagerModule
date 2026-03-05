// DeferredLighting_dx12.hlsl
// Fullscreen deferred lighting resolve.

SamplerState gLinear : register(s0);
SamplerComparisonState gShadowCmp : register(s1);
SamplerState gPointClamp : register(s2);
SamplerState gLinearClamp : register(s3);

// -----------------------------------------------------------------------------
// GBuffer + depth
// -----------------------------------------------------------------------------
Texture2D gGBuffer0 : register(t0); // albedo.rgb, roughness
Texture2D gGBuffer1 : register(t1); // normal.xyz (encoded), metalness
Texture2D gGBuffer2 : register(t2); // emissive.rgb, ao
Texture2D<float> gDepth : register(t3); // depth SRV (0..1)
Texture2D gGBuffer3 : register(t4); // env selector: r=envSource, g=probeIdxN (reserved)

// -----------------------------------------------------------------------------
// Shadows
// -----------------------------------------------------------------------------

// Directional CSM atlas (D32 atlas; cascades packed horizontally)
Texture2D<float> gDirShadow : register(t5);

struct ShadowDataSB
{
	float4 dirVPRows[12];
	float4 dirSplits; // { split1, split2, split3(max), fadeFrac }
	float4 dirInfo; // { invAtlasW, invAtlasH, invTileRes, cascadeCount }

	float4 spotVPRows[16];
	float4 spotInfo[4];

	float4 pointPosRange[4];
	float4 pointInfo[4];
};

StructuredBuffer<ShadowDataSB> gShadowData : register(t6);

// Spot shadow maps (depth) - NO ARRAYS (root sig uses 1-descriptor tables per tN)
Texture2D<float> gSpotShadow0 : register(t7);
Texture2D<float> gSpotShadow1 : register(t8);
Texture2D<float> gSpotShadow2 : register(t9);
Texture2D<float> gSpotShadow3 : register(t10);

// Point distance cubemaps (normalized distance) as Texture2DArray[6] (cubemap faces)
Texture2DArray<float> gPointShadow0 : register(t11);
Texture2DArray<float> gPointShadow1 : register(t12);
Texture2DArray<float> gPointShadow2 : register(t13);
Texture2DArray<float> gPointShadow3 : register(t14);

// -----------------------------------------------------------------------------
// Environment + SSAO
// -----------------------------------------------------------------------------
TextureCube gSkyboxEnv : register(t15); // global skybox cubemap
TextureCube gReflEnv : register(t17); // selected reflection capture cubemap (renderer-owned)
Texture2D<float> gSSAO : register(t18); // SSAO (0..1)

// -----------------------------------------------------------------------------
// Lights
// -----------------------------------------------------------------------------
struct GPULight
{
	float4 p0; // pos.xyz, type
	float4 p1; // dir.xyz (FROM light), intensity
	float4 p2; // color.rgb, range
	float4 p3; // cosInner, cosOuter, attLin, attQuad
};

StructuredBuffer<GPULight> gLights : register(t16);

// -----------------------------------------------------------------------------
// Constants
// -----------------------------------------------------------------------------
cbuffer Deferred : register(b0)
{
	float4x4 uInvViewProj;
	float4 uCameraPosAmbient; // xyz + ambientStrength
	float4 uCameraForward; // xyz + pad
	float4 uShadowBias; // x=dirBaseBiasTexels, y=slopeScaleTexels, z/w unused here
	float4 uCounts; // x = lightCount, y = spotShadowCount, z = pointShadowCount, w unused
};

// -----------------------------------------------------------------------------
// Fullscreen triangle
// -----------------------------------------------------------------------------
struct VSOut
{
	float4 svPos : SV_POSITION;
	float2 uv : TEXCOORD0;
};

VSOut VS_Fullscreen(uint vid : SV_VertexID)
{

	float2 pos = (vid == 0) ? float2(-1.0, -1.0) : (vid == 1) ? float2(-1.0, 3.0) : float2(3.0, -1.0);
	// IMPORTANT: produce "texture-space" UV where (0,0) is TOP-left for Texture2D sampling.
	// This matches CopyToSwapChain_dx12.hlsl and fixes the upside-down deferred output.
	float2 uv = float2((pos.x + 1.0f) * 0.5f, 1.0f - (pos.y + 1.0f) * 0.5f);
	VSOut o;
     o.svPos = float4(pos, 0.0, 1.0);
     o.uv = uv;
     return o;
}

// -----------------------------------------------------------------------------
// BRDF helpers (direct lighting)
// -----------------------------------------------------------------------------
static const float kPI = 3.14159265f;

float3 FresnelSchlick(float cosTheta, float3 F0)
{
	return F0 + (1.0f - F0) * pow(1.0f - cosTheta, 5.0f);
}

float DistributionGGX(float NdotH, float roughness)
{
	float a = max(roughness, 0.04f);
	float a2 = a * a;
	float d = (NdotH * NdotH) * (a2 - 1.0f) + 1.0f;
	return a2 / max(kPI * d * d, 1e-6f);
}

float GeometrySchlickGGX(float NdotV, float roughness)
{
	float r = roughness + 1.0f;
	float k = (r * r) / 8.0f;
	return NdotV / max(NdotV * (1.0f - k) + k, 1e-6f);
}

float GeometrySmith(float NdotV, float NdotL, float roughness)
{
	return GeometrySchlickGGX(NdotV, roughness) * GeometrySchlickGGX(NdotL, roughness);
}

float3 BRDF_CookTorrance(float3 N, float3 V, float3 L, float3 albedo, float metallic, float roughness)
{
	float3 H = normalize(V + L);

	float NdotV = saturate(dot(N, V));
	float NdotL = saturate(dot(N, L));
	float NdotH = saturate(dot(N, H));
	float VdotH = saturate(dot(V, H));

	float3 F0 = lerp(float3(0.04f, 0.04f, 0.04f), albedo, metallic);
	float3 F = FresnelSchlick(VdotH, F0);

	float D = DistributionGGX(NdotH, roughness);
	float G = GeometrySmith(NdotV, NdotL, roughness);

	float3 spec = (D * G) * F / max(4.0f * NdotV * NdotL, 1e-6f);

	float3 kS = F;
	float3 kD = (1.0f - kS) * (1.0f - metallic);
	float3 diff = kD * albedo / kPI;

	return diff + spec;
}

// -----------------------------------------------------------------------------
// Matrix rows helper
// -----------------------------------------------------------------------------
float4 MulRows(float4 r0, float4 r1, float4 r2, float4 r3, float4 v)
{
	return float4(dot(r0, v), dot(r1, v), dot(r2, v), dot(r3, v));
}

// -----------------------------------------------------------------------------
// Directional CSM shadow (3x3 PCF)
// -----------------------------------------------------------------------------
uint SelectDirCascade(float viewDist, float4 splits, uint cascadeCount)
{
    // splits = { split1, split2, split3(max), fadeFrac }
	if (cascadeCount <= 1u)
		return 0u;
	if (cascadeCount == 2u)
		return (viewDist < splits.x) ? 0u : 1u;
    // 3 cascades
	return (viewDist < splits.x) ? 0u : (viewDist < splits.y) ? 1u : 2u;
}

float SampleDirShadowPCF3x3(ShadowDataSB sd, float3 worldPos, float NdotL, float viewDist)
{
	const uint cascadeCount = (uint) sd.dirInfo.w;
	if (cascadeCount == 0u)
		return 1.0f;

    // If beyond shadow distance, skip.
	const float shadowMax = sd.dirSplits.z;
	if (viewDist >= shadowMax)
		return 1.0f;

	const uint c = SelectDirCascade(viewDist, sd.dirSplits, cascadeCount);

	const uint base = c * 4u;
	const float4 r0 = sd.dirVPRows[base + 0u];
	const float4 r1 = sd.dirVPRows[base + 1u];
	const float4 r2 = sd.dirVPRows[base + 2u];
	const float4 r3 = sd.dirVPRows[base + 3u];

	const float4 p = float4(worldPos, 1.0f);
	const float4 clip = MulRows(r0, r1, r2, r3, p);

	if (abs(clip.w) <= 1e-6f)
		return 1.0f;

	const float3 ndc = clip.xyz / clip.w; // RH_ZO -> z in [0..1]
	if (ndc.z <= 0.0f || ndc.z >= 1.0f)
		return 1.0f;

    // Tile UV (flip Y for texture space).
	float2 tileUV = ndc.xy * float2(0.5f, -0.5f) + 0.5f;
	if (tileUV.x <= 0.0f || tileUV.x >= 1.0f || tileUV.y <= 0.0f || tileUV.y >= 1.0f)
		return 1.0f;

	const float fadeFrac = saturate(sd.dirSplits.w);

    // Bias in "texel-ish" units converted by invTileRes.
	const float baseBiasTexels = uShadowBias.x;
	const float slopeScaleTexels = uShadowBias.y;
	const float invTileRes = sd.dirInfo.z;

	const float bias = (baseBiasTexels + slopeScaleTexels * (1.0f - saturate(NdotL))) * invTileRes;
	const float cmpDepth = ndc.z - bias;

    // Prevent PCF bleeding between cascades by clamping inside the tile.
	const float margin = 1.5f * invTileRes;
	tileUV = clamp(tileUV, margin, 1.0f - margin);

	const float invCascadeCount = 1.0f / (float) cascadeCount;

	float sum = 0.0f;
    [unroll]
	for (int dy = -1; dy <= 1; ++dy)
	{
        [unroll]
		for (int dx = -1; dx <= 1; ++dx)
		{
			float2 tuv = tileUV + float2((float) dx, (float) dy) * invTileRes;
			tuv = clamp(tuv, margin, 1.0f - margin);

			float2 atlasUV;
			atlasUV.x = (tuv.x + (float) c) * invCascadeCount;
			atlasUV.y = tuv.y;

			sum += gDirShadow.SampleCmpLevelZero(gShadowCmp, atlasUV, cmpDepth);
		}
	}

	float shadow = sum / 9.0f;

    // Fade shadows near max distance to reduce popping.
	if (fadeFrac > 0.0f)
	{
		const float fadeStart = shadowMax * (1.0f - fadeFrac);
		if (viewDist > fadeStart)
		{
			const float t = saturate((viewDist - fadeStart) / max(shadowMax - fadeStart, 1e-6f));
			shadow = lerp(shadow, 1.0f, t);
		}
	}

	return shadow;
}

// -----------------------------------------------------------------------------
// World position reconstruction
// -----------------------------------------------------------------------------
float3 ReconstructWorldPos(float2 uv, float depth)
{
    // uv is in TEXTURE space (0,0 top-left). For NDC we need Y-up, so flip Y.
    float4 clip;
    clip.x = uv.x * 2.0f - 1.0f;
    clip.y = 1.0f - uv.y * 2.0f;
    clip.z = depth;
    clip.w = 1.0f;
    float4 worldH = mul(clip, uInvViewProj);
    return worldH.xyz / max(worldH.w, 1e-6f);
}

// -----------------------------------------------------------------------------
// IBL helpers (v1) - works with either skybox or reflection capture cubemap
// -----------------------------------------------------------------------------
float3 FresnelSchlickRoughness(float cosTheta, float3 F0, float roughness)
{
    // Better grazing behavior for rough surfaces.
    const float oneMinusR = 1.0f - roughness;
    const float3 oneMinusR3 = float3(oneMinusR, oneMinusR, oneMinusR);
    return F0 + (max(oneMinusR3, F0) - F0) * pow(1.0f - cosTheta, 5.0f);
}

float3 SampleEnvPrefiltered(TextureCube env, float3 dir, float roughness)
{
	uint w = 0, h = 0, mips = 1;
	env.GetDimensions(0, w, h, mips);
	float lod = saturate(roughness) * max(0.0f, (float) (mips - 1));
	return env.SampleLevel(gLinearClamp, dir, lod).rgb;
}

float3 SampleEnvDiffuseIrradiance(TextureCube env, float3 dir)
{
	uint w = 0, h = 0, mips = 1;
	env.GetDimensions(0, w, h, mips);
	float lod = max(0.0f, (float) (mips - 1));
	return env.SampleLevel(gLinearClamp, dir, lod).rgb;
}

float3 SampleEnvPrefilteredSel(bool useRefl, float3 dir, float roughness)
{
	return useRefl ? SampleEnvPrefiltered(gReflEnv, dir, roughness)
                   : SampleEnvPrefiltered(gSkyboxEnv, dir, roughness);
}

float3 SampleEnvDiffuseSel(bool useRefl, float3 dir)
{
	return useRefl ? SampleEnvDiffuseIrradiance(gReflEnv, dir)
                   : SampleEnvDiffuseIrradiance(gSkyboxEnv, dir);
}

// -----------------------------------------------------------------------------
// Shared shadow helpers (spot + point)
// -----------------------------------------------------------------------------
static const uint kMaxSpotShadows = 4u;
static const uint kMaxPointShadows = 4u;

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

// Generic 2D depth compare with 3x3 PCF (used by spot shadows).
float Shadow2D(Texture2D<float> shadowMap, float4 shadowClip, float biasTexels)
{
	float3 p = shadowClip.xyz / max(shadowClip.w, 1e-6f);

	if (p.z < 0.0f || p.z > 1.0f)
		return 1.0f;

    // NDC -> UV (flip Y)
	float2 uv = float2(p.x, -p.y) * 0.5f + 0.5f;

	uint w, h;
	shadowMap.GetDimensions(w, h);
	float2 texel = 1.0f / float2(max(w, 1u), max(h, 1u));

	float biasDepth = biasTexels * max(texel.x, texel.y);
	float z = p.z - biasDepth;

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

    // Fade near edges to avoid hard cut (requires BORDER on gShadowCmp)
	float edge = min(min(uv.x, uv.y), min(1.0f - uv.x, 1.0f - uv.y));
	float fade = saturate(edge / (2.0f * max(texel.x, texel.y)));
	return lerp(1.0f, shadow, fade);
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

float SpotShadowFactor(uint slot, ShadowDataSB sd, float3 worldPos, float biasTexels)
{
	if (slot >= 4u)
		return 1.0f;

	float4 r0 = sd.spotVPRows[slot * 4u + 0u];
	float4 r1 = sd.spotVPRows[slot * 4u + 1u];
	float4 r2 = sd.spotVPRows[slot * 4u + 2u];
	float4 r3 = sd.spotVPRows[slot * 4u + 3u];
	float4 clip = MulRows(r0, r1, r2, r3, float4(worldPos, 1.0f));

	if (slot == 0u)
		return Shadow2D(gSpotShadow0, clip, biasTexels);
	if (slot == 1u)
		return Shadow2D(gSpotShadow1, clip, biasTexels);
	if (slot == 2u)
		return Shadow2D(gSpotShadow2, clip, biasTexels);
	return Shadow2D(gSpotShadow3, clip, biasTexels);
}

struct CubeFaceUV
{
	uint face; // 0..5 : +X,-X,+Y,-Y,+Z,-Z
	float2 uv; // [0..1]
};

CubeFaceUV CubeDirToFaceUV(float3 dir)
{
	float3 a = abs(dir);
	CubeFaceUV o;
	float2 st;

	if (a.x >= a.y && a.x >= a.z)
	{
		float inv = 1.0f / max(a.x, 1e-6f);
		if (dir.x > 0.0f)
		{
			o.face = 0u; // +X
			st = float2(-dir.z, dir.y) * inv;
		}
		else
		{
			o.face = 1u; // -X
			st = float2(dir.z, dir.y) * inv;
		}
	}
	else if (a.y >= a.x && a.y >= a.z)
	{
		float inv = 1.0f / max(a.y, 1e-6f);
		if (dir.y > 0.0f)
		{
			o.face = 2u; // +Y
			st = float2(dir.x, -dir.z) * inv;
		}
		else
		{
			o.face = 3u; // -Y
			st = float2(dir.x, dir.z) * inv;
		}
	}
	else
	{
		float inv = 1.0f / max(a.z, 1e-6f);
		if (dir.z > 0.0f)
		{
			o.face = 4u; // +Z
			st = float2(dir.x, dir.y) * inv;
		}
		else
		{
			o.face = 5u; // -Z
			st = float2(-dir.x, dir.y) * inv;
		}
	}

    // [-1..1] -> [0..1]
	o.uv = st * 0.5f + 0.5f;
	return o;
}

// Simple point shadow: sample normalized distance cubemap (2DArray[6]) with a seam-fade.
// Stored shadow is (distance / range) in [0..1], with 1 meaning "far" (no occluder).
float ShadowPoint_SimpleSeam(Texture2DArray<float> shadowCube,
                            float3 lightPos,
                            float range,
                            float3 worldPos,
                            float biasTexels)
{
	float3 toP = worldPos - lightPos;
	float dist = length(toP);
	if (dist <= 1e-6f)
		return 1.0f;

	float3 dir = toP / dist;
	CubeFaceUV fu = CubeDirToFaceUV(dir);

	uint w, h, layers;
	shadowCube.GetDimensions(w, h, layers);
	float2 texel = 1.0f / float2(max(w, 1u), max(h, 1u));

	float bias = biasTexels * max(texel.x, texel.y);
	float d = saturate(dist / max(range, 1e-6f));
	float z = d - bias;

	float stored = shadowCube.SampleLevel(gPointClamp, float3(fu.uv, fu.face), 0.0f);
	float shadow = (z <= stored) ? 1.0f : 0.0f;

    // Fade near edges of each face to reduce seams.
	float edge = min(min(fu.uv.x, fu.uv.y), min(1.0f - fu.uv.x, 1.0f - fu.uv.y));
	float fade = saturate(edge / (2.0f * max(texel.x, texel.y)));
	return lerp(1.0f, shadow, fade);
}

float PointShadowFactor(uint slot, ShadowDataSB sd, float3 worldPos, float biasTexels)
{
	if (slot >= 4u)
		return 1.0f;

	float3 lp = sd.pointPosRange[slot].xyz;
	float range = sd.pointPosRange[slot].w;

	if (slot == 0u)
		return ShadowPoint_SimpleSeam(gPointShadow0, lp, range, worldPos, biasTexels);
	if (slot == 1u)
		return ShadowPoint_SimpleSeam(gPointShadow1, lp, range, worldPos, biasTexels);
	if (slot == 2u)
		return ShadowPoint_SimpleSeam(gPointShadow2, lp, range, worldPos, biasTexels);
	return ShadowPoint_SimpleSeam(gPointShadow3, lp, range, worldPos, biasTexels);
}

// -----------------------------------------------------------------------------
// Pixel shader
// -----------------------------------------------------------------------------
float4 PS_DeferredLighting(VSOut IN) : SV_Target0
{
    // Use point sampling to avoid gbuffer filtering across edges.
	float4 g0 = gGBuffer0.Sample(gPointClamp, IN.uv);
	float4 g1 = gGBuffer1.Sample(gPointClamp, IN.uv);
	float4 g2 = gGBuffer2.Sample(gPointClamp, IN.uv);

	float3 albedo = g0.rgb;
	float roughness = saturate(g0.a);

	float3 N = normalize(g1.rgb * 2.0f - 1.0f);
	float metallic = saturate(g1.a);

	float3 emissive = g2.rgb;
	float ao = saturate(g2.a);

    // SSAO multiply (expects 0..1)
	ao *= saturate(gSSAO.Sample(gPointClamp, IN.uv).r);

    // Env selector from GBuffer3
	const float2 envSel = gGBuffer3.SampleLevel(gPointClamp, IN.uv, 0).rg; // r=envSource

	float depth = gDepth.Sample(gPointClamp, IN.uv).r;

    // If depth is 1.0 (no geometry), output black; skybox is rendered later.
	if (depth >= 0.999999f)
	{
		return float4(0.0f, 0.0f, 0.0f, 1.0f);
	}

	float3 worldPos = ReconstructWorldPos(IN.uv, depth);

	float3 camPos = uCameraPosAmbient.xyz;
	float ambientStrength = uCameraPosAmbient.w;

	float3 V = normalize(camPos - worldPos);

    // Shadow metadata (CSM atlas) is in element 0.
	ShadowDataSB sd = gShadowData[0];
	const float viewDist = max(0.0f, dot(worldPos - camPos, uCameraForward.xyz));

	const uint lightCount = (uint) uCounts.x;

	float3 Lo = 0.0f;

    [loop]
	for (uint i = 0; i < lightCount; ++i)
	{
		GPULight l = gLights[i];
		const uint type = (uint) l.p0.w;

		float3 radiance = l.p2.rgb * l.p1.w;
		float3 L;
		float att = 1.0f;
		float shadow = 1.0f;

		if (type == 0u) // Directional
		{
            // l.p1.xyz is direction FROM light towards the scene.
			L = normalize(-l.p1.xyz);
		}
		else
		{
			float3 toLight = l.p0.xyz - worldPos;
			float dist = length(toLight);
			if (dist <= 1e-6f)
				continue;

			L = toLight / dist;

            // Range falloff
			float range = max(l.p2.w, 0.001f);
			float rangeAtt = saturate(1.0f - dist / range);
			rangeAtt *= rangeAtt;

            // Attenuation (classic)
			float attLin = l.p3.z;
			float attQuad = l.p3.w;
			att = rangeAtt / max(1.0f + attLin * dist + attQuad * dist * dist, 1e-6f);

			if (type == 2u) // Spot
			{
				float3 dirFromLight = normalize(l.p1.xyz);
				float3 dirToPoint = normalize(worldPos - l.p0.xyz);
				float cd = dot(dirToPoint, dirFromLight);

				float cosInner = l.p3.x;
				float cosOuter = l.p3.y;

                // Smoothstep between outer and inner cones.
				float t = saturate((cd - cosOuter) / max(cosInner - cosOuter, 1e-6f));
				float spot = t * t * (3.0f - 2.0f * t);
				att *= spot;
			}
		}

		float NdotL = saturate(dot(N, L));
		if (NdotL > 0.0f)
		{
			if (type == 0u)
			{
				shadow = SampleDirShadowPCF3x3(sd, worldPos, NdotL, viewDist);
			}
			else if (type == 2u) // Spot
			{
				const uint spotCount = (uint) uCounts.y;
				const int slot = FindSpotShadowSlot(i, spotCount);
				if (slot >= 0)
				{
					const float extraBias = sd.spotInfo[(uint) slot].y;
					const float biasTexels = ComputeBiasTexels(NdotL, uShadowBias.x, uShadowBias.y, 0.0f, extraBias);
					shadow = SpotShadowFactor((uint) slot, sd, worldPos, biasTexels);
				}
			}
			else if (type == 1u) // Point
			{
				const uint pointCount = (uint) uCounts.z;
				const int slot = FindPointShadowSlot(i, pointCount);
				if (slot >= 0)
				{
					const float extraBias = sd.pointInfo[(uint) slot].y;
					const float biasTexels = ComputeBiasTexels(NdotL, uShadowBias.x, uShadowBias.y, 0.0f, extraBias);
					shadow = PointShadowFactor((uint) slot, sd, worldPos, biasTexels);
				}
			}

			float3 brdf = BRDF_CookTorrance(N, V, L, albedo, metallic, roughness);
			Lo += brdf * radiance * (NdotL * att * shadow);
		}
	}

    // -------------------------------------------------------------------------
    // Indirect lighting (IBL v1)
    // -------------------------------------------------------------------------
	const bool useRefl = (envSel.x > 0.5f);

	float3 F0 = lerp(float3(0.04f, 0.04f, 0.04f), albedo, metallic);

	float NdotV = saturate(dot(N, V));
	float3 F = FresnelSchlickRoughness(NdotV, F0, roughness);

	float3 kS = F;
	float3 kD = (1.0f - kS) * (1.0f - metallic);

	float3 R = normalize(reflect(-V, N));

	float3 envDiffuse = SampleEnvDiffuseSel(useRefl, N);
	float3 envSpec = SampleEnvPrefilteredSel(useRefl, R, roughness);

    // AO modulates only the diffuse/ambient part.
	float3 indirect = (kD * (envDiffuse * albedo) * ao + envSpec * F) * ambientStrength;

	float3 color = indirect + Lo + emissive;
	return float4(color, 1.0f);
}