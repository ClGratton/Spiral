#pragma once

#include "Engine/RHI/RHICommon.h"

#include <string>
#include <vector>

namespace Engine::RHI
{
    enum class ShaderBinaryFormat
    {
        None,
        Dxil,
        Spirv
    };

    struct ShaderDescription
    {
        std::string DebugName;
        std::string SourceName;
        std::string Source;
        std::string EntryPoint = "main";
        std::string TargetProfile;
        ShaderStage Stage = ShaderStage::None;
        ShaderBinaryFormat BinaryFormat = ShaderBinaryFormat::None;
        std::vector<u8> Binary;
    };

    class Shader
    {
    public:
        virtual ~Shader() = default;

        virtual const ShaderDescription& GetDescription() const = 0;
    };
}
