// ReflectionCaptureVI_dx12.hlsl (View Instancing: render ALL faces in one pass)
// Save as UTF-8 without BOM.

SamplerState gLinear : register(s0);
Texture2D gAlbedo : register(t0);

struct GPULight
{
    float4 p0; // pos.xyz, type
    float4 p1; // dir.xyz, intensity
    float4 p2; // color.rgb, range
    float4 p3; // cosInner, cosOuter, attLin, attQuad
};
StructuredBuffer<GPULight> gLights : register(t2);

cbuffer ReflectionCaptureCB : register(b0)
{
	row_major float4x4 uFaceViewProj[6]; // face view-proj matrices (column-major in cbuffer)
    float4 uCapturePosAmbient; // xyz + ambientStrength
    float4 uBaseColor; // rgba
    float4 uParams; // x=lightCount, y=flags(asfloat)а
};

static const uint FLAG_USE_TEX = 1u << 0;
static const int LIGHT_DIR = 0;

row_major float4x4 MakeMatRows(float4 r0, float4 r1, float4 r2, float4 r3)
{
    return float4x4(r0, r1, r2, r3);
}

struct VSIn
{
    float3 pos : POSITION;
    float3 nrm : NORMAL;
    float2 uv : TEXCOORD0;

    float4 i0 : TEXCOORD1;
    float4 i1 : TEXCOORD2;
    float4 i2 : TEXCOORD3;
    float4 i3 : TEXCOORD4;
};

struct VSOut
{
    float4 posH : SV_Position;
    float3 worldPos : TEXCOORD0;
    float3 nrmW : TEXCOORD1;
    float2 uv : TEXCOORD2;
};

VSOut VS_ReflectionCaptureVI(VSIn IN, uint viewId : SV_ViewID)
{
    VSOut OUT;

	row_major float4x4 model = MakeMatRows(IN.i0, IN.i1, IN.i2, IN.i3);
    float4 world = mul(float4(IN.pos, 1.0f), model);

    OUT.worldPos = world.xyz;
    OUT.nrmW = normalize(mul(float4(IN.nrm, 0.0f), model).xyz);
    OUT.uv = IN.uv;

	row_major float4x4 vp = uFaceViewProj[viewId];
    OUT.posH = mul(world, vp);
    
    return OUT;
}

float3 EvalDirLight(float3 N, float3 baseColor)
{
    uint lightCount = (uint) uParams.x;
    [loop]
    for (uint i = 0; i < lightCount; ++i)
    {
        const GPULight L = gLights[i];
        const int type = (int) L.p0.w;
        if (type == LIGHT_DIR)
        {
            float3 dirFromLight = normalize(L.p1.xyz);
            float3 Ldir = -dirFromLight;
            float ndl = saturate(dot(N, Ldir));
            float3 col = L.p2.rgb * L.p1.w;
            return baseColor * col * ndl;
        }
    }
    return 0.0.xxx;
}

float4 PS_ReflectionCaptureVI(VSOut IN) : SV_Target0
{
    const uint flags = asuint(uParams.y);
    float3 baseColor = uBaseColor.rgb;
    float alphaOut = uBaseColor.a;

    if ((flags & FLAG_USE_TEX) != 0)
    {
        float4 tex = gAlbedo.Sample(gLinear, IN.uv);
        baseColor *= tex.rgb;
        alphaOut *= tex.a;
    }

    const float ambient = uCapturePosAmbient.w;
    float3 color = baseColor * ambient + EvalDirLight(IN.nrmW, baseColor);

    return float4(color, alphaOut);
}
