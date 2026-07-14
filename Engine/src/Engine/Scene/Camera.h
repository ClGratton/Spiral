#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Math/Math.h"
#include "Engine/Math/WorldGrid.h"

#include <unordered_map>

namespace Engine
{
    struct CameraProjection
    {
        float VerticalFovDegrees = 60.0f;
        float NearClip = 0.1f;
        float FarClip = 100.0f;
    };

    struct CameraView
    {
        Math::Mat4 View;
        Math::Mat4 Projection;
        Math::Mat4 ViewProjection;
        Math::DVec3 WorldPosition;
        Math::DVec3 TranslationOrigin;
        u64 StableViewId = 0;
        Math::SectorIndex TranslationOriginSector;
        bool TemporalHistoryInvalidated = false;
        bool Valid = false;
    };

    CameraView BuildCameraView(
        const Math::DVec3& worldPosition,
        const Math::Vec3& rotationDegrees,
        const CameraProjection& projection,
        float aspectRatio,
        const Math::DVec3& translationOrigin);

    // Returns whether this canonical camera axis must select a new snapped-origin sector.
    // Adjacent sectors compare local boundary detail directly; non-adjacent changes rebase without overflow.
    bool ShouldRebaseCameraOriginAxis(
        i64 cameraSector,
        double cameraLocal,
        i64 originSector,
        const Math::WorldGridPolicy& policy);

    struct TrackedCameraViewRequest
    {
        u64 StableViewId = 0;
        Math::DVec3 WorldPosition;
        Math::Vec3 RotationDegrees;
        CameraProjection Projection;
        float AspectRatio = 0.0f;
        // Callers set this when the view was relocated discontinuously.
        bool DiscontinuousRelocation = false;
    };

    // View-publisher-owned per-view translated-origin state, implemented in the
    // Scene module. It deliberately does not own persistent transforms or world-grid arithmetic.
    class CameraViewOriginTracker
    {
    public:
        CameraView BuildView(const TrackedCameraViewRequest& request, const Math::WorldGridPolicy& policy);
        void ForgetView(u64 stableViewId);
        void Clear();

    private:
        struct ViewState
        {
            Math::SectorIndex OriginSector;
            Math::WorldGridPolicy Policy;
            Math::WorldOriginMode Mode = Math::WorldOriginMode::ExactCamera;
            bool Initialized = false;
        };

        std::unordered_map<u64, ViewState> m_ViewStates;
    };

    class EditorCamera
    {
    public:
        EditorCamera();

        void SetViewportSize(u32 width, u32 height);
        void SetPosition(const Math::DVec3& position);
        void SetRotationDegrees(const Math::Vec3& rotationDegrees);
        void SetProjection(const CameraProjection& projection);

        const Math::DVec3& GetPosition() const { return m_Position; }
        const Math::Vec3& GetRotationDegrees() const { return m_RotationDegrees; }
        const CameraProjection& GetProjection() const { return m_Projection; }
        const CameraView& GetCameraView() const { return m_View; }
        float GetAspectRatio() const { return m_AspectRatio; }

    private:
        void Recalculate();

    private:
        Math::DVec3 m_Position = { 0.0, 0.0, -3.35 };
        Math::Vec3 m_RotationDegrees = { 0.0f, 0.0f, 0.0f };
        CameraProjection m_Projection;
        CameraView m_View;
        float m_AspectRatio = 16.0f / 9.0f;
    };
}
