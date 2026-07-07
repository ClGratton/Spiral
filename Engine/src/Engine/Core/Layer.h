#pragma once

#include "Engine/Core/Timestep.h"

#include <string>

namespace Engine
{
    class Event;

    class Layer
    {
    public:
        explicit Layer(std::string name = "Layer");
        virtual ~Layer() = default;

        virtual void OnAttach() {}
        virtual void OnDetach() {}
        virtual void OnUpdate(Timestep timestep) { (void)timestep; }
        virtual void OnFixedUpdate(Timestep timestep) { (void)timestep; }
        virtual void OnRender() {}
        virtual void OnUiRender() {}
        virtual void OnEvent(Event& event) { (void)event; }

        const std::string& GetName() const { return m_DebugName; }

    protected:
        std::string m_DebugName;
    };
}
