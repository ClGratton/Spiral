#pragma once

#include "Engine/Assets/AssetHandle.h"
#include "Engine/Math/WorldGrid.h"
#include "Engine/Scene/Camera.h"

#include <cmath>
#include <string>
#include <string_view>

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

    enum class LightPhotometricUnit
    {
        Lux,
        Lumens
    };

    constexpr double kDefaultDirectionalIlluminanceLux = 10000.0;
    constexpr double kMaximumDirectionalIlluminanceLux = 1000000000.0;
    constexpr double kMaximumLocalLuminousFluxLumens = 10000000.0;
    constexpr double kLegacyDirectionalIntensityToLux = 10000.0;
    constexpr double kLegacyLocalIntensityToLumens = 100.0;

    inline const char* ToString(LightType type)
    {
        switch (type)
        {
            case LightType::Directional: return "Directional";
            case LightType::Point: return "Point";
            case LightType::Spot: return "Spot";
        }
        return "Directional";
    }

    inline bool TryParseLightType(std::string_view value, LightType& outType)
    {
        if (value == "Directional") outType = LightType::Directional;
        else if (value == "Point") outType = LightType::Point;
        else if (value == "Spot") outType = LightType::Spot;
        else return false;
        return true;
    }

    inline bool IsValidLightType(LightType type)
    {
        return type == LightType::Directional
            || type == LightType::Point || type == LightType::Spot;
    }

    inline const char* ToString(LightPhotometricUnit unit)
    {
        switch (unit)
        {
            case LightPhotometricUnit::Lux: return "Lux";
            case LightPhotometricUnit::Lumens: return "Lumens";
        }
        return "Lux";
    }

    inline bool TryParseLightPhotometricUnit(std::string_view value, LightPhotometricUnit& outUnit)
    {
        if (value == "Lux") outUnit = LightPhotometricUnit::Lux;
        else if (value == "Lumens") outUnit = LightPhotometricUnit::Lumens;
        else return false;
        return true;
    }

    inline LightPhotometricUnit GetLightPhotometricUnit(LightType type)
    {
        return type == LightType::Directional
            ? LightPhotometricUnit::Lux : LightPhotometricUnit::Lumens;
    }

    inline const char* GetLightPhotometricUnitSymbol(LightPhotometricUnit unit)
    {
        return unit == LightPhotometricUnit::Lux ? "lux" : "lm";
    }

    inline const char* GetLightPhotometricControlLabel(LightType type)
    {
        return type == LightType::Directional
            ? "Illuminance (lux)" : "Luminous flux (lm)";
    }

    inline double GetMaximumLightPhotometricValue(LightType type)
    {
        return type == LightType::Directional
            ? kMaximumDirectionalIlluminanceLux : kMaximumLocalLuminousFluxLumens;
    }

    inline bool IsValidLightPhotometricValue(
        LightType type, LightPhotometricUnit unit, double value)
    {
        return IsValidLightType(type) && unit == GetLightPhotometricUnit(type)
            && std::isfinite(value) && value >= 0.0
            && value <= GetMaximumLightPhotometricValue(type);
    }

    inline bool TryMigrateLegacyLightIntensity(LightType type, double legacyIntensity,
        double& outPhotometricValue, LightPhotometricUnit& outUnit)
    {
        if (!std::isfinite(legacyIntensity) || legacyIntensity < 0.0)
            return false;
        const double scale = type == LightType::Directional
            ? kLegacyDirectionalIntensityToLux : kLegacyLocalIntensityToLumens;
        const double value = legacyIntensity * scale;
        const LightPhotometricUnit unit = GetLightPhotometricUnit(type);
        if (!IsValidLightPhotometricValue(type, unit, value))
            return false;
        outPhotometricValue = value;
        outUnit = unit;
        return true;
    }

    struct LightComponent
    {
        LightType Type = LightType::Directional;
        Math::Vec3 Color = { 1.0f, 1.0f, 1.0f };
        double PhotometricValue = kDefaultDirectionalIlluminanceLux;
        LightPhotometricUnit PhotometricUnit = LightPhotometricUnit::Lux;
        float Range = 10.0f;
        float InnerConeDegrees = 25.0f;
        float OuterConeDegrees = 45.0f;
        bool CastsShadows = true;
    };

    inline bool IsValidLightComponent(const LightComponent& light)
    {
        return IsValidLightType(light.Type)
            && std::isfinite(light.Color.X) && light.Color.X >= 0.0f
            && std::isfinite(light.Color.Y) && light.Color.Y >= 0.0f
            && std::isfinite(light.Color.Z) && light.Color.Z >= 0.0f
            && IsValidLightPhotometricValue(
                light.Type, light.PhotometricUnit, light.PhotometricValue)
            && std::isfinite(light.Range) && light.Range >= 0.0f
            && std::isfinite(light.InnerConeDegrees) && light.InnerConeDegrees >= 0.0f
            && std::isfinite(light.OuterConeDegrees)
            && light.OuterConeDegrees >= light.InnerConeDegrees
            && light.OuterConeDegrees <= 180.0f;
    }

    struct MeshRendererComponent
    {
        AssetHandle MeshAsset = kInvalidAssetHandle;
        AssetHandle MaterialAsset = kInvalidAssetHandle;
        std::string MeshName;
        bool Visible = true;
        bool CastsShadows = true;
    };
}
