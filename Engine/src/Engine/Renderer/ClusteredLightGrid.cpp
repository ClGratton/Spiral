#include "Engine/Renderer/ClusteredLightGrid.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace Engine
{
    namespace
    {
        constexpr size_t kMaximumClusterCount = 4u * 1024u * 1024u;

        struct LocalLightClusterBounds
        {
            u32 LightIndex = 0;
            u32 MinimumTileX = 0;
            u32 MaximumTileX = 0;
            u32 MinimumTileY = 0;
            u32 MaximumTileY = 0;
            u32 MinimumDepthSlice = 0;
            u32 MaximumDepthSlice = 0;
        };

        bool IsFinite(const Math::Vec3& value)
        {
            return std::isfinite(value.X) && std::isfinite(value.Y) && std::isfinite(value.Z);
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

        Math::Vec3 TransformDirection(const Math::Vec3& direction, const Math::Mat4& matrix)
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

        Math::Vec3 LightDirection(const Math::Vec3& rotationDegrees)
        {
            const float yaw = Math::DegreesToRadians(rotationDegrees.Y);
            const float pitch = Math::DegreesToRadians(rotationDegrees.X);
            return {
                std::sin(yaw) * std::cos(pitch),
                -std::sin(pitch),
                std::cos(yaw) * std::cos(pitch)
            };
        }

        bool TryProjectionClipPlanes(const Math::Mat4& projection, float& outNear, float& outFar)
        {
            const float zScale = projection.Values[10];
            const float zOffset = projection.Values[14];
            if (!std::isfinite(zScale) || !std::isfinite(zOffset)
                || zScale <= 1.0f || zOffset >= 0.0f)
            {
                return false;
            }

            const float nearClip = -zOffset / zScale;
            const float farClip = -zOffset / (zScale - 1.0f);
            if (!std::isfinite(nearClip) || !std::isfinite(farClip)
                || nearClip <= 0.0f || farClip <= nearClip)
            {
                return false;
            }

            outNear = nearClip;
            outFar = farClip;
            return true;
        }

        u32 ClampTile(float value, u32 tileCount)
        {
            if (value <= 0.0f)
                return 0;
            return std::min(static_cast<u32>(value), tileCount - 1);
        }
    }

    size_t ClusteredLightGrid::GetClusterCount() const
    {
        return static_cast<size_t>(TileCountX) * TileCountY * DepthSliceCount;
    }

    size_t ClusteredLightGrid::GetClusterIndex(u32 tileX, u32 tileY, u32 depthSlice) const
    {
        return (static_cast<size_t>(depthSlice) * TileCountY + tileY) * TileCountX + tileX;
    }

    u32 ClusteredLightGrid::SelectDepthSlice(float viewDepth) const
    {
        if (DepthSliceCount == 0 || !(NearClip > 0.0f) || !(FarClip > NearClip))
            return 0;
        const float clampedDepth = std::clamp(viewDepth, NearClip, FarClip);
        const float normalized = std::log(clampedDepth / NearClip) / std::log(FarClip / NearClip);
        return std::min(static_cast<u32>(normalized * static_cast<float>(DepthSliceCount)),
            DepthSliceCount - 1);
    }

    bool BuildClusteredLightGrid(const SceneRenderSnapshot& snapshot,
        size_t viewIndex, u32 viewportWidth, u32 viewportHeight,
        const ClusteredLightGridConfig& config,
        ClusteredLightGrid& outGrid, std::string& outError)
    {
        if (viewIndex >= snapshot.Views.size() || !snapshot.Views[viewIndex].Camera.Valid)
        {
            outError = "clustered light grid requires a valid snapshot view";
            return false;
        }
        if (viewportWidth == 0 || viewportHeight == 0
            || config.TileSizePixels == 0 || config.DepthSliceCount == 0
            || config.MaximumLocalLightsPerCluster == 0
            || config.TileSizePixels > 4096 || config.DepthSliceCount > 128
            || config.MaximumLocalLightsPerCluster > 1024)
        {
            outError = "clustered light grid dimensions and limits must be nonzero";
            return false;
        }

        const CameraView& view = snapshot.Views[viewIndex].Camera;
        Math::SectorLocalPosition translationOriginPosition;
        if (view.HasCanonicalTranslationOrigin)
        {
            translationOriginPosition = view.TranslationOriginPosition;
        }
        else if (!Math::TryDecomposeWorldPosition(
            view.TranslationOrigin, snapshot.WorldGridPolicy, translationOriginPosition))
        {
            outError = "clustered light grid could not canonicalize the view origin";
            return false;
        }
        ClusteredLightGrid candidate;
        candidate.ViewportWidth = viewportWidth;
        candidate.ViewportHeight = viewportHeight;
        candidate.TileSizePixels = config.TileSizePixels;
        candidate.TileCountX = 1 + (viewportWidth - 1) / config.TileSizePixels;
        candidate.TileCountY = 1 + (viewportHeight - 1) / config.TileSizePixels;
        candidate.DepthSliceCount = config.DepthSliceCount;
        candidate.MaximumLocalLightsPerCluster = config.MaximumLocalLightsPerCluster;
        if (!TryProjectionClipPlanes(view.Projection, candidate.NearClip, candidate.FarClip))
        {
            outError = "clustered light grid could not recover finite projection clip planes";
            return false;
        }

        const size_t clusterCount = candidate.GetClusterCount();
        if (clusterCount == 0 || clusterCount > kMaximumClusterCount
            || clusterCount > std::numeric_limits<u32>::max()
                / candidate.MaximumLocalLightsPerCluster)
        {
            outError = "clustered light grid exceeds its bounded cluster capacity";
            return false;
        }

        std::vector<LocalLightClusterBounds> localBounds;
        candidate.Lights.reserve(snapshot.Lights.size());
        localBounds.reserve(snapshot.Lights.size());
        for (const SceneRenderLight& source : snapshot.Lights)
        {
            if (source.SourceEntity == kInvalidEntityId || !IsFinite(source.Color)
                || source.Color.X < 0.0f || source.Color.Y < 0.0f || source.Color.Z < 0.0f
                || !IsFinite(source.Transform.RotationDegrees)
                || !IsValidLightPhotometricValue(
                    source.Type, source.PhotometricUnit, source.PhotometricValue)
                || !std::isfinite(source.Range) || source.Range < 0.0f
                || !std::isfinite(source.InnerConeDegrees)
                || !std::isfinite(source.OuterConeDegrees)
                || source.InnerConeDegrees < 0.0f
                || source.OuterConeDegrees < source.InnerConeDegrees
                || source.OuterConeDegrees > 180.0f)
            {
                outError = "clustered light grid rejected invalid light data";
                return false;
            }

            Math::DVec3 originRelativePosition;
            if (!Math::TryGetSectorLocalRelativePosition(source.Transform.Position,
                translationOriginPosition, snapshot.WorldGridPolicy, originRelativePosition))
            {
                outError = "clustered light grid could not translate a light into the view origin";
                return false;
            }
            const double floatLimit = static_cast<double>(std::numeric_limits<float>::max());
            if (originRelativePosition.X < -floatLimit || originRelativePosition.X > floatLimit
                || originRelativePosition.Y < -floatLimit || originRelativePosition.Y > floatLimit
                || originRelativePosition.Z < -floatLimit || originRelativePosition.Z > floatLimit)
            {
                outError = "clustered light grid light position exceeds float view space";
                return false;
            }

            ClusteredLightRecord light;
            light.SourceEntity = source.SourceEntity;
            light.Type = source.Type;
            light.ViewPosition = TransformPoint({
                static_cast<float>(originRelativePosition.X),
                static_cast<float>(originRelativePosition.Y),
                static_cast<float>(originRelativePosition.Z)
            }, view.View);
            light.WorldDirection = LightDirection(source.Transform.RotationDegrees);
            light.ViewDirection = TransformDirection(light.WorldDirection, view.View);
            light.Color = source.Color;
            light.PhotometricValue = source.PhotometricValue;
            light.PhotometricUnit = source.PhotometricUnit;
            light.Range = source.Range;
            light.InnerConeCosine = std::cos(Math::DegreesToRadians(source.InnerConeDegrees));
            light.OuterConeCosine = std::cos(Math::DegreesToRadians(source.OuterConeDegrees));
            light.CastsShadows = source.CastsShadows;
            const u32 lightIndex = static_cast<u32>(candidate.Lights.size());
            candidate.Lights.push_back(light);

            if (source.Type == LightType::Directional)
            {
                candidate.GlobalLightIndices.push_back(lightIndex);
                continue;
            }
            if (source.Range <= 0.0f || source.PhotometricValue <= 0.0)
                continue;

            const float minimumDepth = std::max(candidate.NearClip, light.ViewPosition.Z - source.Range);
            const float maximumDepth = std::min(candidate.FarClip, light.ViewPosition.Z + source.Range);
            if (maximumDepth < candidate.NearClip || minimumDepth > candidate.FarClip
                || maximumDepth < minimumDepth)
            {
                continue;
            }

            LocalLightClusterBounds bounds;
            bounds.LightIndex = lightIndex;
            bounds.MinimumDepthSlice = candidate.SelectDepthSlice(minimumDepth);
            bounds.MaximumDepthSlice = candidate.SelectDepthSlice(maximumDepth);
            if (light.ViewPosition.Z - source.Range <= candidate.NearClip)
            {
                bounds.MaximumTileX = candidate.TileCountX - 1;
                bounds.MaximumTileY = candidate.TileCountY - 1;
            }
            else
            {
                const float inverseDepth = 1.0f / light.ViewPosition.Z;
                const float centerX = light.ViewPosition.X * view.Projection.Values[0] * inverseDepth;
                const float centerY = light.ViewPosition.Y * view.Projection.Values[5] * inverseDepth;
                const float radiusX = source.Range * std::abs(view.Projection.Values[0])
                    / std::max(candidate.NearClip, light.ViewPosition.Z - source.Range);
                const float radiusY = source.Range * std::abs(view.Projection.Values[5])
                    / std::max(candidate.NearClip, light.ViewPosition.Z - source.Range);
                const float minimumPixelX = (centerX - radiusX + 1.0f) * 0.5f * viewportWidth;
                const float maximumPixelX = (centerX + radiusX + 1.0f) * 0.5f * viewportWidth;
                const float minimumPixelY = (centerY - radiusY + 1.0f) * 0.5f * viewportHeight;
                const float maximumPixelY = (centerY + radiusY + 1.0f) * 0.5f * viewportHeight;
                if (maximumPixelX < 0.0f || minimumPixelX >= viewportWidth
                    || maximumPixelY < 0.0f || minimumPixelY >= viewportHeight)
                {
                    continue;
                }
                bounds.MinimumTileX = ClampTile(std::max(0.0f, minimumPixelX)
                    / config.TileSizePixels, candidate.TileCountX);
                bounds.MaximumTileX = ClampTile(std::max(0.0f, maximumPixelX)
                    / config.TileSizePixels, candidate.TileCountX);
                bounds.MinimumTileY = ClampTile(std::max(0.0f, minimumPixelY)
                    / config.TileSizePixels, candidate.TileCountY);
                bounds.MaximumTileY = ClampTile(std::max(0.0f, maximumPixelY)
                    / config.TileSizePixels, candidate.TileCountY);
            }
            localBounds.push_back(bounds);
        }

        std::vector<u32> clusterCounts(clusterCount, 0);
        const auto forEachBoundedCluster = [&candidate](const LocalLightClusterBounds& bounds, auto&& operation)
        {
            for (u32 depth = bounds.MinimumDepthSlice; depth <= bounds.MaximumDepthSlice; ++depth)
                for (u32 tileY = bounds.MinimumTileY; tileY <= bounds.MaximumTileY; ++tileY)
                    for (u32 tileX = bounds.MinimumTileX; tileX <= bounds.MaximumTileX; ++tileX)
                        operation(candidate.GetClusterIndex(tileX, tileY, depth));
        };
        for (const LocalLightClusterBounds& bounds : localBounds)
        {
            forEachBoundedCluster(bounds, [&candidate, &clusterCounts](size_t clusterIndex)
            {
                if (clusterCounts[clusterIndex] < candidate.MaximumLocalLightsPerCluster)
                    ++clusterCounts[clusterIndex];
                else
                    ++candidate.OverflowedLocalLightReferences;
            });
        }

        candidate.ClusterOffsets.resize(clusterCount + 1, 0);
        for (size_t cluster = 0; cluster < clusterCount; ++cluster)
            candidate.ClusterOffsets[cluster + 1] = candidate.ClusterOffsets[cluster] + clusterCounts[cluster];
        candidate.LocalLightIndices.resize(candidate.ClusterOffsets.back());
        std::vector<u32> writeCounts(clusterCount, 0);
        for (const LocalLightClusterBounds& bounds : localBounds)
        {
            forEachBoundedCluster(bounds, [&candidate, &clusterCounts, &writeCounts, &bounds](size_t clusterIndex)
            {
                if (writeCounts[clusterIndex] >= clusterCounts[clusterIndex])
                    return;
                candidate.LocalLightIndices[candidate.ClusterOffsets[clusterIndex]
                    + writeCounts[clusterIndex]++] = bounds.LightIndex;
            });
        }

        outGrid = std::move(candidate);
        outError.clear();
        return true;
    }
}
