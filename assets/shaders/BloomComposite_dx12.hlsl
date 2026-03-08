Texture2D gSceneColor : register(t0);
Texture2D gBloom : register(t1);
SamplerState gLinear : register(s0);

struct VSOut
{
	float4 pos : SV_Position;
	float2 uv : TEXCOORD0;
};

VSOut VS_Fullscreen(uint id : SV_VertexID)
{
	float2 pos = float2((id == 2) ? 3.0 : -1.0, (id == 1) ? 3.0 : -1.0);
	float2 uv = float2((pos.x + 1.0) * 0.5, 1.0 - (pos.y + 1.0) * 0.5);

	VSOut o;
	o.pos = float4(pos, 0.0, 1.0);
	o.uv = uv;
	return o;
}

cbuffer BloomComposite : register(b0)
{
	float4 uParams; // intensity
}

float4 PS_BloomComposite(VSOut IN) : SV_Target
{
	const float3 scene = gSceneColor.Sample(gLinear, IN.uv).rgb;
	const float3 bloom = gBloom.Sample(gLinear, IN.uv).rgb;
	return float4(scene + bloom * uParams.x, 1.0f);
}
