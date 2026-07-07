#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace Engine
{
    class AssetFileSystem
    {
    public:
        static std::filesystem::path ResolvePath(std::string_view relativePath);
        static bool ReadTextFile(const std::filesystem::path& path, std::string& outText);

    private:
        static std::filesystem::path GetExecutableDirectory();
    };
}
