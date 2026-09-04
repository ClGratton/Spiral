#include "Engine/Renderer/SceneDebugVisualization.h"

#include "Engine/Renderer/Renderer.h"
#include "Engine/Renderer/SceneRasterPreparation.h"
#include "Engine/Renderer/SceneShadowMap.h"

#include <algorithm>
#include <atomic>
#include <array>
#include <cmath>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>

namespace Engine
{
    namespace
    {
        struct HomogeneousPoint
        {
            float X = 0.0f;
            float Y = 0.0f;
            float Z = 0.0f;
            float W = 0.0f;
        };

        constexpr std::array<std::array<size_t, 2>, 12> kBoundsEdges {{
            {{ 0, 1 }}, {{ 1, 3 }}, {{ 3, 2 }}, {{ 2, 0 }},
            {{ 4, 5 }}, {{ 5, 7 }}, {{ 7, 6 }}, {{ 6, 4 }},
            {{ 0, 4 }}, {{ 1, 5 }}, {{ 2, 6 }}, {{ 3, 7 }}
        }};

#if defined(__cpp_lib_atomic_shared_ptr) && __cpp_lib_atomic_shared_ptr >= 201711L
        std::atomic<std::shared_ptr<const SceneDebugVisualizationPublication>>
            s_DebugVisualizationPublication;
#else
        std::shared_ptr<const SceneDebugVisualizationPublication>
            s_DebugVisualizationPublication;
#endif
        u64 s_DebugVisualizationGeneration = 0;
        std::mutex s_DebugVisualizationMutex;

        std::shared_ptr<const SceneDebugVisualizationPublication>
        LoadDebugVisualizationPublication()
        {
#if defined(__cpp_lib_atomic_shared_ptr) && __cpp_lib_atomic_shared_ptr >= 201711L
            return s_DebugVisualizationPublication.load(std::memory_order_acquire);
#else
            return std::atomic_load_explicit(&s_DebugVisualizationPublication,
                std::memory_order_acquire);
#endif
        }

        void StoreDebugVisualizationPublication(
            std::shared_ptr<const SceneDebugVisualizationPublication> publication)
        {
#if defined(__cpp_lib_atomic_shared_ptr) && __cpp_lib_atomic_shared_ptr >= 201711L
            s_DebugVisualizationPublication.store(std::move(publication),
                std::memory_order_release);
#else
            std::atomic_store_explicit(&s_DebugVisualizationPublication,
                std::move(publication), std::memory_order_release);
#endif
        }

        HomogeneousPoint TransformPoint(const Math::Vec3& point,
            const Math::Mat4& matrix)
        {
            return {
                point.X * matrix.Values[0] + point.Y * matrix.Values[4]
                    + point.Z * matrix.Values[8] + matrix.Values[12],
                point.X * matrix.Values[1] + point.Y * matrix.Values[5]
                    + point.Z * matrix.Values[9] + matrix.Values[13],
                point.X * matrix.Values[2] + point.Y * matrix.Values[6]
                    + point.Z * matrix.Values[10] + matrix.Values[14],
                point.X * matrix.Values[3] + point.Y * matrix.Values[7]
                    + point.Z * matrix.Values[11] + matrix.Values[15]
            };
        }

        HomogeneousPoint Interpolate(const HomogeneousPoint& first,
            const HomogeneousPoint& second, float amount)
        {
            return {
                first.X + (second.X - first.X) * amount,
                first.Y + (second.Y - first.Y) * amount,
                first.Z + (second.Z - first.Z) * amount,
                first.W + (second.W - first.W) * amount
            };
        }

