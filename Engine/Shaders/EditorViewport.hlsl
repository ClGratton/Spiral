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
    row_major float4x4 ShadowViewProjection;
    float4 ShadowParameters;
    uint4 ShadowState;
    float4 SkyIrradianceUpper;
    float4 SkyIrradianceLower;
};

#ifndef GE_READ_ONLY_TEXTURE_CAPACITY
#define GE_READ_ONLY_TEXTURE_CAPACITY 4096
#endif

Texture2D ReadOnlyTextures[GE_READ_ONLY_TEXTURE_CAPACITY] : register(t0, space1);
SamplerState ReadOnlySamplers[GE_READ_ONLY_TEXTURE_CAPACITY] : register(s0, space1);
#ifndef GE_SCENE_LIGHT_PAYLOAD
#define GE_SCENE_LIGHT_PAYLOAD 0
#endif
// Fixed renderer-owned Scene payload. Production PSMain validates and consumes
// the bounded global/clustered light lists directly from this immutable ABI.
#if GE_SCENE_LIGHT_PAYLOAD
StructuredBuffer<uint4> SceneLightPayload : register(t0, space3);
#endif
#ifndef GE_SCENE_SHADOW_MAP
#define GE_SCENE_SHADOW_MAP 0
#endif
#if GE_SCENE_SHADOW_MAP
Texture2D<float> SceneShadowDepth : register(t0, space2);
SamplerState SceneShadowSampler : register(s0, space2);
#endif

float4 StoreSceneLinearHdr(float3 sceneLinear, float alpha, float preExposure)
{
    if (!all(isfinite(sceneLinear)) || !isfinite(alpha)
        || !isfinite(preExposure) || !(preExposure > 0.0f))
        return float4(4.0f, 0.0f, 4.0f, 1.0f);
#if GE_SCENE_LIGHT_PAYLOAD
    const float maximumInput = 65504.0f / preExposure;
    if (!isfinite(maximumInput) || !(maximumInput > 0.0f))
        return float4(4.0f, 0.0f, 4.0f, 1.0f);
    const float3 nonnegative = max(sceneLinear, 0.0f);
    const float3 exposed = min(nonnegative, maximumInput.xxx) * preExposure;
    return float4(min(exposed, 65504.0f), saturate(alpha));
#else
    return float4(max(sceneLinear, 0.0f), saturate(alpha));
#endif
}

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
    float4 ShadowPosition : TEXCOORD2;
};

VSOutput VSMain(VSInput input)
{
    VSOutput output;
    output.Position = mul(float4(input.Position, 1.0f), ViewProjection);
    output.GeometricNormal = normalize(mul(float4(input.Normal, 0.0f), NormalTransform).xyz);
    output.ViewPosition = mul(float4(input.Position, 1.0f), ModelView).xyz;
    output.ViewNormal = mul(float4(input.Normal, 0.0f), NormalViewTransform).xyz;
    output.ShadowPosition = mul(float4(input.Position, 1.0f), ShadowViewProjection);
    output.Color = input.Color;
    output.UV = input.UV;
    return output;
}

struct ShadowVSOutput
{
    float4 Position : SV_Position;
    float2 UV : TEXCOORD0;
};

ShadowVSOutput VSShadowCaster(VSInput input)
{
    ShadowVSOutput output;
    output.Position = mul(float4(input.Position, 1.0f), ShadowViewProjection);
    output.UV = input.UV;
    return output;
}

void PSShadowCaster(ShadowVSOutput input)
{
    if (ShadowState.w == 1u)
    {
        const uint declared = TextureState.x;
        float alpha = 1.0f;
        if ((declared & (1u << 0u)) != 0u)
            alpha *= ReadOnlyTextures[TextureIndices0.x]
                .Sample(ReadOnlySamplers[TextureIndices0.x], input.UV).a;
        if ((declared & (1u << 4u)) != 0u)
            alpha *= ReadOnlyTextures[TextureIndices1.x]
                .Sample(ReadOnlySamplers[TextureIndices1.x], input.UV).x;
        clip(alpha - BaseColorAndAlphaCutoff.a);
    }
    else if (ShadowState.w > 2u)
    {
        clip(-1.0f);
    }
}

