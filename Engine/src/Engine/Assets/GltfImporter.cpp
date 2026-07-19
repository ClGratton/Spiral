#include "Engine/Assets/GltfImporter.h"

#include "Engine/Assets/AssetFileSystem.h"
#include "Engine/Assets/MeshArtifact.h"
#include "Engine/Assets/AssetRegistry.h"

#define CGLTF_IMPLEMENTATION
#include "cgltf.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <limits>
#include <fstream>
#include <iomanip>
#include <memory>
#include <system_error>
#include <utility>

namespace Engine
{
    namespace
    {
        std::string GetResultName(cgltf_result result)
        {
            switch (result)
            {
                case cgltf_result_success: return "success";
                case cgltf_result_data_too_short: return "data too short";
                case cgltf_result_unknown_format: return "unknown format";
                case cgltf_result_invalid_json: return "invalid JSON";
                case cgltf_result_invalid_gltf: return "invalid glTF";
                case cgltf_result_invalid_options: return "invalid parser options";
                case cgltf_result_file_not_found: return "file not found";
                case cgltf_result_io_error: return "I/O error";
                case cgltf_result_out_of_memory: return "out of memory";
                case cgltf_result_legacy_gltf: return "legacy glTF";
                case cgltf_result_max_enum: return "unknown parser error";
            }

            return "unknown parser error";
        }

