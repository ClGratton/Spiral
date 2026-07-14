#pragma once

#include "Engine/Renderer/PortableShaderContract.h"

#include <filesystem>

namespace Engine
{
    class SlangShaderCompiler
    {
    public:
        explicit SlangShaderCompiler(std::filesystem::path cacheDirectory);

        PortableShaderPackage Compile(const PortableShaderRequest& request) const;

    private:
        std::filesystem::path m_CacheDirectory;
    };
}
