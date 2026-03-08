Texture2D gSceneColor : register(t0);
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

cbuffer BloomExtract : register(b0)
{
	float4 uInvSourceSize; // 1/w, 1/h
	float4 uParams;        // threshold, softKnee, clampMax, pad
}

float3 SampleScene(float2 uv)
{
	const float2 texel = uInvSourceSize.xy;
	const float2 o = 0.5 * texel;
	float3 c = 0.0f;
	c += gSceneColor.Sample(gLinear, uv + float2(-o.x, -o.y)).rgb;
	c += gSceneColor.Sample(gLinear, uv + float2( o.x, -o.y)).rgb;
	c += gSceneColor.Sample(gLinear, uv + float2(-o.x,  o.y)).rgb;
	c += gSceneColor.Sample(gLinear, uv + float2( o.x,  o.y)).rgb;
	return c * 0.25f;
}

float4 PS_BloomExtract(VSOut IN) : SV_Target
{
	float3 color = SampleScene(IN.uv);
	const float brightness = max(color.r, max(color.g, color.b));

	const float threshold = max(uParams.x, 1e-4f);
	const float knee = max(uParams.y, 1e-4f);
	float soft = saturate((brightness - threshold + knee) / (2.0f * knee));
	soft = soft * soft * (3.0f - 2.0f * soft);

	float contribution = max(brightness - threshold, 0.0f) + soft * knee;
	contribution /= max(brightness, 1e-4f);

	float3 bloom = color * contribution;
	if (uParams.z > 0.0f)
	{
		bloom = min(bloom, float3(uParams.z, uParams.z, uParams.z));
	}
	return float4(bloom, 1.0f);
}
