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

VSOut VS_Skybox(VSIn i)
{
	VSOut o;
	o.dir = i.pos;

	float4 clip = mul(float4(i.pos, 1.0), uViewProj);
	
	o.pos = float4(clip.xy, clip.w, clip.w);
	return o;
}

float4 PS_Skybox(VSOut i) : SV_Target
{
	float3 d = normalize(i.dir);

	float t = saturate(d.y * 0.5 + 0.5);
	float3 horizon = float3(0.80, 0.86, 0.95);
	float3 zenith = float3(0.10, 0.25, 0.60);
	float3 col = lerp(horizon, zenith, t);

	float3 sunDir = normalize(float3(0.2, 0.7, 0.1));
	float sun = pow(saturate(dot(d, sunDir)), 256.0);
	col += float3(1.2, 1.1, 0.9) * sun;

	return float4(col, 1.0);
}
