#pragma once

#include "Engine/RHI/RHICommon.h"

#include <string>

namespace Engine::RHI
{
    enum class BufferUsage : u32
    {
        None = 0,
        Vertex = 1u << 0u,
        Index = 1u << 1u,
        Constant = 1u << 2u,
        Structured = 1u << 3u,
        Storage = 1u << 4u,
        IndirectArgs = 1u << 5u,
        AccelerationStructure = 1u << 6u,
        CopySource = 1u << 7u,
        CopyDest = 1u << 8u
    };

    struct BufferDescription
    {
        std::string DebugName;
        u64 SizeBytes = 0;
        u32 StrideBytes = 0;
        BufferUsage Usage = BufferUsage::None;
        ResourceState InitialState = ResourceState::Common;
    };

    class Buffer
    {
    public:
        virtual ~Buffer() = default;

        virtual const BufferDescription& GetDescription() const = 0;
    };
}
