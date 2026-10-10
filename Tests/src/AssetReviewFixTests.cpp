#include "AssetReviewFixTests.h"

#include "Engine/Assets/AssetRegistry.h"
#include "Engine/Assets/AssetWatcher.h"
#include "Engine/Assets/GltfImporter.h"
#include "Engine/Assets/MaterialAsset.h"
#include "Engine/Assets/MeshArtifact.h"
#include "Engine/Assets/TextureArtifact.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cctype>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#if defined(GE_PLATFORM_LINUX)
    #include <sys/wait.h>
    #include <unistd.h>
#endif

namespace
{
    using namespace Engine;

    bool Check(bool condition, std::string_view message)
    {
        if (!condition)
            std::cerr << "Asset review-fix test failed: " << message << '\n';
        return condition;
    }

    bool Contains(std::string_view text, std::string_view fragment)
    {
        return text.find(fragment) != std::string_view::npos;
    }

    double MillisecondsSince(std::chrono::steady_clock::time_point start)
    {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    }

    // A unique directory that removes only itself. `EnterAsWorkingDirectory` is for the tests that
    // exercise the legacy cwd-relative cooked locations; it restores the previous directory.
    class ScopedFixture
    {
    public:
        ScopedFixture()
        {
            static std::atomic<u64> sequence { 0 };
            std::error_code error;
            m_Original = std::filesystem::current_path(error);
            m_Root = std::filesystem::temp_directory_path(error)
                / ("AssetReviewFixTests-" + std::to_string(static_cast<u64>(
                    std::chrono::steady_clock::now().time_since_epoch().count()))
                    + "-" + std::to_string(sequence.fetch_add(1, std::memory_order_relaxed)));
            m_Ready = !error && std::filesystem::create_directory(m_Root, error) && !error;
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

        bool IsReady() const { return m_Ready; }
        const std::filesystem::path& Root() const { return m_Root; }
        std::filesystem::path Path(std::string_view name) const { return m_Root / std::string(name); }

        bool EnterAsWorkingDirectory()
        {
            std::error_code error;
            std::filesystem::current_path(m_Root, error);
            m_Entered = !error;
            return m_Entered;
        }

    private:
        std::filesystem::path m_Original;
        std::filesystem::path m_Root;
        bool m_Ready = false;
        bool m_Entered = false;
    };

    bool WriteText(const std::filesystem::path& path, std::string_view text)
    {
        std::error_code error;
        if (!path.parent_path().empty())
            std::filesystem::create_directories(path.parent_path(), error);
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output.write(text.data(), static_cast<std::streamsize>(text.size()));
        return !error && static_cast<bool>(output);
    }

