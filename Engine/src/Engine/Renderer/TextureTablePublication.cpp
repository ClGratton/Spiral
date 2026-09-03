#include "Engine/Renderer/TextureTablePublication.h"

#include <algorithm>

namespace Engine
{
    namespace
    {
        bool SameToken(const RHI::CompletionToken& left, const RHI::CompletionToken& right)
        {
            return left.DeviceId == right.DeviceId && left.SubmissionId == right.SubmissionId;
        }

        bool SameDescription(const RHI::TextureDescription& left, const RHI::TextureDescription& right)
        {
            return left.DebugName == right.DebugName
                && left.Extent.Width == right.Extent.Width && left.Extent.Height == right.Extent.Height
                && left.TextureFormat == right.TextureFormat && left.Usage == right.Usage
                && left.InitialState == right.InitialState && left.MipLevels == right.MipLevels
                && left.ArrayLayers == right.ArrayLayers && left.SampleCount == right.SampleCount;
        }
    }

    struct TextureTablePublication::PendingOperation
    {
        enum class Kind { Replacement, Removal } Operation = Kind::Replacement;
        RHI::CompletionToken LastUse;
        Ref<const TextureGpuResourceBundle> OldBundle;
        Ref<const TextureGpuResourceBundle> NewBundle;
    };

    struct TextureTablePublication::Entry
    {
        AssetHandle Asset = kInvalidAssetHandle;
        RHI::TextureBindingHandle Handle;
        Ref<const TextureGpuResourceBundle> Bundle;
        RHI::TextureSampler Sampler = RHI::TextureSampler::LinearClamp;
        RHI::CompletionToken LastAcceptedUse;
        std::optional<PendingOperation> Pending;
    };

    struct TextureTablePublication::RetainedFrame
    {
        RHI::CompletionToken Token;
        std::vector<Ref<const TextureGpuResourceBundle>> Bundles;
    };

    Scope<TextureTablePublication> TextureTablePublication::Create(RHI::Device& device,
        const RHI::TextureBindingTableDescription& description)
    {
        Scope<RHI::TextureBindingTable> table = RHI::TextureBindingTable::Create(device, description);
        return table ? Scope<TextureTablePublication>(new TextureTablePublication(device, std::move(table))) : nullptr;
    }

    TextureTablePublication::TextureTablePublication(RHI::Device& device,
        Scope<RHI::TextureBindingTable> table)
        : m_Device(device), m_Table(std::move(table)), m_ErrorHandle(m_Table->GetErrorHandle())
    {
        m_Entries.reserve(m_Table->GetCapacity() - 1);
        m_Frames.reserve(RetainedFrameCapacity);
    }

    TextureTablePublication::~TextureTablePublication() = default;

    bool TextureTablePublication::IsPublishable(AssetHandle asset,
        const Ref<const TextureGpuResourceBundle>& bundle, std::string& outError) const
    {
        if (!m_Table)
        {
            outError = "texture table publication has been released";
            return false;
        }
        if (asset == kInvalidAssetHandle || !bundle || !bundle->Texture || !bundle->UploadPlan.Payload
            || bundle->Generation == 0 || bundle->UploadPlan.Asset != asset)
        {
            outError = "texture table publication requires one matching completed T4B bundle";
            return false;
        }
        RHI::TextureUploadBatch upload;
        if (!BuildTextureUploadBatch(bundle->UploadPlan, upload, outError))
        {
            outError = "texture table publication bundle has an invalid immutable upload plan: " + outError;
            return false;
        }
        if (!m_Device.OwnsResource(bundle->Texture.get())
            || !SameDescription(bundle->Texture->GetDescription(), bundle->UploadPlan.Texture))
        {
            outError = "texture table publication bundle does not belong to the exact device and upload plan";
            return false;
        }
        RHI::ResourceState state = RHI::ResourceState::Unknown;
        if (!m_Device.QueryResourceState(bundle->Texture.get(), state)
            || state != RHI::ResourceState::ShaderResource)
        {
            outError = "texture table publication bundle has not reached ShaderResource";
            return false;
        }
        return true;
    }

    TextureTablePublication::Entry* TextureTablePublication::FindEntry(
        AssetHandle asset, RHI::TextureSampler sampler)
    {
        const auto found = std::find_if(m_Entries.begin(), m_Entries.end(), [asset, sampler](const Entry& entry)
        {
            return entry.Asset == asset && entry.Sampler == sampler;
        });
        return found == m_Entries.end() ? nullptr : &*found;
    }

    const TextureTablePublication::Entry* TextureTablePublication::FindEntry(
        AssetHandle asset, RHI::TextureSampler sampler) const
    {
        const auto found = std::find_if(m_Entries.begin(), m_Entries.end(), [asset, sampler](const Entry& entry)
        {
            return entry.Asset == asset && entry.Sampler == sampler;
        });
        return found == m_Entries.end() ? nullptr : &*found;
    }

