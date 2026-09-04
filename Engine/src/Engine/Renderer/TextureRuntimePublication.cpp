#include "Engine/Renderer/TextureRuntimePublication.h"

#include "Engine/Renderer/Renderer.h"

#include <algorithm>

namespace Engine
{
    namespace
    {
        constexpr std::array<MaterialTextureSlot, kMaterialTextureBindingCount> kMaterialSlots {{
            MaterialTextureSlot::BaseColor,
            MaterialTextureSlot::Normal,
            MaterialTextureSlot::Orm,
            MaterialTextureSlot::Emissive,
            MaterialTextureSlot::Opacity,
            MaterialTextureSlot::CallistoControl
        }};

        bool SameHandle(RHI::TextureBindingHandle left, RHI::TextureBindingHandle right)
        {
            return left.Index == right.Index && left.Generation == right.Generation;
        }

        bool SameToken(const RHI::CompletionToken& left, const RHI::CompletionToken& right)
        {
            return left.DeviceId == right.DeviceId && left.SubmissionId == right.SubmissionId;
        }

        bool MapSampler(MaterialTextureSampler source, RHI::TextureSampler& destination)
        {
            switch (source)
            {
                case MaterialTextureSampler::LinearWrap: destination = RHI::TextureSampler::LinearWrap; return true;
                case MaterialTextureSampler::LinearClamp: destination = RHI::TextureSampler::LinearClamp; return true;
                case MaterialTextureSampler::PointWrap: destination = RHI::TextureSampler::PointWrap; return true;
                case MaterialTextureSampler::PointClamp: destination = RHI::TextureSampler::PointClamp; return true;
            }
            return false;
        }

        bool HasExpectedSemantics(MaterialTextureSlot slot, const TextureArtifact& artifact)
        {
            switch (slot)
            {
                case MaterialTextureSlot::BaseColor:
                    return artifact.Role == TextureRole::BaseColor
                        && artifact.ColorSpace == TextureColorSpace::Srgb;
                case MaterialTextureSlot::Normal:
                    return artifact.Role == TextureRole::Normal
                        && artifact.ColorSpace == TextureColorSpace::Linear;
                case MaterialTextureSlot::Orm:
                    return artifact.Role == TextureRole::Orm
                        && artifact.ColorSpace == TextureColorSpace::Linear;
                case MaterialTextureSlot::Emissive:
                    return artifact.Role == TextureRole::Emissive
                        && artifact.ColorSpace == TextureColorSpace::Srgb;
                case MaterialTextureSlot::Opacity:
                case MaterialTextureSlot::CallistoControl:
                    return artifact.Role == TextureRole::Mask
                        && artifact.ColorSpace == TextureColorSpace::Linear;
            }
            return false;
        }
    }

    struct TextureRuntimePublication::Entry
    {
        enum class PendingKind : u32
        {
            None,
            Replacement,
            Removal
        };

        AssetHandle Asset = kInvalidAssetHandle;
        u64 CatalogGeneration = 0;
        RHI::TextureBindingHandle Handle;
        Ref<const TextureGpuResourceBundle> Bundle;
        RHI::TextureSampler Sampler = RHI::TextureSampler::LinearClamp;
        bool HasAcceptedUse = false;
        RHI::CompletionToken LastAcceptedUse;
        bool LastAcceptedUseTerminal = false;
        PendingKind Pending = PendingKind::None;
        RHI::CompletionToken PendingToken;
        u64 PendingCatalogGeneration = 0;
        Ref<const TextureGpuResourceBundle> PendingBundle;
    };

