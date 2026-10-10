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
        // Called inside an open UI frame (between ImGuiLayer::Begin and End when
        // an ImGui layer is attached), and only in iterations that rendered a
        // frame. Never called while the window is minimized.
        virtual void OnUiRender() {}
        // Called instead of OnUiRender in iterations that did not render a frame
        // (minimized window), outside any UI frame. Pages, downloads and other
        // background services that must keep running belong here; it must not
        // call ImGui.
        virtual void OnBackgroundUpdate() {}
        virtual void OnEvent(Event& event) { (void)event; }

        const std::string& GetName() const { return m_DebugName; }

    protected:
        std::string m_DebugName;
    };
}
