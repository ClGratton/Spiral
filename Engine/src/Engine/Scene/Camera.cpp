#include "Engine/Scene/Camera.h"

#include <cmath>

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
    }

    CameraView BuildCameraView(
        const Math::DVec3& worldPosition,
        const Math::Vec3& rotationDegrees,
        const CameraProjection& projection,
        float aspectRatio,
        const Math::DVec3& translationOrigin)
    {
        CameraView view;
        if (!IsFinite(worldPosition)
            || !IsFinite(rotationDegrees)
            || !IsFinite(translationOrigin)
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
        const Math::Vec3 cameraRelative = Math::CameraRelative(worldPosition, translationOrigin);
        const Math::Mat4 relativeTranslation = Math::Translation({
            -cameraRelative.X,
            -cameraRelative.Y,
            -cameraRelative.Z
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
