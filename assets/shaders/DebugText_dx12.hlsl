// DebugText_dx12.hlsl
// Screen-space debug text rendering (pixel coords -> NDC)

cbuffer DebugTextCB : register(b0)
{
	float2 uInvViewportSize; // (1/width, 1/height)
	float2 _pad;
};

struct VSIn
{
	float2 posPx : POSITION; // pixel coordinates (0,0 is top-left)
	float4 color : COLOR0; // from R8G8B8A8_UNORM, normalized to 0..1
};

struct VSOut
{
	float4 posH : SV_POSITION;
	float4 color : COLOR0;
};

VSOut VS_DebugText(VSIn IN)
{
	VSOut OUT;

    // Convert pixel coords to NDC:
    // x: [0..W] -> [-1..+1]
    // y: [0..H] -> [+1..-1] (top-left origin, so invert)
	float x = IN.posPx.x * uInvViewportSize.x * 2.0f - 1.0f;
	float y = 1.0f - IN.posPx.y * uInvViewportSize.y * 2.0f;

	OUT.posH = float4(x, y, 0.0f, 1.0f);
	OUT.color = IN.color;
	return OUT;
}

float4 PS_DebugText(VSOut IN) : SV_TARGET0
{
	return IN.color;
}