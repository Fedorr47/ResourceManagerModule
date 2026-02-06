struct VSIn
{
    float3 pos : POSITION;
    float3 nrm : NORMAL;
    float2 uv  : TEXCOORD0;

    // Instancing: model matrix rows
    float4 i0  : TEXCOORD1;
    float4 i1  : TEXCOORD2;
    float4 i2  : TEXCOORD3;
    float4 i3  : TEXCOORD4;
};

struct VSOut
{
    float4 pos : SV_Position;
};

cbuffer DebugGizmoCB : register(b0)
{
    float4x4 uViewProj;
    float4   uColor;
};

float4x4 MakeMatRows(float4 row0, float4 row1, float4 row2, float4 row3)
{
    return float4x4(
        row0.x, row1.x, row2.x, row3.x,
        row0.y, row1.y, row2.y, row3.y,
        row0.z, row1.z, row2.z, row3.z,
        row0.w, row1.w, row2.w, row3.w);
}

VSOut VS_DebugGizmo(VSIn input)
{
    VSOut output;

    float4x4 model = MakeMatRows(input.i0, input.i1, input.i2, input.i3);
    float4 worldPos = mul(float4(input.pos, 1.0f), model);

    output.pos = mul(worldPos, uViewProj);
    return output;
}

float4 PS_DebugGizmo(VSOut input) : SV_Target
{
    return uColor;
}
