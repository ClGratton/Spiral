#pragma once

#include "Engine/RHI/Capability.h"
#include "Engine/RHI/CompletionToken.h"
#include "Engine/RHI/Device.h"

#include <limits>
#include <vector>

namespace Engine::RHI
{
    // Logical sampler state only. Native sampler objects and shader bindings are
    // deliberately deferred until the texture-upload consumer exists.
    enum class TextureSampler : u32
    {
        LinearClamp,
        LinearWrap,
        PointClamp,
        PointWrap
    };

    constexpr u32 kMaximumReadOnlyTextureTableCapacity = 4096;

    struct TextureBindingTableDescription
    {
        // Includes the mandatory error slot at index zero. The bounded fallback
        // uses precisely this capacity; it never depends on a vendor identity.
        u32 Capacity = 0;
        Ref<Texture> ErrorTexture;
        TextureSampler ErrorSampler = TextureSampler::LinearClamp;
    };

    struct TextureBindingHandle
    {
        static constexpr u32 InvalidIndex = std::numeric_limits<u32>::max();
        u32 Index = InvalidIndex;
        u32 Generation = 0;

        bool IsValid() const { return Index != InvalidIndex && Generation != 0; }
    };

    struct TextureBindingView
    {
        Ref<Texture> TextureResource;
        TextureSampler Sampler = TextureSampler::LinearClamp;
        bool IsError = true;
    };

    CapabilityPath SelectReadOnlyTextureTablePath(const DeviceCapabilities& capabilities);

    // CPU-side authority for future Scene texture/material consumers. It models
    // only read-only sampled texture identities; it creates no native descriptor
    // heap, sampler, shader binding, or texture upload work.
    class TextureBindingTable
    {
    public:
        static Scope<TextureBindingTable> Create(Device& device, const TextureBindingTableDescription& description);

        CapabilityPath GetSelectedPath() const { return m_SelectedPath; }
        u32 GetCapacity() const { return static_cast<u32>(m_Slots.size()); }
        TextureBindingHandle GetErrorHandle() const { return { 0, m_Slots[0].Generation }; }
        TextureBindingHandle Allocate(const Ref<Texture>& texture, TextureSampler sampler);
        bool QueueUpdate(TextureBindingHandle handle, const Ref<Texture>& replacement, TextureSampler sampler, const CompletionToken& retireToken);
        bool QueueRemoval(TextureBindingHandle handle, const CompletionToken& retireToken);
        // Applies only operations owned by this exact token after the exact device
        // reports it Complete or Failed. Incomplete/foreign/invalid tokens do not
        // mutate publication or make a slot reusable.
        bool Retire(const CompletionToken& token);
        TextureBindingView Resolve(TextureBindingHandle handle) const;
        u32 GetPendingOperationCount() const;

    private:
        struct PendingOperation
        {
            enum class Kind : u32 { Update, Remove } Operation = Kind::Update;
            CompletionToken Token;
            Ref<Texture> TextureResource;
            TextureSampler Sampler = TextureSampler::LinearClamp;
        };
        struct Slot
        {
            Ref<Texture> TextureResource;
            TextureSampler Sampler = TextureSampler::LinearClamp;
            u32 Generation = 1;
            bool Active = false;
            std::vector<PendingOperation> Pending;
        };

        TextureBindingTable(Device& device, CapabilityPath selectedPath, const TextureBindingTableDescription& description);
        bool IsReadOnlyOwnedTexture(const Ref<Texture>& texture) const;
        static bool IsValidSampler(TextureSampler sampler);
        bool IsCurrent(TextureBindingHandle handle) const;
        bool CanRetireToken(const CompletionToken& token) const;

        Device& m_Device;
        CapabilityPath m_SelectedPath = CapabilityPath::None;
        std::vector<Slot> m_Slots;
    };
}