    std::string ReadText(const std::filesystem::path& path)
    {
        std::ifstream input(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    }

    // The pre-fix NormalizeSourcePath, kept verbatim as the independent oracle for the single-scan rewrite.
    std::string ReferenceNormalize(std::string_view sourcePath)
    {
        std::string normalized(sourcePath);
        std::replace(normalized.begin(), normalized.end(), '\\', '/');
        while (!normalized.empty() && std::isspace(static_cast<unsigned char>(normalized.back())))
            normalized.pop_back();
        while (!normalized.empty() && std::isspace(static_cast<unsigned char>(normalized.front())))
            normalized.erase(normalized.begin());
        while (normalized.rfind("./", 0) == 0)
            normalized.erase(0, 2);
        const bool isAbsolutePath = std::filesystem::path(normalized).is_absolute();
        while (!isAbsolutePath && !normalized.empty() && normalized.front() == '/')
            normalized.erase(normalized.begin());
        return normalized;
    }

    // ------------------------------------------------------------------ registry
    bool RegistryRejectsUnsaveableMetadata(const ScopedFixture& fixture)
    {
        bool passed = true;
        AssetRegistry registry;

        // A raw control byte inside a JSON name is accepted by cgltf; it must not poison the registry.
        const AssetHandle hostile = registry.RegisterAsset(AssetType::Mesh, "models/a.gltf",
            std::string("Bad\nName\x01 tail"));
        passed &= Check(hostile != kInvalidAssetHandle, "a hostile name still registers the asset");
        const AssetMetadata* stored = registry.GetAsset(hostile);
        passed &= Check(stored && stored->Name == "Bad Name  tail", "control characters in a name become spaces");

        const AssetHandle longName = registry.RegisterAsset(AssetType::Mesh, "models/long.gltf", std::string(1 << 20, 'n'));
        const AssetMetadata* longStored = registry.GetAsset(longName);
        passed &= Check(longStored && longStored->Name.size() == 256, "an oversized name is capped");

        passed &= Check(registry.RegisterAsset(AssetType::Mesh, "models/line\nbreak.gltf") == kInvalidAssetHandle,
            "a source path with a control character is refused");
        passed &= Check(registry.RegisterAsset(AssetType::Mesh, std::string(5000, 'p')) == kInvalidAssetHandle,
            "an oversized source path is refused");

        passed &= Check(!registry.SetAssetName(hostile, "two\nlines") && registry.GetAsset(hostile)->Name == "Bad Name  tail",
            "renaming to unsafe text is refused and leaves the name alone");
        passed &= Check(registry.SetAssetName(hostile, "Renamed") && registry.GetAsset(hostile)->Name == "Renamed",
            "a safe rename succeeds");

        const std::filesystem::path path = fixture.Path("registry/registry.spiralassets");
        passed &= Check(registry.SaveToFile(path), "a registry that took hostile names saves");
        AssetRegistry reloaded;
        passed &= Check(reloaded.LoadFromFile(path) && reloaded.GetAssets().size() == registry.GetAssets().size(),
            "the saved registry reloads");

        // A single oversized line is refused before it is parsed.
        std::ostringstream oversized;
        oversized << "SpiralAssetRegistry 2\nAsset 1 Mesh PhysicalFile \"" << std::string(20000, 'x') << "\" \"n\" \"\"\n";
        const std::filesystem::path oversizedPath = fixture.Path("registry/oversized.spiralassets");
        AssetRegistry untouched;
        passed &= Check(WriteText(oversizedPath, oversized.str()) && !untouched.LoadFromFile(oversizedPath)
            && untouched.GetAssets().empty(), "an oversized registry line is rejected");
        return passed;
    }

    bool NormalizeSourcePathIsLinearAndEquivalent()
    {
        bool passed = true;
        const std::vector<std::string> cases {
            "", " ", "./", "././a", "  ./  ./a", "\\a\\b", "//a//b", "/abs/path", " /abs ", "./ a", ".//a", "a/./b",
            "\t./x.gltf\n", "..\\up", "./.\\a", "   ", "\t\t./\t", "a b  ", ".", "./.", "./a/", "////", "/ /"
        };
        for (const std::string& item : cases)
            passed &= Check(AssetRegistry::NormalizeSourcePath(item) == ReferenceNormalize(item),
                "NormalizeSourcePath matches the reference for a short case");

        // A generated differential sweep over the characters the algorithm treats specially.
        const std::string alphabet = " ./\\a\t";
        u64 state = 0x9e3779b97f4a7c15ull;
        bool sweep = true;
        for (size_t round = 0; round < 4000; ++round)
        {
            std::string item;
            state = state * 6364136223846793005ull + 1442695040888963407ull;
            const size_t length = static_cast<size_t>((state >> 33) % 14);
            for (size_t index = 0; index < length; ++index)
            {
                state = state * 6364136223846793005ull + 1442695040888963407ull;
                item.push_back(alphabet[static_cast<size_t>((state >> 33) % alphabet.size())]);
            }
            sweep = sweep && AssetRegistry::NormalizeSourcePath(item) == ReferenceNormalize(item);
        }
        passed &= Check(sweep, "NormalizeSourcePath matches the reference over 4000 generated strings");

        // 2 MiB of "./" cost 7.1 s with one erase per token (review measurement); one scan is linear.
        std::string hostile;
        for (size_t index = 0; index < 1024 * 1024; ++index)
            hostile += "./";
        hostile += "target.gltf";
        const auto start = std::chrono::steady_clock::now();
        const std::string normalized = AssetRegistry::NormalizeSourcePath(hostile);
        const double milliseconds = MillisecondsSince(start);
        std::cout << "note: NormalizeSourcePath over 2 MiB of './' took " << milliseconds << " ms\n";
        passed &= Check(normalized == "target.gltf", "a long './' run normalizes to the file name");
        passed &= Check(milliseconds < 1000.0, "NormalizeSourcePath is linear in the input length");
        return passed;
    }

    // ------------------------------------------------------------------ glTF importer
    std::string TriangleGltf(const std::string& meshName, const std::string& bufferUri, const std::string& extra = {},
        const std::string& indicesAccessor = {})
    {
        // 3 vertices (36 bytes) followed by three u16 indices (6 bytes, padded to 8).
        std::string json = "{\"asset\":{\"version\":\"2.0\"}";
        json += extra;
        json += ",\"scene\":0,\"scenes\":[{\"nodes\":[0]}],\"nodes\":[{\"mesh\":0}]";
        json += ",\"meshes\":[{\"name\":\"" + meshName + "\",\"primitives\":[{\"attributes\":{\"POSITION\":0}";
        json += ",\"indices\":1}]}]";
        json += ",\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"},";
        json += indicesAccessor.empty()
            ? "{\"bufferView\":1,\"componentType\":5123,\"count\":3,\"type\":\"SCALAR\"}" : indicesAccessor;
        json += "],\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36},"
                "{\"buffer\":0,\"byteOffset\":36,\"byteLength\":6}]";
        json += ",\"buffers\":[{\"uri\":\"" + bufferUri + "\",\"byteLength\":44}]}";
        return json;
    }

