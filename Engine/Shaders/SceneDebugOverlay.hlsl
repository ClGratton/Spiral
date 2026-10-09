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
    // Inverse view depth at both endpoints of two segments: xy=even, zw=odd.
    float4 SegmentDepth0;
    float4 SegmentDepth1;
    float4 SegmentDepth2;
    float4 SegmentDepth3;
    float4 SegmentDepth4;
    float4 SegmentDepth5;
    // rgb=selection color, a=visible opacity.
    float4 OverlayColorAndOpacity;
    // x=segment count, y=viewport width, z=viewport height, w=visible thickness px.
    float4 OverlayState;
    // x=draw occluded edges (0/1), y=occluded opacity, z=occluded thickness px,
    // w=absolute inverse-depth quantization tolerance.
    float4 OccludedState;
    // x=ndc depth scale, y=ndc depth offset (ndc = x + y / viewZ),
    // z=relative inverse-depth tolerance, w=slope footprint in pixels.
    float4 DepthState;
};

// The Scene depth the raster pass wrote (zero-to-one, standard Z), sampled with
// point filtering. The overlay reads it instead of binding it as an attachment.
Texture2D<float> SceneDepth : register(t0, space2);
SamplerState SceneDepthSampler : register(s0, space2);

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

float4 GetSegmentDepthPair(uint pair)
{
    if (pair == 0u) return SegmentDepth0;
    if (pair == 1u) return SegmentDepth1;
    if (pair == 2u) return SegmentDepth2;
    if (pair == 3u) return SegmentDepth3;
    if (pair == 4u) return SegmentDepth4;
    return SegmentDepth5;
}

float2 GetSegmentDepth(uint index)
{
    const float4 pair = GetSegmentDepthPair(index >> 1u);
    return (index & 1u) != 0u ? pair.zw : pair.xy;
}

float DistanceToSegment(float2 point, float2 first, float2 second,
    out float amount)
{
    const float2 direction = second - first;
    const float lengthSquared = dot(direction, direction);
    amount = lengthSquared > 0.000001f
        ? saturate(dot(point - first, direction) / lengthSquared) : 0.0f;
    return length(point - (first + direction * amount));
}

// Inverse view depth (1 / viewZ, larger is nearer) of the Scene at a UV.
float SampleSceneInverseDepth(float2 uv)
{
    const float ndcDepth = SceneDepth.SampleLevel(SceneDepthSampler, uv, 0.0f);
    return (ndcDepth - DepthState.x) / DepthState.y;
}

float4 PSMain(VSOutput input) : SV_Target0
{
    if (!all(isfinite(OverlayState)) || !all(isfinite(OverlayColorAndOpacity))
        || !all(isfinite(OccludedState)) || !all(isfinite(DepthState))
        || OverlayState.x < 1.0f || OverlayState.x > 12.0f
        || OverlayState.y < 1.0f || OverlayState.z < 1.0f
        || OverlayState.w <= 0.0f || OverlayColorAndOpacity.a < 0.0f
        || OverlayColorAndOpacity.a > 1.0f
        || OccludedState.y < 0.0f || OccludedState.y > 1.0f
        || OccludedState.z <= 0.0f
        || DepthState.x <= 1.0f || DepthState.y >= 0.0f)
        return float4(1.0f, 0.0f, 1.0f, 1.0f);
    const uint segmentCount = (uint)OverlayState.x;
    const float2 viewport = OverlayState.yz;
    const float feather = 0.75f;
    const float reach = max(OverlayState.w,
        OccludedState.x > 0.5f ? OccludedState.z : 0.0f) + feather;

    // The Scene depth is sampled lazily: most pixels are nowhere near an edge.
    bool depthLoaded = false;
    float sceneU = 0.0f;
    float sceneGradient = 0.0f;
    float visibleAlpha = 0.0f;
    float occludedAlpha = 0.0f;
    [unroll] for (uint index = 0u; index < 12u; ++index)
    {
        if (index >= segmentCount)
            break;
        const float4 segment = GetSegment(index);
        float amount;
        const float distancePixels = DistanceToSegment(input.Position.xy,
            segment.xy * viewport, segment.zw * viewport, amount);
        if (distancePixels >= reach)
            continue;
        if (!depthLoaded)
        {
            depthLoaded = true;
            const float2 texel = 1.0f / viewport;
            sceneU = SampleSceneInverseDepth(input.UV);
            const float left = SampleSceneInverseDepth(input.UV - float2(texel.x, 0.0f));
            const float right = SampleSceneInverseDepth(input.UV + float2(texel.x, 0.0f));
            const float up = SampleSceneInverseDepth(input.UV - float2(0.0f, texel.y));
            const float down = SampleSceneInverseDepth(input.UV + float2(0.0f, texel.y));
            // Inverse depth is affine in screen space on a planar face. The
            // smaller one-sided difference per axis ignores a depth cliff on
            // the far side of a silhouette while keeping the face's own slope.
            sceneGradient = length(float2(
                min(abs(sceneU - left), abs(right - sceneU)),
                min(abs(sceneU - up), abs(down - sceneU))));
        }
        const float2 depths = GetSegmentDepth(index);
        const float edgeU = lerp(depths.x, depths.y, amount);
        const float tolerance = DepthState.z * sceneU + OccludedState.w
            + DepthState.w * sceneGradient;
        if (edgeU >= sceneU - tolerance)
        {
            const float coverage = 1.0f - smoothstep(
                OverlayState.w - feather, OverlayState.w + feather, distancePixels);
            visibleAlpha = max(visibleAlpha, coverage * OverlayColorAndOpacity.a);
        }
        else if (OccludedState.x > 0.5f)
        {
            const float coverage = 1.0f - smoothstep(
                OccludedState.z - feather, OccludedState.z + feather, distancePixels);
            occludedAlpha = max(occludedAlpha, coverage * OccludedState.y);
        }
    }
    // Straight alpha; the pipeline's fixed-function blend composes it over the
    // tone-mapped color already in the target.
    return float4(OverlayColorAndOpacity.rgb, max(visibleAlpha, occludedAlpha));
}
