#include "Engine/Renderer/SlangShaderCompiler.h"

#include <slang-com-ptr.h>
#include <slang.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <limits>
#include <mutex>
#include <set>
#include <sstream>
#include <string_view>
#include <unordered_map>

namespace Engine
{
    namespace
    {
        struct ReflectedInterface
        {
            std::vector<PortableShaderBinding> Bindings;
            std::vector<PortableShaderVertexInput> VertexInputs;
        };

        std::mutex& GetSlangMutex()
        {
            static std::mutex mutex;
            return mutex;
        }

        Slang::ComPtr<slang::IGlobalSession>& GetGlobalSession()
        {
            static Slang::ComPtr<slang::IGlobalSession> session;
            return session;
        }

        std::string BlobText(slang::IBlob* blob)
        {
            if (!blob || !blob->getBufferPointer() || blob->getBufferSize() == 0)
                return {};
            return { static_cast<const char*>(blob->getBufferPointer()), blob->getBufferSize() };
        }

        std::string TargetName(PortableShaderTarget target)
        {
            return target == PortableShaderTarget::Dxil ? "DXIL" : "SPIR-V";
        }

        void AddDiagnostic(PortableShaderPackage& package, const PortableShaderRequest& request,
            std::string target, std::string message)
        {
            if (message.empty())
                message = "Slang operation failed without diagnostic text";
            package.Diagnostics.push_back({
                request.SourceName,
                request.EntryPoint,
                std::move(target),
                "Slang",
                std::move(message)
            });
        }

        SlangStage ToSlangStage(RHI::ShaderStage stage)
        {
            switch (stage)
            {
            case RHI::ShaderStage::Vertex: return SLANG_STAGE_VERTEX;
            case RHI::ShaderStage::Pixel: return SLANG_STAGE_FRAGMENT;
            case RHI::ShaderStage::Compute: return SLANG_STAGE_COMPUTE;
            default: return SLANG_STAGE_NONE;
            }
        }

        std::string NormalizeDependencyPath(std::string_view path)
        {
            std::filesystem::path normalized = std::filesystem::path(path).lexically_normal();
            std::string result = normalized.generic_string();
            while (result.starts_with("./"))
                result.erase(0, 2);
            return result;
        }

