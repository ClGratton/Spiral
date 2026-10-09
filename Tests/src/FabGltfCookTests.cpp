#include "FabGltfCookTests.h"

#include "Engine/Assets/FabGltfCook.h"
#include "Engine/Core/Sha256.h"

#ifndef MINIZ_NO_ZLIB_COMPATIBLE_NAMES
    #define MINIZ_NO_ZLIB_COMPATIBLE_NAMES
#endif
#include "miniz.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace
{
    using namespace Engine;

    using Bytes = std::vector<u8>;

    bool Check(bool condition, std::string_view message)
    {
        if (!condition)
            std::cerr << "Fab glTF cook test failed: " << message << '\n';
        return condition;
    }

    // The detail is read after the checked call has run, unlike a message built in the call's argument list.
    bool CheckWith(bool condition, std::string_view message, const std::string& detail)
    {
        if (!condition)
            std::cerr << "Fab glTF cook test failed: " << message << ": " << detail << '\n';
        return condition;
    }

    bool Near(double actual, double expected, double tolerance = 1e-6)
    {
        return std::abs(actual - expected) <= tolerance;
    }

    bool Contains(std::string_view text, std::string_view fragment)
    {
        return text.find(fragment) != std::string_view::npos;
    }

    // Unique temporary directory. It never changes the working directory and
    // removes only itself.
    class ScopedFixtureRoot
    {
    public:
        ScopedFixtureRoot()
        {
            static std::atomic<u64> sequence { 0 };
            std::error_code error;
            const u64 timestamp = static_cast<u64>(std::chrono::steady_clock::now().time_since_epoch().count());
            m_Root = std::filesystem::temp_directory_path(error)
                / ("FabGltfCookTests-" + std::to_string(timestamp) + "-"
                    + std::to_string(sequence.fetch_add(1, std::memory_order_relaxed)));
            if (error)
                return;
            m_Ready = std::filesystem::create_directory(m_Root, error) && !error;
        }

        ~ScopedFixtureRoot()
        {
            std::error_code error;
            if (m_Ready)
                std::filesystem::remove_all(m_Root, error);
        }

        ScopedFixtureRoot(const ScopedFixtureRoot&) = delete;
        ScopedFixtureRoot& operator=(const ScopedFixtureRoot&) = delete;

        bool IsReady() const { return m_Ready; }
        std::filesystem::path Path(std::string_view name) const { return m_Root / std::string(name); }
        const std::filesystem::path& Root() const { return m_Root; }

    private:
        std::filesystem::path m_Root;
        bool m_Ready = false;
    };

    bool WriteBytes(const std::filesystem::path& path, const Bytes& bytes)
    {
        std::error_code error;
        std::filesystem::create_directories(path.parent_path(), error);
        if (error)
            return false;
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        return static_cast<bool>(output);
    }

    Bytes ReadBytes(const std::filesystem::path& path)
    {
        std::ifstream input(path, std::ios::binary);
        return Bytes(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    }

    Bytes ToBytes(std::string_view text)
    {
        return Bytes(text.begin(), text.end());
    }

    std::vector<std::string> Listing(const std::filesystem::path& root)
    {
        std::vector<std::string> entries;
        std::error_code error;
        for (std::filesystem::recursive_directory_iterator iterator(root,
                 std::filesystem::directory_options::skip_permission_denied, error), end;
             !error && iterator != end; iterator.increment(error))
        {
            std::string relative = iterator->path().lexically_relative(root).generic_string();
            if (iterator->is_directory(error))
                relative += '/';
            entries.push_back(std::move(relative));
        }
        std::sort(entries.begin(), entries.end());
        return entries;
    }

    // ---------------------------------------------------------------- PNG
    // Stored-deflate encoder so the decoder under test and the oracle are independent.
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

    Bytes MakePng(u32 width, u32 height, const Bytes& rgba)
    {
        Bytes raw;
        for (u32 row = 0; row < height; ++row)
        {
            raw.push_back(0);
            raw.insert(raw.end(), rgba.begin() + static_cast<std::ptrdiff_t>(row) * width * 4,
                rgba.begin() + static_cast<std::ptrdiff_t>(row + 1) * width * 4);
        }
        Bytes zlib { 0x78, 0x01 };
        for (size_t offset = 0; offset < raw.size();)
        {
            const size_t length = std::min<size_t>(65535, raw.size() - offset);
            const bool last = offset + length == raw.size();
            zlib.push_back(last ? 1 : 0);
            zlib.push_back(static_cast<u8>(length & 0xff));
            zlib.push_back(static_cast<u8>(length >> 8));
            zlib.push_back(static_cast<u8>(~length & 0xff));
            zlib.push_back(static_cast<u8>((~length >> 8) & 0xff));
            zlib.insert(zlib.end(), raw.begin() + static_cast<std::ptrdiff_t>(offset),
                raw.begin() + static_cast<std::ptrdiff_t>(offset + length));
            offset += length;
        }
        PutBe32(zlib, static_cast<u32>(mz_adler32(mz_adler32(0, nullptr, 0), raw.data(), raw.size())));
        Bytes png { 0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a };
        Bytes header;
        PutBe32(header, width);
        PutBe32(header, height);
        header.insert(header.end(), { 8, 6, 0, 0, 0 });
        PutChunk(png, "IHDR", header);
        PutChunk(png, "IDAT", zlib);
        PutChunk(png, "IEND", {});
        return png;
    }

    // ---------------------------------------------------------------- JPEG
    // cjpeg 3.2.0 -quality 95 -sample 1x1 -optimize -baseline of a 48x32 image of 3x2 flat 16x16
    // cells; copied from the common-image test corpus. Cell (0,0) is about (220, 40, 40).
    constexpr u8 JpegBaseline444[] =
    {
        0xff, 0xd8, 0xff, 0xe0, 0x00, 0x10, 0x4a, 0x46, 0x49, 0x46, 0x00, 0x01, 0x01, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00,
        0xff, 0xdb, 0x00, 0x43, 0x00, 0x02, 0x01, 0x01, 0x01, 0x01, 0x01, 0x02, 0x01, 0x01, 0x01, 0x02, 0x02, 0x02, 0x02, 0x02,
        0x04, 0x03, 0x02, 0x02, 0x02, 0x02, 0x05, 0x04, 0x04, 0x03, 0x04, 0x06, 0x05, 0x06, 0x06, 0x06, 0x05, 0x06, 0x06, 0x06,
        0x07, 0x09, 0x08, 0x06, 0x07, 0x09, 0x07, 0x06, 0x06, 0x08, 0x0b, 0x08, 0x09, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x06, 0x08,
        0x0b, 0x0c, 0x0b, 0x0a, 0x0c, 0x09, 0x0a, 0x0a, 0x0a, 0xff, 0xdb, 0x00, 0x43, 0x01, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02,
        0x05, 0x03, 0x03, 0x05, 0x0a, 0x07, 0x06, 0x07, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a,
        0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a,
        0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0xff, 0xc0,
        0x00, 0x11, 0x08, 0x00, 0x20, 0x00, 0x30, 0x03, 0x01, 0x11, 0x00, 0x02, 0x11, 0x01, 0x03, 0x11, 0x01, 0xff, 0xc4, 0x00,
        0x17, 0x00, 0x01, 0x01, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x08,
        0x09, 0x07, 0xff, 0xc4, 0x00, 0x14, 0x10, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0xff, 0xc4, 0x00, 0x19, 0x01, 0x01, 0x00, 0x03, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x08, 0x09, 0x0a, 0x07, 0x06, 0xff, 0xc4, 0x00, 0x14, 0x11, 0x01, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0xda, 0x00, 0x0c, 0x03, 0x01, 0x00,
        0x02, 0x11, 0x03, 0x11, 0x00, 0x3f, 0x00, 0x9d, 0xdc, 0x1d, 0x6d, 0x00, 0x29, 0x07, 0x97, 0x66, 0x5c, 0x04, 0x46, 0xbd,
        0xc4, 0xb8, 0x01, 0xd3, 0x14, 0x96, 0xbb, 0x00, 0x14, 0x83, 0xcb, 0xb3, 0x2e, 0x02, 0x23, 0x5e, 0xe2, 0x5c, 0x00, 0xdf,
        0x06, 0x59, 0xd2, 0x50, 0x06, 0x4f, 0xb4, 0x20, 0x96, 0x80, 0x2a, 0x87, 0x3f, 0x66, 0xfc, 0x05, 0x70, 0xa2, 0xf5, 0xc6,
        0x00, 0xc9, 0xf6, 0x84, 0x12, 0xd0, 0x05, 0x50, 0xe7, 0xec, 0xdf, 0x80, 0xff, 0xd9,
    };

    // ---------------------------------------------------------------- glTF builder
    struct Gltf
    {
        Bytes Bin;
        std::vector<std::string> BufferViews, Accessors, Meshes, Nodes, Materials, Textures, Images, Samplers;
        std::string Scene = "0";
        std::string Scenes = "[{\"nodes\":[0]}]";
        std::string Required;
        std::string Used;
        std::string Extra;
        std::string BufferUri;
        bool Glb = true;

        int AddView(const Bytes& data)
        {
            while (Bin.size() % 4 != 0)
                Bin.push_back(0);
            BufferViews.push_back("{\"buffer\":0,\"byteOffset\":" + std::to_string(Bin.size())
                + ",\"byteLength\":" + std::to_string(data.size()) + "}");
            Bin.insert(Bin.end(), data.begin(), data.end());
            return static_cast<int>(BufferViews.size()) - 1;
        }

        int AddAccessor(int view, int componentType, size_t count, const char* type, const std::string& extra = {})
        {
            Accessors.push_back("{" + (view >= 0 ? "\"bufferView\":" + std::to_string(view) + "," : std::string())
                + "\"componentType\":" + std::to_string(componentType) + ",\"count\":" + std::to_string(count)
                + ",\"type\":\"" + type + "\"" + extra + "}");
            return static_cast<int>(Accessors.size()) - 1;
        }

        int AddFloats(const std::vector<float>& values, const char* type, size_t components)
        {
            Bytes data(values.size() * 4);
            std::memcpy(data.data(), values.data(), data.size());
            return AddAccessor(AddView(data), 5126, values.size() / components, type);
        }

        int AddIndices(const std::vector<u32>& values)
        {
            const bool wide = std::any_of(values.begin(), values.end(), [](u32 value) { return value > 65535; });
            Bytes data;
            for (u32 value : values)
            {
                data.push_back(static_cast<u8>(value));
                data.push_back(static_cast<u8>(value >> 8));
                if (wide)
                {
                    data.push_back(static_cast<u8>(value >> 16));
                    data.push_back(static_cast<u8>(value >> 24));
                }
            }
            return AddAccessor(AddView(data), wide ? 5125 : 5123, values.size(), "SCALAR");
        }

        int AddImage(const Bytes& encoded, const char* mime)
        {
            Images.push_back("{\"bufferView\":" + std::to_string(AddView(encoded)) + ",\"mimeType\":\"" + mime + "\"}");
            return static_cast<int>(Images.size()) - 1;
        }

        static std::string Join(const std::vector<std::string>& items)
        {
            std::string text;
            for (size_t index = 0; index < items.size(); ++index)
                text += (index ? "," : "") + items[index];
            return text;
        }

        std::string Json() const
        {
            std::string json = "{\"asset\":{\"version\":\"2.0\"}";
            if (!Scene.empty())
                json += ",\"scene\":" + Scene;
            if (!Scenes.empty())
                json += ",\"scenes\":" + Scenes;
            const auto add = [&json](const char* key, const std::vector<std::string>& items)
            {
                if (!items.empty())
                    json += std::string(",\"") + key + "\":[" + Join(items) + "]";
            };
            add("nodes", Nodes);
            add("meshes", Meshes);
            add("materials", Materials);
            add("textures", Textures);
            add("images", Images);
            add("samplers", Samplers);
            add("accessors", Accessors);
            add("bufferViews", BufferViews);
            json += ",\"buffers\":[{" + (BufferUri.empty() ? std::string() : "\"uri\":\"" + BufferUri + "\",")
                + "\"byteLength\":" + std::to_string(Bin.size()) + "}]";
            if (!Required.empty())
                json += ",\"extensionsRequired\":" + Required;
            if (!Used.empty())
                json += ",\"extensionsUsed\":" + Used;
            if (!Extra.empty())
                json += "," + Extra;
            return json + "}";
        }

        Bytes PaddedBin() const
        {
            Bytes padded = Bin;
            while (padded.size() % 4 != 0)
                padded.push_back(0);
            return padded;
        }

        Bytes RootBytes() const
        {
            const std::string json = Json();
            if (!Glb)
                return ToBytes(json);
            Bytes jsonChunk = ToBytes(json);
            while (jsonChunk.size() % 4 != 0)
                jsonChunk.push_back(' ');
            const Bytes bin = PaddedBin();
            Bytes glb { 'g', 'l', 'T', 'F' };
            const auto putLe32 = [&glb](u32 value)
            {
                for (int shift = 0; shift < 32; shift += 8)
                    glb.push_back(static_cast<u8>(value >> shift));
            };
            putLe32(2);
            putLe32(static_cast<u32>(12 + 8 + jsonChunk.size() + (bin.empty() ? 0 : 8 + bin.size())));
            putLe32(static_cast<u32>(jsonChunk.size()));
            glb.insert(glb.end(), { 'J', 'S', 'O', 'N' });
            glb.insert(glb.end(), jsonChunk.begin(), jsonChunk.end());
            if (!bin.empty())
            {
                putLe32(static_cast<u32>(bin.size()));
                glb.insert(glb.end(), { 'B', 'I', 'N', 0 });
                glb.insert(glb.end(), bin.begin(), bin.end());
            }
            return glb;
        }
    };

    struct Package
    {
        Gltf Doc;
        std::vector<std::pair<std::string, Bytes>> Files;
        // Replaces the generated root; used by hostile-document fixtures.
        Bytes RootOverride;
        bool HasRootOverride = false;
    };

    const std::vector<float> kQuadPositions { 0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0 };
    const std::vector<float> kQuadNormals { 0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1 };
    const std::vector<float> kQuadUv { 0, 0, 1, 0, 1, 1, 0, 1 };
    const std::vector<u32> kQuadIndices { 0, 1, 2, 0, 2, 3 };

    std::string AddPrimitive(Gltf& doc, const std::vector<float>& positions, const std::vector<float>* normals,
        const std::vector<float>* uvs, const std::vector<u32>& indices, int material = -1,
        const std::string& extra = {})
    {
        std::string attributes = "\"POSITION\":" + std::to_string(doc.AddFloats(positions, "VEC3", 3));
        if (normals)
            attributes += ",\"NORMAL\":" + std::to_string(doc.AddFloats(*normals, "VEC3", 3));
        if (uvs)
            attributes += ",\"TEXCOORD_0\":" + std::to_string(doc.AddFloats(*uvs, "VEC2", 2));
        std::string primitive = "{\"attributes\":{" + attributes + "}";
        if (!indices.empty())
            primitive += ",\"indices\":" + std::to_string(doc.AddIndices(indices));
        if (material >= 0)
            primitive += ",\"material\":" + std::to_string(material);
        return primitive + extra + "}";
    }

    // One node (index 0) referencing one mesh (index 0) with one quad primitive.
    Package MakeQuadPackage(int material = -1, bool normals = true, bool uvs = true, const std::string& primitiveExtra = {})
    {
        Package package;
        const std::string primitive = AddPrimitive(package.Doc, kQuadPositions, normals ? &kQuadNormals : nullptr,
            uvs ? &kQuadUv : nullptr, kQuadIndices, material, primitiveExtra);
        package.Doc.Meshes.push_back("{\"primitives\":[" + primitive + "]}");
        package.Doc.Nodes.push_back("{\"mesh\":0}");
        return package;
    }

    // ---------------------------------------------------------------- snapshot and declaration
    bool CreateSnapshot(ScopedFixtureRoot& fixture, const Package& package, LocalPackageSnapshot& snapshot,
        std::string& error)
    {
        static std::atomic<u64> sequence { 0 };
        const std::string id = std::to_string(sequence.fetch_add(1, std::memory_order_relaxed));
        const std::filesystem::path source = fixture.Path("src-" + id);
        const std::filesystem::path staging = fixture.Path("stg-" + id);
        std::error_code filesystemError;
        std::filesystem::create_directories(source, filesystemError);
        std::filesystem::create_directories(staging, filesystemError);
        std::filesystem::permissions(staging, std::filesystem::perms::owner_all,
            std::filesystem::perm_options::replace, filesystemError);
        const Gltf& doc = package.Doc;
        bool written = WriteBytes(source / (doc.Glb ? "model.glb" : "model.gltf"),
            package.HasRootOverride ? package.RootOverride : doc.RootBytes());
        if (!doc.Glb && !doc.BufferUri.empty())
            written = written && WriteBytes(source / doc.BufferUri, doc.PaddedBin());
        for (const auto& [name, bytes] : package.Files)
            written = written && WriteBytes(source / name, bytes);
        if (!written)
        {
            error = "fixture could not be written";
            return false;
        }
        return LocalPackageSnapshot::Create(source, staging, {}, snapshot, error);
    }

    FabImportReceipt MakeDeclaration(const LocalPackageSnapshot& snapshot, FabPackageFormat format,
        std::string_view version = "1.0")
    {
        FabImportReceipt receipt;
        receipt.ProductIdentity = "https://www.fab.com/listings/11111111-2222-3333-4444-555555555555";
        receipt.ProductName = "Ancient Statue";
        receipt.Publisher = "Example Studio";
        receipt.VersionOrDownloadLabel = std::string(version);
        receipt.PackageFormat = format;
        receipt.LicenseFamily = FabLicenseFamily::FabStandard;
        receipt.LicenseTier = FabLicenseTier::Personal;
        receipt.MetadataConfirmedByUser = true;
        receipt.GeneratedWithAI = FabMetadataFlag::No;
        receipt.SourceDigestKind = FabDigestKind::Sha256;
        // Directory and dropped-file intake have no archive: the source digest is the expanded-tree digest.
        receipt.SourceSha256 = snapshot.GetTreeSha256();
        receipt.DiagnosticAcquiredAtUtc = "2026-10-09T10:00:00Z";
        receipt.RawSourcePolicy = FabRawSourcePolicy::ExcludedFromProject;
        receipt.Relation = FabGenerationRelation::Initial;
        return receipt;
    }

    struct Prepared
    {
        LocalPackageSnapshot Snapshot;
        FabImportReceipt Declaration;
        FabGltfPreparedPackage Package;
        std::string Error;
        bool SnapshotOk = false;
        bool PrepareOk = false;
    };

    bool PrepareFixture(ScopedFixtureRoot& fixture, const Package& package, Prepared& result,
        const FabGltfPrepareOptions& options = {}, std::string_view version = "1.0")
    {
        result.SnapshotOk = CreateSnapshot(fixture, package, result.Snapshot, result.Error);
        if (!result.SnapshotOk)
            return false;
        result.Declaration = MakeDeclaration(result.Snapshot,
            package.Doc.Glb ? FabPackageFormat::Glb : FabPackageFormat::Gltf, version);
        result.PrepareOk = PrepareFabGltfPackage(result.Snapshot, result.Declaration, options, result.Package, result.Error);
        return result.PrepareOk;
    }

    FabGltfPreparedPackage MakeSentinelPackage()
    {
        FabGltfPreparedPackage sentinel;
        sentinel.VertexCount = 424242;
        sentinel.Mesh.SourcePath = "sentinel";
        sentinel.Material.Name = "sentinel";
        sentinel.ReceiptDraft.ProductName = "sentinel";
        sentinel.Assets.resize(1);
        sentinel.Assets[0].Name = "sentinel";
        return sentinel;
    }

    bool IsSentinel(const FabGltfPreparedPackage& value)
    {
        return value.VertexCount == 424242 && value.Mesh.SourcePath == "sentinel" && value.Material.Name == "sentinel"
            && value.ReceiptDraft.ProductName == "sentinel" && value.Assets.size() == 1
            && value.Assets[0].Name == "sentinel" && value.Textures.empty() && value.Mesh.Vertices.empty()
            && value.Mesh.Indices.empty();
    }

    // Prepare must fail with a stable diagnostic, leave `out` untouched and create no file.
    bool ExpectReject(ScopedFixtureRoot& fixture, std::string_view label, const Package& package,
        std::string_view diagnostic, const FabGltfPrepareOptions& options = {})
    {
        LocalPackageSnapshot snapshot;
        std::string error;
        if (!CheckWith(CreateSnapshot(fixture, package, snapshot, error), std::string(label) + " snapshot", error))
            return false;
        const FabImportReceipt declaration = MakeDeclaration(snapshot,
            package.Doc.Glb ? FabPackageFormat::Glb : FabPackageFormat::Gltf);
        FabGltfPreparedPackage out = MakeSentinelPackage();
        const std::vector<std::string> before = Listing(fixture.Root());
        error = "stale";
        bool passed = Check(!PrepareFabGltfPackage(snapshot, declaration, options, out, error),
            std::string(label) + " is rejected");
        passed &= Check(Contains(error, diagnostic),
            std::string(label) + " reports '" + std::string(diagnostic) + "' but reported '" + error + "'");
        passed &= Check(IsSentinel(out), std::string(label) + " leaves the output untouched");
        passed &= Check(Listing(fixture.Root()) == before, std::string(label) + " creates no file");
        return passed;
    }

    bool Expect(const Prepared& prepared, std::string_view label)
    {
        return Check(prepared.PrepareOk, std::string(label) + " prepares: " + prepared.Error);
    }

    bool NearVertex(const MeshArtifactVertex& vertex, const std::array<double, 3>& position,
        const std::array<double, 3>& normal, const std::array<double, 2>& uv)
    {
        for (size_t component = 0; component < 3; ++component)
            if (!Near(vertex.Position[component], position[component])
                || !Near(vertex.Normal[component], normal[component]))
                return false;
        return Near(vertex.UV[0], uv[0]) && Near(vertex.UV[1], uv[1]);
    }

    std::string StreamIdOf(const FabImportReceipt& declaration)
    {
        return ComputeFabStreamId(declaration.ProductIdentity, declaration.VersionOrDownloadLabel,
            declaration.PackageFormat);
    }

    // ---------------------------------------------------------------- geometry fixtures
    std::string WithAttribute(std::string primitive, std::string_view name, int accessor)
    {
        const size_t offset = primitive.find("\"attributes\":{");
        primitive.insert(offset + 14, "\"" + std::string(name) + "\":" + std::to_string(accessor) + ",");
        return primitive;
    }

    bool CheckIdentity(const Prepared& prepared)
    {
        const FabGltfPreparedPackage& package = prepared.Package;
        const std::string streamId = StreamIdOf(prepared.Declaration);
        bool passed = Check(package.ReceiptDraft.StreamId == streamId, "draft carries the computed stream id");
        passed &= Check(package.ReceiptDraft.GenerationId == ComputeFabGenerationId(streamId,
            prepared.Snapshot.GetTreeSha256(), prepared.Snapshot.GetTreeSha256()),
            "draft carries the generation id derived from the tree digest used as the source digest");
        passed &= Check(package.ReceiptDraft.ExpandedTreeSha256 == prepared.Snapshot.GetTreeSha256(),
            "draft carries the snapshot tree digest");
        passed &= Check(package.ReceiptDraft.ImporterVersion == kFabGltfImporterVersion
            && package.ReceiptDraft.CookerVersion == kFabGltfCookerVersion, "draft names the importer and cooker versions");
        passed &= Check(package.ReceiptDraft.Assets.size() == package.Assets.size() && package.Assets.size() >= 2,
            "draft lists every prepared asset");
        if (package.Assets.size() < 2 || package.ReceiptDraft.Assets.size() != package.Assets.size())
            return false;
        for (size_t index = 0; index < package.Assets.size(); ++index)
        {
            const FabGltfPreparedAsset& asset = package.Assets[index];
            const FabImportedAssetRecord& record = package.ReceiptDraft.Assets[index];
            passed &= Check(record.Handle == asset.Handle && record.Type == asset.Type
                && record.SemanticRole == asset.SemanticRole && record.LogicalPath == asset.LogicalPath
                && record.GenerationRelativeCookedPath == asset.GenerationRelativeCookedPath
                && record.ArtifactSha256.empty(), "draft record mirrors prepared asset");
            passed &= Check(asset.Handle == ComputeFabStableAssetHandle(streamId, asset.Type, asset.SemanticRole),
                "handle derives from stream, type and semantic role only");
            passed &= Check(asset.RegistrySourcePath == "fab:" + streamId + "/" + asset.LogicalPath
                && asset.LogicalPath.find(':') == std::string::npos, "registry path is stream scoped and semantic");
        }
        passed &= Check(package.Assets[0].SemanticRole == "mesh.main" && package.Assets[0].LogicalPath == "mesh/main"
            && package.Assets[0].GenerationRelativeCookedPath
                == "meshes/" + std::to_string(package.Assets[0].Handle) + ".spiralmesh", "mesh asset naming");
        passed &= Check(package.Assets[1].SemanticRole == "material.main" && package.Assets[1].LogicalPath == "material/main"
            && package.Assets[1].GenerationRelativeCookedPath
                == "materials/" + std::to_string(package.Assets[1].Handle) + ".spiralmat", "material asset naming");
        passed &= Check(package.Mesh.Asset == package.Assets[0].Handle && package.Mesh.SourcePath == package.Assets[0].RegistrySourcePath,
            "mesh artifact carries the registry identity");
        std::string meshError;
        passed &= CheckWith(ValidateMeshArtifact(package.Mesh, meshError), "prepared mesh validates", meshError);
        return passed;
    }

    bool GeometryTransformChain(ScopedFixtureRoot& fixture)
    {
        Package package;
        const float s = 0.70710678f;
        const std::vector<float> normals { 0, s, s, 0, s, s, 0, s, s, 0, s, s };
        package.Doc.Meshes.push_back("{\"primitives\":["
            + AddPrimitive(package.Doc, kQuadPositions, &normals, &kQuadUv, kQuadIndices) + "]}");
        package.Doc.Nodes.push_back("{\"translation\":[10,0,0],\"children\":[1]}");
        package.Doc.Nodes.push_back("{\"scale\":[2,3,4],\"mesh\":0}");
        Prepared prepared;
        PrepareFixture(fixture, package, prepared);
        bool passed = Expect(prepared, "transform chain");
        if (!passed)
            return false;
        const MeshArtifact& mesh = prepared.Package.Mesh;
        passed &= Check(mesh.Vertices.size() == 4 && mesh.Indices == kQuadIndices && mesh.Primitives.size() == 1,
            "transform chain keeps the quad topology");
        // Hand arithmetic: T(10,0,0) * S(2,3,4) * (1,1,0) = (12,3,0); the inverse transpose of
        // diag(2,3,4) takes (0,1,1)/sqrt2 to (0,3*... ) = (0,8,6)/10 = (0,0.8,0.6).
        passed &= Check(mesh.Vertices.size() == 4 && NearVertex(mesh.Vertices[2], { 12, 3, 0 }, { 0, 0.8, 0.6 }, { 1, 1 }),
            "vertex (1,1,0) bakes to (12,3,0) with normal (0,0.8,0.6) and UV (1,1)");
        passed &= Check(mesh.Vertices.size() == 4 && NearVertex(mesh.Vertices[0], { 10, 0, 0 }, { 0, 0.8, 0.6 }, { 0, 0 }),
            "vertex (0,0,0) bakes to the parent translation");
        passed &= Check(mesh.Vertices.size() == 4 && mesh.Vertices[3].Color[0] == 1.0f && mesh.Vertices[3].Color[2] == 1.0f,
            "vertex colour stays the default white");
        passed &= Check(mesh.Primitives.size() == 1 && mesh.Primitives[0].SourceMeshIndex == 0
            && mesh.Primitives[0].SourcePrimitiveIndex == 0 && mesh.Primitives[0].VertexByteOffset == 0
            && mesh.Primitives[0].VertexByteSize == 4 * sizeof(MeshArtifactVertex) && mesh.Primitives[0].IndexByteOffset == 0
            && mesh.Primitives[0].IndexByteSize == 24, "primitive ranges are byte exact");
        passed &= Check(prepared.Package.VertexCount == 4 && prepared.Package.TriangleCount == 2
            && prepared.Package.PrimitiveInstanceCount == 1, "diagnostic counts");
        passed &= Check(prepared.Package.Assets.size() == 2 && prepared.Package.Textures.empty(), "untextured package has two assets");
        passed &= CheckIdentity(prepared);
        return passed;
    }

    bool GeometryExplicitMatrix(ScopedFixtureRoot& fixture)
    {
        Package package;
        const std::vector<float> normals { 1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0 };
        package.Doc.Meshes.push_back("{\"primitives\":["
            + AddPrimitive(package.Doc, kQuadPositions, &normals, &kQuadUv, kQuadIndices) + "]}");
        // Rotation by 90 degrees about +Z (x -> y) with translation (5,6,7), column-major.
        package.Doc.Nodes.push_back("{\"mesh\":0,\"matrix\":[0,1,0,0,-1,0,0,0,0,0,1,0,5,6,7,1]}");
        Prepared prepared;
        PrepareFixture(fixture, package, prepared);
        bool passed = Expect(prepared, "explicit matrix");
        if (!passed)
            return false;
        const MeshArtifact& mesh = prepared.Package.Mesh;
        passed &= Check(mesh.Vertices.size() == 4 && NearVertex(mesh.Vertices[1], { 5, 7, 7 }, { 0, 1, 0 }, { 1, 0 }),
            "(1,0,0) rotates to (0,1,0) then translates to (5,7,7); normal (1,0,0) rotates to (0,1,0)");
        passed &= Check(mesh.Vertices.size() == 4 && NearVertex(mesh.Vertices[3], { 4, 6, 7 }, { 0, 1, 0 }, { 0, 1 }),
            "(0,1,0) rotates to (-1,0,0) then translates to (4,6,7)");
        passed &= Check(mesh.Indices == kQuadIndices, "positive determinant keeps the winding");
        return passed;
    }

    bool GeometryMirroredNodeFlipsWinding(ScopedFixtureRoot& fixture)
    {
        bool passed = true;
        for (const bool authored : { true, false })
        {
            Package package;
            const std::vector<float> positions { 0, 0, 0, 1, 0, 0, 0, 1, 0 };
            const std::vector<float> normals { 0, 0, 1, 0, 0, 1, 0, 0, 1 };
            package.Doc.Meshes.push_back("{\"primitives\":["
                + AddPrimitive(package.Doc, positions, authored ? &normals : nullptr, nullptr, { 0, 1, 2 }) + "]}");
            package.Doc.Nodes.push_back("{\"mesh\":0,\"scale\":[-1,1,1]}");
            Prepared prepared;
            PrepareFixture(fixture, package, prepared);
            const std::string label = authored ? "mirrored authored normals" : "mirrored derived normals";
            if (!Expect(prepared, label))
            {
                passed = false;
                continue;
            }
            const MeshArtifact& mesh = prepared.Package.Mesh;
            // Mirroring keeps the triangle counter-clockwise from +Z only if two indices swap:
            // (b-a)x(c-a) with b=(0,1,0), c=(-1,0,0) is (0,0,1).
            passed &= Check(mesh.Indices == std::vector<u32> { 0, 2, 1 }, label + " swaps indices 1 and 2");
            passed &= Check(mesh.Vertices.size() == 3 && NearVertex(mesh.Vertices[1], { -1, 0, 0 }, { 0, 0, 1 }, { 0, 0 })
                && NearVertex(mesh.Vertices[2], { 0, 1, 0 }, { 0, 0, 1 }, { 0, 0 }), label + " keeps +Z normals");
        }
        return passed;
    }

    bool GeometryDerivedNormalsAreAreaWeighted(ScopedFixtureRoot& fixture)
    {
        Package package;
        const std::vector<float> positions { 0, 0, 0, 2, 0, 0, 0, 2, 0, 0, 1, 0, 0, 0, 1 };
        package.Doc.Meshes.push_back("{\"primitives\":["
            + AddPrimitive(package.Doc, positions, nullptr, nullptr, { 0, 1, 2, 0, 3, 4 }) + "]}");
        package.Doc.Nodes.push_back("{\"mesh\":0}");
        Prepared prepared;
        PrepareFixture(fixture, package, prepared);
        if (!Expect(prepared, "derived normals"))
            return false;
        const MeshArtifact& mesh = prepared.Package.Mesh;
        // Face vectors (unnormalised cross products): (0,0,4) and (1,0,0). Vertex 0 is shared:
        // (1,0,4)/sqrt(17) = (0.2425356, 0, 0.9701425).
        bool passed = Check(mesh.Vertices.size() == 5, "derived-normal vertex count");
        passed &= Check(mesh.Vertices.size() == 5 && NearVertex(mesh.Vertices[0], { 0, 0, 0 }, { 0.2425356250, 0, 0.9701425001 }, { 0, 0 }),
            "shared vertex normal is area weighted");
        passed &= Check(mesh.Vertices.size() == 5 && NearVertex(mesh.Vertices[1], { 2, 0, 0 }, { 0, 0, 1 }, { 0, 0 })
            && NearVertex(mesh.Vertices[3], { 0, 1, 0 }, { 1, 0, 0 }, { 0, 0 }), "unshared vertices take their face normal");
        return passed;
    }

    bool GeometryInstancedMeshAndSharedMaterial(ScopedFixtureRoot& fixture)
    {
        bool passed = true;
        {
            Package package = MakeQuadPackage();
            package.Doc.Nodes = { "{\"mesh\":0}", "{\"mesh\":0,\"translation\":[3,0,0]}" };
            package.Doc.Scenes = "[{\"nodes\":[0,1]}]";
            Prepared prepared;
            PrepareFixture(fixture, package, prepared);
            if (!Expect(prepared, "instanced mesh"))
                return false;
            const MeshArtifact& mesh = prepared.Package.Mesh;
            passed &= Check(mesh.Vertices.size() == 8 && mesh.Indices.size() == 12 && mesh.Primitives.size() == 2,
                "two nodes bake two primitive ranges into one artifact");
            passed &= Check(mesh.Primitives.size() == 2 && mesh.Primitives[0].SourceMeshIndex == mesh.Primitives[1].SourceMeshIndex
                && mesh.Primitives[0].SourcePrimitiveIndex == mesh.Primitives[1].SourcePrimitiveIndex
                && mesh.Primitives[1].VertexByteOffset == 4 * sizeof(MeshArtifactVertex) && mesh.Primitives[1].IndexByteOffset == 24,
                "instances share source indices with disjoint byte ranges");
            passed &= Check(mesh.Indices.size() == 12 && mesh.Indices[6] == 4 && mesh.Indices[7] == 5 && mesh.Indices[8] == 6
                && mesh.Indices[9] == 4 && mesh.Indices[10] == 6 && mesh.Indices[11] == 7, "second instance indices are rebased");
            passed &= Check(mesh.Vertices.size() == 8 && NearVertex(mesh.Vertices[4], { 3, 0, 0 }, { 0, 0, 1 }, { 0, 0 }),
                "second instance is translated");
        }
        {
            // Several nodes and primitives that all use ONE material (the common real-world layout).
            Package package;
            package.Doc.Materials.push_back("{\"name\":\"Shared\",\"pbrMetallicRoughness\":{\"baseColorFactor\":[0.5,0.25,0.125,1],"
                "\"metallicFactor\":0,\"roughnessFactor\":0.5}}");
            const std::string quad = AddPrimitive(package.Doc, kQuadPositions, &kQuadNormals, &kQuadUv, kQuadIndices, 0);
            const std::vector<float> trianglePositions { 0, 0, 0, 1, 0, 0, 0, 1, 0 };
            const std::vector<float> triangleNormals { 0, 0, 1, 0, 0, 1, 0, 0, 1 };
            const std::vector<float> triangleUv { 0, 0, 1, 0, 0, 1 };
            const std::string triangle = AddPrimitive(package.Doc, trianglePositions, &triangleNormals, &triangleUv, { 0, 1, 2 }, 0);
            package.Doc.Meshes.push_back("{\"primitives\":[" + quad + "," + triangle + "]}");
            package.Doc.Nodes = { "{\"mesh\":0}", "{\"mesh\":0,\"translation\":[0,5,0]}" };
            package.Doc.Scenes = "[{\"nodes\":[0,1]}]";
            Prepared prepared;
            PrepareFixture(fixture, package, prepared);
            if (!Expect(prepared, "multi-primitive single material"))
                return false;
            const FabGltfPreparedPackage& result = prepared.Package;
            passed &= Check(result.PrimitiveInstanceCount == 4 && result.VertexCount == 14 && result.TriangleCount == 6,
                "four instances bake 14 vertices and 6 triangles");
            passed &= Check(result.Mesh.Primitives.size() == 4 && result.Mesh.Primitives[0].SourcePrimitiveIndex == 0
                && result.Mesh.Primitives[1].SourcePrimitiveIndex == 1 && result.Mesh.Primitives[2].SourcePrimitiveIndex == 0
                && result.Mesh.Primitives[3].SourcePrimitiveIndex == 1, "instances keep their source primitive indices in node order");
            passed &= Check(result.Mesh.Primitives.size() == 4 && result.Mesh.Primitives[1].VertexByteOffset == 4 * sizeof(MeshArtifactVertex)
                && result.Mesh.Primitives[2].VertexByteOffset == 7 * sizeof(MeshArtifactVertex)
                && result.Mesh.Primitives[3].IndexByteOffset == 15 * sizeof(u32), "primitive ranges accumulate");
            passed &= Check(result.Assets.size() == 2 && Near(result.Material.BaseColor.X, 0.5) && Near(result.Material.BaseColor.Y, 0.25)
                && Near(result.Material.BaseColor.Z, 0.125) && Near(result.Material.Metallic, 0) && Near(result.Material.Roughness, 0.5)
                && result.Material.Name == "Shared", "the single shared material is cooked once");
            passed &= Check(result.Mesh.Vertices.size() == 14 && NearVertex(result.Mesh.Vertices[7], { 0, 5, 0 }, { 0, 0, 1 }, { 0, 0 }),
                "third primitive belongs to the second node");
        }
        return passed;
    }

    bool GeometrySparseNormalizedAndOptionalAttributes(ScopedFixtureRoot& fixture)
    {
        bool passed = true;
        {
            Package package;
            Gltf& doc = package.Doc;
            const int base = doc.AddView([&]
            {
                Bytes data(kQuadPositions.size() * 4);
                std::memcpy(data.data(), kQuadPositions.data(), data.size());
                return data;
            }());
            const int indexView = doc.AddView(Bytes { 2 });
            const std::vector<float> substitute { 5, 6, 7 };
            Bytes substituteBytes(12);
            std::memcpy(substituteBytes.data(), substitute.data(), 12);
            const int valueView = doc.AddView(substituteBytes);
            const int position = doc.AddAccessor(base, 5126, 4, "VEC3",
                ",\"sparse\":{\"count\":1,\"indices\":{\"bufferView\":" + std::to_string(indexView)
                + ",\"componentType\":5121},\"values\":{\"bufferView\":" + std::to_string(valueView) + "}}");
            const int normal = doc.AddFloats(kQuadNormals, "VEC3", 3);
            doc.Meshes.push_back("{\"primitives\":[{\"attributes\":{\"POSITION\":" + std::to_string(position)
                + ",\"NORMAL\":" + std::to_string(normal) + "},\"indices\":" + std::to_string(doc.AddIndices(kQuadIndices)) + "}]}");
            doc.Nodes.push_back("{\"mesh\":0}");
            Prepared prepared;
            PrepareFixture(fixture, package, prepared);
            if (Expect(prepared, "sparse positions"))
                passed &= Check(prepared.Package.Mesh.Vertices.size() == 4
                    && NearVertex(prepared.Package.Mesh.Vertices[2], { 5, 6, 7 }, { 0, 0, 1 }, { 0, 0 })
                    && NearVertex(prepared.Package.Mesh.Vertices[1], { 1, 0, 0 }, { 0, 0, 1 }, { 0, 0 }),
                    "sparse substitution replaces exactly the named vertex");
            else
                passed = false;
        }
        {
            Package package;
            Gltf& doc = package.Doc;
            const int position = doc.AddFloats(kQuadPositions, "VEC3", 3);
            const int normal = doc.AddFloats(kQuadNormals, "VEC3", 3);
            Bytes uv;
            for (const u32 value : { 0u, 0u, 65535u, 0u, 65535u, 32768u, 0u, 65535u })
            {
                uv.push_back(static_cast<u8>(value));
                uv.push_back(static_cast<u8>(value >> 8));
            }
            const int uvAccessor = doc.AddAccessor(doc.AddView(uv), 5123, 4, "VEC2", ",\"normalized\":true");
            doc.Meshes.push_back("{\"primitives\":[{\"attributes\":{\"POSITION\":" + std::to_string(position)
                + ",\"NORMAL\":" + std::to_string(normal) + ",\"TEXCOORD_0\":" + std::to_string(uvAccessor)
                + "},\"indices\":" + std::to_string(doc.AddIndices(kQuadIndices)) + "}]}");
            doc.Nodes.push_back("{\"mesh\":0}");
            Prepared prepared;
            PrepareFixture(fixture, package, prepared);
            if (Expect(prepared, "normalized UV"))
                passed &= Check(prepared.Package.Mesh.Vertices.size() == 4
                    && Near(prepared.Package.Mesh.Vertices[2].UV[0], 1.0) && Near(prepared.Package.Mesh.Vertices[2].UV[1], 32768.0 / 65535.0)
                    && Near(prepared.Package.Mesh.Vertices[3].UV[1], 1.0), "normalized unsigned UVs convert to 0..1");
            else
                passed = false;
        }
        {
            Package package = MakeQuadPackage(-1, true, false);
            Prepared prepared;
            PrepareFixture(fixture, package, prepared);
            if (Expect(prepared, "untextured mesh without UV"))
                passed &= Check(prepared.Package.Mesh.Vertices.size() == 4 && prepared.Package.Mesh.Vertices[2].UV[0] == 0.0f
                    && prepared.Package.Mesh.Vertices[2].UV[1] == 0.0f, "missing UVs are zero for an untextured material");
            else
                passed = false;
        }
        {
            Package package;
            Gltf& doc = package.Doc;
            const std::vector<float> white(16, 1.0f);
            std::string primitive = AddPrimitive(doc, kQuadPositions, &kQuadNormals, &kQuadUv, kQuadIndices);
            primitive = WithAttribute(primitive, "COLOR_0", doc.AddFloats(white, "VEC4", 4));
            doc.Meshes.push_back("{\"primitives\":[" + primitive + "]}");
            doc.Nodes.push_back("{\"mesh\":0}");
            Prepared prepared;
            PrepareFixture(fixture, package, prepared);
            passed &= Expect(prepared, "all-white COLOR_0 is the identity tint");
        }
        {
            Package package;
            const std::vector<float> triangle { 0, 0, 0, 1, 0, 0, 0, 1, 0 };
            package.Doc.Meshes.push_back("{\"primitives\":["
                + AddPrimitive(package.Doc, triangle, nullptr, nullptr, {}) + "]}");
            package.Doc.Nodes.push_back("{\"mesh\":0}");
            Prepared prepared;
            PrepareFixture(fixture, package, prepared);
            if (Expect(prepared, "non-indexed primitive"))
                passed &= Check(prepared.Package.Mesh.Indices == std::vector<u32> { 0, 1, 2 } && prepared.Package.TriangleCount == 1
                    && NearVertex(prepared.Package.Mesh.Vertices[0], { 0, 0, 0 }, { 0, 0, 1 }, { 0, 0 }),
                    "non-indexed triangles use the identity index list");
            else
                passed = false;
        }
        return passed;
    }

    bool GeometryDefaultMaterialAndIgnoredSceneContent(ScopedFixtureRoot& fixture)
    {
        bool passed = true;
        {
            Package package = MakeQuadPackage();
            Prepared prepared;
            PrepareFixture(fixture, package, prepared);
            if (!Expect(prepared, "default material"))
                return false;
            const MaterialAsset& material = prepared.Package.Material;
            passed &= Check(Near(material.BaseColor.X, 1) && Near(material.BaseColor.Y, 1) && Near(material.BaseColor.Z, 1)
                && Near(material.Metallic, 1) && Near(material.Roughness, 1) && Near(material.NormalScale, 1)
                && Near(material.OcclusionStrength, 1) && Near(material.EmissiveColor.X, 0) && Near(material.EmissiveStrength, 1)
                && !material.TwoSided && material.AlphaMode == MaterialAlphaMode::Opaque
                && material.ShadingModel == MaterialShadingModel::Standard, "glTF default material values");
            passed &= Check(material.Textures.BaseColor == kInvalidAssetHandle && material.Textures.Orm == kInvalidAssetHandle
                && material.Textures.Normal == kInvalidAssetHandle && material.Textures.Emissive == kInvalidAssetHandle
                && prepared.Package.Textures.empty(), "no textures without references");
            passed &= Check(material.Name == "Ancient Statue" && prepared.Package.Assets[0].Name == "Ancient Statue",
                "names fall back to the confirmed product name");
        }
        {
            Package package = MakeQuadPackage();
            package.Doc.Nodes = { "{\"mesh\":0,\"children\":[1,2]}", "{\"camera\":0}",
                "{\"extensions\":{\"KHR_lights_punctual\":{\"light\":0}}}" };
            package.Doc.Materials.push_back("{\"name\":\"Unreferenced\",\"pbrMetallicRoughness\":{\"metallicFactor\":0.3}}");
            package.Doc.Scenes = "[{\"nodes\":[0]},{\"nodes\":[0]}]";
            package.Doc.Used = "[\"KHR_lights_punctual\"]";
            package.Doc.Extra = "\"cameras\":[{\"type\":\"perspective\",\"perspective\":{\"yfov\":0.5,\"znear\":0.1}}],"
                "\"extensions\":{\"KHR_lights_punctual\":{\"lights\":[{\"type\":\"point\"}]}}";
            Prepared prepared;
            PrepareFixture(fixture, package, prepared);
            if (Expect(prepared, "camera, light, unreferenced material and spare scene"))
                passed &= Check(prepared.Package.Mesh.Vertices.size() == 4 && Near(prepared.Package.Material.Metallic, 1),
                    "ignored scene content does not change geometry or the material");
            else
                passed = false;
        }
        return passed;
    }

    // ---------------------------------------------------------------- material and texture fixtures
    using Pixel = std::array<u8, 4>;

    Bytes Flatten(const std::vector<Pixel>& pixels)
    {
        Bytes bytes;
        for (const Pixel& pixel : pixels)
            bytes.insert(bytes.end(), pixel.begin(), pixel.end());
        return bytes;
    }

    Bytes PngOf(u32 width, u32 height, const std::vector<Pixel>& pixels)
    {
        return MakePng(width, height, Flatten(pixels));
    }

    const TextureArtifact* FindTexture(const FabGltfPreparedPackage& package, TextureRole role)
    {
        for (const TextureArtifact& texture : package.Textures)
            if (texture.Role == role)
                return &texture;
        return nullptr;
    }

    bool CheckTexture(const FabGltfPreparedPackage& package, const FabImportReceipt& declaration, TextureRole role,
        std::string_view semanticRole, TextureColorSpace colorSpace, u32 width, u32 height, size_t mipCount,
        const Bytes& mip0, std::string_view label)
    {
        const TextureArtifact* texture = FindTexture(package, role);
        if (!Check(texture != nullptr, std::string(label) + " is cooked"))
            return false;
        const std::string streamId = StreamIdOf(declaration);
        bool passed = Check(texture->Asset == ComputeFabStableAssetHandle(streamId, AssetType::Texture, semanticRole),
            std::string(label) + " handle derives from its semantic role");
        std::string logical(semanticRole);
        logical[logical.find('.')] = '/';
        passed &= Check(texture->SourcePath == "fab:" + streamId + "/" + logical, std::string(label) + " registry path");
        passed &= Check(texture->ColorSpace == colorSpace && texture->TargetProfile == TextureTargetProfile::RGBAFallback
            && texture->CookedFormat == (colorSpace == TextureColorSpace::Srgb ? TextureCookedFormat::R8G8B8A8Srgb
                                                                                : TextureCookedFormat::R8G8B8A8Unorm),
            std::string(label) + " colour space and cooked format");
        passed &= Check(!texture->HasAlpha, std::string(label) + " is opaque");
        passed &= Check(texture->Mips.size() == mipCount && texture->Mips[0].Width == width && texture->Mips[0].Height == height,
            std::string(label) + " mip shape");
        passed &= Check(texture->Payload.size() >= mip0.size()
            && std::equal(mip0.begin(), mip0.end(), texture->Payload.begin()), std::string(label) + " mip 0 payload");
        std::string error;
        passed &= CheckWith(ValidateTextureArtifact(*texture, error), std::string(label) + " validates", error);
        return passed;
    }

    bool ExpectMip(const TextureArtifact& texture, size_t mip, const Pixel& expected, std::string_view label)
    {
        if (!Check(texture.Mips.size() > mip && texture.Mips[mip].ByteSize >= 4, std::string(label) + " mip exists"))
            return false;
        const u8* data = texture.Payload.data() + texture.Mips[mip].ByteOffset;
        return Check(data[0] == expected[0] && data[1] == expected[1] && data[2] == expected[2] && data[3] == expected[3],
            std::string(label) + " mip " + std::to_string(mip) + " first texel");
    }

    const std::vector<Pixel> kAllRolesBase {
        { 10, 11, 12, 255 }, { 20, 21, 22, 255 }, { 30, 31, 32, 7 }, { 40, 41, 42, 255 },
        { 50, 51, 52, 255 }, { 60, 61, 62, 255 }, { 70, 71, 72, 255 }, { 80, 81, 82, 255 } };
    const std::vector<Pixel> kAllRolesMetallicRoughness {
        { 10, 20, 30, 255 }, { 20, 40, 60, 255 }, { 30, 60, 90, 255 }, { 42, 81, 123, 255 } };
    const std::vector<Pixel> kAllRolesNormal(4, Pixel { 128, 128, 255, 255 });
    const std::vector<Pixel> kAllRolesEmissive(4, Pixel { 200, 100, 50, 255 });

    // One quad with every supported material role, each backed by an embedded PNG.
    Package BuildAllRolesPackage(const std::vector<Pixel>& base = kAllRolesBase, bool withEmissive = true)
    {
        Package package = MakeQuadPackage(0);
        Gltf& doc = package.Doc;
        doc.AddImage(PngOf(4, 2, base), "image/png");
        doc.AddImage(PngOf(2, 2, kAllRolesMetallicRoughness), "image/png");
        doc.AddImage(PngOf(2, 2, kAllRolesNormal), "image/png");
        doc.Samplers = { "{\"magFilter\":9729,\"minFilter\":9987,\"wrapS\":33071,\"wrapT\":33071}",
            "{\"magFilter\":9728,\"minFilter\":9984}", "{\"magFilter\":9728,\"wrapS\":33071,\"wrapT\":33071}" };
        doc.Textures = { "{\"source\":0,\"sampler\":0}", "{\"source\":1,\"sampler\":1}", "{\"source\":2}" };
        std::string emissive;
        if (withEmissive)
        {
            doc.AddImage(PngOf(2, 2, kAllRolesEmissive), "image/png");
            doc.Textures.push_back("{\"source\":3,\"sampler\":2}");
            emissive = ",\"emissiveTexture\":{\"index\":3}";
        }
        doc.Materials.push_back("{\"name\":\"Marble\",\"doubleSided\":true,\"pbrMetallicRoughness\":{"
            "\"baseColorFactor\":[0.8,0.6,0.4,1.0],\"metallicFactor\":0.25,\"roughnessFactor\":0.5,"
            "\"baseColorTexture\":{\"index\":0},\"metallicRoughnessTexture\":{\"index\":1}},"
            "\"normalTexture\":{\"index\":2,\"scale\":2.0},\"occlusionTexture\":{\"index\":1,\"strength\":0.75}"
            + emissive + ",\"emissiveFactor\":[0.1,0.2,0.3]}");
        return package;
    }

    bool MaterialAllRoles(ScopedFixtureRoot& fixture)
    {
        std::vector<Pixel> baseOpaque = kAllRolesBase;
        baseOpaque[2][3] = 255;
        const std::vector<Pixel>& metallicRoughness = kAllRolesMetallicRoughness;
        const std::vector<Pixel>& normal = kAllRolesNormal;
        const std::vector<Pixel>& emissive = kAllRolesEmissive;
        const Package package = BuildAllRolesPackage();
        Prepared prepared;
        PrepareFixture(fixture, package, prepared);
        if (!Expect(prepared, "all material roles"))
            return false;
        const FabGltfPreparedPackage& result = prepared.Package;
        const std::string streamId = StreamIdOf(prepared.Declaration);

        bool passed = Check(result.Textures.size() == 4 && result.Assets.size() == 6, "four textures plus mesh and material");
        passed &= CheckTexture(result, prepared.Declaration, TextureRole::BaseColor, "texture.base-color",
            TextureColorSpace::Srgb, 4, 2, 3, Flatten(baseOpaque), "base colour");
        passed &= CheckTexture(result, prepared.Declaration, TextureRole::Orm, "texture.orm",
            TextureColorSpace::Linear, 2, 2, 2, Flatten(metallicRoughness), "ORM");
        passed &= CheckTexture(result, prepared.Declaration, TextureRole::Normal, "texture.normal",
            TextureColorSpace::Linear, 2, 2, 2, Flatten(normal), "normal");
        passed &= CheckTexture(result, prepared.Declaration, TextureRole::Emissive, "texture.emissive",
            TextureColorSpace::Srgb, 2, 2, 2, Flatten(emissive), "emissive");
        if (const TextureArtifact* orm = FindTexture(result, TextureRole::Orm))
        {
            // Hand arithmetic, rounded box average (sum + 2) / 4: R 102 -> 26, G 201 -> 50, B 303 -> 76.
            passed &= ExpectMip(*orm, 1, { 26, 50, 76, 255 }, "ORM");
        }
        if (const TextureArtifact* normalTexture = FindTexture(result, TextureRole::Normal))
            passed &= ExpectMip(*normalTexture, 1, { 128, 128, 255, 255 }, "flat normal");
        if (const TextureArtifact* emissiveTexture = FindTexture(result, TextureRole::Emissive))
            passed &= ExpectMip(*emissiveTexture, 1, { 200, 100, 50, 255 }, "flat emissive");
        if (const TextureArtifact* baseTexture = FindTexture(result, TextureRole::BaseColor))
        {
            passed &= Check(baseTexture->Mips.size() == 3 && baseTexture->Mips[1].Width == 2 && baseTexture->Mips[1].Height == 1
                && baseTexture->Mips[2].Width == 1 && baseTexture->Mips[2].Height == 1, "4x2 base colour has 4x2, 2x1 and 1x1 mips");
            passed &= Check(baseTexture->Payload[3] == 255 && baseTexture->Payload[11] == 255,
                "alpha 7 in the source is cooked as opaque 255");
        }

        const MaterialAsset& material = result.Material;
        passed &= Check(material.Name == "Marble" && material.TwoSided && material.AlphaMode == MaterialAlphaMode::Opaque
            && material.ShadingModel == MaterialShadingModel::Standard, "material flags and name");
        passed &= Check(Near(material.BaseColor.X, 0.8) && Near(material.BaseColor.Y, 0.6) && Near(material.BaseColor.Z, 0.4)
            && Near(material.Metallic, 0.25) && Near(material.Roughness, 0.5) && Near(material.NormalScale, 2.0)
            && Near(material.OcclusionStrength, 0.75) && Near(material.EmissiveColor.X, 0.1) && Near(material.EmissiveColor.Y, 0.2)
            && Near(material.EmissiveColor.Z, 0.3) && Near(material.EmissiveStrength, 1.0), "material factors");
        passed &= Check(material.Textures.BaseColor == ComputeFabStableAssetHandle(streamId, AssetType::Texture, "texture.base-color")
            && material.Textures.Orm == ComputeFabStableAssetHandle(streamId, AssetType::Texture, "texture.orm")
            && material.Textures.Normal == ComputeFabStableAssetHandle(streamId, AssetType::Texture, "texture.normal")
            && material.Textures.Emissive == ComputeFabStableAssetHandle(streamId, AssetType::Texture, "texture.emissive")
            && material.Textures.Opacity == kInvalidAssetHandle && material.Textures.CallistoControl == kInvalidAssetHandle,
            "material binds the stable texture handles");
        passed &= Check(material.Samplers.BaseColor == MaterialTextureSampler::LinearClamp
            && material.Samplers.Orm == MaterialTextureSampler::PointWrap
            && material.Samplers.Normal == MaterialTextureSampler::LinearWrap
            && material.Samplers.Emissive == MaterialTextureSampler::PointClamp, "glTF samplers map to material samplers");
        passed &= Check(IsValidMaterialAssetValues(material), "prepared material is valid");
        for (size_t index = 0; index < result.Textures.size(); ++index)
            passed &= Check(result.Assets[2 + index].Handle == result.Textures[index].Asset
                && result.Assets[2 + index].Type == AssetType::Texture, "texture asset order matches artifacts");
        passed &= Check(result.Assets[2].SemanticRole == "texture.base-color" && result.Assets[3].SemanticRole == "texture.orm"
            && result.Assets[4].SemanticRole == "texture.normal" && result.Assets[5].SemanticRole == "texture.emissive"
            && result.Assets[3].GenerationRelativeCookedPath
                == "textures/" + std::to_string(result.Assets[3].Handle) + ".rgba-fallback.spiraltexture", "texture asset roles and paths");
        passed &= CheckIdentity(prepared);
        return passed;
    }

    bool MaterialExternalFilesAndJpeg(ScopedFixtureRoot& fixture)
    {
        Package package = MakeQuadPackage(0);
        Gltf& doc = package.Doc;
        doc.Glb = false;
        doc.BufferUri = "data.bin";
        const std::vector<Pixel> emissive(4, Pixel { 9, 90, 200, 255 });
        doc.Images = { "{\"uri\":\"tex/base.jpg\",\"mimeType\":\"image/jpeg\"}", "{\"uri\":\"tex/my%20base.png\"}" };
        doc.Textures = { "{\"source\":0}", "{\"source\":1}" };
        doc.Materials.push_back("{\"pbrMetallicRoughness\":{\"baseColorTexture\":{\"index\":0}},\"emissiveTexture\":{\"index\":1},"
            "\"emissiveFactor\":[1,1,1]}");
        package.Files.emplace_back("tex/base.jpg", Bytes(std::begin(JpegBaseline444), std::end(JpegBaseline444)));
        package.Files.emplace_back("tex/my base.png", PngOf(2, 2, emissive));
        Prepared prepared;
        PrepareFixture(fixture, package, prepared);
        if (!Expect(prepared, "external gltf with a JPEG and a percent-encoded PNG"))
            return false;
        bool passed = Check(prepared.Declaration.PackageFormat == FabPackageFormat::Gltf && prepared.Package.Textures.size() == 2,
            "external package cooks two textures");
        const TextureArtifact* base = FindTexture(prepared.Package, TextureRole::BaseColor);
        if (!Check(base != nullptr, "JPEG base colour is cooked"))
            return false;
        passed &= Check(base->Mips.size() == 6 && base->Mips[0].Width == 48 && base->Mips[0].Height == 32 && !base->HasAlpha
            && base->ColorSpace == TextureColorSpace::Srgb, "JPEG 48x32 cooks six sRGB mips");
        // Cell (0,0) is about (220,40,40); the decoder is lossy so allow a small margin.
        const u8* texel = base->Payload.data() + (8 * 48 + 8) * 4;
        passed &= Check(std::abs(texel[0] - 220) <= 6 && std::abs(texel[1] - 40) <= 6 && std::abs(texel[2] - 40) <= 6
            && texel[3] == 255, "JPEG cell colour is decoded with alpha 255");
        passed &= CheckTexture(prepared.Package, prepared.Declaration, TextureRole::Emissive, "texture.emissive",
            TextureColorSpace::Srgb, 2, 2, 2, Flatten(emissive), "percent-encoded PNG emissive");
        return passed;
    }

    bool MaterialOrmPacking(ScopedFixtureRoot& fixture)
    {
        const std::vector<Pixel> metallicRoughness { { 1, 2, 3, 255 }, { 4, 5, 6, 255 }, { 7, 8, 9, 255 }, { 10, 11, 12, 255 } };
        const std::vector<Pixel> occlusion { { 100, 0, 0, 255 }, { 101, 1, 1, 255 }, { 102, 2, 2, 255 }, { 103, 3, 3, 255 } };
        const auto build = [&](bool withMetallicRoughness, bool withOcclusion, Prepared& prepared)
        {
            Package package = MakeQuadPackage(0);
            Gltf& doc = package.Doc;
            std::string material = "{\"pbrMetallicRoughness\":{";
            if (withMetallicRoughness)
            {
                doc.Images.push_back("{\"bufferView\":" + std::to_string(doc.AddView(PngOf(2, 2, metallicRoughness))) + ",\"mimeType\":\"image/png\"}");
                doc.Textures.push_back("{\"source\":" + std::to_string(doc.Images.size() - 1) + "}");
                material += "\"metallicRoughnessTexture\":{\"index\":" + std::to_string(doc.Textures.size() - 1) + "}";
            }
            else
                material += "\"metallicFactor\":0.5";
            material += "}";
            if (withOcclusion)
            {
                doc.Images.push_back("{\"bufferView\":" + std::to_string(doc.AddView(PngOf(2, 2, occlusion))) + ",\"mimeType\":\"image/png\"}");
                doc.Textures.push_back("{\"source\":" + std::to_string(doc.Images.size() - 1) + "}");
                material += ",\"occlusionTexture\":{\"index\":" + std::to_string(doc.Textures.size() - 1) + "}";
            }
            doc.Materials.push_back(material + "}");
            return PrepareFixture(fixture, package, prepared);
        };
        bool passed = true;
        {
            Prepared prepared;
            if (build(true, true, prepared))
            {
                passed &= Check(prepared.Package.Textures.size() == 1, "separate maps pack into one ORM texture");
                passed &= CheckTexture(prepared.Package, prepared.Declaration, TextureRole::Orm, "texture.orm",
                    TextureColorSpace::Linear, 2, 2, 2,
                    Flatten({ { 100, 2, 3, 255 }, { 101, 5, 6, 255 }, { 102, 8, 9, 255 }, { 103, 11, 12, 255 } }),
                    "occlusion R with metallic-roughness GB");
            }
            else
                passed &= Expect(prepared, "separate ORM");
        }
        {
            Prepared prepared;
            if (build(true, false, prepared))
                passed &= CheckTexture(prepared.Package, prepared.Declaration, TextureRole::Orm, "texture.orm",
                    TextureColorSpace::Linear, 2, 2, 2,
                    Flatten({ { 255, 2, 3, 255 }, { 255, 5, 6, 255 }, { 255, 8, 9, 255 }, { 255, 11, 12, 255 } }),
                    "metallic-roughness only forces R to 255");
            else
                passed &= Expect(prepared, "metallic-roughness only");
        }
        {
            Prepared prepared;
            if (build(false, true, prepared))
            {
                passed &= CheckTexture(prepared.Package, prepared.Declaration, TextureRole::Orm, "texture.orm",
                    TextureColorSpace::Linear, 2, 2, 2,
                    Flatten({ { 100, 255, 255, 255 }, { 101, 255, 255, 255 }, { 102, 255, 255, 255 }, { 103, 255, 255, 255 } }),
                    "occlusion only forces G and B to 255");
                passed &= Check(Near(prepared.Package.Material.Metallic, 0.5), "factors act directly without a metallic-roughness map");
            }
            else
                passed &= Expect(prepared, "occlusion only");
        }
        {
            // One PNG as base colour and as normal map: two roles, two colour spaces, two handles.
            const std::vector<Pixel> shared { { 5, 6, 7, 255 }, { 8, 9, 10, 255 }, { 11, 12, 13, 255 }, { 14, 15, 16, 255 } };
            Package package = MakeQuadPackage(0);
            Gltf& doc = package.Doc;
            doc.Images.push_back("{\"bufferView\":" + std::to_string(doc.AddView(PngOf(2, 2, shared))) + ",\"mimeType\":\"image/png\"}");
            doc.Textures = { "{\"source\":0}", "{\"source\":0}" };
            doc.Materials.push_back("{\"pbrMetallicRoughness\":{\"baseColorTexture\":{\"index\":0}},\"normalTexture\":{\"index\":1}}");
            Prepared prepared;
            PrepareFixture(fixture, package, prepared);
            if (Expect(prepared, "shared image"))
            {
                passed &= Check(prepared.Package.Textures.size() == 2, "one image in two roles cooks two artifacts");
                passed &= CheckTexture(prepared.Package, prepared.Declaration, TextureRole::BaseColor, "texture.base-color",
                    TextureColorSpace::Srgb, 2, 2, 2, Flatten(shared), "shared as base colour");
                passed &= CheckTexture(prepared.Package, prepared.Declaration, TextureRole::Normal, "texture.normal",
                    TextureColorSpace::Linear, 2, 2, 2, Flatten(shared), "shared as normal");
            }
            else
                passed = false;
        }
        return passed;
    }

    // ---------------------------------------------------------------- rejection fixtures
    // Quad with a 2x2 image bound through `materialBody` as texture 0 (optionally with sampler 0).
    Package TexturedQuad(const std::string& materialBody, const std::string& sampler = {}, bool uvs = true,
        const Bytes* image = nullptr, const char* mime = "image/png")
    {
        Package package = MakeQuadPackage(0, true, uvs);
        const Bytes encoded = image ? *image : PngOf(2, 2, std::vector<Pixel>(4, Pixel { 1, 2, 3, 255 }));
        package.Doc.AddImage(encoded, mime);
        package.Doc.Textures.push_back(sampler.empty() ? "{\"source\":0}" : "{\"source\":0,\"sampler\":0}");
        if (!sampler.empty())
            package.Doc.Samplers.push_back(sampler);
        package.Doc.Materials.push_back(materialBody);
        return package;
    }

    const char* const kBaseColorMaterial = "{\"pbrMetallicRoughness\":{\"baseColorTexture\":{\"index\":0}}}";

    Package WithMaterialExtension(std::string_view extension, std::string_view body)
    {
        Package package = MakeQuadPackage(0);
        package.Doc.Materials.push_back("{\"extensions\":{\"" + std::string(extension) + "\":" + std::string(body) + "}}");
        package.Doc.Used = "[\"" + std::string(extension) + "\"]";
        return package;
    }

    bool RejectFeatureFixtures(ScopedFixtureRoot& fixture)
    {
        bool passed = true;
        {
            Package package = MakeQuadPackage();
            package.Doc.Extra = "\"skins\":[{\"joints\":[0]}]";
            passed &= ExpectReject(fixture, "R-SKIN", package, "skins are not supported");
        }
        {
            Package package = MakeQuadPackage();
            const int input = package.Doc.AddFloats({ 0.0f }, "SCALAR", 1);
            const int output = package.Doc.AddFloats({ 1.0f, 2.0f, 3.0f }, "VEC3", 3);
            package.Doc.Extra = "\"animations\":[{\"samplers\":[{\"input\":" + std::to_string(input) + ",\"output\":"
                + std::to_string(output) + "}],\"channels\":[{\"sampler\":0,\"target\":{\"node\":0,\"path\":\"translation\"}}]}]";
            passed &= ExpectReject(fixture, "R-ANIM", package, "animations are not supported");
        }
        {
            Package package = MakeQuadPackage(-1, true, true, ",\"targets\":[{\"POSITION\":0}]");
            passed &= ExpectReject(fixture, "R-MORPH primitive target", package, "morph targets are not supported");
        }
        {
            Package package = MakeQuadPackage(-1, true, true, ",\"targets\":[{\"POSITION\":0}]");
            package.Doc.Meshes[0].insert(package.Doc.Meshes[0].size() - 1, ",\"weights\":[0.5]");
            passed &= ExpectReject(fixture, "R-MORPH mesh weights", package, "morph targets are not supported");
        }
        for (const char* extension : { "KHR_draco_mesh_compression", "EXT_meshopt_compression", "KHR_mesh_quantization", "EXT_foo" })
        {
            Package package = MakeQuadPackage();
            package.Doc.Required = std::string("[\"") + extension + "\"]";
            package.Doc.Used = package.Doc.Required;
            passed &= ExpectReject(fixture, std::string("R-REQ ") + extension, package,
                std::string("required glTF extension is not supported: ") + extension);
        }
        {
            Package package = MakeQuadPackage(-1, true, true,
                ",\"extensions\":{\"KHR_draco_mesh_compression\":{\"bufferView\":0,\"attributes\":{\"POSITION\":0}}}");
            package.Doc.Used = "[\"KHR_draco_mesh_compression\"]";
            passed &= ExpectReject(fixture, "R-DRACO used with fallback", package, "compressed geometry is not supported");
        }
        {
            Package package = MakeQuadPackage();
            package.Doc.BufferViews.push_back("{\"buffer\":0,\"byteOffset\":0,\"byteLength\":4,\"extensions\":{\"EXT_meshopt_compression\":"
                "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":4,\"byteStride\":4,\"count\":1,\"mode\":\"ATTRIBUTES\"}}}");
            package.Doc.Used = "[\"EXT_meshopt_compression\"]";
            passed &= ExpectReject(fixture, "R-MESHOPT used with fallback", package, "compressed geometry is not supported");
        }
        {
            Package package = MakeQuadPackage();
            package.Doc.Nodes[0] = "{\"mesh\":0,\"extensions\":{\"EXT_mesh_gpu_instancing\":{\"attributes\":{\"TRANSLATION\":0}}}}";
            package.Doc.Used = "[\"EXT_mesh_gpu_instancing\"]";
            passed &= ExpectReject(fixture, "R-GPU-INSTANCING", package, "EXT_mesh_gpu_instancing");
        }
        {
            Package package = TexturedQuad("{\"pbrMetallicRoughness\":{\"baseColorTexture\":{\"index\":0,\"extensions\":"
                "{\"KHR_texture_transform\":{\"offset\":[0.5,0]}}}}}");
            package.Doc.Used = "[\"KHR_texture_transform\"]";
            passed &= ExpectReject(fixture, "R-TEXXFORM", package, "KHR_texture_transform");
        }
        const std::pair<const char*, const char*> materialExtensions[] = {
            { "KHR_materials_clearcoat", "{\"clearcoatFactor\":1.0}" },
            { "KHR_materials_transmission", "{\"transmissionFactor\":0.5}" },
            { "KHR_materials_specular", "{\"specularFactor\":0.5}" },
            { "KHR_materials_ior", "{\"ior\":1.4}" },
            { "KHR_materials_sheen", "{\"sheenRoughnessFactor\":0.5}" },
            { "KHR_materials_unlit", "{}" },
            { "KHR_materials_pbrSpecularGlossiness", "{\"glossinessFactor\":0.5}" },
            { "KHR_materials_emissive_strength", "{\"emissiveStrength\":2.0}" },
            { "KHR_materials_iridescence", "{\"iridescenceFactor\":1.0}" },
            { "KHR_materials_anisotropy", "{\"anisotropyStrength\":0.5}" } };
        for (const auto& [extension, body] : materialExtensions)
            passed &= ExpectReject(fixture, std::string("R-MATEXT ") + extension, WithMaterialExtension(extension, body),
                "unsupported material extension");
        for (const char* mode : { "MASK", "BLEND" })
        {
            Package package = MakeQuadPackage(0);
            package.Doc.Materials.push_back(std::string("{\"alphaMode\":\"") + mode + "\"}");
            passed &= ExpectReject(fixture, std::string("R-ALPHA ") + mode, package, "only OPAQUE alpha mode is supported");
        }
        for (const char* mode : { "1", "5" })
        {
            Package package = MakeQuadPackage(-1, true, true, std::string(",\"mode\":") + mode);
            passed &= ExpectReject(fixture, std::string("R-MODE ") + mode, package, "only triangle primitives are supported");
        }
        {
            Package package;
            package.Doc.AddView(Bytes(4, 0));
            package.Doc.Nodes.push_back("{\"name\":\"empty\"}");
            passed &= ExpectReject(fixture, "R-EMPTY-SCENE", package, "no renderable mesh");
        }
        return passed;
    }

    bool RejectMaterialAndSceneFixtures(ScopedFixtureRoot& fixture)
    {
        bool passed = true;
        {
            Package package;
            Gltf& doc = package.Doc;
            doc.Materials = { "{\"name\":\"A\"}", "{\"name\":\"B\"}" };
            const std::string quad = AddPrimitive(doc, kQuadPositions, &kQuadNormals, &kQuadUv, kQuadIndices, 0);
            const std::string second = AddPrimitive(doc, kQuadPositions, &kQuadNormals, &kQuadUv, kQuadIndices, 1);
            doc.Meshes.push_back("{\"primitives\":[" + quad + "," + second + "]}");
            doc.Nodes.push_back("{\"mesh\":0}");
            passed &= ExpectReject(fixture, "R-MULTIMAT in one mesh", package, "multiple materials are not supported");
        }
        {
            Package package;
            Gltf& doc = package.Doc;
            doc.Materials = { "{\"name\":\"A\"}", "{\"name\":\"B\"}" };
            doc.Meshes.push_back("{\"primitives\":[" + AddPrimitive(doc, kQuadPositions, &kQuadNormals, &kQuadUv, kQuadIndices, 0) + "]}");
            doc.Meshes.push_back("{\"primitives\":[" + AddPrimitive(doc, kQuadPositions, &kQuadNormals, &kQuadUv, kQuadIndices, 1) + "]}");
            doc.Nodes = { "{\"mesh\":0}", "{\"mesh\":1}" };
            doc.Scenes = "[{\"nodes\":[0,1]}]";
            passed &= ExpectReject(fixture, "R-MULTIMAT across nodes", package, "multiple materials are not supported");
        }
        {
            Package package;
            Gltf& doc = package.Doc;
            doc.Materials = { "{\"name\":\"A\"}" };
            const std::string withMaterial = AddPrimitive(doc, kQuadPositions, &kQuadNormals, &kQuadUv, kQuadIndices, 0);
            const std::string without = AddPrimitive(doc, kQuadPositions, &kQuadNormals, &kQuadUv, kQuadIndices);
            doc.Meshes.push_back("{\"primitives\":[" + withMaterial + "," + without + "]}");
            doc.Nodes.push_back("{\"mesh\":0}");
            passed &= ExpectReject(fixture, "R-MIXMAT", package, "multiple materials are not supported");
        }
        passed &= ExpectReject(fixture, "R-NOUV with a texture", TexturedQuad(kBaseColorMaterial, {}, false),
            "textured material requires TEXCOORD_0");
        {
            Package package;
            Gltf& doc = package.Doc;
            const std::vector<float> tint(16, 0.5f);
            std::string primitive = AddPrimitive(doc, kQuadPositions, &kQuadNormals, &kQuadUv, kQuadIndices);
            primitive = WithAttribute(primitive, "COLOR_0", doc.AddFloats(tint, "VEC4", 4));
            doc.Meshes.push_back("{\"primitives\":[" + primitive + "]}");
            doc.Nodes.push_back("{\"mesh\":0}");
            passed &= ExpectReject(fixture, "R-COLOR0 tint", package, "COLOR_0 vertex tint is not supported");
        }
        {
            Package package;
            Gltf& doc = package.Doc;
            const std::string primitive = WithAttribute(AddPrimitive(doc, kQuadPositions, &kQuadNormals, &kQuadUv, kQuadIndices),
                "JOINTS_0", 0);
            doc.Meshes.push_back("{\"primitives\":[" + primitive + "]}");
            doc.Nodes.push_back("{\"mesh\":0}");
            passed &= ExpectReject(fixture, "R-JOINTS", package, "skinned vertex attributes are not supported");
        }
        {
            Package package;
            Gltf& doc = package.Doc;
            const int position = doc.AddFloats(kQuadPositions, "VEC3", 3);
            const int uv = doc.AddAccessor(doc.AddView(Bytes(16, 0)), 5123, 4, "VEC2");
            doc.Meshes.push_back("{\"primitives\":[{\"attributes\":{\"POSITION\":" + std::to_string(position)
                + ",\"TEXCOORD_0\":" + std::to_string(uv) + "},\"indices\":" + std::to_string(doc.AddIndices(kQuadIndices)) + "}]}");
            doc.Nodes.push_back("{\"mesh\":0}");
            passed &= ExpectReject(fixture, "R-UVTYPE integer UV is not normalized", package,
                "TEXCOORD_0 must be a VEC2 FLOAT or normalized unsigned accessor");
        }
        passed &= ExpectReject(fixture, "R-TEXCOORD1",
            TexturedQuad("{\"pbrMetallicRoughness\":{\"baseColorTexture\":{\"index\":0,\"texCoord\":1}}}"),
            "TEXCOORD set other than 0");
        const std::pair<const char*, const char*> samplers[] = {
            { "R-SAMPLER mirrored repeat", "{\"wrapS\":33648,\"wrapT\":33648}" },
            { "R-SAMPLER mixed wrap", "{\"wrapS\":33071,\"wrapT\":10497}" },
            { "R-SAMPLER nearest magnify with linear minify", "{\"magFilter\":9728,\"minFilter\":9987}" },
            { "R-SAMPLER linear magnify with nearest minify", "{\"magFilter\":9729,\"minFilter\":9984}" } };
        for (const auto& [label, sampler] : samplers)
            passed &= ExpectReject(fixture, label, TexturedQuad(kBaseColorMaterial, sampler), "unsupported texture sampler");
        {
            Package package = MakeQuadPackage();
            package.Doc.Scene.clear();
            package.Doc.Scenes = "[{\"nodes\":[0]},{\"nodes\":[0]}]";
            passed &= ExpectReject(fixture, "R-SCENE two scenes without a default", package, "no default scene");
            package.Doc.Scenes.clear();
            passed &= ExpectReject(fixture, "R-SCENE zero scenes", package, "no default scene");
        }
        {
            Package package = MakeQuadPackage(0);
            Gltf& doc = package.Doc;
            doc.AddImage(PngOf(2, 2, std::vector<Pixel>(4, Pixel { 1, 2, 3, 255 })), "image/png");
            doc.AddImage(PngOf(4, 2, std::vector<Pixel>(8, Pixel { 1, 2, 3, 255 })), "image/png");
            doc.Textures = { "{\"source\":0}", "{\"source\":1}" };
            doc.Materials.push_back("{\"pbrMetallicRoughness\":{\"metallicRoughnessTexture\":{\"index\":0}},"
                "\"occlusionTexture\":{\"index\":1}}");
            passed &= ExpectReject(fixture, "R-ORM-DIMS", package, "differ in size");
        }
        {
            Package package = MakeQuadPackage(0);
            Gltf& doc = package.Doc;
            doc.AddImage(PngOf(2, 2, std::vector<Pixel>(4, Pixel { 1, 2, 3, 255 })), "image/png");
            doc.Samplers = { "{\"wrapS\":33071,\"wrapT\":33071}" };
            doc.Textures = { "{\"source\":0}", "{\"source\":0,\"sampler\":0}" };
            doc.Materials.push_back("{\"pbrMetallicRoughness\":{\"metallicRoughnessTexture\":{\"index\":0}},"
                "\"occlusionTexture\":{\"index\":1}}");
            passed &= ExpectReject(fixture, "R-ORM-SAMPLERS", package, "use different samplers");
        }
        {
            Package package = MakeQuadPackage(0);
            package.Doc.Textures = { "{}" };
            package.Doc.Materials.push_back(kBaseColorMaterial);
            passed &= ExpectReject(fixture, "R-NOIMAGE", package, "has no PNG or JPEG image source");
        }
        {
            Package package = MakeQuadPackage(0);
            package.Doc.Textures = { "{\"source\":0}" };
            package.Doc.Images.push_back("{\"mimeType\":\"image/png\"}");
            package.Doc.Materials.push_back(kBaseColorMaterial);
            LocalPackageSnapshot snapshot;
            std::string error;
            passed &= CheckWith(!CreateSnapshot(fixture, package, snapshot, error)
                && Contains(error, "neither a dependency URI nor an embedded buffer view"), "snapshot rejects an image with no source before Prepare", error);
        }
        const Bytes webp = ToBytes("RIFF\x24\x00\x00\x00WEBPVP8 \x18\x00\x00\x00 padding payload bytes");
        passed &= ExpectReject(fixture, "R-IMGMAGIC WebP", TexturedQuad(kBaseColorMaterial, {}, true, &webp, "image/webp"),
            "texture image rejected");
        const Bytes text = ToBytes("this is not an image at all");
        passed &= ExpectReject(fixture, "R-IMGMAGIC text", TexturedQuad(kBaseColorMaterial, {}, true, &text),
            "image content magic is unsupported");
        Bytes truncated = PngOf(2, 2, std::vector<Pixel>(4, Pixel { 1, 2, 3, 255 }));
        truncated.resize(truncated.size() - 20);
        passed &= ExpectReject(fixture, "R-IMGCORRUPT truncated PNG", TexturedQuad(kBaseColorMaterial, {}, true, &truncated),
            "texture image rejected");
        {
            FabGltfPrepareOptions options;
            options.ImageLimits.MaximumPixels = 1;
            passed &= ExpectReject(fixture, "R-IMGBIG", TexturedQuad(kBaseColorMaterial), "texture image rejected", options);
        }
        {
            const Bytes png = PngOf(2, 2, std::vector<Pixel>(4, Pixel { 1, 2, 3, 255 }));
            passed &= ExpectReject(fixture, "R-MIME", TexturedQuad(kBaseColorMaterial, {}, true, &png, "image/jpeg"),
                "MIME type disagrees with its content");
        }
        return passed;
    }

    bool RejectGeometryAndLimitFixtures(ScopedFixtureRoot& fixture)
    {
        bool passed = true;
        {
            Package package = MakeQuadPackage();
            package.Doc.Nodes[0] = "{\"mesh\":0,\"scale\":[0,1,1]}";
            passed &= ExpectReject(fixture, "R-SINGULAR", package, "singular");
        }
        {
            Package package;
            const std::vector<float> zeroNormals(12, 0.0f);
            package.Doc.Meshes.push_back("{\"primitives\":["
                + AddPrimitive(package.Doc, kQuadPositions, &zeroNormals, &kQuadUv, kQuadIndices) + "]}");
            package.Doc.Nodes.push_back("{\"mesh\":0}");
            passed &= ExpectReject(fixture, "R-ZERONORMAL", package, "authored normal is zero-length");
        }
        {
            Package package;
            const std::vector<float> positions { 0, 0, 0, 0, 0, 0, 1, 0, 0 };
            package.Doc.Meshes.push_back("{\"primitives\":["
                + AddPrimitive(package.Doc, positions, nullptr, nullptr, { 0, 1, 2 }) + "]}");
            package.Doc.Nodes.push_back("{\"mesh\":0}");
            passed &= ExpectReject(fixture, "R-DEGEN derived normals", package, "degenerate triangles");
        }
        {
            Package package;
            const std::vector<float> positions { 0, 0, 0, 1, 0, 0, 0, 1, 0 };
            package.Doc.Meshes.push_back("{\"primitives\":["
                + AddPrimitive(package.Doc, positions, nullptr, nullptr, { 0, 1, 7 }) + "]}");
            package.Doc.Nodes.push_back("{\"mesh\":0}");
            passed &= ExpectReject(fixture, "R-IDXRANGE", package, "glTF structure is invalid");
        }
        {
            Package package;
            package.Doc.Meshes.push_back("{\"primitives\":["
                + AddPrimitive(package.Doc, { 0, 0, 0, 1, 0, 0 }, nullptr, nullptr, {}) + "]}");
            package.Doc.Nodes.push_back("{\"mesh\":0}");
            passed &= ExpectReject(fixture, "R-NOTRIANGLES", package, "non-multiple-of-three");
        }
        {
            FabGltfPrepareOptions options;
            options.Limits.MaximumVertices = 3;
            passed &= ExpectReject(fixture, "R-LIMIT-VERT", MakeQuadPackage(), "exceeds the configured limit", options);
        }
        {
            FabGltfPrepareOptions options;
            options.Limits.MaximumIndices = 5;
            passed &= ExpectReject(fixture, "R-LIMIT-INDEX", MakeQuadPackage(), "exceeds the configured limit", options);
        }
        {
            Package package = MakeQuadPackage();
            package.Doc.Nodes = { "{\"mesh\":0}", "{\"mesh\":0}" };
            package.Doc.Scenes = "[{\"nodes\":[0,1]}]";
            FabGltfPrepareOptions options;
            options.Limits.MaximumPrimitiveInstances = 1;
            passed &= ExpectReject(fixture, "R-LIMIT-INST", package, "primitive instance count exceeds", options);
            options = {};
            options.Limits.MaximumNodeCount = 1;
            passed &= ExpectReject(fixture, "R-LIMIT-NODE", package, "node count exceeds", options);
        }
        {
            FabGltfPrepareOptions options;
            options.Limits.MaximumRootBytes = 16;
            passed &= ExpectReject(fixture, "R-LIMIT-ROOT", MakeQuadPackage(), "exceeds the configured size limit", options);
        }
        {
            FabGltfPrepareOptions options;
            options.Limits.MaximumTotalDecodedImageBytes = 8;
            passed &= ExpectReject(fixture, "R-LIMIT-DECODED", TexturedQuad(kBaseColorMaterial),
                "aggregate size limit", options);
        }
        {
            Package package = MakeQuadPackage();
            package.Doc.Glb = false;
            package.Doc.BufferUri = "data.bin";
            FabGltfPrepareOptions options;
            options.Limits.MaximumTotalBufferBytes = 8;
            passed &= ExpectReject(fixture, "R-LIMIT-BUFFER", package, "aggregate size limit", options);
        }
        return passed;
    }

    bool RejectDeclarationFixtures(ScopedFixtureRoot& fixture)
    {
        LocalPackageSnapshot snapshot;
        std::string error;
        const Package package = MakeQuadPackage();
        if (!CheckWith(CreateSnapshot(fixture, package, snapshot, error), "declaration snapshot", error))
            return false;
        struct Case
        {
            const char* Label;
            const char* Diagnostic;
            void (*Mutate)(FabImportReceipt&, const LocalPackageSnapshot&);
        };
        const Case cases[] = {
            { "R-FORMAT declared glTF for a GLB root", "declared package format does not match",
                [](FabImportReceipt& r, const LocalPackageSnapshot&) { r.PackageFormat = FabPackageFormat::Gltf; } },
            { "R-FORMAT unknown", "declared package format does not match",
                [](FabImportReceipt& r, const LocalPackageSnapshot&) { r.PackageFormat = FabPackageFormat::Unknown; } },
            { "R-DECL unconfirmed metadata", "receipt declaration is invalid",
                [](FabImportReceipt& r, const LocalPackageSnapshot&) { r.MetadataConfirmedByUser = false; } },
            { "R-DECL Reference Only", "receipt declaration is invalid",
                [](FabImportReceipt& r, const LocalPackageSnapshot&) { r.LicenseFamily = FabLicenseFamily::ReferenceOnly; } },
            { "R-DECL CC-BY without attribution", "receipt declaration is invalid",
                [](FabImportReceipt& r, const LocalPackageSnapshot&) {
                    r.LicenseFamily = FabLicenseFamily::CreativeCommonsAttribution; r.LicenseTier = FabLicenseTier::NotApplicable; } },
            { "R-DECL malformed source digest", "cannot form a generation id",
                [](FabImportReceipt& r, const LocalPackageSnapshot&) { r.SourceSha256 = "ABC"; } },
            { "R-DECL bad product identity", "cannot form a stream id",
                [](FabImportReceipt& r, const LocalPackageSnapshot&) { r.ProductIdentity = "https://example.com/x"; } },
            { "R-DECL foreign stream id", "values owned by the cook",
                [](FabImportReceipt& r, const LocalPackageSnapshot&) { r.StreamId = std::string(64, 'a'); } },
            { "R-DECL foreign tree digest", "values owned by the cook",
                [](FabImportReceipt& r, const LocalPackageSnapshot&) { r.ExpandedTreeSha256 = std::string(64, 'b'); } },
            { "R-DECL foreign importer version", "values owned by the cook",
                [](FabImportReceipt& r, const LocalPackageSnapshot&) { r.ImporterVersion = "Other/1"; } },
            { "R-DECL supplied assets", "values owned by the cook",
                [](FabImportReceipt& r, const LocalPackageSnapshot&) { r.Assets.resize(1); } } };
        bool passed = true;
        for (const Case& item : cases)
        {
            FabImportReceipt declaration = MakeDeclaration(snapshot, FabPackageFormat::Glb);
            item.Mutate(declaration, snapshot);
            FabGltfPreparedPackage out = MakeSentinelPackage();
            std::string reported = "stale";
            passed &= Check(!PrepareFabGltfPackage(snapshot, declaration, {}, out, reported), std::string(item.Label) + " is rejected");
            passed &= Check(Contains(reported, item.Diagnostic),
                std::string(item.Label) + " reports '" + item.Diagnostic + "' but reported '" + reported + "'");
            passed &= Check(IsSentinel(out), std::string(item.Label) + " leaves the output untouched");
        }
        // A declaration that restates the computed identity exactly is accepted.
        FabImportReceipt restated = MakeDeclaration(snapshot, FabPackageFormat::Glb);
        restated.StreamId = StreamIdOf(restated);
        restated.GenerationId = ComputeFabGenerationId(restated.StreamId, restated.SourceSha256, snapshot.GetTreeSha256());
        restated.ExpandedTreeSha256 = snapshot.GetTreeSha256();
        restated.ImporterVersion = std::string(kFabGltfImporterVersion);
        restated.CookerVersion = std::string(kFabGltfCookerVersion);
        FabGltfPreparedPackage accepted;
        std::string acceptedError;
        passed &= CheckWith(PrepareFabGltfPackage(snapshot, restated, {}, accepted, acceptedError), "an equal restated identity is accepted", acceptedError);
        return passed;
    }

    Bytes HostileRoot(std::string_view uri)
    {
        return ToBytes("{\"asset\":{\"version\":\"2.0\"},\"buffers\":[{\"uri\":\"" + std::string(uri) + "\",\"byteLength\":4}]}");
    }

    std::filesystem::path MakeRootlessDirectory(ScopedFixtureRoot& fixture)
    {
        const std::filesystem::path directory = fixture.Path("rootless");
        WriteBytes(directory / "data.bin", Bytes { 1, 2, 3, 4 });
        return directory;
    }

    bool RejectDependencyUris(ScopedFixtureRoot& fixture)
    {
        struct Case
        {
            const char* Uri;           // JSON text of the uri value
            const char* Decoded;       // what the resolver sees after JSON unescaping
            const char* Diagnostic;
        };
        const Case cases[] = {
            { "http://example.com/x.bin", "http://example.com/x.bin", "schemes are not allowed" },
            { "https://example.com/x.bin", "https://example.com/x.bin", "schemes are not allowed" },
            { "file:///etc/passwd", "file:///etc/passwd", "schemes are not allowed" },
            { "//host/share/x.bin", "//host/share/x.bin", "empty, absolute" },
            { "/etc/passwd", "/etc/passwd", "empty, absolute" },
            { "data:application/octet-stream;base64,AAAAAA==", "data:application/octet-stream;base64,AAAAAA==", "data URI dependencies are not admitted" },
            { "../outside.bin", "../outside.bin", "traversal" },
            { "%2e%2e/outside.bin", "%2e%2e/outside.bin", "traversal" },
            { "a%2Fb.bin", "a%2Fb.bin", "encoded separator" },
            { "a%5Cb.bin", "a%5Cb.bin", "encoded separator" },
            { "sub\\\\x.bin", "sub\\x.bin", "backslash" },
            { "x.bin?query", "x.bin?query", "query, fragment, or backslash" },
            { "x.bin#fragment", "x.bin#fragment", "query, fragment, or backslash" },
            { "%zz.bin", "%zz.bin", "invalid percent encoding" },
            { "a./x.bin", "a./x.bin", "not portable" },
            { "", "", "empty, absolute" } };
        bool passed = true;
        for (const Case& item : cases)
        {
            Package package;
            package.Doc.Glb = false;
            package.HasRootOverride = true;
            package.RootOverride = HostileRoot(item.Uri);
            LocalPackageSnapshot snapshot;
            std::string error;
            passed &= Check(!CreateSnapshot(fixture, package, snapshot, error) && Contains(error, item.Diagnostic),
                std::string("snapshot rejects dependency '") + item.Decoded + "' with '" + item.Diagnostic + "' but reported '" + error + "'");
            std::string resolved = "sentinel";
            std::string resolveError = "stale";
            passed &= Check(!ResolveGltfDependencyUri("a/model.gltf", item.Decoded, {}, resolved, resolveError)
                && Contains(resolveError, item.Diagnostic) && resolved == "sentinel",
                std::string("resolver rejects '") + item.Decoded + "' with '" + item.Diagnostic + "' but reported '" + resolveError + "'");
        }
        {
            Package package;
            package.Doc.Glb = false;
            package.HasRootOverride = true;
            package.RootOverride = HostileRoot("missing.bin");
            LocalPackageSnapshot snapshot;
            std::string error;
            passed &= CheckWith(!CreateSnapshot(fixture, package, snapshot, error) && Contains(error, "missing from the immutable snapshot"), "snapshot rejects a missing dependency", error);
            package.Files.emplace_back("missing.bin", Bytes { 1, 2 });
            error.clear();
            passed &= CheckWith(!CreateSnapshot(fixture, package, snapshot, error) && Contains(error, "shorter than its declared byteLength"), "snapshot rejects an undersized buffer", error);
        }
        {
            Package package;
            package.Doc.Glb = false;
            package.HasRootOverride = true;
            package.RootOverride = ToBytes("{\"asset\":{\"version\":\"2.0\"},\"images\":[{\"uri\":\"tex/missing.png\"}]}");
            LocalPackageSnapshot snapshot;
            std::string error;
            passed &= Check(!CreateSnapshot(fixture, package, snapshot, error) && Contains(error, "missing from the immutable snapshot"),
                "snapshot rejects a missing image dependency: " + error);

            Package ambiguous = MakeQuadPackage();
            ambiguous.Files.emplace_back("second.gltf", ToBytes("{}"));
            error.clear();
            passed &= Check(!CreateSnapshot(fixture, ambiguous, snapshot, error) && Contains(error, "exactly one glTF or GLB root"),
                "snapshot rejects two roots: " + error);

            const std::filesystem::path rootless = MakeRootlessDirectory(fixture);
            const std::filesystem::path staging = fixture.Path("stg-rootless");
            std::error_code filesystemError;
            std::filesystem::create_directories(staging, filesystemError);
            std::filesystem::permissions(staging, std::filesystem::perms::owner_all,
                std::filesystem::perm_options::replace, filesystemError);
            error.clear();
            passed &= Check(!LocalPackageSnapshot::Create(rootless, staging, {}, snapshot, error)
                && Contains(error, "exactly one glTF or GLB root"), "snapshot rejects a package with no root: " + error);
        }
        struct Accepted
        {
            const char* Root;
            const char* Uri;
            const char* Resolved;
        };
        const Accepted accepted[] = {
            { "model.gltf", "data.bin", "data.bin" },
            { "a/b/model.gltf", "tex/my%20base.png", "a/b/tex/my base.png" },
            { "a/model.gltf", "%41.bin", "a/A.bin" },
            { "model.glb", "dir/sub/x.png", "dir/sub/x.png" } };
        for (const Accepted& item : accepted)
        {
            std::string resolved;
            std::string error;
            passed &= Check(ResolveGltfDependencyUri(item.Root, item.Uri, {}, resolved, error) && resolved == item.Resolved && error.empty(),
                std::string("resolver maps '") + item.Uri + "' under '" + item.Root + "' to '" + item.Resolved + "' but produced '" + resolved + "'");
        }
        LocalPackageSnapshotLimits tight;
        tight.MaximumPathBytes = 8;
        std::string resolved = "sentinel";
        std::string error;
        passed &= Check(!ResolveGltfDependencyUri("model.gltf", "long-name.bin", tight, resolved, error) && resolved == "sentinel",
            "resolver enforces the supplied path limit");
        return passed;
    }

    bool PrepareCancelSweep(ScopedFixtureRoot& fixture)
    {
        const Package package = BuildAllRolesPackage();
        LocalPackageSnapshot snapshot;
        std::string error;
        if (!CheckWith(CreateSnapshot(fixture, package, snapshot, error), "sweep snapshot", error))
            return false;
        const FabImportReceipt declaration = MakeDeclaration(snapshot, FabPackageFormat::Glb);
        const std::vector<std::string> before = Listing(fixture.Root());

        size_t polls = 0;
        std::set<FabGltfStage> stages;
        FabGltfPrepareOptions options;
        options.IsCancelled = [&polls]() { ++polls; return false; };
        options.TestHook = [&stages](FabGltfStage stage, std::string_view) { stages.insert(stage); };
        FabGltfPreparedPackage complete;
        bool passed = CheckWith(PrepareFabGltfPackage(snapshot, declaration, options, complete, error), "uncancelled sweep run", error);
        for (const FabGltfStage stage : { FabGltfStage::Started, FabGltfStage::RootRead, FabGltfStage::Parsed,
                 FabGltfStage::BuffersLoaded, FabGltfStage::GeometryBaked, FabGltfStage::ImageDecoded,
                 FabGltfStage::TextureCooked, FabGltfStage::MaterialBuilt, FabGltfStage::Completed })
            passed &= Check(stages.contains(stage), "prepare reports stage " + std::to_string(static_cast<int>(stage)));
        passed &= Check(Listing(fixture.Root()) == before, "prepare writes no file");
        passed &= Check(polls > 12 && polls < 2000, "cancellation is polled at a plausible number of points: " + std::to_string(polls));

        const size_t totalPolls = polls;
        for (size_t cancelAt = 1; cancelAt <= totalPolls; ++cancelAt)
        {
            size_t calls = 0;
            FabGltfPrepareOptions cancelling;
            cancelling.IsCancelled = [&calls, cancelAt]() { return ++calls >= cancelAt; };
            FabGltfPreparedPackage out = MakeSentinelPackage();
            error.clear();
            const bool succeeded = PrepareFabGltfPackage(snapshot, declaration, cancelling, out, error);
            if (!Check(!succeeded && Contains(error, "cancelled") && IsSentinel(out),
                "prepare cancelled at poll " + std::to_string(cancelAt) + " fails cleanly: " + error))
            {
                passed = false;
                break;
            }
        }
        size_t calls = 0;
        FabGltfPrepareOptions late;
        late.IsCancelled = [&calls, totalPolls]() { return ++calls > totalPolls; };
        FabGltfPreparedPackage out;
        passed &= CheckWith(PrepareFabGltfPackage(snapshot, declaration, late, out, error), "a cancel request after the last poll is not observed", error);
        passed &= Check(Listing(fixture.Root()) == before, "the cancellation sweep writes no file");
        return passed;
    }

    // ---------------------------------------------------------------- stage and candidate fixtures
    bool SameMeshExact(const MeshArtifact& left, const MeshArtifact& right)
    {
        if (left.Asset != right.Asset || left.SourcePath != right.SourcePath || left.Primitives.size() != right.Primitives.size()
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
        return std::memcmp(left.Vertices.data(), right.Vertices.data(), left.Vertices.size() * sizeof(MeshArtifactVertex)) == 0;
    }

    bool SameTextureExact(const TextureArtifact& left, const TextureArtifact& right)
    {
        if (left.Asset != right.Asset || left.SourcePath != right.SourcePath || left.Role != right.Role
            || left.ColorSpace != right.ColorSpace || left.TargetProfile != right.TargetProfile
            || left.CookedFormat != right.CookedFormat || left.HasAlpha != right.HasAlpha || left.Payload != right.Payload
            || left.Mips.size() != right.Mips.size())
            return false;
        for (size_t index = 0; index < left.Mips.size(); ++index)
            if (left.Mips[index].Width != right.Mips[index].Width || left.Mips[index].Height != right.Mips[index].Height
                || left.Mips[index].ByteOffset != right.Mips[index].ByteOffset || left.Mips[index].ByteSize != right.Mips[index].ByteSize)
                return false;
        return true;
    }

    bool SameMaterialExact(const MaterialAsset& left, const MaterialAsset& right)
    {
        const auto same = [](const Math::Vec3& a, const Math::Vec3& b) { return a.X == b.X && a.Y == b.Y && a.Z == b.Z; };
        return left.Name == right.Name && left.ShadingModel == right.ShadingModel && left.AlphaMode == right.AlphaMode
            && left.TwoSided == right.TwoSided && same(left.BaseColor, right.BaseColor) && left.Metallic == right.Metallic
            && left.Roughness == right.Roughness && left.NormalScale == right.NormalScale
            && left.OcclusionStrength == right.OcclusionStrength && same(left.EmissiveColor, right.EmissiveColor)
            && left.EmissiveStrength == right.EmissiveStrength
            && left.Textures.BaseColor == right.Textures.BaseColor && left.Textures.Normal == right.Textures.Normal
            && left.Textures.Orm == right.Textures.Orm && left.Textures.Emissive == right.Textures.Emissive
            && left.Samplers.BaseColor == right.Samplers.BaseColor && left.Samplers.Normal == right.Samplers.Normal
            && left.Samplers.Orm == right.Samplers.Orm && left.Samplers.Emissive == right.Samplers.Emissive;
    }

    bool SameRegistryDeep(const AssetRegistry& left, const AssetRegistry& right)
    {
        const auto& a = left.GetAssets();
        const auto& b = right.GetAssets();
        if (a.size() != b.size() || left.GetCookedArtifactBasePath() != right.GetCookedArtifactBasePath())
            return false;
        for (size_t index = 0; index < a.size(); ++index)
            if (a[index].Handle != b[index].Handle || a[index].Type != b[index].Type || a[index].SourcePath != b[index].SourcePath
                || a[index].Name != b[index].Name || a[index].SourcePolicy != b[index].SourcePolicy
                || a[index].CookedRoot != b[index].CookedRoot)
                return false;
        return true;
    }

    std::string FileSha256(const std::filesystem::path& path)
    {
        const Bytes bytes = ReadBytes(path);
        return Sha256Builder::ToHex(Sha256Builder::HashBytes(bytes));
    }

    std::filesystem::path MakeDirectory(const ScopedFixtureRoot& fixture, std::string_view name)
    {
        const std::filesystem::path path = fixture.Path(name);
        std::error_code error;
        std::filesystem::create_directories(path, error);
        return path;
    }

    bool StageInto(const std::filesystem::path& base, const FabGltfPreparedPackage& prepared,
        FabGltfStagedGeneration& staged, std::string& error, const FabGltfStageOptions& options = {})
    {
        return StageFabGltfGeneration(prepared, base, options, staged, error);
    }

    // Registers the staged assets the way a controller would and returns a registry rooted at `base`.
    AssetRegistry RegistryFor(const FabGltfPreparedPackage& prepared, const std::filesystem::path& base)
    {
        AssetRegistry registry;
        const std::string root = GetFabCookedRoot(prepared.ReceiptDraft.GenerationId);
        for (const FabGltfPreparedAsset& asset : prepared.Assets)
        {
            AssetMetadata metadata;
            metadata.Handle = asset.Handle;
            metadata.Type = asset.Type;
            metadata.SourcePath = asset.RegistrySourcePath;
            metadata.Name = asset.Name;
            metadata.SourcePolicy = AssetSourcePolicy::ImmutablePackage;
            metadata.CookedRoot = root;
            registry.RegisterAsset(metadata);
        }
        registry.SetCookedArtifactBasePath(base);
        return registry;
    }

    bool CheckResolvedThroughRealLoaders(const AssetRegistry& registry, const FabGltfPreparedPackage& prepared,
        std::string_view label)
    {
        bool passed = true;
        std::string error;
        MeshArtifact mesh;
        passed &= Check(ResolveMeshArtifact(registry, prepared.Assets[0].Handle, mesh, error) && SameMeshExact(mesh, prepared.Mesh),
            std::string(label) + " mesh resolves through the registry: " + error);
        for (size_t index = 0; index < prepared.Textures.size(); ++index)
        {
            TextureArtifactVariantSet variants;
            error.clear();
            passed &= Check(ResolveTextureArtifactVariantSet(registry, prepared.Textures[index].Asset,
                    TextureTargetProfile::RGBAFallback, variants, error)
                && SameTextureExact(variants.Preferred, prepared.Textures[index]),
                std::string(label) + " texture " + std::to_string(index) + " resolves through the registry: " + error);
        }
        const AssetMetadata* material = registry.GetAsset(prepared.Assets[1].Handle);
        MaterialAsset loaded;
        passed &= Check(material != nullptr && MaterialAsset::LoadFromFile(GetFabGltfCookedMaterialPath(material->Handle,
                material->CookedRoot, registry.GetCookedArtifactBasePath()), loaded)
            && SameMaterialExact(loaded, prepared.Material), std::string(label) + " material loads from its rooted path");
        return passed;
    }

    std::vector<std::string> ExpectedGenerationListing(const FabGltfPreparedPackage& prepared)
    {
        const std::string root = "fab/" + prepared.ReceiptDraft.GenerationId;
        std::vector<std::string> expected { "fab/", root + "/", root + "/materials/", root + "/meshes/", root + "/textures/" };
        for (const FabGltfPreparedAsset& asset : prepared.Assets)
            expected.push_back(root + "/" + asset.GenerationRelativeCookedPath);
        std::sort(expected.begin(), expected.end());
        return expected;
    }

    bool StageDeterministicLayout(ScopedFixtureRoot& fixture)
    {
        bool passed = Check(std::locale() == std::locale::classic(), "the global C++ locale is classic");
        const Package package = BuildAllRolesPackage();
        Prepared first;
        Prepared second;
        if (!CheckWith(PrepareFixture(fixture, package, first), "first independent snapshot prepares", first.Error)
            || !CheckWith(PrepareFixture(fixture, package, second), "second independent snapshot prepares", second.Error))
            return false;
        passed &= Check(first.Snapshot.GetTreeSha256() == second.Snapshot.GetTreeSha256()
            && first.Package.ReceiptDraft.GenerationId == second.Package.ReceiptDraft.GenerationId,
            "identical bytes in separate snapshots give one generation id");
        bool sameAssets = first.Package.Assets.size() == second.Package.Assets.size();
        for (size_t index = 0; sameAssets && index < first.Package.Assets.size(); ++index)
            sameAssets = first.Package.Assets[index].Handle == second.Package.Assets[index].Handle
                && first.Package.Assets[index].LogicalPath == second.Package.Assets[index].LogicalPath
                && first.Package.Assets[index].GenerationRelativeCookedPath == second.Package.Assets[index].GenerationRelativeCookedPath
                && first.Package.Assets[index].RegistrySourcePath == second.Package.Assets[index].RegistrySourcePath;
        passed &= Check(sameAssets, "identical bytes give identical handles and paths");
        passed &= Check(SameMeshExact(first.Package.Mesh, second.Package.Mesh), "identical bytes give an identical mesh");

        const std::filesystem::path baseA = MakeDirectory(fixture, "base-a");
        const std::filesystem::path baseB = MakeDirectory(fixture, "base-b");
        std::vector<std::pair<FabGltfStage, std::string>> events;
        FabGltfStageOptions recording;
        recording.TestHook = [&events](FabGltfStage stage, std::string_view label) { events.emplace_back(stage, std::string(label)); };
        FabGltfStagedGeneration stagedA;
        FabGltfStagedGeneration stagedB;
        std::string error;
        if (!CheckWith(StageInto(baseA, first.Package, stagedA, error, recording), "stage A", error)
            || !CheckWith(StageInto(baseB, second.Package, stagedB, error), "stage B", error))
            return false;
        passed &= Check(!events.empty() && events.front().first == FabGltfStage::Started
            && events.back().first == FabGltfStage::StagingCompleted
            && std::count_if(events.begin(), events.end(), [](const auto& event) { return event.first == FabGltfStage::ArtifactWritten; })
                == static_cast<std::ptrdiff_t>(2 * first.Package.Assets.size()), "stage reports each artifact before and after it is written");

        passed &= Check(Listing(baseA) == ExpectedGenerationListing(first.Package), "staging creates exactly the expected tree");
        passed &= Check(Listing(baseA) == Listing(baseB), "independent stagings produce the same tree");
        passed &= Check(stagedA.CookedRoot == "fab/" + first.Package.ReceiptDraft.GenerationId
            && stagedA.CookedRoot == stagedB.CookedRoot && AssetRegistry::IsValidCookedRoot(stagedA.CookedRoot),
            "cooked root is the stable fab/<generation> path");

        bool bytesEqual = true;
        bool hashesMatch = stagedA.Receipt.Assets.size() == first.Package.Assets.size();
        for (size_t index = 0; hashesMatch && index < first.Package.Assets.size(); ++index)
        {
            const std::string relative = stagedA.CookedRoot + "/" + first.Package.Assets[index].GenerationRelativeCookedPath;
            bytesEqual = bytesEqual && ReadBytes(baseA / relative) == ReadBytes(baseB / relative) && !ReadBytes(baseA / relative).empty();
            hashesMatch = hashesMatch && stagedA.Receipt.Assets[index].ArtifactSha256 == FileSha256(baseA / relative)
                && stagedA.Receipt.Assets[index].ArtifactSha256 == stagedB.Receipt.Assets[index].ArtifactSha256;
        }
        passed &= Check(bytesEqual, "independent stagings write byte-identical files");
        passed &= Check(hashesMatch, "receipt artifact hashes equal the SHA-256 of the staged bytes");
        passed &= Check(stagedA.Receipt.Assets == stagedB.Receipt.Assets, "identical receipt asset records");
        FabImportReceipt validated = stagedA.Receipt;
        validated.Relation = FabGenerationRelation::Initial;
        std::string receiptError;
        passed &= CheckWith(ValidateFabImportReceipt(validated, receiptError), "staged receipt validates", receiptError);

        passed &= CheckResolvedThroughRealLoaders(RegistryFor(first.Package, baseA), first.Package, "staged generation");

        // Create-once: a second stage of the same generation refuses and changes nothing.
        const std::vector<std::string> before = Listing(baseA);
        const Bytes meshBefore = ReadBytes(baseA / stagedA.CookedRoot / first.Package.Assets[0].GenerationRelativeCookedPath);
        FabGltfStagedGeneration again;
        error.clear();
        passed &= CheckWith(!StageInto(baseA, first.Package, again, error) && Contains(error, "already exists"), "a second stage of the same generation is refused", error);
        passed &= Check(Listing(baseA) == before
            && ReadBytes(baseA / stagedA.CookedRoot / first.Package.Assets[0].GenerationRelativeCookedPath) == meshBefore,
            "the refused stage leaves the existing generation untouched");

        const std::filesystem::path missing = fixture.Path("no-such-base");
        error.clear();
        passed &= Check(!StageInto(missing, first.Package, again, error) && Contains(error, "staging base"), "missing staging base is refused");
        error.clear();
        passed &= Check(!StageInto("relative-base", first.Package, again, error) && Contains(error, "staging base"), "relative staging base is refused");
        FabGltfPreparedPackage incomplete = first.Package;
        incomplete.Assets.pop_back();
        const std::filesystem::path baseC = MakeDirectory(fixture, "base-c");
        error.clear();
        passed &= Check(!StageInto(baseC, incomplete, again, error) && Contains(error, "incomplete") && Listing(baseC).empty(),
            "an inconsistent prepared package is refused without writing");
        return passed;
    }

    bool CandidateReplacementPreservesOldResolvers(ScopedFixtureRoot& fixture)
    {
        bool passed = true;
        const std::filesystem::path project = MakeDirectory(fixture, "project");
        std::vector<Pixel> changed = kAllRolesBase;
        changed[0] = { 11, 11, 12, 255 };
        Prepared a, b, sameAsA;
        if (!CheckWith(PrepareFixture(fixture, BuildAllRolesPackage(), a), "A prepares", a.Error)
            || !CheckWith(PrepareFixture(fixture, BuildAllRolesPackage(changed), b), "B prepares", b.Error)
            || !CheckWith(PrepareFixture(fixture, BuildAllRolesPackage(), sameAsA), "A again prepares", sameAsA.Error))
            return false;
        passed &= Check(a.Package.ReceiptDraft.StreamId == b.Package.ReceiptDraft.StreamId
            && a.Package.ReceiptDraft.GenerationId != b.Package.ReceiptDraft.GenerationId, "changed pixel keeps the stream and changes the generation");
        for (size_t index = 0; index < a.Package.Assets.size(); ++index)
            passed &= Check(a.Package.Assets[index].Handle == b.Package.Assets[index].Handle
                && a.Package.Assets[index].RegistrySourcePath == b.Package.Assets[index].RegistrySourcePath,
                "source replacement keeps every handle and registry path");

        FabGltfStagedGeneration stagedA, stagedB;
        std::string error;
        if (!CheckWith(StageInto(project, a.Package, stagedA, error), "stage A", error)
            || !CheckWith(StageInto(project, b.Package, stagedB, error), "stage B", error))
            return false;

        const AssetRegistry emptyRegistry {};
        const FabReceiptCollection emptyReceipts {};
        FabGltfCandidate candidateA;
        if (!CheckWith(BuildFabGltfCandidate(emptyRegistry, emptyReceipts, a.Package, stagedA, project, candidateA, error), "new stream candidate", error))
            return false;
        passed &= Check(emptyRegistry.GetAssets().empty() && emptyRegistry.GetCookedArtifactBasePath().empty()
            && emptyReceipts.Receipts.empty(), "the base registry and receipts are not mutated");
        passed &= Check(candidateA.Decision.Kind == FabReceiptDecisionKind::AddNewStream && candidateA.NeedsPublish
            && candidateA.Receipt.Relation == FabGenerationRelation::Initial && candidateA.Receipts.Receipts.size() == 1,
            "first import is a new initial stream");
        passed &= Check(candidateA.Registry.GetAssets().size() == a.Package.Assets.size()
            && candidateA.Registry.GetCookedArtifactBasePath() == std::filesystem::absolute(project).lexically_normal(),
            "all assets register under the final cooked base");
        for (const AssetMetadata& metadata : candidateA.Registry.GetAssets())
            passed &= Check(metadata.SourcePolicy == AssetSourcePolicy::ImmutablePackage && metadata.CookedRoot == stagedA.CookedRoot
                && metadata.SourcePath.starts_with("fab:" + a.Package.ReceiptDraft.StreamId + "/"), "registry entry is an immutable stream path");
        passed &= Check(SameMaterialExact(candidateA.Material, a.Package.Material), "candidate carries the prepared material");
        passed &= CheckResolvedThroughRealLoaders(candidateA.Registry, a.Package, "candidate A");
        passed &= CheckWith(ValidateFabReceiptCollection(candidateA.Receipts, error), "candidate receipts validate", error);

        // Replacement: same stream, changed bytes.
        const AssetRegistry oldRegistry = candidateA.Registry;
        const FabReceiptCollection oldReceipts = candidateA.Receipts;
        FabGltfCandidate candidateB;
        if (!CheckWith(BuildFabGltfCandidate(candidateA.Registry, candidateA.Receipts, b.Package, stagedB, project, candidateB, error), "replacement candidate", error))
            return false;
        passed &= Check(SameRegistryDeep(candidateA.Registry, oldRegistry) && candidateA.Receipts == oldReceipts,
            "building the replacement does not mutate the base registry or receipts");
        passed &= Check(candidateB.Decision.Kind == FabReceiptDecisionKind::ReplaceSameStreamSource && candidateB.NeedsPublish
            && candidateB.Receipt.Relation == FabGenerationRelation::SourceReplacement
            && candidateB.Receipt.RelatedStreamId == a.Package.ReceiptDraft.StreamId
            && candidateB.Receipt.RelatedGenerationId == a.Package.ReceiptDraft.GenerationId
            && candidateB.Receipts.Receipts.size() == 2, "replacement relation points at the previous generation");
        passed &= Check(candidateB.Registry.GetAssets().size() == oldRegistry.GetAssets().size(), "replacement keeps the asset count");
        for (size_t index = 0; index < candidateB.Registry.GetAssets().size(); ++index)
        {
            const AssetMetadata& oldMetadata = oldRegistry.GetAssets()[index];
            const AssetMetadata& newMetadata = candidateB.Registry.GetAssets()[index];
            passed &= Check(oldMetadata.Handle == newMetadata.Handle && oldMetadata.SourcePath == newMetadata.SourcePath
                && oldMetadata.Name == newMetadata.Name && newMetadata.CookedRoot == stagedB.CookedRoot
                && oldMetadata.CookedRoot == stagedA.CookedRoot, "replacement swaps only the cooked root");
        }
        // An older renderer-style copy still resolves the old payload; the candidate resolves the new one.
        passed &= CheckResolvedThroughRealLoaders(oldRegistry, a.Package, "old generation after replacement");
        passed &= CheckResolvedThroughRealLoaders(candidateB.Registry, b.Package, "replacement generation");
        TextureArtifactVariantSet oldBase;
        TextureArtifactVariantSet newBase;
        const AssetHandle baseHandle = a.Package.Textures[0].Asset;
        passed &= Check(ResolveTextureArtifactVariantSet(oldRegistry, baseHandle, TextureTargetProfile::RGBAFallback, oldBase, error)
            && ResolveTextureArtifactVariantSet(candidateB.Registry, baseHandle, TextureTargetProfile::RGBAFallback, newBase, error)
            && oldBase.Preferred.Payload[0] == 10 && newBase.Preferred.Payload[0] == 11,
            "same texture handle yields the old pixel through the old registry and the new pixel through the candidate");

        // Exact reuse of identical bytes needs no publication and no change.
        const std::filesystem::path other = MakeDirectory(fixture, "other-staging");
        FabGltfStagedGeneration stagedAgain;
        FabGltfCandidate reuse;
        if (CheckWith(StageInto(other, sameAsA.Package, stagedAgain, error), "stage identical import", error)
            && CheckWith(BuildFabGltfCandidate(oldRegistry, oldReceipts, sameAsA.Package, stagedAgain, project, reuse, error), "identical import candidate", error))
        {
            passed &= Check(reuse.Decision.Kind == FabReceiptDecisionKind::ExactReuse && !reuse.NeedsPublish
                && SameRegistryDeep(reuse.Registry, oldRegistry) && reuse.Receipts == oldReceipts
                && stagedAgain.Receipt.Assets == stagedA.Receipt.Assets, "identical bytes classify as exact reuse with no publication");
        }
        else
            passed = false;

        FabGltfCandidate wrongBase;
        passed &= CheckWith(!BuildFabGltfCandidate(oldRegistry, oldReceipts, sameAsA.Package, stagedAgain, other, wrongBase, error)
            && Contains(error, "cooked base differs"), "a different final cooked base is refused", error);
        return passed;
    }

    bool CandidateRejectionsLeaveEverythingUntouched(ScopedFixtureRoot& fixture)
    {
        bool passed = true;
        const std::filesystem::path project = MakeDirectory(fixture, "project-rejections");
        Prepared a, noEmissive, update;
        if (!CheckWith(PrepareFixture(fixture, BuildAllRolesPackage(), a), "A prepares", a.Error)
            || !CheckWith(PrepareFixture(fixture, BuildAllRolesPackage(kAllRolesBase, false), noEmissive), "role-reduced package prepares", noEmissive.Error)
            || !CheckWith(PrepareFixture(fixture, BuildAllRolesPackage(), update, {}, "2.0"), "product update prepares", update.Error))
            return false;
        FabGltfStagedGeneration stagedA, stagedReduced, stagedUpdate;
        std::string error;
        if (!CheckWith(StageInto(project, a.Package, stagedA, error), "stage A", error)
            || !CheckWith(StageInto(project, noEmissive.Package, stagedReduced, error), "stage reduced", error)
            || !CheckWith(StageInto(project, update.Package, stagedUpdate, error), "stage update", error))
            return false;
        FabGltfCandidate candidateA;
        if (!CheckWith(BuildFabGltfCandidate({}, {}, a.Package, stagedA, project, candidateA, error), "A candidate", error))
            return false;
        const AssetRegistry registry = candidateA.Registry;
        const FabReceiptCollection receipts = candidateA.Receipts;

        // Dropping a texture role changes the stable asset set: a conflict, never a silent dangling handle.
        FabGltfCandidate rejected;
        rejected.NeedsPublish = true;
        error.clear();
        passed &= CheckWith(a.Package.ReceiptDraft.StreamId == noEmissive.Package.ReceiptDraft.StreamId
            && !BuildFabGltfCandidate(registry, receipts, noEmissive.Package, stagedReduced, project, rejected, error)
            && rejected.Decision.Kind == FabReceiptDecisionKind::Conflict && !error.empty(), "a replacement with a different role set is a conflict", error);
        passed &= Check(SameRegistryDeep(registry, candidateA.Registry) && receipts == candidateA.Receipts
            && rejected.Registry.GetAssets().empty() && rejected.Receipts.Receipts.empty(), "the conflict leaves registry, receipts and output untouched");

        // A new version label is a new stream with new handles.
        FabGltfCandidate productUpdate;
        if (CheckWith(BuildFabGltfCandidate(registry, receipts, update.Package, stagedUpdate, project, productUpdate, error), "product update candidate", error))
        {
            passed &= Check(productUpdate.Decision.Kind == FabReceiptDecisionKind::AddProductUpdateStream
                && productUpdate.Receipt.Relation == FabGenerationRelation::ProductUpdate
                && productUpdate.Receipt.RelatedStreamId == a.Package.ReceiptDraft.StreamId
                && productUpdate.Receipt.RelatedGenerationId == a.Package.ReceiptDraft.GenerationId
                && productUpdate.Registry.GetAssets().size() == 2 * registry.GetAssets().size()
                && update.Package.Assets[0].Handle != a.Package.Assets[0].Handle, "a new version label adds a product update stream");
            passed &= CheckResolvedThroughRealLoaders(productUpdate.Registry, update.Package, "product update");
            passed &= CheckResolvedThroughRealLoaders(productUpdate.Registry, a.Package, "product update keeps the old stream");
        }
        else
            passed = false;

        // Corrupt prior state: receipts name a generation the registry does not hold.
        FabGltfCandidate corrupt;
        error.clear();
        passed &= CheckWith(!BuildFabGltfCandidate({}, receipts, a.Package, stagedA, project, corrupt, error)
            && Contains(error, "registry does not hold the previously accepted"), "exact reuse against an empty registry is corrupt prior state", error);
        AssetRegistry wrongRoot = registry;
        const AssetGeneration expected { AssetSourcePolicy::ImmutablePackage, stagedA.CookedRoot };
        const AssetGeneration foreign { AssetSourcePolicy::ImmutablePackage, "fab/" + std::string(64, 'c') };
        passed &= Check(wrongRoot.CompareAndSwapAssetGeneration(a.Package.Assets[2].Handle, expected, foreign), "fixture swaps one root");
        std::vector<Pixel> changed = kAllRolesBase;
        changed[1] = { 99, 99, 99, 255 };
        Prepared replacement;
        FabGltfStagedGeneration stagedReplacement;
        if (CheckWith(PrepareFixture(fixture, BuildAllRolesPackage(changed), replacement), "replacement prepares", replacement.Error)
            && CheckWith(StageInto(project, replacement.Package, stagedReplacement, error), "stage replacement", error))
        {
            error.clear();
            passed &= CheckWith(!BuildFabGltfCandidate(wrongRoot, receipts, replacement.Package, stagedReplacement, project, corrupt, error)
                && Contains(error, "previous generation"), "a registry holding a different root than the receipts is refused", error);
            AssetRegistry missingAsset;
            for (size_t index = 0; index + 1 < registry.GetAssets().size(); ++index)
                missingAsset.RegisterAsset(registry.GetAssets()[index]);
            missingAsset.SetCookedArtifactBasePath(project);
            error.clear();
            passed &= CheckWith(!BuildFabGltfCandidate(missingAsset, receipts, replacement.Package, stagedReplacement, project, corrupt, error)
                && Contains(error, "previous generation"), "a registry missing an asset is refused", error);
        }
        else
            passed = false;
        // Handles registered without a receipt are corrupt prior state too, never silently adopted.
        error.clear();
        passed &= Check(!BuildFabGltfCandidate(registry, {}, a.Package, stagedA, project, corrupt, error)
            && Contains(error, "already contains"), "assets registered without a receipt are refused: " + error);
        passed &= Check(corrupt.Registry.GetAssets().empty() && corrupt.Receipts.Receipts.empty() && corrupt.NeedsPublish,
            "rejected candidates leave the output untouched");
        passed &= Check(SameRegistryDeep(registry, candidateA.Registry) && receipts == candidateA.Receipts, "rejections never mutate their inputs");
        return passed;
    }

    bool StageIsFailureAtomic(ScopedFixtureRoot& fixture)
    {
        bool passed = true;
        Prepared prepared;
        if (!CheckWith(PrepareFixture(fixture, BuildAllRolesPackage(), prepared), "package prepares", prepared.Error))
            return false;
        const std::string generation = prepared.Package.ReceiptDraft.GenerationId;

        {
            // A pre-existing generation directory, whatever it holds, is never entered or removed.
            const std::filesystem::path base = MakeDirectory(fixture, "preexisting");
            const std::filesystem::path existing = base / "fab" / generation;
            std::error_code ignored;
            std::filesystem::create_directories(existing, ignored);
            WriteBytes(existing / "sentinel.keep", ToBytes("keep"));
            const std::vector<std::string> before = Listing(base);
            FabGltfStagedGeneration staged;
            std::string error;
            passed &= CheckWith(!StageInto(base, prepared.Package, staged, error) && Contains(error, "already exists")
                && Listing(base) == before && ReadBytes(existing / "sentinel.keep") == ToBytes("keep"), "an existing generation directory is refused and untouched", error);
        }

        {
            const std::filesystem::path base = MakeDirectory(fixture, "cancel-sweep");
            size_t polls = 0;
            FabGltfStageOptions counting;
            counting.IsCancelled = [&polls]() { ++polls; return false; };
            const std::filesystem::path probe = MakeDirectory(fixture, "cancel-probe");
            FabGltfStagedGeneration staged;
            std::string error;
            passed &= CheckWith(StageInto(probe, prepared.Package, staged, error, counting), "uncancelled stage", error);
            const size_t totalPolls = polls;
            passed &= Check(totalPolls >= 2 + 2 * prepared.Package.Assets.size(), "stage polls cancellation around every artifact: " + std::to_string(totalPolls));
            const std::vector<std::string> before = Listing(base);
            for (size_t cancelAt = 1; cancelAt <= totalPolls; ++cancelAt)
            {
                size_t calls = 0;
                FabGltfStageOptions cancelling;
                cancelling.IsCancelled = [&calls, cancelAt]() { return ++calls >= cancelAt; };
                FabGltfStagedGeneration out;
                out.CookedRoot = "sentinel";
                error.clear();
                if (!Check(!StageInto(base, prepared.Package, out, error, cancelling) && Contains(error, "cancelled")
                        && out.CookedRoot == "sentinel" && Listing(base) == before,
                    "stage cancelled at poll " + std::to_string(cancelAt) + " leaves the staging tree unchanged: " + error))
                {
                    passed = false;
                    break;
                }
            }
        }

        {
            // Injected real I/O failure: strip write permission from textures/ after the mesh was written.
            const std::filesystem::path base = MakeDirectory(fixture, "write-failure");
            const std::vector<std::string> before = Listing(base);
            size_t artifactEvents = 0;
            bool injectionEffective = true;
            FabGltfStageOptions failing;
            failing.TestHook = [&](FabGltfStage stage, std::string_view)
            {
                if (stage != FabGltfStage::ArtifactWritten || ++artifactEvents != 4)
                    return;
                // Events 1-2 belong to the mesh and 3-4 to the material; the first texture follows event 4.
                const std::filesystem::path textures = base / "fab" / prepared.Package.ReceiptDraft.GenerationId / "textures";
                std::error_code error;
                std::filesystem::permissions(textures, std::filesystem::perms::owner_read | std::filesystem::perms::owner_exec,
                    std::filesystem::perm_options::replace, error);
                std::ofstream probe(textures / "probe.tmp");
                if (probe)
                {
                    probe.close();
                    std::filesystem::remove(textures / "probe.tmp", error);
                    injectionEffective = false;
                    std::filesystem::permissions(textures, std::filesystem::perms::owner_all,
                        std::filesystem::perm_options::replace, error);
                }
            };
            FabGltfStagedGeneration staged;
            std::string error;
            const bool succeeded = StageInto(base, prepared.Package, staged, error, failing);
            if (injectionEffective)
            {
                passed &= CheckWith(!succeeded && Contains(error, "could not write texture/"), "an injected write failure fails the stage", error);
                passed &= Check(Listing(base) == before, "an injected write failure removes everything the stage created");
            }
            else
            {
                std::cerr << "note: write-failure injection is not effective for this user or platform; case not exercised\n";
                passed &= Check(succeeded, "stage succeeds when the injection is ineffective");
            }
        }
        return passed;
    }
}

namespace SpiralTests
{
    bool TestFabGltfPrepareBakesGeometryAgainstIndependentOracles()
    {
        ScopedFixtureRoot fixture;
        if (!Check(fixture.IsReady(), "fixture root is available"))
            return false;
        bool passed = true;
        passed &= GeometryTransformChain(fixture);
        passed &= GeometryExplicitMatrix(fixture);
        passed &= GeometryMirroredNodeFlipsWinding(fixture);
        passed &= GeometryDerivedNormalsAreAreaWeighted(fixture);
        passed &= GeometryInstancedMeshAndSharedMaterial(fixture);
        passed &= GeometrySparseNormalizedAndOptionalAttributes(fixture);
        passed &= GeometryDefaultMaterialAndIgnoredSceneContent(fixture);
        return passed;
    }

    bool TestFabGltfPrepareMapsMaterialsTexturesRolesAndColorSpaces()
    {
        ScopedFixtureRoot fixture;
        if (!Check(fixture.IsReady(), "fixture root is available"))
            return false;
        bool passed = true;
        passed &= MaterialAllRoles(fixture);
        passed &= MaterialExternalFilesAndJpeg(fixture);
        passed &= MaterialOrmPacking(fixture);
        return passed;
    }

    bool TestFabGltfPrepareRejectsUnsupportedAndHostilePackagesWithoutOutput()
    {
        ScopedFixtureRoot fixture;
        if (!Check(fixture.IsReady(), "fixture root is available"))
            return false;
        bool passed = true;
        passed &= RejectFeatureFixtures(fixture);
        passed &= RejectMaterialAndSceneFixtures(fixture);
        passed &= RejectGeometryAndLimitFixtures(fixture);
        passed &= RejectDeclarationFixtures(fixture);
        passed &= RejectDependencyUris(fixture);
        passed &= PrepareCancelSweep(fixture);
        return passed;
    }

    bool TestFabGltfCookStagesDeterministicGenerationsAndStableIdentities()
    {
        ScopedFixtureRoot fixture;
        if (!Check(fixture.IsReady(), "fixture root is available"))
            return false;
        return StageDeterministicLayout(fixture);
    }

    bool TestFabGltfCandidateConstructionIsFailureAtomicAndPreservesOldResolvers()
    {
        ScopedFixtureRoot fixture;
        if (!Check(fixture.IsReady(), "fixture root is available"))
            return false;
        bool passed = true;
        passed &= CandidateReplacementPreservesOldResolvers(fixture);
        passed &= CandidateRejectionsLeaveEverythingUntouched(fixture);
        passed &= StageIsFailureAtomic(fixture);
        return passed;
    }
}