        bool HasSupportedExtension(const std::filesystem::path& path)
        {
            std::string extension = path.extension().string();
            std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char character)
            {
                return static_cast<char>(std::tolower(character));
            });
            return extension == ".gltf" || extension == ".glb";
        }

        std::string GetMeshName(const cgltf_mesh& mesh, std::size_t meshIndex, const std::filesystem::path& sourcePath)
        {
            if (mesh.name && mesh.name[0] != '\0')
                return mesh.name;

            const std::string stem = sourcePath.stem().string();
            return stem.empty() ? "Mesh " + std::to_string(meshIndex) : stem + " Mesh " + std::to_string(meshIndex);
        }

        const cgltf_accessor* FindPositionAccessor(const cgltf_primitive& primitive)
        {
            for (cgltf_size attributeIndex = 0; attributeIndex < primitive.attributes_count; ++attributeIndex)
            {
                const cgltf_attribute& attribute = primitive.attributes[attributeIndex];
                if (attribute.type == cgltf_attribute_type_position)
                    return attribute.data;
            }

            return nullptr;
        }

        bool AppendPrimitive(
            const cgltf_primitive& primitive,
            u32 sourceMeshIndex,
            u32 sourcePrimitiveIndex,
            MeshArtifact& artifact,
            GltfMeshImportInfo& importedMesh,
            std::string& outError)
        {
            const cgltf_accessor* positionAccessor = FindPositionAccessor(primitive);
            if (primitive.type != cgltf_primitive_type_triangles || !positionAccessor
                || positionAccessor->type != cgltf_type_vec3 || positionAccessor->component_type != cgltf_component_type_r_32f)
            {
                ++importedMesh.UnsupportedPrimitiveCount;
                return true;
            }
            const cgltf_size sourceIndexCount = primitive.indices ? primitive.indices->count : positionAccessor->count;
            if (sourceIndexCount == 0 || sourceIndexCount % 3 != 0
                || positionAccessor->count > std::numeric_limits<u32>::max()
                || artifact.Vertices.size() + positionAccessor->count > std::numeric_limits<u32>::max())
            {
                ++importedMesh.UnsupportedPrimitiveCount;
                return true;
            }

            const u32 vertexOffset = static_cast<u32>(artifact.Vertices.size());
            const u32 indexOffset = static_cast<u32>(artifact.Indices.size());
            artifact.Vertices.reserve(artifact.Vertices.size() + positionAccessor->count);
            for (cgltf_size vertexIndex = 0; vertexIndex < positionAccessor->count; ++vertexIndex)
            {
                MeshArtifactVertex vertex;
                if (!cgltf_accessor_read_float(positionAccessor, vertexIndex, vertex.Position, 3))
                {
                    outError = "could not read glTF POSITION data";
                    return false;
                }
                artifact.Vertices.push_back(vertex);
            }
            artifact.Indices.reserve(artifact.Indices.size() + sourceIndexCount);
            for (cgltf_size index = 0; index < sourceIndexCount; ++index)
            {
                const cgltf_size sourceIndex = primitive.indices ? cgltf_accessor_read_index(primitive.indices, index) : index;
                if (sourceIndex >= positionAccessor->count)
                {
                    outError = "glTF primitive index is outside the POSITION range";
                    return false;
                }
                artifact.Indices.push_back(vertexOffset + static_cast<u32>(sourceIndex));
            }

            MeshArtifactPrimitive artifactPrimitive;
            artifactPrimitive.SourceMeshIndex = sourceMeshIndex;
            artifactPrimitive.SourcePrimitiveIndex = sourcePrimitiveIndex;
            artifactPrimitive.VertexByteOffset = static_cast<u64>(vertexOffset) * sizeof(MeshArtifactVertex);
            artifactPrimitive.VertexByteSize = static_cast<u64>(positionAccessor->count) * sizeof(MeshArtifactVertex);
            artifactPrimitive.IndexByteOffset = static_cast<u64>(indexOffset) * sizeof(u32);
            artifactPrimitive.IndexByteSize = static_cast<u64>(sourceIndexCount) * sizeof(u32);
            artifact.Primitives.push_back(artifactPrimitive);

            ++importedMesh.TrianglePrimitiveCount;
            importedMesh.VertexCount += positionAccessor->count;
            importedMesh.TriangleCount += sourceIndexCount / 3;
            return true;
        }
    }

    GltfImportResult GltfImporter::Import(const std::filesystem::path& sourcePath, AssetRegistry& registry)
    {
        GltfImportResult result;
        result.SourcePath = AssetRegistry::NormalizeSourcePath(sourcePath.generic_string());
        if (result.SourcePath.empty())
        {
            result.Error = "A non-empty glTF source path is required";
            return result;
        }

        if (!HasSupportedExtension(sourcePath))
        {
            result.Error = "Only .gltf and .glb sources are supported";
            return result;
        }

        const std::filesystem::path resolvedPath = AssetFileSystem::ResolvePath(result.SourcePath);
        std::error_code error;
        if (!std::filesystem::is_regular_file(resolvedPath, error) || error)
        {
            result.Error = "Could not find glTF source: " + result.SourcePath;
            return result;
        }

        const std::string resolvedPathString = resolvedPath.string();
        cgltf_options options {};
        cgltf_data* rawDocument = nullptr;
        cgltf_result parseResult = cgltf_parse_file(&options, resolvedPathString.c_str(), &rawDocument);
        if (parseResult != cgltf_result_success)
        {
            result.Error = "Could not parse glTF: " + GetResultName(parseResult);
            return result;
        }

        std::unique_ptr<cgltf_data, decltype(&cgltf_free)> document(rawDocument, &cgltf_free);
        for (cgltf_size bufferIndex = 0; bufferIndex < document->buffers_count; ++bufferIndex)
        {
            if (document->buffers[bufferIndex].uri)
                cgltf_decode_string(document->buffers[bufferIndex].uri);
        }

        // cgltf resolves external buffers relative to the original glTF file, not its parent directory.
        parseResult = cgltf_load_buffers(&options, document.get(), resolvedPathString.c_str());
        if (parseResult != cgltf_result_success)
        {
            result.Error = "Could not load glTF buffers: " + GetResultName(parseResult);
            return result;
        }

        parseResult = cgltf_validate(document.get());
        if (parseResult != cgltf_result_success)
        {
            result.Error = "glTF validation failed: " + GetResultName(parseResult);
            return result;
        }

        if (document->meshes_count == 0)
        {
            result.Error = "glTF source does not contain a mesh";
            return result;
        }

        result.Meshes.reserve(document->meshes_count);
        MeshArtifact artifact;
        artifact.SourcePath = result.SourcePath;
        for (cgltf_size meshIndex = 0; meshIndex < document->meshes_count; ++meshIndex)
        {
            const cgltf_mesh& sourceMesh = document->meshes[meshIndex];
            GltfMeshImportInfo importedMesh;
            importedMesh.Name = GetMeshName(sourceMesh, meshIndex, resolvedPath);
            importedMesh.PrimitiveCount = sourceMesh.primitives_count;

            for (cgltf_size primitiveIndex = 0; primitiveIndex < sourceMesh.primitives_count; ++primitiveIndex)
            {
                const cgltf_primitive& primitive = sourceMesh.primitives[primitiveIndex];
                if (meshIndex > std::numeric_limits<u32>::max() || primitiveIndex > std::numeric_limits<u32>::max()
                    || !AppendPrimitive(primitive, static_cast<u32>(meshIndex), static_cast<u32>(primitiveIndex), artifact, importedMesh, result.Error))
                    return result;
            }

            result.Meshes.push_back(std::move(importedMesh));
        }

        const std::string assetName = result.Meshes.size() == 1
            ? result.Meshes.front().Name
            : resolvedPath.stem().string();
        const bool alreadyRegistered = registry.FindAssetByPath(AssetType::Mesh, result.SourcePath) != kInvalidAssetHandle;
        result.MeshAsset = registry.RegisterAsset(AssetType::Mesh, result.SourcePath, assetName);
        if (result.MeshAsset == kInvalidAssetHandle)
        {
            result.Error = "Could not register glTF mesh asset";
            return result;
        }

        artifact.Asset = result.MeshAsset;
        result.CookedPath = GetCookedMeshArtifactPath(result.MeshAsset);
        if (!StoreMeshArtifact(result.CookedPath, artifact, result.Error))
        {
            if (!alreadyRegistered)
                registry.RemoveAsset(result.MeshAsset);
            if (result.Error.empty())
                result.Error = "Could not write cooked glTF mesh artifact";
            return result;
        }

        result.Succeeded = true;
        return result;
    }
}
