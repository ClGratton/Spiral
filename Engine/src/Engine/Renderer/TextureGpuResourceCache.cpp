#include "Engine/Renderer/TextureGpuResourceCache.h"

#include <algorithm>
#include <filesystem>

namespace Engine
{
    namespace
    {
        struct ArtifactIdentity
        {
            TextureArtifactUploadPlan Plan;
        };

        ArtifactIdentity MakeIdentity(const TextureArtifactUploadPlan& plan)
        {
            ArtifactIdentity identity { plan };
            identity.Plan.SourcePath = std::filesystem::path(plan.SourcePath).lexically_normal().generic_string();
            return identity;
        }

        bool SameDescription(const RHI::TextureDescription& left, const RHI::TextureDescription& right)
        {
            return left.DebugName == right.DebugName
                && left.Extent.Width == right.Extent.Width && left.Extent.Height == right.Extent.Height
                && left.TextureFormat == right.TextureFormat && left.Usage == right.Usage
                && left.InitialState == right.InitialState && left.MipLevels == right.MipLevels
                && left.ArrayLayers == right.ArrayLayers && left.SampleCount == right.SampleCount;
        }

        bool SameSubresources(const std::vector<TextureArtifactUploadSubresource>& left,
            const std::vector<TextureArtifactUploadSubresource>& right)
        {
            if (left.size() != right.size())
                return false;
            for (size_t index = 0; index < left.size(); ++index)
            {
                const TextureArtifactUploadSubresource& lhs = left[index];
                const TextureArtifactUploadSubresource& rhs = right[index];
                if (lhs.MipLevel != rhs.MipLevel
                    || lhs.Extent.Width != rhs.Extent.Width || lhs.Extent.Height != rhs.Extent.Height
                    || lhs.ByteOffset != rhs.ByteOffset || lhs.RowPitchBytes != rhs.RowPitchBytes
                    || lhs.ByteSize != rhs.ByteSize)
                    return false;
            }
            return true;
        }

        bool SameIdentity(const ArtifactIdentity& left, const ArtifactIdentity& right)
        {
            const TextureArtifactUploadPlan& lhs = left.Plan;
            const TextureArtifactUploadPlan& rhs = right.Plan;
            return lhs.Asset == rhs.Asset && lhs.SourcePath == rhs.SourcePath
                && lhs.Role == rhs.Role && lhs.ColorSpace == rhs.ColorSpace
                && lhs.TargetProfile == rhs.TargetProfile && lhs.HasAlpha == rhs.HasAlpha
                && SameDescription(lhs.Texture, rhs.Texture)
                && SameSubresources(lhs.Subresources, rhs.Subresources)
                && lhs.Payload && rhs.Payload && *lhs.Payload == *rhs.Payload;
        }
    }

    struct TextureGpuResourceCache::Entry
    {
        const RHI::Device* Device = nullptr;
        ArtifactIdentity Identity;
        Ref<const TextureGpuResourceBundle> Bundle;
        u64 LastAccess = 0;
    };

    TextureGpuResourceCache::TextureGpuResourceCache(size_t capacity)
        : m_Capacity(capacity)
    {
    }

    TextureGpuResourceCache::~TextureGpuResourceCache() = default;

    bool TextureGpuResourceCache::Acquire(RHI::Device& device, const TextureArtifact& preferred,
        const TextureArtifact* rgbaFallback,
        Ref<const TextureGpuResourceBundle>& outBundle, std::string& outError)
    {
        if (m_Capacity == 0)
        {
            outError = "texture GPU resource cache has zero capacity";
            return false;
        }

        TextureArtifactUploadPlan preferredPlan;
        if (!BuildTextureArtifactUploadPlan(preferred, preferredPlan, outError))
            return false;

        TextureArtifactUploadSelection selection;
        if (!SelectTextureArtifactUploadPlan(device, preferredPlan, nullptr, selection, outError))
        {
            if (!rgbaFallback)
                return false;
            TextureArtifactUploadPlan fallbackPlan;
            if (!BuildTextureArtifactUploadPlan(*rgbaFallback, fallbackPlan, outError)
                || !SelectTextureArtifactUploadPlan(device, preferredPlan, &fallbackPlan, selection, outError))
                return false;
        }

        ArtifactIdentity identity = MakeIdentity(selection.Plan);
        for (Entry& entry : m_Entries)
        {
            if (entry.Device == &device && SameIdentity(entry.Identity, identity))
            {
                entry.LastAccess = ++m_NextAccess;
                outBundle = entry.Bundle;
                outError.clear();
                return true;
            }
        }

        RHI::TextureUploadBatch upload;
        if (!BuildTextureUploadBatch(selection.Plan, upload, outError))
            return false;

        Scope<RHI::Texture> texture = device.CreateTexture(selection.Plan.Texture);
        if (!texture || !device.OwnsResource(texture.get()))
        {
            outError = "texture GPU resource cache could not create an exact-device texture";
            return false;
        }
        if (!device.UploadTexture(*texture, upload))
        {
            outError = "texture GPU resource cache could not upload the selected immutable mip chain";
            return false;
        }

        RHI::ResourceState state = RHI::ResourceState::Unknown;
        if (!device.QueryResourceState(texture.get(), state) || state != RHI::ResourceState::ShaderResource)
        {
            outError = "texture GPU resource cache upload did not publish ShaderResource";
            return false;
        }

        Ref<TextureGpuResourceBundle> bundle = CreateRef<TextureGpuResourceBundle>();
        bundle->Texture = Ref<RHI::Texture>(texture.release());
        bundle->UploadPlan = selection.Plan;
        bundle->UsedExplicitRgbaFallback = selection.UsedExplicitRgbaFallback;
        bundle->Diagnostic = selection.Diagnostic;
        bundle->Generation = ++m_NextGeneration;

        if (m_Entries.size() == m_Capacity)
        {
            const auto eviction = std::min_element(m_Entries.begin(), m_Entries.end(), [](const Entry& left, const Entry& right)
            {
                return left.LastAccess != right.LastAccess ? left.LastAccess < right.LastAccess
                    : left.Bundle->Generation < right.Bundle->Generation;
            });
            m_Entries.erase(eviction);
        }

        m_Entries.push_back({ &device, std::move(identity), bundle, ++m_NextAccess });
        outBundle = std::move(bundle);
        outError.clear();
        return true;
    }

    void TextureGpuResourceCache::Clear()
    {
        m_Entries.clear();
    }

    size_t TextureGpuResourceCache::GetEntryCount() const
    {
        return m_Entries.size();
    }
}
