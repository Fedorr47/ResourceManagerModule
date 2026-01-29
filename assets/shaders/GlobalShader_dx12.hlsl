// GlobalShader_dx12.hlsl
// Mesh + simple directional Blinn-Phong + optional shadow map.
// IMPORTANT: cbuffer layout must match PerDrawShadowedDirConstants (256 bytes) in DirectX12Renderer.cppm.

cbuffer PerDraw : register(b0)
{
    // 64 bytes
    float4x4 uMVP;

    // 64 bytes: lightProj * lightView * model
    float4x4 uLightMVP;

    // 48 bytes (3 rows of Model matrix: row0,row1,row2 as float4 each)
    float4 uModelRow0;
    float4 uModelRow1;
    float4 uModelRow2;

    // 16 bytes: cam.xyz + ambientStrength
    float4 uCameraAmbient;

    // 16 bytes: baseColor rgba
    float4 uBaseColor;

    // 16 bytes: shininess, specStrength, shadowBias, flagsPacked (stored as float)
    float4 uMaterialFlags;

    // 16 bytes: directional dir.xyz (FROM light towards scene), intensity
    float4 uDir_DirIntensity;

    // 16 bytes: directional color (rgb) + unused
    float4 uDir_Color;
};

Texture2D gAlbedo : register(t0);
Texture2D<float> gShadow : register(t1);

SamplerState gSamp : register(s0);
SamplerComparisonState gShadowSamp : register(s1);

static const uint kFlagUseTex = 1u << 0;
static const uint kFlagUseShadow = 1u << 1;
static const uint kFlagDirLight = 1u << 2;

struct VSIn
{
    float3 pos : POSITION0;
    float3 nrm : NORMAL0;
    float2 uv : TEXCOORD0;
};

struct VSOut
{
    float4 posH : SV_Position;
    float3 worldPos : TEXCOORD0;
    float3 normalW : TEXCOORD1;
    float2 uv : TEXCOORD2;
    float4 shadowPos : TEXCOORD3;
};

// Reconstruct world position using the 3 model rows.
// Assumes affine transform.
float3 WorldPosFromRows(float3 localPos)
{
    float x = dot(uModelRow0.xyz, localPos) + uModelRow0.w;
    float y = dot(uModelRow1.xyz, localPos) + uModelRow1.w;
    float z = dot(uModelRow2.xyz, localPos) + uModelRow2.w;
    return float3(x, y, z);
}

// Transform normal using the 3x3 part (rows) of model matrix.
// NOTE: correct only if no non-uniform scale.
float3 NormalWFromRows(float3 localN)
{
    float3 w;
    w.x = dot(uModelRow0.xyz, localN);
    w.y = dot(uModelRow1.xyz, localN);
    w.z = dot(uModelRow2.xyz, localN);
    return normalize(w);
}

VSOut VSMain(VSIn vin)
{
    VSOut o;

    // IMPORTANT: column-major matrices from glm => M * v
    o.posH = mul(uMVP, float4(vin.pos, 1.0f));

    o.worldPos = WorldPosFromRows(vin.pos);
    o.normalW = NormalWFromRows(vin.nrm);
    o.uv = vin.uv;

    // light clip pos
    o.shadowPos = mul(uLightMVP, float4(vin.pos, 1.0f));

    return o;
}

float3 BlinnPhong(float3 albedo, float3 N, float3 V, float3 L, float3 lightColor, float intensity, float shininess, float specStrength)
{
    float ndotl = saturate(dot(N, L));
    float3 diffuse = albedo * (ndotl * intensity) * lightColor;

    float3 H = normalize(L + V);
    float spec = pow(saturate(dot(N, H)), shininess) * specStrength;
    float3 specular = spec.xxx * (intensity) * lightColor;

    return diffuse + specular;
}

float ShadowFactor(float4 shadowPos, float shadowBias)
{
    // NDC
    float3 p = shadowPos.xyz / max(shadowPos.w, 1e-6f);

    // Outside the light frustum => consider lit.
    // Z in [0..1] for orthoRH_ZO.
    if (p.z < 0.0f || p.z > 1.0f)
        return 1.0f;

    float2 uv;
    uv.x = p.x * 0.5f + 0.5f;
    uv.y = -p.y * 0.5f + 0.5f;

    // Outside shadow texture => lit.
    if (uv.x < 0.0f || uv.x > 1.0f || uv.y < 0.0f || uv.y > 1.0f)
        return 1.0f;

    // Comparison sample: returns [0..1]
    return gShadow.SampleCmpLevelZero(gShadowSamp, uv, p.z - shadowBias);
}

float4 PSMain(VSOut pin) : SV_Target0
{
    uint flags = (uint) uMaterialFlags.w;

    float4 base = uBaseColor;
    if ((flags & kFlagUseTex) != 0u)
        base *= gAlbedo.Sample(gSamp, pin.uv);

    float3 albedo = base.rgb;
    float3 N = normalize(pin.normalW);
    float3 V = normalize(uCameraAmbient.xyz - pin.worldPos);

    float ambientStrength = uCameraAmbient.w;
    float shininess = uMaterialFlags.x;
    float specStrength = uMaterialFlags.y;
    float shadowBias = uMaterialFlags.z;

    float3 color = albedo * ambientStrength;

    if ((flags & kFlagDirLight) != 0u)
    {
        // uDir_DirIntensity.xyz = direction FROM light towards scene
        float3 L = normalize(-uDir_DirIntensity.xyz); // from point to light

        float shadow = 1.0f;
        if ((flags & kFlagUseShadow) != 0u)
        {
            shadow = ShadowFactor(pin.shadowPos, shadowBias);
        }

        float3 lightColor = uDir_Color.rgb;
        color += shadow * BlinnPhong(albedo, N, V, L, lightColor, uDir_DirIntensity.w, shininess, specStrength);
    }

    return float4(color, base.a);
}
