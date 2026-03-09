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

cbuffer FXAA : register(b0)
{
	float4 uInvSourceSize; // x=1/w, y=1/h
	float4 uParams;        // x=subpix, y=edgeThreshold, z=edgeThresholdMin
}

float Luma(float3 color)
{
	return dot(color, float3(0.299f, 0.587f, 0.114f));
}

float3 SampleColor(float2 uv)
{
	return gSceneColor.SampleLevel(gLinear, uv, 0.0f).rgb;
}

float4 PS_FXAA(VSOut IN) : SV_Target
{
	const float2 texel = uInvSourceSize.xy;

	const float3 rgbM = SampleColor(IN.uv);
	const float3 rgbN = SampleColor(IN.uv + float2(0.0f, -texel.y));
	const float3 rgbS = SampleColor(IN.uv + float2(0.0f, texel.y));
	const float3 rgbE = SampleColor(IN.uv + float2(texel.x, 0.0f));
	const float3 rgbW = SampleColor(IN.uv + float2(-texel.x, 0.0f));

	const float lumaM = Luma(rgbM);
	const float lumaN = Luma(rgbN);
	const float lumaS = Luma(rgbS);
	const float lumaE = Luma(rgbE);
	const float lumaW = Luma(rgbW);

	const float lumaMin = min(lumaM, min(min(lumaN, lumaS), min(lumaE, lumaW)));
	const float lumaMax = max(lumaM, max(max(lumaN, lumaS), max(lumaE, lumaW)));
	const float lumaRange = lumaMax - lumaMin;
	const float edgeThreshold = max(uParams.z, lumaMax * uParams.y);

	if (lumaRange < edgeThreshold)
	{
		return float4(rgbM, 1.0f);
	}

	const float3 rgbNW = SampleColor(IN.uv + texel * float2(-1.0f, -1.0f));
	const float3 rgbNE = SampleColor(IN.uv + texel * float2(1.0f, -1.0f));
	const float3 rgbSW = SampleColor(IN.uv + texel * float2(-1.0f, 1.0f));
	const float3 rgbSE = SampleColor(IN.uv + texel * float2(1.0f, 1.0f));

	const float lumaNW = Luma(rgbNW);
	const float lumaNE = Luma(rgbNE);
	const float lumaSW = Luma(rgbSW);
	const float lumaSE = Luma(rgbSE);

	float2 dir;
	dir.x = -((lumaNW + lumaNE) - (lumaSW + lumaSE));
	dir.y =  ((lumaNW + lumaSW) - (lumaNE + lumaSE));

	const float dirReduce = max((lumaNW + lumaNE + lumaSW + lumaSE) * (0.25f * 0.125f), 1.0f / 128.0f);
	const float rcpDirMin = 1.0f / (min(abs(dir.x), abs(dir.y)) + dirReduce);
	dir = clamp(dir * rcpDirMin, float2(-8.0f, -8.0f), float2(8.0f, 8.0f)) * texel;

	const float3 rgbA = 0.5f * (
		SampleColor(IN.uv + dir * (1.0f / 3.0f - 0.5f)) +
		SampleColor(IN.uv + dir * (2.0f / 3.0f - 0.5f)));

	const float3 rgbB = rgbA * 0.5f + 0.25f * (
		SampleColor(IN.uv + dir * -0.5f) +
		SampleColor(IN.uv + dir * 0.5f));

	const float lumaB = Luma(rgbB);
	float3 rgbFXAA = ((lumaB < lumaMin) || (lumaB > lumaMax)) ? rgbA : rgbB;

	const float lumaAvg = (lumaN + lumaS + lumaE + lumaW) * 0.25f;
	float subpix = saturate(abs(lumaAvg - lumaM) / max(lumaRange, 1e-4f));
	subpix = subpix * subpix * saturate(uParams.x);

	const float3 rgbLocalAvg = (rgbN + rgbS + rgbE + rgbW + rgbM) * 0.2f;
	rgbFXAA = lerp(rgbFXAA, rgbLocalAvg, subpix * 0.25f);

	return float4(rgbFXAA, 1.0f);
}