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

#if defined(__cpp_lib_atomic_shared_ptr) && __cpp_lib_atomic_shared_ptr >= 201711L
        std::atomic<std::shared_ptr<const ArtifactResolverCatalog>> s_ArtifactResolvers;
#else
        std::shared_ptr<const ArtifactResolverCatalog> s_ArtifactResolvers;
#endif

        void StoreResolvers(std::shared_ptr<const ArtifactResolverCatalog> resolvers)
        {
#if defined(__cpp_lib_atomic_shared_ptr) && __cpp_lib_atomic_shared_ptr >= 201711L
            s_ArtifactResolvers.store(std::move(resolvers), std::memory_order_release);
#else
            std::atomic_store_explicit(&s_ArtifactResolvers, std::move(resolvers), std::memory_order_release);
#endif
        }

        std::shared_ptr<const ArtifactResolverCatalog> LoadResolvers()
        {
#if defined(__cpp_lib_atomic_shared_ptr) && __cpp_lib_atomic_shared_ptr >= 201711L
            return s_ArtifactResolvers.load(std::memory_order_acquire);
#else
            return std::atomic_load_explicit(&s_ArtifactResolvers, std::memory_order_acquire);
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
        const std::shared_ptr<const ArtifactResolverCatalog> resolvers = LoadResolvers();
        if (!resolvers)
        {
            outError = "renderer has no published mesh artifact resolver";
            return false;
        }

        return resolvers->Mesh.Resolve(asset, outArtifact, outError);
    }

    bool Renderer::ResolvePublishedTextureArtifact(AssetHandle asset, TextureArtifact& outArtifact, std::string& outError)
    {
        const std::shared_ptr<const ArtifactResolverCatalog> resolvers = LoadResolvers();
        if (!resolvers)
        {
            outError = "renderer has no published texture artifact resolver";
            return false;
        }

        return resolvers->Texture.Resolve(asset, outArtifact, outError);
    }

    void Renderer::ClearArtifactResolvers()
    {
        StoreResolvers({});
    }
}
