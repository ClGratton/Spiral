#include "Engine/Scene/Scene.h"

namespace Engine
{
    Scene::Scene(std::string name)
        : m_Name(std::move(name))
    {
    }

    void Scene::OnUpdate(Timestep timestep)
    {
        (void)timestep;
    }
}
