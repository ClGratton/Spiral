#include "Engine/Scene/Camera.h"

#include <cmath>
#include <limits>

namespace Engine
{
    namespace
    {
        bool IsFinite(const Math::DVec3& value)
        {
            return std::isfinite(value.X) && std::isfinite(value.Y) && std::isfinite(value.Z);
        }

        bool IsFinite(const Math::Vec3& value)
        {
            return std::isfinite(value.X) && std::isfinite(value.Y) && std::isfinite(value.Z);
        }

        bool IsFloatRepresentable(const Math::DVec3& value)
        {
            const double limit = static_cast<double>(std::numeric_limits<float>::max());
            return IsFinite(value)
                && value.X >= -limit && value.X <= limit
                && value.Y >= -limit && value.Y <= limit
                && value.Z >= -limit && value.Z <= limit;
        }

        CameraView BuildCameraViewFromRelativePosition(
            const Math::DVec3& worldPosition,
            const Math::Vec3& rotationDegrees,
            const CameraProjection& projection,
            float aspectRatio,
            const Math::DVec3& translationOrigin,
            const Math::DVec3& cameraRelativePosition)
        {
            CameraView view;
            if (!IsFinite(worldPosition)
                || !IsFinite(rotationDegrees)
                || !IsFinite(translationOrigin)
                || !IsFloatRepresentable(cameraRelativePosition)
                || !std::isfinite(aspectRatio)
                || !std::isfinite(projection.VerticalFovDegrees)
                || !std::isfinite(projection.NearClip)
                || !std::isfinite(projection.FarClip))
            {
                return view;
            }

            const float pitch = Math::DegreesToRadians(rotationDegrees.X);
            const float yaw = Math::DegreesToRadians(rotationDegrees.Y);
            const float roll = Math::DegreesToRadians(rotationDegrees.Z);
            const Math::Mat4 inverseRotation = Math::RotationYawPitchRoll(-yaw, -pitch, -roll);
            const Math::Mat4 relativeTranslation = Math::Translation({
                -static_cast<float>(cameraRelativePosition.X),
                -static_cast<float>(cameraRelativePosition.Y),
                -static_cast<float>(cameraRelativePosition.Z)
            });

            view.View = Math::Multiply(relativeTranslation, inverseRotation);
            view.Projection = Math::PerspectiveLH(
                Math::DegreesToRadians(projection.VerticalFovDegrees),
                aspectRatio,
                projection.NearClip,
                projection.FarClip);
            view.ViewProjection = Math::Multiply(view.View, view.Projection);
            view.WorldPosition = worldPosition;
            view.TranslationOrigin = translationOrigin;
            view.Valid = true;
            return view;
        }
    }

    CameraView BuildCameraView(
        const Math::DVec3& worldPosition,
        const Math::Vec3& rotationDegrees,
        const CameraProjection& projection,
        float aspectRatio,
        const Math::DVec3& translationOrigin)
    {
        return BuildCameraViewFromRelativePosition(
            worldPosition,
            rotationDegrees,
            projection,
            aspectRatio,
            translationOrigin,
            {
                worldPosition.X - translationOrigin.X,
                worldPosition.Y - translationOrigin.Y,
                worldPosition.Z - translationOrigin.Z
            });
    }

    bool ShouldRebaseCameraOriginAxis(
        i64 cameraSector,
        double cameraLocal,
        i64 originSector,
        const Math::WorldGridPolicy& policy)
    {
        if (!Math::IsWorldGridPolicyValid(policy) || !std::isfinite(cameraLocal))
            return false;

        if (cameraSector == originSector)
            return false;

        const double halfExtent = policy.SectorExtent * 0.5;
        if (originSector != std::numeric_limits<i64>::max() && cameraSector == originSector + 1)
            return cameraLocal > -halfExtent + policy.OriginHysteresis;
        if (originSector != std::numeric_limits<i64>::min() && cameraSector == originSector - 1)
            return cameraLocal < halfExtent - policy.OriginHysteresis;

        return true;
    }

