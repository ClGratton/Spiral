#pragma once

#include "Engine/RHI/RHICommon.h"
#include "Engine/RHI/Shader.h"

#include <optional>
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

    // The sampled read-only texture table has one portable declaration even
    // while native descriptor realization remains deferred. Space one keeps it
    // separate from the current per-draw b0,space0 constant-buffer contract;
    // the Vulkan offsets reserve distinct bindings when a native consumer is
    // admitted.
    struct SampledTextureTableBinding
    {
        u32 TextureRegister = 0;
        u32 SamplerRegister = 0;
        u32 RegisterSpace = 1;
        u32 VulkanTextureBindingOffset = 1;
        u32 VulkanSamplerBindingOffset = 2;
    };

    inline bool IsValidSampledTextureTableBinding(const SampledTextureTableBinding& binding,
        const std::vector<RootConstantBufferBinding>& constantBuffers)
    {
        if (binding.TextureRegister != 0 || binding.SamplerRegister != 0 || binding.RegisterSpace != 1
            || binding.VulkanTextureBindingOffset != 1 || binding.VulkanSamplerBindingOffset != 2) return false;
        for (const RootConstantBufferBinding& constantBuffer : constantBuffers)
            if (constantBuffer.RegisterSpace == binding.RegisterSpace) return false;
        return true;
    }

    struct PipelineDescription
    {
        std::string DebugName;
        PipelineType Type = PipelineType::Graphics;
        Shader* VertexShader = nullptr;
        Shader* PixelShader = nullptr;
        std::vector<VertexInputAttribute> VertexInputs;
        std::vector<RootConstantBufferBinding> ConstantBufferBindings;
        std::optional<SampledTextureTableBinding> SampledTextureTable;
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
