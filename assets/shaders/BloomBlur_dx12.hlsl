Texture2D gBloomInput : register(t0);
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

cbuffer BloomBlur : register(b0)
{
	float4 uInvSourceSize; // 1/w, 1/h
	float4 uDirection;     // x/y axis * radius
}

float4 PS_BloomBlur(VSOut IN) : SV_Target
{
	const float2 stepUV = uInvSourceSize.xy * uDirection.xy;

	float3 c = gBloomInput.Sample(gLinear, IN.uv).rgb * 0.2270270270f;
	c += gBloomInput.Sample(gLinear, IN.uv + stepUV * 1.3846153846f).rgb * 0.3162162162f;
	c += gBloomInput.Sample(gLinear, IN.uv - stepUV * 1.3846153846f).rgb * 0.3162162162f;
	c += gBloomInput.Sample(gLinear, IN.uv + stepUV * 3.2307692308f).rgb * 0.0702702703f;
	c += gBloomInput.Sample(gLinear, IN.uv - stepUV * 3.2307692308f).rgb * 0.0702702703f;

	return float4(c, 1.0f);
}