    CameraView CameraViewOriginTracker::BuildView(
        const TrackedCameraViewRequest& request,
        const Math::WorldGridPolicy& policy)
    {
        if (request.StableViewId == 0 || !Math::IsWorldGridPolicyValid(policy))
            return {};

        Math::SectorLocalPosition cameraPosition;
        if (request.HasCanonicalWorldPosition)
        {
            if (!Math::IsCanonical(request.CanonicalWorldPosition, policy))
                return {};
            cameraPosition = request.CanonicalWorldPosition;
        }
        else if (!Math::TryDecomposeWorldPosition(request.WorldPosition, policy, cameraPosition))
        {
            return {};
        }

        const auto stateIt = m_ViewStates.find(request.StableViewId);
        ViewState candidate = stateIt == m_ViewStates.end() ? ViewState {} : stateIt->second;
        const bool policyOrModeChanged = !candidate.Initialized
            || candidate.Mode != policy.OriginMode
            || candidate.Policy.Version != policy.Version
            || candidate.Policy.SectorExtent != policy.SectorExtent
            || candidate.Policy.OriginHysteresis != policy.OriginHysteresis;
        if (policy.OriginMode == Math::WorldOriginMode::ExactCamera)
        {
            CameraView view = BuildCameraViewFromRelativePosition(
                request.WorldPosition,
                request.RotationDegrees,
                request.Projection,
                request.AspectRatio,
                request.WorldPosition,
                {});
            view.StableViewId = request.StableViewId;
            view.TranslationOriginSector = cameraPosition.Sector;
            view.TranslationOriginPosition = cameraPosition;
            view.HasCanonicalTranslationOrigin = true;
            view.TemporalHistoryInvalidated = policyOrModeChanged || request.DiscontinuousRelocation;
            if (!view.Valid)
                return {};

            candidate.Policy = policy;
            candidate.Mode = policy.OriginMode;
            candidate.Initialized = true;
            m_ViewStates[request.StableViewId] = candidate;
            return view;
        }

        bool originChanged = false;
        if (policyOrModeChanged || request.DiscontinuousRelocation)
        {
            candidate.OriginSector = cameraPosition.Sector;
            originChanged = candidate.Initialized;
        }
        else
        {
            if (ShouldRebaseCameraOriginAxis(
                cameraPosition.Sector.X, cameraPosition.Local.X, candidate.OriginSector.X, policy))
            {
                originChanged = originChanged || candidate.OriginSector.X != cameraPosition.Sector.X;
                candidate.OriginSector.X = cameraPosition.Sector.X;
            }
            if (ShouldRebaseCameraOriginAxis(
                cameraPosition.Sector.Y, cameraPosition.Local.Y, candidate.OriginSector.Y, policy))
            {
                originChanged = originChanged || candidate.OriginSector.Y != cameraPosition.Sector.Y;
                candidate.OriginSector.Y = cameraPosition.Sector.Y;
            }
            if (ShouldRebaseCameraOriginAxis(
                cameraPosition.Sector.Z, cameraPosition.Local.Z, candidate.OriginSector.Z, policy))
            {
                originChanged = originChanged || candidate.OriginSector.Z != cameraPosition.Sector.Z;
                candidate.OriginSector.Z = cameraPosition.Sector.Z;
            }
        }

        Math::DVec3 translationOrigin;
        const Math::SectorLocalPosition originPosition { candidate.OriginSector, {} };
        if (!Math::TryComposeApproximateWorldPosition(originPosition, policy, translationOrigin))
            return {};

        Math::DVec3 cameraRelativePosition;
        if (!Math::TryGetSectorLocalRelativePosition(
            cameraPosition,
            originPosition,
            policy,
            cameraRelativePosition))
        {
            return {};
        }

        CameraView view = BuildCameraViewFromRelativePosition(
            request.WorldPosition,
            request.RotationDegrees,
            request.Projection,
            request.AspectRatio,
            translationOrigin,
            cameraRelativePosition);
        if (!view.Valid)
            return {};

        candidate.Policy = policy;
        candidate.Mode = policy.OriginMode;
        candidate.Initialized = true;
        m_ViewStates[request.StableViewId] = candidate;
        view.StableViewId = request.StableViewId;
        view.TranslationOriginSector = candidate.OriginSector;
        view.TranslationOriginPosition = originPosition;
        view.HasCanonicalTranslationOrigin = true;
        view.TemporalHistoryInvalidated = policyOrModeChanged || originChanged || request.DiscontinuousRelocation;
        return view;
    }

    void CameraViewOriginTracker::ForgetView(u64 stableViewId)
    {
        m_ViewStates.erase(stableViewId);
    }

    void CameraViewOriginTracker::Clear()
    {
        m_ViewStates.clear();
    }

    EditorCamera::EditorCamera()
    {
        Recalculate();
    }

    void EditorCamera::SetViewportSize(u32 width, u32 height)
    {
        if (width == 0 || height == 0)
            return;

        m_AspectRatio = static_cast<float>(width) / static_cast<float>(height);
        Recalculate();
    }

    void EditorCamera::SetPosition(const Math::DVec3& position)
    {
        m_Position = position;
        Recalculate();
    }

    void EditorCamera::SetRotationDegrees(const Math::Vec3& rotationDegrees)
    {
        m_RotationDegrees = rotationDegrees;
        Recalculate();
    }

    void EditorCamera::SetProjection(const CameraProjection& projection)
    {
        m_Projection = projection;
        Recalculate();
    }

    void EditorCamera::Recalculate()
    {
        m_View = BuildCameraView(m_Position, m_RotationDegrees, m_Projection, m_AspectRatio, m_Position);
    }
}
