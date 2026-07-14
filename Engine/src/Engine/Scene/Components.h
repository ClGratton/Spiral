#pragma once

#include "Engine/Assets/AssetHandle.h"
#include "Engine/Math/WorldGrid.h"
#include "Engine/Scene/Camera.h"

#include <string>

namespace Engine
{
    class TransformComponent
    {
    public:
        Math::Vec3 RotationDegrees = { 0.0f, 0.0f, 0.0f };
        Math::Vec3 Scale = { 1.0f, 1.0f, 1.0f };

        const Math::SectorLocalPosition& GetPosition() const { return m_Position; }

        bool TryGetApproximateWorldPosition(
            const Math::WorldGridPolicy& policy,
            Math::DVec3& outPosition) const
        {
            return Math::TryComposeApproximateWorldPosition(m_Position, policy, outPosition);
        }

        Math::Mat4 GetCameraRelativeTransform(
            const Math::DVec3& translationOrigin,
            const Math::WorldGridPolicy& policy) const
        {
            Math::DVec3 worldPosition;
            if (!TryGetApproximateWorldPosition(policy, worldPosition))
                return Math::Mat4::Identity();

            return Math::Multiply(
                Math::Multiply(
                    Math::Scale(Scale),
                    Math::RotationYawPitchRoll(
                        Math::DegreesToRadians(RotationDegrees.Y),
                        Math::DegreesToRadians(RotationDegrees.X),
                        Math::DegreesToRadians(RotationDegrees.Z))),
                Math::Translation(Math::CameraRelative(worldPosition, translationOrigin)));
        }

    private:
        friend class Scene;

        bool SetPosition(const Math::SectorLocalPosition& position, const Math::WorldGridPolicy& policy)
        {
            Math::SectorLocalPosition normalized;
            if (!Math::TryNormalizeSectorLocal(position, policy, normalized))
                return false;

            m_Position = normalized;
            return true;
        }

        bool SetWorldPosition(const Math::DVec3& position, const Math::WorldGridPolicy& policy)
        {
            Math::SectorLocalPosition decomposed;
            if (!Math::TryDecomposeWorldPosition(position, policy, decomposed))
                return false;

            m_Position = decomposed;
            return true;
        }

        bool SetWorldPositionAxis(u32 axis, double position, const Math::WorldGridPolicy& policy)
        {
            if (axis >= 3)
                return false;

            Math::SectorLocalPosition decomposed;
            if (!Math::TryDecomposeWorldPosition({ position, 0.0, 0.0 }, policy, decomposed))
                return false;

            switch (axis)
            {
                case 0:
                    m_Position.Sector.X = decomposed.Sector.X;
                    m_Position.Local.X = decomposed.Local.X;
                    break;
                case 1:
                    m_Position.Sector.Y = decomposed.Sector.X;
                    m_Position.Local.Y = decomposed.Local.X;
                    break;
                case 2:
                    m_Position.Sector.Z = decomposed.Sector.X;
                    m_Position.Local.Z = decomposed.Local.X;
                    break;
                default:
                    return false;
            }

            return true;
        }

        Math::SectorLocalPosition m_Position;
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
