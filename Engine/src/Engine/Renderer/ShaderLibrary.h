#pragma once

#include "Engine/RHI/Shader.h"

#include <filesystem>
#include <string>
#include <string_view>

namespace Engine
{
    enum class ShaderSourceStatus
    {
        Unloaded,
        Loaded,
        Missing,
        ReadFailed
    };

    struct ShaderSourceFile
    {
        std::string DebugName;
        std::string RelativePath;
        std::filesystem::path ResolvedPath;
        std::string Source;
        std::filesystem::file_time_type LastWriteTime {};
        ShaderSourceStatus Status = ShaderSourceStatus::Unloaded;
        u64 Revision = 0;
    };

    struct ShaderStageCompileRequest
    {
        RHI::ShaderStage Stage = RHI::ShaderStage::None;
        std::string EntryPoint = "main";
        std::string TargetProfile;
    };

    class ShaderLibrary
    {
    public:
        static ShaderSourceFile LoadSource(std::string_view relativePath, std::string_view debugName = {});
        static bool ReloadSourceIfChanged(ShaderSourceFile& sourceFile);
        static bool HasSourceChanged(const ShaderSourceFile& sourceFile);
        static const char* ToString(ShaderSourceStatus status);
        static const char* DefaultTargetProfile(RHI::ShaderStage stage);

    private:
        static bool LoadResolvedSource(ShaderSourceFile& sourceFile);
    };
}
