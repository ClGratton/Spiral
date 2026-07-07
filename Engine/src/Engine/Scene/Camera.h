#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Math/Math.h"

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
        Math::Vec3 Position;
        bool Valid = false;
    };

    class EditorCamera
    {
    public:
        EditorCamera();

        void SetViewportSize(u32 width, u32 height);
        void SetPosition(const Math::Vec3& position);
        void SetRotationDegrees(const Math::Vec3& rotationDegrees);
        void SetProjection(const CameraProjection& projection);

        const Math::Vec3& GetPosition() const { return m_Position; }
        const Math::Vec3& GetRotationDegrees() const { return m_RotationDegrees; }
        const CameraProjection& GetProjection() const { return m_Projection; }
        const CameraView& GetCameraView() const { return m_View; }
        float GetAspectRatio() const { return m_AspectRatio; }

    private:
        void Recalculate();

    private:
        Math::Vec3 m_Position = { 0.0f, 0.0f, -3.35f };
        Math::Vec3 m_RotationDegrees = { 0.0f, 0.0f, 0.0f };
        CameraProjection m_Projection;
        CameraView m_View;
        float m_AspectRatio = 16.0f / 9.0f;
    };
}