    std::string TriangleBuffer()
    {
        const float positions[9] { 0, 0, 0, 1, 0, 0, 0, 1, 0 };
        std::string bytes(reinterpret_cast<const char*>(positions), sizeof(positions));
        const unsigned char indices[8] { 0, 0, 1, 0, 2, 0, 0, 0 };
        bytes.append(reinterpret_cast<const char*>(indices), sizeof(indices));
        return bytes;
    }

    bool GltfImporterRejectsHostileInputs(ScopedFixture& fixture)
    {
        bool passed = true;
        if (!Check(fixture.EnterAsWorkingDirectory(), "importer fixtures run in their own working directory"))
            return false;
        const std::string buffer = TriangleBuffer();

        const auto importFrom = [&](const std::string& gltfName, const std::string& json, AssetRegistry& registry,
            const AssetPathResolver& resolver = {})
        {
            WriteText(fixture.Path(gltfName), json);
            return GltfImporter::Import(fixture.Path(gltfName), registry, resolver);
        };
        WriteText(fixture.Path("tri.bin"), buffer);

        {
            // A raw control byte plus a JSON escape inside the mesh name used to make the registry unsaveable.
            AssetRegistry registry;
            const GltfImportResult result = importFrom("named.gltf",
                TriangleGltf(std::string("Cube\\nBody\x01") + "Z", "tri.bin"), registry);
            passed &= Check(result.Succeeded, "a valid triangle with an awkward name imports: " + result.Error);
            const AssetMetadata* mesh = registry.GetAsset(result.MeshAsset);
            passed &= Check(mesh && mesh->Name == "Cube Body Z", "the mesh name is JSON-decoded and sanitized");
            passed &= Check(registry.SaveToFile(fixture.Path("named.spiralassets")), "the registry stays saveable");
        }
        {
            AssetRegistry registry;
            const GltfImportResult result = importFrom("draco.gltf",
                TriangleGltf("m", "tri.bin", ",\"extensionsRequired\":[\"KHR_draco_mesh_compression\"]"), registry);
            passed &= Check(!result.Succeeded && Contains(result.Error, "KHR_draco_mesh_compression")
                && registry.GetAssets().empty(), "a required geometry-compression extension is rejected");
        }
        {
            AssetRegistry registry;
            const GltfImportResult result = importFrom("sparse.gltf", TriangleGltf("m", "tri.bin", {},
                "{\"componentType\":5123,\"count\":3,\"type\":\"SCALAR\",\"sparse\":{\"count\":1,"
                "\"indices\":{\"bufferView\":1,\"componentType\":5121},\"values\":{\"bufferView\":1}}}"), registry);
            passed &= Check(!result.Succeeded && registry.GetAssets().empty(),
                "a sparse index accessor no longer imports as collapsed triangles");
        }
        {
            AssetRegistry registry;
            const GltfImportResult result = importFrom("noview.gltf", TriangleGltf("m", "tri.bin", {},
                "{\"componentType\":5123,\"count\":3,\"type\":\"SCALAR\"}"), registry);
            passed &= Check(!result.Succeeded && Contains(result.Error, "no bufferView"),
                "a bufferView-less index accessor is rejected");
        }
        for (const char* uri : { "../outside.bin", "sub/../../outside.bin", "%2e%2e/outside.bin", "/etc/hostname", "file://x" })
        {
            AssetRegistry registry;
            const GltfImportResult result = importFrom("escape.gltf", TriangleGltf("m", uri), registry);
            passed &= Check(!result.Succeeded && Contains(result.Error, "not allowed"),
                std::string("buffer URI '") + uri + "' is refused");
        }
        {
            // 4 * (2^62 + 1 - 1) wraps to zero in cgltf's size check; the independent bounds reject it first.
            AssetRegistry registry;
            const GltfImportResult result = importFrom("wrap.gltf", TriangleGltf("m", "tri.bin", {},
                "{\"bufferView\":1,\"componentType\":5125,\"count\":4611686018427387905,\"type\":\"SCALAR\"}"), registry);
            passed &= Check(!result.Succeeded && Contains(result.Error, "glTF structure is invalid"),
                "an overflowing accessor count is rejected before cgltf_validate scans it");
        }
        {
            // A manifest-relative project outside every cwd/executable ancestor needs its own resolver.
            const std::filesystem::path project = fixture.Path("project-root");
            WriteText(project / "models/tri.gltf", TriangleGltf("tri", "tri.bin"));
            WriteText(project / "models/tri.bin", buffer);
            const AssetPathResolver resolver = [project](std::string_view relative)
            {
                return project / std::filesystem::path(std::string(relative));
            };
            AssetRegistry withoutResolver;
            const GltfImportResult missing = GltfImporter::Import("models/tri.gltf", withoutResolver);
            AssetRegistry withResolver;
            const GltfImportResult found = GltfImporter::Import("models/tri.gltf", withResolver, resolver);
            passed &= Check(!missing.Succeeded && Contains(missing.Error, "Could not find"),
                "the default search does not find a project-relative file outside the working directory");
            passed &= Check(found.Succeeded, "the project resolver finds and imports it: " + found.Error);
        }
        return passed;
    }

