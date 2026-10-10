#include "Engine/Assets/FabGltfCook.h"

#include "Engine/Assets/GltfBounds.h"
#include "Engine/Core/Sha256.h"

#include "cgltf.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <locale>
#include <memory>
#include <span>
#include <system_error>
#include <unordered_map>
#include <utility>

namespace Engine
{
    namespace
    {
        constexpr std::string_view kMeshRole = "mesh.main";
        constexpr std::string_view kMaterialRole = "material.main";

        struct TextureRoleSpec
        {
            std::string_view SemanticRole;
            std::string_view Label;
            TextureRole Role;
            TextureColorSpace ColorSpace;
            MaterialTextureSlot Slot;
        };

        constexpr std::array<TextureRoleSpec, 4> kTextureRoles { {
            { "texture.base-color", "Base Color", TextureRole::BaseColor, TextureColorSpace::Srgb, MaterialTextureSlot::BaseColor },
            { "texture.orm", "ORM", TextureRole::Orm, TextureColorSpace::Linear, MaterialTextureSlot::Orm },
            { "texture.normal", "Normal", TextureRole::Normal, TextureColorSpace::Linear, MaterialTextureSlot::Normal },
            { "texture.emissive", "Emissive", TextureRole::Emissive, TextureColorSpace::Srgb, MaterialTextureSlot::Emissive }
        } };

        const TextureRoleSpec& GetRoleSpec(TextureRole role)
        {
            for (const TextureRoleSpec& spec : kTextureRoles)
                if (spec.Role == role)
                    return spec;
            return kTextureRoles[0];
        }

        bool IsLowerHex64(std::string_view value)
        {
            return value.size() == 64 && std::all_of(value.begin(), value.end(), [](unsigned char character)
            {
                return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f');
            });
        }

        std::string SanitizeName(std::string_view value, std::string_view fallback)
        {
            std::string result;
            for (const char character : value)
            {
                if (result.size() >= 128)
                    break;
                const unsigned char byte = static_cast<unsigned char>(character);
                result.push_back(byte >= 0x20 && byte <= 0x7e ? character : '_');
            }
            return result.empty() ? std::string(fallback) : result;
        }

        // The material writer streams six significant digits. Quantizing here
        // makes the in-memory candidate equal to what a reload reads back.
        float QuantizeToMaterialPrecision(float value)
        {
            std::array<char, 48> text {};
            const auto written = std::to_chars(text.data(), text.data() + text.size(), value,
                std::chars_format::general, 6);
            float result = value;
            if (written.ec == std::errc {})
                std::from_chars(text.data(), written.ptr, result);
            return result;
        }

        std::string LogicalPathForRole(std::string_view role)
        {
            std::string path(role);
            const size_t dot = path.find('.');
            if (dot != std::string::npos)
                path[dot] = '/';
            return path;
        }

        const char* ArtifactExtension(AssetType type)
        {
            return type == AssetType::Mesh ? ".spiralmesh"
                : type == AssetType::Material ? ".spiralmat" : ".rgba-fallback.spiraltexture";
        }

        const char* ArtifactDirectory(AssetType type)
        {
            return type == AssetType::Mesh ? "meshes" : type == AssetType::Material ? "materials" : "textures";
        }

        struct PrimitiveInstance
        {
            const cgltf_node* Node = nullptr;
            u32 MeshIndex = 0;
            u32 PrimitiveIndex = 0;
            const cgltf_accessor* Position = nullptr;
            const cgltf_accessor* Normal = nullptr;
            const cgltf_accessor* Uv = nullptr;
            const cgltf_accessor* Color = nullptr;
            const cgltf_accessor* Indices = nullptr;
            u32 VertexCount = 0;
            u32 IndexCount = 0;
        };

        struct ViewInfo
        {
            const cgltf_image* Image = nullptr;
            MaterialTextureSampler Sampler = MaterialTextureSampler::LinearWrap;
        };

        bool IsNearestFilterFamily(cgltf_filter_type filter)
        {
            return filter == cgltf_filter_type_nearest || filter == cgltf_filter_type_nearest_mipmap_nearest
                || filter == cgltf_filter_type_nearest_mipmap_linear;
        }

        bool IsLinearFilterFamily(cgltf_filter_type filter)
        {
            return filter == cgltf_filter_type_linear || filter == cgltf_filter_type_linear_mipmap_nearest
                || filter == cgltf_filter_type_linear_mipmap_linear;
        }

        class PrepareSession
        {
        public:
            PrepareSession(const LocalPackageSnapshot& snapshot, const FabImportReceipt& declaration,
                const FabGltfPrepareOptions& options)
                : m_Snapshot(snapshot), m_Declaration(declaration), m_Options(options), m_Data(nullptr, cgltf_free)
            {
            }

            bool Run(FabGltfPreparedPackage& out, std::string& error)
            {
                try
                {
                    if (!Execute(out))
                    {
                        error = m_Error.empty() ? "Fab glTF preparation failed" : m_Error;
                        return false;
                    }
                }
                catch (...)
                {
                    error = "Fab glTF preparation failed on an exception or allocation failure";
                    return false;
                }
                error.clear();
                return true;
            }

        private:
            bool Fail(std::string message)
            {
                if (m_Error.empty())
                    m_Error = std::move(message);
                return false;
            }

            bool Poll()
            {
                if (m_Options.IsCancelled && m_Options.IsCancelled())
                {
                    if (m_Error.empty())
                        m_Error = "Fab glTF preparation was cancelled";
                    return true;
                }
                return false;
            }

            bool Checkpoint(FabGltfStage stage, std::string_view label)
            {
                if (m_Options.TestHook)
                    m_Options.TestHook(stage, label);
                return !Poll();
            }

            bool Execute(FabGltfPreparedPackage& out)
            {
                if (!Checkpoint(FabGltfStage::Started, {}))
                    return false;
                if (!CheckDeclaration() || !ReadRoot() || !Checkpoint(FabGltfStage::RootRead, {})
                    || !Parse() || !Checkpoint(FabGltfStage::Parsed, {})
                    || !AttachBuffers() || !Checkpoint(FabGltfStage::BuffersLoaded, {})
                    || !CheckDocumentPolicy() || !CollectInstances() || !ResolveMaterial() || !ValidateInstances())
                    return false;

                FabGltfPreparedPackage local;
                if (!BakeGeometry(local.Mesh) || !Checkpoint(FabGltfStage::GeometryBaked, {})
                    || !BuildTextures(local) || !BuildMaterial(local) || !Checkpoint(FabGltfStage::MaterialBuilt, {})
                    || !AssembleAssets(local))
                    return false;
                local.VertexCount = static_cast<u32>(m_TotalVertices);
                local.TriangleCount = static_cast<u32>(m_TotalIndices / 3);
                local.PrimitiveInstanceCount = static_cast<u32>(m_Instances.size());
                if (!Checkpoint(FabGltfStage::Completed, {}))
                    return false;
                out = std::move(local);
                return true;
            }