// Smoke-only readback entry point. The production vertex input and b0 payload
// are unchanged; ordinary Editor output continues to compile PSMain.
float4 PSSurfaceBasisMaterialProbe(VSOutput input) : SV_Target0
{
    if (MaterialState.x != 1u || MaterialState.y != 0u)
        return float4(0.7f, 0.0f, 0.0f, 1.0f);
    return float4(0.5f + input.GeometricNormal * 0.25f, 1.0f);
}

#if GE_SCENE_LIGHT_PAYLOAD
float4 EncodeSceneLightPayloadByte(uint4 word, uint byteIndex)
{
    const uint shift = byteIndex * 8u;
    return float4(
        (word.x >> shift) & 255u,
        (word.y >> shift) & 255u,
        (word.z >> shift) & 255u,
        (word.w >> shift) & 255u);
}

// Mutually exclusive native diagnostic entry. Its horizontal cells sample
// all four bytes of varied words from every payload section without changing
// production PSMain light evaluation.
float4 PSLightPayloadProbe(VSOutput input) : SV_Target0
{
    const uint pixelX = (uint)input.Position.x;
    const uint cell = min(pixelX / 4u, 15u);
    const uint4 header1 = SceneLightPayload[1];
    const uint4 header2 = SceneLightPayload[2];
    const uint records = header1.x;
    uint4 word = SceneLightPayload[0];
    if (cell == 1u) word = header1;
    else if (cell == 2u) word = header2;
    else if (cell == 3u) word = SceneLightPayload[5];
    else if (cell == 4u) word = SceneLightPayload[records + 0u];
    else if (cell == 5u) word = SceneLightPayload[records + 1u];
    else if (cell == 6u) word = SceneLightPayload[records + 6u];
    else if (cell == 7u) word = SceneLightPayload[records + 7u];
    else if (cell == 8u) word = SceneLightPayload[records + 13u];
    else if (cell == 9u) word = SceneLightPayload[records + 14u];
    else if (cell == 10u) word = SceneLightPayload[records + 17u];
    else if (cell == 11u) word = SceneLightPayload[records + 18u];
    else if (cell == 12u) word = SceneLightPayload[records + 20u];
    else if (cell == 13u) word = SceneLightPayload[header1.z];
    else if (cell == 14u) word = SceneLightPayload[header2.x + 1u];
    else if (cell == 15u) word = SceneLightPayload[header2.z];
    return EncodeSceneLightPayloadByte(word, pixelX & 3u);
}
#endif

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

#if GE_SCENE_LIGHT_PAYLOAD
struct SceneLightHeader
{
    uint Length;
    uint LightCount;
    uint DirectionalOffset;
    uint DirectionalCount;
    uint OffsetsOffset;
    uint OffsetCount;
    uint LocalOffset;
    uint LocalCount;
    uint ViewportWidth;
    uint ViewportHeight;
    uint TileSize;
    uint DepthSliceCount;
    uint TileCountX;
    uint TileCountY;
    uint MaximumLocalLights;
    float NearClip;
    float FarClip;
    float PreExposure;
};