    // ------------------------------------------------------------------ watcher
    bool WatcherUsesExplicitProjectRoot(ScopedFixture& fixture)
    {
        bool passed = true;
        const std::filesystem::path project = fixture.Path("watched-project");
        const std::filesystem::path file = project / "assets/data.bin";
        WriteText(file, "one");

        AssetRegistry registry;
        const AssetHandle handle = registry.RegisterAsset(AssetType::Mesh, "assets/data.bin", "Data");

        AssetWatcher defaultWatcher;
        defaultWatcher.SyncRegistry(registry);
        passed &= Check(defaultWatcher.GetTrackedCount() == 1 && defaultWatcher.GetMissingCount() == 1,
            "without a resolver a project-relative source outside the cwd ancestors is reported missing");

        AssetWatcher watcher;
        watcher.SetPathResolver([project](std::string_view relative)
        {
            return project / std::filesystem::path(std::string(relative));
        });
        watcher.SyncRegistry(registry);
        passed &= Check(watcher.GetTrackedCount() == 1 && watcher.GetMissingCount() == 0,
            "an explicit project root finds the source");
        passed &= Check(watcher.Poll(registry).empty(), "an unchanged file produces no event");

        WriteText(file, "longer contents");
        std::vector<AssetWatchEvent> events = watcher.Poll(registry);
        passed &= Check(events.size() == 1 && events[0].Handle == handle
            && events[0].EventType == AssetWatchEventType::Modified && events[0].ResolvedPath == file,
            "a content change is a Modified event for the resolved path");
        passed &= Check(watcher.Poll(registry).empty(), "the change is reported once");

        WriteText(file, "third version, again longer");
        watcher.Acknowledge(handle);
        passed &= Check(watcher.Poll(registry).empty(), "Acknowledge records the new state without an event");

        std::error_code error;
        std::filesystem::remove(file, error);
        events = watcher.Poll(registry);
        passed &= Check(events.size() == 1 && events[0].EventType == AssetWatchEventType::Deleted
            && watcher.GetMissingCount() == 1, "removal is a Deleted event");
        passed &= Check(watcher.Poll(registry).empty(), "deletion is reported once");

        WriteText(file, "back");
        events = watcher.Poll(registry);
        passed &= Check(events.size() == 1 && events[0].EventType == AssetWatchEventType::Restored
            && watcher.GetMissingCount() == 0, "reappearance is a Restored event");

        // The interval gate skips a poll entirely, then yields to the next permitted one.
        watcher.SetMinimumPollInterval(std::chrono::hours(1));
        passed &= Check(watcher.Poll(registry).empty(), "the first throttled poll establishes the clock");
        WriteText(file, "changed during the throttle window");
        passed &= Check(watcher.Poll(registry).empty(), "a poll inside the interval returns nothing");
        watcher.SetMinimumPollInterval(std::chrono::milliseconds(0));
        events = watcher.Poll(registry);
        passed &= Check(events.size() == 1 && events[0].EventType == AssetWatchEventType::Modified,
            "the change surfaces once polling is allowed again");

        // Registry reconciliation keeps tracking consistent when assets come and go.
        AssetRegistry smaller;
        smaller.RegisterAsset(AssetType::Mesh, "assets/other.bin", "Other");
        watcher.SyncRegistry(smaller);
        passed &= Check(watcher.GetTrackedCount() == 1 && watcher.GetMissingCount() == 1,
            "tracking follows the registry (old asset dropped, new one tracked)");
        return passed;
    }

