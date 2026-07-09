#pragma once

#include "Engine/Core/Timestep.h"
#include "Engine/Scene/Components.h"
#include "Engine/Scene/Entity.h"

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
    };

    class Scene
    {
    public:
        explicit Scene(std::string name = "Untitled Scene");

        void OnUpdate(Timestep timestep);
        const std::string& GetName() const { return m_Name; }

        Entity CreateEntity(std::string name = "Entity");
        bool DestroyEntity(Entity entity);
        bool IsEntityValid(Entity entity) const;
        Entity FindEntityByName(std::string_view name) const;
        const std::vector<SceneEntity>& GetEntities() const { return m_Entities; }
        SceneEntity* TryGetEntity(Entity entity);
        const SceneEntity* TryGetEntity(Entity entity) const;
        TransformComponent* TryGetTransform(Entity entity);
        const TransformComponent* TryGetTransform(Entity entity) const;
        CameraComponent* AddCameraComponent(Entity entity, const CameraComponent& camera = {});
        CameraComponent* TryGetCameraComponent(Entity entity);
        const CameraComponent* TryGetCameraComponent(Entity entity) const;
        bool RemoveCameraComponent(Entity entity);

        Entity GetMainCameraEntity() const { return m_MainCameraEntity; }
        bool SetMainCameraEntity(Entity entity);
        const TransformComponent& GetMainCameraTransform() const { return m_MainCameraTransform; }
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
        std::vector<SceneEntity> m_Entities;
        EntityId m_NextEntityId = 1;
        Entity m_MainCameraEntity;
        TransformComponent m_MainCameraTransform;
        CameraComponent m_MainCamera;
    };
}