            bool CheckDeclaration()
            {
                if (!m_Snapshot.IsValid() || m_Snapshot.GetRootRelativePath().empty())
                    return Fail("local package snapshot is not valid");
                const std::string& rootPath = m_Snapshot.GetRootRelativePath();
                const size_t dot = rootPath.rfind('.');
                std::string extension = dot == std::string::npos ? std::string() : rootPath.substr(dot);
                for (char& character : extension)
                    if (character >= 'A' && character <= 'Z')
                        character = static_cast<char>(character + ('a' - 'A'));
                m_IsGlb = extension == ".glb";
                const FabPackageFormat rootFormat = m_IsGlb ? FabPackageFormat::Glb
                    : extension == ".gltf" ? FabPackageFormat::Gltf : FabPackageFormat::Unknown;
                if (rootFormat == FabPackageFormat::Unknown || m_Declaration.PackageFormat != rootFormat)
                    return Fail("declared package format does not match the snapshot root extension");

                m_StreamId = ComputeFabStreamId(m_Declaration.ProductIdentity,
                    m_Declaration.VersionOrDownloadLabel, m_Declaration.PackageFormat);
                if (m_StreamId.empty())
                    return Fail("Fab product identity, version label or format cannot form a stream id");
                m_GenerationId = ComputeFabGenerationId(m_StreamId, m_Declaration.SourceSha256,
                    m_Snapshot.GetTreeSha256());
                if (m_GenerationId.empty())
                    return Fail("source or expanded-tree SHA-256 cannot form a generation id");
                const auto agrees = [](const std::string& supplied, std::string_view computed)
                {
                    return supplied.empty() || supplied == computed;
                };
                if (!agrees(m_Declaration.StreamId, m_StreamId) || !agrees(m_Declaration.GenerationId, m_GenerationId)
                    || !agrees(m_Declaration.ExpandedTreeSha256, m_Snapshot.GetTreeSha256())
                    || !agrees(m_Declaration.ImporterVersion, kFabGltfImporterVersion)
                    || !agrees(m_Declaration.CookerVersion, kFabGltfCookerVersion)
                    || !m_Declaration.Assets.empty())
                    return Fail("receipt declaration contains values owned by the cook");

                // Exercise every declaration rule now with a single placeholder asset.
                FabImportReceipt probe = m_Declaration;
                probe.StreamId = m_StreamId;
                probe.GenerationId = m_GenerationId;
                probe.ExpandedTreeSha256 = m_Snapshot.GetTreeSha256();
                probe.ImporterVersion = std::string(kFabGltfImporterVersion);
                probe.CookerVersion = std::string(kFabGltfCookerVersion);
                probe.Relation = FabGenerationRelation::Initial;
                probe.RelatedStreamId.clear();
                probe.RelatedGenerationId.clear();
                FabImportedAssetRecord record;
                record.Type = AssetType::Mesh;
                record.SemanticRole = std::string(kMeshRole);
                record.Handle = ComputeFabStableAssetHandle(m_StreamId, record.Type, record.SemanticRole);
                record.LogicalPath = LogicalPathForRole(kMeshRole);
                record.GenerationRelativeCookedPath = "meshes/x";
                record.ArtifactSha256 = std::string(64, '0');
                probe.Assets.push_back(std::move(record));
                std::string declarationError;
                if (!ValidateFabImportReceipt(probe, declarationError))
                    return Fail("receipt declaration is invalid: " + declarationError);

                for (const LocalPackageSnapshotEntry& entry : m_Snapshot.GetEntries())
                    m_Files.emplace(entry.RelativePath, entry.SizeBytes);
                return true;
            }

            bool ReadMember(std::string_view path, std::vector<u8>& bytes)
            {
                const auto found = m_Files.find(std::string(path));
                if (found == m_Files.end())
                    return Fail("glTF dependency is not a member of the snapshot");
                const u64 expected = found->second;
                bytes.clear();
                bytes.reserve(static_cast<size_t>(expected));
                bool cancelled = false;
                std::string readError;
                const bool read = m_Snapshot.StreamFile(path, [&](std::span<const u8> chunk)
                {
                    if (Poll())
                    {
                        cancelled = true;
                        return false;
                    }
                    if (chunk.size() > expected - bytes.size())
                        return false;
                    bytes.insert(bytes.end(), chunk.begin(), chunk.end());
                    return true;
                }, readError);
                if (cancelled)
                    return false;
                if (!read || bytes.size() != expected)
                    return Fail("snapshot member could not be read: " + readError);
                return true;
            }

            bool ReadRoot()
            {
                const auto found = m_Files.find(m_Snapshot.GetRootRelativePath());
                if (found == m_Files.end() || found->second == 0 || found->second > m_Options.Limits.MaximumRootBytes)
                    return Fail("glTF root is missing, empty or exceeds the configured size limit");
                return ReadMember(m_Snapshot.GetRootRelativePath(), m_Root);
            }

            bool Parse()
            {
                cgltf_options parseOptions {};
                cgltf_data* raw = nullptr;
                const cgltf_result result = cgltf_parse(&parseOptions, m_Root.data(), m_Root.size(), &raw);
                m_Data.reset(raw);
                if (result != cgltf_result_success || !raw)
                    return Fail(m_IsGlb ? "GLB payload is corrupt" : "glTF JSON is corrupt");
                if (raw->file_type != (m_IsGlb ? cgltf_file_type_glb : cgltf_file_type_gltf)
                    || !raw->asset.version || std::strcmp(raw->asset.version, "2.0") != 0)
                    return Fail("glTF container type or asset version is not supported");
                return true;
            }

            static std::string DecodeUri(const char* raw)
            {
                std::string uri(raw);
                const cgltf_size decoded = cgltf_decode_string(uri.data());
                uri.resize(decoded);
                return uri;
            }

            bool ResolveUri(const char* raw, std::string& relativePath)
            {
                const std::string uri = DecodeUri(raw);
                if (uri.find('\0') != std::string::npos)
                    return Fail("glTF dependency URI decodes to an embedded NUL byte");
                std::string resolveError;
                if (!ResolveGltfDependencyUri(m_Snapshot.GetRootRelativePath(), uri, LocalPackageSnapshotLimits {},
                    relativePath, resolveError))
                    return Fail(resolveError);
                return true;
            }

            bool AttachBuffers()
            {
                cgltf_data& data = *m_Data;
                m_BufferStorage.reserve(data.buffers_count);
                std::unordered_map<std::string, size_t> loaded;
                u64 totalBytes = 0;
                for (cgltf_size index = 0; index < data.buffers_count; ++index)
                {
                    cgltf_buffer& buffer = data.buffers[index];
                    buffer.data_free_method = cgltf_data_free_method_none;
                    if (!buffer.uri)
                    {
                        if (!m_IsGlb || index != 0 || !data.bin || data.bin_size < buffer.size)
                            return Fail("glTF buffer has no valid GLB binary chunk or external dependency URI");
                        buffer.data = const_cast<void*>(data.bin);
                        continue;
                    }
                    std::string relativePath;
                    if (!ResolveUri(buffer.uri, relativePath))
                        return false;
                    const auto cached = loaded.find(relativePath);
                    if (cached == loaded.end())
                    {
                        const auto member = m_Files.find(relativePath);
                        if (member == m_Files.end() || member->second < buffer.size)
                            return Fail("glTF buffer dependency is missing or shorter than its declared byteLength");
                        if (member->second > m_Options.Limits.MaximumTotalBufferBytes - totalBytes)
                            return Fail("glTF buffers exceed the configured aggregate size limit");
                        totalBytes += member->second;
                        m_BufferStorage.emplace_back();
                        if (!ReadMember(relativePath, m_BufferStorage.back()))
                            return false;
                        loaded.emplace(relativePath, m_BufferStorage.size() - 1);
                        buffer.data = m_BufferStorage.back().data();
                    }
                    else
                        buffer.data = m_BufferStorage[cached->second].data();
                }
                // The overflow-checked bounds authority runs first: cgltf_validate itself scans
                // index data through unchecked arithmetic.
                std::string boundsError;
                if (!ValidateGltfBufferBounds(*m_Data, boundsError))
                    return Fail(std::move(boundsError));
                if (cgltf_validate(m_Data.get()) != cgltf_result_success)
                    return Fail("glTF structure is invalid");
                return true;
            }

            bool CheckDocumentPolicy()
            {
                const cgltf_data& data = *m_Data;
                if (data.extensions_required_count > 0)
                    return Fail(std::string("required glTF extension is not supported: ")
                        + (data.extensions_required[0] ? data.extensions_required[0] : "?"));
                if (data.skins_count > 0)
                    return Fail("skins are not supported");
                if (data.animations_count > 0)
                    return Fail("animations are not supported");
                if (data.nodes_count > m_Options.Limits.MaximumNodeCount)
                    return Fail("glTF node count exceeds the configured limit");
                for (cgltf_size index = 0; index < data.buffer_views_count; ++index)
                    if (data.buffer_views[index].has_meshopt_compression)
                        return Fail("compressed geometry is not supported");
                for (cgltf_size index = 0; index < data.meshes_count; ++index)
                {
                    const cgltf_mesh& mesh = data.meshes[index];
                    if (mesh.weights_count > 0)
                        return Fail("morph targets are not supported");
                    for (cgltf_size primitive = 0; primitive < mesh.primitives_count; ++primitive)
                    {
                        if (mesh.primitives[primitive].targets_count > 0)
                            return Fail("morph targets are not supported");
                        if (mesh.primitives[primitive].has_draco_mesh_compression)
                            return Fail("compressed geometry is not supported");
                    }
                }
                return true;
            }

