#include "Engine/Core/LayerStack.h"

#include <algorithm>

namespace Engine
{
    LayerStack::~LayerStack()
    {
        for (auto& layer : m_Layers)
            layer->OnDetach();
    }

    Layer* LayerStack::PushLayer(Scope<Layer> layer)
    {
        Layer* rawLayer = layer.get();
        m_Layers.emplace(m_Layers.begin() + static_cast<std::ptrdiff_t>(m_LayerInsertIndex), std::move(layer));
        ++m_LayerInsertIndex;
        rawLayer->OnAttach();
        return rawLayer;
    }

    Layer* LayerStack::PushOverlay(Scope<Layer> overlay)
    {
        Layer* rawLayer = overlay.get();
        m_Layers.emplace_back(std::move(overlay));
        rawLayer->OnAttach();
        return rawLayer;
    }

    void LayerStack::PopLayer(Layer* layer)
    {
        auto it = std::find_if(m_Layers.begin(), m_Layers.begin() + static_cast<std::ptrdiff_t>(m_LayerInsertIndex),
            [layer](const Scope<Layer>& candidate) { return candidate.get() == layer; });

        if (it == m_Layers.begin() + static_cast<std::ptrdiff_t>(m_LayerInsertIndex))
            return;

        (*it)->OnDetach();
        m_Layers.erase(it);
        --m_LayerInsertIndex;
    }

    void LayerStack::PopOverlay(Layer* overlay)
    {
        auto it = std::find_if(m_Layers.begin() + static_cast<std::ptrdiff_t>(m_LayerInsertIndex), m_Layers.end(),
            [overlay](const Scope<Layer>& candidate) { return candidate.get() == overlay; });

        if (it == m_Layers.end())
            return;

        (*it)->OnDetach();
        m_Layers.erase(it);
    }
}
