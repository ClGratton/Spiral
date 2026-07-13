#include "Engine/Scene/Camera.h"

namespace Engine
{
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
        const float pitch = Math::DegreesToRadians(m_RotationDegrees.X);
        const float yaw = Math::DegreesToRadians(m_RotationDegrees.Y);
        const float roll = Math::DegreesToRadians(m_RotationDegrees.Z);

        const Math::Mat4 inverseRotation = Math::RotationYawPitchRoll(-yaw, -pitch, -roll);

        m_View.View = inverseRotation;
        m_View.Projection = Math::PerspectiveLH(
            Math::DegreesToRadians(m_Projection.VerticalFovDegrees),
            m_AspectRatio,
            m_Projection.NearClip,
            m_Projection.FarClip);
        m_View.ViewProjection = Math::Multiply(m_View.View, m_View.Projection);
        m_View.TranslationOrigin = m_Position;
        m_View.Valid = true;
    }
}
