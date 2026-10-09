#pragma once

#include "Engine/Core/Base.h"
#include "Engine/RHI/Device.h"
#include "Engine/Renderer/UiTexture.h"

#include <atomic>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace Engine
{
    // Native half of the UI-texture service: ImGui registration and the
    // presentation submission serials. The service logic below never touches a
    // native graphics type, so headless tests drive it with a fake bridge.
    //
    // A presentation serial numbers every ImGui draw submission in order.
    // Implementations must keep both values monotonic.
    class UiTextureNativeBridge
    {
    public:
        virtual ~UiTextureNativeBridge() = default;

        // Makes the texture's shader view drawable by ImGui and returns the
        // ImTextureID payload, or zero when the registration failed.
        virtual u64 Register(RHI::Texture& texture) = 0;
        // Releases a registration. The caller guarantees no incomplete GPU work
        // still reads it.
        virtual void Unregister(u64 imGuiId) = 0;
        // Highest serial of any presentation submission that may still
        // reference a texture released at this moment. While an ImGui frame is
        // open this is the serial that frame's own submission will carry,
        // because the frame's draw data can already name the texture.
        virtual u64 ReferenceBoundSerial() = 0;
        // Highest serial N such that every presentation submission up to and
        // including N has completed on the GPU. Polls without waiting.
        virtual u64 CompletedSerial() = 0;
    };

    // Renderer-owned dynamic RGBA8 texture registry (see UiTexture.h for the
    // Editor-facing contract and thread rule). Textures are created in
    // ShaderResource, their steady state, and every write is bracketed
    // ShaderResource -> CopyDest -> ShaderResource by RHI::RecordTextureWrite;
    // a new texture gets one zero-filling first write through the two-argument
    // transitions because a never-submitted image has no defined native layout.
    //
    // Destruction and resize retire the old GPU texture. It is released, and its
    // ImGui registration removed, only after (a) the RHI completion token of
    // its last write is Complete and (b) the bridge reports that every
    // presentation submission that could have drawn it has completed. The
    // service itself never waits for the GPU; Shutdown() must be called after
    // the owner has waited for the device to go idle.
    //
    // The constructing thread is the only thread that may call any method.
    class UiTextureService final
    {
    public:
        UiTextureService(RHI::Device& device, UiTextureNativeBridge& bridge);
        ~UiTextureService();

        UiTextureService(const UiTextureService&) = delete;
        UiTextureService& operator=(const UiTextureService&) = delete;

        // Returns kInvalidUiTextureHandle on failure; GetLastError() says why.
        // The new texture is transparent black.
        UiTextureHandle Create(u32 width, u32 height, std::string_view debugName);
        // Writes one dirty rectangle. Submitted without waiting; false means
        // nothing was recorded or submitted and the texture is unchanged.
        bool Update(UiTextureHandle handle, const UiTextureUpdate& update);
        // Replaces the GPU texture with a transparent-black one of the new size
        // and retires the old one. The handle stays valid, the ImGui id
        // changes, and the caller must write its full content again. A same-size
        // resize does nothing. On failure the old texture is untouched.
        bool Resize(UiTextureHandle handle, u32 width, u32 height);
        // Never blocks. The handle is stale immediately; the GPU texture is
        // retired later. A second Destroy of the same handle is rejected.
        bool Destroy(UiTextureHandle handle);
        // Zero for a stale or invalid handle.
        u64 GetImGuiId(UiTextureHandle handle) const;
        bool GetExtent(UiTextureHandle handle, u32& outWidth, u32& outHeight) const;

        // Releases every retirement whose write token and presentation bound
        // are complete and returns how many. Call once per frame.
        u32 CollectRetired();
        // Releases everything still registered after the owner's device-idle
        // wait. Live textures are leaks of the caller and are counted as such.
        void Shutdown();

        UiTextureCounters GetCounters() const;
        UiTextureError GetLastError() const { return m_LastError; }
        bool IsShutDown() const { return m_ShutDown; }

    private:
        struct GpuTexture
        {
            Scope<RHI::Texture> Resource;
            u64 ImGuiId = 0;
            u32 Width = 0;
            u32 Height = 0;
            RHI::CompletionToken LastWrite;
        };

        struct Slot
        {
            u32 Generation = 1;
            bool Live = false;
            std::string DebugName;
            GpuTexture Texture;
        };

        struct PooledList
        {
            Scope<RHI::CommandList> List;
            RHI::CompletionToken LastSubmission;
        };

        struct Retirement
        {
            GpuTexture Texture;
            u64 BoundSerial = 0;
        };

        bool OnOwnerThread() const;
        bool Fail(UiTextureError error);
        UiTextureHandle FailCreate(UiTextureError error);
        Slot* FindSlot(UiTextureHandle handle);
        const Slot* FindSlot(UiTextureHandle handle) const;
        UiTextureError CreateGpuTexture(u32 width, u32 height, std::string_view debugName, GpuTexture& outTexture);
        void ReleaseGpuTexture(GpuTexture& texture);
        void QueueRetirement(GpuTexture&& texture);
        // `firstUse` selects the two-argument transitions for a texture that
        // has never been in an accepted submission.
        UiTextureError RecordAndSubmit(RHI::Texture& texture, const RHI::TextureWrite& write, bool firstUse,
            RHI::CompletionToken& outToken);
        UiTextureError AcquireList(size_t& outIndex);
        u32 RegisteredCount() const;

        RHI::Device& m_Device;
        UiTextureNativeBridge& m_Bridge;
        std::thread::id m_OwnerThread;
        std::vector<Slot> m_Slots;
        std::vector<u32> m_FreeSlots;
        std::vector<PooledList> m_Lists;
        std::vector<Retirement> m_Retirements;
        UiTextureCounters m_Counters;
        mutable std::atomic<u64> m_WrongThreadRejections { 0 };
        UiTextureError m_LastError = UiTextureError::None;
        bool m_ShutDown = false;
    };
}
