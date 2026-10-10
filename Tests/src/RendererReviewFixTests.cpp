#include "RendererReviewFixTests.h"

#include "Engine/Assets/AssetRegistry.h"
#include "Engine/Assets/MaterialAsset.h"
#include "Engine/Assets/MeshArtifact.h"
#include "Engine/Assets/TextureArtifact.h"
#include "Engine/Platform/MonotonicDeadlineWaiter.h"
#include "Engine/Renderer/ClusteredLightGrid.h"
#include "Engine/Renderer/FramePacingBenchmark.h"
#include "Engine/Renderer/PresentationPolicy.h"
#include "Engine/Renderer/Renderer.h"
#include "Engine/Renderer/SceneLightPayload.h"
#include "Engine/Renderer/SceneRasterPreparation.h"
#include "Engine/Renderer/SceneShadowMap.h"
#include "Engine/Renderer/SceneSkyAtmosphere.h"
#include "Engine/Scene/Camera.h"
#include "Engine/Scene/Scene.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <random>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

namespace
{
    using namespace Engine;

    bool Check(bool condition, std::string_view message)
    {
        if (!condition)
            std::cerr << "Renderer review-fix test failed: " << message << '\n';
        return condition;
    }

    // A unique directory that removes only itself. The legacy cooked-artifact
    // locations are working-directory relative, so entering the fixture keeps
    // every artifact these tests write away from the default project.
    class ScopedFixture
    {
    public:
        ScopedFixture()
        {
            static std::atomic<u64> sequence { 0 };
            std::error_code error;
            m_Original = std::filesystem::current_path(error);
            m_Root = std::filesystem::temp_directory_path(error)
                / ("RendererReviewFixTests-" + std::to_string(static_cast<u64>(
                    std::chrono::steady_clock::now().time_since_epoch().count()))
                    + "-" + std::to_string(sequence.fetch_add(1, std::memory_order_relaxed)));
            m_Ready = !error && std::filesystem::create_directory(m_Root, error) && !error;
            if (m_Ready)
            {
                std::filesystem::current_path(m_Root, error);
                m_Entered = !error;
            }
        }

        ~ScopedFixture()
        {
            std::error_code error;
            if (m_Entered)
                std::filesystem::current_path(m_Original, error);
            if (m_Ready)
                std::filesystem::remove_all(m_Root, error);
        }

        ScopedFixture(const ScopedFixture&) = delete;
        ScopedFixture& operator=(const ScopedFixture&) = delete;

        bool IsReady() const { return m_Ready && m_Entered; }

    private:
        std::filesystem::path m_Original;
        std::filesystem::path m_Root;
        bool m_Ready = false;
        bool m_Entered = false;
    };

    // A valid single-primitive mesh: an n-by-n vertex grid of quads, so tests
    // can scale the decode cost without inventing a second artifact format.
    MeshArtifact MakeGridMesh(AssetHandle asset, std::string sourcePath, u32 n, float scale)
    {
        MeshArtifact mesh;
        mesh.Asset = asset;
        mesh.SourcePath = std::move(sourcePath);
        for (u32 y = 0; y < n; ++y)
            for (u32 x = 0; x < n; ++x)
            {
                MeshArtifactVertex vertex;
                vertex.Position[0] = static_cast<float>(x) * scale;
                vertex.Position[1] = static_cast<float>(y) * scale;
                vertex.Position[2] = 0.0f;
                vertex.Normal[2] = 1.0f;
                vertex.UV[0] = static_cast<float>(x) / static_cast<float>(n);
                vertex.UV[1] = static_cast<float>(y) / static_cast<float>(n);
                mesh.Vertices.push_back(vertex);
            }
        for (u32 y = 0; y + 1 < n; ++y)
            for (u32 x = 0; x + 1 < n; ++x)
            {
                const u32 i = y * n + x;
                mesh.Indices.insert(mesh.Indices.end(), { i, i + 1, i + n, i + 1, i + n + 1, i + n });
            }
        MeshArtifactPrimitive primitive;
        primitive.VertexByteSize = mesh.Vertices.size() * sizeof(MeshArtifactVertex);
        primitive.IndexByteSize = mesh.Indices.size() * sizeof(u32);
        mesh.Primitives.push_back(primitive);
        return mesh;
    }

    TextureArtifact MakeTexture(AssetHandle asset, std::string sourcePath, TextureRole role, TextureColorSpace space)
    {
        TextureArtifact artifact;
        artifact.Asset = asset;
        artifact.SourcePath = std::move(sourcePath);
        artifact.Role = role;
        artifact.ColorSpace = space;
        artifact.TargetProfile = TextureTargetProfile::RGBAFallback;
        artifact.CookedFormat = space == TextureColorSpace::Srgb
            ? TextureCookedFormat::R8G8B8A8Srgb : TextureCookedFormat::R8G8B8A8Unorm;
        artifact.Mips = { { 1, 1, 0, 4 } };
        artifact.Payload = { 1, 2, 3, 255 };
        return artifact;
    }
}

