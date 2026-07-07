#pragma once

#include "Engine/Core/Layer.h"

namespace Engine
{
    class ImGuiLayer final : public Layer
    {
    public:
        ImGuiLayer();
        ~ImGuiLayer() override = default;

        void OnAttach() override;
        void OnDetach() override;

        void Begin();
        void End();

        void SetDarkThemeColors();

    private:
        bool m_UseNativeRenderer = false;
    };
}
