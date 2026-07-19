#include "Engine/RHI/TextureBindingTable.h"

namespace Engine::RHI
{
    CapabilityPath SelectReadOnlyTextureTablePath(const DeviceCapabilities& capabilities)
    {
        return capabilities.GetFeature(DeviceFeature::DescriptorIndexing).IsUsable()
            ? CapabilityPath::ReadOnlyBindlessDescriptorTable
            : CapabilityPath::BoundedReadOnlyTextureTable;
    }

    Scope<TextureBindingTable> TextureBindingTable::Create(Device& device, const TextureBindingTableDescription& description)
    {
        if (description.Capacity < 2 || description.Capacity > kMaximumReadOnlyTextureTableCapacity
            || !description.ErrorTexture || !IsValidSampler(description.ErrorSampler)) return nullptr;
        Scope<TextureBindingTable> table(new TextureBindingTable(
            device, SelectReadOnlyTextureTablePath(device.GetCapabilities()), description));
        if (!table->IsReadOnlyOwnedTexture(description.ErrorTexture)) return nullptr;
        return table;
    }

    TextureBindingTable::TextureBindingTable(Device& device, CapabilityPath selectedPath, const TextureBindingTableDescription& description)
        : m_Device(device), m_SelectedPath(selectedPath), m_Slots(description.Capacity)
    {
        m_Slots[0].TextureResource = description.ErrorTexture;
        m_Slots[0].Sampler = description.ErrorSampler;
        m_Slots[0].Active = true;
    }

    bool TextureBindingTable::IsReadOnlyOwnedTexture(const Ref<Texture>& texture) const
    {
        if (!texture || !m_Device.OwnsResource(texture.get())) return false;
        const TextureUsage usage = texture->GetDescription().Usage;
        const u32 writableUsage = static_cast<u32>(TextureUsage::RenderTarget)
            | static_cast<u32>(TextureUsage::DepthStencil) | static_cast<u32>(TextureUsage::UnorderedAccess);
        return (static_cast<u32>(usage) & static_cast<u32>(TextureUsage::ShaderResource)) != 0
            && (static_cast<u32>(usage) & writableUsage) == 0;
    }

    bool TextureBindingTable::IsValidSampler(TextureSampler sampler)
    {
        switch (sampler)
        {
            case TextureSampler::LinearClamp:
            case TextureSampler::LinearWrap:
            case TextureSampler::PointClamp:
            case TextureSampler::PointWrap: return true;
        }
        return false;
    }

    bool TextureBindingTable::IsCurrent(TextureBindingHandle handle) const
    {
        return handle.IsValid() && handle.Index > 0 && handle.Index < m_Slots.size()
            && m_Slots[handle.Index].Active && m_Slots[handle.Index].Generation == handle.Generation;
    }

    bool TextureBindingTable::CanRetireToken(const CompletionToken& token) const
    {
        return token.IsValid() && m_Device.QueryCompletion(token) != CompletionStatus::Invalid;
    }

    TextureBindingHandle TextureBindingTable::Allocate(const Ref<Texture>& texture, TextureSampler sampler)
    {
        if (!IsReadOnlyOwnedTexture(texture) || !IsValidSampler(sampler)) return {};
        for (u32 index = 1; index < m_Slots.size(); ++index)
        {
            Slot& slot = m_Slots[index];
            if (slot.Active || !slot.Pending.empty()) continue;
            slot.Active = true;
            slot.TextureResource = texture;
            slot.Sampler = sampler;
            return { index, slot.Generation };
        }
        return {};
    }

    bool TextureBindingTable::QueueUpdate(TextureBindingHandle handle, const Ref<Texture>& replacement, TextureSampler sampler, const CompletionToken& retireToken)
    {
        if (!IsCurrent(handle) || !IsReadOnlyOwnedTexture(replacement) || !IsValidSampler(sampler) || !CanRetireToken(retireToken)) return false;
        Slot& slot = m_Slots[handle.Index];
        if (!slot.Pending.empty()) return false;
        slot.Pending.push_back({ PendingOperation::Kind::Update, retireToken, replacement, sampler });
        return true;
    }

    bool TextureBindingTable::QueueRemoval(TextureBindingHandle handle, const CompletionToken& retireToken)
    {
        if (!IsCurrent(handle) || !CanRetireToken(retireToken)) return false;
        Slot& slot = m_Slots[handle.Index];
        if (!slot.Pending.empty()) return false;
        slot.Pending.push_back({ PendingOperation::Kind::Remove, retireToken, nullptr, TextureSampler::LinearClamp });
        return true;
    }

    bool TextureBindingTable::Retire(const CompletionToken& token)
    {
        if (!CanRetireToken(token)) return false;
        const CompletionStatus status = m_Device.QueryCompletion(token);
        if (status == CompletionStatus::Incomplete) return false;
        bool changed = false;
        for (u32 index = 1; index < m_Slots.size(); ++index)
        {
            Slot& slot = m_Slots[index];
            if (slot.Pending.empty() || slot.Pending.front().Token.DeviceId != token.DeviceId
                || slot.Pending.front().Token.SubmissionId != token.SubmissionId) continue;
            const PendingOperation operation = slot.Pending.front();
            slot.Pending.clear();
            if (operation.Operation == PendingOperation::Kind::Update)
            {
                slot.TextureResource = operation.TextureResource;
                slot.Sampler = operation.Sampler;
            }
            else
            {
                slot.Active = false;
                slot.TextureResource = nullptr;
                ++slot.Generation;
                if (slot.Generation == 0) ++slot.Generation;
            }
            changed = true;
        }
        return changed;
    }

    TextureBindingView TextureBindingTable::Resolve(TextureBindingHandle handle) const
    {
        if (IsCurrent(handle))
        {
            const Slot& slot = m_Slots[handle.Index];
            return { slot.TextureResource, slot.Sampler, false };
        }
        const Slot& error = m_Slots[0];
        return { error.TextureResource, error.Sampler, true };
    }

    u32 TextureBindingTable::GetPendingOperationCount() const
    {
        u32 count = 0;
        for (const Slot& slot : m_Slots) count += static_cast<u32>(slot.Pending.size());
        return count;
    }
}
