#include "Engine/Renderer/SceneShadowMap.h"

#include "Engine/Assets/MaterialAsset.h"
#include "Engine/Renderer/SceneRasterPreparation.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <iterator>

namespace Engine
{
    namespace
    {
        bool Finite(const Math::Vec3& value)
        {
            return std::isfinite(value.X) && std::isfinite(value.Y)
                && std::isfinite(value.Z);
        }

        bool Finite(const Math::Mat4& value)
        {
            return std::all_of(std::begin(value.Values), std::end(value.Values),
                [](float component) { return std::isfinite(component); });
        }

        Math::Vec3 Cross(const Math::Vec3& a, const Math::Vec3& b)
        {
            return { a.Y * b.Z - a.Z * b.Y,
                a.Z * b.X - a.X * b.Z,
                a.X * b.Y - a.Y * b.X };
        }

        bool Normalize(const Math::Vec3& value, Math::Vec3& out)
        {
            const double lengthSquared = static_cast<double>(value.X) * value.X
                + static_cast<double>(value.Y) * value.Y
                + static_cast<double>(value.Z) * value.Z;
            if (!std::isfinite(lengthSquared) || lengthSquared <= 0.0)
                return false;
            const float inverseLength = static_cast<float>(1.0 / std::sqrt(lengthSquared));
            out = { value.X * inverseLength, value.Y * inverseLength,
                value.Z * inverseLength };
            return Finite(out);
        }

        Math::Vec3 TransformPoint(const Math::Vec3& point, const Math::Mat4& matrix)
        {
            return {
                point.X * matrix.Values[0] + point.Y * matrix.Values[4]
                    + point.Z * matrix.Values[8] + matrix.Values[12],
                point.X * matrix.Values[1] + point.Y * matrix.Values[5]
                    + point.Z * matrix.Values[9] + matrix.Values[13],
                point.X * matrix.Values[2] + point.Y * matrix.Values[6]
                    + point.Z * matrix.Values[10] + matrix.Values[14]
            };
        }

        std::array<Math::Vec3, 8> Corners(const SceneObjectBounds& bounds)
        {
            return {{
                { bounds.Minimum.X, bounds.Minimum.Y, bounds.Minimum.Z },
                { bounds.Maximum.X, bounds.Minimum.Y, bounds.Minimum.Z },
                { bounds.Minimum.X, bounds.Maximum.Y, bounds.Minimum.Z },
                { bounds.Maximum.X, bounds.Maximum.Y, bounds.Minimum.Z },
                { bounds.Minimum.X, bounds.Minimum.Y, bounds.Maximum.Z },
                { bounds.Maximum.X, bounds.Minimum.Y, bounds.Maximum.Z },
                { bounds.Minimum.X, bounds.Maximum.Y, bounds.Maximum.Z },
                { bounds.Maximum.X, bounds.Maximum.Y, bounds.Maximum.Z }
            }};
        }

        bool ValidBounds(const SceneObjectBounds& bounds)
        {
            return Finite(bounds.Minimum) && Finite(bounds.Maximum)
                && bounds.Minimum.X <= bounds.Maximum.X
                && bounds.Minimum.Y <= bounds.Maximum.Y
                && bounds.Minimum.Z <= bounds.Maximum.Z;
        }

        Math::Mat4 BuildLightBasis(const Math::Vec3& right,
            const Math::Vec3& up, const Math::Vec3& forward)
        {
            Math::Mat4 result = Math::Mat4::Identity();
            result.Values[0] = right.X;
            result.Values[4] = right.Y;
            result.Values[8] = right.Z;
            result.Values[1] = up.X;
            result.Values[5] = up.Y;
            result.Values[9] = up.Z;
            result.Values[2] = forward.X;
            result.Values[6] = forward.Y;
            result.Values[10] = forward.Z;
            return result;
        }