        // Clips a segment against the D3D/Vulkan zero-to-one homogeneous
        // frustum before division. This keeps boxes crossing the near plane
        // finite instead of emitting inverted or screen-spanning edges.
        bool ClipSegment(HomogeneousPoint& first, HomogeneousPoint& second)
        {
            constexpr float minimumW = 0.00001f;
            const std::array<float (*)(const HomogeneousPoint&), 7> planes {{
                +[](const HomogeneousPoint& value) { return value.X + value.W; },
                +[](const HomogeneousPoint& value) { return value.W - value.X; },
                +[](const HomogeneousPoint& value) { return value.Y + value.W; },
                +[](const HomogeneousPoint& value) { return value.W - value.Y; },
                +[](const HomogeneousPoint& value) { return value.Z; },
                +[](const HomogeneousPoint& value) { return value.W - value.Z; },
                +[](const HomogeneousPoint& value) { return value.W - 0.00001f; }
            }};
            float minimum = 0.0f;
            float maximum = 1.0f;
            for (const auto& plane : planes)
            {
                const float firstDistance = plane(first);
                const float secondDistance = plane(second);
                if (!std::isfinite(firstDistance) || !std::isfinite(secondDistance))
                    return false;
                if (firstDistance < 0.0f && secondDistance < 0.0f)
                    return false;
                if (firstDistance >= 0.0f && secondDistance >= 0.0f)
                    continue;
                const float denominator = firstDistance - secondDistance;
                if (!std::isfinite(denominator) || denominator == 0.0f)
                    return false;
                const float intersection = firstDistance / denominator;
                if (firstDistance < 0.0f)
                    minimum = std::max(minimum, intersection);
                else
                    maximum = std::min(maximum, intersection);
                if (minimum > maximum)
                    return false;
            }
            const HomogeneousPoint originalFirst = first;
            const HomogeneousPoint originalSecond = second;
            first = Interpolate(originalFirst, originalSecond, minimum);
            second = Interpolate(originalFirst, originalSecond, maximum);
            return first.W >= minimumW && second.W >= minimumW;
        }

        bool IsFiniteBounds(const SceneObjectBounds& bounds)
        {
            const float values[] { bounds.Minimum.X, bounds.Minimum.Y,
                bounds.Minimum.Z, bounds.Maximum.X, bounds.Maximum.Y,
                bounds.Maximum.Z };
            return std::all_of(std::begin(values), std::end(values),
                [](float value) { return std::isfinite(value); })
                && bounds.Minimum.X <= bounds.Maximum.X
                && bounds.Minimum.Y <= bounds.Maximum.Y
                && bounds.Minimum.Z <= bounds.Maximum.Z;
        }
    }

    const char* ToString(SceneDebugView view)
    {
        switch (view)
        {
            case SceneDebugView::Lit: return "Lit";
            case SceneDebugView::MaterialId: return "MaterialId";
            case SceneDebugView::GeometricNormal: return "GeometricNormal";
            case SceneDebugView::ShadowCaster: return "ShadowCaster";
        }
        return "Unknown";
    }

    bool TryParseSceneDebugView(std::string_view value, SceneDebugView& outView)
    {
        SceneDebugView candidate;
        if (value == "Lit") candidate = SceneDebugView::Lit;
        else if (value == "MaterialId") candidate = SceneDebugView::MaterialId;
        else if (value == "GeometricNormal") candidate = SceneDebugView::GeometricNormal;
        else if (value == "ShadowCaster") candidate = SceneDebugView::ShadowCaster;
        else return false;
        outView = candidate;
        return true;
    }

    bool IsValidSceneDebugVisualizationSettings(
        const SceneDebugVisualizationSettings& settings)
    {
        return static_cast<u32>(settings.View)
            <= static_cast<u32>(SceneDebugView::ShadowCaster);
    }

    bool Renderer::SetSceneDebugVisualization(
        const SceneDebugVisualizationSettings& settings)
    {
        if (!IsValidSceneDebugVisualizationSettings(settings))
            return false;
        // The complete no-op check/generation/store transaction is serialized.
        // Readers remain lock-free through the immutable atomic publication.
        std::scoped_lock lock(s_DebugVisualizationMutex);
        const std::shared_ptr<const SceneDebugVisualizationPublication> current =
            LoadDebugVisualizationPublication();
        if (current && current->Settings == settings)
            return true;
        auto publication = std::make_shared<SceneDebugVisualizationPublication>();
        publication->Settings = settings;
        publication->Generation = ++s_DebugVisualizationGeneration;
        StoreDebugVisualizationPublication(std::move(publication));
        return true;
    }

