#include "Engine/Scene/Scene.h"

namespace Engine
{
    Scene::Scene(std::string name)
        : m_Name(std::move(name))
    {
        m_MainCameraTransform.Position = { 0.0f, 0.0f, -3.35f };
    }

    void Scene::OnUpdate(Timestep timestep)
    {
        (void)timestep;
    }
}
