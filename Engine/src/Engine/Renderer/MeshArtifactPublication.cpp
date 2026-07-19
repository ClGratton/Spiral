#include "Engine/Renderer/Renderer.h"

#include "Engine/Assets/MeshArtifact.h"

#include <atomic>
#include <memory>

namespace Engine
{
    namespace
    {
#if defined(__cpp_lib_atomic_shared_ptr) && __cpp_lib_atomic_shared_ptr >= 201711L
        std::atomic<std::shared_ptr<const MeshArtifactResolver>> s_MeshArtifactResolver;
#else
        std::shared_ptr<const MeshArtifactResolver> s_MeshArtifactResolver;
#endif

        void StoreResolver(std::shared_ptr<const MeshArtifactResolver> resolver)
        {
#if defined(__cpp_lib_atomic_shared_ptr) && __cpp_lib_atomic_shared_ptr >= 201711L
            s_MeshArtifactResolver.store(std::move(resolver), std::memory_order_release);
#else
            std::atomic_store_explicit(&s_MeshArtifactResolver, std::move(resolver), std::memory_order_release);
#endif
        }

        std::shared_ptr<const MeshArtifactResolver> LoadResolver()
        {
#if defined(__cpp_lib_atomic_shared_ptr) && __cpp_lib_atomic_shared_ptr >= 201711L
            return s_MeshArtifactResolver.load(std::memory_order_acquire);
#else
            return std::atomic_load_explicit(&s_MeshArtifactResolver, std::memory_order_acquire);
#endif
        }
    }

    void Renderer::PublishMeshArtifactResolver(const AssetRegistry& registry)
    {
        std::shared_ptr<const MeshArtifactResolver> published = std::make_shared<const MeshArtifactResolver>(registry);
        StoreResolver(std::move(published));
    }

    bool Renderer::ResolvePublishedMeshArtifact(AssetHandle asset, MeshArtifact& outArtifact, std::string& outError)
    {
        const std::shared_ptr<const MeshArtifactResolver> resolver = LoadResolver();
        if (!resolver)
        {
            outError = "renderer has no published mesh artifact resolver";
            return false;
        }

        return resolver->Resolve(asset, outArtifact, outError);
    }

    void Renderer::ClearMeshArtifactResolver()
    {
        StoreResolver({});
    }
}
