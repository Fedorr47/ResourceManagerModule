// Simple SSAO in world-space using depth + world-space normals from GBuffer.
// Outputs AO factor in R32_FLOAT render target (0..1).

SamplerState gLinear : register(s0);
SamplerState gPointClamp : register(s2);

Texture2D gGBuffer1 : register(t0); // normal.xyz (encoded), metalness
Texture2D gDepth : register(t1); // depth (0..1)

cbuffer SSAO : register(b0)
{
	float4x4 uInvViewProj;
	float4 uParams; // x=radiusWorld, y=biasWorld, z=strength, w=power
	float4 uInvSize; // x=1/width, y=1/height
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
	// Texture-space UV (0,0 top-left)
    float2 uv = float2((pos.x + 1.0f) * 0.5f, 1.0f - (pos.y + 1.0f) * 0.5f);

	VSOut o;
	o.svPos = float4(pos, 0.0, 1.0);
	o.uv = uv;
	return o;
}

float Hash12(float2 p)
{
	// Small, deterministic hash
	float3 p3 = frac(float3(p.xyx) * 0.1031f);
	p3 += dot(p3, p3.yzx + 33.33f);
	return frac((p3.x + p3.y) * p3.z);
}

float3 ReconstructWorldPos(float2 uv, float depth)
{
	// uv is texture-space (0,0 top-left) -> flip Y for NDC (Y-up)
	float4 ndc;
	ndc.x = uv.x * 2.0f - 1.0f;
	ndc.y = 1.0f - uv.y * 2.0f;
	ndc.z = depth;
	ndc.w = 1.0f;
	float4 wp = mul(ndc, uInvViewProj);
	return wp.xyz / max(wp.w, 1e-6f);
}

float PS_SSAO(VSOut IN) : SV_Target0
{
	// If depth is 1.0 (background), AO=1
	float depthC = gDepth.Sample(gPointClamp, IN.uv).r;
	if (depthC >= 0.999999f)
	{
		return 1.0f;
	}

	float3 N = normalize(gGBuffer1.Sample(gPointClamp, IN.uv).rgb * 2.0f - 1.0f);
	float3 P = ReconstructWorldPos(IN.uv, depthC);

	const float radius = max(uParams.x, 1e-3f);
	const float bias = max(uParams.y, 0.0f);
	const float strength = max(uParams.z, 0.0f);
	const float power = max(uParams.w, 1.0f);

	// Sample kernel (unit disk). 8 taps.
	static const float2 kOffsets[8] =
	{
		float2(1, 0),
		float2(-1, 0),
		float2(0, 1),
		float2(0, -1),
		float2(1, 1),
		float2(-1, 1),
		float2(1, -1),
		float2(-1, -1)
	};

	// Per-pixel rotation to reduce banding
	float rnd = Hash12(IN.svPos.xy);
	float ang = rnd * 6.2831853f;
	float2x2 R = float2x2(cos(ang), -sin(ang), sin(ang), cos(ang));

	float occ = 0.0f;
	const int kCount = 8;
	[unroll]
	for (int i = 0; i < kCount; ++i)
	{
		float2 o = mul(kOffsets[i], R);
		o = normalize(o) * (0.5f + 0.5f * Hash12(IN.svPos.xy + float2(i, i * 7)));

		// Screen-space radius scale: smaller footprint when far.
		// We don't have view-space Z, so approximate using world distance from reconstructed positions.
		float2 uvS = IN.uv + o * (radius * 0.15f) * uInvSize.xy;

		float dS = gDepth.Sample(gPointClamp, uvS).r;
		if (dS >= 0.999999f)
		{
			continue;
		}

		float3 PS = ReconstructWorldPos(uvS, dS);
		float3 V = PS - P;
		float dist = length(V);
		if (dist > radius || dist < 1e-4f)
		{
			continue;
		}

		float3 dir = V / dist;
		float NdotV = dot(N, dir);

		// Occluder if it's in the hemisphere and close enough.
		float w = saturate(1.0f - dist / radius);
		float hemi = saturate(NdotV - bias);
		occ += w * hemi;
	}

	occ = occ / (float) kCount;
	float ao = 1.0f - strength * pow(saturate(occ), power);
	return saturate(ao);
}