            bool CollectInstances()
            {
                cgltf_data& data = *m_Data;
                const cgltf_scene* scene = data.scene;
                if (!scene)
                {
                    if (data.scenes_count != 1)
                        return Fail("glTF has no default scene and does not contain exactly one scene");
                    scene = &data.scenes[0];
                }
                std::vector<u8> visited(data.nodes_count, 0);
                m_NodeWorld.assign(data.nodes_count, std::array<double, 16> {});
                // Each entry carries the parent it was reached through; that, not node->parent, is
                // the ancestor whose world matrix is composed.
                std::vector<std::pair<const cgltf_node*, const cgltf_node*>> stack;
                for (cgltf_size index = scene->nodes_count; index > 0; --index)
                    stack.emplace_back(scene->nodes[index - 1], nullptr);
                while (!stack.empty())
                {
                    const auto [node, reachedFrom] = stack.back();
                    stack.pop_back();
                    const size_t nodeIndex = static_cast<size_t>(node - data.nodes);
                    if (visited[nodeIndex])
                        return Fail("glTF node is reachable twice");
                    visited[nodeIndex] = 1;
                    if (Poll())
                        return false;
                    // World = parentWorld * local, computed once per node so a deep chain costs O(n)
                    // instead of walking every ancestor for every primitive instance. A parent is
                    // always visited before its children.
                    {
                        cgltf_float local[16];
                        cgltf_node_transform_local(node, local);
                        std::array<double, 16>& world = m_NodeWorld[nodeIndex];
                        if (reachedFrom)
                        {
                            const std::array<double, 16>& parent = m_NodeWorld[static_cast<size_t>(reachedFrom - data.nodes)];
                            for (size_t column = 0; column < 4; ++column)
                                for (size_t row = 0; row < 4; ++row)
                                {
                                    double sum = 0.0;
                                    for (size_t inner = 0; inner < 4; ++inner)
                                        sum += parent[inner * 4 + row] * static_cast<double>(local[column * 4 + inner]);
                                    world[column * 4 + row] = sum;
                                }
                        }
                        else
                            for (size_t index = 0; index < 16; ++index)
                                world[index] = local[index];
                    }
                    if (node->skin || node->weights_count > 0)
                        return Fail(node->skin ? "skins are not supported" : "morph targets are not supported");
                    if (node->has_mesh_gpu_instancing)
                        return Fail("EXT_mesh_gpu_instancing is not supported");
                    if (node->mesh)
                    {
                        const u32 meshIndex = static_cast<u32>(node->mesh - data.meshes);
                        for (cgltf_size primitive = 0; primitive < node->mesh->primitives_count; ++primitive)
                        {
                            if (m_Instances.size() >= m_Options.Limits.MaximumPrimitiveInstances)
                                return Fail("primitive instance count exceeds the configured limit");
                            PrimitiveInstance instance;
                            instance.Node = node;
                            instance.MeshIndex = meshIndex;
                            instance.PrimitiveIndex = static_cast<u32>(primitive);
                            m_Instances.push_back(instance);
                        }
                    }
                    for (cgltf_size child = node->children_count; child > 0; --child)
                        stack.emplace_back(node->children[child - 1], node);
                }
                if (m_Instances.empty())
                    return Fail("selected glTF scene contains no renderable mesh");
                return true;
            }

            bool MapSampler(const cgltf_sampler* sampler, MaterialTextureSampler& mapped)
            {
                if (!sampler)
                {
                    mapped = MaterialTextureSampler::LinearWrap;
                    return true;
                }
                const bool repeat = sampler->wrap_s == cgltf_wrap_mode_repeat && sampler->wrap_t == cgltf_wrap_mode_repeat;
                const bool clamp = sampler->wrap_s == cgltf_wrap_mode_clamp_to_edge
                    && sampler->wrap_t == cgltf_wrap_mode_clamp_to_edge;
                if (!repeat && !clamp)
                    return Fail("unsupported texture sampler wrap mode");
                const cgltf_filter_type magnify = sampler->mag_filter;
                const cgltf_filter_type minify = sampler->min_filter;
                const bool minifyKnown = minify == cgltf_filter_type_undefined
                    || IsNearestFilterFamily(minify) || IsLinearFilterFamily(minify);
                bool point = false;
                if (magnify == cgltf_filter_type_nearest)
                    point = true;
                else if (magnify != cgltf_filter_type_undefined && magnify != cgltf_filter_type_linear)
                    return Fail("unsupported texture sampler filter");
                if (!minifyKnown || (point ? IsLinearFilterFamily(minify) : IsNearestFilterFamily(minify)))
                    return Fail("unsupported texture sampler filter combination");
                mapped = point ? (repeat ? MaterialTextureSampler::PointWrap : MaterialTextureSampler::PointClamp)
                    : (repeat ? MaterialTextureSampler::LinearWrap : MaterialTextureSampler::LinearClamp);
                return true;
            }

            bool ReadView(const cgltf_texture_view& view, std::string_view what, ViewInfo& info)
            {
                if (view.texcoord != 0)
                    return Fail(std::string(what) + " texture uses a TEXCOORD set other than 0");
                if (view.has_transform)
                    return Fail(std::string(what) + " texture uses the unsupported KHR_texture_transform extension");
                if (!view.texture->image)
                    return Fail(std::string(what) + " texture has no PNG or JPEG image source");
                info.Image = view.texture->image;
                if (!MapSampler(view.texture->sampler, info.Sampler))
                {
                    m_Error = std::string(what) + " texture: " + m_Error;
                    return false;
                }
                return true;
            }

            bool ResolveMaterial()
            {
                const cgltf_data& data = *m_Data;
                bool anyNull = false;
                for (const PrimitiveInstance& instance : m_Instances)
                {
                    const cgltf_material* material = data.meshes[instance.MeshIndex].primitives[instance.PrimitiveIndex].material;
                    if (!material)
                        anyNull = true;
                    else if (!m_GltfMaterial)
                        m_GltfMaterial = material;
                    else if (m_GltfMaterial != material)
                        return Fail("multiple materials are not supported");
                }
                if (anyNull && m_GltfMaterial)
                    return Fail("multiple materials are not supported (mixed assigned and default materials)");
                if (!m_GltfMaterial)
                    return true;

                const cgltf_material& material = *m_GltfMaterial;
                if (material.has_pbr_specular_glossiness) return Fail("unsupported material extension: KHR_materials_pbrSpecularGlossiness");
                if (material.has_clearcoat) return Fail("unsupported material extension: KHR_materials_clearcoat");
                if (material.has_transmission) return Fail("unsupported material extension: KHR_materials_transmission");
                if (material.has_volume) return Fail("unsupported material extension: KHR_materials_volume");
                if (material.has_ior) return Fail("unsupported material extension: KHR_materials_ior");
                if (material.has_specular) return Fail("unsupported material extension: KHR_materials_specular");
                if (material.has_sheen) return Fail("unsupported material extension: KHR_materials_sheen");
                if (material.has_emissive_strength) return Fail("unsupported material extension: KHR_materials_emissive_strength");
                if (material.has_iridescence) return Fail("unsupported material extension: KHR_materials_iridescence");
                if (material.has_diffuse_transmission) return Fail("unsupported material extension: KHR_materials_diffuse_transmission");
                if (material.has_anisotropy) return Fail("unsupported material extension: KHR_materials_anisotropy");
                if (material.has_dispersion) return Fail("unsupported material extension: KHR_materials_dispersion");
                if (material.unlit) return Fail("unsupported material extension: KHR_materials_unlit");
                if (material.alpha_mode != cgltf_alpha_mode_opaque)
                    return Fail("only OPAQUE alpha mode is supported");

                if (material.pbr_metallic_roughness.base_color_texture.texture
                    && !ReadView(material.pbr_metallic_roughness.base_color_texture, "base color", m_BaseColorView))
                    return false;
                if (material.emissive_texture.texture && !ReadView(material.emissive_texture, "emissive", m_EmissiveView))
                    return false;
                if (material.normal_texture.texture && !ReadView(material.normal_texture, "normal", m_NormalView))
                    return false;
                if (material.pbr_metallic_roughness.metallic_roughness_texture.texture
                    && !ReadView(material.pbr_metallic_roughness.metallic_roughness_texture, "metallic-roughness", m_MetallicRoughnessView))
                    return false;
                if (material.occlusion_texture.texture && !ReadView(material.occlusion_texture, "occlusion", m_OcclusionView))
                    return false;
                m_Textured = m_BaseColorView.Image || m_EmissiveView.Image || m_NormalView.Image
                    || m_MetallicRoughnessView.Image || m_OcclusionView.Image;
                return true;
            }

            static bool IsFloatVec(const cgltf_accessor& accessor, cgltf_type type)
            {
                return accessor.type == type && accessor.component_type == cgltf_component_type_r_32f
                    && !accessor.normalized;
            }

