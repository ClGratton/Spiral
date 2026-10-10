#include "FabImportControllerTests.h"

#include "FabImportController.h"

#include "Engine/Assets/AssetRegistry.h"
#include "Engine/Assets/FabArchive.h"
#include "Engine/Assets/FabProjectState.h"
#include "Engine/Assets/FabZipStaging.h"
#include "Engine/Assets/MeshArtifact.h"
#include "Engine/Core/Sha256.h"
#include "Engine/Jobs/JobSystem.h"
#include "Engine/Scene/Scene.h"

#ifndef MINIZ_NO_ZLIB_COMPATIBLE_NAMES
    #define MINIZ_NO_ZLIB_COMPATIBLE_NAMES
#endif
#include "miniz.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#if defined(__linux__)
    #include <fcntl.h>
    #include <sys/stat.h>
    #include <unistd.h>
#endif

namespace
{
    using namespace Engine;
    using namespace Fab;
    namespace fs = std::filesystem;

    using Bytes = std::vector<u8>;

    constexpr const char* kManifestName = "Project.spiralproject";
    constexpr const char* kListingA = "https://www.fab.com/listings/aaaaaaaa-0000-4000-8000-000000000001";
    constexpr const char* kListingB = "https://www.fab.com/listings/bbbbbbbb-0000-4000-8000-000000000002";

    struct Checker
    {
        bool Passed = true;
        const char* Prefix = "";

        void operator()(bool condition, const std::string& message)
        {
            if (!condition)
            {
                std::cerr << Prefix << ": " << message << '\n';
                Passed = false;
            }
        }
    };

    // ------------------------------------------------------------------ files
    Bytes ToBytes(std::string_view text)
    {
        return Bytes(text.begin(), text.end());
    }