bool ValidateSceneLightPayload(out SceneLightHeader header)
{
    header = (SceneLightHeader)0;
    uint length, stride;
    SceneLightPayload.GetDimensions(length, stride);
    if (stride != 16u || length < 6u || length > 4194304u)
        return false;
    const uint4 h0 = SceneLightPayload[0];
    const uint4 h1 = SceneLightPayload[1];
    const uint4 h2 = SceneLightPayload[2];
    const uint4 h3 = SceneLightPayload[3];
    const uint4 h4 = SceneLightPayload[4];
    const uint4 h5 = SceneLightPayload[5];
    if (h0.x != 0x504C5347u || h0.y != 3u || h0.z != length
        || h0.w != 6u || h1.x != 6u || h5.z != 7u
        || h1.w > 16u || h1.w > h1.y || h5.x == 0u || h5.x > 64u
        || h3.x == 0u || h3.y == 0u || h3.z == 0u || h3.z > 4096u
        || h3.w == 0u || h3.w > 128u || h4.x == 0u || h4.y == 0u)
        return false;
    const float nearClip = asfloat(h4.z);
    const float farClip = asfloat(h4.w);
    const float preExposure = asfloat(h5.w);
    if (!isfinite(nearClip) || !isfinite(farClip) || !(nearClip > 0.0f)
        || !(farClip > nearClip) || !isfinite(preExposure) || !(preExposure > 0.0f))
        return false;

    if (h1.y > (length - 6u) / 7u)
        return false;
    const uint recordsEnd = 6u + h1.y * 7u;
    if (recordsEnd != h1.z)
        return false;

    const uint maximumScalarCount = length * 4u;
    if (h2.y > maximumScalarCount || h2.w > maximumScalarCount)
        return false;
    const uint directionalWords = (h1.w + 3u) / 4u;
    const uint offsetWords = (h2.y + 3u) / 4u;
    const uint localWords = (h2.w + 3u) / 4u;
    if (h1.z > length || directionalWords > length - h1.z
        || h2.x != h1.z + directionalWords || h2.x > length
        || offsetWords > length - h2.x
        || h2.z != h2.x + offsetWords || h2.z > length
        || localWords != length - h2.z)
        return false;

    if (h4.x != 1u + (h3.x - 1u) / h3.z
        || h4.y != 1u + (h3.y - 1u) / h3.z
        || h4.y > 4194304u / h4.x)
        return false;
    const uint tileCount = h4.x * h4.y;
    if (h3.w > 4194304u / tileCount)
        return false;
    const uint clusterCount = tileCount * h3.w;
    if (h2.y != clusterCount + 1u
        || h2.w > clusterCount * h5.x)
        return false;

    header.Length = length;
    header.LightCount = h1.y;
    header.DirectionalOffset = h1.z;
    header.DirectionalCount = h1.w;
    header.OffsetsOffset = h2.x;
    header.OffsetCount = h2.y;
    header.LocalOffset = h2.z;
    header.LocalCount = h2.w;
    header.ViewportWidth = h3.x;
    header.ViewportHeight = h3.y;
    header.TileSize = h3.z;
    header.DepthSliceCount = h3.w;
    header.TileCountX = h4.x;
    header.TileCountY = h4.y;
    header.MaximumLocalLights = h5.x;
    header.NearClip = nearClip;
    header.FarClip = farClip;
    header.PreExposure = preExposure;
    return true;
}

uint LoadPackedScalar(uint sectionOffset, uint scalarIndex)
{
    return SceneLightPayload[sectionOffset + scalarIndex / 4u][scalarIndex % 4u];
}

bool TryNormalizeDirection(float3 value, out float3 normalized)
{
    normalized = 0.0f.xxx;
    if (!all(isfinite(value)))
        return false;
    const float largest = max(abs(value.x), max(abs(value.y), abs(value.z)));
    if (!(largest > 0.0f))
        return false;
    const float3 scaled = value / largest;
    const float lengthSquared = dot(scaled, scaled);
    if (!isfinite(lengthSquared) || !(lengthSquared > 0.0f))
        return false;
    normalized = scaled * rsqrt(lengthSquared);
    return all(isfinite(normalized));
}
#endif

float3 EvaluateDirectBrdf(float3 surfaceBaseColor, float metallic,
    float perceptualRoughness, float3 N, float3 V, float3 L,
    float3 incidentIlluminance)
{
    const float NoV = saturate(dot(N, V));
    const float NoL = saturate(dot(N, L));
    if (!(NoV > 0.0f) || !(NoL > 0.0f))
        return 0.0f.xxx;
    const float3 H = NormalizeOrFallback(V + L, N);
    const float NoH = saturate(dot(N, H));
    const float VoH = saturate(dot(V, H));
    const float alphaRoughness = perceptualRoughness * perceptualRoughness;
    const float alphaSquared = alphaRoughness * alphaRoughness;
    const float distributionDenominator = NoH * NoH * (alphaSquared - 1.0f) + 1.0f;
    const float D = alphaSquared / max(3.14159265359f
        * distributionDenominator * distributionDenominator, 0.000001f);
    const float smithV = NoL * sqrt(NoV * NoV * (1.0f - alphaSquared) + alphaSquared);
    const float smithL = NoV * sqrt(NoL * NoL * (1.0f - alphaSquared) + alphaSquared);
    const float visibility = 0.5f / max(smithV + smithL, 0.000001f);
    const float3 F0 = lerp(0.04f.xxx, surfaceBaseColor, metallic);
    const float fresnelFactor = pow(1.0f - VoH, 5.0f);
    const float3 F = F0 + (1.0f - F0) * fresnelFactor;
    const float3 specularBrdf = D * visibility * F;
    const float fd90 = 0.5f + 2.0f * alphaRoughness * VoH * VoH;
    const float lightScatter = 1.0f + (fd90 - 1.0f) * pow(1.0f - NoL, 5.0f);
    const float viewScatter = 1.0f + (fd90 - 1.0f) * pow(1.0f - NoV, 5.0f);
    const float3 diffuseBrdf = surfaceBaseColor * (1.0f - metallic)
        * lightScatter * viewScatter / 3.14159265359f;
    return (diffuseBrdf + specularBrdf) * incidentIlluminance * NoL;
}

