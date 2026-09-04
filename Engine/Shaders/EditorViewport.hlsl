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
    row_major float4x4 ModelView;
    row_major float4x4 NormalViewTransform;
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
    float3 ViewPosition : TEXCOORD1;
    float3 ViewNormal : NORMAL1;
};

VSOutput VSMain(VSInput input)
{
    VSOutput output;
    output.Position = mul(float4(input.Position, 1.0f), ViewProjection);
    output.GeometricNormal = normalize(mul(float4(input.Normal, 0.0f), NormalTransform).xyz);
    output.ViewPosition = mul(float4(input.Position, 1.0f), ModelView).xyz;
    output.ViewNormal = mul(float4(input.Normal, 0.0f), NormalViewTransform).xyz;
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

float3 NormalizeOrFallback(float3 value, float3 fallback)
{
    if (!all(isfinite(value)))
        return fallback;
    const float maximumComponent = max(abs(value.x), max(abs(value.y), abs(value.z)));
    if (!(maximumComponent > 0.0f))
        return fallback;
    const float3 scaled = value / maximumComponent;
    const float lengthSquared = dot(scaled, scaled);
    return isfinite(lengthSquared) && lengthSquared > 0.0f
        ? scaled * rsqrt(lengthSquared) : fallback;
}

float4 PSMain(VSOutput input) : SV_Target0
{
    // Row zero is never a persistent material identity. It is a deliberately
    // obvious deterministic error result in scene-linear HDR.
    if (MaterialState.x == 0u || MaterialState.y != 0u)
        return float4(4.0f, 0.0f, 4.0f, 1.0f);

    const float2 uv = input.UV;
    const uint declared = TextureState.x;
    float4 baseSample = float4(1.0f, 1.0f, 1.0f, 1.0f);
    float3 ormSample = float3(1.0f, 1.0f, 1.0f);
    float3 emissiveSample = float3(1.0f, 1.0f, 1.0f);
    float opacitySample = 1.0f;
    if ((declared & (1u << 0u)) != 0u)
        baseSample = ReadOnlyTextures[TextureIndices0.x].Sample(ReadOnlySamplers[TextureIndices0.x], uv);
    if ((declared & (1u << 2u)) != 0u)
        ormSample = ReadOnlyTextures[TextureIndices0.z].Sample(ReadOnlySamplers[TextureIndices0.z], uv).xyz;
    if ((declared & (1u << 3u)) != 0u)
        emissiveSample = ReadOnlyTextures[TextureIndices0.w].Sample(ReadOnlySamplers[TextureIndices0.w], uv).xyz;
    if ((declared & (1u << 4u)) != 0u)
        opacitySample = ReadOnlyTextures[TextureIndices1.x].Sample(ReadOnlySamplers[TextureIndices1.x], uv).x;
    const float3 baseColor = max(BaseColorAndAlphaCutoff.rgb * baseSample.rgb, 0.0f);
    const float perceptualRoughness = max(saturate(SurfaceFactors.y * ormSample.g), 0.045f);
    const float metallic = saturate(SurfaceFactors.x * ormSample.b);
    const float3 surfaceBaseColor = baseColor;

    const float alpha = saturate(baseSample.a * opacitySample);
    if (TextureState.z == 1u)
        clip(alpha - BaseColorAndAlphaCutoff.a);
    const float3 emissive = max(EmissiveAndStrength.rgb, 0.0f)
        * max(EmissiveAndStrength.a, 0.0f) * emissiveSample;
    if (TextureState.w == 1u)
        return float4(baseColor + emissive, TextureState.z == 2u ? alpha : 1.0f);

    const float3 N = NormalizeOrFallback(input.ViewNormal, float3(0.0f, 0.0f, -1.0f));
    const float3 V = NormalizeOrFallback(-input.ViewPosition, N);
    // Deterministic neutral preview illumination for the basic-PBR slice.
    // These are renderer-owned non-photometric values; Scene light payloads
    // intentionally remain unconsumed until the next roadmap item.
    const float3 neutralPreviewDirectionToLightView = normalize(float3(0.96f, 0.0f, -0.28f));
    const float3 neutralPreviewRadiance = float3(4.0f, 4.0f, 4.0f);
    const float3 H = NormalizeOrFallback(V + neutralPreviewDirectionToLightView, N);
    const float NoV = saturate(dot(N, V));
    const float NoL = saturate(dot(N, neutralPreviewDirectionToLightView));
    const float NoH = saturate(dot(N, H));
    const float VoH = saturate(dot(V, H));

    // Trowbridge-Reitz GGX NDF using the accepted Filament convention:
    // perceptual p=max(saturate(r), 0.045), then alpha=p*p.
    const float alphaRoughness = perceptualRoughness * perceptualRoughness;
    const float alphaSquared = alphaRoughness * alphaRoughness;
    const float distributionDenominator = NoH * NoH * (alphaSquared - 1.0f) + 1.0f;
    const float D = alphaSquared / max(3.14159265359f
        * distributionDenominator * distributionDenominator, 0.000001f);

    // Height-correlated Smith GGX visibility (the 1/(4 NoL NoV) term is
    // already included in this form).
    const float smithV = NoL * sqrt(NoV * NoV * (1.0f - alphaSquared) + alphaSquared);
    const float smithL = NoV * sqrt(NoL * NoL * (1.0f - alphaSquared) + alphaSquared);
    const float visibility = 0.5f / max(smithV + smithL, 0.000001f);

    // Schlick Fresnel with dielectric F0=0.04, tinted by base color for metals.
    const float3 F0 = lerp(0.04f.xxx, surfaceBaseColor, metallic);
    const float fresnelFactor = pow(1.0f - VoH, 5.0f);
    const float3 F = F0 + (1.0f - F0) * fresnelFactor;
    const float3 specularBrdf = D * visibility * F;

    // Disney/Burley diffuse baseline using Filament's remapped alpha in Fd90.
    // This slice deliberately uses base*(1-metal)*Fd_Burley with no additional
    // (1-F) coupling.
    const float fd90 = 0.5f + 2.0f * alphaRoughness * VoH * VoH;
    const float lightScatter = 1.0f + (fd90 - 1.0f) * pow(1.0f - NoL, 5.0f);
    const float viewScatter = 1.0f + (fd90 - 1.0f) * pow(1.0f - NoV, 5.0f);
    const float3 diffuseBrdf = surfaceBaseColor * (1.0f - metallic)
        * lightScatter * viewScatter / 3.14159265359f;
    // The ORM occlusion channel is reserved for a future indirect-light term;
    // it does not attenuate this direct neutral preview light.
    const float3 direct = NoV > 0.0f && NoL > 0.0f
        ? (diffuseBrdf + specularBrdf) * neutralPreviewRadiance * NoL
        : 0.0f.xxx;
    const float3 shaded = direct + emissive;
    const float3 finiteShaded = all(isfinite(shaded))
        ? max(shaded, 0.0f) : float3(4.0f, 0.0f, 4.0f);
    return float4(finiteShaded,
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