namespace SpiralTests
{
    // Behavior: one immutable resolver snapshot decodes each cooked mesh or
    // texture-semantics artifact at most once and shares the result; the next
    // published generation starts empty, so a hot reload or generation swap
    // can never be answered from older decoded data.
    // Failure hypothesis: the renderer re-read every artifact from disk per
    // instance per frame (200 instances x 20 meshes), or a cache survived a
    // republish and served stale content.
    // Oracle: the cooked file itself. Deleting or rewriting it after the first
    // resolve is visible only to a resolver that touches the disk again.
    bool TestRetainedResolverCacheDecodesOncePerSnapshotAndInvalidatesOnPublication()
    {
        ScopedFixture fixture;
        bool passed = Check(fixture.IsReady(), "fixture directory could not be entered");
        if (!passed)
            return false;

        constexpr u32 kMeshCount = 20;
        constexpr u32 kInstanceCount = 200;
        AssetRegistry registry;
        std::vector<AssetHandle> meshes;
        std::string error;
        for (u32 index = 0; index < kMeshCount; ++index)
        {
            const std::string source = "Tests/Retained/Mesh" + std::to_string(index) + ".gltf";
            const AssetHandle handle = registry.RegisterAsset(AssetType::Mesh, source, "Mesh");
            passed &= Check(handle != kInvalidAssetHandle
                && StoreMeshArtifact(GetCookedMeshArtifactPath(handle),
                    MakeGridMesh(handle, source, 4 + index, 1.0f), error),
                "mesh fixture could not be stored");
            meshes.push_back(handle);
        }
        const AssetHandle texture = registry.RegisterAsset(AssetType::Texture, "Tests/Retained/Albedo.png", "Albedo");
        passed &= Check(texture != kInvalidAssetHandle
            && StoreTextureArtifact(GetCookedTextureArtifactPath(texture, TextureTargetProfile::RGBAFallback),
                MakeTexture(texture, "Tests/Retained/Albedo.png", TextureRole::BaseColor, TextureColorSpace::Srgb), error),
            "texture fixture could not be stored");
        if (!passed)
            return false;

        Renderer::PublishArtifactResolvers(registry, MaterialLibrary {});
        const Ref<const ArtifactResolverSnapshot> first = Renderer::GetPublishedArtifactResolverSnapshot();
        passed &= Check(first != nullptr, "no snapshot was published");
        if (!first)
            return false;

        // 200 instances over 20 meshes: 20 decodes, 180 retained hits.
        std::vector<PublishedMeshRecord> records(kMeshCount);
        for (u32 instance = 0; instance < kInstanceCount; ++instance)
        {
            PublishedMeshRecord record;
            const u32 slot = instance % kMeshCount;
            passed &= Check(Renderer::ResolvePublishedMeshRecord(*first, meshes[slot], record, error),
                "snapshot mesh record did not resolve");
            if (instance < kMeshCount)
                records[slot] = record;
            passed &= Check(record.Artifact && record.Artifact == records[slot].Artifact,
                "instances sharing a mesh must share one decoded artifact");
            passed &= Check(record.Artifact && record.Artifact->Vertices.size() == (4 + slot) * (4 + slot)
                && record.BoundsMinimum[0] == 0.0f
                && record.BoundsMaximum[0] == static_cast<float>(3 + slot)
                && record.BoundsMaximum[1] == static_cast<float>(3 + slot),
                "decoded record must carry its own vertex bounds");
        }
        ArtifactResolverCacheStats stats = Renderer::GetArtifactResolverSnapshotCacheStats(*first);
        passed &= Check(stats.MeshLoads == kMeshCount && stats.MeshHits == kInstanceCount - kMeshCount,
            "200 instances over 20 meshes must decode 20 times, not per instance");

        // Texture semantics decode once; the large payload is not retained.
        PublishedTextureSemantics semantics;
        TextureArtifactVariantSet loaded;
        passed &= Check(Renderer::ResolvePublishedTextureSemantics(*first, texture,
                TextureTargetProfile::RGBAFallback, semantics, &loaded, error)
            && semantics.Role == TextureRole::BaseColor && semantics.ColorSpace == TextureColorSpace::Srgb
            && loaded.Preferred.Payload.size() == 4,
            "first texture semantics resolve must decode and hand back the variant set");
        TextureArtifactVariantSet unusedLoad;
        for (int repeat = 0; repeat < 50; ++repeat)
            passed &= Check(Renderer::ResolvePublishedTextureSemantics(*first, texture,
                    TextureTargetProfile::RGBAFallback, semantics, &unusedLoad, error)
                && unusedLoad.Preferred.Payload.empty(),
                "a retained semantics hit must not decode or hand back a variant set");
        stats = Renderer::GetArtifactResolverSnapshotCacheStats(*first);
        passed &= Check(stats.TextureSemanticLoads == 1 && stats.TextureSemanticHits == 50,
            "texture semantics must decode exactly once per snapshot");

        // Failures are retained too: a missing mesh is not retried from disk per frame.
        const AssetHandle missing = registry.RegisterAsset(AssetType::Mesh, "Tests/Retained/Missing.gltf", "Missing");
        PublishedMeshRecord missingRecord;
        passed &= Check(!Renderer::ResolvePublishedMeshRecord(*first, missing, missingRecord, error) && !error.empty(),
            "a mesh without a cooked artifact must fail");
        // The snapshot predates the registration, so it never knew this asset.
        const u64 loadsBefore = Renderer::GetArtifactResolverSnapshotCacheStats(*first).MeshLoads;
        passed &= Check(!Renderer::ResolvePublishedMeshRecord(*first, missing, missingRecord, error),
            "a retained failure must stay a failure");
        passed &= Check(Renderer::GetArtifactResolverSnapshotCacheStats(*first).MeshLoads == loadsBefore,
            "a retained failure must not hit the disk again");

        // Oracle: remove the cooked mesh and texture files. The published
        // snapshot is immutable, so it still answers from its retained decode.
        std::error_code filesystemError;
        std::filesystem::remove(GetCookedMeshArtifactPath(meshes[0]), filesystemError);
        std::filesystem::remove(GetCookedTextureArtifactPath(texture, TextureTargetProfile::RGBAFallback), filesystemError);
        PublishedMeshRecord afterDelete;
        passed &= Check(Renderer::ResolvePublishedMeshRecord(*first, meshes[0], afterDelete, error)
            && afterDelete.Artifact == records[0].Artifact,
            "a snapshot must keep serving its retained mesh after the file is gone");
        passed &= Check(Renderer::ResolvePublishedTextureSemantics(*first, texture,
                TextureTargetProfile::RGBAFallback, semantics, nullptr, error)
            && semantics.Role == TextureRole::BaseColor,
            "a snapshot must keep serving its retained texture semantics after the file is gone");

        // Rewrite mesh 1 with different geometry, mesh 0 stays deleted, and
        // flip the texture role. Only a new publication may observe any of it.
        const std::string source1 = "Tests/Retained/Mesh1.gltf";
        passed &= Check(StoreMeshArtifact(GetCookedMeshArtifactPath(meshes[1]),
                MakeGridMesh(meshes[1], source1, 40, 2.0f), error),
            "rewritten mesh could not be stored");
        passed &= Check(StoreTextureArtifact(GetCookedTextureArtifactPath(texture, TextureTargetProfile::RGBAFallback),
                MakeTexture(texture, "Tests/Retained/Albedo.png", TextureRole::Normal, TextureColorSpace::Linear), error),
            "rewritten texture could not be stored");
        PublishedMeshRecord stillOld;
        passed &= Check(Renderer::ResolvePublishedMeshRecord(*first, meshes[1], stillOld, error)
            && stillOld.Artifact == records[1].Artifact && stillOld.Artifact->Vertices.size() == 25,
            "the older snapshot must not observe the rewritten file");

        Renderer::PublishArtifactResolvers(registry, MaterialLibrary {});
        const Ref<const ArtifactResolverSnapshot> second = Renderer::GetPublishedArtifactResolverSnapshot();
        passed &= Check(second && second != first
            && Renderer::GetArtifactResolverSnapshotGeneration(*second)
                > Renderer::GetArtifactResolverSnapshotGeneration(*first),
            "republication must create a newer snapshot generation");
        if (!second)
            return false;
        passed &= Check(Renderer::GetArtifactResolverSnapshotCacheStats(*second).MeshLoads == 0,
            "a new generation must start with an empty decode cache");
        PublishedMeshRecord rewritten;
        passed &= Check(Renderer::ResolvePublishedMeshRecord(*second, meshes[1], rewritten, error)
            && rewritten.Artifact->Vertices.size() == 1600
            && rewritten.BoundsMaximum[0] == 78.0f,
            "the new generation must decode the rewritten mesh");
        PublishedMeshRecord deleted;
        passed &= Check(!Renderer::ResolvePublishedMeshRecord(*second, meshes[0], deleted, error),
            "the new generation must observe the deleted mesh");
        passed &= Check(Renderer::ResolvePublishedTextureSemantics(*second, texture,
                TextureTargetProfile::RGBAFallback, semantics, nullptr, error)
            && semantics.Role == TextureRole::Normal && semantics.ColorSpace == TextureColorSpace::Linear,
            "the new generation must decode the rewritten texture semantics");
        // The older snapshot is still intact for frames that retained it.
        passed &= Check(Renderer::ResolvePublishedMeshRecord(*first, meshes[1], stillOld, error)
            && stillOld.Artifact->Vertices.size() == 25,
            "publishing a newer generation must not disturb a retained older snapshot");

        // Non-finite geometry is rejected at the decode, not by the frame loop.
        Renderer::ClearArtifactResolvers();
        passed &= Check(Renderer::GetPublishedArtifactResolverSnapshot() == nullptr
            || !Renderer::ResolvePublishedMeshRecord(*Renderer::GetPublishedArtifactResolverSnapshot(),
                meshes[1], rewritten, error),
            "a cleared catalog must not resolve meshes");
        return passed;
    }

