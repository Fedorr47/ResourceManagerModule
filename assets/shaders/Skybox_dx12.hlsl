struct VSIn
{
	float3 pos : POSITION;
};

struct VSOut
{
	float4 pos : SV_Position;
	float3 dir : TEXCOORD0;
};

cbuffer PerDraw : register(b0)
{
	float4x4 uViewProj;
};

// Root signature: s0 exists
SamplerState gLinear : register(s0);
// Cubemap bound at t0
TextureCube<float4> gSkybox : register(t0);
SamplerState gLinearClamp : register(s3);

VSOut VS_Skybox(VSIn input)
{
	VSOut output;
	output.dir = input.pos;

	float4 clip = mul(float4(input.pos, 1.0), uViewProj);
	output.pos = float4(clip.xy, clip.w, clip.w); // always at far plane
	return output;
}

float4 PS_Skybox(VSOut input) : SV_Target
{
	float3 dir = normalize(input.dir);
	dir = float3(dir.x, dir.y, -dir.z);
	const float3 color = gSkybox.Sample(gLinearClamp, dir).rgb;
	return float4(color, 1.0);
}
