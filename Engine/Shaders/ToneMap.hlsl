cbuffer ToneMapConstants : register(b0)
{
    // x = exposure in EV100 stops, y = paper-white scale,
    // z = post-tone-map saturation, w = post-tone-map contrast.
    float4 ExposureAndOutput;
};

Texture2D HdrScene : register(t0, space2);
SamplerState HdrSceneSampler : register(s0, space2);

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

// Khronos PBR Neutral tone mapping. It preserves neutral colors and avoids the
// strong hue shifts of independent per-channel compression.
float3 NeutralToneMap(float3 color)
{
    const float startCompression = 0.76f;
    const float desaturation = 0.15f;
    color = min(color, 6.25f);

    float offset = min(color.r, min(color.g, color.b));
    offset = offset < 0.08f ? offset - 6.25f * offset * offset : 0.04f;
    color -= offset;

    const float peak = max(color.r, max(color.g, color.b));
    if (peak < startCompression)
        return color;

    const float distance = 1.0f - startCompression;
    const float newPeak = 1.0f - distance * distance / (peak + distance - startCompression);
    color *= newPeak / peak;
    const float desaturationAmount = 1.0f - 1.0f / (desaturation * (peak - newPeak) + 1.0f);
    return lerp(color, newPeak.xxx, desaturationAmount);
}

float3 LinearToSrgb(float3 color)
{
    const float3 low = color * 12.92f;
    const float3 high = 1.055f * pow(max(color, 0.0f), 1.0f / 2.4f) - 0.055f;
    return lerp(high, low, step(color, 0.0031308f));
}

float3 ApplyPostToneMapGrade(float3 displayLinear)
{
    // Preserve the pre-grading output path exactly for project defaults. The
    // algebraic identity form can still round at float/UNORM boundaries.
    if (ExposureAndOutput.z == 1.0f && ExposureAndOutput.w == 1.0f)
        return displayLinear;

    const float luminance = dot(displayLinear, float3(0.2126f, 0.7152f, 0.0722f));
    const float3 saturated = lerp(luminance.xxx, displayLinear, ExposureAndOutput.z);
    return (saturated - 0.5f) * ExposureAndOutput.w + 0.5f;
}

float4 PSMain(VSOutput input) : SV_Target0
{
    const float exposure = exp2(-ExposureAndOutput.x) * ExposureAndOutput.y;
    const float3 hdr = max(HdrScene.SampleLevel(HdrSceneSampler, input.UV, 0.0f).rgb, 0.0f);
    const float3 displayLinear = saturate(NeutralToneMap(hdr * exposure));
    const float3 graded = saturate(ApplyPostToneMapGrade(displayLinear));
    return float4(LinearToSrgb(graded), 1.0f);
}