            bool ValidateInstances()
            {
                const cgltf_data& data = *m_Data;
                u64 vertexTotal = 0;
                u64 indexTotal = 0;
                bool needsNormals = false;
                for (PrimitiveInstance& instance : m_Instances)
                {
                    const cgltf_primitive& primitive = data.meshes[instance.MeshIndex].primitives[instance.PrimitiveIndex];
                    if (primitive.type != cgltf_primitive_type_triangles)
                        return Fail("only triangle primitives are supported");
                    for (cgltf_size index = 0; index < primitive.attributes_count; ++index)
                    {
                        const cgltf_attribute& attribute = primitive.attributes[index];
                        if (!attribute.data)
                            return Fail("glTF primitive attribute has no accessor");
                        switch (attribute.type)
                        {
                            case cgltf_attribute_type_position:
                                if (attribute.index == 0)
                                    instance.Position = attribute.data;
                                break;
                            case cgltf_attribute_type_normal:
                                if (attribute.index == 0)
                                    instance.Normal = attribute.data;
                                break;
                            case cgltf_attribute_type_texcoord:
                                if (attribute.index == 0)
                                    instance.Uv = attribute.data;
                                break;
                            case cgltf_attribute_type_color:
                                if (attribute.index == 0)
                                    instance.Color = attribute.data;
                                break;
                            case cgltf_attribute_type_joints:
                            case cgltf_attribute_type_weights:
                                return Fail("skinned vertex attributes are not supported");
                            default:
                                break;
                        }
                    }
                    if (!instance.Position || !IsFloatVec(*instance.Position, cgltf_type_vec3))
                        return Fail("primitive POSITION must be a FLOAT VEC3 accessor");
                    if (instance.Normal && !IsFloatVec(*instance.Normal, cgltf_type_vec3))
                        return Fail("primitive NORMAL must be a FLOAT VEC3 accessor");
                    if (instance.Uv)
                    {
                        const cgltf_accessor& uv = *instance.Uv;
                        const bool normalizedInteger = uv.normalized
                            && (uv.component_type == cgltf_component_type_r_8u || uv.component_type == cgltf_component_type_r_16u);
                        if (uv.type != cgltf_type_vec2 || !(IsFloatVec(uv, cgltf_type_vec2) || normalizedInteger))
                            return Fail("primitive TEXCOORD_0 must be a VEC2 FLOAT or normalized unsigned accessor");
                    }
                    else if (m_Textured)
                        return Fail("textured material requires TEXCOORD_0");
                    if (instance.Color)
                    {
                        const cgltf_accessor& color = *instance.Color;
                        const bool normalizedInteger = color.normalized
                            && (color.component_type == cgltf_component_type_r_8u || color.component_type == cgltf_component_type_r_16u);
                        if ((color.type != cgltf_type_vec3 && color.type != cgltf_type_vec4)
                            || !((color.component_type == cgltf_component_type_r_32f && !color.normalized) || normalizedInteger))
                            return Fail("primitive COLOR_0 has an unsupported accessor type");
                    }
                    if (!instance.Normal)
                        needsNormals = true;

                    const u64 vertices = instance.Position->count;
                    const u64 indices = primitive.indices ? primitive.indices->count : vertices;
                    if (vertices == 0 || indices == 0 || indices % 3 != 0)
                        return Fail("primitive has no triangles or a non-multiple-of-three index count");
                    if (vertices > m_Options.Limits.MaximumVertices - vertexTotal
                        || indices > m_Options.Limits.MaximumIndices - indexTotal
                        || vertices > std::numeric_limits<u32>::max() || indices > std::numeric_limits<u32>::max())
                        return Fail("vertex or index count exceeds the configured limit");
                    instance.VertexCount = static_cast<u32>(vertices);
                    instance.IndexCount = static_cast<u32>(indices);
                    vertexTotal += vertices;
                    indexTotal += indices;
                    m_MaxInstanceVertices = std::max<u64>(m_MaxInstanceVertices, vertices);
                    m_MaxInstanceIndices = std::max<u64>(m_MaxInstanceIndices, indices);
                }
                if (vertexTotal > std::numeric_limits<u32>::max() / 2 || indexTotal > std::numeric_limits<u32>::max() / 2)
                    return Fail("vertex or index count exceeds the configured limit");
                m_TotalVertices = vertexTotal;
                m_TotalIndices = indexTotal;
                m_DeriveNormals = needsNormals;
                return true;
            }

            // Column-major world matrix as doubles, precomputed by CollectInstances.
            void WorldMatrix(const cgltf_node* node, double (&matrix)[16]) const
            {
                const std::array<double, 16>& world = m_NodeWorld[static_cast<size_t>(node - m_Data->nodes)];
                for (size_t index = 0; index < 16; ++index)
                    matrix[index] = world[index];
            }

            bool BakeGeometry(MeshArtifact& mesh)
            {
                const cgltf_data& data = *m_Data;
                MeshArtifact local;
                local.Vertices.assign(static_cast<size_t>(m_TotalVertices), MeshArtifactVertex {});
                local.Indices.resize(static_cast<size_t>(m_TotalIndices));
                local.Primitives.reserve(m_Instances.size());
                std::vector<float> positions(static_cast<size_t>(m_MaxInstanceVertices) * 3);
                std::vector<float> normals;
                std::vector<float> uvs;
                std::vector<float> colors;
                std::vector<u32> indices(static_cast<size_t>(m_MaxInstanceIndices));
                bool anyNormals = false;
                bool anyUv = false;
                for (const PrimitiveInstance& instance : m_Instances)
                {
                    anyNormals = anyNormals || instance.Normal;
                    anyUv = anyUv || instance.Uv;
                }
                if (anyNormals)
                    normals.resize(static_cast<size_t>(m_MaxInstanceVertices) * 3);
                if (anyUv)
                    uvs.resize(static_cast<size_t>(m_MaxInstanceVertices) * 2);

                size_t firstVertex = 0;
                size_t firstIndex = 0;
                for (const PrimitiveInstance& instance : m_Instances)
                {
                    if (Poll())
                        return false;
                    const size_t vertexCount = instance.VertexCount;
                    const size_t indexCount = instance.IndexCount;
                    const cgltf_primitive& primitive = data.meshes[instance.MeshIndex].primitives[instance.PrimitiveIndex];
                    if (cgltf_accessor_unpack_floats(instance.Position, positions.data(), vertexCount * 3) != vertexCount * 3)
                        return Fail("POSITION accessor could not be read");
                    if (instance.Normal && cgltf_accessor_unpack_floats(instance.Normal, normals.data(), vertexCount * 3) != vertexCount * 3)
                        return Fail("NORMAL accessor could not be read");
                    if (instance.Uv && cgltf_accessor_unpack_floats(instance.Uv, uvs.data(), vertexCount * 2) != vertexCount * 2)
                        return Fail("TEXCOORD_0 accessor could not be read");
                    if (instance.Color)
                    {
                        const size_t components = instance.Color->type == cgltf_type_vec4 ? 4 : 3;
                        colors.resize(vertexCount * components);
                        if (cgltf_accessor_unpack_floats(instance.Color, colors.data(), colors.size()) != colors.size())
                            return Fail("COLOR_0 accessor could not be read");
                        for (size_t vertex = 0; vertex < vertexCount; ++vertex)
                            if (colors[vertex * components] != 1.0f || colors[vertex * components + 1] != 1.0f
                                || colors[vertex * components + 2] != 1.0f)
                                return Fail("COLOR_0 vertex tint is not supported");
                    }
                    if (primitive.indices)
                    {
                        if (cgltf_accessor_unpack_indices(primitive.indices, indices.data(), sizeof(u32), indexCount) != indexCount)
                            return Fail("index accessor could not be read");
                    }
                    else
                        for (size_t index = 0; index < indexCount; ++index)
                            indices[index] = static_cast<u32>(index);

                    double m[16];
                    WorldMatrix(instance.Node, m);
                    const double c00 = m[5] * m[10] - m[9] * m[6];
                    const double c01 = -(m[1] * m[10] - m[9] * m[2]);
                    const double c02 = m[1] * m[6] - m[5] * m[2];
                    const double c10 = -(m[4] * m[10] - m[8] * m[6]);
                    const double c11 = m[0] * m[10] - m[8] * m[2];
                    const double c12 = -(m[0] * m[6] - m[4] * m[2]);
                    const double c20 = m[4] * m[9] - m[8] * m[5];
                    const double c21 = -(m[0] * m[9] - m[8] * m[1]);
                    const double c22 = m[0] * m[5] - m[4] * m[1];
                    const double determinant = m[0] * c00 + m[4] * c01 + m[8] * c02;
                    if (!std::isfinite(determinant) || determinant == 0.0)
                        return Fail("node transform is singular or not finite");
                    const double handedness = determinant < 0.0 ? -1.0 : 1.0;

                    MeshArtifactVertex* output = local.Vertices.data() + firstVertex;
                    for (size_t vertex = 0; vertex < vertexCount; ++vertex)
                    {
                        const double x = positions[vertex * 3], y = positions[vertex * 3 + 1], z = positions[vertex * 3 + 2];
                        MeshArtifactVertex& target = output[vertex];
                        target.Position[0] = static_cast<float>(m[0] * x + m[4] * y + m[8] * z + m[12]);
                        target.Position[1] = static_cast<float>(m[1] * x + m[5] * y + m[9] * z + m[13]);
                        target.Position[2] = static_cast<float>(m[2] * x + m[6] * y + m[10] * z + m[14]);
                        if (instance.Normal)
                        {
                            const double nx = normals[vertex * 3], ny = normals[vertex * 3 + 1], nz = normals[vertex * 3 + 2];
                            const double tx = (c00 * nx + c01 * ny + c02 * nz) * handedness;
                            const double ty = (c10 * nx + c11 * ny + c12 * nz) * handedness;
                            const double tz = (c20 * nx + c21 * ny + c22 * nz) * handedness;
                            const double lengthSquared = tx * tx + ty * ty + tz * tz;
                            if (!std::isfinite(lengthSquared) || lengthSquared <= 0.0)
                                return Fail("authored normal is zero-length or not finite");
                            const double inverseLength = 1.0 / std::sqrt(lengthSquared);
                            target.Normal[0] = static_cast<float>(tx * inverseLength);
                            target.Normal[1] = static_cast<float>(ty * inverseLength);
                            target.Normal[2] = static_cast<float>(tz * inverseLength);
                        }
                        if (instance.Uv)
                        {
                            target.UV[0] = uvs[vertex * 2];
                            target.UV[1] = uvs[vertex * 2 + 1];
                        }
                    }

                    u32* indexOutput = local.Indices.data() + firstIndex;
                    const u32 vertexBase = static_cast<u32>(firstVertex);
                    const bool flip = determinant < 0.0;
                    for (size_t index = 0; index < indexCount; index += 3)
                    {
                        const u32 a = indices[index], b = indices[index + 1], c = indices[index + 2];
                        if (a >= vertexCount || b >= vertexCount || c >= vertexCount)
                            return Fail("primitive index is outside its vertex range");
                        indexOutput[index] = vertexBase + a;
                        indexOutput[index + 1] = vertexBase + (flip ? c : b);
                        indexOutput[index + 2] = vertexBase + (flip ? b : c);
                    }

                    MeshArtifactPrimitive record;
                    record.SourceMeshIndex = instance.MeshIndex;
                    record.SourcePrimitiveIndex = instance.PrimitiveIndex;
                    record.VertexByteOffset = static_cast<u64>(firstVertex) * sizeof(MeshArtifactVertex);
                    record.VertexByteSize = static_cast<u64>(vertexCount) * sizeof(MeshArtifactVertex);
                    record.IndexByteOffset = static_cast<u64>(firstIndex) * sizeof(u32);
                    record.IndexByteSize = static_cast<u64>(indexCount) * sizeof(u32);
                    local.Primitives.push_back(record);
                    firstVertex += vertexCount;
                    firstIndex += indexCount;
                }

                if (Poll())
                    return false;
                // The mesh identity is assigned by AssembleAssets; the geometric-normal
                // derivation does not depend on it.
                std::string geometryError;
                if (m_DeriveNormals && !EnsureMeshArtifactGeometricNormals(local, geometryError))
                    return Fail(geometryError);
                mesh = std::move(local);
                return true;
            }

