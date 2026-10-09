#pragma once

#include "Engine/RHI/Buffer.h"
#include "Engine/RHI/BufferOwnership.h"
#include "Engine/RHI/TextureOwnership.h"
#include "Engine/RHI/Pipeline.h"
#include "Engine/RHI/Query.h"
#include "Engine/RHI/RHICommon.h"
#include "Engine/RHI/Texture.h"
#include "Engine/RHI/TextureWrite.h"

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
        // Dedicated depth-only rendering for shadow maps and selected prepasses.
        // Backends that have not implemented a native depth-only framebuffer
        // reject explicitly instead of allocating a hidden color attachment.
        virtual bool BindDepthOutput(Texture& depthTarget)
        {
            (void)depthTarget;
            return false;
        }
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
        // Records one non-stalling CPU-to-texture write: the source bytes are
        // copied (or privately staged) during this call and the backend retains
        // that staging until the submission carrying this list completes, so the
        // caller may reuse `Data` immediately. The destination must already be
        // in CopyDest at this point of the recording, either by an explicit
        // TransitionTexture earlier in this list or by being left there by an
        // accepted submission. Validation (`ValidateTextureWrite`) runs before
        // any native work, so a false return records nothing and leaves the
        // list's staged texture state unchanged. Recording is limited to a
        // Graphics-queue list. The default rejects, which is the answer for
        // every backend and test list that has not implemented the capability;
        // `Device::SupportsTextureWrite` reports the implemented answer.
        virtual bool WriteTexture(Texture& texture, const TextureWrite& write)
        {
            (void)texture;
            (void)write;
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
        // Binds the one renderer-internal SRV declared by the active graphics
        // pipeline. The texture must already be in ShaderResource state.
        virtual bool BindGraphicsSampledTexture(Texture& texture)
        {
            (void)texture;
            return false;
        }
        // Binds the one optional pixel-stage StructuredBuffer<uint4> declared
        // at t0,space3 by the active pipeline. This fixed call intentionally
        // exposes no register, space, array, or writable-buffer controls.
        virtual bool BindGraphicsReadOnlyStructuredBuffer(Buffer& buffer)
        {
            (void)buffer;
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

    // Documented bracket for a repeatable update of a texture whose steady state
    // is `steadyState` (normally ShaderResource): steady -> CopyDest, one
    // WriteTexture, CopyDest -> steady, all with explicit expected-before
    // states so submission validates the live state. The write is fully
    // validated before the first transition, so a false return from validation
    // records nothing. A false return after recording began (a native rejection
    // of a transition or write) leaves the list in a partially recorded state
    // and the caller must discard it without submitting.
    //
    // A texture that has never been in an accepted submission has no defined
    // native layout; give it a first write with the two-argument
    // TransitionTexture overloads (which let the backend start from the real
    // initial layout) before relying on this explicit-expected form. Create
    // dynamic textures with InitialState equal to `steadyState` so backends
    // that restore a texture's creation state at list close agree with the
    // bracket.
    inline bool RecordTextureWrite(CommandList& commandList, Texture& texture, const TextureWrite& write,
        ResourceState steadyState = ResourceState::ShaderResource)
    {
        if (steadyState == ResourceState::Unknown || steadyState == ResourceState::CopyDest
            || ValidateTextureWrite(texture.GetDescription(), ResourceState::CopyDest, write) != TextureWriteStatus::Valid)
            return false;
        return commandList.TransitionTexture(texture, steadyState, ResourceState::CopyDest)
            && commandList.WriteTexture(texture, write)
            && commandList.TransitionTexture(texture, ResourceState::CopyDest, steadyState);
    }

    // Keeps backend marker nesting balanced across early returns and exceptions.
    // The command list must outlive the scope and already be recording.
    class ScopedDebugMarker final
    {
    public:
        ScopedDebugMarker(CommandList& commandList, std::string_view name)
            : m_CommandList(commandList)
        {
            m_CommandList.BeginDebugMarker(name);
        }

        ~ScopedDebugMarker()
        {
            m_CommandList.EndDebugMarker();
        }

        ScopedDebugMarker(const ScopedDebugMarker&) = delete;
        ScopedDebugMarker& operator=(const ScopedDebugMarker&) = delete;

    private:
        CommandList& m_CommandList;
    };
}