#if GE_SCENE_SHADOW_MAP
bool ValidateSceneShadowState()
{
    if (ShadowState.x > 1u || ShadowState.z < 64u || ShadowState.z > 8192u
        || !all(isfinite(ShadowParameters)) || !(ShadowParameters.x > 0.0f)
        || ShadowParameters.y < 0.0f || ShadowParameters.z < ShadowParameters.y
        || ShadowParameters.w < 0.0f)
        return false;
    const float expectedInverseResolution = 1.0f / (float)ShadowState.z;
    return abs(ShadowParameters.x - expectedInverseResolution) <= 0.0000001f;
}

float EvaluatePrimaryDirectionalShadow(float4 shadowPosition, float3 N, float3 L)
{
    if (ShadowState.x == 0u)
        return 1.0f;
    if (!all(isfinite(shadowPosition)) || !(abs(shadowPosition.w) > 0.000001f))
        return 0.0f;
    const float3 projected = shadowPosition.xyz / shadowPosition.w;
    const float2 uv = float2(projected.x * 0.5f + 0.5f,
        0.5f - projected.y * 0.5f);
    if (projected.z < 0.0f || projected.z > 1.0f
        || any(uv < 0.0f) || any(uv > 1.0f))
        return 1.0f;
    const float bias = ShadowParameters.y
        + ShadowParameters.z * (1.0f - saturate(dot(N, L)));
    const float receiverDepth = projected.z - bias;
    float visibility = 0.0f;
    [unroll] for (int y = -1; y <= 1; ++y)
    {
        [unroll] for (int x = -1; x <= 1; ++x)
        {
            const float sampledDepth = SceneShadowDepth.SampleLevel(
                SceneShadowSampler, uv + float2(x, y) * ShadowParameters.x, 0.0f);
            visibility += receiverDepth <= sampledDepth ? 1.0f : 0.0f;
        }
    }
    return visibility / 9.0f;
}
#endif

