#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Layer.h"

#include <vector>

namespace Engine
{
    class LayerStack
    {
    public:
        LayerStack() = default;
        ~LayerStack();

        Layer* PushLayer(Scope<Layer> layer);
        Layer* PushOverlay(Scope<Layer> overlay);
        void PopLayer(Layer* layer);
        void PopOverlay(Layer* overlay);

        auto begin() { return m_Layers.begin(); }
        auto end() { return m_Layers.end(); }
        auto rbegin() { return m_Layers.rbegin(); }
        auto rend() { return m_Layers.rend(); }

    private:
        std::vector<Scope<Layer>> m_Layers;
        std::size_t m_LayerInsertIndex = 0;
    };
}
