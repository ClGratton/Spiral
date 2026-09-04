#include "Engine/Renderer/PortableShaderContract.h"

#include "Engine/Core/Sha256.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <fstream>
#include <limits>
#include <sstream>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

namespace Engine
{
    namespace
    {
        constexpr u64 kMaximumStringBytes = 64ull * 1024ull * 1024ull;
        constexpr u64 kMaximumBinaryBytes = 256ull * 1024ull * 1024ull;
        constexpr u64 kMaximumReflectionItems = 4096;

        void Append(std::string& destination, std::string_view value)
        {
            destination += std::to_string(value.size());
            destination += ':';
            destination.append(value);
        }

        template <typename T>
        void AppendNumber(std::string& destination, T value)
        {
            Append(destination, std::to_string(value));
        }

        void WriteString(std::ofstream& stream, std::string_view value)
        {
            const u64 size = value.size();
            stream.write(reinterpret_cast<const char*>(&size), sizeof(size));
            stream.write(value.data(), static_cast<std::streamsize>(size));
        }

        bool ReadString(std::ifstream& stream, std::string& value)
        {
            u64 size = 0;
            if (!stream.read(reinterpret_cast<char*>(&size), sizeof(size)) || size > kMaximumStringBytes)
                return false;
            value.resize(static_cast<size_t>(size));
            return size == 0 || !!stream.read(value.data(), static_cast<std::streamsize>(size));
        }

        void WriteBytes(std::ofstream& stream, const std::vector<u8>& value)
        {
            const u64 size = value.size();
            stream.write(reinterpret_cast<const char*>(&size), sizeof(size));
            stream.write(reinterpret_cast<const char*>(value.data()), static_cast<std::streamsize>(size));
        }

        bool ReadBytes(std::ifstream& stream, std::vector<u8>& value)
        {
            u64 size = 0;
            if (!stream.read(reinterpret_cast<char*>(&size), sizeof(size)) || size > kMaximumBinaryBytes)
                return false;
            value.resize(static_cast<size_t>(size));
            return size == 0 || !!stream.read(reinterpret_cast<char*>(value.data()), static_cast<std::streamsize>(size));
        }

        void WriteBinding(std::ofstream& stream, const PortableShaderBinding& binding)
        {
            WriteString(stream, binding.Name);
            stream.write(&binding.Kind, sizeof(binding.Kind));
            stream.write(reinterpret_cast<const char*>(&binding.Register), sizeof(binding.Register));
            stream.write(reinterpret_cast<const char*>(&binding.Space), sizeof(binding.Space));
            const u32 stages = static_cast<u32>(binding.Stages);
            stream.write(reinterpret_cast<const char*>(&stages), sizeof(stages));
            WriteString(stream, binding.ResourceKind);
            WriteString(stream, binding.TypeShape);
            stream.write(reinterpret_cast<const char*>(&binding.Count), sizeof(binding.Count));
            stream.write(reinterpret_cast<const char*>(&binding.ByteSize), sizeof(binding.ByteSize));
            stream.write(reinterpret_cast<const char*>(&binding.Rows), sizeof(binding.Rows));
            stream.write(reinterpret_cast<const char*>(&binding.Columns), sizeof(binding.Columns));
        }

        bool ReadBinding(std::ifstream& stream, PortableShaderBinding& binding)
        {
            u32 stages = 0;
            if (!ReadString(stream, binding.Name)
                || !stream.read(&binding.Kind, sizeof(binding.Kind))
                || !stream.read(reinterpret_cast<char*>(&binding.Register), sizeof(binding.Register))
                || !stream.read(reinterpret_cast<char*>(&binding.Space), sizeof(binding.Space))
                || !stream.read(reinterpret_cast<char*>(&stages), sizeof(stages))
                || !ReadString(stream, binding.ResourceKind)
                || !ReadString(stream, binding.TypeShape)
                || !stream.read(reinterpret_cast<char*>(&binding.Count), sizeof(binding.Count))
                || !stream.read(reinterpret_cast<char*>(&binding.ByteSize), sizeof(binding.ByteSize))
                || !stream.read(reinterpret_cast<char*>(&binding.Rows), sizeof(binding.Rows))
                || !stream.read(reinterpret_cast<char*>(&binding.Columns), sizeof(binding.Columns)))
            {
                return false;
            }
            binding.Stages = static_cast<RHI::ShaderStage>(stages);
            return true;
        }