        std::string Trim(std::string_view value)
        {
            size_t begin = 0;
            size_t end = value.size();
            while (begin < end && std::isspace(static_cast<unsigned char>(value[begin]))) ++begin;
            while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1]))) --end;
            return std::string(value.substr(begin, end - begin));
        }

        bool ParseInclude(std::string_view line, std::string& path, bool& isDirective)
        {
            isDirective = false;
            const std::string trimmed = Trim(line);
            size_t cursor = 0;
            if (cursor >= trimmed.size() || trimmed[cursor] != '#')
                return true;
            ++cursor;
            while (cursor < trimmed.size() && std::isspace(static_cast<unsigned char>(trimmed[cursor]))) ++cursor;
            constexpr std::string_view includeWord = "include";
            if (trimmed.compare(cursor, includeWord.size(), includeWord) != 0)
                return true;
            cursor += includeWord.size();
            isDirective = true;
            while (cursor < trimmed.size() && std::isspace(static_cast<unsigned char>(trimmed[cursor]))) ++cursor;
            if (cursor >= trimmed.size() || trimmed[cursor] != '"')
                return false;
            const size_t close = trimmed.find('"', cursor + 1);
            if (close == std::string::npos || !Trim(std::string_view(trimmed).substr(close + 1)).empty())
                return false;
            path = trimmed.substr(cursor + 1, close - cursor - 1);
            return !path.empty();
        }

        bool ExpandControlledSource(
            std::string_view source,
            std::string_view sourcePath,
            const std::unordered_map<std::string, const PortableShaderDependency*>& dependencies,
            std::set<std::string>& resolved,
            std::set<std::string>& expansionStack,
            std::string& output,
            std::string& error)
        {
            std::istringstream stream { std::string(source) };
            std::string line;
            while (std::getline(stream, line))
            {
                std::string includePath;
                bool includeDirective = false;
                if (!ParseInclude(line, includePath, includeDirective))
                {
                    error = "only quoted, standalone #include directives are supported";
                    return false;
                }
                if (!includeDirective)
                {
                    const std::string trimmed = Trim(line);
                    if (trimmed.starts_with("import ") || trimmed.starts_with("__include"))
                    {
                        error = "module imports and nonstandard include mechanisms are unsupported by controlled shader dependency resolution";
                        return false;
                    }
                    output += line;
                    output += '\n';
                    continue;
                }

                const std::string direct = NormalizeDependencyPath(includePath);
                const std::string relative = NormalizeDependencyPath(
                    (std::filesystem::path(sourcePath).parent_path() / includePath).generic_string());
                auto dependency = dependencies.find(direct);
                if (dependency == dependencies.end())
                    dependency = dependencies.find(relative);
                if (dependency == dependencies.end())
                {
                    error = "include '" + includePath + "' is absent from the declared dependency closure";
                    return false;
                }
                const std::string normalizedDependencyPath = NormalizeDependencyPath(dependency->second->Path);
                if (!expansionStack.insert(normalizedDependencyPath).second)
                {
                    error = "cyclic shader include dependency: " + normalizedDependencyPath;
                    return false;
                }
                resolved.insert(normalizedDependencyPath);
                output += "// begin controlled include: " + normalizedDependencyPath + "\n";
                if (!ExpandControlledSource(
                        dependency->second->Content,
                        normalizedDependencyPath,
                        dependencies,
                        resolved,
                        expansionStack,
                        output,
                        error))
                {
                    return false;
                }
                output += "// end controlled include: " + normalizedDependencyPath + "\n";
                expansionStack.erase(normalizedDependencyPath);
            }
            return true;
        }

        bool PrepareControlledSource(const PortableShaderRequest& request, std::string& source, std::string& error)
        {
            std::unordered_map<std::string, const PortableShaderDependency*> dependencies;
            for (const PortableShaderDependency& dependency : request.Dependencies)
            {
                const std::string path = NormalizeDependencyPath(dependency.Path);
                if (path.empty() || path == "." || std::filesystem::path(path).is_absolute()
                    || path == ".." || path.starts_with("../"))
                {
                    error = "shader dependency paths must be normalized workspace-relative paths";
                    return false;
                }
                if (dependency.ContentHash != PortableShaderContract::Sha256(dependency.Content))
                {
                    error = "declared content hash does not match dependency contents: " + path;
                    return false;
                }
                if (!dependencies.emplace(path, &dependency).second)
                {
                    error = "duplicate shader dependency path: " + path;
                    return false;
                }
            }

            std::set<std::string> resolved;
            std::set<std::string> expansionStack;
            if (!ExpandControlledSource(
                    request.Source,
                    NormalizeDependencyPath(request.SourceName),
                    dependencies,
                    resolved,
                    expansionStack,
                    source,
                    error))
            {
                return false;
            }
            if (resolved.size() != dependencies.size())
            {
                for (const auto& [path, dependency] : dependencies)
                {
                    if (!resolved.contains(path))
                    {
                        error = "declared shader dependency was not part of the resolved closure: " + path;
                        return false;
                    }
                }
            }
            return true;
        }

        bool IsIdentifier(std::string_view value)
        {
            if (value.empty() || !(std::isalpha(static_cast<unsigned char>(value.front())) || value.front() == '_'))
                return false;
            return std::all_of(value.begin() + 1, value.end(), [](char character)
            {
                return std::isalnum(static_cast<unsigned char>(character)) || character == '_';
            });
        }

        bool PrepareDefines(
            const PortableShaderRequest& request,
            std::vector<std::string>& names,
            std::vector<std::string>& values,
            std::vector<slang::PreprocessorMacroDesc>& macros,
            std::string& error)
        {
            std::vector<std::string> defines = request.Defines;
            std::sort(defines.begin(), defines.end());
            names.reserve(defines.size());
            values.reserve(defines.size());
            for (const std::string& define : defines)
            {
                const size_t separator = define.find('=');
                const std::string name = Trim(std::string_view(define).substr(0, separator));
                const std::string value = separator == std::string::npos
                    ? "1"
                    : Trim(std::string_view(define).substr(separator + 1));
                if (!IsIdentifier(name) || value.empty())
                {
                    error = "unsupported shader define: " + define;
                    return false;
                }
                if (!names.empty() && names.back() == name)
                {
                    error = "duplicate shader define: " + name;
                    return false;
                }
                names.push_back(name);
                values.push_back(value);
            }
            macros.reserve(names.size());
            for (size_t index = 0; index < names.size(); ++index)
                macros.push_back({ names[index].c_str(), values[index].c_str() });
            return true;
        }

        slang::CompilerOptionEntry IntegerOption(slang::CompilerOptionName name, int value)
        {
            slang::CompilerOptionEntry entry {};
            entry.name = name;
            entry.value.kind = slang::CompilerOptionValueKind::Int;
            entry.value.intValue0 = value;
            return entry;
        }

        slang::CompilerOptionEntry StringOption(slang::CompilerOptionName name, const char* value)
        {
            slang::CompilerOptionEntry entry {};
            entry.name = name;
            entry.value.kind = slang::CompilerOptionValueKind::String;
            entry.value.stringValue0 = value;
            return entry;
        }

        bool PrepareOptions(
            const PortableShaderRequest& request,
            std::vector<slang::CompilerOptionEntry>& options,
            std::string& error)
        {
            options.push_back(IntegerOption(slang::CompilerOptionName::MatrixLayoutRow, 1));
            std::set<std::string> unique;
            bool optimizationSpecified = false;
            for (const std::string& option : request.Options)
            {
                if (!unique.insert(option).second)
                {
                    error = "duplicate shader compiler option: " + option;
                    return false;
                }
                if (option == "-O0" || option == "-O1" || option == "-O2" || option == "-O3")
                {
                    if (optimizationSpecified)
                    {
                        error = "multiple optimization levels are unsupported";
                        return false;
                    }
                    optimizationSpecified = true;
                    const int level = option == "-O0" ? SLANG_OPTIMIZATION_LEVEL_NONE
                        : option == "-O1" ? SLANG_OPTIMIZATION_LEVEL_DEFAULT
                        : option == "-O2" ? SLANG_OPTIMIZATION_LEVEL_HIGH
                        : SLANG_OPTIMIZATION_LEVEL_MAXIMAL;
                    options.push_back(IntegerOption(slang::CompilerOptionName::Optimization, level));
                }
                else if (option == "-g")
                {
                    options.push_back(IntegerOption(
                        slang::CompilerOptionName::DebugInformation,
                        SLANG_DEBUG_INFO_LEVEL_STANDARD));
                }
                else if (option == "-Werror")
                {
                    options.push_back(StringOption(slang::CompilerOptionName::WarningsAsErrors, "all"));
                }
                else
                {
                    error = "unsupported shader compiler option: " + option;
                    return false;
                }
            }
            return true;
        }

        char BindingKind(slang::VariableLayoutReflection* parameter)
        {
            const slang::ParameterCategory category = parameter->getCategory();
            switch (category)
            {
            case slang::ParameterCategory::ConstantBuffer: return 'b';
            case slang::ParameterCategory::ShaderResource: return 't';
            case slang::ParameterCategory::UnorderedAccess: return 'u';
            case slang::ParameterCategory::SamplerState: return 's';
            default: break;
            }

            slang::TypeLayoutReflection* typeLayout = parameter->getTypeLayout();
            if (!typeLayout || typeLayout->getBindingRangeCount() == 0)
                return '?';
            switch (typeLayout->getBindingRangeType(0))
            {
            case slang::BindingType::ConstantBuffer:
            case slang::BindingType::ParameterBlock: return 'b';
            case slang::BindingType::Sampler: return 's';
            case slang::BindingType::Texture:
            case slang::BindingType::TypedBuffer:
            case slang::BindingType::RawBuffer:
            case slang::BindingType::CombinedTextureSampler:
            case slang::BindingType::InputRenderTarget:
            case slang::BindingType::RayTracingAccelerationStructure: return 't';
            case slang::BindingType::MutableTexture:
            case slang::BindingType::MutableTypedBuffer:
            case slang::BindingType::MutableRawBuffer: return 'u';
            default: return '?';
            }
        }

        const char* ScalarTypeName(slang::TypeReflection::ScalarType scalar)
        {
            switch (scalar)
            {
            case slang::TypeReflection::Bool: return "bool";
            case slang::TypeReflection::Int8: return "int8";
            case slang::TypeReflection::UInt8: return "uint8";
            case slang::TypeReflection::Int16: return "int16";
            case slang::TypeReflection::UInt16: return "uint16";
            case slang::TypeReflection::Int32: return "int32";
            case slang::TypeReflection::UInt32: return "uint32";
            case slang::TypeReflection::Int64: return "int64";
            case slang::TypeReflection::UInt64: return "uint64";
            case slang::TypeReflection::Float16: return "float16";
            case slang::TypeReflection::Float32: return "float32";
            case slang::TypeReflection::Float64: return "float64";
            default: return "unknown";
            }
        }

        u32 ScalarByteSize(slang::TypeReflection::ScalarType scalar)
        {
            switch (scalar)
            {
            case slang::TypeReflection::Int8:
            case slang::TypeReflection::UInt8: return 1;
            case slang::TypeReflection::Int16:
            case slang::TypeReflection::UInt16:
            case slang::TypeReflection::Float16: return 2;
            case slang::TypeReflection::Bool:
            case slang::TypeReflection::Int32:
            case slang::TypeReflection::UInt32:
            case slang::TypeReflection::Float32: return 4;
            case slang::TypeReflection::Int64:
            case slang::TypeReflection::UInt64:
            case slang::TypeReflection::Float64: return 8;
            default: return 0;
            }
        }

        std::string NumericShape(slang::TypeReflection* type)
        {
            if (!type)
                return "unknown";
            const char* scalar = ScalarTypeName(type->getScalarType());
            switch (type->getKind())
            {
            case slang::TypeReflection::Kind::Scalar:
                return scalar;
            case slang::TypeReflection::Kind::Vector:
                return std::string(scalar) + "x" + std::to_string(type->getElementCount());
            case slang::TypeReflection::Kind::Matrix:
                return std::string(scalar) + "x" + std::to_string(type->getRowCount()) + "x" + std::to_string(type->getColumnCount());
            default:
                return type->getName() && *type->getName() ? type->getName() : "opaque";
            }
        }

        std::string LayoutShape(slang::TypeLayoutReflection* typeLayout)
        {
            if (!typeLayout || !typeLayout->getType())
                return "unknown";
            slang::TypeReflection* type = typeLayout->getType();
            if (type->getKind() == slang::TypeReflection::Kind::Struct)
            {
                std::string result = "struct{";
                for (unsigned index = 0; index < typeLayout->getFieldCount(); ++index)
                {
                    slang::VariableLayoutReflection* field = typeLayout->getFieldByIndex(index);
                    if (index != 0) result += ',';
                    result += field && field->getName() ? field->getName() : "?";
                    result += ':';
                    result += LayoutShape(field ? field->getTypeLayout() : nullptr);
                    result += '@';
                    result += std::to_string(field ? field->getOffset(slang::ParameterCategory::Uniform) : SLANG_UNKNOWN_SIZE);
                }
                result += '}';
                return result;
            }
            if (type->getKind() == slang::TypeReflection::Kind::Array)
            {
                return "array[" + std::to_string(type->getTotalArrayElementCount()) + "]<"
                    + LayoutShape(typeLayout->getElementTypeLayout()) + '>';
            }
            std::string result = NumericShape(type);
            if (type->getKind() == slang::TypeReflection::Kind::Matrix)
            {
                result += typeLayout->getMatrixLayoutMode() == SLANG_MATRIX_LAYOUT_ROW_MAJOR
                    ? ":row-major"
                    : ":column-major";
            }
            return result;
        }

        std::string ResourceShapeName(slang::TypeReflection* type, char kind)
        {
            if (kind == 'b') return "ConstantBuffer";
            if (kind == 's') return "SamplerState";
            if (!type) return "UnknownResource";
            const SlangResourceShape shape = type->getResourceShape();
            const unsigned base = shape & SLANG_RESOURCE_BASE_SHAPE_MASK;
            std::string name;
            switch (base)
            {
            case SLANG_TEXTURE_1D: name = "Texture1D"; break;
            case SLANG_TEXTURE_2D: name = "Texture2D"; break;
            case SLANG_TEXTURE_3D: name = "Texture3D"; break;
            case SLANG_TEXTURE_CUBE: name = "TextureCube"; break;
            case SLANG_TEXTURE_BUFFER: name = "TextureBuffer"; break;
            case SLANG_STRUCTURED_BUFFER: name = "StructuredBuffer"; break;
            case SLANG_BYTE_ADDRESS_BUFFER: name = "ByteAddressBuffer"; break;
            case SLANG_ACCELERATION_STRUCTURE: name = "AccelerationStructure"; break;
            case SLANG_TEXTURE_SUBPASS: name = "SubpassInput"; break;
            default: name = "Resource"; break;
            }
            if ((shape & SLANG_TEXTURE_ARRAY_FLAG) != 0) name += "Array";
            if ((shape & SLANG_TEXTURE_MULTISAMPLE_FLAG) != 0) name += "MS";
            if (kind == 'u') name = "RW" + name;
            return name;
        }

        bool CheckedU32(size_t value, u32& output)
        {
            if (value == SLANG_UNKNOWN_SIZE || value == SLANG_UNBOUNDED_SIZE
                || value > std::numeric_limits<u32>::max())
            {
                return false;
            }
            output = static_cast<u32>(value);
            return true;
        }

        bool BuildBinding(
            slang::VariableLayoutReflection* parameter,
            const PortableShaderRequest& request,
            PortableShaderBinding& binding,
            std::string& error)
        {
            const char* name = parameter->getName();
            if (!name || !*name)
            {
                error = "reflected binding has no name";
                return false;
            }
            binding.Name = name;
            binding.Kind = BindingKind(parameter);
            if (binding.Kind == '?')
                return false;
            const unsigned registerIndex = parameter->getBindingIndex();
            const unsigned space = parameter->getBindingSpace();
            if (registerIndex == SLANG_UNKNOWN_SIZE || space == SLANG_UNKNOWN_SIZE)
            {
                error = "reflected binding is unresolved";
                return false;
            }
            binding.Register = registerIndex;
            binding.Space = space;
            binding.Stages = request.Stage;

            slang::TypeLayoutReflection* typeLayout = parameter->getTypeLayout();
            slang::TypeReflection* declaredType = parameter->getType();
            if (!typeLayout || !declaredType)
            {
                error = "reflected binding has no type layout";
                return false;
            }
            binding.Count = 1;
            if (declaredType->isArray())
            {
                if (!CheckedU32(declaredType->getTotalArrayElementCount(), binding.Count) || binding.Count == 0)
                {
                    error = "reflected binding array count is unresolved";
                    return false;
                }
                declaredType = declaredType->unwrapArray();
                typeLayout = typeLayout->unwrapArray();
            }

            binding.ResourceKind = ResourceShapeName(declaredType, binding.Kind);
            slang::TypeReflection* valueType = declaredType->getResourceResultType();
            if (binding.Kind == 'b')
            {
                slang::TypeLayoutReflection* elementLayout = typeLayout->getElementTypeLayout();
                binding.TypeShape = LayoutShape(elementLayout ? elementLayout : typeLayout);
                size_t byteSize = typeLayout->getSize(slang::ParameterCategory::Uniform);
                if (byteSize == 0 && elementLayout)
                    byteSize = elementLayout->getSize(slang::ParameterCategory::Uniform);
                if (!CheckedU32(byteSize, binding.ByteSize))
                {
                    error = "constant-buffer byte size is unresolved";
                    return false;
                }
            }
            else if (binding.Kind == 's')
            {
                binding.TypeShape = "sampler";
            }
            else
            {
                binding.TypeShape = NumericShape(valueType);
                if (valueType)
                {
                    binding.Rows = std::max(1u, valueType->getRowCount());
                    binding.Columns = std::max(1u, valueType->getColumnCount());
                }
            }
            return true;
        }

        bool CollectVertexInput(
            slang::VariableLayoutReflection* parameter,
            std::vector<PortableShaderVertexInput>& output,
            std::string& error)
        {
            if (!parameter || !parameter->getTypeLayout())
            {
                error = "vertex input reflection is incomplete";
                return false;
            }
            const char* semantic = parameter->getSemanticName();
            if (semantic && *semantic)
            {
                PortableShaderVertexInput input;
                const char* name = parameter->getName();
                input.Name = name ? name : "";
                input.Semantic = semantic;
                input.SemanticIndex = static_cast<u32>(parameter->getSemanticIndex());
                input.Location = static_cast<u32>(output.size());
                slang::TypeLayoutReflection* typeLayout = parameter->getTypeLayout();
                slang::TypeReflection* type = typeLayout->getType();
                input.TypeShape = NumericShape(type);
                input.Rows = std::max(1u, type->getRowCount());
                input.Columns = std::max(1u, type->getColumnCount());
                input.ByteSize = ScalarByteSize(type->getScalarType()) * input.Rows * input.Columns;
                if (input.ByteSize == 0)
                {
                    error = "vertex input byte size is unresolved";
                    return false;
                }
                output.push_back(std::move(input));
                return true;
            }

            slang::TypeLayoutReflection* typeLayout = parameter->getTypeLayout();
            if (typeLayout->getFieldCount() == 0)
            {
                error = "vertex input has neither a semantic nor semantic-bearing fields";
                return false;
            }
            for (unsigned fieldIndex = 0; fieldIndex < typeLayout->getFieldCount(); ++fieldIndex)
            {
                if (!CollectVertexInput(typeLayout->getFieldByIndex(fieldIndex), output, error))
                    return false;
            }
            return true;
        }

        bool CollectReflection(
            slang::ProgramLayout* layout,
            const PortableShaderRequest& request,
            ReflectedInterface& output,
            std::string& error)
        {
            if (!layout)
            {
                error = "linked program did not provide reflection";
                return false;
            }
            for (unsigned index = 0; index < layout->getParameterCount(); ++index)
            {
                slang::VariableLayoutReflection* parameter = layout->getParameterByIndex(index);
                if (!parameter)
                {
                    error = "linked program returned a null reflected parameter";
                    return false;
                }
                PortableShaderBinding binding;
                if (BindingKind(parameter) == '?')
                    continue;
                if (!BuildBinding(parameter, request, binding, error))
                    return false;
                output.Bindings.push_back(std::move(binding));
            }
            std::sort(output.Bindings.begin(), output.Bindings.end(), [](const auto& left, const auto& right)
            {
                if (left.Space != right.Space) return left.Space < right.Space;
                if (left.Register != right.Register) return left.Register < right.Register;
                if (left.Kind != right.Kind) return left.Kind < right.Kind;
                return left.Name < right.Name;
            });

            if (layout->getEntryPointCount() != 1)
            {
                error = "linked reflection did not contain exactly one entry point";
                return false;
            }
            slang::EntryPointReflection* entryPoint = layout->getEntryPointByIndex(0);
            if (!entryPoint || !entryPoint->getName() || request.EntryPoint != entryPoint->getName()
                || entryPoint->getStage() != ToSlangStage(request.Stage))
            {
                error = "linked entry-point reflection does not match the request";
                return false;
            }
            if (request.Stage == RHI::ShaderStage::Vertex)
            {
                for (unsigned index = 0; index < entryPoint->getParameterCount(); ++index)
                {
                    if (!CollectVertexInput(entryPoint->getParameterByIndex(index), output.VertexInputs, error))
                        return false;
                }
            }
            return true;
        }

        bool SameBinding(const PortableShaderBinding& left, const PortableShaderBinding& right)
        {
            return left.Name == right.Name && left.Kind == right.Kind
                && left.Register == right.Register && left.Space == right.Space
                && left.Stages == right.Stages && left.ResourceKind == right.ResourceKind
                && left.TypeShape == right.TypeShape && left.Count == right.Count
                && left.ByteSize == right.ByteSize && left.Rows == right.Rows
                && left.Columns == right.Columns;
        }

        bool SameVertexInput(const PortableShaderVertexInput& left, const PortableShaderVertexInput& right)
        {
            return left.Name == right.Name && left.Semantic == right.Semantic
                && left.SemanticIndex == right.SemanticIndex && left.Location == right.Location
                && left.TypeShape == right.TypeShape && left.ByteSize == right.ByteSize
                && left.Rows == right.Rows && left.Columns == right.Columns;
        }

        bool SameReflection(const ReflectedInterface& left, const ReflectedInterface& right)
        {
            return left.Bindings.size() == right.Bindings.size()
                && left.VertexInputs.size() == right.VertexInputs.size()
                && std::equal(left.Bindings.begin(), left.Bindings.end(), right.Bindings.begin(), SameBinding)
                && std::equal(left.VertexInputs.begin(), left.VertexInputs.end(), right.VertexInputs.begin(), SameVertexInput);
        }
    }

    SlangShaderCompiler::SlangShaderCompiler(std::filesystem::path cacheDirectory)
        : m_CacheDirectory(std::move(cacheDirectory))
    {
    }

    PortableShaderPackage SlangShaderCompiler::Compile(const PortableShaderRequest& request) const
    {
        PortableShaderPackage package;
        package.Key = PortableShaderContract::CacheKey(request);
        const std::filesystem::path cachePath = m_CacheDirectory / (package.Key + ".shaderpkg");

        std::string validationError;
        if (!PortableShaderContract::Validate(
                request,
                request.ExpectedLayout,
                request.ExpectedVertexInputs,
                validationError))
        {
            AddDiagnostic(package, request, "", validationError);
            return package;
        }
        if (std::find(request.Targets.begin(), request.Targets.end(), PortableShaderTarget::Dxil) == request.Targets.end()
            || std::find(request.Targets.begin(), request.Targets.end(), PortableShaderTarget::Spirv) == request.Targets.end())
        {
            AddDiagnostic(package, request, "", "both DXIL and SPIR-V targets are required");
            return package;
        }

        std::string controlledSource;
        if (!PrepareControlledSource(request, controlledSource, validationError))
        {
            AddDiagnostic(package, request, "", validationError);
            return package;
        }
        std::vector<std::string> macroNames;
        std::vector<std::string> macroValues;
        std::vector<slang::PreprocessorMacroDesc> macros;
        if (!PrepareDefines(request, macroNames, macroValues, macros, validationError))
        {
            AddDiagnostic(package, request, "", validationError);
            return package;
        }
        std::vector<slang::CompilerOptionEntry> sessionOptions;
        if (!PrepareOptions(request, sessionOptions, validationError))
        {
            AddDiagnostic(package, request, "", validationError);
            return package;
        }
        if (!m_CacheDirectory.empty() && PortableShaderContract::Load(cachePath, package.Key, package))
        {
            package.CacheSource = PortableShaderCacheSource::DiskCache;
            return package;
        }

        std::scoped_lock lock(GetSlangMutex());
        Slang::ComPtr<slang::IGlobalSession>& global = GetGlobalSession();
        if (!global && SLANG_FAILED(slang::createGlobalSession(global.writeRef())))
        {
            AddDiagnostic(package, request, "", "failed to create the Slang global session");
            return package;
        }
        slang::TargetDesc targets[2];
        targets[0].format = SLANG_DXIL;
        targets[0].profile = global->findProfile("sm_6_0");
        targets[1].format = SLANG_SPIRV;
        targets[1].profile = global->findProfile("sm_6_0");
        std::array<slang::CompilerOptionEntry, 2> spirvConventionOptions = {
            IntegerOption(slang::CompilerOptionName::VulkanInvertY, 1),
            IntegerOption(slang::CompilerOptionName::VulkanUseDxPositionW, 1)
        };
        targets[1].compilerOptionEntries = spirvConventionOptions.data();
        targets[1].compilerOptionEntryCount = static_cast<u32>(spirvConventionOptions.size());
        if (targets[0].profile == SLANG_PROFILE_UNKNOWN || targets[1].profile == SLANG_PROFILE_UNKNOWN)
        {
            AddDiagnostic(package, request, "", "Slang could not resolve the required sm_6_0 profile");
            return package;
        }

        slang::SessionDesc sessionDesc;
        sessionDesc.targets = targets;
        sessionDesc.targetCount = 2;
        sessionDesc.defaultMatrixLayoutMode = SLANG_MATRIX_LAYOUT_ROW_MAJOR;
        sessionDesc.preprocessorMacros = macros.data();
        sessionDesc.preprocessorMacroCount = static_cast<SlangInt>(macros.size());
        sessionDesc.compilerOptionEntries = sessionOptions.data();
        sessionDesc.compilerOptionEntryCount = static_cast<u32>(sessionOptions.size());
        Slang::ComPtr<slang::ISession> session;
        if (SLANG_FAILED(global->createSession(sessionDesc, session.writeRef())))
        {
            AddDiagnostic(package, request, "", "failed to create the Slang compilation session");
            return package;
        }

        Slang::ComPtr<slang::IBlob> source(
            Slang::INIT_ATTACH,
            slang_createBlob(controlledSource.data(), controlledSource.size()));
        Slang::ComPtr<slang::IBlob> diagnostics;
        Slang::ComPtr<slang::IModule> module;
        module = session->loadModuleFromSource(
            "EnginePortableShader",
            request.SourceName.c_str(),
            source,
            diagnostics.writeRef());
        if (!module)
        {
            AddDiagnostic(package, request, "", BlobText(diagnostics));
            return package;
        }

        Slang::ComPtr<slang::IEntryPoint> entryPoint;
        diagnostics.setNull();
        if (SLANG_FAILED(module->findAndCheckEntryPoint(
                request.EntryPoint.c_str(),
                ToSlangStage(request.Stage),
                entryPoint.writeRef(),
                diagnostics.writeRef())) || !entryPoint)
        {
            AddDiagnostic(package, request, "", BlobText(diagnostics));
            return package;
        }

        slang::IComponentType* components[] = { module, entryPoint };
        Slang::ComPtr<slang::IComponentType> composite;
        diagnostics.setNull();
        if (SLANG_FAILED(session->createCompositeComponentType(
                components,
                2,
                composite.writeRef(),
                diagnostics.writeRef())) || !composite)
        {
            AddDiagnostic(package, request, "", BlobText(diagnostics));
            return package;
        }

        Slang::ComPtr<slang::IComponentType> linked;
        diagnostics.setNull();
        if (SLANG_FAILED(composite->link(linked.writeRef(), diagnostics.writeRef())) || !linked)
        {
            AddDiagnostic(package, request, "", BlobText(diagnostics));
            return package;
        }
        ReflectedInterface targetReflection[2];
        for (SlangInt targetIndex = 0; targetIndex < 2; ++targetIndex)
        {
            const PortableShaderTarget target = targetIndex == 0
                ? PortableShaderTarget::Dxil
                : PortableShaderTarget::Spirv;
            diagnostics.setNull();
            slang::ProgramLayout* layout = linked->getLayout(targetIndex, diagnostics.writeRef());
            std::string reflectionError;
            if (!CollectReflection(layout, request, targetReflection[targetIndex], reflectionError))
            {
                const std::string detail = BlobText(diagnostics);
                AddDiagnostic(package, request, TargetName(target),
                    detail.empty() ? reflectionError : detail + ": " + reflectionError);
                return package;
            }
        }

        if (!SameReflection(targetReflection[0], targetReflection[1]))
        {
            AddDiagnostic(package, request, "", "DXIL and SPIR-V program/entry-point reflections differ semantically");
            return package;
        }
        package.Reflection = std::move(targetReflection[0].Bindings);
        package.VertexInputs = std::move(targetReflection[0].VertexInputs);
        if (!PortableShaderContract::Validate(
                request,
                package.Reflection,
                package.VertexInputs,
                validationError))
        {
            AddDiagnostic(package, request, "", validationError);
            package.Reflection.clear();
            package.VertexInputs.clear();
            return package;
        }

        for (SlangInt targetIndex = 0; targetIndex < 2; ++targetIndex)
        {
            const PortableShaderTarget target = targetIndex == 0
                ? PortableShaderTarget::Dxil
                : PortableShaderTarget::Spirv;
            Slang::ComPtr<slang::IBlob> code;
            diagnostics.setNull();
            if (SLANG_FAILED(linked->getEntryPointCode(
                    0,
                    targetIndex,
                    code.writeRef(),
                    diagnostics.writeRef())) || !code || code->getBufferSize() == 0)
            {
                const std::string detail = BlobText(diagnostics);
                AddDiagnostic(package, request, TargetName(target),
                    detail.empty() ? "target code was empty" : detail);
                return package;
            }
            const auto* bytes = static_cast<const u8*>(code->getBufferPointer());
            std::vector<u8>& destination = target == PortableShaderTarget::Dxil
                ? package.Dxil
                : package.Spirv;
            destination.assign(bytes, bytes + code->getBufferSize());
        }

        const bool validDxilContainer = package.Dxil.size() >= 4
            && std::memcmp(package.Dxil.data(), "DXBC", 4) == 0;
        const u32 spirvMagic = package.Spirv.size() >= sizeof(u32)
            ? static_cast<u32>(package.Spirv[0])
                | (static_cast<u32>(package.Spirv[1]) << 8)
                | (static_cast<u32>(package.Spirv[2]) << 16)
                | (static_cast<u32>(package.Spirv[3]) << 24)
            : 0;
        const bool validSpirvModule = package.Spirv.size() >= sizeof(u32)
            && package.Spirv.size() % sizeof(u32) == 0
            && spirvMagic == 0x07230203u;
        if (!validDxilContainer || !validSpirvModule)
        {
            AddDiagnostic(package, request, "", "generated artifacts did not match the required DXIL/SPIR-V container policy");
            package.Dxil.clear();
            package.Spirv.clear();
            return package;
        }

        package.Conventions = request.Conventions;
        if (!m_CacheDirectory.empty() && !PortableShaderContract::StoreAtomic(cachePath, package))
        {
            AddDiagnostic(package, request, "", "failed to store the validated shader package in the cache");
            package.Dxil.clear();
            package.Spirv.clear();
            package.Reflection.clear();
            package.VertexInputs.clear();
        }
        return package;
    }
}