    Scope<TextureRuntimePublication> TextureRuntimePublication::Create(RHI::Device& device,
        TextureTargetProfile preferredTarget, size_t cacheCapacity, u32 tableCapacity)
    {
        if (cacheCapacity == 0 || tableCapacity < 2
            || tableCapacity > RHI::kMaximumReadOnlyTextureTableCapacity)
            return nullptr;

        RHI::TextureDescription description;
        description.DebugName = "Texture Runtime Error Resource";
        description.Extent = { 1, 1 };
        description.TextureFormat = RHI::Format::R8G8B8A8Unorm;
        description.Usage = static_cast<RHI::TextureUsage>(static_cast<u32>(RHI::TextureUsage::CopyDest)
            | static_cast<u32>(RHI::TextureUsage::ShaderResource));
        description.InitialState = RHI::ResourceState::CopyDest;
        Scope<RHI::Texture> ownedError = device.CreateTexture(description);
        const Ref<std::vector<u8>> pixels = CreateRef<std::vector<u8>>(
            std::initializer_list<u8> { 255, 0, 255, 255 });
        RHI::TextureUploadBatch upload;
        upload.TextureFormat = description.TextureFormat;
        upload.Subresources = { { 0, 0, { 1, 1 }, 0, 4, 4 } };
        upload.Bytes = pixels;
        RHI::ResourceState state = RHI::ResourceState::Unknown;
        if (!ownedError || !device.UploadTexture(*ownedError, upload)
            || !device.QueryResourceState(ownedError.get(), state)
            || state != RHI::ResourceState::ShaderResource)
            return nullptr;

        Ref<RHI::Texture> errorTexture(ownedError.release());
        Scope<TextureTablePublication> publication = TextureTablePublication::Create(
            device, { tableCapacity, errorTexture, RHI::TextureSampler::PointWrap });
        if (!publication)
            return nullptr;
        return Scope<TextureRuntimePublication>(new TextureRuntimePublication(
            device, preferredTarget, cacheCapacity, std::move(errorTexture), std::move(publication)));
    }

    TextureRuntimePublication::TextureRuntimePublication(RHI::Device& device,
        TextureTargetProfile preferredTarget, size_t cacheCapacity,
        Ref<RHI::Texture> errorTexture, Scope<TextureTablePublication> publication)
        : m_Device(device), m_PreferredTarget(preferredTarget), m_Cache(cacheCapacity),
          m_ErrorTexture(std::move(errorTexture)), m_Publication(std::move(publication))
    {
        m_Entries.reserve(cacheCapacity);
        m_RetainedTokens.reserve(TextureTablePublication::RetainedFrameCapacity);
    }

    TextureRuntimePublication::~TextureRuntimePublication() = default;

    TextureRuntimePublication::Entry* TextureRuntimePublication::FindEntry(
        AssetHandle asset, RHI::TextureSampler sampler)
    {
        const auto found = std::find_if(m_Entries.begin(), m_Entries.end(), [asset, sampler](const Entry& entry)
        {
            return entry.Asset == asset && entry.Sampler == sampler;
        });
        return found == m_Entries.end() ? nullptr : &*found;
    }

    TextureRuntimePublication::Entry* TextureRuntimePublication::FindEntry(
        RHI::TextureBindingHandle handle)
    {
        const auto found = std::find_if(m_Entries.begin(), m_Entries.end(), [handle](const Entry& entry)
        {
            return SameHandle(entry.Handle, handle);
        });
        return found == m_Entries.end() ? nullptr : &*found;
    }

    bool TextureRuntimePublication::IsError(RHI::TextureBindingHandle handle) const
    {
        return !m_Publication || SameHandle(handle, m_Publication->GetErrorHandle());
    }

    RHI::TextureBindingHandle TextureRuntimePublication::Resolve(AssetHandle asset,
        RHI::TextureSampler sampler, std::string& outError)
    {
        const Ref<const ArtifactResolverSnapshot> artifactResolvers =
            Renderer::GetPublishedArtifactResolverSnapshot();
        if (!artifactResolvers)
        {
            outError = "renderer has no published artifact resolver snapshot";
            return m_Publication ? m_Publication->GetErrorHandle()
                : RHI::TextureBindingHandle {};
        }
        return Resolve(*artifactResolvers, asset, sampler, outError);
    }

