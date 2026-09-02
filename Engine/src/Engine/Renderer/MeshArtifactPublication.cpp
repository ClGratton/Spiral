#include "Engine/Renderer/Renderer.h"

#include "Engine/Assets/AssetRegistry.h"
#include "Engine/Assets/MeshArtifact.h"
#include "Engine/Assets/TextureArtifact.h"

#include <atomic>
#include <memory>

namespace Engine
{
    namespace
    {
        struct ArtifactResolverCatalog
        {
            explicit ArtifactResolverCatalog(const AssetRegistry& registry)
                : Registry(std::make_shared<const AssetRegistry>(registry)), Mesh(Registry), Texture(Registry)
            {
            }

            std::shared_ptr<const AssetRegistry> Registry;
            MeshArtifactResolver Mesh;
            TextureArtifactResolver Texture;
        };

        struct ArtifactResolverState
        {
            u64 Generation = 0;
            std::shared_ptr<const ArtifactResolverCatalog> Catalog;
        };

#if defined(__cpp_lib_atomic_shared_ptr) && __cpp_lib_atomic_shared_ptr >= 201711L
        std::atomic<std::shared_ptr<const ArtifactResolverState>> s_ArtifactResolverState;
#else
        std::shared_ptr<const ArtifactResolverState> s_ArtifactResolverState;
#endif
        std::atomic<u64> s_NextArtifactResolverGeneration { 0 };

        void StoreResolvers(std::shared_ptr<const ArtifactResolverCatalog> resolvers)
        {
            auto state = std::make_shared<ArtifactResolverState>();
            state->Generation = s_NextArtifactResolverGeneration.fetch_add(1, std::memory_order_relaxed) + 1;
            state->Catalog = std::move(resolvers);
#if defined(__cpp_lib_atomic_shared_ptr) && __cpp_lib_atomic_shared_ptr >= 201711L
            s_ArtifactResolverState.store(std::move(state), std::memory_order_release);
#else
            std::atomic_store_explicit(&s_ArtifactResolverState,
                std::shared_ptr<const ArtifactResolverState>(std::move(state)), std::memory_order_release);
#endif
        }

        std::shared_ptr<const ArtifactResolverState> LoadResolverState()
        {
#if defined(__cpp_lib_atomic_shared_ptr) && __cpp_lib_atomic_shared_ptr >= 201711L
            return s_ArtifactResolverState.load(std::memory_order_acquire);
#else
            return std::atomic_load_explicit(&s_ArtifactResolverState, std::memory_order_acquire);
#endif
        }
    }

    void Renderer::PublishArtifactResolvers(const AssetRegistry& registry)
    {
        std::shared_ptr<const ArtifactResolverCatalog> published = std::make_shared<const ArtifactResolverCatalog>(registry);
        StoreResolvers(std::move(published));
    }

    void Renderer::PublishMeshArtifactResolver(const AssetRegistry& registry)
    {
        PublishArtifactResolvers(registry);
    }

    bool Renderer::ResolvePublishedMeshArtifact(AssetHandle asset, MeshArtifact& outArtifact, std::string& outError)
    {
        const std::shared_ptr<const ArtifactResolverState> state = LoadResolverState();
        if (!state || !state->Catalog)
        {
            outError = "renderer has no published mesh artifact resolver";
            return false;
        }

        return state->Catalog->Mesh.Resolve(asset, outArtifact, outError);
    }

    bool Renderer::ResolvePublishedTextureArtifact(AssetHandle asset, TextureArtifact& outArtifact, std::string& outError)
    {
        const std::shared_ptr<const ArtifactResolverState> state = LoadResolverState();
        if (!state || !state->Catalog)
        {
            outError = "renderer has no published texture artifact resolver";
            return false;
        }

        return state->Catalog->Texture.Resolve(asset, outArtifact, outError);
    }

    bool Renderer::ResolvePublishedTextureArtifactVariantSet(AssetHandle asset,
        TextureTargetProfile preferredTarget, TextureArtifactVariantSet& outVariants,
        std::string& outError)
    {
        u64 ignoredGeneration = 0;
        return ResolvePublishedTextureArtifactVariantSet(
            asset, preferredTarget, outVariants, ignoredGeneration, outError);
    }

    bool Renderer::ResolvePublishedTextureArtifactVariantSet(AssetHandle asset,
        TextureTargetProfile preferredTarget, TextureArtifactVariantSet& outVariants,
        u64& outCatalogGeneration, std::string& outError)
    {
        const std::shared_ptr<const ArtifactResolverState> state = LoadResolverState();
        outCatalogGeneration = state ? state->Generation : 0;
        if (!state || !state->Catalog)
        {
            outError = "renderer has no published texture artifact resolver";
            return false;
        }

        return state->Catalog->Texture.ResolveVariantSet(asset, preferredTarget, outVariants, outError);
    }

    u64 Renderer::GetPublishedArtifactResolverGeneration()
    {
        const std::shared_ptr<const ArtifactResolverState> state = LoadResolverState();
        return state ? state->Generation : 0;
    }

    void Renderer::ClearArtifactResolvers()
    {
        StoreResolvers({});
    }
}
