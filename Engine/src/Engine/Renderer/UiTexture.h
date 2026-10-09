#pragma once

#include "Engine/Core/Base.h"

namespace Engine
{
    // Editor-facing contract of the Renderer-owned dynamic UI-texture service
    // (Renderer::CreateUiTexture and friends). A UI texture is one RGBA8
    // (R8G8B8A8Unorm, straight alpha) 2D texture that CPU code updates by dirty
    // rectangle and ImGui draws with ImGui::Image.
    //
    // Thread rule: every call is main-thread-only (the thread that initialized
    // ImGui). A call from any other thread is rejected and counted, never
    // executed. Nothing in this service blocks on the GPU: updates are
    // submitted without a completion wait and destruction is deferred.
    using UiTextureHandle = u64;
    constexpr UiTextureHandle kInvalidUiTextureHandle = 0;

    constexpr u32 kMaximumUiTextureDimension = 4096;
    // Live textures plus textures still waiting for GPU retirement. It stays
    // well under the 128 sampled-image descriptors of the Vulkan ImGui pool and
    // the 256-entry D3D12 SRV heap that the font atlas and Scene viewport share.
    constexpr u32 kMaximumRegisteredUiTextures = 64;
    // Submitted-but-incomplete update command lists. A further update while all
    // are busy is rejected (UpdateBackpressure) instead of waiting.
    constexpr u32 kMaximumUiTextureCommandLists = 8;

    // Rectangle inside the texture, in texels.
    struct UiTextureRect
    {
        u32 X = 0;
        u32 Y = 0;
        u32 Width = 0;
        u32 Height = 0;
    };

    // `Pixels` points at the top-left texel of `Rect` inside CPU RGBA8 memory.
    // Rows are `RowPitchBytes` apart (zero means tightly packed) and
    // `PixelBytes` is the readable size from `Pixels`; the last row needs only
    // its tight width. The service copies the bytes before the call returns, so
    // the caller may reuse the memory immediately.
    struct UiTextureUpdate
    {
        UiTextureRect Rect;
        const void* Pixels = nullptr;
        u64 RowPitchBytes = 0;
        u64 PixelBytes = 0;
    };

    // The reason the most recent call failed. Rejection is atomic: a failed
    // call leaves every texture, handle, registration and counter-visible
    // resource exactly as it was.
    enum class UiTextureError : u8
    {
        None,
        ServiceUnavailable,
        InvalidSize,
        InvalidHandle,
        InvalidUpdate,
        TextureLimit,
        DeviceCreateFailed,
        NativeRegistrationFailed,
        InitialWriteFailed,
        UpdateBackpressure,
        UpdateRecordFailed,
        UpdateSubmitFailed
    };

    inline const char* ToString(UiTextureError error)
    {
        switch (error)
        {
            case UiTextureError::None: return "None";
            case UiTextureError::ServiceUnavailable: return "ServiceUnavailable";
            case UiTextureError::InvalidSize: return "InvalidSize";
            case UiTextureError::InvalidHandle: return "InvalidHandle";
            case UiTextureError::InvalidUpdate: return "InvalidUpdate";
            case UiTextureError::TextureLimit: return "TextureLimit";
            case UiTextureError::DeviceCreateFailed: return "DeviceCreateFailed";
            case UiTextureError::NativeRegistrationFailed: return "NativeRegistrationFailed";
            case UiTextureError::InitialWriteFailed: return "InitialWriteFailed";
            case UiTextureError::UpdateBackpressure: return "UpdateBackpressure";
            case UiTextureError::UpdateRecordFailed: return "UpdateRecordFailed";
            case UiTextureError::UpdateSubmitFailed: return "UpdateSubmitFailed";
        }
        return "Unknown";
    }

    struct UiTextureCounters
    {
        // Public API outcomes.
        u64 Created = 0;
        u64 Resized = 0;
        u64 Destroyed = 0;
        u64 UpdatesSubmitted = 0;
        u64 UpdateBytes = 0;
        u64 Rejected = 0;
        u64 WrongThreadRejections = 0;
        // Zero-filling writes that give a new texture a defined layout and
        // transparent content before ImGui can sample it.
        u64 InitialWritesSubmitted = 0;
        // GPU texture and ImGui registration life cycle.
        u64 GpuTexturesCreated = 0;
        u64 GpuTexturesReleased = 0;
        u64 NativeRegistrations = 0;
        u64 NativeUnregistrations = 0;
        // Deferred destruction. A queued retirement is released only when every
        // recorded write token and every presentation submission that may have
        // referenced the texture has completed; shutdown drains the rest after
        // the caller's WaitIdle.
        u64 RetirementsQueued = 0;
        u64 RetirementsReleased = 0;
        u64 RetirementsDrainedAtShutdown = 0;
        u64 RetirementHoldsWriteToken = 0;
        u64 RetirementHoldsPresentation = 0;
        u64 CommandListsCreated = 0;
        u64 CommandListReuses = 0;
        u32 LiveTextures = 0;
        u32 PendingRetirements = 0;
        u32 PeakPendingRetirements = 0;
        u32 LeakedAtShutdown = 0;

        // Holds at every point in time, not only at shutdown.
        bool IsBalanced() const
        {
            const u64 outstanding = static_cast<u64>(LiveTextures) + PendingRetirements;
            return GpuTexturesCreated == GpuTexturesReleased + outstanding
                && NativeRegistrations == NativeUnregistrations + outstanding
                && RetirementsQueued == RetirementsReleased + RetirementsDrainedAtShutdown + PendingRetirements;
        }

        bool IsDrained() const { return LiveTextures == 0 && PendingRetirements == 0; }
    };
}
