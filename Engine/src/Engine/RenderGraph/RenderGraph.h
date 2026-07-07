#pragma once

#include <string>
#include <vector>

namespace Engine
{
    class RenderGraph
    {
    public:
        void AddDebugPass(std::string name)
        {
            m_DebugPasses.emplace_back(std::move(name));
        }

        const std::vector<std::string>& GetDebugPasses() const
        {
            return m_DebugPasses;
        }

    private:
        std::vector<std::string> m_DebugPasses;
    };
}