    bool WatcherScalesLinearly(ScopedFixture& fixture)
    {
        constexpr size_t kAssets = 2000;
        const std::filesystem::path project = fixture.Path("many-assets");
        AssetRegistry registry;
        for (size_t index = 0; index < kAssets; ++index)
        {
            const std::string relative = "a/f" + std::to_string(index) + ".bin";
            WriteText(project / relative, "x");
            registry.RegisterAsset(AssetType::Mesh, relative, "n" + std::to_string(index));
        }
        AssetWatcher watcher;
        watcher.SetPathResolver([project](std::string_view relative)
        {
            return project / std::filesystem::path(std::string(relative));
        });
        watcher.SyncRegistry(registry);
        bool passed = Check(watcher.GetTrackedCount() == kAssets && watcher.GetMissingCount() == 0,
            "every asset is tracked");

        watcher.Poll(registry);
        constexpr int kPolls = 20;
        const auto start = std::chrono::steady_clock::now();
        size_t events = 0;
        for (int poll = 0; poll < kPolls; ++poll)
            events += watcher.Poll(registry).size();
        const double perPoll = MillisecondsSince(start) / kPolls;
        std::cout << "note: steady-state Poll over " << kAssets << " assets took " << perPoll << " ms each (Debug)\n";
        passed &= Check(events == 0, "a quiet project emits no events");
        passed &= Check(perPoll < 100.0, "a steady-state poll over 2000 assets stays well under a frame budget");
        return passed;
    }