    std::string ReadAll(const fs::path& path)
    {
        std::ifstream input(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    }

    bool WriteBytes(const fs::path& path, const Bytes& bytes)
    {
        std::error_code error;
        fs::create_directories(path.parent_path(), error);
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        return static_cast<bool>(output);
    }

    bool WriteText(const fs::path& path, std::string_view text)
    {
        return WriteBytes(path, ToBytes(text));
    }

    std::string Sha256OfFile(const fs::path& path)
    {
        std::ifstream input(path, std::ios::binary);
        Sha256Builder hash;
        std::vector<char> buffer(1u << 16);
        while (input.read(buffer.data(), static_cast<std::streamsize>(buffer.size())) || input.gcount() > 0)
            hash.Update(std::span<const u8>(reinterpret_cast<const u8*>(buffer.data()), static_cast<size_t>(input.gcount())));
        return hash.FinalizeHex();
    }

    // Path -> "D" | "L:<target>" | "F:<size>:<sha256>"; "unchanged" is a byte-level statement.
    using TreeState = std::map<std::string, std::string>;

    TreeState TakeTree(const fs::path& root, std::string_view skipPrefix = {})
    {
        TreeState state;
        std::error_code error;
        if (!fs::exists(root, error))
            return state;
        for (fs::recursive_directory_iterator iterator(root, error), end; !error && iterator != end; iterator.increment(error))
        {
            const std::string relative = fs::relative(iterator->path(), root, error).generic_string();
            if (!skipPrefix.empty() && (relative == skipPrefix || relative.starts_with(std::string(skipPrefix) + "/")))
                continue;
            const fs::file_status status = iterator->symlink_status(error);
            if (fs::is_symlink(status))
                state[relative] = "L:" + fs::read_symlink(iterator->path(), error).string();
            else if (fs::is_directory(status))
                state[relative] = "D";
            else
                state[relative] = "F:" + std::to_string(fs::file_size(iterator->path(), error)) + ":" + Sha256OfFile(iterator->path());
        }
        return state;
    }

    size_t CountEntries(const fs::path& directory)
    {
        std::error_code error;
        if (!fs::exists(directory, error))
            return 0;
        size_t count = 0;
        for (fs::directory_iterator iterator(directory, error), end; !error && iterator != end; iterator.increment(error))
            ++count;
        return count;
    }

    class Workspace
    {
    public:
        Workspace()
        {
            static std::atomic<u64> sequence { 0 };
            std::error_code error;
            const u64 tick = static_cast<u64>(std::chrono::steady_clock::now().time_since_epoch().count());
            m_Base = fs::temp_directory_path(error) / ("spiral-fab-import-controller-test-" + std::to_string(tick) + "-"
                + std::to_string(sequence.fetch_add(1, std::memory_order_relaxed)));
            m_Ready = !error && fs::create_directories(m_Base / "project", error) && !error
                && fs::create_directories(m_Base / "inputs", error) && !error;
        }

        ~Workspace()
        {
            if (!m_Ready)
                return;
            std::error_code error;
            for (const fs::directory_entry& entry :
                fs::recursive_directory_iterator(m_Base, fs::directory_options::skip_permission_denied, error))
                if (entry.is_directory(error) && !entry.is_symlink(error))
                    fs::permissions(entry.path(), fs::perms::owner_all, fs::perm_options::add, error);
            fs::remove_all(m_Base, error);
        }

        Workspace(const Workspace&) = delete;
        Workspace& operator=(const Workspace&) = delete;

        bool IsReady() const { return m_Ready; }
        fs::path Base() const { return m_Base; }
        fs::path Project() const { return m_Base / "project"; }
        fs::path Manifest() const { return Project() / kManifestName; }
        fs::path PackageStaging() const { return m_Base / "pkg-staging"; }
        fs::path Inputs() const { return m_Base / "inputs"; }
        fs::path CookStaging() const { return Project() / ".fab-staging"; }
        fs::path Input(std::string_view name) const { return Inputs() / std::string(name); }

    private:
        fs::path m_Base;
        bool m_Ready = false;
    };

    class ScopedJobSystem
    {
    public:
        ScopedJobSystem()
        {
            JobSystem& jobs = JobSystem::Get();
            m_Started = !jobs.IsRunning();
            if (m_Started)
                jobs.Initialize(2);
        }

        ~ScopedJobSystem()
        {
            if (m_Started)
                JobSystem::Get().Shutdown();
        }

        ScopedJobSystem(const ScopedJobSystem&) = delete;
        ScopedJobSystem& operator=(const ScopedJobSystem&) = delete;

    private:
        bool m_Started = false;
    };

    // ------------------------------------------------------------------ package builders
    void PutBe32(Bytes& bytes, u32 value)
    {
        for (int shift = 24; shift >= 0; shift -= 8)
            bytes.push_back(static_cast<u8>(value >> shift));
    }

    void PutChunk(Bytes& png, const char (&type)[5], const Bytes& data)
    {
        PutBe32(png, static_cast<u32>(data.size()));
        Bytes typed(type, type + 4);
        typed.insert(typed.end(), data.begin(), data.end());
        png.insert(png.end(), typed.begin(), typed.end());
        PutBe32(png, static_cast<u32>(mz_crc32(mz_crc32(0, nullptr, 0), typed.data(), typed.size())));
    }

    // 2x2 opaque PNG (stored deflate); `seed` changes the first texel so a "changed source" has changed bytes.
    Bytes MakePng(u32 seed, bool corruptChunkCrc = false)
    {
        const Bytes rgba { static_cast<u8>(10 + seed), 20, 30, 255, 40, 50, 60, 255, 70, 80, 90, 255, 100, 110, 120, 255 };
        Bytes raw;
        for (u32 row = 0; row < 2; ++row)
        {
            raw.push_back(0);
            raw.insert(raw.end(), rgba.begin() + row * 8, rgba.begin() + (row + 1) * 8);
        }
        Bytes zlib { 0x78, 0x01, 0x01, static_cast<u8>(raw.size()), 0, static_cast<u8>(~raw.size() & 0xff), 0xff };
        zlib.insert(zlib.end(), raw.begin(), raw.end());
        PutBe32(zlib, static_cast<u32>(mz_adler32(mz_adler32(0, nullptr, 0), raw.data(), raw.size())));
        Bytes png { 0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a };
        Bytes header;
        PutBe32(header, 2);
        PutBe32(header, 2);
        header.insert(header.end(), { 8, 6, 0, 0, 0 });
        PutChunk(png, "IHDR", header);
        PutChunk(png, "IDAT", zlib);
        PutChunk(png, "IEND", {});
        if (corruptChunkCrc)
            png[png.size() - 20] ^= 0x55;
        return png;
    }

    Bytes GeometryBin()
    {
        const float positions[] = { 0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0 };
        const float normals[] = { 0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1 };
        const float uvs[] = { 0, 0, 1, 0, 1, 1, 0, 1 };
        const u16 indices[] = { 0, 1, 2, 0, 2, 3 };
        Bytes bin(140);
        std::memcpy(bin.data(), positions, 48);
        std::memcpy(bin.data() + 48, normals, 48);
        std::memcpy(bin.data() + 96, uvs, 32);
        std::memcpy(bin.data() + 128, indices, 12);
        return bin;
    }

    struct PackageOptions
    {
        u32 Variant = 0;
        bool Textured = true;
        bool Glb = false;
        std::string RequiredExtension;
        std::string BufferUri = "model.bin";
        std::string ImageUri = "base.png";
        bool CorruptPng = false;
    };

    PackageOptions WithVariant(u32 variant)
    {
        PackageOptions options;
        options.Variant = variant;
        return options;
    }

    std::string GltfJson(const PackageOptions& options, size_t bufferLength, size_t pngLength)
    {
        std::ostringstream json;
        json << "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],\"nodes\":[{\"mesh\":0}],"
             << "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0,\"NORMAL\":1,\"TEXCOORD_0\":2},\"indices\":3,\"material\":0}]}],";
        if (options.Textured)
            json << "\"materials\":[{\"pbrMetallicRoughness\":{\"baseColorTexture\":{\"index\":0},\"metallicFactor\":0.5,\"roughnessFactor\":0.5}}],"
                 << "\"textures\":[{\"source\":0}],\"images\":["
                 << (options.Glb ? "{\"bufferView\":4,\"mimeType\":\"image/png\"}" : "{\"uri\":\"" + options.ImageUri + "\"}") << "],";
        else
            json << "\"materials\":[{\"pbrMetallicRoughness\":{\"baseColorFactor\":[0.5,0.25,0.125,1.0],\"metallicFactor\":0.5,\"roughnessFactor\":0.5}}],";
        json << "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":4,\"type\":\"VEC3\"},"
             << "{\"bufferView\":1,\"componentType\":5126,\"count\":4,\"type\":\"VEC3\"},"
             << "{\"bufferView\":2,\"componentType\":5126,\"count\":4,\"type\":\"VEC2\"},"
             << "{\"bufferView\":3,\"componentType\":5123,\"count\":6,\"type\":\"SCALAR\"}],"
             << "\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,\"byteLength\":48},{\"buffer\":0,\"byteOffset\":48,\"byteLength\":48},"
             << "{\"buffer\":0,\"byteOffset\":96,\"byteLength\":32},{\"buffer\":0,\"byteOffset\":128,\"byteLength\":12}";
        if (options.Textured && options.Glb)
            json << ",{\"buffer\":0,\"byteOffset\":140,\"byteLength\":" << pngLength << "}";
        json << "],\"buffers\":[{" << (options.Glb ? "" : "\"uri\":\"" + options.BufferUri + "\",") << "\"byteLength\":" << bufferLength << "}]";
        if (!options.RequiredExtension.empty())
            json << ",\"extensionsRequired\":[\"" << options.RequiredExtension << "\"],\"extensionsUsed\":[\"" << options.RequiredExtension << "\"]";
        json << "}";
        return json.str();
    }

    Bytes MakeGlb(const PackageOptions& options)
    {
        Bytes bin = GeometryBin();
        const Bytes png = options.Textured ? MakePng(options.Variant, options.CorruptPng) : Bytes();
        bin.insert(bin.end(), png.begin(), png.end());
        while (bin.size() % 4 != 0)
            bin.push_back(0);
        PackageOptions glb = options;
        glb.Glb = true;
        Bytes jsonChunk = ToBytes(GltfJson(glb, bin.size(), png.size()));
        while (jsonChunk.size() % 4 != 0)
            jsonChunk.push_back(' ');
        Bytes out { 'g', 'l', 'T', 'F' };
        const auto putLe32 = [&out](u32 value)
        {
            for (int shift = 0; shift < 32; shift += 8)
                out.push_back(static_cast<u8>(value >> shift));
        };
        putLe32(2);
        putLe32(static_cast<u32>(12 + 8 + jsonChunk.size() + 8 + bin.size()));
        putLe32(static_cast<u32>(jsonChunk.size()));
        out.insert(out.end(), { 'J', 'S', 'O', 'N' });
        out.insert(out.end(), jsonChunk.begin(), jsonChunk.end());
        putLe32(static_cast<u32>(bin.size()));
        out.insert(out.end(), { 'B', 'I', 'N', 0 });
        out.insert(out.end(), bin.begin(), bin.end());
        return out;
    }

    // model.gltf + model.bin (+ base.png when textured).
    std::vector<std::pair<std::string, Bytes>> MakeSplitFiles(const PackageOptions& options)
    {
        PackageOptions split = options;
        split.Glb = false;
        std::vector<std::pair<std::string, Bytes>> files;
        files.emplace_back("model.gltf", ToBytes(GltfJson(split, 140, 0)));
        files.emplace_back(split.BufferUri, GeometryBin());
        if (split.Textured)
            files.emplace_back(split.ImageUri, MakePng(split.Variant, split.CorruptPng));
        return files;
    }

    bool WriteFolder(const fs::path& folder, const std::vector<std::pair<std::string, Bytes>>& files)
    {
        for (const auto& [name, bytes] : files)
            if (!WriteBytes(folder / name, bytes))
                return false;
        return true;
    }

    struct ZipMember
    {
        std::string Name;
        Bytes Data;
        int Level = MZ_DEFAULT_LEVEL;
    };

    Bytes BuildZip(const std::vector<ZipMember>& members)
    {
        mz_zip_archive writer {};
        Bytes result;
        if (!mz_zip_writer_init_heap(&writer, 0, 0))
            return result;
        bool built = true;
        for (const ZipMember& member : members)
            built = built && mz_zip_writer_add_mem(&writer, member.Name.c_str(), member.Data.data(), member.Data.size(),
                                 static_cast<mz_uint>(member.Level));
        void* buffer = nullptr;
        size_t size = 0;
        if (built && mz_zip_writer_finalize_heap_archive(&writer, &buffer, &size))
            result.assign(static_cast<const u8*>(buffer), static_cast<const u8*>(buffer) + size);
        mz_zip_writer_end(&writer);
        if (buffer)
            mz_free(buffer);
        return result;
    }

    std::vector<ZipMember> SplitZipMembers(const PackageOptions& options, int level = MZ_DEFAULT_LEVEL)
    {
        std::vector<ZipMember> members;
        for (auto& [name, bytes] : MakeSplitFiles(options))
            members.push_back({ "pkg/" + name, std::move(bytes), level });
        return members;
    }

    // ------------------------------------------------------------------ project fixture
    struct BaseHandles
    {
        AssetHandle Mesh = kInvalidAssetHandle;
        AssetHandle Material = kInvalidAssetHandle;
        Entity Cube;
    };

    bool CreateBaseProject(const Workspace& workspace, BaseHandles& handles)
    {
        AssetRegistry registry;
        handles.Mesh = registry.RegisterAsset(AssetType::Mesh, "Engine/Generated/PrototypeCube.mesh", "Cube");
        handles.Material = registry.RegisterAsset(AssetType::Material, "Assets/Materials/Base.spiralmat", "Base");
        Scene scene("Base Scene");
        handles.Cube = scene.CreateEntity("Cube");
        MeshRendererComponent renderer;
        renderer.MeshAsset = handles.Mesh;
        renderer.MaterialAsset = handles.Material;
        renderer.MeshName = "Cube";
        scene.AddMeshRendererComponent(handles.Cube, renderer);
        return scene.SaveToFile(workspace.Project() / "Scenes" / "Main.spiral")
            && registry.SaveToFile(workspace.Project() / "Assets" / "assets.spiralassets")
            && WriteText(workspace.Manifest(),
                "SpiralProject 6\nScene \"Scenes/Main.spiral\"\nAssetRegistry \"Assets/assets.spiralassets\"\n"
                "FramePacingMode Responsive\nFramePacingTargetFps 60\nPresentationPolicy Synchronized\n"
                "ManualExposureEV100 0\nPostToneMapSaturation 1\nPostToneMapContrast 1\n"
                "ExposureMode ManualEV100\nCameraApertureFNumber 1\nCameraShutterSeconds 1\nCameraISO 100\n");
    }

    bool ReadContext(const Workspace& workspace, FabImportProjectContext& context)
    {
        context = {};
        context.ProjectRoot = workspace.Project();
        context.ManifestRelativePath = kManifestName;
        context.ManifestBytes = ReadAll(workspace.Manifest());
        ProjectManifest manifest;
        std::string error;
        if (!DeserializeProjectManifest(context.ManifestBytes, manifest, error)
            || !context.Registry.LoadFromFile(workspace.Project() / fs::path(manifest.AssetRegistryPath)))
            return false;
        if (!manifest.FabReceiptsPath.empty()
            && !LoadFabReceiptCollection(workspace.Project() / fs::path(manifest.FabReceiptsPath), context.Receipts, error))
            return false;
        context.SceneBytes = ReadAll(workspace.Project() / fs::path(manifest.ScenePath));
        return !context.SceneBytes.empty();
    }

    FabProvenance MakeProvenance(std::string_view listing = kListingA, std::string_view version = "1.0")
    {
        FabProvenance provenance;
        provenance.ProductIdentity = std::string(listing);
        provenance.ProductName = "Synthetic Statue";
        provenance.Publisher = "Test Publisher";
        provenance.VersionOrDownloadLabel = std::string(version);
        provenance.LicenseFamily = FabLicenseFamily::FabStandard;
        provenance.LicenseTier = FabLicenseTier::Personal;
        provenance.NoAI = FabMetadataFlag::No;
        provenance.GeneratedWithAI = FabMetadataFlag::No;
        return provenance;
    }

    // ------------------------------------------------------------------ driving the controller
    FabImportControllerConfig MakeConfig(const Workspace& workspace)
    {
        FabImportControllerConfig config;
        config.PackageStagingRoot = workspace.PackageStaging();
        return config;
    }

    bool Settle(FabImportController& controller, int timeoutMs = 120000)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        for (;;)
        {
            controller.Update();
            if (!controller.IsBusy())
            {
                controller.Update();
                return true;
            }
            if (std::chrono::steady_clock::now() > deadline)
                return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    // Bypasses PlanFabIntake so the worker's own re-classification is exercised too.
    FabIntakeRequest RawRequest(const fs::path& path, FabIntakeKind kind)
    {
        FabIntakeRequest request;
        request.Path = path;
        request.Origin = FabIntakeOrigin::Typed;
        request.Kind = kind;
        return request;
    }

    bool SubmitPlanned(FabImportController& controller, const fs::path& path)
    {
        const std::vector<std::string> paths { path.string() };
        const FabIntakePlan plan = PlanFabIntake(FabIntakeOrigin::Typed, paths);
        return plan.Accepted.size() == 1 && controller.Submit(plan.Accepted.front());
    }

    // Submit -> AwaitingProvenance -> provenance -> Cooking -> ReadyToCommit.
    bool DriveToReady(FabImportController& controller, const Workspace& workspace, const fs::path& source,
        const FabProvenance& provenance, std::string& why)
    {
        if (!SubmitPlanned(controller, source) || !Settle(controller))
        {
            why = "submit or snapshot settle failed";
            return false;
        }
        FabImportStatus status = controller.GetStatus();
        if (status.State != FabImportState::AwaitingProvenance)
        {
            why = std::string("expected AwaitingProvenance, saw ") + ToString(status.State) + ": " + status.Message;
            return false;
        }
        FabImportProjectContext context;
        if (!controller.SetProvenance(provenance) || !ReadContext(workspace, context)
            || !controller.ConfirmProvenance(context, controller.GetStatus().ProvenanceDigest))
        {
            why = "provenance rejected: " + controller.GetStatus().LastRejection;
            return false;
        }
        if (!Settle(controller))
        {
            why = "cook did not settle";
            return false;
        }
        status = controller.GetStatus();
        if (status.State != FabImportState::ReadyToCommit)
        {
            why = std::string("expected ReadyToCommit, saw ") + ToString(status.State) + ": " + status.Message;
            return false;
        }
        return true;
    }

    bool CommitFresh(FabImportController& controller, const Workspace& workspace, std::optional<FabAssignmentTarget> assignment = {})
    {
        FabImportProjectContext context;
        if (!ReadContext(workspace, context))
            return false;
        context.Assignment = assignment;
        return controller.Commit(context);
    }

    bool NoStagingLeaks(const Workspace& workspace)
    {
        std::error_code error;
        return CountEntries(workspace.PackageStaging()) == 0 && !fs::exists(workspace.CookStaging(), error);
    }

    bool ControllerIsSupported()
    {
        return IsFabStagingSupported() && IsProjectCommitSupported();
    }

    // Non-Linux hosts fail closed: the import is refused as Unsupported and nothing is created.
    bool CheckUnsupportedPlatformFailsClosed(Checker& check)
    {
        Workspace workspace;
        BaseHandles base;
        check(workspace.IsReady() && CreateBaseProject(workspace, base), "unsupported platform: workspace");
        check(WriteBytes(workspace.Input("p.glb"), MakeGlb({})), "unsupported platform: fixture");
        if (!check.Passed)
            return false;
        const TreeState before = TakeTree(workspace.Project());
        ScopedJobSystem jobs;
        FabImportController controller(MakeConfig(workspace));
        check(controller.Submit(RawRequest(workspace.Input("p.glb"), FabIntakeKind::Glb)) && Settle(controller)
                && controller.GetStatus().State == FabImportState::Failed
                && controller.GetStatus().Error == FabImportError::Unsupported,
            "unsupported platform: the import fails closed as Unsupported");
        check(TakeTree(workspace.Project()) == before && CountEntries(workspace.PackageStaging()) == 0,
            "unsupported platform: nothing is created");
        return check.Passed;
    }

    // Independent post-commit verification through the real loaders.
    void VerifyCommittedProject(Checker& check, const Workspace& workspace, const FabImportController& controller,
        u64 expectedRevision, const std::string& label, bool checkLeaks = true)
    {
        const FabImportStatus status = controller.GetStatus();
        const FabImportCommitResult* result = controller.GetCommitResult();
        check(status.State == FabImportState::Done && result != nullptr, label + ": the import is Done with a commit result");
        if (!result)
            return;
        std::string error;
        FabProjectState state;
        FabProjectValidationOptions options;
        options.Level = FabProjectValidationLevel::FullHash;
        check(LoadFabProjectState(workspace.Project(), kManifestName, options, state, error),
            label + ": the committed project passes full-hash validation: " + error);
        check(state.Manifest.ProjectRevision == expectedRevision, label + ": the manifest revision is " + std::to_string(expectedRevision));
        check(result->Manifest == state.Manifest, label + ": the result manifest is the committed manifest");
        const FabImportReceipt* receipt = nullptr;
        for (const FabImportReceipt& candidate : state.Receipts.Receipts)
            if (candidate.GenerationId == status.GenerationId)
                receipt = &candidate;
        check(receipt != nullptr && receipt->StreamId == status.StreamId && receipt->MetadataConfirmedByUser,
            label + ": the receipt for the generation is on disk and user-confirmed");
        const AssetMetadata* mesh = state.Registry.GetAsset(result->MeshAsset);
        check(mesh && mesh->SourcePolicy == AssetSourcePolicy::ImmutablePackage && mesh->CookedRoot == "fab/" + status.GenerationId,
            label + ": the mesh is registered as an immutable asset of the generation");
        MeshArtifact artifact;
        check(ResolveMeshArtifact(state.Registry, result->MeshAsset, artifact, error) && artifact.Vertices.size() == 4
                && artifact.Indices.size() == 6,
            label + ": the committed mesh resolves from disk with the quad's four vertices and six indices: " + error);
        check(ReadAll(workspace.Project() / fs::path(state.Manifest.ScenePath)) == result->SceneBytes,
            label + ": the result Scene bytes are the committed Scene file");
        check(result->Registry.GetAssets().size() == state.Registry.GetAssets().size() && result->Receipts == state.Receipts,
            label + ": the result registry and receipts equal the committed ones");
        check(result->Commit.UndoBarrier.ClearUndoRedoHistory && result->Commit.UndoBarrier.NewProjectRevision == expectedRevision,
            label + ": the undo barrier data names the new revision");
        check(!checkLeaks || NoStagingLeaks(workspace), label + ": no staging directory is left behind");
    }

}

namespace SpiralTests
{
    // ================================================================== ZIP staging sink
    bool TestFabZipStagingSinkWritesOwnerOnlyExactTrees()
    {
        Checker check { true, "Fab ZIP staging sink test failed" };
        Workspace workspace;
        check(workspace.IsReady(), "workspace is created");
        if (!check.Passed)
            return false;
        if (!IsFabStagingSupported())
        {
            FabStagingDirectory directory;
            std::string error;
            check(!PrepareFabStagingRoot(workspace.Base() / "root", error) && !error.empty()
                    && !FabStagingDirectory::Create(workspace.Base(), "x", directory, error),
                "an unsupported platform fails closed");
            return check.Passed;
        }

        std::string error;
        const fs::path root = workspace.Base() / "deep" / "staging-root";
        check(PrepareFabStagingRoot(root, error), "the staging root is created: " + error);
#if defined(__linux__)
        struct stat status {};
        check(::stat(root.c_str(), &status) == 0 && (status.st_mode & 0777) == 0700, "the staging root is mode 0700");
        const fs::path loose = workspace.Base() / "loose";
        fs::create_directories(loose);
        ::chmod(loose.c_str(), 0755);
        check(!PrepareFabStagingRoot(loose, error), "a group/other-accessible root is refused");
        check(::stat(loose.c_str(), &status) == 0 && (status.st_mode & 0777) == 0755, "a refused root is not chmod-ed");
        const fs::path linkRoot = workspace.Base() / "link-root";
        fs::create_directory_symlink(root, linkRoot);
        check(!PrepareFabStagingRoot(linkRoot, error), "a symlinked final root component is refused");
#endif

        Bytes pattern(300000);
        u32 state = 7;
        for (u8& byte : pattern)
        {
            state = state * 1664525u + 1013904223u;
            byte = static_cast<u8>(state >> 24);
        }
        const Bytes zip = BuildZip({ { "a.txt", ToBytes("alpha") }, { "dir/b.bin", pattern, 0 },
            { "dir/sub/c.txt", ToBytes(std::string(70000, 'c')) } });
        check(!zip.empty() && WriteBytes(workspace.Input("sink.zip"), zip), "the sink fixture zip is built");

        fs::path extractedPath;
        {
            FabStagingDirectory directory;
            check(FabStagingDirectory::Create(root, "extract", directory, error), "a staging directory is created: " + error);
            extractedPath = directory.GetPath();
            u64 files = 0;
            u64 bytes = 0;
            check(ExtractFabZipToStaging(FabArchiveInput::FromFile(workspace.Input("sink.zip")), FabArchiveLimits {}, directory,
                      {}, &files, &bytes, error),
                "the archive extracts: " + error);
            check(files == 3 && bytes == 5 + pattern.size() + 70000, "the sink counted every committed member and byte");
            check(ReadAll(extractedPath / "a.txt") == "alpha" && ReadAll(extractedPath / "dir/b.bin")
                        == std::string(pattern.begin(), pattern.end())
                    && ReadAll(extractedPath / "dir/sub/c.txt") == std::string(70000, 'c'),
                "every member's bytes equal the source bytes");
#if defined(__linux__)
            check(::stat((extractedPath / "dir/b.bin").c_str(), &status) == 0 && (status.st_mode & 0777) == 0600
                    && ::stat((extractedPath / "dir/sub").c_str(), &status) == 0 && (status.st_mode & 0777) == 0700
                    && ::stat(extractedPath.c_str(), &status) == 0 && (status.st_mode & 0777) == 0700,
                "files are 0600 and directories 0700");
#endif
        }
        std::error_code filesystemError;
        check(!fs::exists(extractedPath, filesystemError), "destroying the staging directory removes exactly its tree");
        check(CountEntries(root) == 0, "nothing else was created beneath the root");

        // Corrupt member: the partial member is removed, earlier members stay for the caller to discard.
        Bytes badZip = BuildZip({ { "ok.txt", ToBytes("fine"), 0 }, { "bad.txt", ToBytes("0123456789abcdef"), 0 } });
        const size_t badOffset = 30 + std::strlen("ok.txt") + 4 + 30 + std::strlen("bad.txt") + 5;
        check(badOffset < badZip.size(), "bad-CRC fixture offset is inside the archive");
        badZip[badOffset] ^= 0x20;
        check(WriteBytes(workspace.Input("bad.zip"), badZip), "bad-CRC zip is written");
        {
            FabStagingDirectory directory;
            check(FabStagingDirectory::Create(root, "bad", directory, error), "a second staging directory is created");
            check(!ExtractFabZipToStaging(FabArchiveInput::FromFile(workspace.Input("bad.zip")), FabArchiveLimits {}, directory,
                      {}, nullptr, nullptr, error) && !error.empty(),
                "a CRC mismatch fails the extraction");
            check(!fs::exists(directory.GetPath() / "bad.txt", filesystemError), "the failed member's partial file is removed by exact inode");
        }
        check(CountEntries(root) == 0, "the failed staging directory is gone");

        // Sink protocol: exact size, create-once, hostile names, planted symlinks, cancellation.
        {
            FabStagingDirectory directory;
            check(FabStagingDirectory::Create(root, "proto", directory, error), "protocol staging directory is created");
            FabZipStagingSink sink(directory);
            check(sink.BeginFile("one.bin", 4) && sink.Write(ToBytes("abcd")) && sink.EndFile(), "a four-byte member is written");
            check(!sink.BeginFile("one.bin", 4), "an existing member is never overwritten (O_EXCL)");
            check(ReadAll(directory.GetPath() / "one.bin") == "abcd", "the existing member is unchanged");
            check(sink.BeginFile("two.bin", 4) && !sink.Write(ToBytes("abcde")), "more bytes than declared are refused");
            sink.AbortFile();
            check(!fs::exists(directory.GetPath() / "two.bin", filesystemError), "abort removes the partial member");
            check(sink.BeginFile("short.bin", 4) && sink.Write(ToBytes("ab")) && !sink.EndFile(), "fewer bytes than declared fail EndFile");
            sink.AbortFile();
            check(!fs::exists(directory.GetPath() / "short.bin", filesystemError), "abort after a short member removes it");
            for (const char* hostile : { "../x", "a/../b", "/abs", "a//b", "a\\b", "", ".", "a/./b" })
                check(!sink.BeginFile(hostile, 1), std::string("hostile member name is refused: ") + hostile);
            check(sink.CommittedFileCount() == 1 && sink.CommittedBytes() == 4, "only the one good member is counted");
#if defined(__linux__)
            const fs::path outside = workspace.Base() / "outside";
            fs::create_directories(outside);
            fs::create_directory_symlink(outside, directory.GetPath() / "planted");
            check(!sink.BeginFile("planted/escape.bin", 1) && CountEntries(outside) == 0,
                "a planted symlink directory is never followed");
#endif
            std::atomic<bool> cancelled { false };
            FabZipStagingSink cancellable(directory, [&cancelled]() { return cancelled.load(); });
            check(cancellable.BeginFile("c.bin", 4) && cancellable.Write(ToBytes("ab")), "a member starts before cancellation");
            cancelled = true;
            check(!cancellable.Write(ToBytes("cd")) && !cancellable.BeginFile("d.bin", 1), "cancellation refuses further writes and members");
            cancellable.AbortFile();
            check(!fs::exists(directory.GetPath() / "c.bin", filesystemError), "cancelled partial member is removed");
        }

        // Exact-owned cleanup: a replaced directory is not ours.
        {
            FabStagingDirectory directory;
            check(FabStagingDirectory::Create(root, "owned", directory, error), "ownership staging directory is created");
            const fs::path original = directory.GetPath();
            const fs::path moved = original.string() + ".moved";
            fs::rename(original, moved);
            fs::create_directory(original);
            WriteText(original / "stranger.txt", "not ours");
            check(directory.Remove(error) && !directory.IsValid(), "Remove succeeds when the name was replaced");
            check(fs::exists(original / "stranger.txt", filesystemError), "the replacement directory is left alone");
            check(fs::exists(moved, filesystemError), "the original inode that moved away is not touched either");
            fs::remove_all(original, filesystemError);
            fs::remove_all(moved, filesystemError);
        }

        // Regular-file copy with hashing.
        {
            check(WriteBytes(workspace.Input("copy-source.bin"), pattern), "copy source is written");
            FabStagingDirectory directory;
            check(FabStagingDirectory::Create(root, "copy", directory, error), "copy staging directory is created");
            FabStagedCopy copy;
            check(CopyRegularFileIntoStaging(workspace.Input("copy-source.bin"), directory, "source.bin", 1 << 20, {}, copy, error),
                "a regular file is copied: " + error);
            check(copy.Bytes == pattern.size() && copy.Sha256 == Sha256OfFile(workspace.Input("copy-source.bin"))
                    && Sha256OfFile(directory.GetPath() / "source.bin") == copy.Sha256,
                "the reported SHA-256 is of the exact copied bytes");
            FabStagedCopy again;
            check(!CopyRegularFileIntoStaging(workspace.Input("copy-source.bin"), directory, "source.bin", 1 << 20, {}, again, error),
                "an existing destination name is refused");
            check(Sha256OfFile(directory.GetPath() / "source.bin") == copy.Sha256, "the refused copy did not clobber the first");
            check(!CopyRegularFileIntoStaging(workspace.Input("copy-source.bin"), directory, "small.bin", 100, {}, again, error)
                    && !fs::exists(directory.GetPath() / "small.bin", filesystemError),
                "a file over the byte limit is refused without leaving a partial copy");
            check(!CopyRegularFileIntoStaging(workspace.Input("copy-source.bin"), directory, "../escape", 1 << 20, {}, again, error),
                "a non-single-segment destination name is refused");
            std::atomic<bool> cancelled { true };
            check(!CopyRegularFileIntoStaging(workspace.Input("copy-source.bin"), directory, "cancel.bin", 1 << 20,
                      [&cancelled]() { return cancelled.load(); }, again, error)
                    && error == "cancelled" && !fs::exists(directory.GetPath() / "cancel.bin", filesystemError),
                "cancellation removes the partial copy");
#if defined(__linux__)
            fs::create_symlink(workspace.Input("copy-source.bin"), workspace.Input("copy-link.bin"));
            check(!CopyRegularFileIntoStaging(workspace.Input("copy-link.bin"), directory, "link.bin", 1 << 20, {}, again, error),
                "a symlinked source is refused");
            ::mkfifo(workspace.Input("copy.fifo").c_str(), 0600);
            check(!CopyRegularFileIntoStaging(workspace.Input("copy.fifo"), directory, "fifo.bin", 1 << 20, {}, again, error),
                "a FIFO source is refused without blocking");
#endif
        }
        return check.Passed;
    }

    // ================================================================== folder, ZIP and bare GLB
    bool TestFabImportControllerImportsFolderZipAndGlbPackages()
    {
        Checker check { true, "Fab import controller intake test failed" };
        ScopedJobSystem jobs;
        if (!ControllerIsSupported())
            return CheckUnsupportedPlatformFailsClosed(check);
        PackageOptions options;
        const Bytes glb = MakeGlb(options);

        struct Case
        {
            const char* Label;
            FabIntakeKind Kind;
            bool ArchiveHash;
        };
        for (const Case& item : { Case { "folder", FabIntakeKind::Folder, false }, Case { "zip", FabIntakeKind::Zip, true },
                 Case { "glb", FabIntakeKind::Glb, false } })
        {
            Workspace workspace;
            BaseHandles base;
            check(workspace.IsReady() && CreateBaseProject(workspace, base), std::string(item.Label) + ": workspace and base project");
            if (!check.Passed)
                return false;
            const TreeState before = TakeTree(workspace.Project());

            fs::path source;
            if (item.Kind == FabIntakeKind::Folder)
            {
                source = workspace.Input("package");
                check(WriteFolder(source, MakeSplitFiles(options)), "folder fixture is written");
            }
            else if (item.Kind == FabIntakeKind::Zip)
            {
                source = workspace.Input("package.zip");
                check(WriteBytes(source, BuildZip(SplitZipMembers(options))), "zip fixture is written");
            }
            else
            {
                source = workspace.Input("My Model (final).glb");  // the leaf name is irrelevant to the importer
                check(WriteBytes(source, glb), "glb fixture is written");
            }

            FabImportController controller(MakeConfig(workspace));
            std::string why;
            const FabProvenance provenance = MakeProvenance();
            check(SubmitPlanned(controller, source), std::string(item.Label) + ": the package is accepted for intake");
            const FabImportStatus submitted = controller.GetStatus();
            check(submitted.State == FabImportState::Snapshotting && submitted.SourceKind == item.Kind
                    && !submitted.SourceName.empty(),
                std::string(item.Label) + ": Submit enters Snapshotting immediately");
            check(!SubmitPlanned(controller, source) && !controller.GetStatus().LastRejection.empty(),
                std::string(item.Label) + ": a second submission while active is refused");
            check(Settle(controller), std::string(item.Label) + ": the snapshot phase settles");
            FabImportStatus status = controller.GetStatus();
            check(status.State == FabImportState::AwaitingProvenance, std::string(item.Label) + ": reaches AwaitingProvenance: " + status.Message);
            check(status.Format == (item.Kind == FabIntakeKind::Glb ? FabPackageFormat::Glb : FabPackageFormat::Gltf),
                std::string(item.Label) + ": the format comes from the snapshot root");
            check(status.Summary.VertexCount == 4 && status.Summary.TriangleCount == 2 && status.Summary.PrimitiveInstanceCount == 1
                    && status.Summary.Textures.size() == 1 && status.Summary.Textures[0].Width == 2
                    && status.Summary.Textures[0].Height == 2 && status.Summary.SourceFileCount >= 1,
                std::string(item.Label) + ": the provenance-independent summary matches the fixture's quad and texture");
            if (item.ArchiveHash)
                check(status.SourceSha256 == Sha256OfFile(source) && status.SourceSha256 != status.ExpandedTreeSha256,
                    "zip: the receipt source digest is the archive's SHA-256");
            else
                check(status.SourceSha256 == status.ExpandedTreeSha256 && !status.SourceSha256.empty(),
                    std::string(item.Label) + ": folder and bare-file source digests are the snapshot tree digest");
            check(!status.ProvenanceValid && !status.ProvenanceConfirmed && !status.ProvenanceError.empty(),
                std::string(item.Label) + ": provenance starts invalid and unconfirmed");
            check(TakeTree(workspace.Project()) == before, std::string(item.Label) + ": the project is untouched before commit");
            check(CountEntries(workspace.PackageStaging()) == 1, std::string(item.Label) + ": the retained snapshot lives in private staging");

            FabImportProjectContext context;
            check(ReadContext(workspace, context), "context is read");
            check(controller.SetProvenance(provenance), "provenance is stored");
            status = controller.GetStatus();
            check(status.ProvenanceValid && !status.ProvenanceConfirmed && status.ProvenanceDigest.size() == 64,
                std::string(item.Label) + ": valid but unconfirmed provenance has a digest");
            check(!controller.Commit(context) && controller.GetStatus().State == FabImportState::AwaitingProvenance,
                std::string(item.Label) + ": commit before cooking is refused");
            check(controller.ConfirmProvenance(context, status.ProvenanceDigest) && controller.GetStatus().State == FabImportState::Cooking,
                std::string(item.Label) + ": the confirmation starts cooking");
            check(Settle(controller), "the cook phase settles");
            status = controller.GetStatus();
            check(status.State == FabImportState::ReadyToCommit && status.HasDecision
                    && status.Decision == FabReceiptDecisionKind::AddNewStream && status.ProvenanceConfirmed,
                std::string(item.Label) + ": cooking ends in ReadyToCommit as a new stream: " + status.Message);
            const std::string streamId = ComputeFabStreamId(provenance.ProductIdentity, provenance.VersionOrDownloadLabel, status.Format);
            check(status.StreamId == streamId && status.GenerationId == ComputeFabGenerationId(streamId, status.SourceSha256, status.ExpandedTreeSha256),
                std::string(item.Label) + ": stream and generation identities derive from the confirmed provenance and measured digests");
            check(TakeTree(workspace.Project()) == before || TakeTree(workspace.Project(), ".fab-staging") == before,
                std::string(item.Label) + ": only the private cook staging exists before commit");
            check(CountEntries(workspace.PackageStaging()) == 0, std::string(item.Label) + ": raw package staging is released after cooking");

            check(CommitFresh(controller, workspace), std::string(item.Label) + ": the commit succeeds: " + controller.GetStatus().Message);
            VerifyCommittedProject(check, workspace, controller, 1, item.Label);
            check(controller.GetStatus().ProjectChanged && controller.GetStatus().ProjectRevision == 1,
                std::string(item.Label) + ": the status reports the committed revision");
            check(controller.Dismiss() && controller.GetStatus().State == FabImportState::Idle && controller.GetCommitResult() == nullptr,
                std::string(item.Label) + ": dismiss returns to Idle");
        }

        // Untextured GLB: the generation has no texture directory content and must still commit and verify.
        {
            Workspace workspace;
            BaseHandles base;
            check(workspace.IsReady() && CreateBaseProject(workspace, base), "untextured: workspace");
            PackageOptions plain;
            plain.Textured = false;
            check(WriteBytes(workspace.Input("plain.glb"), MakeGlb(plain)), "untextured glb is written");
            FabImportController controller(MakeConfig(workspace));
            std::string why;
            check(DriveToReady(controller, workspace, workspace.Input("plain.glb"), MakeProvenance(), why), "untextured: " + why);
            check(CommitFresh(controller, workspace), "untextured: the commit succeeds: " + controller.GetStatus().Message);
            VerifyCommittedProject(check, workspace, controller, 1, "untextured");
        }

        // With no worker pool the job system runs a phase inline inside Submit; the controller must
        // publish that finished phase through Update exactly as it does for a real worker.
        {
            JobSystem& jobSystem = JobSystem::Get();
            const bool wasRunning = jobSystem.IsRunning();
            jobSystem.Shutdown();
            Workspace workspace;
            BaseHandles base;
            check(workspace.IsReady() && CreateBaseProject(workspace, base), "inline: workspace");
            check(WriteBytes(workspace.Input("p.zip"), BuildZip(SplitZipMembers({}))), "inline: fixture");
            FabImportController controller(MakeConfig(workspace));
            std::string why;
            check(DriveToReady(controller, workspace, workspace.Input("p.zip"), MakeProvenance(), why), "inline: " + why);
            check(CommitFresh(controller, workspace), "inline: the commit succeeds");
            VerifyCommittedProject(check, workspace, controller, 1, "inline");
            if (wasRunning)
                jobSystem.Initialize(2);
        }
        return check.Passed;
    }

    // ================================================================== identity outcomes
    bool TestFabImportControllerClassifiesReimportReplacementAndConflict()
    {
        Checker check { true, "Fab import controller classification test failed" };
        ScopedJobSystem jobs;
        if (!ControllerIsSupported())
            return CheckUnsupportedPlatformFailsClosed(check);
        Workspace workspace;
        BaseHandles base;
        check(workspace.IsReady() && CreateBaseProject(workspace, base), "workspace and base project");
        if (!check.Passed)
            return false;
        PackageOptions original;
        PackageOptions changed;
        changed.Variant = 5;
        check(WriteFolder(workspace.Input("original"), MakeSplitFiles(original)), "original fixture");
        check(WriteFolder(workspace.Input("changed"), MakeSplitFiles(changed)), "changed fixture");
        FabImportController controller(MakeConfig(workspace));
        std::string why;

        check(DriveToReady(controller, workspace, workspace.Input("original"), MakeProvenance(), why) && CommitFresh(controller, workspace),
            "first import commits: " + why);
        VerifyCommittedProject(check, workspace, controller, 1, "first import");
        const std::string firstGeneration = controller.GetStatus().GenerationId;
        const std::string firstStream = controller.GetStatus().StreamId;
        const AssetHandle firstMesh = controller.GetCommitResult() ? controller.GetCommitResult()->MeshAsset : kInvalidAssetHandle;
        controller.Dismiss();
        const TreeState afterFirst = TakeTree(workspace.Project());

        // Identical reimport -> ExactReuse: no new commit, byte-identical project.
        check(DriveToReady(controller, workspace, workspace.Input("original"), MakeProvenance(), why), "identical reimport reaches ReadyToCommit: " + why);
        check(controller.GetStatus().Decision == FabReceiptDecisionKind::ExactReuse && controller.GetStatus().GenerationId == firstGeneration,
            "identical reimport is classified ExactReuse of the same generation");
        check(CommitFresh(controller, workspace) && controller.GetStatus().State == FabImportState::Done, "exact reuse completes");
        check(!controller.GetStatus().ProjectChanged && controller.GetCommitResult() && !controller.GetCommitResult()->ProjectChanged
                && controller.GetCommitResult()->Decision == FabReceiptDecisionKind::ExactReuse,
            "exact reuse reports no project change");
        check(TakeTree(workspace.Project()) == afterFirst, "exact reuse leaves the entire project byte-identical (no new commit, no new generation)");
        check(NoStagingLeaks(workspace), "exact reuse leaves no staging");
        controller.Dismiss();

        // Changed bytes under the same listing and version -> same-stream replacement, old generation retained.
        check(DriveToReady(controller, workspace, workspace.Input("changed"), MakeProvenance(), why), "changed source reaches ReadyToCommit: " + why);
        check(controller.GetStatus().Decision == FabReceiptDecisionKind::ReplaceSameStreamSource && controller.GetStatus().StreamId == firstStream
                && controller.GetStatus().GenerationId != firstGeneration,
            "changed source is a replacement within the same stream");
        check(CommitFresh(controller, workspace), "replacement commits: " + controller.GetStatus().Message);
        VerifyCommittedProject(check, workspace, controller, 2, "replacement");
        check(controller.GetCommitResult() && controller.GetCommitResult()->MeshAsset == firstMesh,
            "replacement keeps the stable mesh handle");
        const std::string secondGeneration = controller.GetStatus().GenerationId;
        std::error_code error;
        check(fs::exists(workspace.Project() / "Assets" / "fab" / firstGeneration, error)
                && fs::exists(workspace.Project() / "Assets" / "fab" / secondGeneration, error),
            "the replaced generation directory is retained beside the new one");
        {
            FabProjectState state;
            std::string loadError;
            FabProjectValidationOptions options;
            options.Level = FabProjectValidationLevel::FullHash;
            check(LoadFabProjectState(workspace.Project(), kManifestName, options, state, loadError), "project validates after replacement: " + loadError);
            const AssetMetadata* mesh = state.Registry.GetAsset(firstMesh);
            check(mesh && mesh->CookedRoot == "fab/" + secondGeneration, "the registry now points at the replacement generation");
        }
        controller.Dismiss();

        // New version label under the same listing -> a product-update stream.
        check(DriveToReady(controller, workspace, workspace.Input("original"), MakeProvenance(kListingA, "2.0"), why),
            "product update reaches ReadyToCommit: " + why);
        check(controller.GetStatus().Decision == FabReceiptDecisionKind::AddProductUpdateStream && controller.GetStatus().StreamId != firstStream,
            "a new version label is a product update stream");
        check(CommitFresh(controller, workspace), "product update commits");
        VerifyCommittedProject(check, workspace, controller, 3, "product update");
        controller.Dismiss();

        // A different listing is simply a new stream.
        check(DriveToReady(controller, workspace, workspace.Input("original"), MakeProvenance(kListingB, "1.0"), why),
            "a second listing reaches ReadyToCommit: " + why);
        check(controller.GetStatus().Decision == FabReceiptDecisionKind::AddNewStream, "a different listing is a new stream");
        check(CommitFresh(controller, workspace), "second listing commits");
        VerifyCommittedProject(check, workspace, controller, 4, "second listing");
        controller.Dismiss();

        // Same identity with different confirmed provenance is a conflict, surfaced exactly and committing nothing.
        const TreeState beforeConflict = TakeTree(workspace.Project());
        FabProvenance different = MakeProvenance(kListingB, "1.0");
        different.LicenseTier = FabLicenseTier::Professional;
        check(!DriveToReady(controller, workspace, workspace.Input("original"), different, why), "a conflicting reimport does not reach ReadyToCommit");
        FabImportStatus status = controller.GetStatus();
        check(status.State == FabImportState::Failed && status.Error == FabImportError::Conflict && status.HasDecision
                && status.Decision == FabReceiptDecisionKind::Conflict && !status.DecisionDiagnostic.empty(),
            std::string("the conflict is surfaced as Conflict, saw ") + ToString(status.State) + "/" + ToString(status.Error) + ": " + status.Message);
        check(TakeTree(workspace.Project()) == beforeConflict && NoStagingLeaks(workspace), "the conflict leaves the project and staging untouched");
        return check.Passed;
    }

    // ================================================================== hostile input
    namespace
    {
        struct HostileCase
        {
            std::string Label;
            std::function<bool(const Workspace&, fs::path&, FabIntakeKind&, FabImportControllerConfig&)> Build;
            std::set<FabImportError> Accepted;
        };
    }

    bool TestFabImportControllerRejectsHostilePackagesWithoutChangingTheProject()
    {
        Checker check { true, "Fab import controller hostile input test failed" };
        ScopedJobSystem jobs;
        if (!ControllerIsSupported())
            return CheckUnsupportedPlatformFailsClosed(check);
        Workspace workspace;
        BaseHandles base;
        check(workspace.IsReady() && CreateBaseProject(workspace, base), "workspace and base project");
        if (!check.Passed)
            return false;
        const PackageOptions plain;
        std::vector<HostileCase> cases;
        const auto zipCase = [&cases](std::string label, std::function<Bytes()> bytes, std::set<FabImportError> accepted,
                                 std::function<void(FabImportControllerConfig&)> tweak = {})
        {
            cases.push_back({ std::move(label),
                [bytes, tweak](const Workspace& w, fs::path& path, FabIntakeKind& kind, FabImportControllerConfig& config)
                {
                    path = w.Input("hostile.zip");
                    kind = FabIntakeKind::Zip;
                    if (tweak)
                        tweak(config);
                    return WriteBytes(path, bytes());
                },
                std::move(accepted) });
        };
        const auto folderCase = [&cases](std::string label, std::function<bool(const fs::path&)> build, std::set<FabImportError> accepted,
                                    std::function<void(FabImportControllerConfig&)> tweak = {})
        {
            cases.push_back({ std::move(label),
                [build, tweak](const Workspace& w, fs::path& path, FabIntakeKind& kind, FabImportControllerConfig& config)
                {
                    path = w.Input("hostile-folder");
                    kind = FabIntakeKind::Folder;
                    if (tweak)
                        tweak(config);
                    return build(path);
                },
                std::move(accepted) });
        };
        const auto glbCase = [&cases](std::string label, std::function<Bytes()> bytes, std::set<FabImportError> accepted)
        {
            cases.push_back({ std::move(label),
                [bytes](const Workspace& w, fs::path& path, FabIntakeKind& kind, FabImportControllerConfig&)
                {
                    path = w.Input("hostile.glb");
                    kind = FabIntakeKind::Glb;
                    return WriteBytes(path, bytes());
                },
                std::move(accepted) });
        };

        zipCase("truncated zip", [&]() { Bytes zip = BuildZip(SplitZipMembers(plain)); zip.resize(zip.size() - 16); return zip; },
            { FabImportError::InvalidIntake });
        zipCase("zip bad member CRC", [&]()
            {
                Bytes zip = BuildZip(SplitZipMembers(plain, MZ_NO_COMPRESSION));
                zip[30 + std::strlen("pkg/model.gltf") + 20] ^= 0x01;
                return zip;
            },
            { FabImportError::ArchiveRejected });
        zipCase("zip parent traversal", [&]() { auto m = SplitZipMembers(plain); m.push_back({ "../evil.txt", ToBytes("x") }); return BuildZip(m); },
            { FabImportError::ArchiveRejected });
        zipCase("zip nested traversal", [&]() { auto m = SplitZipMembers(plain); m.push_back({ "pkg/../../evil.txt", ToBytes("x") }); return BuildZip(m); },
            { FabImportError::ArchiveRejected });
        zipCase("zip absolute member", [&]() { auto m = SplitZipMembers(plain); m.push_back({ "/tmp/evil.txt", ToBytes("x") }); return BuildZip(m); },
            { FabImportError::InvalidIntake });
        zipCase("zip backslash member", [&]() { auto m = SplitZipMembers(plain); m.push_back({ "pkg\\evil.txt", ToBytes("x") }); return BuildZip(m); },
            { FabImportError::ArchiveRejected });
        zipCase("zip duplicate member", [&]() { auto m = SplitZipMembers(plain); m.push_back(m.front()); return BuildZip(m); },
            { FabImportError::ArchiveRejected });
        zipCase("zip case-fold duplicate", [&]() { auto m = SplitZipMembers(plain); m.push_back({ "PKG/MODEL.BIN", ToBytes("x") }); return BuildZip(m); },
            { FabImportError::ArchiveRejected });
        zipCase("zip embedded executable", [&]()
            {
                auto m = SplitZipMembers(plain);
                Bytes elf { 0x7f, 'E', 'L', 'F', 2, 1, 1, 0 };
                elf.resize(64, 0);
                m.push_back({ "pkg/tool.bin", elf });
                return BuildZip(m);
            },
            { FabImportError::ArchiveRejected });
        zipCase("zip nested archive", [&]()
            {
                auto m = SplitZipMembers(plain);
                m.push_back({ "pkg/inner.dat", BuildZip({ { "a.txt", ToBytes("a") } }) });
                return BuildZip(m);
            },
            { FabImportError::ArchiveRejected });
        zipCase("empty zip", []() { return BuildZip({}); }, { FabImportError::SnapshotRejected });
        zipCase("zip member over the byte limit", [&]() { return BuildZip(SplitZipMembers(plain)); }, { FabImportError::ArchiveRejected },
            [](FabImportControllerConfig& config) { config.ArchiveLimits.MaximumFileBytes = 64; });
        zipCase("zip over the file-count limit", [&]() { return BuildZip(SplitZipMembers(plain)); }, { FabImportError::ArchiveRejected },
            [](FabImportControllerConfig& config) { config.ArchiveLimits.MaximumFileCount = 2; });
        zipCase("zip without a glTF root", []() { return BuildZip({ { "pkg/readme.txt", ToBytes("nothing here") } }); },
            { FabImportError::SnapshotRejected });
        zipCase("zip with missing buffer", [&]()
            {
                auto m = SplitZipMembers(plain);
                m.erase(m.begin() + 1);
                return BuildZip(m);
            },
            { FabImportError::SnapshotRejected });
        zipCase("zip with a corrupt PNG chunk", [&]()
            {
                PackageOptions bad;
                bad.CorruptPng = true;
                return BuildZip(SplitZipMembers(bad));
            },
            { FabImportError::PackageRejected });

        folderCase("folder missing the buffer file", [&](const fs::path& p)
            {
                auto files = MakeSplitFiles(plain);
                files.erase(files.begin() + 1);
                return WriteFolder(p, files);
            },
            { FabImportError::SnapshotRejected });
        folderCase("folder missing the texture file", [&](const fs::path& p)
            {
                auto files = MakeSplitFiles(plain);
                files.pop_back();
                return WriteFolder(p, files);
            },
            { FabImportError::SnapshotRejected });
        folderCase("folder with a buffer uri that escapes", [&](const fs::path& p)
            {
                PackageOptions escape;
                escape.BufferUri = "../outside.bin";
                return WriteFolder(p, MakeSplitFiles(escape));
            },
            { FabImportError::SnapshotRejected });
        folderCase("folder with a percent-encoded traversal uri", [&](const fs::path& p)
            {
                PackageOptions escape;
                escape.BufferUri = "..%2foutside.bin";
                return WriteFolder(p, MakeSplitFiles(escape));
            },
            { FabImportError::SnapshotRejected });
        folderCase("folder with a remote image uri", [&](const fs::path& p)
            {
                PackageOptions remote;
                remote.ImageUri = "https://example.com/base.png";
                return WriteFolder(p, MakeSplitFiles(remote));
            },
            { FabImportError::SnapshotRejected });
#if defined(__linux__)
        folderCase("folder with a symlinked texture", [&](const fs::path& p)
            {
                auto files = MakeSplitFiles(plain);
                files.pop_back();
                if (!WriteFolder(p, files) || !WriteBytes(p / "real.png", MakePng(0)))
                    return false;
                std::error_code error;
                fs::create_symlink("real.png", p / "base.png", error);
                return !error;
            },
            { FabImportError::SnapshotRejected });
#endif
        folderCase("folder over the aggregate byte limit", [&](const fs::path& p) { return WriteFolder(p, MakeSplitFiles(plain)); },
            { FabImportError::SnapshotRejected },
            [](FabImportControllerConfig& config) { config.SnapshotLimits.MaximumAggregateBytes = 100; });
        folderCase("folder over the file-count limit", [&](const fs::path& p) { return WriteFolder(p, MakeSplitFiles(plain)); },
            { FabImportError::SnapshotRejected },
            [](FabImportControllerConfig& config) { config.SnapshotLimits.MaximumFileCount = 2; });
        folderCase("folder without any glTF", [](const fs::path& p) { return WriteText(p / "readme.txt", "no model"); },
            { FabImportError::InvalidIntake });
        folderCase("folder with an unsupported required extension", [&](const fs::path& p)
            {
                PackageOptions draco;
                draco.RequiredExtension = "KHR_draco_mesh_compression";
                return WriteFolder(p, MakeSplitFiles(draco));
            },
            { FabImportError::PackageRejected });
        folderCase("folder with a decoded-image limit", [&](const fs::path& p) { return WriteFolder(p, MakeSplitFiles(plain)); },
            { FabImportError::PackageRejected },
            [](FabImportControllerConfig& config) { config.ImageLimits.MaximumPixels = 3; });
        folderCase("folder over the vertex limit", [&](const fs::path& p) { return WriteFolder(p, MakeSplitFiles(plain)); },
            { FabImportError::PackageRejected },
            [](FabImportControllerConfig& config) { config.GltfLimits.MaximumVertices = 3; });

        cases.push_back({ "loose gltf with an external buffer",
            [&plain](const Workspace& w, fs::path& path, FabIntakeKind& kind, FabImportControllerConfig&)
            {
                path = w.Input("loose.gltf");
                kind = FabIntakeKind::Gltf;
                return WriteBytes(path, MakeSplitFiles(plain).front().second);
            },
            { FabImportError::SnapshotRejected } });
        glbCase("glb with a required extension", [&]() { PackageOptions o; o.RequiredExtension = "KHR_draco_mesh_compression"; return MakeGlb(o); },
            { FabImportError::PackageRejected });
        glbCase("glb with a corrupt PNG chunk", [&]() { PackageOptions o; o.CorruptPng = true; return MakeGlb(o); }, { FabImportError::PackageRejected });
        glbCase("glb with a lying chunk length", [&]()
            {
                Bytes glb = MakeGlb(plain);
                glb[12] = 0xff;
                glb[13] = 0xff;
                return glb;
            },
            { FabImportError::InvalidIntake });
        glbCase("glb truncated mid-chunk", [&]() { Bytes glb = MakeGlb(plain); glb.resize(glb.size() / 2); return glb; },
            { FabImportError::InvalidIntake });
        glbCase("text renamed to glb", []() { return ToBytes("this is not a binary glTF container at all"); },
            { FabImportError::InvalidIntake });
        glbCase("empty glb", []() { return Bytes(); }, { FabImportError::InvalidIntake });

        // One controller serves the whole table: each rejection must leave it reusable.
        FabImportControllerConfig baseConfig = MakeConfig(workspace);
        const TreeState before = TakeTree(workspace.Project());
        for (HostileCase& item : cases)
        {
            std::error_code error;
            fs::remove_all(workspace.Inputs(), error);
            fs::create_directories(workspace.Inputs(), error);
            FabImportControllerConfig config = baseConfig;
            fs::path source;
            FabIntakeKind kind = FabIntakeKind::Unsupported;
            if (!item.Build(workspace, source, kind, config))
            {
                check(false, item.Label + ": fixture could not be built");
                continue;
            }
            FabImportController controller(config);
            const bool submitted = controller.Submit(RawRequest(source, kind));
            check(submitted, item.Label + ": the request is accepted for the worker to judge");
            check(Settle(controller), item.Label + ": the worker settles");
            const FabImportStatus status = controller.GetStatus();
            check(status.State == FabImportState::Failed && item.Accepted.contains(status.Error) && !status.Message.empty(),
                item.Label + ": rejected as one of the expected errors; saw " + ToString(status.State) + "/" + ToString(status.Error)
                    + ": " + status.Message);
            check(TakeTree(workspace.Project()) == before, item.Label + ": the accepted project is byte-identical");
            check(NoStagingLeaks(workspace), item.Label + ": no staging directory leaked");
            check(controller.GetCommitResult() == nullptr && !controller.Commit({}) && !controller.GetStatus().LastRejection.empty(),
                item.Label + ": a rejected import cannot be committed");
            check(controller.Dismiss() && controller.GetStatus().State == FabImportState::Idle, item.Label + ": the controller is reusable");
        }

        // The same controller object must also accept a good package after rejections.
        {
            FabImportController controller(baseConfig);
            const std::string badPath = workspace.Input("bad.glb").string();
            WriteText(workspace.Input("bad.glb"), "nope");
            check(controller.Submit(RawRequest(workspace.Input("bad.glb"), FabIntakeKind::Glb)) && Settle(controller)
                    && controller.GetStatus().State == FabImportState::Failed && controller.Dismiss(),
                "recovery: a rejection and dismiss");
            WriteBytes(workspace.Input("good.glb"), MakeGlb(plain));
            std::string why;
            check(DriveToReady(controller, workspace, workspace.Input("good.glb"), MakeProvenance(), why) && CommitFresh(controller, workspace),
                "recovery: the same controller imports a good package afterwards: " + why);
            VerifyCommittedProject(check, workspace, controller, 1, "recovery");
        }
        return check.Passed;
    }

    // ================================================================== provenance
    bool TestFabImportControllerValidatesAndBindsProvenance()
    {
        Checker check { true, "Fab import controller provenance test failed" };
        ScopedJobSystem jobs;

        // Pure validator table.
        struct Row
        {
            const char* Label;
            std::function<void(FabProvenance&)> Mutate;
            bool Valid;
        };
        const Row rows[] = {
            { "baseline Fab Standard Personal", [](FabProvenance&) {}, true },
            { "Professional tier", [](FabProvenance& p) { p.LicenseTier = FabLicenseTier::Professional; }, true },
            { "legacy marketplace needs no tier", [](FabProvenance& p) { p.LicenseFamily = FabLicenseFamily::LegacyUnrealMarketplace; p.LicenseTier = FabLicenseTier::NotApplicable; }, true },
            { "CC-BY with attribution", [](FabProvenance& p) { p.LicenseFamily = FabLicenseFamily::CreativeCommonsAttribution; p.LicenseTier = FabLicenseTier::NotApplicable; p.AttributionText = "Statue by Someone"; p.AttributionLink = "https://example.com/someone"; }, true },
            { "AI flags unknown", [](FabProvenance& p) { p.NoAI = FabMetadataFlag::Unknown; p.GeneratedWithAI = FabMetadataFlag::Unknown; }, true },
            { "empty URL", [](FabProvenance& p) { p.ProductIdentity.clear(); }, false },
            { "http scheme", [](FabProvenance& p) { p.ProductIdentity = "http://www.fab.com/listings/abc"; }, false },
            { "wrong host", [](FabProvenance& p) { p.ProductIdentity = "https://fab.com/listings/abc"; }, false },
            { "foreign host", [](FabProvenance& p) { p.ProductIdentity = "https://www.fab.com.evil.example/listings/abc"; }, false },
            { "no listing id", [](FabProvenance& p) { p.ProductIdentity = "https://www.fab.com/listings/"; }, false },
            { "query in URL", [](FabProvenance& p) { p.ProductIdentity = std::string(kListingA) + "?utm=x"; }, false },
            { "path suffix", [](FabProvenance& p) { p.ProductIdentity = std::string(kListingA) + "/edit"; }, false },
            { "credentials in URL", [](FabProvenance& p) { p.ProductIdentity = "https://user@www.fab.com/listings/abc"; }, false },
            { "control byte in URL", [](FabProvenance& p) { p.ProductIdentity += '\n'; }, false },
            { "other fab path", [](FabProvenance& p) { p.ProductIdentity = "https://www.fab.com/sellers/abc"; }, false },
            { "empty version", [](FabProvenance& p) { p.VersionOrDownloadLabel.clear(); }, false },
            { "padded version", [](FabProvenance& p) { p.VersionOrDownloadLabel = " 1.0"; }, false },
            { "control byte in version", [](FabProvenance& p) { p.VersionOrDownloadLabel = "1\t0"; }, false },
            { "empty publisher", [](FabProvenance& p) { p.Publisher.clear(); }, false },
            { "unknown license", [](FabProvenance& p) { p.LicenseFamily = FabLicenseFamily::Unknown; }, false },
            { "reference only license", [](FabProvenance& p) { p.LicenseFamily = FabLicenseFamily::ReferenceOnly; p.LicenseTier = FabLicenseTier::NotApplicable; }, false },
            { "Fab Standard without a tier", [](FabProvenance& p) { p.LicenseTier = FabLicenseTier::Unknown; }, false },
            { "Fab Standard with N/A tier", [](FabProvenance& p) { p.LicenseTier = FabLicenseTier::NotApplicable; }, false },
            { "CC-BY without attribution text", [](FabProvenance& p) { p.LicenseFamily = FabLicenseFamily::CreativeCommonsAttribution; p.LicenseTier = FabLicenseTier::NotApplicable; p.AttributionLink = "https://example.com/x"; }, false },
            { "CC-BY without link", [](FabProvenance& p) { p.LicenseFamily = FabLicenseFamily::CreativeCommonsAttribution; p.LicenseTier = FabLicenseTier::NotApplicable; p.AttributionText = "x"; }, false },
            { "CC-BY with insecure link", [](FabProvenance& p) { p.LicenseFamily = FabLicenseFamily::CreativeCommonsAttribution; p.LicenseTier = FabLicenseTier::NotApplicable; p.AttributionText = "x"; p.AttributionLink = "http://example.com/x"; }, false },
            { "CC-BY with a Fab tier", [](FabProvenance& p) { p.LicenseFamily = FabLicenseFamily::CreativeCommonsAttribution; p.AttributionText = "x"; p.AttributionLink = "https://example.com/x"; }, false },
            { "oversized attribution", [](FabProvenance& p) { p.AttributionText = std::string(16 * 1024 + 1, 'a'); }, false },
            { "raw source policy unknown", [](FabProvenance& p) { p.RawSourcePolicy = FabRawSourcePolicy::Unknown; }, false },
        };
        for (const Row& row : rows)
        {
            FabProvenance provenance = MakeProvenance();
            row.Mutate(provenance);
            std::string error;
            const bool valid = ValidateFabProvenance(provenance, error);
            check(valid == row.Valid, std::string("validator: ") + row.Label + " -> " + (valid ? "valid" : "invalid: " + error));
            check(valid || !error.empty(), std::string("validator: an invalid row explains itself: ") + row.Label);
        }

        // Digest: each field participates, equal values agree, canonical encoding has no concatenation ambiguity.
        {
            const FabProvenance base = MakeProvenance();
            const std::string digest = ComputeFabProvenanceDigest(base);
            check(digest.size() == 64 && digest == ComputeFabProvenanceDigest(MakeProvenance()), "equal provenance has an equal digest");
            std::set<std::string> seen { digest };
            const std::function<void(FabProvenance&)> mutations[] = {
                [](FabProvenance& p) { p.ProductIdentity = kListingB; }, [](FabProvenance& p) { p.ProductName += "x"; },
                [](FabProvenance& p) { p.Publisher += "x"; }, [](FabProvenance& p) { p.VersionOrDownloadLabel += "x"; },
                [](FabProvenance& p) { p.LicenseFamily = FabLicenseFamily::LegacyUnrealMarketplace; },
                [](FabProvenance& p) { p.LicenseTier = FabLicenseTier::Professional; },
                [](FabProvenance& p) { p.AttributionText = "x"; }, [](FabProvenance& p) { p.AttributionLink = "x"; },
                [](FabProvenance& p) { p.NoAI = FabMetadataFlag::Yes; }, [](FabProvenance& p) { p.GeneratedWithAI = FabMetadataFlag::Yes; },
                [](FabProvenance& p) { p.RawSourcePolicy = FabRawSourcePolicy::PrivateProjectOnly; },
                [](FabProvenance& p) { p.ProductName = "ab"; p.Publisher = "c"; }, [](FabProvenance& p) { p.ProductName = "a"; p.Publisher = "bc"; } };
            for (const auto& mutate : mutations)
            {
                FabProvenance changed = base;
                mutate(changed);
                check(seen.insert(ComputeFabProvenanceDigest(changed)).second, "every single-field change (and field-boundary shift) changes the digest");
            }
        }

        if (!ControllerIsSupported())
            return check.Passed && CheckUnsupportedPlatformFailsClosed(check);

        // Controller binding.
        Workspace workspace;
        BaseHandles baseHandles;
        check(workspace.IsReady() && CreateBaseProject(workspace, baseHandles), "workspace and base project");
        if (!check.Passed)
            return false;
        check(WriteBytes(workspace.Input("p.glb"), MakeGlb({})), "fixture glb");
        const TreeState before = TakeTree(workspace.Project());
        FabImportController controller(MakeConfig(workspace));
        FabImportProjectContext context;
        check(ReadContext(workspace, context), "context");

        check(!controller.SetProvenance(MakeProvenance()) && !controller.ConfirmProvenance(context) && !controller.Commit(context) && !controller.Dismiss(),
            "Idle refuses provenance, confirmation, commit and dismiss");
        check(controller.GetStatus().State == FabImportState::Idle, "refused calls do not move the state");
        check(SubmitPlanned(controller, workspace.Input("p.glb")), "submit");
        check(!controller.SetProvenance(MakeProvenance()), "provenance is not editable while a worker runs");
        check(Settle(controller) && controller.GetStatus().State == FabImportState::AwaitingProvenance, "awaiting provenance");

        for (const Row& row : rows)
        {
            if (row.Valid)
                continue;
            FabProvenance provenance = MakeProvenance();
            row.Mutate(provenance);
            check(controller.SetProvenance(provenance), std::string("an invalid form can still be stored for editing: ") + row.Label);
            FabImportStatus status = controller.GetStatus();
            // The controller normalises the tier of non-Fab-Standard families before validating.
            FabProvenance normalized = provenance;
            if (normalized.LicenseFamily != FabLicenseFamily::FabStandard && normalized.LicenseFamily != FabLicenseFamily::Unknown)
                normalized.LicenseTier = FabLicenseTier::NotApplicable;
            std::string normalizedError;
            const bool expectValid = ValidateFabProvenance(normalized, normalizedError);
            check(status.ProvenanceValid == expectValid && status.ProvenanceError.empty() == expectValid && !status.ProvenanceConfirmed,
                std::string("status reports the validation outcome: ") + row.Label);
            if (expectValid)
                continue;
            check(!controller.ConfirmProvenance(context) && controller.GetStatus().State == FabImportState::AwaitingProvenance
                    && !controller.GetStatus().LastRejection.empty(),
                std::string("confirmation of invalid provenance is refused without a state change: ") + row.Label);
            check(TakeTree(workspace.Project()) == before, std::string("nothing was staged in the project: ") + row.Label);
        }

        // Valid, but a stale digest or a bad project context never starts cooking.
        FabProvenance good = MakeProvenance();
        check(controller.SetProvenance(good), "valid provenance is stored");
        const std::string digest = controller.GetStatus().ProvenanceDigest;
        check(controller.GetStatus().ProvenanceValid && !controller.GetStatus().ProvenanceConfirmed, "valid and not yet confirmed");
        check(!controller.ConfirmProvenance(context, std::string(64, '0')) && controller.GetStatus().State == FabImportState::AwaitingProvenance,
            "a digest that does not match the reviewed provenance is refused");
        FabImportProjectContext broken = context;
        broken.ManifestBytes = "not a manifest";
        check(!controller.ConfirmProvenance(broken, digest) && controller.GetStatus().State == FabImportState::AwaitingProvenance,
            "a context with an invalid manifest is refused");
        broken = context;
        broken.ProjectRoot = workspace.Base() / "does-not-exist";
        check(!controller.ConfirmProvenance(broken, digest), "a context with a missing project root is refused");
        broken = context;
        broken.ManifestRelativePath = "../Project.spiralproject";
        check(!controller.ConfirmProvenance(broken, digest), "a context with an escaping manifest path is refused");
        // Edit after review: the digest changes and the old digest no longer confirms.
        FabProvenance edited = good;
        edited.ProductName = "Edited After Review";
        check(controller.SetProvenance(edited) && controller.GetStatus().ProvenanceDigest != digest
                && !controller.ConfirmProvenance(context, digest),
            "an edit after review invalidates the reviewed digest");
        // Non-Fab-Standard families normalise the tier instead of asking the user to pick one.
        FabProvenance legacy = good;
        legacy.LicenseFamily = FabLicenseFamily::LegacyUnrealMarketplace;
        legacy.LicenseTier = FabLicenseTier::Professional;
        check(controller.SetProvenance(legacy) && controller.GetStatus().ProvenanceValid
                && controller.GetStatus().Provenance.LicenseTier == FabLicenseTier::NotApplicable,
            "a non-Fab-Standard family normalises the tier to not applicable");
        check(TakeTree(workspace.Project()) == before, "no refused call touched the project");
        // The licence-kind gate: a valid-looking legacy Unreal-only declaration is refused at
        // confirmation, with its own message, even when the reviewed digest is supplied.
        {
            const std::string legacyDigest = controller.GetStatus().ProvenanceDigest;
            check(!controller.ConfirmProvenance(context, legacyDigest) && controller.GetStatus().State == FabImportState::AwaitingProvenance
                    && !controller.GetStatus().ProvenanceConfirmed
                    && controller.GetStatus().LastRejection.find("cannot be used as importable source content in Spiral") != std::string::npos,
                "a legacy UE-only declaration is refused at confirmation by the licence-kind gate: "
                    + controller.GetStatus().LastRejection);
            check(TakeTree(workspace.Project()) == before, "the refused legacy confirmation staged nothing");
            FabProvenance referenceOnly = good;
            referenceOnly.LicenseFamily = FabLicenseFamily::ReferenceOnly;
            check(controller.SetProvenance(referenceOnly)
                    && !controller.ConfirmProvenance(context, controller.GetStatus().ProvenanceDigest)
                    && controller.GetStatus().LastRejection.find("Reference Only") != std::string::npos
                    && controller.GetStatus().State == FabImportState::AwaitingProvenance,
                "a Reference-Only declaration is refused with the licence-kind message: " + controller.GetStatus().LastRejection);
        }

        check(controller.SetProvenance(edited) && controller.ConfirmProvenance(context, controller.GetStatus().ProvenanceDigest)
                && controller.GetStatus().ProvenanceConfirmed && controller.GetStatus().State == FabImportState::Cooking,
            "valid provenance with its reviewed digest starts cooking");
        check(!controller.SetProvenance(good), "provenance cannot be edited once cooking started");
        check(Settle(controller) && controller.GetStatus().State == FabImportState::ReadyToCommit, "cooking completes");
        check(CommitFresh(controller, workspace), "commit");
        const FabImportCommitResult* result = controller.GetCommitResult();
        check(result && result->Receipt.ProductName == "Edited After Review" && result->Receipt.MetadataConfirmedByUser
                && result->Receipt.ProductIdentity == good.ProductIdentity && result->Receipt.LicenseTier == FabLicenseTier::Personal,
            "the committed receipt carries exactly the confirmed provenance");
        VerifyCommittedProject(check, workspace, controller, 1, "provenance");
        return check.Passed;
    }

    // ================================================================== cancellation and failure injection
    namespace
    {
        // Runs one full ZIP import (Submit -> provenance -> Commit) with `arm` installing a one-shot cancel/fail
        // at hook ordinal `ordinal`. Returns the final state; `fired` reports whether the ordinal was reached.
        struct SweepResult
        {
            FabImportState Final = FabImportState::Idle;
            FabImportError Error = FabImportError::None;
            bool Fired = false;
            bool ReachedReady = false;
            FabImportState StateBeforeCommit = FabImportState::Idle;
            u64 HooksSeen = 0;
        };

        enum class SweepKind
        {
            Controller,
            Snapshot,
            Cook,
            CommitCancel,
            CommitFail
        };

        SweepResult RunSweep(const Workspace& workspace, const fs::path& source, SweepKind kind, u64 ordinal)
        {
            SweepResult result;
            std::atomic<u64> seen { 0 };
            std::atomic<bool> fired { false };
            FabImportController* self = nullptr;
            FabImportControllerConfig config = MakeConfig(workspace);
            const auto trip = [&]() -> bool
            {
                if (seen.fetch_add(1) == ordinal)
                {
                    fired = true;
                    return true;
                }
                return false;
            };
            if (kind == SweepKind::Controller)
                config.TestHook = [&](FabImportHookPoint) { if (trip() && self) self->RequestCancel(); };
            if (kind == SweepKind::Snapshot)
                config.SnapshotTestHook = [&](LocalPackageSnapshotHookPoint, std::string_view) { if (trip() && self) self->RequestCancel(); };
            if (kind == SweepKind::Cook)
                config.CookTestHook = [&](FabGltfStage, std::string_view) { if (trip() && self) self->RequestCancel(); };
            if (kind == SweepKind::CommitCancel)
                config.CommitTestHook = [&](ProjectCommitHook, std::string_view) { return trip() ? ProjectCommitHookAction::Cancel : ProjectCommitHookAction::Continue; };
            if (kind == SweepKind::CommitFail)
                config.CommitTestHook = [&](ProjectCommitHook, std::string_view) { return trip() ? ProjectCommitHookAction::Fail : ProjectCommitHookAction::Continue; };

            FabImportController controller(config);
            self = &controller;
            std::string why;
            const bool ready = [&]()
            {
                if (!SubmitPlanned(controller, source) || !Settle(controller))
                    return false;
                if (controller.GetStatus().State != FabImportState::AwaitingProvenance)
                    return false;
                FabImportProjectContext context;
                if (!controller.SetProvenance(MakeProvenance()) || !ReadContext(workspace, context)
                    || !controller.ConfirmProvenance(context, controller.GetStatus().ProvenanceDigest) || !Settle(controller))
                    return false;
                return controller.GetStatus().State == FabImportState::ReadyToCommit;
            }();
            result.ReachedReady = ready;
            result.StateBeforeCommit = controller.GetStatus().State;
            if (ready)
                CommitFresh(controller, workspace);
            Settle(controller);
            const FabImportStatus status = controller.GetStatus();
            result.Final = status.State;
            result.Error = status.Error;
            result.Fired = fired.load();
            result.HooksSeen = seen.load();
            return result;
        }

        bool ManifestLoadsAndIsRevision(const Workspace& workspace, u64 revision, std::string& error)
        {
            FabProjectState state;
            FabProjectValidationOptions options;
            options.Level = FabProjectValidationLevel::FullHash;
            return LoadFabProjectState(workspace.Project(), kManifestName, options, state, error)
                && state.Manifest.ProjectRevision == revision;
        }
    }

    bool TestFabImportControllerCancelsAtEveryHookAndInjectsCommitFailures()
    {
        Checker check { true, "Fab import controller cancellation test failed" };
        ScopedJobSystem jobs;
        if (!ControllerIsSupported())
            return CheckUnsupportedPlatformFailsClosed(check);

        // Cancellation before the manifest replace must leave the accepted project unchanged for every hook of
        // every layer: the controller's own points, the snapshot points, the cook/stage points, and ProjectCommit's.
        const struct
        {
            SweepKind Kind;
            const char* Label;
            u64 MinimumHooks;
        } sweeps[] = {
            { SweepKind::Controller, "controller hook", 11 },
            { SweepKind::Snapshot, "snapshot hook", 12 },
            { SweepKind::Cook, "cook hook", 20 },
            { SweepKind::CommitCancel, "commit cancel", 13 },
            { SweepKind::CommitFail, "commit failure", 13 },
        };
        for (const auto& sweep : sweeps)
        {
            u64 reached = 0;
            for (u64 ordinal = 0; ordinal < 256; ++ordinal)
            {
                Workspace workspace;
                BaseHandles base;
                check(workspace.IsReady() && CreateBaseProject(workspace, base), "workspace");
                if (!check.Passed)
                    return false;
                check(WriteBytes(workspace.Input("p.zip"), BuildZip(SplitZipMembers({}))), "sweep zip fixture");
                const std::string baseManifest = ReadAll(workspace.Manifest());
                const TreeState before = TakeTree(workspace.Project());
                const SweepResult result = RunSweep(workspace, workspace.Input("p.zip"), sweep.Kind, ordinal);
                const std::string label = std::string(sweep.Label) + " #" + std::to_string(ordinal);
                if (!result.Fired)
                {
                    check(result.Final == FabImportState::Done, label + ": a run that never reached the ordinal completes");
                    std::string error;
                    check(ManifestLoadsAndIsRevision(workspace, 1, error), label + ": the uninterrupted import is committed: " + error);
                    check(NoStagingLeaks(workspace), label + ": no staging after a completed run");
                    reached = ordinal;
                    break;
                }
                const bool afterPointer = (sweep.Kind == SweepKind::CommitCancel || sweep.Kind == SweepKind::CommitFail)
                    && ordinal + 1 == result.HooksSeen && ordinal >= 12;
                if (afterPointer)
                {
                    check(result.Final == FabImportState::Done, label + ": cancel or failure at the post-pointer hook is ignored: the commit stands");
                    std::string error;
                    check(ManifestLoadsAndIsRevision(workspace, 1, error), label + ": the committed project loads: " + error);
                    check(NoStagingLeaks(workspace), label + ": no staging after the post-pointer hook");
                    continue;
                }
                // A cancel raised by a worker-side hook must be published as Cancelled before any commit is
                // attempted; it must not be masked by the commit's own cancellation check.
                const bool workerSideHook = sweep.Kind == SweepKind::Snapshot || sweep.Kind == SweepKind::Cook
                    || (sweep.Kind == SweepKind::Controller && ordinal < 9);
                if (workerSideHook)
                    check(!result.ReachedReady && result.StateBeforeCommit == FabImportState::Cancelled,
                        label + ": the worker phase publishes Cancelled itself; saw " + ToString(result.StateBeforeCommit));
                if (sweep.Kind == SweepKind::CommitFail)
                    check(result.Final == FabImportState::Failed && result.Error == FabImportError::CommitFailed,
                        label + ": an injected commit failure is Failed/CommitFailed; saw " + ToString(result.Final) + "/" + ToString(result.Error));
                else
                    check(result.Final == FabImportState::Cancelled && result.Error == FabImportError::None,
                        label + ": cancellation ends Cancelled; saw " + ToString(result.Final) + "/" + ToString(result.Error));
                check(ReadAll(workspace.Manifest()) == baseManifest, label + ": the accepted manifest is byte-identical");
                std::string error;
                check(ManifestLoadsAndIsRevision(workspace, 0, error), label + ": the old project still loads fully: " + error);
                // Anything new is unreferenced garbage in exactly two places: a published generation directory.
                const TreeState after = TakeTree(workspace.Project());
                for (const auto& [path, state] : before)
                    check(after.contains(path) && after.at(path) == state, label + ": pre-existing entry unchanged: " + path);
                for (const auto& [path, state] : after)
                    if (!before.contains(path))
                        check(path == "Assets/fab" || path.starts_with("Assets/fab/"), label + ": only an unreferenced generation may be new, saw " + path);
                check(NoStagingLeaks(workspace), label + ": no staging directory leaked");
            }
            check(reached >= sweep.MinimumHooks, std::string(sweep.Label) + ": the sweep exercised at least " + std::to_string(sweep.MinimumHooks)
                    + " hook ordinals, saw " + std::to_string(reached));
        }

        // Cancelling in the states that have no worker, and cancel with nothing to cancel.
        {
            Workspace workspace;
            BaseHandles base;
            check(workspace.IsReady() && CreateBaseProject(workspace, base), "workspace");
            if (!check.Passed)
                return false;
            WriteBytes(workspace.Input("p.glb"), MakeGlb({}));
            const TreeState before = TakeTree(workspace.Project());
            FabImportController controller(MakeConfig(workspace));
            controller.RequestCancel();
            controller.Update();
            check(controller.GetStatus().State == FabImportState::Idle, "cancel in Idle is a no-op");

            check(SubmitPlanned(controller, workspace.Input("p.glb")) && Settle(controller)
                    && controller.GetStatus().State == FabImportState::AwaitingProvenance,
                "reach AwaitingProvenance");
            controller.RequestCancel();
            check(controller.GetStatus().CancelRequested, "the request is visible");
            controller.Update();
            check(controller.GetStatus().State == FabImportState::Cancelled && NoStagingLeaks(workspace),
                "cancel in AwaitingProvenance ends Cancelled and releases the snapshot");
            controller.RequestCancel();
            controller.Update();
            check(controller.GetStatus().State == FabImportState::Cancelled, "cancel in a terminal state changes nothing");
            check(controller.Dismiss(), "dismiss");

            std::string why;
            check(DriveToReady(controller, workspace, workspace.Input("p.glb"), MakeProvenance(), why),
                "a cancel from a previous job does not poison the next one: " + why);
            controller.RequestCancel();
            controller.Update();
            check(controller.GetStatus().State == FabImportState::Cancelled && NoStagingLeaks(workspace), "cancel in ReadyToCommit ends Cancelled");
            check(!CommitFresh(controller, workspace), "a cancelled import cannot be committed");
            check(TakeTree(workspace.Project()) == before, "the project is untouched");
        }

        // Destroying the controller while a worker is mid-flight cancels, joins and cleans up.
        {
            Workspace workspace;
            BaseHandles base;
            check(workspace.IsReady() && CreateBaseProject(workspace, base), "workspace");
            if (!check.Passed)
                return false;
            WriteBytes(workspace.Input("p.zip"), BuildZip(SplitZipMembers({})));
            const TreeState before = TakeTree(workspace.Project());
            std::atomic<bool> entered { false };
            {
                FabImportControllerConfig config = MakeConfig(workspace);
                config.TestHook = [&entered](FabImportHookPoint point)
                {
                    if (point == FabImportHookPoint::ArchiveCopied)
                    {
                        entered = true;
                        std::this_thread::sleep_for(std::chrono::milliseconds(50));
                    }
                };
                FabImportController controller(config);
                check(SubmitPlanned(controller, workspace.Input("p.zip")), "submit before destruction");
                while (!entered.load())
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            check(NoStagingLeaks(workspace) && TakeTree(workspace.Project()) == before,
                "destruction mid-flight leaves no staging and an untouched project");
        }
        return check.Passed;
    }

    // ================================================================== stale base and assignment
    bool TestFabImportControllerRejectsStaleBasesAndAssignmentMismatches()
    {
        Checker check { true, "Fab import controller stale-base test failed" };
        ScopedJobSystem jobs;
        if (!ControllerIsSupported())
            return CheckUnsupportedPlatformFailsClosed(check);
        Workspace workspace;
        BaseHandles base;
        check(workspace.IsReady() && CreateBaseProject(workspace, base), "workspace and base project");
        if (!check.Passed)
            return false;
        PackageOptions optionsA;
        PackageOptions optionsB;
        optionsB.Variant = 9;
        WriteBytes(workspace.Input("a.glb"), MakeGlb(optionsA));
        WriteBytes(workspace.Input("b.glb"), MakeGlb(optionsB));
        std::string why;

        // Controller 1 cooks A against revision 0; a competing controller commits B first.
        FabImportController first(MakeConfig(workspace));
        FabImportController second(MakeConfig(workspace));
        check(DriveToReady(first, workspace, workspace.Input("a.glb"), MakeProvenance(kListingA), why), "first reaches ReadyToCommit: " + why);
        FabImportProjectContext staleContext;
        check(ReadContext(workspace, staleContext), "stale context is read at revision 0");
        check(DriveToReady(second, workspace, workspace.Input("b.glb"), MakeProvenance(kListingB), why) && CommitFresh(second, workspace),
            "the competing import commits: " + why);
        VerifyCommittedProject(check, workspace, second, 1, "competing import", false);
        const TreeState afterCompeting = TakeTree(workspace.Project(), ".fab-staging");

        check(!first.Commit(staleContext) && first.GetStatus().State == FabImportState::ReadyToCommit,
            "a commit built on the stale manifest is rejected and the import stays ReadyToCommit");
        check(!first.GetStatus().LastRejection.empty() && first.GetCommitResult() == nullptr, "the rejection is explained and nothing was adopted");
        check(TakeTree(workspace.Project(), ".fab-staging") == afterCompeting, "the stale rejection changed nothing on disk (no generation, no revision files)");
        check(CountEntries(workspace.Project() / "Assets" / "fab") == 1, "no second generation was published");

        // Same stale manifest bytes but the registry/receipts of the new state: the manifest hash alone is the base.
        FabImportProjectContext mixed;
        check(ReadContext(workspace, mixed), "fresh context");
        mixed.ManifestBytes = staleContext.ManifestBytes;
        check(!first.Commit(mixed) && first.GetStatus().State == FabImportState::ReadyToCommit && TakeTree(workspace.Project(), ".fab-staging") == afterCompeting,
            "stale manifest bytes with otherwise fresh state are rejected");

        // The retry with a refreshed context succeeds and the project then holds both streams.
        check(CommitFresh(first, workspace) && first.GetStatus().State == FabImportState::Done, "the refreshed retry commits: " + first.GetStatus().LastRejection);
        VerifyCommittedProject(check, workspace, first, 2, "retry");
        {
            FabProjectState state;
            std::string error;
            FabProjectValidationOptions options;
            options.Level = FabProjectValidationLevel::FullHash;
            check(LoadFabProjectState(workspace.Project(), kManifestName, options, state, error) && state.Receipts.Receipts.size() == 2,
                "both competing imports survive in the final project: " + error);
        }
        first.Dismiss();
        second.Dismiss();

        // Assignment: compare-and-swap of an existing entity's handles, persisted in the same commit.
        WriteBytes(workspace.Input("c.glb"), MakeGlb(WithVariant(3)));
        check(DriveToReady(first, workspace, workspace.Input("c.glb"), MakeProvenance("https://www.fab.com/listings/cccccccc-0000-4000-8000-000000000003"), why),
            "assignment import reaches ReadyToCommit: " + why);
        const TreeState beforeAssignment = TakeTree(workspace.Project(), ".fab-staging");
        std::vector<std::string> sceneNamesBefore;
        {
            ProjectManifest manifest;
            std::string error;
            Scene scene;
            check(DeserializeProjectManifest(ReadAll(workspace.Manifest()), manifest, error)
                    && Scene::LoadFromFile(workspace.Project() / manifest.ScenePath, scene),
                "the Scene before the assignment loads");
            for (const SceneEntity& entity : scene.GetEntities())
                sceneNamesBefore.push_back(entity.Name);
        }

        FabAssignmentTarget wrongHandles;
        wrongHandles.Entity = base.Cube.Id;
        wrongHandles.ExpectedMeshAsset = base.Mesh + 1;
        wrongHandles.ExpectedMaterialAsset = base.Material;
        check(!CommitFresh(first, workspace, wrongHandles) && first.GetStatus().State == FabImportState::ReadyToCommit
                && TakeTree(workspace.Project(), ".fab-staging") == beforeAssignment,
            "an assignment whose expected handles no longer match is rejected without changing anything");
        FabAssignmentTarget missingEntity;
        missingEntity.Entity = 9999;
        missingEntity.ExpectedMeshAsset = base.Mesh;
        missingEntity.ExpectedMaterialAsset = base.Material;
        check(!CommitFresh(first, workspace, missingEntity) && TakeTree(workspace.Project(), ".fab-staging") == beforeAssignment,
            "an assignment to a missing entity is rejected without changing anything");

        FabAssignmentTarget target;
        target.Entity = base.Cube.Id;
        target.ExpectedMeshAsset = base.Mesh;
        target.ExpectedMaterialAsset = base.Material;
        check(CommitFresh(first, workspace, target) && first.GetStatus().State == FabImportState::Done, "the CAS assignment commits: " + first.GetStatus().LastRejection);
        VerifyCommittedProject(check, workspace, first, 3, "assignment");
        const FabImportCommitResult* result = first.GetCommitResult();
        check(result && result->AssignmentApplied, "the result reports the assignment");
        {
            ProjectManifest manifest;
            std::string error;
            check(DeserializeProjectManifest(ReadAll(workspace.Manifest()), manifest, error), "committed manifest parses");
            Scene scene;
            check(Scene::LoadFromFile(workspace.Project() / manifest.ScenePath, scene), "committed Scene loads");
            const MeshRendererComponent* renderer = scene.TryGetMeshRendererComponent(base.Cube);
            check(renderer && result && renderer->MeshAsset == result->MeshAsset && renderer->MaterialAsset == result->MaterialHandle
                    && renderer->MeshAsset != base.Mesh,
                "the Scene on disk assigns the entity to the imported mesh and immutable material");
            std::vector<std::string> sceneNamesAfter;
            for (const SceneEntity& entity : scene.GetEntities())
                sceneNamesAfter.push_back(entity.Name);
            check(!sceneNamesBefore.empty() && sceneNamesAfter == sceneNamesBefore, "the rest of the Scene is preserved");
        }
        const AssetHandle importedMesh = result ? result->MeshAsset : kInvalidAssetHandle;
        const AssetHandle importedMaterial = result ? result->MaterialHandle : kInvalidAssetHandle;
        first.Dismiss();

        // Exact reuse with an assignment still commits the Scene revision (no generation), and an
        // immediately repeated assignment is a CAS failure because the handles changed.
        check(DriveToReady(first, workspace, workspace.Input("c.glb"), MakeProvenance("https://www.fab.com/listings/cccccccc-0000-4000-8000-000000000003"), why),
            "reimport reaches ReadyToCommit: " + why);
        check(first.GetStatus().Decision == FabReceiptDecisionKind::ExactReuse, "the reimport is an exact reuse");
        check(!CommitFresh(first, workspace, target), "the previous expected handles are stale after the first assignment");
        FabAssignmentTarget second_target = target;
        second_target.ExpectedMeshAsset = importedMesh;
        second_target.ExpectedMaterialAsset = importedMaterial;
        const size_t generationsBefore = CountEntries(workspace.Project() / "Assets" / "fab");
        check(CommitFresh(first, workspace, second_target) && first.GetStatus().State == FabImportState::Done && first.GetStatus().ProjectChanged,
            "exact reuse with a matching assignment commits a Scene revision");
        check(CountEntries(workspace.Project() / "Assets" / "fab") == generationsBefore && first.GetStatus().ProjectRevision == 4,
            "no new generation is published for an exact reuse");
        std::string error;
        check(ManifestLoadsAndIsRevision(workspace, 4, error), "the project validates at revision 4: " + error);
        return check.Passed;
    }

    // ================================================================== model check
    namespace
    {
        enum class ModelState
        {
            Idle,
            Awaiting,
            Ready,
            Done,
            Failed,
            Cancelled
        };

        FabImportState ToState(ModelState state)
        {
            switch (state)
            {
                case ModelState::Idle: return FabImportState::Idle;
                case ModelState::Awaiting: return FabImportState::AwaitingProvenance;
                case ModelState::Ready: return FabImportState::ReadyToCommit;
                case ModelState::Done: return FabImportState::Done;
                case ModelState::Failed: return FabImportState::Failed;
                case ModelState::Cancelled: return FabImportState::Cancelled;
            }
            return FabImportState::Idle;
        }

        struct Xorshift
        {
            u64 State;

            u64 Next()
            {
                State ^= State << 13;
                State ^= State >> 7;
                State ^= State << 17;
                return State;
            }

            u32 Below(u32 bound) { return static_cast<u32>(Next() % bound); }
        };
    }

    bool TestFabImportControllerStateMachineMatchesModel()
    {
        Checker check { true, "Fab import controller model test failed" };
        ScopedJobSystem jobs;
        if (!ControllerIsSupported())
            return CheckUnsupportedPlatformFailsClosed(check);
        u64 newCommits = 0;
        u64 exactReuses = 0;
        u64 failedImports = 0;
        u64 cancelledImports = 0;
        u64 refusedCalls = 0;

        for (const u64 seed : { 0x5eedull, 0xbeefull, 0x1234567ull })
        {
            Workspace workspace;
            BaseHandles base;
            check(workspace.IsReady() && CreateBaseProject(workspace, base), "workspace");
            if (!check.Passed)
                return false;
            // Three packages: two valid (A, B) and one corrupt. The model knows which is which.
            WriteBytes(workspace.Input("a.glb"), MakeGlb(WithVariant(0)));
            WriteBytes(workspace.Input("b.zip"), BuildZip(SplitZipMembers(WithVariant(1))));
            WriteBytes(workspace.Input("bad.glb"), ToBytes("definitely not glTF"));
            const struct
            {
                const char* Name;
                FabIntakeKind Kind;
                bool Valid;
            } packages[] = { { "a.glb", FabIntakeKind::Glb, true }, { "b.zip", FabIntakeKind::Zip, true },
                { "bad.glb", FabIntakeKind::Glb, false } };

            FabImportController controller(MakeConfig(workspace));
            Xorshift random { seed };
            ModelState model = ModelState::Idle;
            bool packageValid = false;
            size_t packageIndex = 0;
            bool provenanceValid = false;
            bool cancelPending = false;
            std::set<size_t> committed;  // packages already in the project (same provenance -> exact reuse)
            u64 revision = 0;
            std::string trace;

            for (int step = 0; step < 300; ++step)
            {
                // 60% of the time take the productive next call for the model state, otherwise any call,
                // so both the happy path and every refused call are exercised in volume.
                u32 op = random.Below(8);
                if (random.Below(10) < 6)
                {
                    switch (model)
                    {
                        case ModelState::Idle: op = 0; break;
                        case ModelState::Awaiting: op = provenanceValid ? 4 : 2; break;
                        case ModelState::Ready: op = 5; break;
                        default: op = 7; break;
                    }
                }
                bool accepted = false;
                bool expectedAccept = false;
                std::string opName;
                switch (op)
                {
                    case 0:
                    case 1:  // Submit
                    {
                        opName = "Submit";
                        const size_t chosen = random.Below(3);
                        expectedAccept = model == ModelState::Idle;
                        accepted = controller.Submit(RawRequest(workspace.Input(packages[chosen].Name), packages[chosen].Kind));
                        if (expectedAccept)
                        {
                            packageIndex = chosen;
                            packageValid = packages[packageIndex].Valid;
                            provenanceValid = false;
                            cancelPending = false;
                            opName += std::string(" ") + packages[packageIndex].Name;
                            if (random.Below(4) == 0)
                            {
                                controller.RequestCancel();
                                cancelPending = true;
                                opName += "+cancel";
                            }
                            Settle(controller);
                            model = cancelPending ? ModelState::Cancelled : (packageValid ? ModelState::Awaiting : ModelState::Failed);
                        }
                        break;
                    }
                    case 2:
                    case 3:  // SetProvenance (valid or invalid)
                    {
                        const bool valid = random.Below(3) != 0;
                        opName = valid ? "SetProvenance(valid)" : "SetProvenance(invalid)";
                        FabProvenance provenance = MakeProvenance();
                        if (!valid)
                            provenance.ProductIdentity = "https://example.com/not-fab";
                        expectedAccept = model == ModelState::Awaiting;
                        accepted = controller.SetProvenance(provenance);
                        if (expectedAccept)
                            provenanceValid = valid;
                        break;
                    }
                    case 4:  // Confirm
                    {
                        opName = "Confirm";
                        FabImportProjectContext context;
                        ReadContext(workspace, context);
                        expectedAccept = model == ModelState::Awaiting && provenanceValid;
                        accepted = controller.ConfirmProvenance(context);
                        if (expectedAccept)
                        {
                            if (random.Below(4) == 0)
                            {
                                controller.RequestCancel();
                                cancelPending = true;
                                opName += "+cancel";
                            }
                            Settle(controller);
                            model = cancelPending ? ModelState::Cancelled : ModelState::Ready;
                        }
                        break;
                    }
                    case 5:  // Commit
                    {
                        opName = "Commit";
                        expectedAccept = model == ModelState::Ready;
                        accepted = CommitFresh(controller, workspace);
                        if (expectedAccept)
                        {
                            model = ModelState::Done;
                            if (!committed.contains(packageIndex))
                            {
                                committed.insert(packageIndex);
                                ++revision;
                                ++newCommits;
                            }
                            else
                                ++exactReuses;
                        }
                        break;
                    }
                    case 6:  // Cancel + Update
                    {
                        opName = "Cancel";
                        controller.RequestCancel();
                        Settle(controller);
                        accepted = expectedAccept = true;
                        if (model == ModelState::Awaiting || model == ModelState::Ready)
                            model = ModelState::Cancelled;
                        break;
                    }
                    default:  // Dismiss
                    {
                        opName = "Dismiss";
                        expectedAccept = model == ModelState::Done || model == ModelState::Failed || model == ModelState::Cancelled;
                        accepted = controller.Dismiss();
                        if (expectedAccept)
                            model = ModelState::Idle;
                        break;
                    }
                }
                refusedCalls += accepted ? 0 : 1;
                failedImports += model == ModelState::Failed && op <= 1 && accepted ? 1 : 0;
                cancelledImports += model == ModelState::Cancelled && (op == 4 || op <= 1 || op == 6) && accepted ? 1 : 0;
                trace += opName + (accepted ? "" : "(refused)") + " -> " + ToString(ToState(model)) + "; ";
                controller.Update();
                const FabImportStatus status = controller.GetStatus();
                const std::string context = "seed " + std::to_string(seed) + " step " + std::to_string(step) + " after " + opName + ": ";
                check(accepted == expectedAccept, context + "call acceptance matches the model; trace: " + trace);
                check(status.State == ToState(model), context + "state is " + ToString(status.State) + " but the model says " + ToString(ToState(model)) + "; trace: " + trace);
                check(!controller.IsBusy(), context + "no worker is left running after the call settles");
                if (model == ModelState::Idle || model == ModelState::Done || model == ModelState::Failed || model == ModelState::Cancelled)
                    check(NoStagingLeaks(workspace), context + "no staging directory outlives the import; trace: " + trace);
                if (model == ModelState::Awaiting)
                    check(CountEntries(workspace.PackageStaging()) == 1, context + "exactly the retained snapshot is staged; trace: " + trace);
                if (model == ModelState::Ready)
                    check(CountEntries(workspace.PackageStaging()) == 0 && fs::exists(workspace.CookStaging()),
                        context + "a ready import holds only its cook staging; trace: " + trace);
                if (model == ModelState::Failed)
                    check(!status.Message.empty() && status.Error != FabImportError::None, context + "a failure carries an error and message");
                std::string error;
                check(ManifestLoadsAndIsRevision(workspace, revision, error), context + "the on-disk project is valid at revision " + std::to_string(revision) + ": " + error);
                if (!check.Passed)
                    return false;
            }
        }
        check(newCommits >= 6 && exactReuses >= 3 && failedImports >= 3 && cancelledImports >= 3 && refusedCalls >= 20,
            "the random walk covered new commits (" + std::to_string(newCommits) + "), exact reuses (" + std::to_string(exactReuses)
                + "), failed imports (" + std::to_string(failedImports) + "), cancellations (" + std::to_string(cancelledImports)
                + ") and refused calls (" + std::to_string(refusedCalls) + ")");
        return check.Passed;
    }
}
