cbuffer SceneDebugOverlayConstants : register(b0)
{
    float4 Segment0;
    float4 Segment1;
    float4 Segment2;
    float4 Segment3;
    float4 Segment4;
    float4 Segment5;
    float4 Segment6;
    float4 Segment7;
    float4 Segment8;
    float4 Segment9;
    float4 Segment10;
    float4 Segment11;
    float4 OverlayColorAndOpacity;
    // x=segment count, y=viewport width, z=viewport height, w=thickness px.
    float4 OverlayState;
};

Texture2D ResolvedScene : register(t0, space2);
SamplerState ResolvedSceneSampler : register(s0, space2);

struct VSInput
{
    float3 Position : POSITION;
    float3 Color : COLOR;
    float2 UV : TEXCOORD;
};

struct VSOutput
{
    float4 Position : SV_Position;
    float2 UV : TEXCOORD0;
};

VSOutput VSMain(VSInput input)
{
    VSOutput output;
    output.Position = float4(input.Position.xy, 0.0f, 1.0f);
    output.UV = input.UV;
    return output;
}

float4 GetSegment(uint index)
{
    if (index == 0u) return Segment0;
    if (index == 1u) return Segment1;
    if (index == 2u) return Segment2;
    if (index == 3u) return Segment3;
    if (index == 4u) return Segment4;
    if (index == 5u) return Segment5;
    if (index == 6u) return Segment6;
    if (index == 7u) return Segment7;
    if (index == 8u) return Segment8;
    if (index == 9u) return Segment9;
    if (index == 10u) return Segment10;
    return Segment11;
}

float DistanceToSegment(float2 point, float2 first, float2 second)
{
    const float2 direction = second - first;
    const float lengthSquared = dot(direction, direction);
    const float amount = lengthSquared > 0.000001f
        ? saturate(dot(point - first, direction) / lengthSquared) : 0.0f;
    return length(point - (first + direction * amount));
}

float4 PSMain(VSOutput input) : SV_Target0
{
    const float4 source = ResolvedScene.SampleLevel(
        ResolvedSceneSampler, input.UV, 0.0f);
    if (!all(isfinite(OverlayState)) || !all(isfinite(OverlayColorAndOpacity))
        || OverlayState.x < 1.0f || OverlayState.x > 12.0f
        || OverlayState.y < 1.0f || OverlayState.z < 1.0f
        || OverlayState.w <= 0.0f || OverlayColorAndOpacity.a < 0.0f
        || OverlayColorAndOpacity.a > 1.0f)
        return float4(1.0f, 0.0f, 1.0f, 1.0f);
    const uint segmentCount = (uint)OverlayState.x;
    const float2 viewport = OverlayState.yz;
    float distancePixels = 1000000.0f;
    [unroll] for (uint index = 0u; index < 12u; ++index)
    {
        if (index >= segmentCount)
            break;
        const float4 segment = GetSegment(index);
        distancePixels = min(distancePixels, DistanceToSegment(
            input.Position.xy, segment.xy * viewport, segment.zw * viewport));
    }
    const float coverage = 1.0f - smoothstep(
        OverlayState.w - 0.75f, OverlayState.w + 0.75f, distancePixels);
    const float opacity = coverage * OverlayColorAndOpacity.a;
    return float4(lerp(source.rgb, OverlayColorAndOpacity.rgb, opacity), 1.0f);
}
