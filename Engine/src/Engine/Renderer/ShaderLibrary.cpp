#include "Engine/Renderer/ShaderLibrary.h"

#include "Engine/Assets/AssetFileSystem.h"

#include <system_error>

namespace Engine
{
    ShaderSourceFile ShaderLibrary::LoadSource(std::string_view relativePath, std::string_view debugName)
    {
        ShaderSourceFile sourceFile;
        sourceFile.RelativePath = std::string(relativePath);
        sourceFile.DebugName = debugName.empty() ? sourceFile.RelativePath : std::string(debugName);
        sourceFile.ResolvedPath = AssetFileSystem::ResolvePath(relativePath);

        LoadResolvedSource(sourceFile);
        return sourceFile;
    }

    bool ShaderLibrary::ReloadSourceIfChanged(ShaderSourceFile& sourceFile)
    {
        if (!HasSourceChanged(sourceFile))
            return false;

        return LoadResolvedSource(sourceFile);
    }

    bool ShaderLibrary::HasSourceChanged(const ShaderSourceFile& sourceFile)
    {
        std::error_code fileError;
        if (sourceFile.ResolvedPath.empty() || !std::filesystem::exists(sourceFile.ResolvedPath, fileError))
            return sourceFile.Status == ShaderSourceStatus::Loaded;

        const std::filesystem::file_time_type lastWriteTime = std::filesystem::last_write_time(sourceFile.ResolvedPath, fileError);
        return !fileError && lastWriteTime != sourceFile.LastWriteTime;
    }

    const char* ShaderLibrary::ToString(ShaderSourceStatus status)
    {
        switch (status)
        {
            case ShaderSourceStatus::Unloaded: return "Unloaded";
            case ShaderSourceStatus::Loaded: return "Loaded";
            case ShaderSourceStatus::Missing: return "Missing";
            case ShaderSourceStatus::ReadFailed: return "ReadFailed";
        }

        return "Unknown";
    }

    const char* ShaderLibrary::DefaultTargetProfile(RHI::ShaderStage stage)
    {
        switch (stage)
        {
            case RHI::ShaderStage::Vertex: return "vs_5_0";
            case RHI::ShaderStage::Pixel: return "ps_5_0";
            case RHI::ShaderStage::Compute: return "cs_5_0";
            default: return "";
        }
    }

    bool ShaderLibrary::LoadResolvedSource(ShaderSourceFile& sourceFile)
    {
        std::error_code fileError;
        if (sourceFile.ResolvedPath.empty() || !std::filesystem::exists(sourceFile.ResolvedPath, fileError))
        {
            sourceFile.Status = ShaderSourceStatus::Missing;
            sourceFile.Source.clear();
            return false;
        }

        if (!AssetFileSystem::ReadTextFile(sourceFile.ResolvedPath, sourceFile.Source))
        {
            sourceFile.Status = ShaderSourceStatus::ReadFailed;
            sourceFile.Source.clear();
            return false;
        }

        sourceFile.LastWriteTime = std::filesystem::last_write_time(sourceFile.ResolvedPath, fileError);
        if (fileError)
        {
            sourceFile.Status = ShaderSourceStatus::ReadFailed;
            sourceFile.Source.clear();
            return false;
        }

        sourceFile.Status = ShaderSourceStatus::Loaded;
        ++sourceFile.Revision;
        return true;
    }
}
