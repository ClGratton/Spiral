#include "Engine/Renderer/SceneSkyAtmosphere.h"

#include "Engine/Scene/Components.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace Engine
{
    namespace
    {
        constexpr double kMinimumSkyCosine = 0.01;
        constexpr size_t kSkyIrradianceSampleCount = 512;

        bool Finite(const Math::Vec3& value)
        {
            return std::isfinite(value.X) && std::isfinite(value.Y)
                && std::isfinite(value.Z);
        }

        bool NonnegativeFinite(const Math::Vec3& value)
        {
            return Finite(value) && value.X >= 0.0f && value.Y >= 0.0f
                && value.Z >= 0.0f;
        }

        bool Finite(const SceneSkyPerezCoefficients& value)
        {
            return std::isfinite(value.A) && std::isfinite(value.B)
                && std::isfinite(value.C) && std::isfinite(value.D)
                && std::isfinite(value.E);
        }

        double Dot(const Math::Vec3& left, const Math::Vec3& right)
        {
            return static_cast<double>(left.X) * right.X
                + static_cast<double>(left.Y) * right.Y
                + static_cast<double>(left.Z) * right.Z;
        }

        bool Normalize(const Math::Vec3& value, Math::Vec3& out)
        {
            if (!Finite(value))
                return false;
            const double lengthSquared = Dot(value, value);
            if (!std::isfinite(lengthSquared) || !(lengthSquared > 0.0))
                return false;
            const double reciprocalLength = 1.0 / std::sqrt(lengthSquared);
            out = {
                static_cast<float>(value.X * reciprocalLength),
                static_cast<float>(value.Y * reciprocalLength),
                static_cast<float>(value.Z * reciprocalLength)
            };
            return Finite(out);
        }

        Math::Vec3 TransformDirection(const Math::Vec3& direction,
            const Math::Mat4& matrix)
        {
            return {
                direction.X * matrix.Values[0] + direction.Y * matrix.Values[4]
                    + direction.Z * matrix.Values[8],
                direction.X * matrix.Values[1] + direction.Y * matrix.Values[5]
                    + direction.Z * matrix.Values[9],
                direction.X * matrix.Values[2] + direction.Y * matrix.Values[6]
                    + direction.Z * matrix.Values[10]
            };
        }

        Math::Vec3 LightEmissionDirection(const Math::Vec3& rotationDegrees)
        {
            const float yaw = Math::DegreesToRadians(rotationDegrees.Y);
            const float pitch = Math::DegreesToRadians(rotationDegrees.X);
            return {
                std::sin(yaw) * std::cos(pitch),
                -std::sin(pitch),
                std::cos(yaw) * std::cos(pitch)
            };
        }

        SceneSkyPerezCoefficients LuminanceCoefficients(double turbidity)
        {
            return {
                static_cast<float>(0.1787 * turbidity - 1.4630),
                static_cast<float>(-0.3554 * turbidity + 0.4275),
                static_cast<float>(-0.0227 * turbidity + 5.3251),
                static_cast<float>(0.1206 * turbidity - 2.5771),
                static_cast<float>(-0.0670 * turbidity + 0.3703)
            };
        }

        SceneSkyPerezCoefficients ChromaticityXCoefficients(double turbidity)
        {
            return {
                static_cast<float>(-0.0193 * turbidity - 0.2592),
                static_cast<float>(-0.0665 * turbidity + 0.0008),
                static_cast<float>(-0.0004 * turbidity + 0.2125),
                static_cast<float>(-0.0641 * turbidity - 0.8989),
                static_cast<float>(-0.0033 * turbidity + 0.0452)
            };
        }

        SceneSkyPerezCoefficients ChromaticityYCoefficients(double turbidity)
        {
            return {
                static_cast<float>(-0.0167 * turbidity - 0.2608),
                static_cast<float>(-0.0950 * turbidity + 0.0092),
                static_cast<float>(-0.0079 * turbidity + 0.2102),
                static_cast<float>(-0.0441 * turbidity - 1.6537),
                static_cast<float>(-0.0109 * turbidity + 0.0529)
            };
        }

        double ZenithChromaticityX(double theta, double turbidity)
        {
            const double theta2 = theta * theta;
            const double theta3 = theta2 * theta;
            return (0.00165 * theta3 - 0.00374 * theta2 + 0.00208 * theta)
                    * turbidity * turbidity
                + (-0.02902 * theta3 + 0.06377 * theta2 - 0.03202 * theta
                    + 0.00394) * turbidity
                + (0.11693 * theta3 - 0.21196 * theta2 + 0.06052 * theta
                    + 0.25885);
        }

        double ZenithChromaticityY(double theta, double turbidity)
        {
            const double theta2 = theta * theta;
            const double theta3 = theta2 * theta;
            return (0.00275 * theta3 - 0.00610 * theta2 + 0.00317 * theta)
                    * turbidity * turbidity
                + (-0.04214 * theta3 + 0.08970 * theta2 - 0.04153 * theta
                    + 0.00516) * turbidity
                + (0.15346 * theta3 - 0.26756 * theta2 + 0.06669 * theta
                    + 0.26688);
        }

        double ZenithLuminance(double theta, double turbidity)
        {
            const double chi = (4.0 / 9.0 - turbidity / 120.0)
                * (std::numbers::pi - 2.0 * theta);
            // The Preetham fit returns kcd/m^2.
            return ((4.0453 * turbidity - 4.9710) * std::tan(chi)
                - 0.2155 * turbidity + 2.4192) * 1000.0;
        }

        double Perez(double theta, double gamma,
            const SceneSkyPerezCoefficients& coefficients)
        {
            const double cosineTheta = std::max(std::cos(theta), kMinimumSkyCosine);
            const double cosineGamma = std::cos(gamma);
            return (1.0 + coefficients.A * std::exp(coefficients.B / cosineTheta))
                * (1.0 + coefficients.C * std::exp(coefficients.D * gamma)
                    + coefficients.E * cosineGamma * cosineGamma);
        }

        bool TryXyYToLinearRgb(double x, double y, double luminance,
            Math::Vec3& out)
        {
            if (!std::isfinite(x) || !std::isfinite(y)
                || !std::isfinite(luminance) || !(y > 0.0) || luminance < 0.0)
                return false;
            const double X = x * luminance / y;
            const double Z = (1.0 - x - y) * luminance / y;
            const double red = 3.2406 * X - 1.5372 * luminance - 0.4986 * Z;
            const double green = -0.9689 * X + 1.8758 * luminance + 0.0415 * Z;
            const double blue = 0.0557 * X - 0.2040 * luminance + 1.0570 * Z;
            if (!std::isfinite(red) || !std::isfinite(green) || !std::isfinite(blue)
                || red > std::numeric_limits<float>::max()
                || green > std::numeric_limits<float>::max()
                || blue > std::numeric_limits<float>::max())
                return false;
            out = {
                static_cast<float>(std::max(red, 0.0)),
                static_cast<float>(std::max(green, 0.0)),
                static_cast<float>(std::max(blue, 0.0))
            };
            return NonnegativeFinite(out);
        }

        bool EvaluateSky(const SceneSkyAtmosphereFrame& frame,
            const Math::Vec3& direction, Math::Vec3& out)
        {
            const double cosineTheta = std::clamp(
                static_cast<double>(direction.Y), 0.0, 1.0);
            const double theta = std::acos(cosineTheta);
            const double gamma = std::acos(std::clamp(
                Dot(direction, frame.SurfaceToSunWorld), -1.0, 1.0));
            const double denominatorY = Perez(0.0, frame.SunZenithRadians,
                frame.LuminancePerez);
            const double denominatorX = Perez(0.0, frame.SunZenithRadians,
                frame.ChromaticityXPerez);
            const double denominatory = Perez(0.0, frame.SunZenithRadians,
                frame.ChromaticityYPerez);
            if (!std::isfinite(denominatorY) || !std::isfinite(denominatorX)
                || !std::isfinite(denominatory) || !(denominatorY > 0.0)
                || !(denominatorX > 0.0) || !(denominatory > 0.0))
                return false;
            const double x = frame.ZenithChromaticityX
                * Perez(theta, gamma, frame.ChromaticityXPerez) / denominatorX;
            const double y = frame.ZenithChromaticityY
                * Perez(theta, gamma, frame.ChromaticityYPerez) / denominatory;
            const double Y = frame.ZenithLuminanceCdPerSquareMeter
                * Perez(theta, gamma, frame.LuminancePerez) / denominatorY;
            return TryXyYToLinearRgb(x, y, Y, out);
        }

        bool ComputeUpperDiffuseIrradiance(const SceneSkyAtmosphereFrame& frame,
            Math::Vec3& out)
        {
            // Equal-solid-angle Fibonacci samples make the integral stable and
            // deterministic without introducing a runtime cubemap prerequisite.
            constexpr double goldenAngle = std::numbers::pi
                * (3.0 - 2.2360679774997896964);
            double red = 0.0;
            double green = 0.0;
            double blue = 0.0;
            for (size_t index = 0; index < kSkyIrradianceSampleCount; ++index)
            {
                const double y = (static_cast<double>(index) + 0.5)
                    / static_cast<double>(kSkyIrradianceSampleCount);
                const double radius = std::sqrt(std::max(1.0 - y * y, 0.0));
                const double azimuth = goldenAngle * static_cast<double>(index);
                const Math::Vec3 direction {
                    static_cast<float>(radius * std::cos(azimuth)),
                    static_cast<float>(y),
                    static_cast<float>(radius * std::sin(azimuth))
                };
                Math::Vec3 radiance;
                if (!EvaluateSky(frame, direction, radiance))
                    return false;
                red += static_cast<double>(radiance.X) * y;
                green += static_cast<double>(radiance.Y) * y;
                blue += static_cast<double>(radiance.Z) * y;
            }
            const double solidAngle = 2.0 * std::numbers::pi
                / static_cast<double>(kSkyIrradianceSampleCount);
            red *= solidAngle;
            green *= solidAngle;
            blue *= solidAngle;
            if (!std::isfinite(red) || !std::isfinite(green)
                || !std::isfinite(blue)
                || red > std::numeric_limits<float>::max()
                || green > std::numeric_limits<float>::max()
                || blue > std::numeric_limits<float>::max())
                return false;
            out = { static_cast<float>(red), static_cast<float>(green),
                static_cast<float>(blue) };
            return NonnegativeFinite(out);
        }

        bool ValidProjection(const CameraView& view, float& outTanHalfFov,
            float& outAspect)
        {
            const float xScale = view.Projection.Values[0];
            const float yScale = view.Projection.Values[5];
            if (!std::isfinite(xScale) || !std::isfinite(yScale)
                || !(xScale > 0.0f) || !(yScale > 0.0f))
                return false;
            outTanHalfFov = 1.0f / yScale;
            outAspect = yScale / xScale;
            return std::isfinite(outTanHalfFov) && outTanHalfFov > 0.0f
                && std::isfinite(outAspect) && outAspect > 0.0f;
        }
    }

    bool TryPrepareSceneSkyAtmosphere(const SceneRenderSnapshot& snapshot,
        size_t viewIndex, SceneSkyAtmosphereFrame& outFrame,
        std::string& outError)
    {
        const auto fail = [&outError](const char* message)
        {
            outError = message;
            return false;
        };
        if (viewIndex >= snapshot.Views.size()
            || !snapshot.Views[viewIndex].Camera.Valid)
            return fail("sky atmosphere requires a valid snapshot view");

        const CameraView& view = snapshot.Views[viewIndex].Camera;
        SceneSkyAtmosphereFrame candidate;
        size_t lightIndex = 0;
        for (; lightIndex < snapshot.Lights.size(); ++lightIndex)
            if (snapshot.Lights[lightIndex].Type == LightType::Directional)
                break;
        if (lightIndex == snapshot.Lights.size())
        {
            outError.clear();
            outFrame = candidate;
            return true;
        }

        const SceneRenderLight& sun = snapshot.Lights[lightIndex];
        if (sun.SourceEntity == kInvalidEntityId
            || sun.PhotometricUnit != LightPhotometricUnit::Lux
            || !IsValidLightPhotometricValue(sun.Type, sun.PhotometricUnit,
                sun.PhotometricValue)
            || !NonnegativeFinite(sun.Color)
            || !Finite(sun.Transform.RotationDegrees))
            return fail("sky atmosphere found an invalid first directional light");

        Math::Vec3 emission;
        if (!Normalize(LightEmissionDirection(sun.Transform.RotationDegrees), emission))
            return fail("sky atmosphere could not normalize its sun direction");
        const Math::Vec3 surfaceToSunWorld { -emission.X, -emission.Y, -emission.Z };
        const double sunHeight = surfaceToSunWorld.Y;
        if (!(sunHeight > 0.0) || !(sun.PhotometricValue > 0.0))
        {
            outError.clear();
            outFrame = candidate;
            return true;
        }

        Math::Vec3 surfaceToSunView;
        if (!Normalize(TransformDirection(surfaceToSunWorld, view.View),
            surfaceToSunView))
            return fail("sky atmosphere could not transform its sun direction");

        constexpr double rec709R = 0.2126;
        constexpr double rec709G = 0.7152;
        constexpr double rec709B = 0.0722;
        const double tintLuminance = sun.Color.X * rec709R
            + sun.Color.Y * rec709G + sun.Color.Z * rec709B;
        if (!std::isfinite(tintLuminance) || !(tintLuminance > 0.0))
        {
            outError.clear();
            outFrame = candidate;
            return true;
        }

        if (!ValidProjection(view, candidate.TanHalfVerticalFov,
            candidate.AspectRatio))
            return fail("sky atmosphere requires finite positive projection scales");
        Math::Vec3 viewUp;
        if (!Normalize(TransformDirection({ 0.0f, 1.0f, 0.0f }, view.View),
            viewUp))
            return fail("sky atmosphere could not transform world up into the view");
        candidate.ViewUp = viewUp;

        candidate.Enabled = true;
        candidate.SunEntity = sun.SourceEntity;
        candidate.SunLightIndex = static_cast<u32>(lightIndex);
        candidate.SurfaceToSunWorld = surfaceToSunWorld;
        candidate.SurfaceToSunView = surfaceToSunView;
        candidate.SunZenithRadians = static_cast<float>(std::acos(
            std::clamp(sunHeight, 0.0, 1.0)));
        candidate.LuminancePerez = LuminanceCoefficients(candidate.Turbidity);
        candidate.ChromaticityXPerez = ChromaticityXCoefficients(candidate.Turbidity);
        candidate.ChromaticityYPerez = ChromaticityYCoefficients(candidate.Turbidity);
        const double skyScale = sun.PhotometricValue
            / kBasicSkyReferenceSunIlluminanceLux;
        candidate.ZenithChromaticityX = static_cast<float>(ZenithChromaticityX(
            candidate.SunZenithRadians, candidate.Turbidity));
        candidate.ZenithChromaticityY = static_cast<float>(ZenithChromaticityY(
            candidate.SunZenithRadians, candidate.Turbidity));
        candidate.ZenithLuminanceCdPerSquareMeter = static_cast<float>(
            std::max(ZenithLuminance(candidate.SunZenithRadians,
                candidate.Turbidity) * skyScale, 0.0));

        const double sunSolidAngle = 2.0 * std::numbers::pi
            * (1.0 - std::cos(candidate.SunAngularRadiusRadians));
        if (!std::isfinite(sunSolidAngle) || !(sunSolidAngle > 0.0))
            return fail("sky atmosphere has an invalid solar solid angle");
        const double sunRadianceScale = sun.PhotometricValue
            / (tintLuminance * sunSolidAngle);
        candidate.SunRadiance = {
            static_cast<float>(sun.Color.X * sunRadianceScale),
            static_cast<float>(sun.Color.Y * sunRadianceScale),
            static_cast<float>(sun.Color.Z * sunRadianceScale)
        };
        if (!NonnegativeFinite(candidate.SunRadiance)
            || !ComputeUpperDiffuseIrradiance(candidate,
                candidate.UpperDiffuseIrradiance))
            return fail("sky atmosphere could not produce finite radiance or irradiance");

        candidate.LowerDiffuseIrradiance = {
            candidate.UpperDiffuseIrradiance.X * candidate.GroundAlbedo,
            candidate.UpperDiffuseIrradiance.Y * candidate.GroundAlbedo,
            candidate.UpperDiffuseIrradiance.Z * candidate.GroundAlbedo
        };
        constexpr float inversePi = 0.31830988618379067154f;
        candidate.GroundRadiance = {
            candidate.LowerDiffuseIrradiance.X * inversePi,
            candidate.LowerDiffuseIrradiance.Y * inversePi,
            candidate.LowerDiffuseIrradiance.Z * inversePi
        };
        if (!IsValidSceneSkyAtmosphereFrame(candidate))
            return fail("sky atmosphere preparation produced an invalid frame");
        outError.clear();
        outFrame = candidate;
        return true;
    }

    bool IsValidSceneSkyAtmosphereFrame(const SceneSkyAtmosphereFrame& frame)
    {
        const bool common = std::isfinite(frame.Turbidity)
            && frame.Turbidity >= 1.0f && frame.Turbidity <= 10.0f
            && std::isfinite(frame.GroundAlbedo) && frame.GroundAlbedo >= 0.0f
            && frame.GroundAlbedo <= 1.0f
            && std::isfinite(frame.SunAngularRadiusRadians)
            && frame.SunAngularRadiusRadians > 0.0f
            && frame.SunAngularRadiusRadians < 0.1f;
        if (!common)
            return false;
        if (!frame.Enabled)
            return frame.SunEntity == kInvalidEntityId
                && frame.SunLightIndex == std::numeric_limits<u32>::max();
        return frame.SunEntity != kInvalidEntityId
            && frame.SunLightIndex != std::numeric_limits<u32>::max()
            && std::isfinite(frame.TanHalfVerticalFov)
            && frame.TanHalfVerticalFov > 0.0f
            && std::isfinite(frame.AspectRatio) && frame.AspectRatio > 0.0f
            && Finite(frame.ViewUp)
            && std::isfinite(frame.SunZenithRadians)
            && frame.SunZenithRadians >= 0.0f
            && frame.SunZenithRadians < std::numbers::pi_v<float> * 0.5f
            && Finite(frame.SurfaceToSunWorld)
            && Finite(frame.SurfaceToSunView)
            && NonnegativeFinite(frame.SunRadiance)
            && NonnegativeFinite(frame.GroundRadiance)
            && NonnegativeFinite(frame.UpperDiffuseIrradiance)
            && NonnegativeFinite(frame.LowerDiffuseIrradiance)
            && std::isfinite(frame.ZenithChromaticityX)
            && frame.ZenithChromaticityX > 0.0f
            && std::isfinite(frame.ZenithChromaticityY)
            && frame.ZenithChromaticityY > 0.0f
            && std::isfinite(frame.ZenithLuminanceCdPerSquareMeter)
            && frame.ZenithLuminanceCdPerSquareMeter >= 0.0f
            && Finite(frame.LuminancePerez)
            && Finite(frame.ChromaticityXPerez)
            && Finite(frame.ChromaticityYPerez);
    }

    bool TryBuildSceneSkyAtmosphereGpuConstants(
        const SceneSkyAtmosphereFrame& frame, float preExposure,
        SceneSkyAtmosphereGpuConstants& outConstants, std::string& outError)
    {
        if (!IsValidSceneSkyAtmosphereFrame(frame) || !std::isfinite(preExposure)
            || !(preExposure > 0.0f))
        {
            outError = "sky atmosphere GPU constants require a valid frame and pre-exposure";
            return false;
        }
        SceneSkyAtmosphereGpuConstants candidate;
        candidate.ProjectionAndExposure[0] = frame.TanHalfVerticalFov;
        candidate.ProjectionAndExposure[1] = frame.AspectRatio;
        candidate.ProjectionAndExposure[2] = preExposure;
        candidate.SunDirectionAndCosRadius[0] = frame.SurfaceToSunView.X;
        candidate.SunDirectionAndCosRadius[1] = frame.SurfaceToSunView.Y;
        candidate.SunDirectionAndCosRadius[2] = frame.SurfaceToSunView.Z;
        candidate.SunDirectionAndCosRadius[3] = std::cos(
            frame.SunAngularRadiusRadians);
        candidate.ViewUpAndEnabled[0] = frame.ViewUp.X;
        candidate.ViewUpAndEnabled[1] = frame.ViewUp.Y;
        candidate.ViewUpAndEnabled[2] = frame.ViewUp.Z;
        candidate.ViewUpAndEnabled[3] = frame.Enabled ? 1.0f : 0.0f;
        candidate.ZenithxyYAndSunTheta[0] = frame.ZenithChromaticityX;
        candidate.ZenithxyYAndSunTheta[1] = frame.ZenithChromaticityY;
        candidate.ZenithxyYAndSunTheta[2] = frame.ZenithLuminanceCdPerSquareMeter;
        candidate.ZenithxyYAndSunTheta[3] = frame.SunZenithRadians;
        candidate.LuminancePerezABCD[0] = frame.LuminancePerez.A;
        candidate.LuminancePerezABCD[1] = frame.LuminancePerez.B;
        candidate.LuminancePerezABCD[2] = frame.LuminancePerez.C;
        candidate.LuminancePerezABCD[3] = frame.LuminancePerez.D;
        candidate.ChromaticityXPerezABCD[0] = frame.ChromaticityXPerez.A;
        candidate.ChromaticityXPerezABCD[1] = frame.ChromaticityXPerez.B;
        candidate.ChromaticityXPerezABCD[2] = frame.ChromaticityXPerez.C;
        candidate.ChromaticityXPerezABCD[3] = frame.ChromaticityXPerez.D;
        candidate.ChromaticityYPerezABCD[0] = frame.ChromaticityYPerez.A;
        candidate.ChromaticityYPerezABCD[1] = frame.ChromaticityYPerez.B;
        candidate.ChromaticityYPerezABCD[2] = frame.ChromaticityYPerez.C;
        candidate.ChromaticityYPerezABCD[3] = frame.ChromaticityYPerez.D;
        candidate.PerezE[0] = frame.LuminancePerez.E;
        candidate.PerezE[1] = frame.ChromaticityXPerez.E;
        candidate.PerezE[2] = frame.ChromaticityYPerez.E;
        candidate.SunRadiance[0] = frame.SunRadiance.X;
        candidate.SunRadiance[1] = frame.SunRadiance.Y;
        candidate.SunRadiance[2] = frame.SunRadiance.Z;
        candidate.GroundRadiance[0] = frame.GroundRadiance.X;
        candidate.GroundRadiance[1] = frame.GroundRadiance.Y;
        candidate.GroundRadiance[2] = frame.GroundRadiance.Z;
        outError.clear();
        outConstants = candidate;
        return true;
    }
}
