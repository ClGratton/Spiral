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
        // Occluded portions of the selected-bounds edges are hidden by default
        // (depth tested against the Scene depth). When true they are drawn with
        // a dimmer, thinner treatment so the whole box stays readable.
        bool ShowOccludedSelectionBounds = false;

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
        // Zero-to-one NDC depth at the first and second endpoint, in the same
        // convention the raster pass writes (standard Z, nearer is smaller).
        float Depth[2] {};
    };

    // One authority for the selected-bounds treatment and its depth tolerance.
    // The overlay shader receives these through the constant buffer; the CPU
    // oracle below mirrors the shader's visibility decision exactly.
    struct SceneDebugOverlayStyle
    {
        static constexpr float VisibleOpacity = 0.92f;
        static constexpr float VisibleThicknessPixels = 2.0f;
        static constexpr float OccludedOpacity = 0.25f;
        static constexpr float OccludedThicknessPixels = 1.0f;
        // Relative tolerance on inverse view depth. Interpolating an edge in
        // screen space and reconstructing the Scene depth are both accurate to
        // about 1e-5 relative, so 1e-3 leaves two orders of magnitude of margin
        // while staying far below the separation of a real occluder.
        static constexpr float RelativeDepthTolerance = 0.001f;
        // The D32 depth buffer resolves 2^-24 near one; sixteen steps cover the
        // quantization of the sampled depth and of the reconstructed edge.
        static constexpr float DepthQuantizationStep = 1.0f / 16777216.0f;
        static constexpr float DepthQuantizationSteps = 16.0f;
        // Inverse view depth is affine in screen space on a planar face, so an
        // edge lying on a sloped face differs from the face depth at a pixel
        // that is d pixels away by exactly gradient * d. The widest covered
        // distance is the line half-thickness plus the 0.75 px antialias feather.
        static constexpr float SlopeFootprintPixels = 3.0f;
    };

    // Inverse view depth u = 1 / viewZ recovered from a zero-to-one NDC depth
    // produced by a perspective projection whose third and fourth rows give
    // ndc = depthScale + depthOffset / viewZ. Larger u is nearer the camera.
    float SceneDebugInverseViewDepth(float ndcDepth, float depthScale,
        float depthOffset);

    // True when an edge at inverse depth edgeU is not hidden by a Scene surface
    // at inverse depth sceneU whose per-pixel inverse-depth gradient magnitude is
    // sceneGradient. depthOffset supplies the quantization scale.
    bool IsSceneDebugEdgeVisible(float edgeU, float sceneU,
        float sceneGradient, float depthOffset);

    struct SceneDebugOverlayFrame
    {
        static constexpr size_t MaximumSegmentCount = 12;

        SceneDebugVisualizationSettings Settings;
        u64 SettingsGeneration = 0;
        std::array<SceneDebugOverlaySegment, MaximumSegmentCount> Segments {};
        u32 SegmentCount = 0;
        u32 ViewportWidth = 0;
        u32 ViewportHeight = 0;
        // ndc = DepthScale + DepthOffset / viewZ of the prepared view
        // (PerspectiveLH: far / (far - near) and -near * far / (far - near)).
        float DepthScale = 0.0f;
        float DepthOffset = 0.0f;

        bool HasPostToneMapOverlay() const { return SegmentCount != 0; }
    };

    bool TryPrepareSceneDebugOverlay(const SceneRasterFrame& frame,
        const std::vector<SceneObjectBounds>& objectBounds,
        u32 width, u32 height, SceneDebugOverlayFrame& outOverlay,
        std::string& outError);
}
