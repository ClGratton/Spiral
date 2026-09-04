#include "Engine/Core/AtomicFile.h"

#include "Engine/Core/Base.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <limits>
#include <system_error>

#if defined(_WIN32)
    #define WIN32_LEAN_AND_MEAN
    #include <Windows.h>
#elif defined(GE_PLATFORM_LINUX) || defined(GE_PLATFORM_MACOS)
    #include <fcntl.h>
    #include <sys/stat.h>
    #include <sys/types.h>
    #include <unistd.h>
#endif

namespace Engine
{
    bool WriteFileAtomically(
        const std::filesystem::path& path, std::string_view bytes, std::string& outError)
    {
        if (path.empty() || path.filename().empty())
        {
            outError = "atomic file destination is invalid";
            return false;
        }

        std::error_code filesystemError;
        if (!path.parent_path().empty())
        {
            std::filesystem::create_directories(path.parent_path(), filesystemError);
            if (filesystemError)
            {
                outError = "could not create the atomic file directory";
                return false;
            }
        }

        static std::atomic<u64> sequence { 0 };
#if defined(_WIN32)
        std::filesystem::path temporary;
        HANDLE output = INVALID_HANDLE_VALUE;
        for (u32 attempt = 0; attempt < 128; ++attempt)
        {
            temporary = path.wstring() + L".tmp." + std::to_wstring(GetCurrentProcessId())
                + L"." + std::to_wstring(sequence.fetch_add(1, std::memory_order_relaxed));
            output = ::CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                FILE_ATTRIBUTE_TEMPORARY, nullptr);
            if (output != INVALID_HANDLE_VALUE || ::GetLastError() != ERROR_FILE_EXISTS)
                break;
        }
        if (output == INVALID_HANDLE_VALUE)
        {
            outError = "could not exclusively create the temporary file";
            return false;
        }
        size_t offset = 0;
        while (offset < bytes.size())
        {
            const DWORD requested = static_cast<DWORD>(std::min<size_t>(
                bytes.size() - offset, std::numeric_limits<DWORD>::max()));
            DWORD written = 0;
            if (!::WriteFile(output, bytes.data() + offset, requested, &written, nullptr)
                || written != requested)
            {
                ::CloseHandle(output);
                ::DeleteFileW(temporary.c_str());
                outError = "could not write the temporary file";
                return false;
            }
            offset += written;
        }
        const bool flushed = ::FlushFileBuffers(output) != FALSE;
        const bool closed = ::CloseHandle(output) != FALSE;
        if (!flushed || !closed)
        {
            ::DeleteFileW(temporary.c_str());
            outError = "could not flush the temporary file";
            return false;
        }
        if (!::ReplaceFileW(path.c_str(), temporary.c_str(), nullptr,
            REPLACEFILE_WRITE_THROUGH, nullptr, nullptr))
        {
            const DWORD replaceError = ::GetLastError();
            if (replaceError != ERROR_FILE_NOT_FOUND
                || !::MoveFileExW(temporary.c_str(), path.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            {
                std::filesystem::remove(temporary, filesystemError);
                outError = "could not atomically publish the file";
                return false;
            }
        }
#elif defined(GE_PLATFORM_LINUX) || defined(GE_PLATFORM_MACOS)
        const std::filesystem::path parentPath = path.parent_path().empty()
            ? std::filesystem::path(".") : path.parent_path();
        const int parent = open(parentPath.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (parent < 0)
        {
            outError = "could not open the atomic file directory";
            return false;
        }
        const std::string targetName = path.filename().string();
        std::string temporaryName;
        int output = -1;
        for (u32 attempt = 0; attempt < 128; ++attempt)
        {
            temporaryName = "." + targetName + ".tmp." + std::to_string(static_cast<u64>(getpid()))
                + "." + std::to_string(sequence.fetch_add(1, std::memory_order_relaxed));
            output = openat(parent, temporaryName.c_str(),
                O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
            if (output >= 0 || errno != EEXIST)
                break;
        }
        if (output < 0)
        {
            close(parent);
            outError = "could not exclusively create the temporary file";
            return false;
        }
        size_t offset = 0;
        while (offset < bytes.size())
        {
            const ssize_t written = write(output, bytes.data() + offset, bytes.size() - offset);
            if (written > 0)
            {
                offset += static_cast<size_t>(written);
                continue;
            }
            if (written < 0 && errno == EINTR)
                continue;
            close(output);
            unlinkat(parent, temporaryName.c_str(), 0);
            close(parent);
            outError = "could not write the temporary file";
            return false;
        }
        const bool flushed = fsync(output) == 0;
        const bool closed = close(output) == 0;
        if (!flushed || !closed)
        {
            unlinkat(parent, temporaryName.c_str(), 0);
            close(parent);
            outError = "could not flush the temporary file";
            return false;
        }
        if (renameat(parent, temporaryName.c_str(), parent, targetName.c_str()) != 0)
        {
            unlinkat(parent, temporaryName.c_str(), 0);
            close(parent);
            outError = "could not atomically publish the file";
            return false;
        }
        (void)fsync(parent);
        close(parent);
#else
        (void)bytes;
        outError = "atomic file storage is unsupported on this platform";
        return false;
#endif
        outError.clear();
        return true;
    }
}
