#pragma once

#include "Engine/Core/Timestep.h"
#include "Engine/Scene/Components.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/SceneRenderSnapshot.h"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Engine
{
    struct SceneEntity
    {
        Entity EntityHandle;
        std::string Name;
        TransformComponent Transform;
        std::optional<CameraComponent> Camera;
        std::optional<LightComponent> Light;
        std::optional<MeshRendererComponent> MeshRenderer;
    };

    class Scene
    {
    public:
        explicit Scene(
            std::string name = "Untitled Scene",
            Math::WorldGridPolicy worldGridPolicy = {});

        void OnUpdate(Timestep timestep);
        const std::string& GetName() const { return m_Name; }
        const Math::WorldGridPolicy& GetWorldGridPolicy() const { return m_WorldGridPolicy; }
        SceneRenderSnapshot ExtractRenderSnapshot(u64 frameIndex, const CameraView& renderView) const;

        Entity CreateEntity(std::string name = "Entity");
        bool DestroyEntity(Entity entity);
        bool IsEntityValid(Entity entity) const;
        Entity FindEntityByName(std::string_view name) const;
        const std::vector<SceneEntity>& GetEntities() const { return m_Entities; }
        SceneEntity* TryGetEntity(Entity entity);
        const SceneEntity* TryGetEntity(Entity entity) const;
        TransformComponent* TryGetTransform(Entity entity);
        const TransformComponent* TryGetTransform(Entity entity) const;
        bool SetEntityWorldPosition(Entity entity, const Math::DVec3& position);
        bool SetEntityWorldPositionAxis(Entity entity, u32 axis, double position);
        bool SetEntitySectorLocalPosition(Entity entity, const Math::SectorLocalPosition& position);
        bool TryGetEntityApproximateWorldPosition(Entity entity, Math::DVec3& outPosition) const;
        CameraComponent* AddCameraComponent(Entity entity, const CameraComponent& camera = {});
        CameraComponent* TryGetCameraComponent(Entity entity);
        const CameraComponent* TryGetCameraComponent(Entity entity) const;
        bool RemoveCameraComponent(Entity entity);
        LightComponent* AddLightComponent(Entity entity, const LightComponent& light = {});
        LightComponent* TryGetLightComponent(Entity entity);
        const LightComponent* TryGetLightComponent(Entity entity) const;
        bool RemoveLightComponent(Entity entity);
        MeshRendererComponent* AddMeshRendererComponent(Entity entity, const MeshRendererComponent& meshRenderer = {});
        MeshRendererComponent* TryGetMeshRendererComponent(Entity entity);
        const MeshRendererComponent* TryGetMeshRendererComponent(Entity entity) const;
        bool RemoveMeshRendererComponent(Entity entity);

        Entity GetMainCameraEntity() const { return m_MainCameraEntity; }
        bool SetMainCameraEntity(Entity entity);
        const TransformComponent& GetMainCameraTransform() const;
        const CameraComponent& GetMainCamera() const { return m_MainCamera; }
        void SetMainCameraTransform(const TransformComponent& transform);
        void SetMainCamera(const CameraComponent& camera);

        bool SaveToFile(const std::filesystem::path& path) const;
        static bool LoadFromFile(const std::filesystem::path& path, Scene& outScene);

    private:
        SceneEntity* FindEntityStorage(Entity entity);
        const SceneEntity* FindEntityStorage(Entity entity) const;
        void SyncMainCameraCacheFromEntity();
        Entity CreateEntityWithId(EntityId id, std::string name);

    private:
        std::string m_Name;
        Math::WorldGridPolicy m_WorldGridPolicy;
        std::vector<SceneEntity> m_Entities;
        EntityId m_NextEntityId = 1;
        Entity m_MainCameraEntity;
        CameraComponent m_MainCamera;
    };
}