        void WriteVertexInput(std::ofstream& stream, const PortableShaderVertexInput& input)
        {
            WriteString(stream, input.Name);
            WriteString(stream, input.Semantic);
            stream.write(reinterpret_cast<const char*>(&input.SemanticIndex), sizeof(input.SemanticIndex));
            stream.write(reinterpret_cast<const char*>(&input.Location), sizeof(input.Location));
            WriteString(stream, input.TypeShape);
            stream.write(reinterpret_cast<const char*>(&input.ByteSize), sizeof(input.ByteSize));
            stream.write(reinterpret_cast<const char*>(&input.Rows), sizeof(input.Rows));
            stream.write(reinterpret_cast<const char*>(&input.Columns), sizeof(input.Columns));
        }

        bool ReadVertexInput(std::ifstream& stream, PortableShaderVertexInput& input)
        {
            return ReadString(stream, input.Name)
                && ReadString(stream, input.Semantic)
                && !!stream.read(reinterpret_cast<char*>(&input.SemanticIndex), sizeof(input.SemanticIndex))
                && !!stream.read(reinterpret_cast<char*>(&input.Location), sizeof(input.Location))
                && ReadString(stream, input.TypeShape)
                && !!stream.read(reinterpret_cast<char*>(&input.ByteSize), sizeof(input.ByteSize))
                && !!stream.read(reinterpret_cast<char*>(&input.Rows), sizeof(input.Rows))
                && !!stream.read(reinterpret_cast<char*>(&input.Columns), sizeof(input.Columns));
        }

        bool SameBinding(const PortableShaderBinding& left, const PortableShaderBinding& right)
        {
            return left.Name == right.Name
                && left.Kind == right.Kind
                && left.Register == right.Register
                && left.Space == right.Space
                && left.Stages == right.Stages
                && left.ResourceKind == right.ResourceKind
                && left.TypeShape == right.TypeShape
                && left.Count == right.Count
                && left.ByteSize == right.ByteSize
                && left.Rows == right.Rows
                && left.Columns == right.Columns;
        }

        std::string BindingSummary(const PortableShaderBinding& binding)
        {
            std::ostringstream output;
            output << binding.Name << ' ' << binding.Kind << binding.Register << " space" << binding.Space
                << " resource=" << binding.ResourceKind << " type=" << binding.TypeShape
                << " count=" << binding.Count << " bytes=" << binding.ByteSize
                << " shape=" << binding.Rows << 'x' << binding.Columns
                << " stages=" << static_cast<u32>(binding.Stages);
            return output.str();
        }

        bool SameVertexInput(const PortableShaderVertexInput& left, const PortableShaderVertexInput& right)
        {
            return left.Name == right.Name
                && left.Semantic == right.Semantic
                && left.SemanticIndex == right.SemanticIndex
                && left.Location == right.Location
                && left.TypeShape == right.TypeShape
                && left.ByteSize == right.ByteSize
                && left.Rows == right.Rows
                && left.Columns == right.Columns;
        }

        std::string VertexInputSummary(const PortableShaderVertexInput& input)
        {
            std::ostringstream output;
            output << input.Name << ' ' << input.Semantic << input.SemanticIndex
                << " location=" << input.Location << " type=" << input.TypeShape
                << " bytes=" << input.ByteSize << " shape=" << input.Rows << 'x' << input.Columns;
            return output.str();
        }

        bool IsValidCachedPackage(const PortableShaderPackage& package)
        {
            if (package.Version != 2 || !package.Succeeded()
                || package.Conventions.Version != 2
                || package.Conventions.Coordinates != "RightHanded"
                || package.Conventions.BindingPolicy != "D3DRegisterSpaceVulkanClassOffsets0_100_200_300"
                || !package.Conventions.RowMajor
                || !package.Conventions.ZeroToOneDepth
                || !package.Conventions.VulkanYFlip
                || !package.Conventions.ClockwiseFrontFace)
            {
                return false;
            }
            for (const PortableShaderBinding& binding : package.Reflection)
            {
                if (binding.Name.empty() || binding.ResourceKind.empty() || binding.TypeShape.empty()
                    || binding.Count == 0 || binding.Stages == RHI::ShaderStage::None
                    || (binding.Kind != 'b' && binding.Kind != 't' && binding.Kind != 'u' && binding.Kind != 's'))
                {
                    return false;
                }
            }
            for (const PortableShaderVertexInput& input : package.VertexInputs)
            {
                if (input.Name.empty() || input.Semantic.empty() || input.TypeShape.empty()
                    || input.ByteSize == 0 || input.Rows == 0 || input.Columns == 0)
                {
                    return false;
                }
            }
            return true;
        }