    TextureTablePublication::Entry* TextureTablePublication::FindEntry(RHI::TextureBindingHandle handle)
    {
        const auto found = std::find_if(m_Entries.begin(), m_Entries.end(), [handle](const Entry& entry)
        {
            return entry.Handle.Index == handle.Index && entry.Handle.Generation == handle.Generation;
        });
        return found == m_Entries.end() ? nullptr : &*found;
    }

    RHI::TextureBindingHandle TextureTablePublication::Resolve(
        AssetHandle asset, RHI::TextureSampler sampler) const
    {
        const Entry* entry = FindEntry(asset, sampler);
        return entry && !entry->Pending ? entry->Handle : m_ErrorHandle;
    }

    RHI::TextureBindingHandle TextureTablePublication::Publish(AssetHandle asset,
        const Ref<const TextureGpuResourceBundle>& bundle,
        RHI::TextureSampler sampler, std::string& outError)
    {
        if (!IsPublishable(asset, bundle, outError))
            return m_ErrorHandle;
        if (Entry* existing = FindEntry(asset, sampler))
        {
            if (!existing->Pending && existing->Bundle == bundle)
            {
                outError.clear();
                return existing->Handle;
            }
            outError = "texture asset/sampler view already has a different or pending table publication";
            return m_ErrorHandle;
        }

        const RHI::TextureBindingHandle handle = m_Table->Allocate(bundle->Texture, sampler);
        if (!handle.IsValid())
        {
            outError = "texture table publication could not allocate a sampled-table slot";
            return m_ErrorHandle;
        }
        m_Entries.push_back({ asset, handle, bundle, sampler, {}, {} });
        outError.clear();
        return handle;
    }

    bool TextureTablePublication::ReplaceUnaccepted(RHI::TextureBindingHandle handle,
        const Ref<const TextureGpuResourceBundle>& replacement, std::string& outError)
    {
        Entry* entry = FindEntry(handle);
        if (!entry || entry->Pending || entry->LastAcceptedUse.IsValid())
        {
            outError = !entry ? "unaccepted texture replacement targets an unpublished view"
                : entry->Pending ? "texture view already has a pending table operation"
                : "texture replacement requires exact GPU retirement after accepted use";
            return false;
        }
        if (!IsPublishable(entry->Asset, replacement, outError))
            return false;
        if (!m_Table->ReplaceUnsubmitted(entry->Handle, replacement->Texture, entry->Sampler))
        {
            outError = "texture table rejected an unaccepted replacement";
            return false;
        }
        entry->Bundle = replacement;
        outError.clear();
        return true;
    }

    bool TextureTablePublication::RemoveUnaccepted(
        RHI::TextureBindingHandle handle, std::string& outError)
    {
        const auto found = std::find_if(m_Entries.begin(), m_Entries.end(), [handle](const Entry& entry)
        {
            return entry.Handle.Index == handle.Index && entry.Handle.Generation == handle.Generation;
        });
        if (found == m_Entries.end() || found->Pending || found->LastAcceptedUse.IsValid())
        {
            outError = found == m_Entries.end() ? "unaccepted texture removal targets an unpublished view"
                : found->Pending ? "texture view already has a pending table operation"
                : "texture removal requires exact GPU retirement after accepted use";
            return false;
        }
        if (!m_Table->RemoveUnsubmitted(found->Handle))
        {
            outError = "texture table rejected an unaccepted removal";
            return false;
        }
        m_Entries.erase(found);
        outError.clear();
        return true;
    }

    bool TextureTablePublication::RetainAcceptedFrame(const RHI::CompletionToken& token,
        const std::vector<RHI::TextureBindingHandle>& handles, std::string& outError)
    {
        if (!m_Table || handles.empty() || m_Frames.size() == RetainedFrameCapacity
            || m_Device.QueryCompletion(token) == RHI::CompletionStatus::Invalid)
        {
            outError = !m_Table ? "texture table publication has been released"
                : handles.empty() ? "accepted texture frame has no bound handles"
                : m_Frames.size() == RetainedFrameCapacity ? "accepted texture frame retention is at bounded capacity"
                : "accepted texture frame token does not belong to the exact device";
            return false;
        }
        if (std::any_of(m_Frames.begin(), m_Frames.end(), [&token](const RetainedFrame& frame)
            { return SameToken(frame.Token, token); }))
        {
            outError = "accepted texture frame token is already retained";
            return false;
        }

        std::vector<Entry*> entries;
        entries.reserve(handles.size());
        for (const RHI::TextureBindingHandle handle : handles)
        {
            Entry* entry = FindEntry(handle);
            if (!entry || entry->Pending
                || std::any_of(entries.begin(), entries.end(), [entry](const Entry* prior) { return prior == entry; }))
            {
                outError = !entry ? "accepted texture frame contains an unknown or stale handle"
                    : entry->Pending ? "accepted texture frame cannot extend a pending view's last use"
                    : "accepted texture frame repeats a bound handle";
                return false;
            }
            entries.push_back(entry);
        }

        RetainedFrame frame;
        frame.Token = token;
        frame.Bundles.reserve(entries.size());
        for (Entry* entry : entries) frame.Bundles.push_back(entry->Bundle);
        m_Frames.push_back(std::move(frame));
        for (Entry* entry : entries) entry->LastAcceptedUse = token;
        outError.clear();
        return true;
    }

