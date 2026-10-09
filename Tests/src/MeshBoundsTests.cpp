#include "MeshBoundsTests.h"

#include "TestSupport/GeneratedTest.h"

#include "Engine/Assets/AssetRegistry.h"
#include "Engine/Assets/MeshArtifact.h"
#include "Engine/Scene/Scene.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace SpiralTests
{
    namespace
    {
        using namespace Engine;

        constexpr std::string_view kMeshSource = "Tests/Generated/Bounds.mesh";

        struct Checker
        {
            bool Passed = true;

            void operator()(bool condition, std::string_view message)
            {
                if (!condition)
                {
                    std::cerr << "Mesh bounds test failed: " << message << '\n';
                    Passed = false;
                }
            }
        };

        class TempDir
        {
        public:
            TempDir()
            {
                static std::atomic<unsigned> counter { 0 };
                std::error_code error;
                m_Path = std::filesystem::temp_directory_path(error)
                    / ("spiral-mesh-bounds-" + std::to_string(counter.fetch_add(1)) + "-"
                        + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
                std::filesystem::create_directories(m_Path, error);
            }

            ~TempDir()
            {
                std::error_code error;
                std::filesystem::remove_all(m_Path, error);
            }

            TempDir(const TempDir&) = delete;
            TempDir& operator=(const TempDir&) = delete;

            const std::filesystem::path& Path() const { return m_Path; }

        private:
            std::filesystem::path m_Path;
        };

        MeshArtifactVertex Vertex(float x, float y, float z)
        {
            MeshArtifactVertex vertex;
            vertex.Position[0] = x;
            vertex.Position[1] = y;
            vertex.Position[2] = z;
            vertex.Normal[1] = 1.0f;
            return vertex;
        }

        MeshArtifactPrimitive Primitive(u64 firstVertex, u64 vertexCount, u64 firstIndex, u64 indexCount)
        {
            MeshArtifactPrimitive primitive;
            primitive.VertexByteOffset = firstVertex * sizeof(MeshArtifactVertex);
            primitive.VertexByteSize = vertexCount * sizeof(MeshArtifactVertex);
            primitive.IndexByteOffset = firstIndex * sizeof(u32);
            primitive.IndexByteSize = indexCount * sizeof(u32);
            return primitive;
        }

        bool Same(const MeshArtifactBounds& a, const MeshArtifactBounds& b)
        {
            return std::memcmp(&a, &b, sizeof(MeshArtifactBounds)) == 0;
        }

        // Compared with == so that -0.0 and 0.0 are equal; every other value is exact.
        bool Equal(const MeshArtifactBounds& a, const MeshArtifactBounds& b)
        {
            for (size_t axis = 0; axis < 3; ++axis)
                if (a.Min[axis] != b.Min[axis] || a.Max[axis] != b.Max[axis])
                    return false;
            return true;
        }

        MeshArtifactBounds Sentinel()
        {
            return { { 111.0f, 222.0f, 333.0f }, { 444.0f, 555.0f, 666.0f } };
        }

        // Two primitives whose drawn vertices and a distractor in each unreferenced position are
        // laid out so every extreme is hand computable:
        //   primitive A: vertices 0-3, indices 0 1 2     (vertex 3 is inside A's range but never drawn)
        //   primitive B: vertices 4-6, indices 4 5 6
        //   vertex 7 is in no primitive's range and is only named by trailing indices.
        MeshArtifact HandMesh()
        {
            MeshArtifact artifact;
            artifact.Asset = 7;
            artifact.SourcePath = std::string(kMeshSource);
            artifact.Vertices = {
                Vertex(1.0f, 2.0f, 3.0f), Vertex(-4.0f, 0.5f, 7.0f), Vertex(0.0f, 0.0f, 0.0f),
                Vertex(100.0f, 100.0f, 100.0f),
                Vertex(10.0f, -20.0f, 30.0f), Vertex(-0.25f, 8.0f, -9.0f), Vertex(3.0f, 3.0f, 3.0f),
                Vertex(-500.0f, -500.0f, -500.0f)
            };
            artifact.Indices = { 0, 1, 2, 4, 5, 6, 7, 7, 7 };
            artifact.Primitives = { Primitive(0, 4, 0, 3), Primitive(4, 3, 3, 3) };
            return artifact;
        }

        // Independent reference: collect the drawn vertex ids first, then reduce each axis.
        MeshArtifactBounds ReferenceBounds(const MeshArtifact& artifact)
        {
            std::set<u32> drawn;
            for (const MeshArtifactPrimitive& primitive : artifact.Primitives)
                for (u64 offset = 0; offset < primitive.IndexByteSize / sizeof(u32); ++offset)
                    drawn.insert(artifact.Indices[primitive.IndexByteOffset / sizeof(u32) + offset]);

            MeshArtifactBounds result;
            for (size_t axis = 0; axis < 3; ++axis)
            {
                std::vector<float> values;
                for (const u32 vertex : drawn)
                    values.push_back(artifact.Vertices[vertex].Position[axis]);
                const auto [low, high] = std::minmax_element(values.begin(), values.end());
                result.Min[axis] = *low;
                result.Max[axis] = *high;
            }
            return result;
        }

        // Row-vector point transform in long double, the independent oracle for TransformMeshArtifactBounds.
        void TransformPoint(const Math::Mat4& matrix, const float point[3], long double out[3])
        {
            for (size_t axis = 0; axis < 3; ++axis)
                out[axis] = static_cast<long double>(matrix.Values[12 + axis])
                    + static_cast<long double>(point[0]) * matrix.Values[0 + axis]
                    + static_cast<long double>(point[1]) * matrix.Values[4 + axis]
                    + static_cast<long double>(point[2]) * matrix.Values[8 + axis];
        }

        bool RunProperty(std::string_view name, const Spiral::Tests::Property& property, size_t iterations)
        {
            Spiral::Tests::CampaignOptions options;
            options.Iterations = iterations;
            if (const char* seed = std::getenv("SPIRAL_MESH_BOUNDS_SEED"))
                options.Seed = std::strtoull(seed, nullptr, 10);
            Spiral::Tests::ChoiceTrace replay;
            if (const char* trace = std::getenv("SPIRAL_MESH_BOUNDS_REPLAY");
                trace && Spiral::Tests::ParseTrace(trace, replay))
                options.Replay = replay;

            Spiral::Tests::Counterexample failure;
            if (Spiral::Tests::RunCampaign(options, property, failure))
                return true;

            const std::string minimized = Spiral::Tests::SerializeTrace(failure.MinimizedTrace);
            const std::string rerun = "SPIRAL_MESH_BOUNDS_SEED=" + std::to_string(failure.Seed)
                + " SPIRAL_MESH_BOUNDS_REPLAY=\"" + minimized + "\" EngineTests --test <registered name of "
                + std::string(name) + ">";
            const std::filesystem::path artifact = std::filesystem::temp_directory_path()
                / "spiral-mesh-bounds-counterexample.json";
            std::string artifactError;
            const bool written = Spiral::Tests::WriteCounterexample(artifact, name, failure, rerun, artifactError);
            std::cerr << "Mesh bounds property failed [" << name << "]: " << failure.Message
                << " seed=" << failure.Seed << " iteration=" << failure.Iteration
                << " originalTrace=" << Spiral::Tests::SerializeTrace(failure.OriginalTrace)
                << " minimizedTrace=" << minimized << "\n  rerun: " << rerun << '\n';
            if (written)
                std::cerr << "  counterexample: " << artifact.string() << '\n';
            else
                std::cerr << "  counterexample write failed: " << artifactError << '\n';
            return false;
        }

        double UnitDouble(Spiral::Tests::ChoiceStream& choices)
        {
            return static_cast<double>(choices.Next() >> 11) / 9007199254740992.0;
        }

        float RangeFloat(Spiral::Tests::ChoiceStream& choices, float minimum, float maximum)
        {
            return minimum + static_cast<float>(UnitDouble(choices)) * (maximum - minimum);
        }

        // Several primitives, random triangles over a random subset of each primitive's vertex range
        // (the rest are undrawn distractors inside the range), and one vertex at +-limit that lies
        // beyond every drawn coordinate, sits in no primitive's range and is named only by trailing indices.
        MeshArtifact RandomMesh(Spiral::Tests::ChoiceStream& choices, float limit)
        {
            const float drawnLimit = limit * 0.5f;
            const std::vector<float> specials { 0.0f, -0.0f, drawnLimit, -drawnLimit, std::numeric_limits<float>::denorm_min(), -1.0f, 1.0f };
            const auto coordinate = [&]
            {
                return choices.NextSize(0, 5) == 0 ? specials[choices.NextSize(0, specials.size() - 1)]
                                                   : RangeFloat(choices, -drawnLimit, drawnLimit);
            };

            MeshArtifact artifact;
            artifact.Asset = 9;
            artifact.SourcePath = std::string(kMeshSource);
            const size_t primitives = choices.NextSize(1, 4);
            for (size_t primitiveIndex = 0; primitiveIndex < primitives; ++primitiveIndex)
            {
                const u64 firstVertex = artifact.Vertices.size();
                const u64 vertexCount = choices.NextSize(3, 9);
                for (u64 vertex = 0; vertex < vertexCount; ++vertex)
                    artifact.Vertices.push_back(Vertex(coordinate(), coordinate(), coordinate()));
                const u64 firstIndex = artifact.Indices.size();
                const size_t triangles = choices.NextSize(1, 6);
                for (size_t triangle = 0; triangle < triangles * 3; ++triangle)
                    artifact.Indices.push_back(static_cast<u32>(firstVertex + choices.NextSize(0, vertexCount - 1)));
                artifact.Primitives.push_back(Primitive(firstVertex, vertexCount, firstIndex, triangles * 3));
            }
            const u32 outside = static_cast<u32>(artifact.Vertices.size());
            artifact.Vertices.push_back(Vertex(limit, -limit, limit));
            for (size_t tail = 0; tail < 3; ++tail)
                artifact.Indices.push_back(outside);
            return artifact;
        }
    }

    bool TestMeshBoundsMatchHandComputedValuesAndOnlyCoverDrawnGeometry()
    {
        Checker check;
        std::string error = "stale";

        MeshArtifact cube;
        check(CreateDefaultSceneMeshArtifact(5, cube, error), "default scene mesh builds");
        MeshArtifactBounds bounds = Sentinel();
        check(ComputeMeshArtifactBounds(cube, bounds, error) && error.empty(), "default cube bounds computed and error cleared");
        check(Equal(bounds, { { -0.75f, -0.75f, -0.75f }, { 0.75f, 0.75f, 0.75f } }), "default cube bounds are +-0.75 on every axis");

        const MeshArtifact hand = HandMesh();
        check(ValidateMeshArtifact(hand, error), "hand mesh is a valid artifact: " + error);
        bounds = Sentinel();
        check(ComputeMeshArtifactBounds(hand, bounds, error)
                && Equal(bounds, { { -4.0f, -20.0f, -9.0f }, { 10.0f, 8.0f, 30.0f } }),
            "hand mesh bounds exclude the undrawn distractors and the out-of-range vertex");

        MeshArtifact single;
        single.Asset = 3;
        single.SourcePath = std::string(kMeshSource);
        single.Vertices = { Vertex(2.0f, 2.0f, 2.0f), Vertex(2.0f, 2.0f, 2.0f), Vertex(2.0f, 2.0f, 2.0f) };
        single.Indices = { 0, 1, 2 };
        single.Primitives = { Primitive(0, 3, 0, 3) };
        check(ComputeMeshArtifactBounds(single, bounds, error)
                && Equal(bounds, { { 2.0f, 2.0f, 2.0f }, { 2.0f, 2.0f, 2.0f } }),
            "a degenerate mesh yields a zero-extent box");

        MeshArtifact negative = single;
        negative.Vertices = { Vertex(-1.5f, -2.5f, -3.5f), Vertex(-1.5f, -2.5f, -3.5f), Vertex(-0.5f, -2.0f, -3.0f) };
        check(ComputeMeshArtifactBounds(negative, bounds, error)
                && Equal(bounds, { { -1.5f, -2.5f, -3.5f }, { -0.5f, -2.0f, -3.0f } }),
            "all-negative coordinates keep Max negative rather than clamping at zero");

        // Failure: each invalid artifact is rejected with a message and leaves the output alone.
        const auto expectRejected = [&](const MeshArtifact& artifact, std::string_view what)
        {
            MeshArtifactBounds guarded = Sentinel();
            std::string message;
            check(!ComputeMeshArtifactBounds(artifact, guarded, message) && Same(guarded, Sentinel()) && !message.empty(),
                what);
        };
        MeshArtifact broken = hand;
        broken.Vertices[1].Position[2] = std::numeric_limits<float>::quiet_NaN();
        expectRejected(broken, "non-finite position is rejected without writing bounds");
        broken = hand;
        broken.Vertices[2].Position[0] = std::numeric_limits<float>::infinity();
        expectRejected(broken, "infinite position is rejected");
        broken = hand;
        broken.Indices[4] = 99;
        expectRejected(broken, "index outside the vertex array is rejected");
        broken = hand;
        broken.Indices[1] = 4;
        expectRejected(broken, "index escaping its primitive's vertex range is rejected");
        broken = hand;
        broken.Primitives.clear();
        expectRejected(broken, "mesh without primitives is rejected");
        broken = hand;
        broken.Vertices.clear();
        expectRejected(broken, "mesh without vertices is rejected");
        broken = hand;
        broken.Primitives[1].IndexByteSize += sizeof(u32) * 100;
        expectRejected(broken, "primitive index range past the index array is rejected");
        broken = hand;
        broken.Asset = kInvalidAssetHandle;
        expectRejected(broken, "mesh without an asset handle is rejected");

        return check.Passed;
    }

    bool TestMeshBoundsGeneratedMeshesMatchBruteForceAndTransformsContainGeometry()
    {
        const Spiral::Tests::Property property = [](Spiral::Tests::ChoiceStream& choices, std::string& message)
        {
            // Exact bounds over extreme coordinates (up to near float max).
            {
                const MeshArtifact extreme = RandomMesh(choices, 3.0e38f);
                std::string error;
                MeshArtifactBounds bounds = Sentinel();
                if (!ValidateMeshArtifact(extreme, error) || !ComputeMeshArtifactBounds(extreme, bounds, error))
                {
                    message = "generated extreme mesh was rejected: " + error;
                    return false;
                }
                if (!Equal(bounds, ReferenceBounds(extreme)))
                {
                    message = "bounds differ from the brute-force drawn-vertex reference";
                    return false;
                }
            }

            const MeshArtifact mesh = RandomMesh(choices, 1.0e4f);
            std::string error;
            MeshArtifactBounds local;
            if (!ComputeMeshArtifactBounds(mesh, local, error) || !Equal(local, ReferenceBounds(mesh)))
            {
                message = "local bounds differ from the brute-force reference";
                return false;
            }

            const Math::Vec3 scale { RangeFloat(choices, -4.0f, 4.0f), RangeFloat(choices, -4.0f, 4.0f), RangeFloat(choices, 0.001f, 4.0f) };
            const Math::Mat4 matrix = Math::Multiply(
                Math::Multiply(Math::Scale(scale),
                    Math::RotationYawPitchRoll(RangeFloat(choices, -7.0f, 7.0f), RangeFloat(choices, -7.0f, 7.0f), RangeFloat(choices, -7.0f, 7.0f))),
                Math::Translation({ RangeFloat(choices, -1.0e5f, 1.0e5f), RangeFloat(choices, -1.0e5f, 1.0e5f), RangeFloat(choices, -1.0e5f, 1.0e5f) }));
            MeshArtifactBounds world = Sentinel();
            if (!TransformMeshArtifactBounds(local, matrix, world))
            {
                message = "finite affine transform was rejected";
                return false;
            }

            // Reference 1: the box of the eight transformed corners.
            long double cornerMin[3] = { 1.0e300L, 1.0e300L, 1.0e300L };
            long double cornerMax[3] = { -1.0e300L, -1.0e300L, -1.0e300L };
            for (unsigned corner = 0; corner < 8; ++corner)
            {
                const float point[3] = { (corner & 1 ? local.Max : local.Min)[0],
                    (corner & 2 ? local.Max : local.Min)[1], (corner & 4 ? local.Max : local.Min)[2] };
                long double transformed[3];
                TransformPoint(matrix, point, transformed);
                for (size_t axis = 0; axis < 3; ++axis)
                {
                    cornerMin[axis] = std::min(cornerMin[axis], transformed[axis]);
                    cornerMax[axis] = std::max(cornerMax[axis], transformed[axis]);
                }
            }
            for (size_t axis = 0; axis < 3; ++axis)
            {
                // Outward rounding: never inside the exact box, and at most a few float ulps outside it.
                const long double slack = 4.0L * std::numeric_limits<float>::epsilon()
                    * std::max<long double>({ std::fabs(cornerMin[axis]), std::fabs(cornerMax[axis]), 1.0L });
                if (static_cast<long double>(world.Min[axis]) > cornerMin[axis] + 1.0e-9L * slack
                    || static_cast<long double>(world.Max[axis]) < cornerMax[axis] - 1.0e-9L * slack
                    || static_cast<long double>(world.Min[axis]) < cornerMin[axis] - slack
                    || static_cast<long double>(world.Max[axis]) > cornerMax[axis] + slack)
                {
                    message = "transformed bounds differ from the eight-corner reference on axis " + std::to_string(axis);
                    return false;
                }
            }

            // Reference 2: every drawn vertex, transformed on its own, lies inside the result.
            for (const MeshArtifactPrimitive& primitive : mesh.Primitives)
            {
                for (u64 offset = 0; offset < primitive.IndexByteSize / sizeof(u32); ++offset)
                {
                    long double transformed[3];
                    TransformPoint(matrix, mesh.Vertices[mesh.Indices[primitive.IndexByteOffset / sizeof(u32) + offset]].Position, transformed);
                    for (size_t axis = 0; axis < 3; ++axis)
                        if (transformed[axis] < static_cast<long double>(world.Min[axis]) - 1.0e-6L
                            || transformed[axis] > static_cast<long double>(world.Max[axis]) + 1.0e-6L)
                        {
                            message = "a transformed drawn vertex lies outside the transformed bounds";
                            return false;
                        }
                }
            }
            return true;
        };

        return RunProperty("Mesh bounds generated meshes match brute force and transforms contain geometry", property, 300);
    }

    bool TestMeshBoundsResolveCookedArtifactsAndFailClosed()
    {
        Checker check;
        TempDir dir;
        const std::string root = "bounds-generation-1";

        AssetRegistry registry;
        check(registry.SetCookedArtifactBasePath(dir.Path()), "cooked base path");
        const AssetHandle meshHandle = AssetRegistry::GenerateStableHandle(AssetType::Mesh, kMeshSource);
        const AssetHandle textureHandle = AssetRegistry::GenerateStableHandle(AssetType::Texture, "Tests/Generated/Bounds.png");
        const AssetHandle missingHandle = AssetRegistry::GenerateStableHandle(AssetType::Mesh, "Tests/Generated/Missing.mesh");
        AssetMetadata mesh;
        mesh.Handle = meshHandle;
        mesh.Type = AssetType::Mesh;
        mesh.SourcePath = std::string(kMeshSource);
        mesh.Name = "Bounds";
        mesh.SourcePolicy = AssetSourcePolicy::ImmutablePackage;
        mesh.CookedRoot = root;
        AssetMetadata texture = mesh;
        texture.Handle = textureHandle;
        texture.Type = AssetType::Texture;
        texture.SourcePath = "Tests/Generated/Bounds.png";
        AssetMetadata missing = mesh;
        missing.Handle = missingHandle;
        missing.SourcePath = "Tests/Generated/Missing.mesh";
        check(registry.RegisterAsset(mesh) && registry.RegisterAsset(texture) && registry.RegisterAsset(missing),
            "fixture assets register");

        MeshArtifact artifact = HandMesh();
        artifact.Asset = meshHandle;
        std::string error;
        const std::filesystem::path path = GetCookedMeshArtifactPath(meshHandle, root, registry.GetCookedArtifactBasePath());
        check(!path.empty() && StoreMeshArtifact(path, artifact, error), "fixture artifact stores: " + error);

        MeshArtifactBounds bounds = Sentinel();
        error = "stale";
        check(ResolveMeshArtifactBounds(registry, meshHandle, bounds, error) && error.empty()
                && Equal(bounds, { { -4.0f, -20.0f, -9.0f }, { 10.0f, 8.0f, 30.0f } }),
            "bounds resolve from the cooked file to the hand-computed box");

        // The resolved bounds equal the bounds of the artifact that LoadMeshArtifact returns.
        MeshArtifact loaded;
        MeshArtifactBounds loadedBounds;
        check(LoadMeshArtifact(path, loaded, error) && ComputeMeshArtifactBounds(loaded, loadedBounds, error)
                && Equal(loadedBounds, bounds),
            "store then load preserves the bounds");

        const auto expectFailure = [&](AssetHandle handle, std::string_view what)
        {
            MeshArtifactBounds guarded = Sentinel();
            std::string message;
            check(!ResolveMeshArtifactBounds(registry, handle, guarded, message) && Same(guarded, Sentinel()) && !message.empty(), what);
        };
        expectFailure(kInvalidAssetHandle, "invalid handle fails");
        expectFailure(0x1234567812345678ull, "unregistered handle fails");
        expectFailure(textureHandle, "a registered non-mesh asset fails");
        expectFailure(missingHandle, "a registered mesh without a cooked file fails");

        // Corrupt payloads fail closed: truncation and an out-of-range index in an otherwise valid file.
        {
            std::ifstream input(path, std::ios::binary);
            const std::string bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
            check(bytes.size() > 100, "fixture bytes read");
            const auto overwrite = [&](const std::string& content)
            {
                std::ofstream output(path, std::ios::binary | std::ios::trunc);
                output.write(content.data(), static_cast<std::streamsize>(content.size()));
            };
            overwrite(bytes.substr(0, bytes.size() / 2));
            expectFailure(meshHandle, "a truncated artifact fails");
            std::string corrupt = bytes;
            const size_t indices = corrupt.rfind("Indices\n");
            check(indices != std::string::npos, "index section located");
            corrupt.replace(indices + 8, 1, "9");
            overwrite(corrupt);
            expectFailure(meshHandle, "an artifact with an out-of-range index fails");
            overwrite(bytes);
            check(ResolveMeshArtifactBounds(registry, meshHandle, bounds, error), "restored artifact resolves again");
        }

        // The shared default scene mesh resolves to the same box through the cooked path.
        {
            const AssetHandle cubeHandle = AssetRegistry::GenerateStableHandle(AssetType::Mesh, GetDefaultSceneMeshSourcePath());
            AssetMetadata cube = mesh;
            cube.Handle = cubeHandle;
            cube.SourcePath = std::string(GetDefaultSceneMeshSourcePath());
            check(registry.RegisterAsset(cube), "default mesh registers");
            MeshArtifact cubeArtifact;
            check(CreateDefaultSceneMeshArtifact(cubeHandle, cubeArtifact, error)
                    && StoreMeshArtifact(GetCookedMeshArtifactPath(cubeHandle, root, registry.GetCookedArtifactBasePath()), cubeArtifact, error),
                "default mesh stores in the isolated generation");
            check(ResolveMeshArtifactBounds(registry, cubeHandle, bounds, error)
                    && Equal(bounds, { { -0.75f, -0.75f, -0.75f }, { 0.75f, 0.75f, 0.75f } }),
                "default mesh resolves to +-0.75");
        }

        return check.Passed;
    }

    bool TestMeshBoundsTransformedSceneInstancesMatchHandComputedBoxes()
    {
        Checker check;
        const MeshArtifactBounds cube { { -0.75f, -0.75f, -0.75f }, { 0.75f, 0.75f, 0.75f } };
        const Math::WorldGridPolicy policy;
        const Math::DVec3 origin {};

        const auto instance = [&](const Math::DVec3& worldPosition, const Math::Vec3& rotation, const Math::Vec3& scale,
                                  const Math::DVec3& translationOrigin) -> Math::Mat4
        {
            Scene scene("bounds", policy);
            const Entity entity = scene.CreateEntity("instance");
            Math::SectorLocalPosition position;
            if (!Math::TryDecomposeWorldPosition(worldPosition, policy, position)
                || !scene.SetEntityTransform(entity, position, rotation, scale))
            {
                check(false, "fixture transform was rejected");
                return Math::Mat4::Identity();
            }
            return scene.TryGetTransform(entity)->GetCameraRelativeTransform(translationOrigin, policy);
        };

        MeshArtifactBounds world = Sentinel();
        check(TransformMeshArtifactBounds(cube, instance({ 10.0, 20.0, 30.0 }, {}, { 1.0f, 1.0f, 1.0f }, origin), world)
                && Equal(world, { { 9.25f, 19.25f, 29.25f }, { 10.75f, 20.75f, 30.75f } }),
            "translation only moves the box");
        check(TransformMeshArtifactBounds(cube, instance({}, {}, { 2.0f, 4.0f, 8.0f }, origin), world)
                && Equal(world, { { -1.5f, -3.0f, -6.0f }, { 1.5f, 3.0f, 6.0f } }),
            "scale stretches the box about the origin");
        check(TransformMeshArtifactBounds(cube, instance({ 1.0, -2.0, 3.0 }, {}, { 2.0f, 4.0f, 8.0f }, origin), world)
                && Equal(world, { { -0.5f, -5.0f, -3.0f }, { 2.5f, 1.0f, 9.0f } }),
            "scale then translation combine as the composed matrix does");

        // 90 degrees about Z swaps the X and Y extents of a stretched box (float sin/cos tolerance).
        const MeshArtifactBounds stretched { { -3.0f, -1.5f, -0.75f }, { 3.0f, 1.5f, 0.75f } };
        check(TransformMeshArtifactBounds(stretched, instance({}, { 0.0f, 0.0f, 90.0f }, { 1.0f, 1.0f, 1.0f }, origin), world)
                && std::fabs(world.Min[0] + 1.5f) < 1.0e-5f && std::fabs(world.Max[0] - 1.5f) < 1.0e-5f
                && std::fabs(world.Min[1] + 3.0f) < 1.0e-5f && std::fabs(world.Max[1] - 3.0f) < 1.0e-5f
                && std::fabs(world.Min[2] + 0.75f) < 1.0e-6f && std::fabs(world.Max[2] - 0.75f) < 1.0e-6f,
            "a quarter turn about Z swaps the X and Y extents");
        // 45 degrees about Z grows the XY footprint to (|x|+|y|)/sqrt(2).
        const float diagonal = (3.0f + 1.5f) / std::sqrt(2.0f);
        check(TransformMeshArtifactBounds(stretched, instance({}, { 0.0f, 0.0f, 45.0f }, { 1.0f, 1.0f, 1.0f }, origin), world)
                && std::fabs(world.Max[0] - diagonal) < 1.0e-5f && std::fabs(world.Min[0] + diagonal) < 1.0e-5f
                && std::fabs(world.Max[1] - diagonal) < 1.0e-5f && std::fabs(world.Min[1] + diagonal) < 1.0e-5f,
            "an eighth turn about Z gives the diagonal footprint");

        // Camera-relative: an instance a million units out is exact relative to a nearby origin.
        check(TransformMeshArtifactBounds(cube,
                  instance({ 1000000.5, 0.0, 0.0 }, {}, { 1.0f, 1.0f, 1.0f }, { 1000000.0, 0.0, 0.0 }), world)
                && Equal(world, { { -0.25f, -0.75f, -0.75f }, { 1.25f, 0.75f, 0.75f } }),
            "a distant instance is exact in camera-relative space");

        // The real cube mesh: its corners are vertices, so the box of the transformed vertices equals the result.
        {
            MeshArtifact mesh;
            std::string error;
            check(CreateDefaultSceneMeshArtifact(5, mesh, error), "default mesh");
            const Math::Mat4 matrix = instance({ 5.0, -6.0, 7.0 }, { 20.0f, 35.0f, -50.0f }, { 1.5f, 0.5f, 2.0f }, origin);
            long double low[3] = { 1.0e300L, 1.0e300L, 1.0e300L };
            long double high[3] = { -1.0e300L, -1.0e300L, -1.0e300L };
            for (const MeshArtifactVertex& vertex : mesh.Vertices)
            {
                long double transformed[3];
                TransformPoint(matrix, vertex.Position, transformed);
                for (size_t axis = 0; axis < 3; ++axis)
                {
                    low[axis] = std::min(low[axis], transformed[axis]);
                    high[axis] = std::max(high[axis], transformed[axis]);
                }
            }
            check(TransformMeshArtifactBounds(cube, matrix, world), "rotated, scaled and translated cube transforms");
            for (size_t axis = 0; axis < 3; ++axis)
                check(static_cast<long double>(world.Min[axis]) <= low[axis] && low[axis] - world.Min[axis] < 1.0e-5L
                        && static_cast<long double>(world.Max[axis]) >= high[axis] && world.Max[axis] - high[axis] < 1.0e-5L,
                    "transformed cube bounds are tight around the transformed mesh vertices");
        }

        // Failures leave the output alone.
        const auto expectRejected = [&](const MeshArtifactBounds& input, const Math::Mat4& matrix, std::string_view what)
        {
            MeshArtifactBounds guarded = Sentinel();
            check(!TransformMeshArtifactBounds(input, matrix, guarded) && Same(guarded, Sentinel()), what);
        };
        Math::Mat4 matrix = Math::Mat4::Identity();
        matrix.Values[5] = std::numeric_limits<float>::quiet_NaN();
        expectRejected(cube, matrix, "NaN matrix is rejected");
        matrix = Math::Mat4::Identity();
        matrix.Values[12] = std::numeric_limits<float>::infinity();
        expectRejected(cube, matrix, "infinite translation is rejected");
        matrix = Math::Mat4::Identity();
        matrix.Values[3] = 0.5f;
        expectRejected(cube, matrix, "a projective column is rejected");
        matrix = Math::Mat4::Identity();
        matrix.Values[15] = 2.0f;
        expectRejected(cube, matrix, "a non-unit homogeneous scale is rejected");
        expectRejected({ { 1.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 1.0f } }, Math::Mat4::Identity(), "an inverted box is rejected");
        expectRejected({ { std::numeric_limits<float>::quiet_NaN(), 0.0f, 0.0f }, { 1.0f, 1.0f, 1.0f } }, Math::Mat4::Identity(),
            "a NaN box is rejected");
        matrix = Math::Scale({ 1.0e38f, 1.0f, 1.0f });
        expectRejected({ { -10.0f, 0.0f, 0.0f }, { 10.0f, 1.0f, 1.0f } }, matrix, "float overflow of the result is rejected");

        // Identity is exact and idempotent on awkward values.
        const MeshArtifactBounds awkward { { -0.1f, -3.4e38f, std::numeric_limits<float>::denorm_min() },
            { 0.3f, 3.4e38f, 1.0f } };
        check(TransformMeshArtifactBounds(awkward, Math::Mat4::Identity(), world) && Equal(world, awkward),
            "the identity transform returns the box unchanged, including at float limits");

        return check.Passed;
    }
}
