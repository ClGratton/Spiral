cbuffer ViewportConstants : register(b0)
{
    row_major float4x4 ViewProjection;
    row_major float4x4 NormalTransform;
    float4 BaseColorAndAlphaCutoff;
    float4 EmissiveAndStrength;
    float4 SurfaceFactors;
    float4 CallistoFactors;
    uint4 TextureIndices0;
    uint4 TextureIndices1;
    uint4 TextureState;
    uint4 MaterialState;
};

#ifndef GE_READ_ONLY_TEXTURE_CAPACITY
#define GE_READ_ONLY_TEXTURE_CAPACITY 4096
#endif

Texture2D ReadOnlyTextures[GE_READ_ONLY_TEXTURE_CAPACITY] : register(t0, space1);
SamplerState ReadOnlySamplers[GE_READ_ONLY_TEXTURE_CAPACITY] : register(s0, space1);

struct VSInput
{
    float3 Position : POSITION;
    float3 Normal : NORMAL;
    float3 Color : COLOR;
    float2 UV : TEXCOORD;
};

struct VSOutput
{
    float4 Position : SV_Position;
    float3 Color : COLOR0;
    float2 UV : TEXCOORD0;
    float3 GeometricNormal : NORMAL0;
};

VSOutput VSMain(VSInput input)
{
    VSOutput output;
    output.Position = mul(float4(input.Position, 1.0f), ViewProjection);
    output.GeometricNormal = normalize(mul(float4(input.Normal, 0.0f), NormalTransform).xyz);
    output.Color = input.Color;
    output.UV = input.UV;
    return output;
}

// Smoke-only readback entry point. The production vertex input and b0 payload
// are unchanged; ordinary Editor output continues to compile PSMain.
float4 PSSurfaceBasisMaterialProbe(VSOutput input) : SV_Target0
{
    if (MaterialState.x != 1u || MaterialState.y != 0u)
        return float4(0.7f, 0.0f, 0.0f, 1.0f);
    return float4(0.5f + input.GeometricNormal * 0.25f, 1.0f);
}

float4 PSMain(VSOutput input) : SV_Target0
{
    const float2 uv = input.UV;
    const uint declared = TextureState.x;
    float4 baseSample = float4(1.0f, 1.0f, 1.0f, 1.0f);
    float3 normalSample = float3(0.5f, 0.5f, 1.0f);
    float3 ormSample = float3(1.0f, 1.0f, 1.0f);
    float3 emissiveSample = float3(0.0f, 0.0f, 0.0f);
    float opacitySample = 1.0f;
    float3 controlSample = float3(1.0f, 1.0f, 1.0f);
    if ((declared & (1u << 0u)) != 0u)
        baseSample = ReadOnlyTextures[TextureIndices0.x].Sample(ReadOnlySamplers[TextureIndices0.x], uv);
    if ((declared & (1u << 1u)) != 0u)
        normalSample = ReadOnlyTextures[TextureIndices0.y].Sample(ReadOnlySamplers[TextureIndices0.y], uv).xyz;
    if ((declared & (1u << 2u)) != 0u)
        ormSample = ReadOnlyTextures[TextureIndices0.z].Sample(ReadOnlySamplers[TextureIndices0.z], uv).xyz;
    if ((declared & (1u << 3u)) != 0u)
        emissiveSample = ReadOnlyTextures[TextureIndices0.w].Sample(ReadOnlySamplers[TextureIndices0.w], uv).xyz;
    if ((declared & (1u << 4u)) != 0u)
        opacitySample = ReadOnlyTextures[TextureIndices1.x].Sample(ReadOnlySamplers[TextureIndices1.x], uv).x;
    if ((declared & (1u << 5u)) != 0u)
        controlSample = ReadOnlyTextures[TextureIndices1.y].Sample(ReadOnlySamplers[TextureIndices1.y], uv).xyz;

    const float3 baseColor = saturate(BaseColorAndAlphaCutoff.rgb * baseSample.rgb);
    const float3 tangentNormal = normalize(normalSample * 2.0f - 1.0f);
    const float normalDetail = lerp(1.0f, saturate(0.45f + tangentNormal.z * 0.55f),
        saturate(SurfaceFactors.z));
    const float occlusion = lerp(1.0f, ormSample.r, saturate(SurfaceFactors.w));
    const float roughness = saturate(SurfaceFactors.y * ormSample.g);
    const float metallic = saturate(SurfaceFactors.x * ormSample.b);
    const float3 faceTint = lerp(float3(1.0f, 1.0f, 1.0f), input.Color, 0.22f);
    float3 shaded = baseColor * faceTint * normalDetail * occlusion;
    shaded *= lerp(1.08f, 0.78f, roughness * 0.65f);
    shaded = lerp(shaded, shaded * 0.72f + baseColor * 0.28f, metallic);

    float2 edge = min(saturate(uv), 1.0f - saturate(uv));
    float edgeDistance = min(edge.x, edge.y);
    float edgeAA = max(fwidth(edgeDistance), 0.001f);
    float outerFrame = 1.0f - smoothstep(0.035f - edgeAA, 0.075f + edgeAA, edgeDistance);
    float innerFrame = 1.0f - smoothstep(
        0.012f - edgeAA,
        0.012f + edgeAA,
        abs(edgeDistance - 0.135f));

    float2 cornerVector = min(uv, 1.0f - uv);
    float cornerDistance = length(cornerVector);
    float cornerGlow = 1.0f - smoothstep(0.08f, 0.24f, cornerDistance);
    float3 accent = lerp(baseColor, float3(0.82f, 0.94f, 1.0f),
        saturate(0.28f * CallistoFactors.x * controlSample.x));
    shaded = lerp(shaded, accent * 1.12f,
        outerFrame * saturate(0.35f + 0.35f * CallistoFactors.y * controlSample.y));
    shaded += accent * (innerFrame * 0.10f + cornerGlow * 0.14f) * controlSample.z;
    shaded += EmissiveAndStrength.rgb * EmissiveAndStrength.a * emissiveSample;

    const float alpha = saturate(baseSample.a * opacitySample);
    if (TextureState.z == 1u)
        clip(alpha - BaseColorAndAlphaCutoff.a);
    if (TextureState.w == 1u)
        shaded = baseColor + EmissiveAndStrength.rgb * EmissiveAndStrength.a * emissiveSample;

    float portableBindingProbe = 1.0f + abs(ViewProjection[0][0]) * 0.0000001f;
    return float4(saturate(shaded) * portableBindingProbe,
        TextureState.z == 2u ? alpha : 1.0f);
}

