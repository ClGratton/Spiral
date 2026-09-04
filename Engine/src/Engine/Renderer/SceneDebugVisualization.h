#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Math/Math.h"
#include "Engine/Scene/Entity.h"

#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace Engine
{
    struct SceneObjectBounds;
    struct SceneRasterFrame;

    enum class SceneDebugView : u32
    {
        Lit = 0,
        MaterialId = 1,
        GeometricNormal = 2,
        ShadowCaster = 3
    };

    const char* ToString(SceneDebugView view);
    bool TryParseSceneDebugView(std::string_view value, SceneDebugView& outView);

    struct SceneDebugVisualizationSettings
    {
        SceneDebugView View = SceneDebugView::Lit;
        EntityId SelectedEntity = kInvalidEntityId;
        bool ShowSelectedBounds = true;

        bool operator==(const SceneDebugVisualizationSettings&) const = default;
    };

    bool IsValidSceneDebugVisualizationSettings(
        const SceneDebugVisualizationSettings& settings);

    struct SceneDebugVisualizationPublication
    {
        SceneDebugVisualizationSettings Settings;
        u64 Generation = 0;
    };

    struct SceneDebugOverlaySegment
    {
        // Normalized top-left viewport coordinates: x0, y0, x1, y1.
        float Values[4] {};
    };

    struct SceneDebugOverlayFrame
    {
        static constexpr size_t MaximumSegmentCount = 12;

        SceneDebugVisualizationSettings Settings;
        u64 SettingsGeneration = 0;
        std::array<SceneDebugOverlaySegment, MaximumSegmentCount> Segments {};
        u32 SegmentCount = 0;
        u32 ViewportWidth = 0;
        u32 ViewportHeight = 0;

        bool HasPostToneMapOverlay() const { return SegmentCount != 0; }
    };

    bool TryPrepareSceneDebugOverlay(const SceneRasterFrame& frame,
        const std::vector<SceneObjectBounds>& objectBounds,
        u32 width, u32 height, SceneDebugOverlayFrame& outOverlay,
        std::string& outError);
}