            bool DecodeImage(const cgltf_image* image, std::string_view what, const DecodedCommonImage*& decoded)
            {
                const auto cached = m_Decoded.find(image);
                if (cached != m_Decoded.end())
                {
                    decoded = &cached->second;
                    return true;
                }
                std::vector<u8> fileBytes;
                std::span<const u8> bytes;
                if (image->uri)
                {
                    std::string relativePath;
                    if (!ResolveUri(image->uri, relativePath))
                        return false;
                    const auto member = m_Files.find(relativePath);
                    if (member == m_Files.end())
                        return Fail("glTF image dependency is not a member of the snapshot");
                    if (member->second > m_Options.ImageLimits.MaximumSourceBytes)
                        return Fail(std::string(what) + " texture image exceeds the configured source size limit");
                    if (!ReadMember(relativePath, fileBytes))
                        return false;
                    bytes = fileBytes;
                }
                else if (image->buffer_view)
                {
                    const cgltf_buffer_view& view = *image->buffer_view;
                    const uint8_t* viewData = cgltf_buffer_view_data(&view);
                    if (!viewData || view.size > m_Options.ImageLimits.MaximumSourceBytes)
                        return Fail(std::string(what) + " texture embedded image is unreadable or too large");
                    bytes = std::span<const u8>(viewData, view.size);
                }
                else
                    return Fail(std::string(what) + " texture image has no source");

                DecodedCommonImage result;
                std::string decodeError;
                if (!DecodeCommonImage(bytes, m_Options.ImageLimits, result, decodeError))
                    return Fail(std::string(what) + " texture image rejected: " + decodeError);
                if (image->mime_type)
                {
                    std::string mime(image->mime_type);
                    for (char& character : mime)
                        if (character >= 'A' && character <= 'Z')
                            character = static_cast<char>(character + ('a' - 'A'));
                    const bool agrees = (mime == "image/png" && result.Source == CommonImageSource::Png)
                        || (mime == "image/jpeg" && result.Source == CommonImageSource::Jpeg);
                    if (!agrees)
                        return Fail(std::string(what) + " texture image MIME type disagrees with its content");
                }
                if (result.Rgba8.size() > m_Options.Limits.MaximumTotalDecodedImageBytes - m_DecodedBytes)
                    return Fail("decoded texture images exceed the configured aggregate size limit");
                m_DecodedBytes += result.Rgba8.size();
                const auto inserted = m_Decoded.emplace(image, std::move(result));
                decoded = &inserted.first->second;
                if (!Checkpoint(FabGltfStage::ImageDecoded, what))
                    return false;
                return true;
            }

            bool AddTexture(FabGltfPreparedPackage& package, const TextureRoleSpec& spec, u32 width, u32 height,
                std::vector<u8>&& rgba, MaterialTextureSampler sampler)
            {
                NormalizedTextureSource source;
                source.SourcePath = "fab:" + m_StreamId + "/" + LogicalPathForRole(spec.SemanticRole);
                source.Role = spec.Role;
                source.ColorSpace = spec.ColorSpace;
                source.Width = width;
                source.Height = height;
                source.Mips.push_back(std::move(rgba));
                source.MipPolicy = TextureMipPolicy::CompleteMissing;
                TextureArtifact artifact;
                std::string buildError;
                if (!TextureImporter::BuildNormalizedRgba8Artifact(source, source.SourcePath,
                    TextureTargetProfile::RGBAFallback, false, artifact, buildError))
                    return Fail(std::string(spec.Label) + " texture could not be cooked: " + buildError);
                artifact.Asset = ComputeFabStableAssetHandle(m_StreamId, AssetType::Texture, spec.SemanticRole);
                package.Material.GetTexture(spec.Slot) = artifact.Asset;
                package.Material.GetSampler(spec.Slot) = sampler;
                package.Textures.push_back(std::move(artifact));
                return Checkpoint(FabGltfStage::TextureCooked, spec.Label);
            }

            static std::vector<u8> OpaqueCopy(const DecodedCommonImage& image)
            {
                std::vector<u8> pixels = image.Rgba8;
                for (size_t offset = 3; offset < pixels.size(); offset += 4)
                    pixels[offset] = 255;
                return pixels;
            }

