cbuffer SkyAtmosphereConstants : register(b0)
{
    // x=tan(verticalFov/2), y=aspect, z=pre-exposure, w=reserved.
    float4 ProjectionAndExposure;
    // xyz=surface-to-sun direction in view space, w=cos(solar radius).
    float4 SunDirectionAndCosRadius;
    // xyz=world up in view space, w=enabled.
    float4 ViewUpAndEnabled;
    // xyz=zenith x,y,Y(cd/m^2), w=sun zenith angle.
    float4 ZenithxyYAndSunTheta;
    float4 LuminancePerezABCD;
    float4 ChromaticityXPerezABCD;
    float4 ChromaticityYPerezABCD;
    // x=luminance E, y=chromaticity-x E, z=chromaticity-y E.
    float4 PerezE;
    float4 SunRadiance;
    float4 GroundRadiance;
};

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

float Perez(float theta, float gamma, float4 abcd, float e)
{
    const float cosineTheta = max(cos(theta), 0.01f);
    const float cosineGamma = cos(gamma);
    return (1.0f + abcd.x * exp(abcd.y / cosineTheta))
        * (1.0f + abcd.z * exp(abcd.w * gamma)
            + e * cosineGamma * cosineGamma);
}

float3 XyYToLinearRgb(float x, float y, float luminance)
{
    y = max(y, 0.000001f);
    const float X = x * luminance / y;
    const float Z = (1.0f - x - y) * luminance / y;
    return max(float3(
        3.2406f * X - 1.5372f * luminance - 0.4986f * Z,
        -0.9689f * X + 1.8758f * luminance + 0.0415f * Z,
        0.0557f * X - 0.2040f * luminance + 1.0570f * Z), 0.0f);
}

float3 EvaluateSky(float3 viewDirection, float height)
{
    const float theta = acos(saturate(height));
    const float gamma = acos(clamp(dot(viewDirection,
        SunDirectionAndCosRadius.xyz), -1.0f, 1.0f));
    const float denominatorY = Perez(0.0f, ZenithxyYAndSunTheta.w,
        LuminancePerezABCD, PerezE.x);
    const float denominatorX = Perez(0.0f, ZenithxyYAndSunTheta.w,
        ChromaticityXPerezABCD, PerezE.y);
    const float denominatory = Perez(0.0f, ZenithxyYAndSunTheta.w,
        ChromaticityYPerezABCD, PerezE.z);
    const float x = ZenithxyYAndSunTheta.x
        * Perez(theta, gamma, ChromaticityXPerezABCD, PerezE.y)
        / max(denominatorX, 0.000001f);
    const float y = ZenithxyYAndSunTheta.y
        * Perez(theta, gamma, ChromaticityYPerezABCD, PerezE.z)
        / max(denominatory, 0.000001f);
    const float Y = ZenithxyYAndSunTheta.z
        * Perez(theta, gamma, LuminancePerezABCD, PerezE.x)
        / max(denominatorY, 0.000001f);
    return XyYToLinearRgb(x, y, max(Y, 0.0f));
}

float4 StorePreExposedSky(float3 radiance)
{
    const float preExposure = ProjectionAndExposure.z;
    if (!all(isfinite(radiance)) || !isfinite(preExposure)
        || !(preExposure > 0.0f))
        return float4(4.0f, 0.0f, 4.0f, 1.0f);
    const float maximumInput = 65504.0f / preExposure;
    if (!isfinite(maximumInput) || !(maximumInput > 0.0f))
        return float4(4.0f, 0.0f, 4.0f, 1.0f);
    return float4(min(max(radiance, 0.0f), maximumInput.xxx)
        * preExposure, 1.0f);
}

float4 PSMain(VSOutput input) : SV_Target0
{
    if (ViewUpAndEnabled.w != 1.0f
        || !all(isfinite(ProjectionAndExposure))
        || !all(isfinite(SunDirectionAndCosRadius))
        || !all(isfinite(ViewUpAndEnabled))
        || !all(isfinite(ZenithxyYAndSunTheta))
        || !all(isfinite(LuminancePerezABCD))
        || !all(isfinite(ChromaticityXPerezABCD))
        || !all(isfinite(ChromaticityYPerezABCD))
        || !all(isfinite(PerezE)) || !all(isfinite(SunRadiance))
        || !all(isfinite(GroundRadiance))
        || !(ProjectionAndExposure.x > 0.0f)
        || !(ProjectionAndExposure.y > 0.0f))
        return float4(4.0f, 0.0f, 4.0f, 1.0f);

    const float2 ndc = float2(input.UV.x * 2.0f - 1.0f,
        1.0f - input.UV.y * 2.0f);
    const float3 viewDirection = normalize(float3(
        ndc.x * ProjectionAndExposure.y * ProjectionAndExposure.x,
        ndc.y * ProjectionAndExposure.x, 1.0f));
    const float height = dot(viewDirection, ViewUpAndEnabled.xyz);
    const float3 sky = EvaluateSky(viewDirection, max(height, 0.0f));
    const float horizonBlend = smoothstep(-0.02f, 0.02f, height);
    float3 radiance = lerp(GroundRadiance.rgb, sky, horizonBlend);
    if (height > 0.0f
        && dot(viewDirection, SunDirectionAndCosRadius.xyz)
            >= SunDirectionAndCosRadius.w)
        radiance += SunRadiance.rgb;
    return StorePreExposedSky(radiance);
}
