#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <string_view>

namespace Engine
{
    // Maps a registry source path to the file it names. The default (an empty function) is
    // AssetFileSystem::ResolvePath, which searches the working directory and the executable's
    // ancestors; a project rooted elsewhere supplies its own root-relative resolver.
    using AssetPathResolver = std::function<std::filesystem::path(std::string_view)>;

    class AssetFileSystem
    {
    public:
        static std::filesystem::path ResolvePath(std::string_view relativePath);
        static bool ReadTextFile(const std::filesystem::path& path, std::string& outText);

    private:
        static std::filesystem::path GetExecutableDirectory();
    };
}