            bool BuildTextures(FabGltfPreparedPackage& package)
            {
                const DecodedCommonImage* decoded = nullptr;
                if (m_BaseColorView.Image)
                {
                    if (!DecodeImage(m_BaseColorView.Image, "base color", decoded)
                        || !AddTexture(package, GetRoleSpec(TextureRole::BaseColor), decoded->Width, decoded->Height,
                            OpaqueCopy(*decoded), m_BaseColorView.Sampler))
                        return false;
                }
                if (m_MetallicRoughnessView.Image || m_OcclusionView.Image)
                {
                    const DecodedCommonImage* metallicRoughness = nullptr;
                    const DecodedCommonImage* occlusion = nullptr;
                    if (m_MetallicRoughnessView.Image && m_OcclusionView.Image
                        && m_MetallicRoughnessView.Sampler != m_OcclusionView.Sampler)
                        return Fail("metallic-roughness and occlusion textures use different samplers");
                    if (m_MetallicRoughnessView.Image
                        && !DecodeImage(m_MetallicRoughnessView.Image, "metallic-roughness", metallicRoughness))
                        return false;
                    if (m_OcclusionView.Image && !DecodeImage(m_OcclusionView.Image, "occlusion", occlusion))
                        return false;
                    const DecodedCommonImage& shape = metallicRoughness ? *metallicRoughness : *occlusion;
                    if (metallicRoughness && occlusion && metallicRoughness != occlusion
                        && (metallicRoughness->Width != occlusion->Width || metallicRoughness->Height != occlusion->Height))
                        return Fail("metallic-roughness and occlusion textures differ in size");
                    std::vector<u8> pixels = OpaqueCopy(shape);
                    if (metallicRoughness && occlusion && metallicRoughness != occlusion)
                    {
                        for (size_t offset = 0; offset < pixels.size(); offset += 4)
                            pixels[offset] = occlusion->Rgba8[offset];
                    }
                    else if (!occlusion)
                    {
                        for (size_t offset = 0; offset < pixels.size(); offset += 4)
                            pixels[offset] = 255;
                    }
                    else if (!metallicRoughness)
                    {
                        for (size_t offset = 0; offset < pixels.size(); offset += 4)
                        {
                            pixels[offset] = occlusion->Rgba8[offset];
                            pixels[offset + 1] = 255;
                            pixels[offset + 2] = 255;
                        }
                    }
                    const MaterialTextureSampler sampler = metallicRoughness
                        ? m_MetallicRoughnessView.Sampler : m_OcclusionView.Sampler;
                    if (!AddTexture(package, GetRoleSpec(TextureRole::Orm), shape.Width, shape.Height,
                        std::move(pixels), sampler))
                        return false;
                }
                if (m_NormalView.Image)
                {
                    if (!DecodeImage(m_NormalView.Image, "normal", decoded)
                        || !AddTexture(package, GetRoleSpec(TextureRole::Normal), decoded->Width, decoded->Height,
                            OpaqueCopy(*decoded), m_NormalView.Sampler))
                        return false;
                }
                if (m_EmissiveView.Image)
                {
                    if (!DecodeImage(m_EmissiveView.Image, "emissive", decoded)
                        || !AddTexture(package, GetRoleSpec(TextureRole::Emissive), decoded->Width, decoded->Height,
                            OpaqueCopy(*decoded), m_EmissiveView.Sampler))
                        return false;
                }
                return true;
            }

            bool BuildMaterial(FabGltfPreparedPackage& package)
            {
                MaterialAsset& material = package.Material;
                const std::string fallbackName = SanitizeName(m_Declaration.ProductName, "Fab Material");
                material.Name = m_GltfMaterial && m_GltfMaterial->name
                    ? SanitizeName(m_GltfMaterial->name, fallbackName) : fallbackName;
                material.ShadingModel = MaterialShadingModel::Standard;
                material.AlphaMode = MaterialAlphaMode::Opaque;
                material.EmissiveStrength = 1.0f;
                float baseColor[3] { 1.0f, 1.0f, 1.0f };
                float metallic = 1.0f;
                float roughness = 1.0f;
                float normalScale = 1.0f;
                float occlusionStrength = 1.0f;
                float emissive[3] { 0.0f, 0.0f, 0.0f };
                if (m_GltfMaterial)
                {
                    const cgltf_material& source = *m_GltfMaterial;
                    material.TwoSided = source.double_sided != 0;
                    std::copy(std::begin(source.pbr_metallic_roughness.base_color_factor),
                        std::begin(source.pbr_metallic_roughness.base_color_factor) + 3, baseColor);
                    metallic = source.pbr_metallic_roughness.metallic_factor;
                    roughness = source.pbr_metallic_roughness.roughness_factor;
                    std::copy(std::begin(source.emissive_factor), std::end(source.emissive_factor), emissive);
                    if (source.normal_texture.texture)
                        normalScale = source.normal_texture.scale;
                    if (source.occlusion_texture.texture)
                        occlusionStrength = source.occlusion_texture.scale;
                }
                material.BaseColor = { QuantizeToMaterialPrecision(baseColor[0]),
                    QuantizeToMaterialPrecision(baseColor[1]), QuantizeToMaterialPrecision(baseColor[2]) };
                material.Metallic = QuantizeToMaterialPrecision(metallic);
                material.Roughness = QuantizeToMaterialPrecision(roughness);
                material.NormalScale = QuantizeToMaterialPrecision(normalScale);
                material.OcclusionStrength = QuantizeToMaterialPrecision(occlusionStrength);
                material.EmissiveColor = { QuantizeToMaterialPrecision(emissive[0]),
                    QuantizeToMaterialPrecision(emissive[1]), QuantizeToMaterialPrecision(emissive[2]) };
                if (!IsValidMaterialAssetValues(material))
                    return Fail("material factors are outside the range supported by the engine material");
                return true;
            }

            bool AssembleAssets(FabGltfPreparedPackage& package)
            {
                const auto describe = [this](AssetType type, std::string_view role, std::string name)
                {
                    FabGltfPreparedAsset asset;
                    asset.Type = type;
                    asset.SemanticRole = std::string(role);
                    asset.Handle = ComputeFabStableAssetHandle(m_StreamId, type, role);
                    asset.LogicalPath = LogicalPathForRole(role);
                    asset.RegistrySourcePath = "fab:" + m_StreamId + "/" + asset.LogicalPath;
                    asset.Name = std::move(name);
                    asset.GenerationRelativeCookedPath = std::string(ArtifactDirectory(type)) + "/"
                        + std::to_string(asset.Handle) + ArtifactExtension(type);
                    return asset;
                };
                const std::string productName = SanitizeName(m_Declaration.ProductName, "Fab Asset");
                package.Assets.push_back(describe(AssetType::Mesh, kMeshRole, productName));
                package.Assets.push_back(describe(AssetType::Material, kMaterialRole, package.Material.Name));
                for (const TextureArtifact& texture : package.Textures)
                {
                    const TextureRoleSpec& spec = GetRoleSpec(texture.Role);
                    package.Assets.push_back(describe(AssetType::Texture, spec.SemanticRole,
                        productName + " " + std::string(spec.Label)));
                }
                if (package.Assets.front().Handle == kInvalidAssetHandle)
                    return Fail("stable asset handle could not be derived");

                package.Mesh.Asset = package.Assets[0].Handle;
                package.Mesh.SourcePath = package.Assets[0].RegistrySourcePath;
                std::string meshError;
                if (!ValidateMeshArtifact(package.Mesh, meshError))
                    return Fail(meshError);

                FabImportReceipt draft = m_Declaration;
                draft.StreamId = m_StreamId;
                draft.GenerationId = m_GenerationId;
                draft.ExpandedTreeSha256 = m_Snapshot.GetTreeSha256();
                draft.ImporterVersion = std::string(kFabGltfImporterVersion);
                draft.CookerVersion = std::string(kFabGltfCookerVersion);
                for (const FabGltfPreparedAsset& asset : package.Assets)
                {
                    FabImportedAssetRecord record;
                    record.Handle = asset.Handle;
                    record.Type = asset.Type;
                    record.SemanticRole = asset.SemanticRole;
                    record.LogicalPath = asset.LogicalPath;
                    record.GenerationRelativeCookedPath = asset.GenerationRelativeCookedPath;
                    draft.Assets.push_back(std::move(record));
                }
                package.ReceiptDraft = std::move(draft);
                return true;
            }

            const LocalPackageSnapshot& m_Snapshot;
            const FabImportReceipt& m_Declaration;
            const FabGltfPrepareOptions& m_Options;
            std::string m_Error;
            bool m_IsGlb = false;
            std::string m_StreamId;
            std::string m_GenerationId;
            std::unordered_map<std::string, u64> m_Files;
            std::vector<u8> m_Root;
            std::vector<std::vector<u8>> m_BufferStorage;
            std::unique_ptr<cgltf_data, decltype(&cgltf_free)> m_Data;
            std::vector<PrimitiveInstance> m_Instances;
            std::vector<std::array<double, 16>> m_NodeWorld;
            const cgltf_material* m_GltfMaterial = nullptr;
            ViewInfo m_BaseColorView, m_EmissiveView, m_NormalView, m_MetallicRoughnessView, m_OcclusionView;
            bool m_Textured = false;
            bool m_DeriveNormals = false;
            u64 m_TotalVertices = 0;
            u64 m_TotalIndices = 0;
            u64 m_MaxInstanceVertices = 0;
            u64 m_MaxInstanceIndices = 0;
            u64 m_DecodedBytes = 0;
            std::unordered_map<const cgltf_image*, DecodedCommonImage> m_Decoded;
        };
    }

    bool PrepareFabGltfPackage(const LocalPackageSnapshot& snapshot, const FabImportReceipt& declaration,
        const FabGltfPrepareOptions& options, FabGltfPreparedPackage& out, std::string& error)
    {
        PrepareSession session(snapshot, declaration, options);
        return session.Run(out, error);
    }