    // ------------------------------------------------------------------ artifacts
    std::string MeshHeader(u64 vertexCount, u64 indexCount, u64 primitiveCount)
    {
        std::ostringstream text;
        text << "SpiralMeshArtifact 2\nSource \"x\"\nMeshAsset 1\nVertexLayout PositionNormalColorUV32F\n"
             << "VertexStrideBytes 44\nVertexCount " << vertexCount << "\nIndexFormat UInt32\nIndexStrideBytes 4\n"
             << "IndexCount " << indexCount << "\nPrimitiveCount " << primitiveCount << '\n';
        return text.str();
    }

    bool ArtifactsRejectUnsatisfiableCounts(ScopedFixture& fixture)
    {
        bool passed = true;
        {
            // ~300 bytes declaring the admitted maximum vertex count: it used to value-initialize ~738 MB first.
            const std::filesystem::path path = fixture.Path("hostile.spiralmesh");
            WriteText(path, MeshHeader(16777216, 3, 1) + "Primitive 0 0 0 0 0 12\nVertices\n0 0 0\n");
            MeshArtifact artifact;
            std::string error;
            const auto start = std::chrono::steady_clock::now();
            const bool loaded = LoadMeshArtifact(path, artifact, error);
            const double milliseconds = MillisecondsSince(start);
            std::cout << "note: hostile mesh header rejected in " << milliseconds << " ms\n";
            passed &= Check(!loaded && Contains(error, "shorter than its declared counts"),
                "a mesh header whose counts exceed the file is rejected up front: " + error);
            passed &= Check(milliseconds < 250.0, "the rejection does not allocate and zero-fill the declared vertices");

            WriteText(path, MeshHeader(3, 50331648, 16777216) + "Primitive 0 0 0 0 0 12\n");
            passed &= Check(!LoadMeshArtifact(path, artifact, error) && Contains(error, "shorter than its declared counts"),
                "a huge primitive count is rejected before the primitive table is sized");
        }
        {
            const std::filesystem::path path = fixture.Path("hostile.spiraltexture");
            WriteText(path, "SpiralTextureArtifact 2\nSource \"x\"\nTextureAsset 1\nRole BaseColor\nColorSpace Srgb\n"
                "TargetProfile RGBAFallback\nCookedFormat R8G8B8A8Srgb\nHasAlpha 1\nMipCount 1\nMip 1 1 0 4\n"
                "PayloadBytes 1073741824\nPayload\nabcd");
            TextureArtifact artifact;
            std::string error;
            const auto start = std::chrono::steady_clock::now();
            const bool loaded = LoadTextureArtifact(path, artifact, error);
            const double milliseconds = MillisecondsSince(start);
            std::cout << "note: hostile texture header rejected in " << milliseconds << " ms\n";
            passed &= Check(!loaded && Contains(error, "payload length does not match"),
                "a texture whose payload header exceeds the file is rejected before resize: " + error);
            passed &= Check(milliseconds < 250.0, "the rejection does not zero-fill a gigabyte");
        }
        return passed;
    }

    MeshArtifact MakeMesh(std::string_view source)
    {
        MeshArtifact mesh;
        std::string error;
        CreateDefaultSceneMeshArtifact(7, mesh, error);
        mesh.SourcePath = std::string(source);
        return mesh;
    }

    TextureArtifact MakeTexture(u8 value)
    {
        TextureArtifact artifact;
        artifact.Asset = 5;
        artifact.SourcePath = "t";
        artifact.Role = TextureRole::BaseColor;
        artifact.ColorSpace = TextureColorSpace::Srgb;
        artifact.TargetProfile = TextureTargetProfile::RGBAFallback;
        artifact.CookedFormat = TextureCookedFormat::R8G8B8A8Srgb;
        artifact.HasAlpha = true;
        artifact.Mips = {{ 1, 1, 0, 4 }};
        artifact.Payload = { value, value, value, 255 };
        return artifact;
    }