    // Behavior: every pixel and depth inside a point light's sphere lies in a
    // cluster whose CSR list names that light. The CPU screen-tile bound used to
    // divide the sphere radius by depth around the projected centre, which is
    // narrower than the true silhouette for an off-axis light (the half-width
    // grows with 1 / cos(theta)), so the shader shaded one tile and not its
    // neighbour and cut the light pool along a tile boundary.
    // Oracle: an independent pixel/slice mapping from the projection values and
    // the published slice selector; the published list is only consulted at the
    // end. The first light is the reproduction recorded with the review finding.
    bool TestClusteredLightGridLocalLightBoundsCoverEveryPixelInsideTheSphere()
    {
        struct Case
        {
            float VerticalFovDegrees;
            u32 Width;
            u32 Height;
        };
        const Case cases[] = { { 90.0f, 1920, 1080 }, { 60.0f, 1920, 1080 }, { 75.0f, 1280, 720 } };
        bool passed = true;
        u64 sampledInside = 0;
        for (const Case& item : cases)
        {
            std::mt19937_64 random(0x5EED0000ull + static_cast<u64>(item.VerticalFovDegrees));
            const double aspect = static_cast<double>(item.Width) / static_cast<double>(item.Height);
            const double tanHalf = std::tan(static_cast<double>(item.VerticalFovDegrees) * 0.5 * 3.14159265358979323846 / 180.0);
            for (int lightIndex = 0; lightIndex < 160 && passed; ++lightIndex)
            {
                double position[3];
                double range;
                u32 sampleCount = 600;
                if (lightIndex == 0)
                {
                    position[0] = -20.64; position[1] = 2.93; position[2] = 18.02;
                    range = 0.687;
                    sampleCount = 40000;
                }
                else
                {
                    const double depth = std::uniform_real_distribution<double>(0.6, 60.0)(random);
                    position[2] = depth;
                    position[0] = std::uniform_real_distribution<double>(-1.15, 1.15)(random) * depth * tanHalf * aspect;
                    position[1] = std::uniform_real_distribution<double>(-1.15, 1.15)(random) * depth * tanHalf;
                    range = std::uniform_real_distribution<double>(0.2, std::min(5.0, depth * 0.45))(random);
                }
                Scene scene("Clustered coverage");
                const Entity entity = scene.CreateEntity("Light");
                LightComponent point;
                point.Type = LightType::Point;
                point.PhotometricUnit = LightPhotometricUnit::Lumens;
                point.PhotometricValue = 2000.0;
                point.Range = static_cast<float>(range);
                scene.AddLightComponent(entity, point);
                scene.SetEntityWorldPosition(entity, { position[0], position[1], position[2] });
                CameraProjection projection;
                projection.VerticalFovDegrees = item.VerticalFovDegrees;
                const CameraView camera = BuildCameraView({}, {}, projection,
                    static_cast<float>(aspect), {});
                const SceneRenderSnapshot snapshot = scene.ExtractRenderSnapshot(900 + static_cast<u64>(lightIndex), camera);
                ClusteredLightGridConfig config;
                config.TileSizePixels = 64;
                config.DepthSliceCount = 16;
                config.MaximumLocalLightsPerCluster = 8;
                ClusteredLightGrid grid;
                std::string error;
                if (!BuildClusteredLightGrid(snapshot, 0, item.Width, item.Height, config, grid, error)
                    || grid.Lights.size() != 1)
                {
                    passed = Check(false, "the clustered grid must build for a single point light");
                    break;
                }
                const double centre[3] = { grid.Lights[0].ViewPosition.X, grid.Lights[0].ViewPosition.Y,
                    grid.Lights[0].ViewPosition.Z };
                const double scaleX = camera.Projection.Values[0];
                const double scaleY = camera.Projection.Values[5];
                const double effectiveRange = static_cast<double>(point.Range);
                std::uniform_real_distribution<double> unit(-1.0, 1.0);
                for (u32 sample = 0; sample < sampleCount; ++sample)
                {
                    // Uniform in the ball, biased outward so the silhouette is probed.
                    double offset[3];
                    double length;
                    do
                    {
                        offset[0] = unit(random); offset[1] = unit(random); offset[2] = unit(random);
                        length = std::sqrt(offset[0] * offset[0] + offset[1] * offset[1] + offset[2] * offset[2]);
                    } while (length > 1.0 || length < 1.0e-6);
                    const double shell = 0.995 * std::cbrt(std::uniform_real_distribution<double>(0.0, 1.0)(random));
                    const double factor = effectiveRange * shell / length;
                    const double view[3] = { centre[0] + offset[0] * factor, centre[1] + offset[1] * factor,
                        centre[2] + offset[2] * factor };
                    if (view[2] <= static_cast<double>(grid.NearClip) || view[2] >= static_cast<double>(grid.FarClip))
                        continue;
                    const double pixelX = (scaleX * view[0] / view[2] + 1.0) * 0.5 * item.Width;
                    const double pixelY = (1.0 - scaleY * view[1] / view[2]) * 0.5 * item.Height;
                    if (pixelX < 0.0 || pixelY < 0.0 || pixelX >= item.Width || pixelY >= item.Height)
                        continue;
                    ++sampledInside;
                    const u32 tileX = static_cast<u32>(pixelX) / config.TileSizePixels;
                    const u32 tileY = static_cast<u32>(pixelY) / config.TileSizePixels;
                    const size_t cluster = grid.GetClusterIndex(tileX, tileY,
                        grid.SelectDepthSlice(static_cast<float>(view[2])));
                    bool listed = false;
                    for (u32 cursor = grid.ClusterOffsets[cluster]; cursor < grid.ClusterOffsets[cluster + 1]; ++cursor)
                        listed = listed || grid.LocalLightIndices[cursor] == 0;
                    if (!listed)
                    {
                        std::cerr << "Renderer review-fix test failed: light " << lightIndex << " fov " << item.VerticalFovDegrees
                            << " view (" << centre[0] << ", " << centre[1] << ", " << centre[2] << ") range " << effectiveRange
                            << " omits pixel (" << pixelX << ", " << pixelY << ") depth " << view[2] << '\n';
                        passed = false;
                        break;
                    }
                }
            }
        }
        return Check(passed, "a clustered local-light bound must cover every pixel inside the light sphere")
            && Check(sampledInside > 100000, "the oracle must actually probe on-screen sphere samples");
    }

