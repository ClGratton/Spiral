#pragma once

#include "Engine/RHI/Buffer.h"
#include "Engine/RHI/BufferOwnership.h"
#include "Engine/RHI/TextureOwnership.h"
#include "Engine/RHI/Pipeline.h"
#include "Engine/RHI/Query.h"
#include "Engine/RHI/RHICommon.h"
#include "Engine/RHI/Texture.h"

#include <string_view>

namespace Engine::RHI
{
    class TextureBindingTable;

    enum class IndexFormat
    {
        Uint16,
        Uint32
    };

    struct Viewport
    {
        float X = 0.0f;
        float Y = 0.0f;
        float Width = 0.0f;
        float Height = 0.0f;
        float MinDepth = 0.0f;
        float MaxDepth = 1.0f;
    };

    struct ScissorRect
    {
        int Left = 0;
        int Top = 0;
        int Right = 0;
        int Bottom = 0;
    };

    struct ViewportClear
    {
        float Color[4] { 0.0f, 0.0f, 0.0f, 1.0f };
        float Depth = 1.0f;
        u8 Stencil = 0;
        bool ClearColor = true;
        bool ClearDepth = true;
    };

    class CommandList
    {
    public:
        virtual ~CommandList() = default;

        virtual QueueType GetQueueType() const = 0;
        virtual bool Begin() = 0;
        virtual bool End() = 0;
        virtual void BeginDebugMarker(std::string_view name) = 0;
        virtual void EndDebugMarker() = 0;
        // Renderer-owned targets only. Presentation retains swapchain/ImGui ownership.
        virtual bool BindViewportOutputs(Texture& colorTarget, Texture* depthTarget) = 0;
        virtual bool ClearViewportOutputs(const ViewportClear& clear) = 0;
        virtual bool TransitionTexture(Texture& texture, ResourceState destinationState) = 0;
        // Graph pre-recording supplies compiler-owned state intent. Implementations
        // record against `expectedBefore` privately and validate it at submission;
        // they must not publish wrapper state while recording.
        virtual bool TransitionTexture(Texture& texture, ResourceState expectedBefore, ResourceState destinationState)
        {
            // An adapter without explicit expected-before validation must fail
            // closed: forwarding to the legacy overload drops the contract.
            (void)texture;
            (void)expectedBefore;
            (void)destinationState;
            return false;
        }
        // Records one explicit whole-buffer transition. Graph execution supplies
        // state intent; this API does not infer pass or native state policy.
        virtual bool TransitionBuffer(Buffer& buffer, ResourceState destinationState) = 0;
        virtual bool TransitionBuffer(Buffer& buffer, ResourceState expectedBefore, ResourceState destinationState)
        {
            (void)buffer;
            (void)expectedBefore;
            (void)destinationState;
            return false;
        }
        virtual bool ReleaseBufferOwnership(const BufferOwnershipRelease& release) { (void)release; return false; }
        virtual bool AcquireBufferOwnership(const BufferOwnershipAcquire& acquire) { (void)acquire; return false; }
        virtual bool ReleaseTextureOwnership(const TextureOwnershipRelease& release) { (void)release; return false; }
        virtual bool AcquireTextureOwnership(const TextureOwnershipAcquire& acquire) { (void)acquire; return false; }
        virtual void SetGraphicsPipeline(Pipeline& pipeline) = 0;
        virtual void SetGraphicsConstantBuffer(u32 rootParameterIndex, Buffer& buffer) = 0;
        // Returns false before native recording unless this command list and the
        // supplied table share an exact device and the active pipeline declares
        // the fixed sampled-table binding. Backends without native descriptor
        // realization retain the default explicit rejection.
        virtual bool BindGraphicsSampledTextureTable(TextureBindingTable& table)
        {
            (void)table;
            return false;
        }
        virtual void SetViewport(const Viewport& viewport) = 0;
        virtual void SetScissorRect(const ScissorRect& rect) = 0;
        virtual void SetVertexBuffer(u32 slot, Buffer& buffer) = 0;
        virtual void SetIndexBuffer(Buffer& buffer, IndexFormat format) = 0;
        virtual bool CopyBuffer(Buffer& destination, u64 destinationOffset, Buffer& source, u64 sourceOffset, u64 sizeBytes) = 0;
        virtual void DrawIndexed(u32 indexCount, u32 instanceCount, u32 startIndex, int baseVertex, u32 startInstance) = 0;
        // False rejects the operation before an adapter records native work.
        virtual bool ResetQueryPool(QueryPool& queryPool, u32 firstQuery, u32 queryCount) = 0;
        virtual bool WriteTimestamp(QueryPool& queryPool, u32 queryIndex) = 0;
        virtual bool ResolveQueryPool(QueryPool& queryPool, u32 firstQuery, u32 queryCount) = 0;
    };
}
