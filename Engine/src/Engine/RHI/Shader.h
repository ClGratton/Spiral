#pragma once

#include "Engine/RHI/RHICommon.h"

#include <string>
#include <vector>

namespace Engine::RHI
{
    // Engine-owned reflection data produced by the portable shader compiler.
    // RHI consumes this value-only metadata but never parses source or owns a
    // compiler; Renderer remains the producer and validator.
    struct ShaderReflectionBinding
    {
        std::string Name;
        char Kind = 'b';
        u32 Register = 0;
        u32 Space = 0;
        ShaderStage Stages = ShaderStage::None;
        std::string ResourceKind;
        std::string TypeShape;
        u32 Count = 1;
        u32 ByteSize = 0;
        u32 Rows = 0;
        u32 Columns = 0;

        bool operator==(const ShaderReflectionBinding&) const = default;
    };

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
        std::vector<ShaderReflectionBinding> Reflection;
    };

    class Shader
    {
    public:
        virtual ~Shader() = default;

        virtual const ShaderDescription& GetDescription() const = 0;
    };
}