    // Behavior: the shadow map's texel lattice is fixed in the world, so a fixed
    // world point keeps the same fractional texel coordinate while the camera (and
    // therefore the camera-relative translation origin) moves by sub-texel and
    // multi-texel amounts. Failure hypothesis: snapping in camera-relative light
    // space makes the lattice slide with the camera and the shadow edges swim.
    bool TestShadowTexelLatticeIsFixedInTheWorldUnderCameraTranslation()
    {
        const Math::Vec3 worldPositions[] = { { -2.0f, 0.3f, 4.0f }, { 3.2f, 1.1f, 6.5f }, { 0.7f, -1.4f, 9.1f } };
        const auto prepare = [&](const Math::DVec3& camera, SceneShadowMapFrame& out, std::string& error)
        {
            SceneRasterFrame frame;
            frame.HasValidView = true;
            frame.TranslationOrigin = camera;
            SceneMaterialRow errorRow;
            errorRow.Id = 0;
            errorRow.IsError = true;
            SceneMaterialRow opaqueRow;
            opaqueRow.Id = 1;
            opaqueRow.IsError = false;
            opaqueRow.Material.AlphaMode = MaterialAlphaMode::Opaque;
            frame.MaterialRows = { errorRow, opaqueRow };
            for (const Math::Vec3& world : worldPositions)
            {
                SceneRasterInstance instance;
                instance.MaterialId = 1;
                instance.CastsShadows = true;
                instance.CameraRelativeModel = Math::Translation({
                    static_cast<float>(static_cast<double>(world.X) - camera.X),
                    static_cast<float>(static_cast<double>(world.Y) - camera.Y),
                    static_cast<float>(static_cast<double>(world.Z) - camera.Z) });
                frame.Instances.push_back(instance);
            }
            ClusteredLightRecord light;
            light.SourceEntity = 91;
            light.Type = LightType::Directional;
            light.WorldDirection = { 0.25f, -0.75f, 0.5f };
            light.CastsShadows = true;
            frame.LightGrid.Lights = { light };
            frame.LightGrid.GlobalLightIndices = { 0 };
            const std::vector<SceneObjectBounds> bounds(frame.Instances.size(),
                { { -0.5f, -0.5f, -0.5f }, { 0.5f, 0.5f, 0.5f } });
            return TryPrepareSceneShadowMap(frame, bounds, kSceneShadowMapResolution, out, error);
        };
        // Texel coordinate of instance 0's local origin, a fixed world point.
        const auto texel = [](const SceneShadowMapFrame& shadow, double& outX, double& outY)
        {
            const Math::Mat4& matrix = shadow.Casters[0].ModelToShadowClip;
            outX = (static_cast<double>(matrix.Values[12]) * 0.5 + 0.5) * static_cast<double>(shadow.Resolution);
            outY = (static_cast<double>(matrix.Values[13]) * 0.5 + 0.5) * static_cast<double>(shadow.Resolution);
        };
        const auto fractionalDistance = [](double left, double right)
        {
            double delta = std::fmod(left - right, 1.0);
            if (delta > 0.5) delta -= 1.0;
            if (delta < -0.5) delta += 1.0;
            return std::abs(delta);
        };

        SceneShadowMapFrame reference;
        std::string error;
        double referenceX = 0.0, referenceY = 0.0;
        bool passed = Check(prepare({ 0.0, 0.0, 0.0 }, reference, error) && reference.Enabled && reference.Casters.size() == 3,
            "the reference shadow frame must prepare");
        if (!passed)
            return false;
        texel(reference, referenceX, referenceY);
        const Math::DVec3 cameras[] = {
            { 0.0137, 0.0, 0.0 }, { 0.0291, -0.0173, 0.0211 }, { 0.0553, 0.0119, -0.0387 },
            { 0.011, 0.0, -0.007 }, { 0.5, 0.25, -0.3 }, { 7.3271, -1.4112, 2.9137 }, { 100.3, 2.0, -7.1 } };
        double worstSwim = 0.0;
        for (const Math::DVec3& camera : cameras)
        {
            SceneShadowMapFrame moved;
            double movedX = 0.0, movedY = 0.0;
            if (!prepare(camera, moved, error) || !moved.Enabled || moved.Casters.size() != 3
                || moved.WorldUnitsPerTexel != reference.WorldUnitsPerTexel)
            {
                passed = Check(false, "a translated camera must prepare the same texel size");
                break;
            }
            texel(moved, movedX, movedY);
            worstSwim = std::max({ worstSwim, fractionalDistance(movedX, referenceX), fractionalDistance(movedY, referenceY) });
        }
        std::cout << "ShadowTexelLatticeSwim texelWorldUnits=" << reference.WorldUnitsPerTexel
            << " worstFractionalTexelDelta=" << worstSwim << '\n';
        return passed && Check(worstSwim < 0.02,
            "a fixed world point must keep its fractional shadow texel coordinate while the camera translates");
    }