        bool ReplacePublishedFile(const std::filesystem::path& temporary, const std::filesystem::path& final)
        {
#ifdef _WIN32
            if (::ReplaceFileW(final.c_str(), temporary.c_str(), nullptr, REPLACEFILE_WRITE_THROUGH, nullptr, nullptr))
                return true;
            const DWORD replaceError = ::GetLastError();
            if (replaceError != ERROR_FILE_NOT_FOUND && replaceError != ERROR_PATH_NOT_FOUND)
                return false;
            return !!::MoveFileExW(temporary.c_str(), final.c_str(), MOVEFILE_WRITE_THROUGH);
#else
            return std::rename(temporary.c_str(), final.c_str()) == 0;
#endif
        }

    }

    std::string PortableShaderContract::Sha256(std::string_view content)
    {
        return Sha256Builder::HashString(content);
    }

    std::string PortableShaderContract::CacheKey(const PortableShaderRequest& request)
    {
        std::string canonical;
        Append(canonical, "portable-shader-v2");
        Append(canonical, request.SourceName);
        Append(canonical, request.Source);
        Append(canonical, request.EntryPoint);
        Append(canonical, request.CompilerIdentity);
        Append(canonical, request.CompilerVersion);
        Append(canonical, request.CompilerPackageHash);
        Append(canonical, request.DownstreamCompilerPackageHash);
        AppendNumber(canonical, static_cast<int>(request.Stage));
        AppendNumber(canonical, request.LayoutVersion);
        AppendNumber(canonical, request.ReflectionVersion);
        AppendNumber(canonical, request.Conventions.Version);
        Append(canonical, request.Conventions.Coordinates);
        Append(canonical, request.Conventions.BindingPolicy);
        AppendNumber(canonical, request.Conventions.RowMajor);
        AppendNumber(canonical, request.Conventions.ZeroToOneDepth);
        AppendNumber(canonical, request.Conventions.VulkanYFlip);
        AppendNumber(canonical, request.Conventions.ClockwiseFrontFace);

        std::vector<std::string> defines = request.Defines;
        std::vector<std::string> options = request.Options;
        std::sort(defines.begin(), defines.end());
        std::sort(options.begin(), options.end());
        for (const std::string& define : defines) Append(canonical, define);
        for (const std::string& option : options) Append(canonical, option);

        std::vector<PortableShaderDependency> dependencies = request.Dependencies;
        std::sort(dependencies.begin(), dependencies.end(), [](const auto& left, const auto& right)
        {
            return left.Path < right.Path;
        });
        for (const PortableShaderDependency& dependency : dependencies)
        {
            Append(canonical, dependency.Path);
            Append(canonical, dependency.ContentHash);
        }
        for (PortableShaderTarget target : request.Targets)
            AppendNumber(canonical, static_cast<int>(target));
        for (const PortableShaderBinding& binding : request.ExpectedLayout)
        {
            Append(canonical, binding.Name);
            AppendNumber(canonical, binding.Kind);
            AppendNumber(canonical, binding.Register);
            AppendNumber(canonical, binding.Space);
            AppendNumber(canonical, static_cast<int>(binding.Stages));
            Append(canonical, binding.ResourceKind);
            Append(canonical, binding.TypeShape);
            AppendNumber(canonical, binding.Count);
            AppendNumber(canonical, binding.ByteSize);
            AppendNumber(canonical, binding.Rows);
            AppendNumber(canonical, binding.Columns);
        }
        for (const PortableShaderVertexInput& input : request.ExpectedVertexInputs)
        {
            Append(canonical, input.Name);
            Append(canonical, input.Semantic);
            AppendNumber(canonical, input.SemanticIndex);
            AppendNumber(canonical, input.Location);
            Append(canonical, input.TypeShape);
            AppendNumber(canonical, input.ByteSize);
            AppendNumber(canonical, input.Rows);
            AppendNumber(canonical, input.Columns);
        }
        return Sha256(canonical);
    }

