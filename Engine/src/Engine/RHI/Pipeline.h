#pragma once

#include "Engine/RHI/RHICommon.h"
#include "Engine/RHI/Shader.h"

#include <string>
#include <vector>

namespace Engine::RHI
{
    enum class PipelineType
    {
        Graphics,
        Compute,
        RayTracing
    };

    enum class PrimitiveTopology
    {
        TriangleList
    };

    enum class CullMode
    {
        None,
        Front,
        Back
    };

    enum class VertexInputRate
    {
        PerVertex,
        PerInstance
    };

    struct VertexInputAttribute
    {
        std::string SemanticName;
        u32 SemanticIndex = 0;
        Format AttributeFormat = Format::Unknown;
        u32 InputSlot = 0;
        u32 OffsetBytes = 0;
        VertexInputRate InputRate = VertexInputRate::PerVertex;
        u32 InstanceStepRate = 0;
    };

    struct RootConstantBufferBinding
    {
        u32 ShaderRegister = 0;
        u32 RegisterSpace = 0;
        ShaderStage Visibility = ShaderStage::AllGraphics;
    };

    struct PipelineDescription
    {
        std::string DebugName;
        PipelineType Type = PipelineType::Graphics;
        Shader* VertexShader = nullptr;
        Shader* PixelShader = nullptr;
        std::vector<VertexInputAttribute> VertexInputs;
        std::vector<RootConstantBufferBinding> ConstantBufferBindings;
        PrimitiveTopology Topology = PrimitiveTopology::TriangleList;
        CullMode RasterCullMode = CullMode::Back;
        Format ColorFormat = Format::R8G8B8A8Unorm;
        Format DepthFormat = Format::Unknown;
        bool DepthTestEnable = false;
        bool DepthWriteEnable = false;
    };

    class Pipeline
    {
    public:
        virtual ~Pipeline() = default;

        virtual const PipelineDescription& GetDescription() const = 0;
    };
}