#if GE_SCENE_LIGHT_PAYLOAD
bool EvaluateSceneLightRecord(uint lightIndex, bool requireDirectional,
    float3 viewPosition, float4 shadowPosition,
    float3 surfaceBaseColor, float metallic,
    float perceptualRoughness, float3 N, float3 V, inout float3 direct)
{
    const uint base = 6u + lightIndex * 7u;
    const uint4 meta = SceneLightPayload[base];
    const float photometricValue = asfloat(SceneLightPayload[base + 1u].z);
    const float4 positionAndRange = asfloat(SceneLightPayload[base + 2u]);
    const float4 worldDirectionAndInner = asfloat(SceneLightPayload[base + 3u]);
    const float4 viewDirectionAndOuter = asfloat(SceneLightPayload[base + 4u]);
    const float4 authoredColorAndInverseSpan = asfloat(SceneLightPayload[base + 5u]);
    const float4 coefficientAndInverseRange = asfloat(SceneLightPayload[base + 6u]);
    if (meta.x == 0u || meta.w > 1u || !isfinite(photometricValue)
        || photometricValue < 0.0f || !all(isfinite(positionAndRange))
        || !all(isfinite(worldDirectionAndInner)) || !all(isfinite(viewDirectionAndOuter))
        || !all(isfinite(authoredColorAndInverseSpan))
        || !all(isfinite(coefficientAndInverseRange))
        || any(authoredColorAndInverseSpan.xyz < 0.0f)
        || any(coefficientAndInverseRange.xyz < 0.0f)
        || positionAndRange.w < 0.0f || authoredColorAndInverseSpan.w < 0.0f
        || coefficientAndInverseRange.w < 0.0f)
        return false;

    if (requireDirectional)
    {
        if (meta.y != 0u || meta.z != 0u
            || authoredColorAndInverseSpan.w != 0.0f
            || coefficientAndInverseRange.w != 0.0f)
            return false;
        float3 emission;
        if (!TryNormalizeDirection(viewDirectionAndOuter.xyz, emission))
            return false;
        const float3 L = -emission;
        float shadowVisibility = 1.0f;
#if GE_SCENE_SHADOW_MAP
        if (ShadowState.x == 1u && lightIndex == ShadowState.y)
        {
            if (meta.w != 1u)
                return false;
            shadowVisibility = EvaluatePrimaryDirectionalShadow(
                shadowPosition, N, L);
        }
#endif
        const float3 contribution = EvaluateDirectBrdf(surfaceBaseColor, metallic,
            perceptualRoughness, N, V, L,
            coefficientAndInverseRange.xyz * shadowVisibility);
        direct += contribution;
        return all(isfinite(direct));
    }

    if ((meta.y != 1u && meta.y != 2u) || meta.z != 1u)
        return false;
    const float range = positionAndRange.w;
    const float inverseRange = coefficientAndInverseRange.w;
    if ((range == 0.0f) != (inverseRange == 0.0f))
        return false;
    if (meta.y == 1u && authoredColorAndInverseSpan.w != 0.0f)
        return false;
    if (meta.y == 2u)
    {
        const float inner = worldDirectionAndInner.w;
        const float outer = viewDirectionAndOuter.w;
        if (inner < -1.0f || inner > 1.0f || outer < -1.0f
            || outer > 1.0f || inner < outer
            || (inner == 1.0f && outer == 1.0f && photometricValue > 0.0f)
            || ((inner > outer) != (authoredColorAndInverseSpan.w > 0.0f)))
            return false;
    }
    const float3 delta = positionAndRange.xyz - viewPosition;
    const float distanceSquared = dot(delta, delta);
    if (!isfinite(distanceSquared) || !(distanceSquared > 0.0f) || !(range > 0.0f))
        return isfinite(distanceSquared) && distanceSquared >= 0.0f;
    const float distance = sqrt(distanceSquared);
    const float3 L = delta / distance;
    const float distanceOverRangeSquared = distanceSquared * inverseRange * inverseRange;
    const float rangeWindowBase = saturate(1.0f
        - distanceOverRangeSquared * distanceOverRangeSquared);
    float attenuation = rangeWindowBase * rangeWindowBase
        / max(distanceSquared, 0.0001f);
    if (meta.y == 2u)
    {
        float3 emission;
        if (!TryNormalizeDirection(viewDirectionAndOuter.xyz, emission))
            return false;
        const float inner = worldDirectionAndInner.w;
        const float outer = viewDirectionAndOuter.w;
        const float angular = inner == outer
            ? (dot(emission, -L) >= outer ? 1.0f : 0.0f)
            : saturate((dot(emission, -L) - outer)
                * authoredColorAndInverseSpan.w);
        attenuation *= inner == outer ? angular : angular * angular;
    }
    direct += EvaluateDirectBrdf(surfaceBaseColor, metallic,
        perceptualRoughness, N, V, L, coefficientAndInverseRange.xyz * attenuation);
    return all(isfinite(direct));
}
#endif

