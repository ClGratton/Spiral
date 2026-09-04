#include "FabImmutableAssetTests.h"

#include "Engine/Assets/AssetRegistry.h"
#include "Engine/Assets/AssetWatcher.h"
#include "Engine/Assets/MeshArtifact.h"
#include "Engine/Assets/TextureArtifact.h"

#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace
{
    using namespace Engine;

    class ScopedFixtureRoot
    {
    public:
        ScopedFixtureRoot()
        {
            std::error_code error;
            m_Original = std::filesystem::current_path(error);
            if (error)
                return;

            static std::atomic<u64> sequence { 0 };
            const u64 timestamp = static_cast<u64>(
                std::chrono::steady_clock::now().time_since_epoch().count());
            m_Token = "FabImmutableAssetTests-" + std::to_string(timestamp) + "-"
                + std::to_string(sequence.fetch_add(1, std::memory_order_relaxed));
            m_Root = std::filesystem::temp_directory_path(error) / m_Token;
            if (error || !std::filesystem::create_directory(m_Root, error) || error)
                return;
            m_OwnsRoot = true;

            std::filesystem::current_path(m_Root, error);
            m_Ready = !error;
        }

        ~ScopedFixtureRoot()
        {
            std::error_code error;
            if (!m_Original.empty())
                std::filesystem::current_path(m_Original, error);
            if (m_OwnsRoot)
                std::filesystem::remove_all(m_Root, error);
        }

        ScopedFixtureRoot(const ScopedFixtureRoot&) = delete;
        ScopedFixtureRoot& operator=(const ScopedFixtureRoot&) = delete;

        bool IsReady() const { return m_Ready; }

        std::string CookedRoot(std::string_view generation) const
        {
            return (std::filesystem::path("generations") / std::string(generation)).generic_string();
        }

    private:
        std::filesystem::path m_Original;
        std::filesystem::path m_Root;
        std::string m_Token;
        bool m_Ready = false;
        bool m_OwnsRoot = false;
    };

    bool Check(bool condition, std::string_view message)
    {
        if (!condition)
            std::cerr << "Fab immutable asset test failed: " << message << '\n';
        return condition;
    }

    bool WriteText(const std::filesystem::path& path, std::string_view text)
    {
        std::error_code error;
        if (!path.parent_path().empty())
            std::filesystem::create_directories(path.parent_path(), error);
        if (error)
            return false;
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output.write(text.data(), static_cast<std::streamsize>(text.size()));
        return static_cast<bool>(output);
    }

    std::string ReadText(const std::filesystem::path& path)
    {
        std::ifstream input(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    }

    bool SameMetadata(const AssetMetadata& left, const AssetMetadata& right)
    {
        return left.Handle == right.Handle && left.Type == right.Type
            && left.SourcePath == right.SourcePath && left.Name == right.Name
            && left.SourcePolicy == right.SourcePolicy && left.CookedRoot == right.CookedRoot;
    }

    bool SameRegistry(const AssetRegistry& left, const AssetRegistry& right)
    {
        const std::vector<AssetMetadata>& leftAssets = left.GetAssets();
        const std::vector<AssetMetadata>& rightAssets = right.GetAssets();
        if (leftAssets.size() != rightAssets.size())
            return false;
        for (size_t index = 0; index < leftAssets.size(); ++index)
            if (!SameMetadata(leftAssets[index], rightAssets[index]))
                return false;
        return left.GetCookedArtifactBasePath() == right.GetCookedArtifactBasePath();
    }

    bool SameMesh(const MeshArtifact& left, const MeshArtifact& right)
    {
        if (left.Asset != right.Asset || left.SourcePath != right.SourcePath
            || left.Primitives.size() != right.Primitives.size()
            || left.Vertices.size() != right.Vertices.size() || left.Indices != right.Indices)
            return false;
        for (size_t index = 0; index < left.Primitives.size(); ++index)
        {
            const MeshArtifactPrimitive& a = left.Primitives[index];
            const MeshArtifactPrimitive& b = right.Primitives[index];
            if (a.SourceMeshIndex != b.SourceMeshIndex || a.SourcePrimitiveIndex != b.SourcePrimitiveIndex
                || a.VertexByteOffset != b.VertexByteOffset || a.VertexByteSize != b.VertexByteSize
                || a.IndexByteOffset != b.IndexByteOffset || a.IndexByteSize != b.IndexByteSize)
                return false;
        }
        for (size_t index = 0; index < left.Vertices.size(); ++index)
        {
            const MeshArtifactVertex& a = left.Vertices[index];
            const MeshArtifactVertex& b = right.Vertices[index];
            for (size_t component = 0; component < 3; ++component)
                if (a.Position[component] != b.Position[component]
                    || a.Normal[component] != b.Normal[component]
                    || a.Color[component] != b.Color[component])
                    return false;
            for (size_t component = 0; component < 2; ++component)
                if (a.UV[component] != b.UV[component])
                    return false;
        }
        return true;
    }

    bool SameTexture(const TextureArtifact& left, const TextureArtifact& right)
    {
        if (left.Asset != right.Asset || left.SourcePath != right.SourcePath
            || left.Role != right.Role || left.ColorSpace != right.ColorSpace
            || left.TargetProfile != right.TargetProfile || left.CookedFormat != right.CookedFormat
            || left.HasAlpha != right.HasAlpha || left.Payload != right.Payload
            || left.Mips.size() != right.Mips.size())
            return false;
        for (size_t index = 0; index < left.Mips.size(); ++index)
        {
            const TextureArtifactMip& a = left.Mips[index];
            const TextureArtifactMip& b = right.Mips[index];
            if (a.Width != b.Width || a.Height != b.Height
                || a.ByteOffset != b.ByteOffset || a.ByteSize != b.ByteSize)
                return false;
        }
        return true;
    }

    bool SameTextureVariants(
        const TextureArtifactVariantSet& left, const TextureArtifactVariantSet& right)
    {
        if (!SameTexture(left.Preferred, right.Preferred)
            || left.RgbaFallback.has_value() != right.RgbaFallback.has_value())
            return false;
        return !left.RgbaFallback
            || SameTexture(*left.RgbaFallback, *right.RgbaFallback);
    }

    AssetMetadata MakeMetadata(AssetHandle handle, AssetType type, std::string source,
        std::string name, AssetSourcePolicy policy, std::string cookedRoot)
    {
        AssetMetadata metadata;
        metadata.Handle = handle;
        metadata.Type = type;
        metadata.SourcePath = std::move(source);
        metadata.Name = std::move(name);
        metadata.SourcePolicy = policy;
        metadata.CookedRoot = std::move(cookedRoot);
        return metadata;
    }

    MeshArtifact MakeMesh(AssetHandle handle, std::string_view source, float marker)
    {
        MeshArtifact artifact;
        std::string error;
        if (!CreateDefaultSceneMeshArtifact(handle, artifact, error))
            return {};
        artifact.SourcePath = std::string(source);
        artifact.Vertices.front().Color[0] = marker;
        return artifact;
    }

    TextureArtifact MakeTexture(AssetHandle handle, std::string_view source, std::array<u8, 4> pixel)
    {
        TextureArtifact artifact;
        artifact.Asset = handle;
        artifact.SourcePath = std::string(source);
        artifact.Role = TextureRole::BaseColor;
        artifact.ColorSpace = TextureColorSpace::Srgb;
        artifact.TargetProfile = TextureTargetProfile::RGBAFallback;
        artifact.CookedFormat = TextureCookedFormat::R8G8B8A8Srgb;
        artifact.HasAlpha = true;
        artifact.Mips = {{ 1, 1, 0, 4 }};
        artifact.Payload.assign(pixel.begin(), pixel.end());
        return artifact;
    }

    bool TestRegistryPersistence(const ScopedFixtureRoot& fixture)
    {
        const std::string generationRoot = fixture.CookedRoot("registry-generation");
        AssetRegistry registry;
        const AssetMetadata package = MakeMetadata(202, AssetType::Texture,
            "./fab-logical/package#sha256=012345", "Fab texture",
            AssetSourcePolicy::ImmutablePackage, generationRoot);
        const AssetMetadata physical = MakeMetadata(101, AssetType::Mesh,
            "Tests/Fixtures/Physical Mesh.gltf", "Physical mesh",
            AssetSourcePolicy::PhysicalFile, {});
        if (!registry.RegisterAsset(package) || !registry.RegisterAsset(physical))
            return Check(false, "schema-2 registry fixture registration");

        const std::filesystem::path firstPath = "registry-v2.spiralassets";
        const std::filesystem::path secondPath = "registry-v2-roundtrip.spiralassets";
        const std::string expected = "SpiralAssetRegistry 2\n"
            "Asset 101 Mesh PhysicalFile \"Tests/Fixtures/Physical Mesh.gltf\" \"Physical mesh\" \"\"\n"
            "Asset 202 Texture ImmutablePackage \"./fab-logical/package#sha256=012345\" \"Fab texture\" \""
            + generationRoot + "\"\n";
        if (!registry.SaveToFile(firstPath) || ReadText(firstPath) != expected)
            return Check(false, "schema-2 save is exact and handle-sorted");

        AssetRegistry loaded;
        if (!loaded.LoadFromFile(firstPath) || !loaded.SaveToFile(secondPath)
            || ReadText(secondPath) != expected
            || loaded.FindAssetByPath(
                AssetType::Texture, "./fab-logical/package#sha256=012345") != 202
            || loaded.GetCookedArtifactBasePath() != std::filesystem::current_path())
            return Check(false, "schema-2 exact round trip");

        const std::filesystem::path legacyPath = "registry-v1.spiralassets";
        const std::string legacy = "SpiralAssetRegistry 1\n"
            "Asset 303 Mesh \".\\\\Tests\\\\Legacy.gltf\" \"Legacy mesh\"\n";
        AssetRegistry migrated;
        if (!WriteText(legacyPath, legacy) || !migrated.LoadFromFile(legacyPath))
            return Check(false, "schema-1 registry migration load");
        const AssetMetadata* migratedMetadata = migrated.GetAsset(303);
        if (!migratedMetadata || migratedMetadata->SourcePath != "Tests/Legacy.gltf"
            || migratedMetadata->SourcePolicy != AssetSourcePolicy::PhysicalFile
            || !migratedMetadata->CookedRoot.empty())
            return Check(false, "schema-1 defaults physical source and empty cooked root");

        AssetRegistry sentinel;
        const AssetMetadata sentinelMetadata = MakeMetadata(404, AssetType::Scene,
            "Tests/Fixtures/Sentinel.spiral", "Sentinel", AssetSourcePolicy::PhysicalFile, {});
        if (!sentinel.RegisterAsset(sentinelMetadata)
            || !sentinel.SetCookedArtifactBasePath(std::filesystem::current_path()))
            return Check(false, "transaction sentinel registration");
        const AssetRegistry before = sentinel;
        std::vector<std::string> rejected = {
            "SpiralAssetRegistry 2 trailing\n",
            "SpiralAssetRegistry 2\nAsset malformed\n",
            "SpiralAssetRegistry 2\nAsset 1 Unknown PhysicalFile \"a\" \"a\" \"\"\n",
            "SpiralAssetRegistry 2\nAsset 1 Mesh UnknownPolicy \"a\" \"a\" \"\"\n",
            "SpiralAssetRegistry 2\nAsset 1 Mesh ImmutablePackage \"logical\" \"a\" \"\"\n",
            "SpiralAssetRegistry 2\nAsset 1 Mesh PhysicalFile \"a\" \"a\" \"\" trailing\n",
            "SpiralAssetRegistry 2\nAsset 1 Mesh PhysicalFile \"./a\" \"a\" \"\"\n",
            "SpiralAssetRegistry 2\nAsset 1 Mesh PhysicalFile \"a\\\\b\" \"a\" \"\"\n",
            "SpiralAssetRegistry 2\nUnknown 1\n",
            "SpiralAssetRegistry 2\nAsset 1 Mesh PhysicalFile \"a\" \"a\" \"\"\n"
                "Asset 1 Texture PhysicalFile \"b\" \"b\" \"\"\n",
            "SpiralAssetRegistry 2\nAsset 1 Mesh PhysicalFile \"a\" \"a\" \"\"\n"
                "Asset 2 Mesh ImmutablePackage \"a\" \"b\" \"safe/root\"\n"
        };
        const std::vector<std::string> unsafeRoots = {
            "/absolute", "../escape", "safe/../escape", "safe\\child", "safe//child",
            "./safe", "safe/.", "C:/root", "safe/trailing.", "safe/ trailing ",
            "safe/CON", std::string(256, 'a')
        };
        for (const std::string& root : unsafeRoots)
        {
            std::ostringstream malformed;
            malformed << "SpiralAssetRegistry 2\nAsset 1 Mesh ImmutablePackage \"logical\" \"name\" \""
                << root << "\"\n";
            rejected.push_back(malformed.str());
        }
        std::string controlRoot = "safe/";
        controlRoot.push_back('\x01');
        controlRoot += "child";
        std::ostringstream controlFixture;
        controlFixture << "SpiralAssetRegistry 2\nAsset 1 Mesh ImmutablePackage \"logical\" \"name\" \""
            << controlRoot << "\"\n";
        rejected.push_back(controlFixture.str());

        for (size_t index = 0; index < rejected.size(); ++index)
        {
            const std::filesystem::path rejectedPath = std::filesystem::path("rejected-registry-bases")
                / std::to_string(index) / "invalid.spiralassets";
            if (!WriteText(rejectedPath, rejected[index]) || sentinel.LoadFromFile(rejectedPath)
                || !SameRegistry(sentinel, before))
                return Check(false, "malformed registry rejection is transactional");
        }
        return Check(AssetRegistry::IsValidCookedRoot(generationRoot)
                && AssetRegistry::IsValidCookedRoot("")
                && !AssetRegistry::IsValidCookedRoot("unsafe/../root"),
            "cooked-root validation accepts only optional normalized project-relative paths");
    }

    bool TestGenerationCasAndWatcher(const ScopedFixtureRoot& fixture)
    {
        const std::string oldRoot = fixture.CookedRoot("cas-old");
        const std::string newRoot = fixture.CookedRoot("cas-new");
        AssetRegistry registry;
        const AssetMetadata metadata = MakeMetadata(501, AssetType::Mesh,
            "fab:logical/cas", "CAS", AssetSourcePolicy::ImmutablePackage, oldRoot);
        if (!registry.RegisterAsset(metadata))
            return Check(false, "CAS fixture registration");

        const AssetGeneration oldGeneration { AssetSourcePolicy::ImmutablePackage, oldRoot };
        const AssetGeneration mismatch { AssetSourcePolicy::ImmutablePackage, newRoot };
        const AssetGeneration replacement { AssetSourcePolicy::ImmutablePackage, newRoot };
        const AssetMetadata before = *registry.GetAsset(501);
        const bool mismatched = !registry.CompareAndSwapAssetGeneration(501, mismatch, replacement)
            && SameMetadata(*registry.GetAsset(501), before);
        const AssetGeneration unsafeReplacement { AssetSourcePolicy::PhysicalFile, "../unsafe" };
        const bool unsafeRejected = !registry.CompareAndSwapAssetGeneration(501, oldGeneration, unsafeReplacement)
            && SameMetadata(*registry.GetAsset(501), before);
        const AssetGeneration invalidReplacement {
            static_cast<AssetSourcePolicy>(99), newRoot
        };
        const bool invalidEnumRejected = !registry.CompareAndSwapAssetGeneration(
            501, oldGeneration, invalidReplacement) && SameMetadata(*registry.GetAsset(501), before);
        const bool noOp = registry.CompareAndSwapAssetGeneration(501, oldGeneration, oldGeneration)
            && SameMetadata(*registry.GetAsset(501), before);
        const AssetGeneration crossPolicyReplacement { AssetSourcePolicy::PhysicalFile, {} };
        const bool crossPolicyRejected = !registry.CompareAndSwapAssetGeneration(
            501, oldGeneration, crossPolicyReplacement) && SameMetadata(*registry.GetAsset(501), before);
        const bool replaced = registry.CompareAndSwapAssetGeneration(501, oldGeneration, replacement)
            && registry.GetAsset(501)->SourcePolicy == AssetSourcePolicy::ImmutablePackage
            && registry.GetAsset(501)->CookedRoot == newRoot;
        if (!Check(mismatched && unsafeRejected && invalidEnumRejected && noOp
                && crossPolicyRejected && replaced,
                "CAS enforces expected-old, same-policy safe replacement, and successful no-op semantics"))
            return false;

        if (!WriteText("physical-source.bin", "physical"))
            return Check(false, "watcher physical fixture write");
        AssetRegistry watched;
        const AssetHandle physical = watched.RegisterAsset(AssetType::Mesh, "physical-source.bin", "Physical");
        const AssetMetadata package = MakeMetadata(502, AssetType::Texture,
            "fab:logical/watched-package", "Package", AssetSourcePolicy::ImmutablePackage, oldRoot);
        if (physical == kInvalidAssetHandle || !watched.RegisterAsset(package))
            return Check(false, "watcher fixture registration");
        AssetWatcher watcher;
        watcher.SyncRegistry(watched);
        if (!Check(watcher.GetTrackedCount() == 1 && watcher.GetMissingCount() == 0,
                "watcher excludes immutable package identities"))
            return false;

        std::error_code error;
        std::filesystem::remove("physical-source.bin", error);
        const std::vector<AssetWatchEvent> events = watcher.Poll(watched);
        if (!Check(events.size() == 1 && events.front().Handle == physical
                && events.front().EventType == AssetWatchEventType::Deleted,
                "watcher emits events only for physical sources"))
            return false;
        AssetRegistry packageOnly;
        const AssetMetadata promoted = MakeMetadata(physical, AssetType::Mesh,
            "fab:logical/promoted", "Promoted", AssetSourcePolicy::ImmutablePackage, oldRoot);
        return Check(packageOnly.RegisterAsset(promoted)
                && (watcher.SyncRegistry(packageOnly), watcher.GetTrackedCount() == 0)
                && watcher.GetMissingCount() == 0,
            "watcher removes physical tracking when a published candidate contains only package identity metadata");
    }

    bool TestImmutableArtifactRoots(const ScopedFixtureRoot& fixture)
    {
        constexpr AssetHandle meshHandle = 701;
        constexpr AssetHandle textureHandle = 702;
        const std::string meshSource = "fab:logical/mesh#sha256=mesh";
        const std::string textureSource = "fab:logical/texture#sha256=texture";
        const std::string oldRoot = fixture.CookedRoot("generation-old");
        const std::string newRoot = fixture.CookedRoot("generation-new");
        const std::string missingRoot = fixture.CookedRoot("generation-missing");

        AssetRegistry oldRegistry;
        if (!oldRegistry.SetCookedArtifactBasePath(std::filesystem::current_path())
            || !oldRegistry.RegisterAsset(MakeMetadata(meshHandle, AssetType::Mesh,
                meshSource, "Mesh", AssetSourcePolicy::ImmutablePackage, oldRoot))
            || !oldRegistry.RegisterAsset(MakeMetadata(textureHandle, AssetType::Texture,
                textureSource, "Texture", AssetSourcePolicy::ImmutablePackage, oldRoot)))
            return Check(false, "immutable artifact registry fixture registration");

        const MeshArtifact oldMesh = MakeMesh(meshHandle, meshSource, 0.125f);
        const MeshArtifact newMesh = MakeMesh(meshHandle, meshSource, 0.875f);
        const MeshArtifact legacyMesh = MakeMesh(meshHandle, meshSource, 0.5f);
        const TextureArtifact oldTexture = MakeTexture(textureHandle, textureSource, {{ 1, 2, 3, 255 }});
        const TextureArtifact newTexture = MakeTexture(textureHandle, textureSource, {{ 9, 8, 7, 255 }});
        const TextureArtifact legacyTexture = MakeTexture(textureHandle, textureSource, {{ 5, 5, 5, 255 }});
        std::string error;
        const std::filesystem::path artifactBase = oldRegistry.GetCookedArtifactBasePath();
        const std::filesystem::path oldMeshPath = GetCookedMeshArtifactPath(
            meshHandle, oldRoot, artifactBase);
        const std::filesystem::path newMeshPath = GetCookedMeshArtifactPath(
            meshHandle, newRoot, artifactBase);
        const std::filesystem::path oldTexturePath = GetCookedTextureArtifactPath(
            textureHandle, TextureTargetProfile::RGBAFallback, oldRoot, artifactBase);
        const std::filesystem::path newTexturePath = GetCookedTextureArtifactPath(
            textureHandle, TextureTargetProfile::RGBAFallback, newRoot, artifactBase);
        const std::string absoluteOldRoot = (artifactBase / oldRoot).lexically_normal().generic_string();
        const std::string absoluteNewRoot = (artifactBase / newRoot).lexically_normal().generic_string();
        const bool pathsSafe = oldMeshPath.generic_string().rfind(absoluteOldRoot + "/", 0) == 0
            && newMeshPath.generic_string().rfind(absoluteNewRoot + "/", 0) == 0
            && oldTexturePath.generic_string().rfind(absoluteOldRoot + "/", 0) == 0
            && newTexturePath.generic_string().rfind(absoluteNewRoot + "/", 0) == 0
            && GetCookedMeshArtifactPath(meshHandle, "../unsafe", artifactBase).empty()
            && GetCookedMeshArtifactPath(meshHandle, oldRoot, {}).empty()
            && GetCookedTextureArtifactPath(
                textureHandle, TextureTargetProfile::RGBAFallback, "../unsafe", artifactBase).empty()
            && GetCookedTextureArtifactPath(
                textureHandle, TextureTargetProfile::RGBAFallback, oldRoot, {}).empty();
        const bool stored = pathsSafe
            && StoreMeshArtifact(oldMeshPath, oldMesh, error)
            && StoreMeshArtifact(newMeshPath, newMesh, error)
            && StoreTextureArtifact(oldTexturePath, oldTexture, error)
            && StoreTextureArtifact(newTexturePath, newTexture, error);
        if (!Check(stored, "two immutable roots and isolated legacy fixture artifacts store"))
            return false;

        std::error_code filesystemError;
        const std::filesystem::path unrelatedCwd = artifactBase / "resolver-unrelated-cwd";
        std::filesystem::create_directories(unrelatedCwd, filesystemError);
        std::filesystem::current_path(unrelatedCwd, filesystemError);
        const bool legacyStored = !filesystemError
            && StoreMeshArtifact(GetCookedMeshArtifactPath(meshHandle), legacyMesh, error)
            && StoreTextureArtifact(GetCookedTextureArtifactPath(
                textureHandle, TextureTargetProfile::RGBAFallback), legacyTexture, error);
        if (!Check(legacyStored, "isolated legacy artifacts store after process cwd changes"))
            return false;
        const std::filesystem::path inGenerationLegacyTexture = GetCookedTextureArtifactPath(
            textureHandle, missingRoot, artifactBase);
        if (!StoreTextureArtifact(inGenerationLegacyTexture, legacyTexture, error))
            return Check(false, "in-generation legacy texture poison stores in the isolated fixture");

        MeshArtifactResolver oldMeshResolver(oldRegistry);
        TextureArtifactResolver oldTextureResolver(oldRegistry);
        AssetRegistry candidate = oldRegistry;
        const AssetGeneration oldGeneration { AssetSourcePolicy::ImmutablePackage, oldRoot };
        const AssetGeneration newGeneration { AssetSourcePolicy::ImmutablePackage, newRoot };
        if (!candidate.CompareAndSwapAssetGeneration(meshHandle, oldGeneration, newGeneration)
            || !candidate.CompareAndSwapAssetGeneration(textureHandle, oldGeneration, newGeneration))
            return Check(false, "candidate registry advances both artifact roots");
        MeshArtifactResolver newMeshResolver(candidate);
        TextureArtifactResolver newTextureResolver(candidate);

        const AssetGeneration missingGeneration { AssetSourcePolicy::ImmutablePackage, missingRoot };
        if (!candidate.CompareAndSwapAssetGeneration(meshHandle, newGeneration, missingGeneration)
            || !candidate.CompareAndSwapAssetGeneration(textureHandle, newGeneration, missingGeneration))
            return Check(false, "later candidate mutation after resolver capture");
        MeshArtifactResolver missingMeshResolver(candidate);
        TextureArtifactResolver missingTextureResolver(candidate);

        MeshArtifact resolvedOldMesh;
        MeshArtifact resolvedNewMesh;
        TextureArtifact resolvedOldTexture;
        TextureArtifact resolvedNewTexture;
        TextureArtifactVariantSet oldVariants;
        TextureArtifactVariantSet newVariants;
        const bool generationsResolve = oldMeshResolver.Resolve(meshHandle, resolvedOldMesh, error)
            && newMeshResolver.Resolve(meshHandle, resolvedNewMesh, error)
            && oldTextureResolver.Resolve(textureHandle, resolvedOldTexture, error)
            && newTextureResolver.Resolve(textureHandle, TextureTargetProfile::RGBAFallback,
                resolvedNewTexture, error)
            && oldTextureResolver.ResolveVariantSet(textureHandle, TextureTargetProfile::RGBAFallback,
                oldVariants, error)
            && newTextureResolver.ResolveVariantSet(textureHandle, TextureTargetProfile::RGBAFallback,
                newVariants, error)
            && SameMesh(resolvedOldMesh, oldMesh) && SameMesh(resolvedNewMesh, newMesh)
            && SameTexture(resolvedOldTexture, oldTexture) && SameTexture(resolvedNewTexture, newTexture)
            && SameTexture(oldVariants.Preferred, oldTexture)
            && SameTexture(newVariants.Preferred, newTexture);
        if (!Check(generationsResolve,
                "captured resolver copies retain exact old and new bytes for the same handles"))
            return false;

        MeshArtifact meshSentinel = resolvedOldMesh;
        TextureArtifact textureSentinel = resolvedOldTexture;
        const MeshArtifact meshBeforeMissing = meshSentinel;
        const TextureArtifact textureBeforeMissing = textureSentinel;
        TextureArtifactVariantSet variantSentinel = oldVariants;
        const bool missingRejected = !missingMeshResolver.Resolve(meshHandle, meshSentinel, error)
            && SameMesh(meshSentinel, meshBeforeMissing)
            && !missingTextureResolver.Resolve(textureHandle, textureSentinel, error)
            && SameTexture(textureSentinel, textureBeforeMissing)
            && !missingTextureResolver.ResolveVariantSet(textureHandle,
                TextureTargetProfile::RGBAFallback, variantSentinel, error)
            && SameTextureVariants(variantSentinel, oldVariants);
        if (!Check(missingRejected,
                "missing generation root preserves outputs and never reaches the legacy location"))
            return false;

        {
            std::ofstream corruptMesh(newMeshPath, std::ios::binary | std::ios::app);
            corruptMesh.put('!');
            std::ofstream corruptTexture(newTexturePath, std::ios::binary | std::ios::app);
            corruptTexture.put('!');
        }
        meshSentinel = resolvedOldMesh;
        textureSentinel = resolvedOldTexture;
        variantSentinel = oldVariants;
        const bool corruptRejected = !newMeshResolver.Resolve(meshHandle, meshSentinel, error)
            && SameMesh(meshSentinel, resolvedOldMesh)
            && !newTextureResolver.Resolve(textureHandle, textureSentinel, error)
            && SameTexture(textureSentinel, resolvedOldTexture)
            && !newTextureResolver.ResolveVariantSet(textureHandle,
                TextureTargetProfile::RGBAFallback, variantSentinel, error)
            && SameTextureVariants(variantSentinel, oldVariants);
        return Check(corruptRejected,
            "corrupt generation root preserves outputs and never falls back to valid legacy bytes");
    }
}

namespace SpiralTests
{
    bool TestFabImmutableAssetGenerations()
    {
        ScopedFixtureRoot fixture;
        if (!Check(fixture.IsReady(), "unique RAII fixture root creation"))
            return false;
        return TestRegistryPersistence(fixture)
            && TestGenerationCasAndWatcher(fixture)
            && TestImmutableArtifactRoots(fixture);
    }
}