        Math::Mat4 OrthographicOffCenter(float left, float right,
            float bottom, float top, float nearPlane, float farPlane)
        {
            Math::Mat4 result {};
            result.Values[0] = 2.0f / (right - left);
            result.Values[5] = 2.0f / (top - bottom);
            result.Values[10] = 1.0f / (farPlane - nearPlane);
            result.Values[12] = -(right + left) / (right - left);
            result.Values[13] = -(top + bottom) / (top - bottom);
            result.Values[14] = -nearPlane / (farPlane - nearPlane);
            result.Values[15] = 1.0f;
            return result;
        }
    }

    bool TryPrepareSceneShadowMap(const SceneRasterFrame& frame,
        const std::vector<SceneObjectBounds>& objectBounds, u32 resolution,
        SceneShadowMapFrame& outShadow, std::string& outError)
    {
        const auto fail = [&](std::string message)
        {
            outError = std::move(message);
            return false;
        };
        if (!frame.HasValidView || resolution < 64 || resolution > 8192
            || objectBounds.size() != frame.Instances.size())
            return fail("shadow preparation requires a valid view, matching bounds, and a 64..8192 map");

        SceneShadowMapFrame candidate;
        candidate.Resolution = resolution;
        for (u32 lightIndex : frame.LightGrid.GlobalLightIndices)
        {
            if (lightIndex >= frame.LightGrid.Lights.size()
                || frame.LightGrid.Lights[lightIndex].Type != LightType::Directional)
                return fail("shadow preparation received a malformed directional list");
            const ClusteredLightRecord& light = frame.LightGrid.Lights[lightIndex];
            if (!light.CastsShadows)
                continue;
            ++candidate.EligibleDirectionalLightCount;
            if (candidate.PrimaryLightIndex == std::numeric_limits<u32>::max())
            {
                candidate.PrimaryLightIndex = lightIndex;
                candidate.PrimaryLightEntity = light.SourceEntity;
            }
        }
        if (candidate.PrimaryLightIndex == std::numeric_limits<u32>::max())
        {
            outShadow = std::move(candidate);
            outError.clear();
            return true;
        }

        for (size_t index = 0; index < frame.Instances.size(); ++index)
        {
            const SceneRasterInstance& instance = frame.Instances[index];
            if (instance.MaterialId >= frame.MaterialRows.size()
                || !ValidBounds(objectBounds[index]) || !Finite(instance.CameraRelativeModel))
                return fail("shadow preparation received invalid instance material or bounds data");
            if (!instance.CastsShadows)
            {
                ++candidate.ComponentExcludedCount;
                continue;
            }
            const SceneMaterialRow& row = frame.MaterialRows[instance.MaterialId];
            SceneShadowCasterMode mode = SceneShadowCasterMode::Opaque;
            if (row.IsError)
            {
                mode = SceneShadowCasterMode::ConservativeError;
                ++candidate.ConservativeErrorCasterCount;
            }
            else if (row.Material.AlphaMode == MaterialAlphaMode::Mask)
            {
                mode = SceneShadowCasterMode::AlphaTested;
                ++candidate.AlphaTestedCasterCount;
            }
            else if (row.Material.AlphaMode == MaterialAlphaMode::Blend)
            {
                ++candidate.BlendExcludedCount;
                continue;
            }
            else if (row.Material.AlphaMode == MaterialAlphaMode::Opaque)
            {
                ++candidate.OpaqueCasterCount;
            }
            else
            {
                return fail("shadow preparation received an invalid material alpha mode");
            }
            candidate.Casters.push_back({ index, mode, {} });
        }
        if (candidate.Casters.empty())
        {
            outShadow = std::move(candidate);
            outError.clear();
            return true;
        }

        Math::Vec3 forward;
        const ClusteredLightRecord& primary = frame.LightGrid.Lights[candidate.PrimaryLightIndex];
        if (!Normalize(primary.WorldDirection, forward))
            return fail("shadow preparation requires a finite nonzero primary-light direction");
        const Math::Vec3 referenceUp = std::abs(forward.Y) < 0.95f
            ? Math::Vec3 { 0.0f, 1.0f, 0.0f }
            : Math::Vec3 { 1.0f, 0.0f, 0.0f };
        Math::Vec3 right;
        if (!Normalize(Cross(referenceUp, forward), right))
            return fail("shadow preparation could not construct a light basis");
        Math::Vec3 up;
        if (!Normalize(Cross(forward, right), up))
            return fail("shadow preparation could not construct a light up vector");
        const Math::Mat4 lightBasis = BuildLightBasis(right, up, forward);

        Math::Vec3 minimum { std::numeric_limits<float>::max(),
            std::numeric_limits<float>::max(), std::numeric_limits<float>::max() };
        Math::Vec3 maximum { -std::numeric_limits<float>::max(),
            -std::numeric_limits<float>::max(), -std::numeric_limits<float>::max() };
        for (size_t index = 0; index < frame.Instances.size(); ++index)
        {
            for (const Math::Vec3& corner : Corners(objectBounds[index]))
            {
                const Math::Vec3 relative = TransformPoint(corner,
                    frame.Instances[index].CameraRelativeModel);
                const Math::Vec3 lightSpace = TransformPoint(relative, lightBasis);
                if (!Finite(lightSpace))
                    return fail("shadow preparation overflowed transformed receiver bounds");
                minimum.X = std::min(minimum.X, lightSpace.X);
                minimum.Y = std::min(minimum.Y, lightSpace.Y);
                minimum.Z = std::min(minimum.Z, lightSpace.Z);
                maximum.X = std::max(maximum.X, lightSpace.X);
                maximum.Y = std::max(maximum.Y, lightSpace.Y);
                maximum.Z = std::max(maximum.Z, lightSpace.Z);
            }
        }

        const float rawHalfExtent = std::max({
            (maximum.X - minimum.X) * 0.5f,
            (maximum.Y - minimum.Y) * 0.5f, 0.5f });
        const float paddedHalfExtent = rawHalfExtent * 1.05f + 0.25f;
        const float halfExtent = std::ceil(paddedHalfExtent * 16.0f) / 16.0f;
        const float worldUnitsPerTexel = (2.0f * halfExtent) / static_cast<float>(resolution);
        float centerX = (minimum.X + maximum.X) * 0.5f;
        float centerY = (minimum.Y + maximum.Y) * 0.5f;
        centerX = std::round(centerX / worldUnitsPerTexel) * worldUnitsPerTexel;
        centerY = std::round(centerY / worldUnitsPerTexel) * worldUnitsPerTexel;
        const float depthPadding = std::max((maximum.Z - minimum.Z) * 0.05f, 1.0f);
        const float nearPlane = minimum.Z - depthPadding;
        const float farPlane = maximum.Z + depthPadding;
        if (!std::isfinite(halfExtent) || !std::isfinite(worldUnitsPerTexel)
            || !(worldUnitsPerTexel > 0.0f) || !std::isfinite(nearPlane)
            || !std::isfinite(farPlane) || !(farPlane > nearPlane))
            return fail("shadow preparation produced invalid stabilized bounds");

        const Math::Mat4 projection = OrthographicOffCenter(
            centerX - halfExtent, centerX + halfExtent,
            centerY - halfExtent, centerY + halfExtent, nearPlane, farPlane);
        candidate.CameraRelativeToShadowClip = Math::Multiply(lightBasis, projection);
        candidate.WorldUnitsPerTexel = worldUnitsPerTexel;
        candidate.ConstantDepthBias = std::max(0.5f / static_cast<float>(resolution), 0.00025f);
        candidate.SlopeDepthBias = candidate.ConstantDepthBias * 2.0f;
        candidate.Enabled = Finite(candidate.CameraRelativeToShadowClip);
        if (!candidate.Enabled)
            return fail("shadow preparation produced a nonfinite light matrix");
        for (SceneShadowCaster& caster : candidate.Casters)
        {
            caster.ModelToShadowClip = Math::Multiply(
                frame.Instances[caster.InstanceIndex].CameraRelativeModel,
                candidate.CameraRelativeToShadowClip);
            if (!Finite(caster.ModelToShadowClip))
                return fail("shadow preparation produced nonfinite caster constants");
        }

        outShadow = std::move(candidate);
        outError.clear();
        return true;
    }
}