float4 PSMain(VSOutput input) : SV_Target0
{
    float preExposure = 1.0f;
#if GE_SCENE_LIGHT_PAYLOAD
    SceneLightHeader lightHeader;
    if (!ValidateSceneLightPayload(lightHeader))
        return float4(4.0f, 0.0f, 4.0f, 1.0f);
    preExposure = lightHeader.PreExposure;
#endif
#if GE_SCENE_SHADOW_MAP
    if (!ValidateSceneShadowState())
        return float4(4.0f, 0.0f, 4.0f, 1.0f);
#endif
    if (!all(isfinite(SkyIrradianceUpper))
        || !all(isfinite(SkyIrradianceLower))
        || any(SkyIrradianceUpper.rgb < 0.0f)
        || any(SkyIrradianceLower.rgb < 0.0f)
        || (SkyIrradianceUpper.w != 0.0f && SkyIrradianceUpper.w != 1.0f)
        || SkyIrradianceLower.w != 0.0f)
        return float4(4.0f, 0.0f, 4.0f, 1.0f);
    // Row zero is never a persistent material identity. It is a deliberately
    // obvious deterministic error result in scene-linear HDR.
    if (MaterialState.x == 0u || MaterialState.y != 0u)
        return StoreSceneLinearHdr(float3(4.0f, 0.0f, 4.0f), 1.0f, preExposure);

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
        return StoreSceneLinearHdr(baseColor + emissive,
            TextureState.z == 2u ? alpha : 1.0f, preExposure);

    const float3 N = NormalizeOrFallback(input.ViewNormal, float3(0.0f, 0.0f, -1.0f));
    const float3 V = NormalizeOrFallback(-input.ViewPosition, N);
#if GE_SCENE_LIGHT_PAYLOAD
    float3 direct = 0.0f.xxx;
    [loop] for (uint i = 0u; i < lightHeader.DirectionalCount; ++i)
    {
        const uint index = LoadPackedScalar(lightHeader.DirectionalOffset, i);
        if (index >= lightHeader.LightCount || !EvaluateSceneLightRecord(index,
            true, input.ViewPosition, input.ShadowPosition, surfaceBaseColor, metallic,
            perceptualRoughness, N, V, direct))
            return StoreSceneLinearHdr(float3(4.0f, 0.0f, 4.0f), 1.0f, preExposure);
    }

    if (!all(isfinite(input.Position.xy)) || !isfinite(input.ViewPosition.z))
        return StoreSceneLinearHdr(float3(4.0f, 0.0f, 4.0f), 1.0f, preExposure);
    const uint tileX = min((uint)max(input.Position.x, 0.0f)
        / lightHeader.TileSize, lightHeader.TileCountX - 1u);
    const uint tileY = min((uint)max(input.Position.y, 0.0f)
        / lightHeader.TileSize, lightHeader.TileCountY - 1u);
    const float clampedDepth = clamp(input.ViewPosition.z,
        lightHeader.NearClip, lightHeader.FarClip);
    const float normalizedDepth = log(clampedDepth / lightHeader.NearClip)
        / log(lightHeader.FarClip / lightHeader.NearClip);
    if (!isfinite(normalizedDepth))
        return StoreSceneLinearHdr(float3(4.0f, 0.0f, 4.0f), 1.0f, preExposure);
    const uint depthSlice = min((uint)(saturate(normalizedDepth)
        * lightHeader.DepthSliceCount), lightHeader.DepthSliceCount - 1u);
    const uint clusterIndex = (depthSlice * lightHeader.TileCountY + tileY)
        * lightHeader.TileCountX + tileX;
    const uint begin = LoadPackedScalar(lightHeader.OffsetsOffset, clusterIndex);
    const uint end = LoadPackedScalar(lightHeader.OffsetsOffset, clusterIndex + 1u);
    if (begin > end || end > lightHeader.LocalCount
        || end - begin > lightHeader.MaximumLocalLights || end - begin > 64u)
        return StoreSceneLinearHdr(float3(4.0f, 0.0f, 4.0f), 1.0f, preExposure);
    [loop] for (uint cursor = begin; cursor < end; ++cursor)
    {
        const uint index = LoadPackedScalar(lightHeader.LocalOffset, cursor);
        if (index >= lightHeader.LightCount || !EvaluateSceneLightRecord(index,
            false, input.ViewPosition, input.ShadowPosition, surfaceBaseColor, metallic,
            perceptualRoughness, N, V, direct))
            return StoreSceneLinearHdr(float3(4.0f, 0.0f, 4.0f), 1.0f, preExposure);
    }
    const float skyWeight = saturate(input.GeometricNormal.y * 0.5f + 0.5f);
    const float3 skyIrradiance = lerp(SkyIrradianceLower.rgb,
        SkyIrradianceUpper.rgb, skyWeight) * SkyIrradianceUpper.w;
    const float ambientOcclusion = lerp(1.0f, saturate(ormSample.r),
        saturate(SurfaceFactors.w));
    const float3 skyDiffuse = surfaceBaseColor * (1.0f - metallic)
        * skyIrradiance * ambientOcclusion / 3.14159265359f;
    const float3 shaded = direct + skyDiffuse + emissive;
#else
    // Shader-tool probes without the Scene payload retain deterministic
    // illumination, but every production Scene permutation defines it.
    const float3 previewL = normalize(float3(0.96f, 0.0f, -0.28f));
    const float3 shaded = EvaluateDirectBrdf(surfaceBaseColor, metallic,
        perceptualRoughness, N, V, previewL, 4.0f.xxx) + emissive;
#endif
    const float3 finiteShaded = all(isfinite(shaded))
        ? max(shaded, 0.0f) : float3(4.0f, 0.0f, 4.0f);
    return StoreSceneLinearHdr(finiteShaded,
        TextureState.z == 2u ? alpha : 1.0f, preExposure);
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
