// Simple depth-aware blur for SSAO (R32_FLOAT). Outputs blurred AO (0..1).

SamplerState gPointClamp : register(s2);

Texture2D<float> gSSAO : register(t0);
Texture2D gDepth : register(t1);

cbuffer Blur : register(b0)
{
	float4 uInvSize; // x=1/w, y=1/h
	float4 uParams; // x=depthThreshold
};

struct VSOut
{
	float4 svPos : SV_POSITION;
	float2 uv : TEXCOORD0;
};

VSOut VS_Fullscreen(uint vid : SV_VertexID)
{
	float2 pos = (vid == 0) ? float2(-1.0, -1.0) : (vid == 1) ? float2(-1.0, 3.0) : float2(3.0, -1.0);
	// Texture-space UV (0,0 top-left)
    float2 uv = float2((pos.x + 1.0f) * 0.5f, 1.0f - (pos.y + 1.0f) * 0.5f);// Texture-space UV (0,0 top-left)

	VSOut o;
	o.svPos = float4(pos, 0.0, 1.0);
	o.uv = uv;
	return o;
}

float PS_SSAOBlur(VSOut IN) : SV_Target0
{
	float d0 = gDepth.Sample(gPointClamp, IN.uv).r;
	float a0 = gSSAO.Sample(gPointClamp, IN.uv);

	if (d0 >= 0.999999f)
	{
		return 1.0f;
	}

	float sum = a0;
	float wsum = 1.0f;

	const float th = max(uParams.x, 0.0005f);

	// 8-neighborhood cross blur
	static const int2 kOff[8] =
	{
		int2(1, 0), int2(-1, 0), int2(0, 1), int2(0, -1),
		int2(2, 0), int2(-2, 0), int2(0, 2), int2(0, -2)
	};

	[unroll]
	for (int i = 0; i < 8; ++i)
	{
		float2 uvS = IN.uv + float2(kOff[i]) * uInvSize.xy;
		float dS = gDepth.Sample(gPointClamp, uvS).r;
		float aS = gSSAO.Sample(gPointClamp, uvS);

		float w = (abs(dS - d0) < th) ? 1.0f : 0.0f;
		sum += aS * w;
		wsum += w;
	}

	return saturate(sum / max(wsum, 1e-6f));
}