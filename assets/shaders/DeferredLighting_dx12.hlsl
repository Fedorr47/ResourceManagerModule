// DeferredLighting_dx12.hlsl
// Fullscreen deferred lighting resolve.

SamplerState gLinear : register(s0);
SamplerComparisonState gShadowCmp : register(s1);
SamplerState gPointClamp : register(s2);
SamplerState gLinearClamp : register(s3);

Texture2D gGBuffer0 : register(t0); // albedo.rgb, roughness
Texture2D gGBuffer1 : register(t1); // normal.xyz (encoded), metalness
Texture2D gGBuffer2 : register(t2); // emissive.rgb, ao
Texture2D gDepth : register(t3); // depth SRV (0..1)

struct GPULight
{
	float4 p0; // pos.xyz, type
	float4 p1; // dir.xyz (FROM light), intensity
	float4 p2; // color.rgb, range
	float4 p3; // cosInner, cosOuter, attLin, attQuad
};

StructuredBuffer<GPULight> gLights : register(t4);

cbuffer Deferred : register(b0)
{
	float4x4 uInvViewProj;
	float4 uCameraPosAmbient; // xyz + ambientStrength
	float4 uCounts; // x = lightCount
};

struct VSOut
{
	float4 svPos : SV_POSITION;
	float2 uv : TEXCOORD0;
};

VSOut VS_Fullscreen(uint vid : SV_VertexID)
{
    // Fullscreen triangle
	float2 pos = (vid == 0) ? float2(-1.0, -1.0) : (vid == 1) ? float2(-1.0, 3.0) : float2(3.0, -1.0);
	float2 uv = (vid == 0) ? float2(0.0, 0.0) : (vid == 1) ? float2(0.0, 2.0) : float2(2.0, 0.0);

	VSOut o;
	o.svPos = float4(pos, 0.0, 1.0);
	o.uv = uv;
	return o;
}

float3 FresnelSchlick(float cosTheta, float3 F0)
{
	return F0 + (1.0f - F0) * pow(1.0f - cosTheta, 5.0f);
}

float DistributionGGX(float NdotH, float roughness)
{
	float a = max(roughness, 0.04f);
	float a2 = a * a;
	float d = (NdotH * NdotH) * (a2 - 1.0f) + 1.0f;
	return a2 / max(3.14159265f * d * d, 1e-6f);
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
	float3 diff = kD * albedo / 3.14159265f;

	return diff + spec;
}

float3 ReconstructWorldPos(float2 uv, float depth)
{
    // uv in [0..1], depth in [0..1] (RH_ZO)
	float2 ndc = uv * 2.0f - 1.0f;
	float4 clip = float4(ndc, depth, 1.0f);
	float4 worldH = mul(clip, uInvViewProj);
	return worldH.xyz / max(worldH.w, 1e-6f);
}

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
			{
				continue;
			}
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
			float3 brdf = BRDF_CookTorrance(N, V, L, albedo, metallic, roughness);
			Lo += brdf * radiance * (NdotL * att);
		}
	}

	float3 ambient = albedo * (ambientStrength * ao);
	float3 color = ambient + Lo + emissive;

	return float4(color, 1.0f);
}