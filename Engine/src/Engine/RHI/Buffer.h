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

    enum class BufferCpuAccess
    {
        None,
        Write,
        Read
    };

    // Buffer transitions describe GPU-visible uses only. CPU-visible upload and
    // readback buffers have fixed native heap states and are not transitionable.
    // Vertex, index, constant, and structured buffers share ShaderResource as
    // the backend-neutral read-only state; the backend selects its compatible
    // native read state.
    inline bool IsBufferStateCompatible(BufferUsage usage, BufferCpuAccess cpuAccess, ResourceState state)
    {
        if (cpuAccess != BufferCpuAccess::None)
            return false;

        const auto hasUsage = [usage](BufferUsage flag)
        {
            return (static_cast<u32>(usage) & static_cast<u32>(flag)) != 0;
        };

        switch (state)
        {
            case ResourceState::Common: return true;
            case ResourceState::ShaderResource:
                return hasUsage(BufferUsage::Vertex) || hasUsage(BufferUsage::Index)
                    || hasUsage(BufferUsage::Constant) || hasUsage(BufferUsage::Structured)
                    || hasUsage(BufferUsage::Storage);
            case ResourceState::UnorderedAccess: return hasUsage(BufferUsage::Storage);
            case ResourceState::CopySource: return hasUsage(BufferUsage::CopySource);
            case ResourceState::CopyDest: return hasUsage(BufferUsage::CopyDest);
            case ResourceState::Unknown:
            case ResourceState::RenderTarget:
            case ResourceState::DepthWrite:
            case ResourceState::Present:
                return false;
        }

        return false;
    }

    struct BufferDescription
    {
        std::string DebugName;
        u64 SizeBytes = 0;
        u32 StrideBytes = 0;
        BufferUsage Usage = BufferUsage::None;
        BufferCpuAccess CpuAccess = BufferCpuAccess::None;
        ResourceState InitialState = ResourceState::Common;
    };

    class Buffer
    {
    public:
        virtual ~Buffer() = default;

        virtual const BufferDescription& GetDescription() const = 0;
        virtual void* Map() = 0;
        virtual void Unmap() = 0;
    };
}