    bool CountDirectoryEntries(const std::filesystem::path& directory, size_t& count)
    {
        std::error_code error;
        count = 0;
        for (std::filesystem::directory_iterator it(directory, error), end; !error && it != end; it.increment(error))
            ++count;
        return !error;
    }

    bool ArtifactStoresPublishAtomically(ScopedFixture& fixture)
    {
        bool passed = true;
        if (!Check(fixture.EnterAsWorkingDirectory(), "store fixtures run in their own working directory"))
            return false;

        // A bare file name has no parent directory; create_directories("") fails with EINVAL.
        std::string error;
        passed &= Check(StoreTextureArtifact("bare.spiraltexture", MakeTexture(9), error), "a bare texture file name stores: " + error);
        TextureArtifact loadedTexture;
        passed &= Check(LoadTextureArtifact("bare.spiraltexture", loadedTexture, error) && loadedTexture.Payload[0] == 9,
            "the bare-name texture reloads");
        passed &= Check(StoreMeshArtifact("bare.spiralmesh", MakeMesh("bare"), error), "a bare mesh file name stores: " + error);

        size_t entries = 0;
        passed &= Check(CountDirectoryEntries(fixture.Root(), entries) && entries == 2,
            "no .tmp.* residue is left beside the published artifacts");

#if defined(GE_PLATFORM_LINUX)
        // Four processes publish different artifacts to the same path. With a per-process counter every
        // child inherits the same temp name, so their bytes interleave into one corrupt file.
        const std::filesystem::path shared = fixture.Path("shared/race.spiralmesh");
        std::filesystem::create_directories(shared.parent_path());
        constexpr int kChildren = 4;
        constexpr int kRounds = 12;
        std::vector<std::string> sources;
        for (int child = 0; child < kChildren; ++child)
            sources.push_back(std::string(8000 + child * 3000, static_cast<char>('a' + child)));
        std::vector<pid_t> pids;
        for (int child = 0; child < kChildren; ++child)
        {
            const pid_t pid = ::fork();
            if (pid == 0)
            {
                const MeshArtifact mesh = MakeMesh(sources[static_cast<size_t>(child)]);
                std::string childError;
                for (int round = 0; round < kRounds; ++round)
                    if (!StoreMeshArtifact(shared, mesh, childError))
                        ::_exit(3);
                ::_exit(0);
            }
            pids.push_back(pid);
        }
        bool childrenSucceeded = true;
        for (const pid_t pid : pids)
        {
            int status = 0;
            childrenSucceeded = childrenSucceeded && ::waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0;
        }
        MeshArtifact raced;
        passed &= Check(childrenSucceeded, "every concurrent writer completes");
        passed &= Check(LoadMeshArtifact(shared, raced, error), "the contended artifact is intact: " + error);
        passed &= Check(std::find(sources.begin(), sources.end(), raced.SourcePath) != sources.end(),
            "the published artifact is exactly one writer's content");
        passed &= Check(CountDirectoryEntries(shared.parent_path(), entries) && entries == 1,
            "contended publishing leaves no temporary files");
#endif
        return passed;
    }

