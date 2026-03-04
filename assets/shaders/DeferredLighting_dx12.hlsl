SamplerState gLinearClamp : register(s3);

Texture2D gGBuffer0 : register(t0);
Texture2D gGBuffer1 : register(t1);
struct GPULight
{
	float4 p0; // pos.xyz, type
	float4 p1; // dir.xyz, intensity
	float4 p2; // color.rgb, range
	float4 p3; // cosInner, cosOuter, attLin, attQuad
};
StructuredBuffer<GPULight> gLights : register(t2);

Texture2D<float> gDepth : register(t3);

cbuffer DeferredCB : register(b0)
{
	float4x4 uInvViewProj;
	float4 uCameraPosAmbient; // xyz + ambientStrength
	float4 uCounts; // x = lightCount
};

static const int LIGHT_DIR = 0;
static const int LIGHT_POINT = 1;
static const int LIGHT_SPOT = 2;

static const float PI = 3.14159265359f;

float Pow5(float x)
{
	float x2 = x * x;
	return x2 * x2 * x;
}

float3 FresnelSchlick(float cosTheta, float3 F0)
{
	return F0 + (1.0f - F0) * Pow5(1.0f - cosTheta);
}

float DistributionGGX(float NdotH, float a)
{
	float a2 = a * a;
	float denom = (NdotH * NdotH) * (a2 - 1.0f) + 1.0f;
	return a2 / max(PI * denom * denom, 1e-6f);
}

float GeometrySchlickGGX(float NdotV, float k)
{
	return NdotV / max(NdotV * (1.0f - k) + k, 1e-6f);
}

float GeometrySmith(float NdotV, float NdotL, float roughness)
{
	float r = roughness + 1.0f;
	float k = (r * r) / 8.0f;
	float ggx1 = GeometrySchlickGGX(NdotV, k);
	float ggx2 = GeometrySchlickGGX(NdotL, k);
	return ggx1 * ggx2;
}

struct VSOut
{
	float4 posH : SV_Position;
	float2 uv : TEXCOORD0;
};

VSOut VS_Fullscreen(uint vid : SV_VertexID)
{
	VSOut o;
	// Fullscreen triangle
	float2 p = (vid == 0) ? float2(-1, -1) : (vid == 1) ? float2(-1, 3) : float2(3, -1);
	float2 u = (vid == 0) ? float2(0, 1) : (vid == 1) ? float2(0, -1) : float2(2, 1);
	o.posH = float4(p, 0, 1);
	o.uv = u;
	return o;
}

float3 ReconstructWorldPos(float2 uv, float depth)
{
	// uv: 0..1, depth: 0..1 (RH_ZO)
	float2 ndc;
	ndc.x = uv.x * 2.0f - 1.0f;
	ndc.y = (1.0f - uv.y) * 2.0f - 1.0f;

	float4 clip = float4(ndc, depth, 1.0f);
	float4 worldH = mul(clip, uInvViewProj);
	return worldH.xyz / max(worldH.w, 1e-6f);
}

float4 PS_DeferredLighting(VSOut i) : SV_Target0
{
	const float2 uv = saturate(i.uv);
	const float depth = gDepth.SampleLevel(gLinearClamp, uv, 0);
	if (depth >= 0.999999f)
	{
		return float4(0, 0, 0, 1);
	}
	const float4 g0 = gGBuffer0.SampleLevel(gLinearClamp, uv, 0);
	const float4 g1 = gGBuffer1.SampleLevel(gLinearClamp, uv, 0);
	
	float3 baseColor = g0.rgb;
	float roughness = saturate(g0.a);
	float metallic = saturate(g1.a);
	
	float3 N = normalize(g1.rgb * 2.0f - 1.0f);
	float3 worldPos = ReconstructWorldPos(uv, depth);
	float3 V = normalize(uCameraPosAmbient.xyz - worldPos);
	
	const float NdotV = max(dot(N, V), 1e-4f);
	const float3 F0 = lerp(float3(0.04f, 0.04f, 0.04f), baseColor, metallic);
	
	float3 Lo = 0.0f;
	const int lightCount = (int) uCounts.x;
	
	[loop]
	for (int li = 0; li < lightCount; ++li)
	{
		const GPULight Ld = gLights[li];
		const int type = (int) Ld.p0.w;
	
		float3 L = 0.0f;
		float attenuation = 1.0f;
	
		if (type == LIGHT_DIR)
		{
			L = normalize(-Ld.p1.xyz);
		}
		else if (type == LIGHT_POINT)
		{
			const float3 toLight = Ld.p0.xyz - worldPos;
			const float dist = length(toLight);
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
			const float3 toLight = Ld.p0.xyz - worldPos;
			const float dist = length(toLight);
			const float range = max(Ld.p2.w, 1e-3f);
			const float fade = max(range * 0.10f, 0.05f);
			float rangeFade = saturate((range - dist) / fade);
			rangeFade = rangeFade * rangeFade;
			if (rangeFade <= 0.0f)
				continue;
	
			L = toLight / max(dist, 1e-6f);
	
			const float3 spotDir = normalize(Ld.p1.xyz); // FROM light
			const float cosTheta = dot(-L, spotDir);
			const float cosInner = Ld.p3.x;
			const float cosOuter = Ld.p3.y;
			const float t = saturate((cosTheta - cosOuter) / max(cosInner - cosOuter, 1e-5f));
			const float spotAtt = t * t * (3.0f - 2.0f * t);
	
			const float attLin = Ld.p3.z;
			const float attQuad = Ld.p3.w;
			attenuation = (spotAtt * rangeFade) / max(1.0f + attLin * dist + attQuad * dist * dist, 1e-6f);
		}
		else
		{
			continue;
		}
	
		const float NdotL = saturate(dot(N, L));
		if (NdotL <= 0.0f)
			continue;
	
		const float3 H = normalize(V + L);
		const float NdotH = saturate(dot(N, H));
		const float VdotH = saturate(dot(V, H));
	
		const float a = max(roughness * roughness, 1e-4f);
		const float D = DistributionGGX(NdotH, a);
		const float G = GeometrySmith(NdotV, NdotL, roughness);
		const float3 F = FresnelSchlick(VdotH, F0);
	
		const float3 numerator = D * G * F;
		const float denom = max(4.0f * NdotV * NdotL, 1e-6f);
		const float3 specular = numerator / denom;
	
		const float3 kS = F;
		const float3 kD = (1.0f - kS) * (1.0f - metallic);
		const float3 diffuse = kD * baseColor / PI;
	
		const float3 radiance = Ld.p2.xyz * (Ld.p1.w * attenuation);
		Lo += (diffuse + specular) * radiance * NdotL;
	}
	
	const float3 ambient = baseColor * max(uCameraPosAmbient.w, 0.0f);
	const float3 color = ambient + Lo;
	return float4(color, 1.0f);
}