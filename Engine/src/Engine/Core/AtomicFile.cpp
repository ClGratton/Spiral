#include "Engine/Core/AtomicFile.h"

#include "Engine/Core/Base.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <limits>
#include <string>
#include <system_error>

#if defined(_WIN32)
    #define WIN32_LEAN_AND_MEAN
    #include <Windows.h>
#elif defined(GE_PLATFORM_LINUX) || defined(GE_PLATFORM_MACOS)
    #include <cstring>
    #include <fcntl.h>
    #include <sys/stat.h>
    #include <sys/syscall.h>
    #include <sys/types.h>
    #include <unistd.h>
#endif

namespace Engine
{
    bool WriteFileAtomically(const std::filesystem::path& path, std::string_view bytes,
        std::string& outError, bool* outDirectoryDurable)
    {
        if (outDirectoryDurable)
            *outDirectoryDurable = false;
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
        bool directoryDurable = false;
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
                bytes.size() - offset, (std::numeric_limits<DWORD>::max)()));
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
            directoryDurable = true;
        }
        // REPLACEFILE_WRITE_THROUGH is documented as unsupported, so a successful
        // ReplaceFileW leaves directory durability unconfirmed.
#elif defined(GE_PLATFORM_LINUX) || defined(GE_PLATFORM_MACOS)
        const std::filesystem::path parentPath = path.parent_path().empty()
            ? std::filesystem::path(".") : path.parent_path();
        // The parent is opened through a symlinked final component on purpose:
        // create_directories above succeeds through one, and the header leaves
        // trust of the parent to the caller. The temporary file and the target
        // are still never followed.
        const int parent = open(parentPath.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (parent < 0)
        {
            outError = std::string("could not open the atomic file directory: ") + std::strerror(errno);
            return false;
        }
        const std::string targetName = path.filename().string();
        // The temporary name only needs to be unique and recognizable. Cap the
        // target's share so the whole name stays under NAME_MAX (255) for any
        // target name the filesystem accepted.
        constexpr size_t maximumTemporaryStem = 120;
        const std::string temporaryStem = targetName.substr(0, maximumTemporaryStem);
        std::string temporaryName;
        int output = -1;
        for (u32 attempt = 0; attempt < 128; ++attempt)
        {
            temporaryName = "." + temporaryStem + ".tmp." + std::to_string(static_cast<u64>(getpid()))
                + "." + std::to_string(sequence.fetch_add(1, std::memory_order_relaxed));
            output = openat(parent, temporaryName.c_str(),
                O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
            if (output >= 0 || errno != EEXIST)
                break;
        }
        if (output < 0)
        {
            const int createError = errno;
            close(parent);
            outError = createError == EEXIST
                ? std::string("could not exclusively create the temporary file")
                : std::string("could not create the temporary file: ") + std::strerror(createError);
            return false;
        }
        // Replacing a file must not silently change who can read it. A new file
        // keeps the private 0600 default; an existing regular file keeps its
        // permission bits (ownership, ACLs and extended attributes are not
        // carried over).
        struct stat existing {};
        if (fstatat(parent, targetName.c_str(), &existing, AT_SYMLINK_NOFOLLOW) == 0 && S_ISREG(existing.st_mode))
            (void)fchmod(output, existing.st_mode & 07777);
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
        directoryDurable = fsync(parent) == 0;
        close(parent);
#else
        (void)bytes;
        outError = "atomic file storage is unsupported on this platform";
        return false;
#endif
        if (outDirectoryDurable)
            *outDirectoryDurable = directoryDurable;
        outError.clear();
        return true;
    }


    DirectoryPublishStatus PublishDirectoryNoReplace(const std::filesystem::path& stagedDirectory,
        const std::filesystem::path& finalDirectory, std::string& outError)
    {
        if (stagedDirectory.empty() || finalDirectory.empty()
            || stagedDirectory.filename().empty() || finalDirectory.filename().empty())
        {
            outError = "directory publication paths are invalid";
            return DirectoryPublishStatus::Failed;
        }

#if defined(GE_PLATFORM_LINUX)
        std::error_code filesystemError;
        if (!finalDirectory.parent_path().empty())
        {
            std::filesystem::create_directories(finalDirectory.parent_path(), filesystemError);
            if (filesystemError)
            {
                outError = "could not create the directory publication parent";
                return DirectoryPublishStatus::Failed;
            }
        }

        const auto openParent = [](const std::filesystem::path& path)
        {
            const std::filesystem::path parentPath = path.parent_path().empty()
                ? std::filesystem::path(".") : path.parent_path();
            return open(parentPath.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        };
        const int sourceParent = openParent(stagedDirectory);
        if (sourceParent < 0)
        {
            outError = "could not open the staged directory parent";
            return DirectoryPublishStatus::Failed;
        }
        const int finalParent = openParent(finalDirectory);
        if (finalParent < 0)
        {
            close(sourceParent);
            outError = "could not open the final directory parent";
            return DirectoryPublishStatus::Failed;
        }

        const std::string sourceName = stagedDirectory.filename().string();
        const std::string finalName = finalDirectory.filename().string();
        struct stat sourceStat {};
        if (fstatat(sourceParent, sourceName.c_str(), &sourceStat, AT_SYMLINK_NOFOLLOW) != 0
            || !S_ISDIR(sourceStat.st_mode))
        {
            close(finalParent);
            close(sourceParent);
            outError = "the staged directory is missing or is not a real directory";
            return DirectoryPublishStatus::Failed;
        }

        constexpr unsigned int renameNoReplace = 1;
        const long renamed = ::syscall(SYS_renameat2, sourceParent, sourceName.c_str(),
            finalParent, finalName.c_str(), renameNoReplace);
        const int renameError = renamed == 0 ? 0 : errno;
        DirectoryPublishStatus status = DirectoryPublishStatus::Published;
        if (renamed != 0)
        {
            if (renameError == EEXIST || renameError == ENOTEMPTY)
            {
                outError = "the final directory already exists";
                status = DirectoryPublishStatus::AlreadyExists;
            }
            else if (renameError == EINVAL || renameError == ENOSYS || renameError == EOPNOTSUPP)
            {
                outError = std::string("no-replace directory publication is unsupported here: ")
                    + std::strerror(renameError);
                status = DirectoryPublishStatus::Unsupported;
            }
            else
            {
                outError = std::string("could not publish the directory: ") + std::strerror(renameError);
                status = DirectoryPublishStatus::Failed;
            }
        }
        else
        {
            bool durable = fsync(finalParent) == 0;
            struct stat finalParentStat {};
            struct stat sourceParentStat {};
            if (fstat(finalParent, &finalParentStat) != 0 || fstat(sourceParent, &sourceParentStat) != 0
                || finalParentStat.st_ino != sourceParentStat.st_ino
                || finalParentStat.st_dev != sourceParentStat.st_dev)
                durable = fsync(sourceParent) == 0 && durable;
            if (!durable)
                status = DirectoryPublishStatus::PublishedDurabilityUnconfirmed;
            outError.clear();
        }
        close(finalParent);
        close(sourceParent);
        return status;
#else
        outError = "no-replace directory publication is not implemented on this platform";
        return DirectoryPublishStatus::Unsupported;
#endif
    }
}
