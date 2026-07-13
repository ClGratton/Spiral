#pragma once

#include "Engine/Assets/AssetHandle.h"
#include "Engine/Math/Math.h"
#include "Engine/Scene/Camera.h"

#include <string>

namespace Engine
{
    struct TransformComponent
    {
        Math::DVec3 Position = { 0.0, 0.0, 0.0 };
        Math::Vec3 RotationDegrees = { 0.0f, 0.0f, 0.0f };
        Math::Vec3 Scale = { 1.0f, 1.0f, 1.0f };

        Math::Mat4 GetCameraRelativeTransform(const Math::DVec3& translationOrigin) const
        {
            return Math::Multiply(
                Math::Multiply(
                    Math::Scale(Scale),
                    Math::RotationYawPitchRoll(
                        Math::DegreesToRadians(RotationDegrees.Y),
                        Math::DegreesToRadians(RotationDegrees.X),
                        Math::DegreesToRadians(RotationDegrees.Z))),
                Math::Translation(Math::CameraRelative(Position, translationOrigin)));
        }
    };

    struct CameraComponent
    {
        CameraProjection Projection;
        Math::Vec3 BackgroundColor = { 0.08f, 0.09f, 0.10f };
        bool Primary = true;
    };

    enum class LightType
    {
        Directional,
        Point,
        Spot
    };

    struct LightComponent
    {
        LightType Type = LightType::Directional;
        Math::Vec3 Color = { 1.0f, 1.0f, 1.0f };
        float Intensity = 1.0f;
        float Range = 10.0f;
        float InnerConeDegrees = 25.0f;
        float OuterConeDegrees = 45.0f;
        bool CastsShadows = true;
    };

    struct MeshRendererComponent
    {
        AssetHandle MeshAsset = kInvalidAssetHandle;
        AssetHandle MaterialAsset = kInvalidAssetHandle;
        std::string MeshName;
        bool Visible = true;
        bool CastsShadows = true;
    };
}
