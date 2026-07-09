#pragma once

#include "Engine/Core/Timestep.h"
#include "Engine/Scene/Components.h"

#include <filesystem>
#include <string>

namespace Engine
{
    class Scene
    {
    public:
        explicit Scene(std::string name = "Untitled Scene");

        void OnUpdate(Timestep timestep);
        const std::string& GetName() const { return m_Name; }
        const TransformComponent& GetMainCameraTransform() const { return m_MainCameraTransform; }
        const CameraComponent& GetMainCamera() const { return m_MainCamera; }
        void SetMainCameraTransform(const TransformComponent& transform);
        void SetMainCamera(const CameraComponent& camera);

        bool SaveToFile(const std::filesystem::path& path) const;
        static bool LoadFromFile(const std::filesystem::path& path, Scene& outScene);

    private:
        std::string m_Name;
        TransformComponent m_MainCameraTransform;
        CameraComponent m_MainCamera;
    };
}