    // Behavior: payload slot buffers are power-of-two capacity buckets of at
    // least the payload size, so the CSR array growing or shrinking by a few
    // light references does not reallocate both buffers.
    bool TestSceneLightPayloadSlotCapacityBuckets()
    {
        bool passed = true;
        u64 previous = 0;
        for (u64 bytes = 16; bytes <= (u64 { 1 } << 24) && passed; bytes += 16 + bytes / 7)
        {
            const u64 capacity = SceneLightPayloadPublication::GetSlotCapacityBytes(bytes);
            const bool power = capacity != 0 && (capacity & (capacity - 1)) == 0;
            const bool tight = capacity == SceneLightPayloadPublication::MinimumSlotCapacityBytes || capacity / 2 < bytes;
            passed = Check(power && capacity >= bytes && tight && capacity >= previous,
                "slot capacity must be the smallest power of two covering the payload, never smaller than the minimum");
            previous = capacity;
        }
        const u64 base = 20000;
        const u64 capacity = SceneLightPayloadPublication::GetSlotCapacityBytes(base);
        return passed
            && Check(SceneLightPayloadPublication::GetSlotCapacityBytes(base + 5 * 16) == capacity
                    && SceneLightPayloadPublication::GetSlotCapacityBytes(base - 5 * 16) == capacity,
                "payloads differing by a few light references share one slot capacity bucket")
            && Check(SceneLightPayloadPublication::GetSlotCapacityBytes(0) == SceneLightPayloadPublication::MinimumSlotCapacityBytes,
                "an empty payload still maps to the minimum bucket");
    }

