SamplerState gLinear : register(s0);

// Material SRVs (match existing root signature slots)
Texture2D gAlbedo : register(t0);
Texture2D gNormal : register(t12);
Texture2D gMetalness : register(t13);
Texture2D gRoughness : register(t14);

cbuffer PerBatchCB : register(b0)
{
	float4x4 uViewProj;
	float4x4 uLightViewProj;
	float4 uCameraAmbient;
	float4 uCameraForward;
	float4 uBaseColor;
	float4 uMaterialFlags; // w = flags bits (asfloat)
	float4 uPbrParams; // x=metallic, y=roughness, z=ao, w=emissiveStrength
	float4 uCounts;
	float4 uShadowBias;
	float4 uEnvProbeBoxMin;
	float4 uEnvProbeBoxMax;
};

static const uint FLAG_USE_TEX = 1u << 0;
static const uint FLAG_USE_NORMAL = 1u << 2;
static const uint FLAG_USE_METAL_TEX = 1u << 3;
static const uint FLAG_USE_ROUGH_TEX = 1u << 4;
static const float kInverseEpsilon = 1e-8f;

float3x3 Inverse3x3(float3x3 m)
{
	const float a00 = m[0][0];
	const float a01 = m[0][1];
	const float a02 = m[0][2];

	const float a10 = m[1][0];
	const float a11 = m[1][1];
	const float a12 = m[1][2];

	const float a20 = m[2][0];
	const float a21 = m[2][1];
	const float a22 = m[2][2];

	const float c00 = (a11 * a22 - a12 * a21);
	const float c01 = -(a10 * a22 - a12 * a20);
	const float c02 = (a10 * a21 - a11 * a20);

	const float c10 = -(a01 * a22 - a02 * a21);
	const float c11 = (a00 * a22 - a02 * a20);
	const float c12 = -(a00 * a21 - a01 * a20);

	const float c20 = (a01 * a12 - a02 * a11);
	const float c21 = -(a00 * a12 - a02 * a10);
	const float c22 = (a00 * a11 - a01 * a10);

	const float det = a00 * c00 + a01 * c01 + a02 * c02;
	if (abs(det) < kInverseEpsilon)
	{
		return float3x3(
			1.0f, 0.0f, 0.0f,
			0.0f, 1.0f, 0.0f,
			0.0f, 0.0f, 1.0f);
	}

	const float invDet = 1.0f / det;
	float3x3 invM;
	invM[0][0] = c00 * invDet;
	invM[0][1] = c10 * invDet;
	invM[0][2] = c20 * invDet;

	invM[1][0] = c01 * invDet;
	invM[1][1] = c11 * invDet;
	invM[1][2] = c21 * invDet;

	invM[2][0] = c02 * invDet;
	invM[2][1] = c12 * invDet;
	invM[2][2] = c22 * invDet;
	return invM;
}

float3x3 InverseTranspose3x3(float3x3 m)
{
	return transpose(Inverse3x3(m));
}

float4x4 MakeMatRows(float4 r0, float4 r1, float4 r2, float4 r3)
{
	return float4x4(r0, r1, r2, r3);
}

struct VSIn
{
	float3 pos : POSITION;
	float3 nrm : NORMAL;
	float2 uv : TEXCOORD0;
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
};

VSOut VS_GBuffer(VSIn vin)
{
	VSOut o;

	float4x4 model = MakeMatRows(vin.i0, vin.i1, vin.i2, vin.i3);
	float3x3 model3x3 = (float3x3) model;
	
	float4 world = mul(float4(vin.pos, 1.0f), model);
	float3x3 normalMatrix = InverseTranspose3x3(model3x3);
	float3 nrmW = normalize(mul(vin.nrm, normalMatrix));
	
	o.worldPos = world.xyz;
	o.nrmW = nrmW;
	o.uv = vin.uv;
	o.posH = mul(world, uViewProj);
	return o;
}

float3 GetNormalMapped(float3 N, float3 worldPos, float2 uv)
{
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

struct PSOut
{
	float4 o0 : SV_Target0; // baseColor.rgb, roughness
	float4 o1 : SV_Target1; // normal.xyz (0..1), metallic
};

PSOut PS_GBuffer(VSOut i)
{
	PSOut o;

	const uint flags = asuint(uMaterialFlags.w);

	float4 base = uBaseColor;
	if ((flags & FLAG_USE_TEX) != 0)
	{
		float4 a = gAlbedo.Sample(gLinear, i.uv);
		base *= a;
	}

	float metallic = uPbrParams.x;
	if ((flags & FLAG_USE_METAL_TEX) != 0)
	{
		metallic = gMetalness.Sample(gLinear, i.uv).r;
	}

	float roughness = uPbrParams.y;
	if ((flags & FLAG_USE_ROUGH_TEX) != 0)
	{
		roughness = gRoughness.Sample(gLinear, i.uv).r;
	}

	float3 N = normalize(i.nrmW);
	if ((flags & FLAG_USE_NORMAL) != 0)
	{
		N = GetNormalMapped(N, i.worldPos, i.uv);
	}

	float3 nEnc = N * 0.5f + 0.5f;

	o.o0 = float4(base.rgb, saturate(roughness));
	o.o1 = float4(saturate(nEnc), saturate(metallic));
	return o;
}