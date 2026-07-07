#pragma once

#include "Engine/Core/Timestep.h"

#include <string>

namespace Engine
{
    class Scene
    {
    public:
        explicit Scene(std::string name = "Untitled Scene");

        void OnUpdate(Timestep timestep);
        const std::string& GetName() const { return m_Name; }

    private:
        std::string m_Name;
    };
}