    // Behavior: a pending presentation-policy transition is deferred, never
    // failed, while the window has no drawable framebuffer, and an idle backend
    // never fails. Failure hypothesis: the D3D12 wrapper threw on every
    // BeginFrame in which a minimized window reported 0x0, even with nothing
    // pending, ending the process.
    bool TestPendingPresentationPolicyDecisionDefersInsteadOfFailing()
    {
        bool passed = true;
        passed &= Check(DecidePendingPolicyApplication(false, true, 1280, 720) == PendingPolicyDecision::Failed,
            "an uninitialized backend reports failure");
        passed &= Check(DecidePendingPolicyApplication(true, false, 0, 0) == PendingPolicyDecision::NothingPending,
            "a minimized window with nothing pending is not a failure");
        passed &= Check(DecidePendingPolicyApplication(true, false, 1280, 720) == PendingPolicyDecision::NothingPending,
            "nothing pending applies nothing");
        passed &= Check(DecidePendingPolicyApplication(true, true, 0, 0) == PendingPolicyDecision::DeferUntilDrawable
            && DecidePendingPolicyApplication(true, true, 0, 720) == PendingPolicyDecision::DeferUntilDrawable
            && DecidePendingPolicyApplication(true, true, 1280, -1) == PendingPolicyDecision::DeferUntilDrawable,
            "a pending transition waits for a drawable framebuffer");
        passed &= Check(DecidePendingPolicyApplication(true, true, 1, 1) == PendingPolicyDecision::Apply,
            "a pending transition applies once the framebuffer is drawable");

        // The deferral keeps the request pending, so it is applied exactly once later.
        PresentationPolicyTransitionState transition;
        transition.Request(PresentationPolicy::TearingAllowed);
        bool appliedWhileMinimized = false;
        for (int frame = 0; frame < 4; ++frame)
            appliedWhileMinimized = appliedWhileMinimized
                || DecidePendingPolicyApplication(true, transition.IsPending(), 0, 0) == PendingPolicyDecision::Apply;
        const bool stillPending = transition.IsPending();
        const bool appliesOnRestore = DecidePendingPolicyApplication(true, transition.IsPending(), 800, 600) == PendingPolicyDecision::Apply;
        transition.Commit();
        passed &= Check(!appliedWhileMinimized && stillPending && appliesOnRestore
                && DecidePendingPolicyApplication(true, transition.IsPending(), 800, 600) == PendingPolicyDecision::NothingPending,
            "a minimized window defers the request and the first drawable frame applies it exactly once");
        return passed;
    }