    bool PortableShaderContract::Validate(
        const PortableShaderRequest& request,
        const std::vector<PortableShaderBinding>& bindings,
        const std::vector<PortableShaderVertexInput>& vertexInputs,
        std::string& error)
    {
        if (request.Stage == RHI::ShaderStage::None || request.EntryPoint.empty() || request.Source.empty())
        {
            error = "source, entry point, and stage are required";
            return false;
        }
        if (request.Conventions.Version != 2
            || request.Conventions.Coordinates != "RightHanded"
            || request.Conventions.BindingPolicy != "D3DRegisterSpaceVulkanClassOffsets0_100_200_300"
            || !request.Conventions.RowMajor
            || !request.Conventions.ZeroToOneDepth
            || !request.Conventions.VulkanYFlip
            || !request.Conventions.ClockwiseFrontFace)
        {
            error = "unsupported portable convention schema";
            return false;
        }
        if (bindings.size() != request.ExpectedLayout.size())
        {
            error = "reflected binding count differs";
            return false;
        }
        for (size_t index = 0; index < bindings.size(); ++index)
        {
            if (!SameBinding(bindings[index], request.ExpectedLayout[index]))
            {
                error = "reflected binding mismatch at " + std::to_string(index)
                    + ": actual [" + BindingSummary(bindings[index]) + "] expected ["
                    + BindingSummary(request.ExpectedLayout[index]) + ']';
                return false;
            }
        }
        if (vertexInputs.size() != request.ExpectedVertexInputs.size())
        {
            error = "reflected vertex-input count differs";
            return false;
        }
        for (size_t index = 0; index < vertexInputs.size(); ++index)
        {
            if (!SameVertexInput(vertexInputs[index], request.ExpectedVertexInputs[index]))
            {
                error = "reflected vertex-input mismatch at " + std::to_string(index)
                    + ": actual [" + VertexInputSummary(vertexInputs[index]) + "] expected ["
                    + VertexInputSummary(request.ExpectedVertexInputs[index]) + ']';
                return false;
            }
        }
        return true;
    }

    bool PortableShaderContract::ValidatePackage(
        const PortableShaderRequest& request,
        const PortableShaderPackage& package,
        std::string& error)
    {
        if (!package.Diagnostics.empty())
        {
            error = "shader package contains diagnostics";
            return false;
        }
        if (package.Key != CacheKey(request))
        {
            error = "shader package key differs from the request cache key";
            return false;
        }
        const auto requiresTarget = [&request](PortableShaderTarget target)
        {
            return std::find(request.Targets.begin(), request.Targets.end(), target) != request.Targets.end();
        };
        if (requiresTarget(PortableShaderTarget::Dxil) && package.Dxil.empty())
        {
            error = "shader package is missing requested DXIL artifact";
            return false;
        }
        if (requiresTarget(PortableShaderTarget::Spirv) && package.Spirv.empty())
        {
            error = "shader package is missing requested SPIR-V artifact";
            return false;
        }
        if (!requiresTarget(PortableShaderTarget::Dxil) && !requiresTarget(PortableShaderTarget::Spirv))
        {
            error = "shader package request has no targets";
            return false;
        }
        if (package.Conventions != request.Conventions)
        {
            error = "shader package conventions differ from the request";
            return false;
        }
        return Validate(request, package.Reflection, package.VertexInputs, error);
    }

