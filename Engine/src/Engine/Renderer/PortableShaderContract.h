#pragma once

#include "Engine/Core/Base.h"
#include "Engine/RHI/Shader.h"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace Engine
{
    enum class PortableShaderTarget
    {
        Dxil,
        Spirv
    };

    enum class PortableShaderCacheSource
    {
        Compiled,
        DiskCache
    };

    struct PortableShaderDependency
    {
        std::string Path;
        std::string ContentHash;
        std::string Content;

        bool operator==(const PortableShaderDependency&) const = default;
    };

    struct PortableShaderBinding
    {
        std::string Name;
        char Kind = 'b';
        u32 Register = 0;
        u32 Space = 0;
        RHI::ShaderStage Stages = RHI::ShaderStage::None;
        std::string ResourceKind;
        std::string TypeShape;
        u32 Count = 1;
        u32 ByteSize = 0;
        u32 Rows = 0;
        u32 Columns = 0;

        bool operator==(const PortableShaderBinding&) const = default;
    };

    struct PortableShaderVertexInput
    {
        std::string Name;
        std::string Semantic;
        u32 SemanticIndex = 0;
        u32 Location = 0;
        std::string TypeShape;
        u32 ByteSize = 0;
        u32 Rows = 0;
        u32 Columns = 0;

        bool operator==(const PortableShaderVertexInput&) const = default;
    };

    struct PortableShaderConventions
    {
        u32 Version = 1;
        bool RowMajor = true;
        bool ZeroToOneDepth = true;
        bool VulkanYFlip = true;
        bool ClockwiseFrontFace = true;
        std::string Coordinates = "RightHanded";
        std::string BindingPolicy = "D3DRegisterSpace";

        bool operator==(const PortableShaderConventions&) const = default;
    };

    struct PortableShaderRequest
    {
        std::string SourceName;
        std::string Source;
        std::string EntryPoint;
        std::string CompilerIdentity;
        std::string CompilerVersion;
        std::string CompilerPackageHash;
        std::string DownstreamCompilerPackageHash;
        RHI::ShaderStage Stage = RHI::ShaderStage::None;
        std::vector<PortableShaderTarget> Targets;
        std::vector<std::string> Defines;
        std::vector<std::string> Options;
        std::vector<PortableShaderDependency> Dependencies;
        u32 LayoutVersion = 1;
        u32 ReflectionVersion = 2;
        PortableShaderConventions Conventions;
        std::vector<PortableShaderBinding> ExpectedLayout;
        std::vector<PortableShaderVertexInput> ExpectedVertexInputs;

        bool operator==(const PortableShaderRequest&) const = default;
    };

    struct PortableShaderDiagnostic
    {
        std::string Source;
        std::string EntryPoint;
        std::string Target;
        std::string Backend;
        std::string Message;

        bool operator==(const PortableShaderDiagnostic&) const = default;
    };

    struct PortableShaderPackage
    {
        u32 Version = 2;
        std::string Key;
        std::vector<u8> Dxil;
        std::vector<u8> Spirv;
        std::vector<PortableShaderBinding> Reflection;
        std::vector<PortableShaderVertexInput> VertexInputs;
        PortableShaderConventions Conventions;
        PortableShaderCacheSource CacheSource = PortableShaderCacheSource::Compiled;
        std::vector<PortableShaderDiagnostic> Diagnostics;

        bool Succeeded() const { return Diagnostics.empty() && !Dxil.empty() && !Spirv.empty(); }
        bool operator==(const PortableShaderPackage&) const = default;
    };

    class PortableShaderContract
    {
    public:
        static std::string Sha256(std::string_view content);
        static std::string CacheKey(const PortableShaderRequest& request);
        static bool Validate(
            const PortableShaderRequest& request,
            const std::vector<PortableShaderBinding>& bindings,
            const std::vector<PortableShaderVertexInput>& vertexInputs,
            std::string& error);
        static bool StoreAtomic(const std::filesystem::path& path, const PortableShaderPackage& package);
        static bool Load(
            const std::filesystem::path& path,
            std::string_view expectedKey,
            PortableShaderPackage& package);
    };
}
