SamplerState gLinearClamp : register(s3);

Texture2D gSceneColor : register(t0);
Texture2D gSSAO : register(t1);

struct VSOut
{
	float4 svPos : SV_POSITION;
	float2 uv : TEXCOORD0;
};

VSOut VS_Fullscreen(uint vid : SV_VertexID)
{
	float2 pos = (vid == 0) ? float2(-1.0, -1.0)
               : (vid == 1) ? float2(-1.0, 3.0)
                            : float2(3.0, -1.0);

	float2 uv = float2((pos.x + 1.0f) * 0.5f, 1.0f - (pos.y + 1.0f) * 0.5f);

	VSOut o;
	o.svPos = float4(pos, 0.0f, 1.0f);
	o.uv = uv;
	return o;
}

float4 PS_SSAOComposite(VSOut IN) : SV_Target0
{
	const float4 scene = gSceneColor.Sample(gLinearClamp, IN.uv);
	const float ao = saturate(gSSAO.Sample(gLinearClamp, IN.uv).r);
	
	return float4(scene.rgb * ao, scene.a);
}