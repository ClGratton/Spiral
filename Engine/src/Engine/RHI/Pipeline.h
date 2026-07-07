#pragma once

#include "Engine/RHI/RHICommon.h"

#include <string>

namespace Engine::RHI
{
    enum class PipelineType
    {
        Graphics,
        Compute,
        RayTracing
    };

    struct PipelineDescription
    {
        std::string DebugName;
        PipelineType Type = PipelineType::Graphics;
        Format ColorFormat = Format::R8G8B8A8Unorm;
        Format DepthFormat = Format::Unknown;
    };

    class Pipeline
    {
    public:
        virtual ~Pipeline() = default;

        virtual const PipelineDescription& GetDescription() const = 0;
    };
}