    bool TextureTablePublication::QueueReplacement(RHI::TextureBindingHandle handle,
        const Ref<const TextureGpuResourceBundle>& replacement, std::string& outError)
    {
        Entry* entry = FindEntry(handle);
        if (!entry || entry->Pending || !entry->LastAcceptedUse.IsValid())
        {
            outError = !entry ? "texture replacement targets an unpublished view"
                : entry->Pending ? "texture view already has a pending table operation"
                : "texture replacement has no accepted last-use token";
            return false;
        }
        if (!IsPublishable(entry->Asset, replacement, outError))
            return false;
        if (entry->Bundle == replacement)
        {
            outError = "texture replacement does not change the published bundle";
            return false;
        }
        if (!m_Table->QueueUpdate(entry->Handle, replacement->Texture,
            entry->Sampler, entry->LastAcceptedUse))
        {
            outError = "texture table rejected the exact last-use replacement operation";
            return false;
        }
        entry->Pending = PendingOperation { PendingOperation::Kind::Replacement,
            entry->LastAcceptedUse, entry->Bundle, replacement };
        outError.clear();
        return true;
    }

    bool TextureTablePublication::QueueRemoval(
        RHI::TextureBindingHandle handle, std::string& outError)
    {
        Entry* entry = FindEntry(handle);
        if (!entry || entry->Pending || !entry->LastAcceptedUse.IsValid())
        {
            outError = !entry ? "texture removal targets an unpublished view"
                : entry->Pending ? "texture view already has a pending table operation"
                : "texture removal has no accepted last-use token";
            return false;
        }
        if (!m_Table->QueueRemoval(entry->Handle, entry->LastAcceptedUse))
        {
            outError = "texture table rejected the exact last-use removal operation";
            return false;
        }
        entry->Pending = PendingOperation { PendingOperation::Kind::Removal,
            entry->LastAcceptedUse, entry->Bundle, {} };
        outError.clear();
        return true;
    }

    bool TextureTablePublication::Retire(const RHI::CompletionToken& token, std::string& outError)
    {
        if (!m_Table)
        {
            outError = "texture table publication has been released";
            return false;
        }
        const RHI::CompletionStatus status = m_Device.QueryCompletion(token);
        if (status != RHI::CompletionStatus::Complete && status != RHI::CompletionStatus::Failed)
        {
            outError = status == RHI::CompletionStatus::Incomplete
                ? "texture table retirement token is incomplete"
                : "texture table retirement token does not belong to the exact device";
            return false;
        }

        const bool hasFrame = std::any_of(m_Frames.begin(), m_Frames.end(), [&token](const RetainedFrame& frame)
        {
            return SameToken(frame.Token, token);
        });
        const bool hasOperation = std::any_of(m_Entries.begin(), m_Entries.end(), [&token](const Entry& entry)
        {
            return entry.Pending && SameToken(entry.Pending->LastUse, token);
        });
        if (!hasFrame && !hasOperation)
        {
            outError = "texture table retirement token has no retained frame or operation";
            return false;
        }
        if (hasOperation && !m_Table->Retire(token))
        {
            outError = "texture table did not retire its matching terminal operation";
            return false;
        }

        for (auto entry = m_Entries.begin(); entry != m_Entries.end();)
        {
            if (!entry->Pending || !SameToken(entry->Pending->LastUse, token))
            {
                ++entry;
                continue;
            }
            if (entry->Pending->Operation == PendingOperation::Kind::Replacement)
            {
                entry->Bundle = std::move(entry->Pending->NewBundle);
                entry->LastAcceptedUse = {};
                entry->Pending.reset();
                ++entry;
            }
            else
            {
                entry = m_Entries.erase(entry);
            }
        }
        m_Frames.erase(std::remove_if(m_Frames.begin(), m_Frames.end(), [&token](const RetainedFrame& frame)
        {
            return SameToken(frame.Token, token);
        }), m_Frames.end());
        outError.clear();
        return true;
    }

    void TextureTablePublication::ReleaseAfterDeviceIdle()
    {
        m_Frames.clear();
        m_Entries.clear();
        m_Table.reset();
    }

    size_t TextureTablePublication::GetViewCount() const
    {
        return m_Entries.size();
    }

    size_t TextureTablePublication::GetRetainedFrameCount() const
    {
        return m_Frames.size();
    }

    size_t TextureTablePublication::GetPendingOperationCount() const
    {
        return static_cast<size_t>(std::count_if(m_Entries.begin(), m_Entries.end(), [](const Entry& entry)
        {
            return entry.Pending.has_value();
        }));
    }
}