    // Behavior: committing a transition that is already applied changes nothing.
    // Failure hypothesis: a restore-after-failed-recreation committed once inside
    // swapchain creation and once more in the recovery path, so one policy change
    // advanced the generation twice.
    bool TestPresentationTransitionCommitIsIdempotentAndOncePerRequest()
    {
        PresentationPolicyTransitionState transition;
        transition.Request(PresentationPolicy::TearingAllowed);
        const bool pending = transition.IsPending();
        transition.Commit();
        const u64 afterFirst = transition.Generation;
        transition.Commit();
        transition.Request(PresentationPolicy::TearingAllowed);
        transition.Commit();
        const bool onceForOneRequest = !transition.IsPending() && transition.Generation == afterFirst && afterFirst == 1;
        transition.Request(PresentationPolicy::Synchronized);
        const bool changedPending = transition.IsPending();
        transition.Commit();
        transition.Reset();
        return Check(pending && onceForOneRequest && changedPending && transition.Generation == 0 && transition.IsPending(),
            "a transition commits once per distinct request even when the backend commits it repeatedly");
    }

    // Behavior: VK_SUBOPTIMAL_KHR requests at most one recreation per extent, so
    // a compositor that keeps answering SUBOPTIMAL cannot force a device-idle
    // swapchain rebuild every frame.
    bool TestSuboptimalRecreationGateRequestsOncePerExtent()
    {
        SuboptimalRecreationGate gate;
        bool passed = Check(gate.ShouldRecreate(1280, 720), "the first suboptimal result recreates");
        for (int frame = 0; frame < 6; ++frame)
            passed &= Check(!gate.ShouldRecreate(1280, 720), "a repeat at the same extent does not recreate again");
        passed &= Check(gate.ShouldRecreate(1600, 900), "a new extent may recreate once");
        passed &= Check(!gate.ShouldRecreate(1600, 900), "and only once");
        gate.Reset();
        passed &= Check(gate.ShouldRecreate(1600, 900), "a deliberate transition re-arms one recreation");
        return passed;
    }

    // Behavior: the deadline waiter reports the CPU consumed by the waiting
    // thread, so a sleeping wait is distinguishable from a spin. Failure
    // hypothesis: std::clock() reports process-wide CPU (POSIX) or wall time
    // (MSVC), so another busy thread, or the wait's own wall time, was charged.
    // Oracle: a second thread burns CPU for the whole wait; the waiting thread
    // sleeps, so its CPU must stay far below the wall time.
    bool TestDeadlineWaiterChargesOnlyTheWaitingThreadsCpu()
    {
        std::atomic<bool> stop { false };
        std::atomic<u64> spinCount { 0 };
        std::thread spinner([&]
        {
            u64 local = 0;
            while (!stop.load(std::memory_order_relaxed))
                ++local;
            spinCount = local;
        });
        Platform::MonotonicDeadlineWaiter waiter;
        const auto start = std::chrono::steady_clock::now();
        const Platform::DeadlineWaitTelemetry telemetry = waiter.WaitUntil(start + std::chrono::milliseconds(120));
        stop = true;
        spinner.join();
        std::cout << "DeadlineWaiterCpuAccounting wallMs=" << telemetry.WallTimeMilliseconds
            << " waitingThreadCpuMs=" << telemetry.CpuTimeMilliseconds << " competingSpinnerIterations=" << spinCount.load() << '\n';
        return Check(telemetry.WallTimeMilliseconds >= 100.0, "the wait must last about its deadline")
            && Check(telemetry.CpuTimeMilliseconds < 50.0,
                "a sleeping wait must not be charged the CPU burned by another thread");
    }

    RendererFrameTiming MakeCaptureFrame(u64 index)
    {
        RendererFrameTiming timing;
        timing.FrameIndex = index;
        return timing;
    }

