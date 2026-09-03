cbuffer ViewportConstants : register(b0)
{
    row_major float4x4 ViewProjection;
    float4 BaseColorAndAlphaCutoff;
    float4 EmissiveAndStrength;
    float4 SurfaceFactors;
    float4 CallistoFactors;
    uint4 TextureIndices0;
    uint4 TextureIndices1;
    uint4 TextureState;
};

Texture2D ReadOnlyTextures[4096] : register(t0, space1);
SamplerState ReadOnlySamplers[4096] : register(s0, space1);

struct VSInput
{
    float3 Position : POSITION;
    float3 Color : COLOR;
    float2 UV : TEXCOORD;
};

struct VSOutput
{
    float4 Position : SV_Position;
    float3 Color : COLOR0;
    float2 UV : TEXCOORD0;
};

VSOutput VSMain(VSInput input)
{
    VSOutput output;
    output.Position = mul(float4(input.Position, 1.0f), ViewProjection);
    output.Color = input.Color;
    output.UV = input.UV;
    return output;
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
