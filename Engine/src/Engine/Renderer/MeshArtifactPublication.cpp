#include "Engine/Renderer/Renderer.h"

#include "Engine/Assets/AssetRegistry.h"
#include "Engine/Assets/MaterialAsset.h"
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
            ArtifactResolverCatalog(const AssetRegistry& registry, const MaterialLibrary& materials)
                : Registry(std::make_shared<const AssetRegistry>(registry)),
                  Materials(std::make_shared<const MaterialLibrary>(materials)),
                  Mesh(Registry), Texture(Registry)
            {
            }

            std::shared_ptr<const AssetRegistry> Registry;
            std::shared_ptr<const MaterialLibrary> Materials;
            MeshArtifactResolver Mesh;
            TextureArtifactResolver Texture;
        };
    }

    class ArtifactResolverSnapshot final
    {
    public:
        u64 Generation = 0;
        std::shared_ptr<const ArtifactResolverCatalog> Catalog;
    };

    namespace
    {
#if defined(__cpp_lib_atomic_shared_ptr) && __cpp_lib_atomic_shared_ptr >= 201711L
        std::atomic<Ref<const ArtifactResolverSnapshot>> s_ArtifactResolverState;
#else
        Ref<const ArtifactResolverSnapshot> s_ArtifactResolverState;
#endif
        std::atomic<u64> s_NextArtifactResolverGeneration { 0 };

        void StoreResolvers(std::shared_ptr<const ArtifactResolverCatalog> resolvers)
        {
            auto state = CreateRef<ArtifactResolverSnapshot>();
            state->Generation = s_NextArtifactResolverGeneration.fetch_add(1, std::memory_order_relaxed) + 1;
            state->Catalog = std::move(resolvers);
#if defined(__cpp_lib_atomic_shared_ptr) && __cpp_lib_atomic_shared_ptr >= 201711L
            s_ArtifactResolverState.store(std::move(state), std::memory_order_release);
#else
            std::atomic_store_explicit(&s_ArtifactResolverState,
                Ref<const ArtifactResolverSnapshot>(std::move(state)),
                std::memory_order_release);
#endif
        }

        Ref<const ArtifactResolverSnapshot> LoadResolverState()
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
        PublishArtifactResolvers(registry, MaterialLibrary {});
    }

    void Renderer::PublishArtifactResolvers(
        const AssetRegistry& registry, const MaterialLibrary& materials)
    {
        std::shared_ptr<const ArtifactResolverCatalog> published
            = std::make_shared<const ArtifactResolverCatalog>(registry, materials);
        StoreResolvers(std::move(published));
    }

    Ref<const ArtifactResolverSnapshot>
    Renderer::GetPublishedArtifactResolverSnapshot()
    {
        return LoadResolverState();
    }

    u64 Renderer::GetArtifactResolverSnapshotGeneration(
        const ArtifactResolverSnapshot& snapshot)
    {
        return snapshot.Generation;
    }

    void Renderer::PublishMeshArtifactResolver(const AssetRegistry& registry)
    {
        PublishArtifactResolvers(registry);
    }

    bool Renderer::ResolvePublishedMeshArtifact(AssetHandle asset, MeshArtifact& outArtifact, std::string& outError)
    {
        const Ref<const ArtifactResolverSnapshot> state = LoadResolverState();
        if (!state)
        {
            outError = "renderer has no published mesh artifact resolver";
            return false;
        }
        return ResolvePublishedMeshArtifact(*state, asset, outArtifact, outError);
    }

    bool Renderer::ResolvePublishedMeshArtifact(
        const ArtifactResolverSnapshot& snapshot, AssetHandle asset,
        MeshArtifact& outArtifact, std::string& outError)
    {
        if (!snapshot.Catalog)
        {
            outError = "renderer has no published mesh artifact resolver";
            return false;
        }
        return snapshot.Catalog->Mesh.Resolve(asset, outArtifact, outError);
    }

    bool Renderer::ResolvePublishedTextureArtifact(AssetHandle asset, TextureArtifact& outArtifact, std::string& outError)
    {
        const Ref<const ArtifactResolverSnapshot> state = LoadResolverState();
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
        const Ref<const ArtifactResolverSnapshot> state = LoadResolverState();
        if (!state)
        {
            outCatalogGeneration = 0;
            outError = "renderer has no published texture artifact resolver";
            return false;
        }
        return ResolvePublishedTextureArtifactVariantSet(*state, asset,
            preferredTarget, outVariants, outCatalogGeneration, outError);
    }

    bool Renderer::ResolvePublishedTextureArtifactVariantSet(
        const ArtifactResolverSnapshot& snapshot, AssetHandle asset,
        TextureTargetProfile preferredTarget,
        TextureArtifactVariantSet& outVariants, u64& outCatalogGeneration,
        std::string& outError)
    {
        outCatalogGeneration = snapshot.Generation;
        if (!snapshot.Catalog)
        {
            outError = "renderer has no published texture artifact resolver";
            return false;
        }
        return snapshot.Catalog->Texture.ResolveVariantSet(
            asset, preferredTarget, outVariants, outError);
    }

    u64 Renderer::GetPublishedArtifactResolverGeneration()
    {
        const Ref<const ArtifactResolverSnapshot> state = LoadResolverState();
        return state ? state->Generation : 0;
    }

    bool Renderer::ResolvePublishedMaterialAsset(AssetHandle asset,
        MaterialAsset& outMaterial, u64& outCatalogGeneration, std::string& outError)
    {
        const Ref<const ArtifactResolverSnapshot> state = LoadResolverState();
        if (!state)
        {
            outCatalogGeneration = 0;
            outError = "renderer has no published material catalog";
            return false;
        }
        return ResolvePublishedMaterialAsset(*state, asset, outMaterial,
            outCatalogGeneration, outError);
    }

    bool Renderer::ResolvePublishedMaterialAsset(
        const ArtifactResolverSnapshot& snapshot, AssetHandle asset,
        MaterialAsset& outMaterial, u64& outCatalogGeneration,
        std::string& outError)
    {
        outCatalogGeneration = snapshot.Generation;
        if (!snapshot.Catalog || !snapshot.Catalog->Registry
            || !snapshot.Catalog->Materials)
        {
            outError = "renderer has no published material catalog";
            return false;
        }
        const AssetMetadata* metadata = snapshot.Catalog->Registry->GetAsset(asset);
        if (!metadata || metadata->Type != AssetType::Material)
        {
            outError = !metadata ? "material asset is not registered in the published catalog"
                : "published asset is not a material";
            return false;
        }
        const MaterialAsset* material = snapshot.Catalog->Materials->Get(asset);
        if (!material)
        {
            outError = "material asset has no content in the published catalog";
            return false;
        }
        outMaterial = *material;
        outError.clear();
        return true;
    }

    void Renderer::ClearArtifactResolvers()
    {
        StoreResolvers({});
    }
}