    // Behavior: retaining a frame and amending one of the newest frames costs the
    // same at 300 and at 30000 retained frames. Failure hypothesis: the capture
    // scanned every retained frame from the front (and erased the front of a
    // vector) inside the measured frame loop, an O(N) cost per frame.
    // Oracle: the previous algorithm, replayed on the same frame type and size.
    bool TestFramePacingCaptureAmendAndRecordCostIsIndependentOfCaptureSize()
    {
        constexpr size_t kCapacity = 30000;
        constexpr int kOperations = 200;
        FramePacingBenchmarkCapture capture(kCapacity);
        capture.Begin(FramePacingBenchmarkCondition {});
        for (u64 index = 0; index < kCapacity; ++index)
            capture.Record(MakeCaptureFrame(index));

        const auto millisecondsFor = [](auto&& body)
        {
            const auto begin = std::chrono::steady_clock::now();
            body();
            return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
        };
        u64 nextFrame = kCapacity;
        bool amended = true;
        const double currentMs = millisecondsFor([&]
        {
            for (int operation = 0; operation < kOperations; ++operation)
            {
                capture.Record(MakeCaptureFrame(nextFrame));
                amended = amended && capture.AmendEffectiveLimitingSource(nextFrame,
                    RendererEffectiveLimitingSource::GpuWork, nextFrame - 1);
                amended = amended && capture.AmendEffectiveLimitingSource(nextFrame - 1,
                    RendererEffectiveLimitingSource::CpuActiveWork, std::nullopt);
                ++nextFrame;
            }
        });

        // The previous implementation, measured on identical data.
        std::vector<RendererFrameTiming> reference;
        reference.reserve(kCapacity);
        for (u64 index = 0; index < kCapacity; ++index)
            reference.push_back(MakeCaptureFrame(index));
        u64 referenceNext = kCapacity;
        bool referenceFound = true;
        const double previousMs = millisecondsFor([&]
        {
            for (int operation = 0; operation < kOperations; ++operation)
            {
                reference.erase(reference.begin());
                reference.push_back(MakeCaptureFrame(referenceNext));
                for (const u64 target : { referenceNext, referenceNext - 1 })
                {
                    const auto found = std::find_if(reference.begin(), reference.end(),
                        [&](const RendererFrameTiming& timing) { return timing.FrameIndex == target; });
                    referenceFound = referenceFound && found != reference.end();
                    if (found != reference.end())
                        found->EffectiveLimitingSource = RendererEffectiveLimitingSource::GpuWork;
                }
                ++referenceNext;
            }
        });
        const std::shared_ptr<const FramePacingBenchmarkSnapshot> snapshot = capture.GetSnapshot();
        std::cout << "FramePacingCaptureCost capacity=" << kCapacity << " sizeofFrameTiming=" << sizeof(RendererFrameTiming)
            << " operations=" << kOperations << " retainedMs=" << currentMs << " previousAlgorithmMs=" << previousMs << '\n';
        return Check(amended && referenceFound && snapshot && snapshot->Frames.size() == kCapacity,
                "the capture must keep its capacity and find every amended frame")
            && Check(snapshot->Frames.back().FrameIndex == nextFrame - 1
                && snapshot->Frames.front().FrameIndex == nextFrame - kCapacity
                && snapshot->Frames.back().EffectiveLimitingSource == RendererEffectiveLimitingSource::GpuWork
                && snapshot->Frames[kCapacity - 2].EffectiveLimitingSource == RendererEffectiveLimitingSource::CpuActiveWork,
                "the newest frames carry their amended limiting sources in order")
            && Check(currentMs * 20.0 < previousMs, "retaining and amending the newest frames must be an order of magnitude cheaper than the front-to-back scan");
    }

    // Measurement only: the cost of one sky preparation (512-sample irradiance
    // integral), reported for the review record; the value must stay finite.
    bool TestSceneSkyAtmospherePreparationCostIsReported()
    {
        SceneRenderSnapshot snapshot;
        snapshot.FrameIndex = 451;
        SceneRenderView view;
        view.Camera = BuildCameraView({}, {}, {}, 16.0f / 9.0f, {});
        snapshot.Views.push_back(view);
        SceneRenderLight sun;
        sun.SourceEntity = 41;
        sun.Type = LightType::Directional;
        sun.PhotometricUnit = LightPhotometricUnit::Lux;
        sun.PhotometricValue = 100000.0;
        sun.Transform.RotationDegrees = { 45.0f, 0.0f, 0.0f };
        snapshot.Lights = { sun };
        SceneSkyAtmosphereFrame frame;
        std::string error;
        constexpr int kIterations = 400;
        bool prepared = true;
        const auto begin = std::chrono::steady_clock::now();
        for (int iteration = 0; iteration < kIterations; ++iteration)
            prepared = prepared && TryPrepareSceneSkyAtmosphere(snapshot, 0, frame, error);
        const double perCallMicroseconds = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - begin).count() / kIterations;
        std::cout << "SceneSkyAtmospherePrepareCost microsecondsPerCall=" << perCallMicroseconds << '\n';
        return Check(prepared && frame.Enabled && std::isfinite(frame.UpperDiffuseIrradiance.X) && frame.UpperDiffuseIrradiance.X > 0.0f,
            "sky preparation must stay finite and enabled");
    }
}