    bool PortableShaderContract::StoreAtomic(const std::filesystem::path& path, const PortableShaderPackage& package)
    {
        if (!IsValidCachedPackage(package))
            return false;
        std::error_code error;
        if (!path.parent_path().empty())
            std::filesystem::create_directories(path.parent_path(), error);
        if (error)
            return false;

        static std::atomic<u64> temporarySequence = 1;
        std::filesystem::path temporary = path;
        temporary += ".tmp." + std::to_string(temporarySequence.fetch_add(1, std::memory_order_relaxed));
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        if (!stream)
            return false;
        constexpr char magic[] = "SPKG2";
        stream.write(magic, 5);
        stream.write(reinterpret_cast<const char*>(&package.Version), sizeof(package.Version));
        WriteString(stream, package.Key);
        WriteBytes(stream, package.Dxil);
        WriteBytes(stream, package.Spirv);
        const u64 bindingCount = package.Reflection.size();
        stream.write(reinterpret_cast<const char*>(&bindingCount), sizeof(bindingCount));
        for (const PortableShaderBinding& binding : package.Reflection)
            WriteBinding(stream, binding);
        const u64 vertexInputCount = package.VertexInputs.size();
        stream.write(reinterpret_cast<const char*>(&vertexInputCount), sizeof(vertexInputCount));
        for (const PortableShaderVertexInput& input : package.VertexInputs)
            WriteVertexInput(stream, input);
        stream.write(reinterpret_cast<const char*>(&package.Conventions.Version), sizeof(package.Conventions.Version));
        const u8 conventionFlags = static_cast<u8>(package.Conventions.RowMajor)
            | (static_cast<u8>(package.Conventions.ZeroToOneDepth) << 1)
            | (static_cast<u8>(package.Conventions.VulkanYFlip) << 2)
            | (static_cast<u8>(package.Conventions.ClockwiseFrontFace) << 3);
        stream.write(reinterpret_cast<const char*>(&conventionFlags), sizeof(conventionFlags));
        WriteString(stream, package.Conventions.Coordinates);
        WriteString(stream, package.Conventions.BindingPolicy);
        stream.close();
        if (!stream || !ReplacePublishedFile(temporary, path))
        {
            std::filesystem::remove(temporary, error);
            return false;
        }
        return true;
    }

    bool PortableShaderContract::Load(
        const std::filesystem::path& path,
        const PortableShaderRequest& request,
        PortableShaderPackage& package)
    {
        std::ifstream stream(path, std::ios::binary);
        PortableShaderPackage candidate;
        char magic[5] {};
        if (!stream.read(magic, sizeof(magic))
            || std::string_view(magic, sizeof(magic)) != "SPKG2"
            || !stream.read(reinterpret_cast<char*>(&candidate.Version), sizeof(candidate.Version))
            || candidate.Version != 2
            || !ReadString(stream, candidate.Key)
            || candidate.Key != CacheKey(request)
            || !ReadBytes(stream, candidate.Dxil)
            || !ReadBytes(stream, candidate.Spirv))
        {
            return false;
        }

        u64 bindingCount = 0;
        if (!stream.read(reinterpret_cast<char*>(&bindingCount), sizeof(bindingCount))
            || bindingCount > kMaximumReflectionItems)
        {
            return false;
        }
        candidate.Reflection.resize(static_cast<size_t>(bindingCount));
        for (PortableShaderBinding& binding : candidate.Reflection)
        {
            if (!ReadBinding(stream, binding))
                return false;
        }

        u64 vertexInputCount = 0;
        if (!stream.read(reinterpret_cast<char*>(&vertexInputCount), sizeof(vertexInputCount))
            || vertexInputCount > kMaximumReflectionItems)
        {
            return false;
        }
        candidate.VertexInputs.resize(static_cast<size_t>(vertexInputCount));
        for (PortableShaderVertexInput& input : candidate.VertexInputs)
        {
            if (!ReadVertexInput(stream, input))
                return false;
        }
        u8 conventionFlags = 0;
        if (!stream.read(reinterpret_cast<char*>(&candidate.Conventions.Version), sizeof(candidate.Conventions.Version))
            || !stream.read(reinterpret_cast<char*>(&conventionFlags), sizeof(conventionFlags))
            || !ReadString(stream, candidate.Conventions.Coordinates)
            || !ReadString(stream, candidate.Conventions.BindingPolicy))
        {
            return false;
        }
        candidate.Conventions.RowMajor = (conventionFlags & 1) != 0;
        candidate.Conventions.ZeroToOneDepth = (conventionFlags & 2) != 0;
        candidate.Conventions.VulkanYFlip = (conventionFlags & 4) != 0;
        candidate.Conventions.ClockwiseFrontFace = (conventionFlags & 8) != 0;
        std::string validationError;
        if (stream.peek() != std::char_traits<char>::eof() || !IsValidCachedPackage(candidate)
            || !ValidatePackage(request, candidate, validationError))
            return false;

        package = std::move(candidate);
        return true;
    }
}
