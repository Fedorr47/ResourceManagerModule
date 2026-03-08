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

cbuffer ToneMap : register(b0)
{
	float4 uParams; // exposure, mode, gamma, enableHDR
}

float3 ToneMapReinhard(float3 x)
{
	return x / (1.0f + x);
}

float3 ToneMapACES(float3 x)
{
	const float a = 2.51f;
	const float b = 0.03f;
	const float c = 2.43f;
	const float d = 0.59f;
	const float e = 0.14f;
	return saturate((x * (a * x + b)) / (x * (c * x + d) + e));
}

float4 PS_ToneMap(VSOut IN) : SV_Target
{
	float3 color = gSceneColor.Sample(gLinear, IN.uv).rgb;

	if (uParams.w > 0.5f)
	{
		color *= max(uParams.x, 0.0f);

		const uint mode = (uint)(uParams.y + 0.5f);
		if (mode == 1u)
		{
			color = ToneMapReinhard(color);
		}
		else if (mode == 2u)
		{
			color = ToneMapACES(color);
		}

		const float gamma = max(uParams.z, 1.0f);
		color = pow(saturate(color), 1.0f / gamma);
	}

	return float4(color, 1.0f);
}
