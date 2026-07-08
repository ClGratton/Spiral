#pragma once

#include "Engine/RHI/RHICommon.h"

#include <string>

namespace Engine::RHI
{
    struct ShaderDescription
    {
        std::string DebugName;
        std::string SourceName;
        std::string Source;
        std::string EntryPoint = "main";
        std::string TargetProfile;
        ShaderStage Stage = ShaderStage::None;
    };

    class Shader
    {
    public:
        virtual ~Shader() = default;

        virtual const ShaderDescription& GetDescription() const = 0;
    };
}