    // ------------------------------------------------------------------ material
    bool MaterialRoundTripsExactly(ScopedFixture& fixture)
    {
        bool passed = true;
        MaterialAsset material;
        material.Name = "m";
        material.Roughness = std::bit_cast<float>(0x3eaaaaaau); // 0.33333334f, six digits would lose bits
        material.BaseColor = { 0.1234567f, 0.7654321f, 1.0f };
        material.Metallic = 0.0000123f;
        const std::filesystem::path path = fixture.Path("exact.spiralmat");
        MaterialAsset loaded;
        passed &= Check(material.SaveToFile(path) && MaterialAsset::LoadFromFile(path, loaded), "a precise material saves and loads");
        passed &= Check(loaded.Roughness == material.Roughness && loaded.BaseColor.X == material.BaseColor.X
            && loaded.BaseColor.Y == material.BaseColor.Y && loaded.Metallic == material.Metallic, "the reloaded material is bit-identical (exact float compare)");

        MaterialAsset nonFinite;
        nonFinite.Roughness = std::numeric_limits<float>::quiet_NaN();
        nonFinite.Metallic = std::numeric_limits<float>::infinity();
        nonFinite.EmissiveStrength = std::numeric_limits<float>::quiet_NaN();
        const std::filesystem::path nanPath = fixture.Path("nan.spiralmat");
        MaterialAsset nanLoaded;
        passed &= Check(nonFinite.SaveToFile(nanPath) && MaterialAsset::LoadFromFile(nanPath, nanLoaded),
            "a NaN field no longer produces a file LoadFromFile rejects");
        passed &= Check(nanLoaded.Roughness == MaterialAsset {}.Roughness && nanLoaded.Metallic == MaterialAsset {}.Metallic
            && nanLoaded.EmissiveStrength == MaterialAsset {}.EmissiveStrength && IsValidMaterialAssetValues(nanLoaded),
            "NaN and infinity fall back to the default, and the result passes the publication gate");

        // BaseColor above 1 is clamped by the loader exactly as the validity gate requires.
        std::string text = ReadText(path);
        const size_t colorLine = text.find("BaseColor ");
        const size_t lineEnd = text.find('\n', colorLine);
        text.replace(colorLine, lineEnd - colorLine, "BaseColor 2 2 2");
        const std::filesystem::path brightPath = fixture.Path("bright.spiralmat");
        MaterialAsset bright;
        passed &= Check(WriteText(brightPath, text) && MaterialAsset::LoadFromFile(brightPath, bright)
            && bright.BaseColor.X == 1.0f && IsValidMaterialAssetValues(bright),
            "an out-of-range BaseColor loads clamped to a value every gate accepts");

        MaterialAsset invalid;
        invalid.AlphaMode = static_cast<MaterialAlphaMode>(99);
        const std::filesystem::path invalidPath = fixture.Path("invalid.spiralmat");
        passed &= Check(!invalid.SaveToFile(invalidPath) && !std::filesystem::exists(invalidPath),
            "an unrepairable material is refused instead of written");
        return passed;
    }
}

namespace SpiralTests
{
    bool TestAssetRegistryRejectsUnsaveableMetadataAndNormalizesLinearly()
    {
        ScopedFixture fixture;
        if (!Check(fixture.IsReady(), "fixture root is available"))
            return false;
        bool passed = true;
        passed &= RegistryRejectsUnsaveableMetadata(fixture);
        passed &= NormalizeSourcePathIsLinearAndEquivalent();
        return passed;
    }

    bool TestAssetWatcherUsesExplicitProjectRootAndReportsEveryTransition()
    {
        ScopedFixture fixture;
        return Check(fixture.IsReady(), "fixture root is available") && WatcherUsesExplicitProjectRoot(fixture);
    }

    bool TestAssetWatcherScalesLinearlyWithAssetCount()
    {
        ScopedFixture fixture;
        return Check(fixture.IsReady(), "fixture root is available") && WatcherScalesLinearly(fixture);
    }

    bool TestGltfImporterRejectsUnsupportedEncodingsAndEscapingBuffers()
    {
        ScopedFixture fixture;
        return Check(fixture.IsReady(), "fixture root is available") && GltfImporterRejectsHostileInputs(fixture);
    }

    bool TestMeshAndTextureArtifactsRejectDeclaredCountsTheFileCannotSupply()
    {
        ScopedFixture fixture;
        return Check(fixture.IsReady(), "fixture root is available") && ArtifactsRejectUnsatisfiableCounts(fixture);
    }

    bool TestMeshAndTextureArtifactStoresPublishAtomicallyAcrossProcesses()
    {
        ScopedFixture fixture;
        return Check(fixture.IsReady(), "fixture root is available") && ArtifactStoresPublishAtomically(fixture);
    }

    bool TestMaterialAssetSavesNonFiniteAndPreciseValuesRoundTripExactly()
    {
        ScopedFixture fixture;
        return Check(fixture.IsReady(), "fixture root is available") && MaterialRoundTripsExactly(fixture);
    }
}