    RHI::TextureBindingHandle TextureRuntimePublication::Resolve(
        const ArtifactResolverSnapshot& artifactResolvers, AssetHandle asset,
        RHI::TextureSampler sampler, std::string& outError)
    {
        if (!m_Publication)
        {
            outError = "texture runtime publication has been released";
            return {};
        }
        const RHI::TextureBindingHandle errorHandle = m_Publication->GetErrorHandle();
        if (asset == kInvalidAssetHandle)
        {
            outError = "texture runtime publication requires a stable asset handle";
            return errorHandle;
        }

        Entry* entry = FindEntry(asset, sampler);
        if (entry && entry->Pending != Entry::PendingKind::None)
        {
            outError = "texture runtime publication is waiting for the asset's exact last-use token";
            return errorHandle;
        }

        const u64 currentGeneration =
            Renderer::GetArtifactResolverSnapshotGeneration(artifactResolvers);
        if (entry && entry->CatalogGeneration == currentGeneration)
        {
            outError.clear();
            return entry->Handle;
        }

        TextureArtifactVariantSet variants;
        u64 resolvedGeneration = 0;
        if (!Renderer::ResolvePublishedTextureArtifactVariantSet(
            artifactResolvers, asset, m_PreferredTarget, variants,
            resolvedGeneration, outError))
        {
            if (entry && !entry->HasAcceptedUse)
            {
                if (!m_Publication->RemoveUnaccepted(entry->Handle, outError))
                    return errorHandle;
                m_Entries.erase(std::remove_if(m_Entries.begin(), m_Entries.end(),
                    [asset, sampler](const Entry& candidate)
                    { return candidate.Asset == asset && candidate.Sampler == sampler; }),
                    m_Entries.end());
                outError = "texture asset is unavailable; its unaccepted publication was removed";
            }
            else if (entry && m_Publication->QueueRemoval(entry->Handle, outError))
            {
                entry->Pending = Entry::PendingKind::Removal;
                entry->PendingToken = entry->LastAcceptedUse;
                entry->PendingCatalogGeneration = resolvedGeneration;
                if (entry->LastAcceptedUseTerminal)
                {
                    const RHI::CompletionToken terminal = entry->PendingToken;
                    if (!Retire(terminal, outError))
                        return errorHandle;
                    outError = "texture asset is unavailable; removal retired against its terminal exact last-use token";
                }
                else
                    outError = "texture asset is unavailable; removal waits for its exact last-use token";
            }
            return errorHandle;
        }

        Ref<const TextureGpuResourceBundle> bundle;
        const TextureArtifact* fallback = variants.RgbaFallback
            ? &*variants.RgbaFallback : nullptr;
        if (!m_Cache.Acquire(m_Device, variants.Preferred, fallback, bundle, outError))
            return errorHandle;

        if (!entry)
        {
            const RHI::TextureBindingHandle handle = m_Publication->Publish(
                asset, bundle, sampler, outError);
            if (IsError(handle))
                return errorHandle;
            Entry published;
            published.Asset = asset;
            published.CatalogGeneration = resolvedGeneration;
            published.Handle = handle;
            published.Bundle = bundle;
            published.Sampler = sampler;
            m_Entries.push_back(std::move(published));
            outError.clear();
            return handle;
        }

        if (entry->Bundle == bundle)
        {
            entry->CatalogGeneration = resolvedGeneration;
            outError.clear();
            return entry->Handle;
        }
        if (!entry->HasAcceptedUse)
        {
            if (!m_Publication->ReplaceUnaccepted(entry->Handle, bundle, outError))
                return errorHandle;
            entry->CatalogGeneration = resolvedGeneration;
            entry->Bundle = std::move(bundle);
            outError.clear();
            return entry->Handle;
        }
        if (!m_Publication->QueueReplacement(entry->Handle, bundle, outError))
            return errorHandle;
        entry->Pending = Entry::PendingKind::Replacement;
        entry->PendingToken = entry->LastAcceptedUse;
        entry->PendingCatalogGeneration = resolvedGeneration;
        entry->PendingBundle = std::move(bundle);
        if (entry->LastAcceptedUseTerminal)
        {
            const RHI::CompletionToken terminal = entry->PendingToken;
            if (!Retire(terminal, outError))
                return errorHandle;
            entry = FindEntry(asset, sampler);
            if (!entry)
            {
                outError = "terminal texture replacement lost its published entry";
                return errorHandle;
            }
            outError.clear();
            return entry->Handle;
        }
        outError = "changed texture generation waits for its exact last-use token";
        return errorHandle;
    }