    std::string GetFabCookedRoot(std::string_view generationId)
    {
        if (!IsLowerHex64(generationId))
            return {};
        return "fab/" + std::string(generationId);
    }

    std::filesystem::path GetFabGltfCookedMaterialPath(AssetHandle material, std::string_view cookedRoot,
        const std::filesystem::path& cookedArtifactBase)
    {
        if (material == kInvalidAssetHandle || cookedRoot.empty() || !AssetRegistry::IsValidCookedRoot(cookedRoot)
            || cookedArtifactBase.empty() || !cookedArtifactBase.is_absolute())
            return {};
        return (cookedArtifactBase / std::filesystem::path(std::string(cookedRoot)) / "materials"
            / (std::to_string(material) + ".spiralmat")).lexically_normal();
    }

    namespace
    {
        // Removes exactly the files and directories this call created.
        class StagingCleanup
        {
        public:
            ~StagingCleanup()
            {
                if (m_Armed)
                    Run();
            }

            void TrackDirectory(const std::filesystem::path& path) { m_Directories.push_back(path); }
            void TrackFile(const std::filesystem::path& path) { m_Files.push_back(path); }
            void Release() { m_Armed = false; }

        private:
            void Run() noexcept
            {
                std::error_code ignored;
                for (auto file = m_Files.rbegin(); file != m_Files.rend(); ++file)
                    std::filesystem::remove(*file, ignored);
                for (auto directory = m_Directories.rbegin(); directory != m_Directories.rend(); ++directory)
                    std::filesystem::remove(*directory, ignored);
            }

            std::vector<std::filesystem::path> m_Files;
            std::vector<std::filesystem::path> m_Directories;
            bool m_Armed = true;
        };

        bool HashFile(const std::filesystem::path& path, std::string& digest)
        {
            std::ifstream input(path, std::ios::binary);
            if (!input)
                return false;
            Sha256Builder hash;
            std::array<char, 64 * 1024> buffer {};
            while (input.read(buffer.data(), static_cast<std::streamsize>(buffer.size())) || input.gcount() > 0)
                hash.Update(std::span<const u8>(reinterpret_cast<const u8*>(buffer.data()),
                    static_cast<size_t>(input.gcount())));
            if (!input.eof())
                return false;
            digest = hash.FinalizeHex();
            return true;
        }

        bool SameMesh(const MeshArtifact& left, const MeshArtifact& right)
        {
            if (left.Asset != right.Asset || left.SourcePath != right.SourcePath
                || left.Primitives.size() != right.Primitives.size() || left.Vertices.size() != right.Vertices.size()
                || left.Indices != right.Indices)
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
            return std::memcmp(left.Vertices.data(), right.Vertices.data(),
                left.Vertices.size() * sizeof(MeshArtifactVertex)) == 0;
        }

        bool SameTexture(const TextureArtifact& left, const TextureArtifact& right)
        {
            if (left.Asset != right.Asset || left.SourcePath != right.SourcePath || left.Role != right.Role
                || left.ColorSpace != right.ColorSpace || left.TargetProfile != right.TargetProfile
                || left.CookedFormat != right.CookedFormat || left.HasAlpha != right.HasAlpha
                || left.Mips.size() != right.Mips.size() || left.Payload != right.Payload)
                return false;
            for (size_t index = 0; index < left.Mips.size(); ++index)
                if (left.Mips[index].Width != right.Mips[index].Width || left.Mips[index].Height != right.Mips[index].Height
                    || left.Mips[index].ByteOffset != right.Mips[index].ByteOffset
                    || left.Mips[index].ByteSize != right.Mips[index].ByteSize)
                    return false;
            return true;
        }

        bool SameMaterial(const MaterialAsset& left, const MaterialAsset& right)
        {
            const auto vec = [](const Math::Vec3& a, const Math::Vec3& b) { return a.X == b.X && a.Y == b.Y && a.Z == b.Z; };
            return left.Name == right.Name && left.ShadingModel == right.ShadingModel && left.AlphaMode == right.AlphaMode
                && left.TwoSided == right.TwoSided && vec(left.BaseColor, right.BaseColor)
                && left.Metallic == right.Metallic && left.Roughness == right.Roughness
                && left.NormalScale == right.NormalScale && left.OcclusionStrength == right.OcclusionStrength
                && vec(left.EmissiveColor, right.EmissiveColor) && left.EmissiveStrength == right.EmissiveStrength
                && left.AlphaCutoff == right.AlphaCutoff
                && left.Textures.BaseColor == right.Textures.BaseColor && left.Textures.Normal == right.Textures.Normal
                && left.Textures.Orm == right.Textures.Orm && left.Textures.Emissive == right.Textures.Emissive
                && left.Textures.Opacity == right.Textures.Opacity
                && left.Textures.CallistoControl == right.Textures.CallistoControl
                && left.Samplers.BaseColor == right.Samplers.BaseColor && left.Samplers.Normal == right.Samplers.Normal
                && left.Samplers.Orm == right.Samplers.Orm && left.Samplers.Emissive == right.Samplers.Emissive
                && left.Samplers.Opacity == right.Samplers.Opacity
                && left.Samplers.CallistoControl == right.Samplers.CallistoControl;
        }
    }

    bool StageFabGltfGeneration(const FabGltfPreparedPackage& prepared, const std::filesystem::path& stagingBase,
        const FabGltfStageOptions& options, FabGltfStagedGeneration& out, std::string& error)
    {
        const auto fail = [&error](std::string message)
        {
            error = std::move(message);
            return false;
        };
        try
        {
            const std::string cookedRoot = GetFabCookedRoot(prepared.ReceiptDraft.GenerationId);
            if (cookedRoot.empty() || prepared.Assets.size() < 2 || prepared.Assets.size() != 2 + prepared.Textures.size()
                || prepared.ReceiptDraft.Assets.size() != prepared.Assets.size()
                || prepared.Mesh.Asset != prepared.Assets[0].Handle)
                return fail("prepared Fab package is incomplete");
            for (size_t index = 0; index < prepared.Assets.size(); ++index)
                if (prepared.ReceiptDraft.Assets[index].Handle != prepared.Assets[index].Handle
                    || prepared.ReceiptDraft.Assets[index].GenerationRelativeCookedPath
                        != prepared.Assets[index].GenerationRelativeCookedPath
                    || (index >= 2 && prepared.Textures[index - 2].Asset != prepared.Assets[index].Handle))
                    return fail("prepared Fab package is incomplete");
            if (std::locale() != std::locale::classic())
                return fail("cooked artifacts require the classic global C++ locale");

            const auto checkpoint = [&options, &error](FabGltfStage stage, std::string_view label)
            {
                if (options.TestHook)
                    options.TestHook(stage, label);
                if (options.IsCancelled && options.IsCancelled())
                {
                    error = "Fab glTF staging was cancelled";
                    return false;
                }
                return true;
            };
            if (!checkpoint(FabGltfStage::Started, {}))
                return false;

            std::error_code filesystemError;
            const std::filesystem::file_status baseStatus = std::filesystem::symlink_status(stagingBase, filesystemError);
            if (filesystemError || !std::filesystem::is_directory(baseStatus) || !stagingBase.is_absolute())
                return fail("staging base must be an existing absolute directory");
            const std::filesystem::path fabDirectory = stagingBase / "fab";
            const std::filesystem::path generationDirectory = fabDirectory / prepared.ReceiptDraft.GenerationId;
            const std::filesystem::file_status generationStatus =
                std::filesystem::symlink_status(generationDirectory, filesystemError);
            if (std::filesystem::exists(generationStatus))
                return fail("staged generation directory already exists");

            StagingCleanup cleanup;
            const auto createDirectory = [&](const std::filesystem::path& path)
            {
                std::error_code createError;
                if (!std::filesystem::create_directory(path, createError) || createError)
                    return false;
                cleanup.TrackDirectory(path);
                return true;
            };
            const std::filesystem::file_status fabStatus = std::filesystem::symlink_status(fabDirectory, filesystemError);
            if (std::filesystem::exists(fabStatus) ? !std::filesystem::is_directory(fabStatus)
                    : !createDirectory(fabDirectory))
                return fail("could not create the staging generation directories");
            if (!createDirectory(generationDirectory))
                return fail("could not create the staging generation directories");
            for (const char* name : { "meshes", "textures", "materials" })
                if (!createDirectory(generationDirectory / name))
                    return fail("could not create the staging generation directories");
            if (!checkpoint(FabGltfStage::StagingDirectoriesCreated, {}))
                return false;

            FabImportReceipt receipt = prepared.ReceiptDraft;
            const auto stageArtifact = [&](size_t index, auto&& store, auto&& verify) -> bool
            {
                const FabGltfPreparedAsset& asset = prepared.Assets[index];
                if (!checkpoint(FabGltfStage::ArtifactWritten, asset.LogicalPath))
                    return false;
                const std::filesystem::path path = generationDirectory / asset.GenerationRelativeCookedPath;
                std::string artifactError;
                cleanup.TrackFile(path);
                if (!store(path, artifactError))
                    return fail("could not write " + asset.LogicalPath + ": " + artifactError);
                if (!verify(path, artifactError))
                    return fail("staged " + asset.LogicalPath + " does not read back identically: " + artifactError);
                if (!HashFile(path, receipt.Assets[index].ArtifactSha256))
                    return fail("could not hash staged " + asset.LogicalPath);
                return checkpoint(FabGltfStage::ArtifactWritten, asset.LogicalPath);
            };

            if (!stageArtifact(0,
                    [&](const std::filesystem::path& path, std::string& e) { return StoreMeshArtifact(path, prepared.Mesh, e); },
                    [&](const std::filesystem::path& path, std::string& e)
                    {
                        MeshArtifact loaded;
                        if (!LoadMeshArtifact(path, loaded, e))
                            return false;
                        if (!SameMesh(loaded, prepared.Mesh))
                            e = "mesh differs after reload";
                        return e.empty();
                    })
                || !stageArtifact(1,
                    [&](const std::filesystem::path& path, std::string& e)
                    {
                        if (prepared.Material.SaveToFile(path))
                            return true;
                        e = "material file could not be saved";
                        return false;
                    },
                    [&](const std::filesystem::path& path, std::string& e)
                    {
                        MaterialAsset loaded;
                        if (!MaterialAsset::LoadFromFile(path, loaded) || !SameMaterial(loaded, prepared.Material))
                            e = "material differs after reload";
                        return e.empty();
                    }))
                return false;
            for (size_t index = 0; index < prepared.Textures.size(); ++index)
            {
                const TextureArtifact& texture = prepared.Textures[index];
                if (!stageArtifact(2 + index,
                        [&](const std::filesystem::path& path, std::string& e) { return StoreTextureArtifact(path, texture, e); },
                        [&](const std::filesystem::path& path, std::string& e)
                        {
                            TextureArtifact loaded;
                            if (!LoadTextureArtifact(path, loaded, e))
                                return false;
                            if (!SameTexture(loaded, texture))
                                e = "texture differs after reload";
                            return e.empty();
                        }))
                    return false;
            }

            FabImportReceipt validated = receipt;
            validated.Relation = FabGenerationRelation::Initial;
            validated.RelatedStreamId.clear();
            validated.RelatedGenerationId.clear();
            std::string receiptError;
            if (!ValidateFabImportReceipt(validated, receiptError))
                return fail("staged receipt is invalid: " + receiptError);
            if (!checkpoint(FabGltfStage::StagingCompleted, {}))
                return false;

            out.CookedRoot = cookedRoot;
            out.Receipt = std::move(receipt);
            cleanup.Release();
            error.clear();
            return true;
        }
        catch (...)
        {
            return fail("Fab glTF staging failed on an exception or allocation failure");
        }
    }

