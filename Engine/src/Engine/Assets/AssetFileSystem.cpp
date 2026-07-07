#include "Engine/Assets/AssetFileSystem.h"

#include "Engine/Core/Base.h"

#include <array>
#include <cstring>
#include <fstream>
#include <sstream>
#include <system_error>

#if defined(GE_PLATFORM_WINDOWS)
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #include <Windows.h>
#elif defined(GE_PLATFORM_MACOS)
    #include <mach-o/dyld.h>
#elif defined(GE_PLATFORM_LINUX)
    #include <unistd.h>
#endif

namespace Engine
{
    namespace
    {
        bool PathExists(const std::filesystem::path& path)
        {
            std::error_code error;
            return std::filesystem::exists(path, error);
        }

        std::filesystem::path GetCurrentPath()
        {
            std::error_code error;
            std::filesystem::path path = std::filesystem::current_path(error);
            return error ? std::filesystem::path {} : path;
        }
    }

    std::filesystem::path AssetFileSystem::ResolvePath(std::string_view relativePath)
    {
        const std::filesystem::path relative { std::string(relativePath) };
        if (relative.is_absolute())
            return relative;

        const std::array<std::filesystem::path, 2> searchRoots = {
            GetCurrentPath(),
            GetExecutableDirectory()
        };

        for (const std::filesystem::path& root : searchRoots)
        {
            if (root.empty())
                continue;

            std::filesystem::path cursor = root;
            for (u32 depth = 0; depth < 8; ++depth)
            {
                const std::filesystem::path candidate = cursor / relative;
                if (PathExists(candidate))
                    return candidate;

                if (!cursor.has_parent_path() || cursor.parent_path() == cursor)
                    break;

                cursor = cursor.parent_path();
            }
        }

        return relative;
    }

    bool AssetFileSystem::ReadTextFile(const std::filesystem::path& path, std::string& outText)
    {
        std::ifstream file(path, std::ios::in | std::ios::binary);
        if (!file)
            return false;

        std::ostringstream stream;
        stream << file.rdbuf();
        outText = stream.str();
        return true;
    }

    std::filesystem::path AssetFileSystem::GetExecutableDirectory()
    {
#if defined(GE_PLATFORM_WINDOWS)
        std::wstring buffer;
        buffer.resize(MAX_PATH);

        DWORD length = 0;
        while (true)
        {
            length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
            if (length == 0)
                return {};

            if (length < buffer.size() - 1)
                break;

            buffer.resize(buffer.size() * 2);
        }

        buffer.resize(length);
        return std::filesystem::path(buffer).parent_path();
#elif defined(GE_PLATFORM_MACOS)
        uint32_t size = 0;
        _NSGetExecutablePath(nullptr, &size);
        if (size == 0)
            return {};

        std::string buffer(size, '\0');
        if (_NSGetExecutablePath(buffer.data(), &size) != 0)
            return {};

        buffer.resize(std::strlen(buffer.c_str()));
        std::error_code error;
        std::filesystem::path executablePath = std::filesystem::weakly_canonical(std::filesystem::path(buffer), error);
        if (error)
            executablePath = std::filesystem::path(buffer);

        return executablePath.parent_path();
#elif defined(GE_PLATFORM_LINUX)
        std::array<char, 4096> buffer {};
        const ssize_t length = readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
        if (length <= 0)
            return {};

        buffer[static_cast<size_t>(length)] = '\0';
        return std::filesystem::path(buffer.data()).parent_path();
#else
        return {};
#endif
    }
}