    SceneDebugVisualizationPublication Renderer::GetSceneDebugVisualization()
    {
        const std::shared_ptr<const SceneDebugVisualizationPublication> current =
            LoadDebugVisualizationPublication();
        return current ? *current : SceneDebugVisualizationPublication {};
    }

    namespace Detail
    {
        void ResetSceneDebugVisualizationPublication()
        {
            std::scoped_lock lock(s_DebugVisualizationMutex);
            StoreDebugVisualizationPublication({});
            s_DebugVisualizationGeneration = 0;
        }
    }

    bool TryPrepareSceneDebugOverlay(const SceneRasterFrame& frame,
        const std::vector<SceneObjectBounds>& objectBounds,
        u32 width, u32 height, SceneDebugOverlayFrame& outOverlay,
        std::string& outError)
    {
        outError.clear();
        if (!IsValidSceneDebugVisualizationSettings(frame.DebugVisualization)
            || width == 0 || height == 0
            || objectBounds.size() != frame.Instances.size())
        {
            outError = "debug overlay received invalid settings, dimensions, or bounds identity";
            return false;
        }

        SceneDebugOverlayFrame candidate;
        candidate.Settings = frame.DebugVisualization;
        candidate.SettingsGeneration = frame.DebugVisualizationGeneration;
        candidate.ViewportWidth = width;
        candidate.ViewportHeight = height;
        if (!candidate.Settings.ShowSelectedBounds
            || candidate.Settings.SelectedEntity == kInvalidEntityId)
        {
            outOverlay = candidate;
            return true;
        }

        const auto selected = std::find_if(frame.Instances.begin(), frame.Instances.end(),
            [&candidate](const SceneRasterInstance& instance)
            {
                return instance.SourceEntity == candidate.Settings.SelectedEntity;
            });
        if (selected == frame.Instances.end())
        {
            outOverlay = candidate;
            return true;
        }
        const size_t selectedIndex = static_cast<size_t>(
            std::distance(frame.Instances.begin(), selected));
        const SceneObjectBounds& bounds = objectBounds[selectedIndex];
        if (!IsFiniteBounds(bounds))
        {
            outError = "selected debug bounds are malformed";
            return false;
        }

        std::array<HomogeneousPoint, 8> corners;
        for (size_t index = 0; index < corners.size(); ++index)
        {
            const Math::Vec3 point {
                (index & 1u) != 0 ? bounds.Maximum.X : bounds.Minimum.X,
                (index & 2u) != 0 ? bounds.Maximum.Y : bounds.Minimum.Y,
                (index & 4u) != 0 ? bounds.Maximum.Z : bounds.Minimum.Z
            };
            corners[index] = TransformPoint(point, selected->ModelViewProjection);
        }

        for (const auto& edge : kBoundsEdges)
        {
            HomogeneousPoint first = corners[edge[0]];
            HomogeneousPoint second = corners[edge[1]];
            if (!ClipSegment(first, second))
                continue;
            const float firstX = first.X / first.W;
            const float firstY = first.Y / first.W;
            const float secondX = second.X / second.W;
            const float secondY = second.Y / second.W;
            const SceneDebugOverlaySegment segment {{
                std::clamp(firstX * 0.5f + 0.5f, 0.0f, 1.0f),
                std::clamp(0.5f - firstY * 0.5f, 0.0f, 1.0f),
                std::clamp(secondX * 0.5f + 0.5f, 0.0f, 1.0f),
                std::clamp(0.5f - secondY * 0.5f, 0.0f, 1.0f)
            }};
            if (!std::all_of(std::begin(segment.Values), std::end(segment.Values),
                    [](float value) { return std::isfinite(value); }))
            {
                outError = "selected debug projection produced nonfinite viewport coordinates";
                return false;
            }
            candidate.Segments[candidate.SegmentCount++] = segment;
        }

        outOverlay = candidate;
        return true;
    }
}
