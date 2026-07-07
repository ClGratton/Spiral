#include "Engine/Core/Layer.h"

namespace Engine
{
    Layer::Layer(std::string name)
        : m_DebugName(std::move(name))
    {
    }
}