    bool TextureRuntimePublication::ResolveMaterialTextures(AssetHandle materialAsset,
        MaterialTextureBindingSet& outBindings, std::string& outError)
    {
        const Ref<const ArtifactResolverSnapshot> artifactResolvers =
            Renderer::GetPublishedArtifactResolverSnapshot();
        if (!artifactResolvers)
        {
            outError = "renderer has no published artifact resolver snapshot";
            return false;
        }
        return ResolveMaterialTextures(*artifactResolvers, materialAsset,
            outBindings, outError);
    }

    bool TextureRuntimePublication::ResolveMaterialTextures(
        const ArtifactResolverSnapshot& artifactResolvers,
        AssetHandle materialAsset, MaterialTextureBindingSet& outBindings,
        std::string& outError)
    {
        if (!m_Publication)
        {
            outError = "material texture resolution requires a live texture publication";
            return false;
        }

        MaterialTextureBindingSet candidate;
        candidate.Handles.fill(m_Publication->GetErrorHandle());
        if (!Renderer::ResolvePublishedMaterialAsset(artifactResolvers,
            materialAsset, candidate.Material, candidate.CatalogGeneration,
            outError))
            return false;

        std::string diagnostics;
        for (size_t index = 0; index < kMaterialSlots.size(); ++index)
        {
            const MaterialTextureSlot slot = kMaterialSlots[index];
            const AssetHandle textureAsset = candidate.Material.GetTexture(slot);
            if (textureAsset == kInvalidAssetHandle)
                continue;

            candidate.DeclaredMask |= 1u << static_cast<u32>(index);
            TextureArtifactVariantSet variants;
            u64 textureGeneration = 0;
            std::string slotError;
            RHI::TextureSampler sampler = RHI::TextureSampler::LinearWrap;
            const bool resolved = MapSampler(candidate.Material.GetSampler(slot), sampler)
                && Renderer::ResolvePublishedTextureArtifactVariantSet(
                    artifactResolvers, textureAsset, m_PreferredTarget, variants,
                    textureGeneration, slotError)
                && textureGeneration == candidate.CatalogGeneration
                && HasExpectedSemantics(slot, variants.Preferred);
            if (!resolved)
            {
                candidate.ErrorMask |= 1u << static_cast<u32>(index);
                if (!diagnostics.empty()) diagnostics += "; ";
                diagnostics += std::string(ToString(slot)) + "="
                    + (slotError.empty() ? "sampler, catalog generation, or texture semantics mismatch" : slotError);
                continue;
            }

            candidate.Handles[index] = Resolve(
                artifactResolvers, textureAsset, sampler, slotError);
            if (IsError(candidate.Handles[index]))
            {
                candidate.ErrorMask |= 1u << static_cast<u32>(index);
                if (!diagnostics.empty()) diagnostics += "; ";
                diagnostics += std::string(ToString(slot)) + "="
                    + (slotError.empty() ? "error texture" : slotError);
            }
        }

        outBindings = std::move(candidate);
        outError = std::move(diagnostics);
        return true;
    }

    bool TextureRuntimePublication::RetainAcceptedFrame(
        const RHI::CompletionToken& token,
        const std::vector<RHI::TextureBindingHandle>& handles,
        std::string& outError)
    {
        if (!m_Publication)
        {
            outError = "texture runtime publication has been released";
            return false;
        }

        std::vector<RHI::TextureBindingHandle> unique;
        unique.reserve(handles.size());
        for (const RHI::TextureBindingHandle handle : handles)
        {
            if (IsError(handle))
                continue;
            Entry* entry = FindEntry(handle);
            if (!entry || entry->Pending != Entry::PendingKind::None)
            {
                outError = !entry
                    ? "accepted texture frame contains an unknown or stale runtime handle"
                    : "accepted texture frame contains a handle pending retirement";
                return false;
            }
            if (std::none_of(unique.begin(), unique.end(), [handle](RHI::TextureBindingHandle prior)
                { return SameHandle(prior, handle); }))
                unique.push_back(handle);
        }
        if (unique.empty())
        {
            outError.clear();
            return true;
        }
        if (!m_Publication->RetainAcceptedFrame(token, unique, outError))
            return false;
        for (const RHI::TextureBindingHandle handle : unique)
        {
            Entry* entry = FindEntry(handle);
            entry->HasAcceptedUse = true;
            entry->LastAcceptedUse = token;
            entry->LastAcceptedUseTerminal = false;
        }
        m_RetainedTokens.push_back(token);
        outError.clear();
        return true;
    }

