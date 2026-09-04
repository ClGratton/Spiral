#pragma once

#include "Engine/Assets/TextureArtifact.h"
#include "Engine/Assets/MaterialAsset.h"
#include "Engine/Core/Base.h"
#include "Engine/RHI/TextureBindingTable.h"
#include "Engine/Renderer/TextureGpuResourceCache.h"
#include "Engine/Renderer/TextureTablePublication.h"

#include <array>
#include <vector>

namespace Engine
{
    class ArtifactResolverSnapshot;

    constexpr size_t kMaterialTextureBindingCount = 6;

    struct MaterialTextureBindingSet
    {
        MaterialAsset Material;
        std::array<RHI::TextureBindingHandle, kMaterialTextureBindingCount> Handles {};
        u32 DeclaredMask = 0;
        u32 ErrorMask = 0;
        u64 CatalogGeneration = 0;
    };

    // Composes the immutable material/artifact catalog, exact-device upload
    // cache, sampled table, and exact GPU-retirement lifetime boundary.
    class TextureRuntimePublication final
    {
    public:
        static Scope<TextureRuntimePublication> Create(RHI::Device& device,
            TextureTargetProfile preferredTarget, size_t cacheCapacity, u32 tableCapacity);
        ~TextureRuntimePublication();

        // A missing, invalid, changed-but-unretired, or upload-failed asset
        // resolves to the declared error resource and never another asset.
        RHI::TextureBindingHandle Resolve(AssetHandle asset,
            RHI::TextureSampler sampler, std::string& outError);
        // Resolves one immutable material generation into table handles. Empty
        // slots remain on shader semantic defaults; declared invalid slots use
        // the error handle without sampling semantically mismatched content.
        bool ResolveMaterialTextures(AssetHandle materialAsset,
            MaterialTextureBindingSet& outBindings, std::string& outError);
        bool ResolveMaterialTextures(
            const ArtifactResolverSnapshot& artifactResolvers,
            AssetHandle materialAsset, MaterialTextureBindingSet& outBindings,
            std::string& outError);

        // The caller aggregates all handles read by one accepted submission.
        // Error handles and duplicates are ignored because the table owns its
        // immutable error resource for the complete device lifetime.
        bool RetainAcceptedFrame(const RHI::CompletionToken& token,
            const std::vector<RHI::TextureBindingHandle>& handles,
            std::string& outError);
        bool Retire(const RHI::CompletionToken& token, std::string& outError);
        bool HasRetainedFrame(const RHI::CompletionToken& token) const;

        RHI::TextureBindingHandle GetErrorHandle() const;
        RHI::TextureBindingTable* GetBindingTable() const;
        size_t GetPublishedViewCount() const;
        size_t GetCachedResourceCount() const;
        size_t GetRetainedFrameCount() const;
        size_t GetPendingOperationCount() const;

        // The caller establishes device idle and calls this before destroying
        // the exact device used to create the coordinator.
        void ReleaseAfterDeviceIdle();

    private:
        struct Entry;

        TextureRuntimePublication(RHI::Device& device,
            TextureTargetProfile preferredTarget, size_t cacheCapacity,
            Ref<RHI::Texture> errorTexture,
            Scope<TextureTablePublication> publication);
        Entry* FindEntry(AssetHandle asset, RHI::TextureSampler sampler);
        Entry* FindEntry(RHI::TextureBindingHandle handle);
        bool IsError(RHI::TextureBindingHandle handle) const;
        RHI::TextureBindingHandle Resolve(
            const ArtifactResolverSnapshot& artifactResolvers,
            AssetHandle asset, RHI::TextureSampler sampler,
            std::string& outError);

        RHI::Device& m_Device;
        TextureTargetProfile m_PreferredTarget = TextureTargetProfile::RGBAFallback;
        TextureGpuResourceCache m_Cache;
        Ref<RHI::Texture> m_ErrorTexture;
        Scope<TextureTablePublication> m_Publication;
        std::vector<Entry> m_Entries;
        std::vector<RHI::CompletionToken> m_RetainedTokens;
    };
}