    bool BuildFabGltfCandidate(const AssetRegistry& baseRegistry, const FabReceiptCollection& priorReceipts,
        const FabGltfPreparedPackage& prepared, const FabGltfStagedGeneration& staged,
        const std::filesystem::path& finalCookedBase, FabGltfCandidate& out, std::string& error)
    {
        try
        {
            FabImportReceipt receipt = staged.Receipt;
            const std::string cookedRoot = GetFabCookedRoot(receipt.GenerationId);
            if (cookedRoot.empty() || cookedRoot != staged.CookedRoot || prepared.Assets.size() != receipt.Assets.size()
                || prepared.Assets.size() != 2 + prepared.Textures.size())
            {
                error = "staged Fab generation does not match the prepared package";
                return false;
            }
            for (size_t index = 0; index < prepared.Assets.size(); ++index)
                if (prepared.Assets[index].Handle != receipt.Assets[index].Handle
                    || prepared.Assets[index].SemanticRole != receipt.Assets[index].SemanticRole)
                {
                    error = "staged Fab receipt assets do not match the prepared package";
                    return false;
                }

            AssetRegistry candidateRegistry = baseRegistry;
            if (!baseRegistry.GetCookedArtifactBasePath().empty())
            {
                AssetRegistry probe;
                if (!probe.SetCookedArtifactBasePath(finalCookedBase)
                    || probe.GetCookedArtifactBasePath() != baseRegistry.GetCookedArtifactBasePath())
                {
                    error = "candidate cooked base differs from the registry's cooked artifact base";
                    return false;
                }
            }
            else if (!candidateRegistry.SetCookedArtifactBasePath(finalCookedBase))
            {
                error = "candidate cooked base path is invalid";
                return false;
            }

            if (!AssignFabGenerationRelation(priorReceipts, receipt, error))
                return false;
            const FabReceiptDecision decision = ClassifyFabImportReceipt(priorReceipts, receipt);
            switch (decision.Kind)
            {
                case FabReceiptDecisionKind::ExactReuse:
                case FabReceiptDecisionKind::AddNewStream:
                case FabReceiptDecisionKind::AddProductUpdateStream:
                case FabReceiptDecisionKind::ReplaceSameStreamSource:
                    break;
                default:
                    out.Decision = decision;
                    error = decision.Diagnostic;
                    return false;
            }

            const auto metadataFor = [](const FabGltfPreparedAsset& asset, const std::string& root)
            {
                AssetMetadata metadata;
                metadata.Handle = asset.Handle;
                metadata.Type = asset.Type;
                metadata.SourcePath = asset.RegistrySourcePath;
                metadata.Name = asset.Name;
                metadata.SourcePolicy = AssetSourcePolicy::ImmutablePackage;
                metadata.CookedRoot = root;
                return metadata;
            };

            FabGltfCandidate candidate;
            candidate.Decision = decision;
            candidate.Receipts = priorReceipts;
            candidate.Material = prepared.Material;
            candidate.Receipt = receipt;
            if (decision.Kind == FabReceiptDecisionKind::ExactReuse)
            {
                const std::string existingRoot = GetFabCookedRoot(decision.ExistingGenerationId);
                for (const FabGltfPreparedAsset& asset : prepared.Assets)
                {
                    const AssetMetadata* existing = baseRegistry.GetAsset(asset.Handle);
                    if (!existing || existing->Type != asset.Type || existing->SourcePath != asset.RegistrySourcePath
                        || existing->SourcePolicy != AssetSourcePolicy::ImmutablePackage
                        || existing->CookedRoot != existingRoot)
                    {
                        error = "registry does not hold the previously accepted Fab generation";
                        return false;
                    }
                }
                candidate.Registry = baseRegistry;
                candidate.NeedsPublish = false;
                out = std::move(candidate);
                error.clear();
                return true;
            }

            if (decision.Kind == FabReceiptDecisionKind::ReplaceSameStreamSource)
            {
                const AssetGeneration expected { AssetSourcePolicy::ImmutablePackage, GetFabCookedRoot(decision.ExistingGenerationId) };
                const AssetGeneration replacement { AssetSourcePolicy::ImmutablePackage, cookedRoot };
                for (const FabGltfPreparedAsset& asset : prepared.Assets)
                {
                    const AssetMetadata* existing = candidateRegistry.GetAsset(asset.Handle);
                    if (!existing || existing->Type != asset.Type || existing->SourcePath != asset.RegistrySourcePath
                        || !candidateRegistry.CompareAndSwapAssetGeneration(asset.Handle, expected, replacement))
                    {
                        error = "registry does not hold the previous generation of this Fab stream";
                        return false;
                    }
                }
            }
            else
            {
                for (const FabGltfPreparedAsset& asset : prepared.Assets)
                {
                    if (candidateRegistry.Contains(asset.Handle)
                        || !candidateRegistry.RegisterAsset(metadataFor(asset, cookedRoot)))
                    {
                        error = "registry already contains or cannot register an asset of this Fab generation";
                        return false;
                    }
                }
            }

            FabGltfCandidate accepted = std::move(candidate);
            accepted.Registry = std::move(candidateRegistry);
            FabReceiptDecision addDecision;
            if (!AddFabImportReceipt(accepted.Receipts, receipt, addDecision, error))
                return false;
            out = std::move(accepted);
            error.clear();
            return true;
        }
        catch (...)
        {
            error = "Fab glTF candidate construction failed on an exception or allocation failure";
            return false;
        }
    }
}