    bool TextureRuntimePublication::Retire(
        const RHI::CompletionToken& token, std::string& outError)
    {
        const auto retained = std::find_if(m_RetainedTokens.begin(), m_RetainedTokens.end(),
            [&token](const RHI::CompletionToken& candidate) { return SameToken(candidate, token); });
        const bool ownsOperation = std::any_of(m_Entries.begin(), m_Entries.end(),
            [&token](const Entry& entry)
            {
                return entry.Pending != Entry::PendingKind::None
                    && SameToken(entry.PendingToken, token);
            });
        if (!m_Publication
            || (retained == m_RetainedTokens.end() && !ownsOperation))
        {
            outError = !m_Publication ? "texture runtime publication has been released"
                : "texture runtime retirement token is not retained";
            return false;
        }
        if (!m_Publication->Retire(token, outError))
            return false;

        if (retained != m_RetainedTokens.end())
            m_RetainedTokens.erase(retained);
        for (Entry& entry : m_Entries)
            if (entry.HasAcceptedUse && SameToken(entry.LastAcceptedUse, token))
                entry.LastAcceptedUseTerminal = true;
        for (auto entry = m_Entries.begin(); entry != m_Entries.end();)
        {
            if (entry->Pending == Entry::PendingKind::None
                || !SameToken(entry->PendingToken, token))
            {
                ++entry;
                continue;
            }
            if (entry->Pending == Entry::PendingKind::Removal)
            {
                entry = m_Entries.erase(entry);
                continue;
            }
            entry->CatalogGeneration = entry->PendingCatalogGeneration;
            entry->Bundle = std::move(entry->PendingBundle);
            entry->HasAcceptedUse = false;
            entry->LastAcceptedUse = {};
            entry->LastAcceptedUseTerminal = false;
            entry->Pending = Entry::PendingKind::None;
            entry->PendingToken = {};
            entry->PendingCatalogGeneration = 0;
            ++entry;
        }
        outError.clear();
        return true;
    }

    bool TextureRuntimePublication::HasRetainedFrame(const RHI::CompletionToken& token) const
    {
        return std::any_of(m_RetainedTokens.begin(), m_RetainedTokens.end(),
            [&token](const RHI::CompletionToken& retained) { return SameToken(retained, token); });
    }

    RHI::TextureBindingHandle TextureRuntimePublication::GetErrorHandle() const
    {
        return m_Publication ? m_Publication->GetErrorHandle() : RHI::TextureBindingHandle {};
    }

    RHI::TextureBindingTable* TextureRuntimePublication::GetBindingTable() const
    {
        return m_Publication ? m_Publication->GetBindingTable() : nullptr;
    }

    size_t TextureRuntimePublication::GetPublishedViewCount() const
    {
        return m_Publication ? m_Publication->GetViewCount() : 0;
    }

    size_t TextureRuntimePublication::GetCachedResourceCount() const
    {
        return m_Cache.GetEntryCount();
    }

    size_t TextureRuntimePublication::GetRetainedFrameCount() const
    {
        return m_Publication ? m_Publication->GetRetainedFrameCount() : 0;
    }

    size_t TextureRuntimePublication::GetPendingOperationCount() const
    {
        return m_Publication ? m_Publication->GetPendingOperationCount() : 0;
    }

    void TextureRuntimePublication::ReleaseAfterDeviceIdle()
    {
        if (m_Publication)
            m_Publication->ReleaseAfterDeviceIdle();
        m_Cache.Clear();
        m_Entries.clear();
        m_RetainedTokens.clear();
        m_ErrorTexture.reset();
    }
}