// Native cross-backend acceptance entry point. Each eight-pixel cell exposes
// one material texture semantic without the display shading used by PSMain.
// The production constants and sampled-table layout remain identical.
float4 PSMaterialProbe(VSOutput input) : SV_Target0
{
    const uint cell = min((uint)(input.Position.x / 8.0f), 7u);
    const uint declared = TextureState.x;
    if (cell == 0u)
        return (declared & (1u << 0u)) != 0u
            ? ReadOnlyTextures[TextureIndices0.x].SampleLevel(ReadOnlySamplers[TextureIndices0.x], float2(0.25f, 0.25f), 0.0f)
            : float4(1.0f, 1.0f, 1.0f, 1.0f);
    if (cell == 1u)
        return (declared & (1u << 0u)) != 0u
            ? ReadOnlyTextures[TextureIndices0.x].SampleLevel(ReadOnlySamplers[TextureIndices0.x], float2(0.25f, 0.25f), 1.0f)
            : float4(1.0f, 1.0f, 1.0f, 1.0f);
    if (cell == 2u)
        return (declared & (1u << 1u)) != 0u
            ? ReadOnlyTextures[TextureIndices0.y].SampleLevel(ReadOnlySamplers[TextureIndices0.y], float2(0.25f, 0.25f), 0.0f)
            : float4(0.5f, 0.5f, 1.0f, 1.0f);
    if (cell == 3u)
        return (declared & (1u << 2u)) != 0u
            ? ReadOnlyTextures[TextureIndices0.z].SampleLevel(ReadOnlySamplers[TextureIndices0.z], float2(0.25f, 0.25f), 0.0f)
            : float4(1.0f, 1.0f, 1.0f, 1.0f);
    if (cell == 4u)
        return (declared & (1u << 3u)) != 0u
            ? ReadOnlyTextures[TextureIndices0.w].SampleLevel(ReadOnlySamplers[TextureIndices0.w], float2(0.25f, 0.25f), 0.0f)
            : float4(0.0f, 0.0f, 0.0f, 1.0f);
    if (cell == 5u)
        return (declared & (1u << 4u)) != 0u
            ? ReadOnlyTextures[TextureIndices1.x].SampleLevel(ReadOnlySamplers[TextureIndices1.x], float2(1.25f, 0.25f), 0.0f)
            : float4(1.0f, 1.0f, 1.0f, 1.0f);
    if (cell == 6u)
        return (declared & (1u << 5u)) != 0u
            ? ReadOnlyTextures[TextureIndices1.y].SampleLevel(ReadOnlySamplers[TextureIndices1.y], float2(1.25f, 0.25f), 0.0f)
            : float4(1.0f, 1.0f, 1.0f, 1.0f);
    return TextureState.y == 0u
        ? float4(0.0f, 1.0f, 0.0f, 1.0f)
        : float4(1.0f, 0.0f, 0.0f, 1.0f);
}
