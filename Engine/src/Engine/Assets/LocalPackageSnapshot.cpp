#if defined(_WIN32)
    #ifndef _WIN32_WINNT
        #define _WIN32_WINNT 0x0A00
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
#endif

#include "Engine/Assets/LocalPackageSnapshot.h"

#include "Engine/Core/Sha256.h"

#include "cgltf.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#if defined(GE_PLATFORM_LINUX)
    #include <dirent.h>
    #include <fcntl.h>
    #include <sys/stat.h>
    #include <sys/types.h>
    #include <unistd.h>
#elif defined(GE_PLATFORM_WINDOWS)
    #include <Windows.h>
    #include <Aclapi.h>
    #include <winternl.h>
#endif

namespace Engine
{
#if defined(GE_PLATFORM_LINUX)
    namespace
    {
        int DuplicateCloexec(int descriptor) noexcept
        {
            return descriptor < 0 ? -1 : fcntl(descriptor, F_DUPFD_CLOEXEC, 0);
        }

        int OpenDirectoryCursor(int descriptor) noexcept
        {
            return descriptor < 0 ? -1 : openat(descriptor, ".",
                O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        }

        struct DirectoryCloser
        {
            void operator()(DIR* directory) const noexcept
            {
                if (directory)
                    closedir(directory);
            }
        };

        bool RemoveSnapshotContentsAt(int directoryDescriptor, u32 depth = 0) noexcept
        {
            if (directoryDescriptor < 0 || depth > 64)
                return false;
            (void)fchmod(directoryDescriptor, 0700);
            const int duplicate = OpenDirectoryCursor(directoryDescriptor);
            if (duplicate < 0)
                return false;
            DIR* rawDirectory = fdopendir(duplicate);
            if (!rawDirectory)
            {
                close(duplicate);
                return false;
            }
            bool success = true;
            errno = 0;
            while (dirent* entry = readdir(rawDirectory))
            {
                const std::string_view name(entry->d_name);
                if (name == "." || name == "..")
                    continue;
                struct stat status {};
                if (fstatat(directoryDescriptor, entry->d_name, &status, AT_SYMLINK_NOFOLLOW) != 0)
                {
                    success = false;
                    continue;
                }
                if (S_ISDIR(status.st_mode))
                {
                    const int child = openat(directoryDescriptor, entry->d_name,
                        O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
                    if (child < 0 || !RemoveSnapshotContentsAt(child, depth + 1)
                        || unlinkat(directoryDescriptor, entry->d_name, AT_REMOVEDIR) != 0)
                        success = false;
                    if (child >= 0)
                        close(child);
                }
                else if (unlinkat(directoryDescriptor, entry->d_name, 0) != 0)
                    success = false;
                errno = 0;
            }
            if (errno != 0)
                success = false;
            closedir(rawDirectory);
            return success;
        }

        void CleanupSnapshotDirectory(int parentDescriptor, int directoryDescriptor,
            std::string_view stagingName, u64 expectedDevice, u64 expectedInode) noexcept
        {
            if (parentDescriptor < 0 || directoryDescriptor < 0 || stagingName.empty())
                return;
            struct stat openedStatus {};
            if (fstat(directoryDescriptor, &openedStatus) != 0 || !S_ISDIR(openedStatus.st_mode)
                || static_cast<u64>(openedStatus.st_dev) != expectedDevice
                || static_cast<u64>(openedStatus.st_ino) != expectedInode)
                return;
            RemoveSnapshotContentsAt(directoryDescriptor);

            struct stat namedStatus {};
            const std::string stableName(stagingName);
            if (fstatat(parentDescriptor, stableName.c_str(), &namedStatus, AT_SYMLINK_NOFOLLOW) == 0
                && S_ISDIR(namedStatus.st_mode)
                && static_cast<u64>(namedStatus.st_dev) == expectedDevice
                && static_cast<u64>(namedStatus.st_ino) == expectedInode)
                unlinkat(parentDescriptor, stableName.c_str(), AT_REMOVEDIR);
        }
    }
#endif

#if defined(GE_PLATFORM_WINDOWS)
    namespace
    {
        void CleanupSnapshotDirectoryWindows(HANDLE directoryHandle) noexcept;
        bool StreamWindowsSnapshotEntry(HANDLE directoryHandle,
            const std::vector<std::string>& segments, const LocalPackageSnapshotEntry& expected,
            const std::function<bool(std::span<const u8>)>& onChunk, std::string& error);
    }
#endif

    LocalPackageSnapshot::~LocalPackageSnapshot()
    {
        Reset();
    }

    LocalPackageSnapshot::LocalPackageSnapshot(LocalPackageSnapshot&& other) noexcept
        : m_Directory(std::move(other.m_Directory)),
          m_Entries(std::move(other.m_Entries)),
          m_TreeSha256(std::move(other.m_TreeSha256)),
          m_RootRelativePath(std::move(other.m_RootRelativePath)),
#if defined(GE_PLATFORM_WINDOWS)
          m_StagingParentHandle(std::exchange(other.m_StagingParentHandle, nullptr)),
          m_DirectoryHandle(std::exchange(other.m_DirectoryHandle, nullptr)),
#else
          m_DirectoryDevice(std::exchange(other.m_DirectoryDevice, 0)),
          m_DirectoryInode(std::exchange(other.m_DirectoryInode, 0)),
          m_StagingParentDescriptor(std::exchange(other.m_StagingParentDescriptor, -1)),
          m_DirectoryDescriptor(std::exchange(other.m_DirectoryDescriptor, -1)),
#endif
          m_StagingName(std::move(other.m_StagingName))
    {
        other.m_Directory.clear();
    }

    LocalPackageSnapshot& LocalPackageSnapshot::operator=(LocalPackageSnapshot&& other) noexcept
    {
        if (this == &other)
            return *this;
        Reset();
        m_Directory = std::move(other.m_Directory);
        m_Entries = std::move(other.m_Entries);
        m_TreeSha256 = std::move(other.m_TreeSha256);
        m_RootRelativePath = std::move(other.m_RootRelativePath);
#if defined(GE_PLATFORM_WINDOWS)
        m_StagingParentHandle = std::exchange(other.m_StagingParentHandle, nullptr);
        m_DirectoryHandle = std::exchange(other.m_DirectoryHandle, nullptr);
#else
        m_DirectoryDevice = std::exchange(other.m_DirectoryDevice, 0);
        m_DirectoryInode = std::exchange(other.m_DirectoryInode, 0);
        m_StagingParentDescriptor = std::exchange(other.m_StagingParentDescriptor, -1);
        m_DirectoryDescriptor = std::exchange(other.m_DirectoryDescriptor, -1);
#endif
        m_StagingName = std::move(other.m_StagingName);
        other.m_Directory.clear();
        return *this;
    }

    bool LocalPackageSnapshot::RetainedOwnershipIsNonInheritableForTesting() const
    {
#if defined(GE_PLATFORM_LINUX)
        return m_StagingParentDescriptor >= 0 && m_DirectoryDescriptor >= 0
            && (fcntl(m_StagingParentDescriptor, F_GETFD) & FD_CLOEXEC) != 0
            && (fcntl(m_DirectoryDescriptor, F_GETFD) & FD_CLOEXEC) != 0;
#elif defined(GE_PLATFORM_WINDOWS)
        DWORD parentFlags = HANDLE_FLAG_INHERIT;
        DWORD directoryFlags = HANDLE_FLAG_INHERIT;
        return m_StagingParentHandle && m_DirectoryHandle
            && ::GetHandleInformation(static_cast<HANDLE>(m_StagingParentHandle), &parentFlags)
            && ::GetHandleInformation(static_cast<HANDLE>(m_DirectoryHandle), &directoryFlags)
            && (parentFlags & HANDLE_FLAG_INHERIT) == 0
            && (directoryFlags & HANDLE_FLAG_INHERIT) == 0;
#else
        return false;
#endif
    }

    bool LocalPackageSnapshot::StreamFile(std::string_view relativePath,
        const std::function<bool(std::span<const u8>)>& onChunk, std::string& error) const
    {
        error.clear();
        const auto found = std::find_if(m_Entries.begin(), m_Entries.end(), [&](const auto& entry)
        {
            return entry.RelativePath == relativePath;
        });
        if (!onChunk || found == m_Entries.end()) { error = "snapshot entry is not accepted"; return false; }
        std::vector<std::string> segments;
        size_t begin = 0;
        while (begin < relativePath.size())
        {
            const size_t end = relativePath.find('/', begin);
            const std::string_view part = relativePath.substr(begin, end - begin);
            if (part.empty() || part == "." || part == "..") { error = "snapshot entry is invalid"; return false; }
            segments.emplace_back(part);
            if (end == std::string_view::npos) break;
            begin = end + 1;
        }
        try
        {
#if defined(GE_PLATFORM_LINUX)
            int parent = m_DirectoryDescriptor < 0 ? -1 : fcntl(m_DirectoryDescriptor, F_DUPFD_CLOEXEC, 0);
            if (parent < 0)
            {
                error = "snapshot root is unavailable";
                return false;
            }
            for (size_t i = 0; i + 1 < segments.size(); ++i)
            {
                const int child = openat(parent, segments[i].c_str(),
                    O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
                if (child < 0)
                {
                    close(parent);
                    error = "snapshot entry is unavailable";
                    return false;
                }
                close(parent);
                parent = child;
            }
            const int file = openat(parent, segments.back().c_str(),
                O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
            close(parent);
            struct stat before {}, after {};
            if (file < 0 || fstat(file, &before) != 0 || !S_ISREG(before.st_mode)
                || before.st_nlink != 1 || static_cast<u64>(before.st_size) != found->SizeBytes)
            {
                if (file >= 0)
                    close(file);
                error = "snapshot entry changed";
                return false;
            }
            Sha256Builder hash;
            std::array<u8, 64 * 1024> buffer {};
            u64 total = 0;
            while (true)
            {
                const ssize_t count = read(file, buffer.data(), buffer.size());
                if (count < 0)
                {
                    if (errno == EINTR)
                        continue;
                    close(file);
                    error = "snapshot entry could not be read";
                    return false;
                }
                if (count == 0)
                    break;
                const auto byteCount = static_cast<u64>(count);
                const std::span<const u8> bytes(buffer.data(), static_cast<size_t>(count));
                if (total > found->SizeBytes || byteCount > found->SizeBytes - total
                    || !onChunk(bytes))
                {
                    close(file);
                    error = "snapshot entry callback aborted";
                    return false;
                }
                hash.Update(bytes);
                total += byteCount;
            }
            if (fstat(file, &after) != 0 || before.st_dev != after.st_dev || before.st_ino != after.st_ino
                || before.st_size != after.st_size || before.st_nlink != after.st_nlink
                || total != found->SizeBytes || hash.FinalizeHex() != found->Sha256)
            {
                close(file);
                error = "snapshot entry changed while reading";
                return false;
            }
            close(file);
            return true;
#elif defined(GE_PLATFORM_WINDOWS)
            return StreamWindowsSnapshotEntry(static_cast<HANDLE>(m_DirectoryHandle), segments,
                *found, onChunk, error);
#else
            error = "secure local package snapshots are unsupported";
            return false;
#endif
        }
        catch (...) { error = "snapshot entry callback or allocation failed"; return false; }
    }

    void LocalPackageSnapshot::Reset() noexcept
    {
#if defined(GE_PLATFORM_LINUX)
        CleanupSnapshotDirectory(m_StagingParentDescriptor, m_DirectoryDescriptor,
            m_StagingName, m_DirectoryDevice, m_DirectoryInode);
        if (m_DirectoryDescriptor >= 0)
            close(m_DirectoryDescriptor);
        if (m_StagingParentDescriptor >= 0)
            close(m_StagingParentDescriptor);
#elif defined(GE_PLATFORM_WINDOWS)
        CleanupSnapshotDirectoryWindows(
            static_cast<HANDLE>(m_DirectoryHandle));
        if (m_DirectoryHandle)
            ::CloseHandle(static_cast<HANDLE>(m_DirectoryHandle));
        if (m_StagingParentHandle)
            ::CloseHandle(static_cast<HANDLE>(m_StagingParentHandle));
#else
        if (!m_Directory.empty())
        {
            std::error_code error;
            std::filesystem::remove_all(m_Directory, error);
        }
#endif
        m_Directory.clear();
        m_Entries.clear();
        m_TreeSha256.clear();
        m_RootRelativePath.clear();
#if defined(GE_PLATFORM_WINDOWS)
        m_StagingParentHandle = nullptr;
        m_DirectoryHandle = nullptr;
#else
        m_DirectoryDevice = 0;
        m_DirectoryInode = 0;
        m_StagingParentDescriptor = -1;
        m_DirectoryDescriptor = -1;
#endif
        m_StagingName.clear();
    }

#if defined(GE_PLATFORM_LINUX) || defined(GE_PLATFORM_WINDOWS)
    namespace
    {
        struct ProgressState
        {
            u64 FilesCompleted = 0;
            u64 FileCount = 0;
            u64 BytesCompleted = 0;
            u64 AggregateBytes = 0;
        };

#if defined(GE_PLATFORM_LINUX)
        class FileDescriptor
        {
        public:
            FileDescriptor() = default;
            explicit FileDescriptor(int descriptor) : m_Descriptor(descriptor) {}
            ~FileDescriptor() { Reset(); }

            FileDescriptor(const FileDescriptor&) = delete;
            FileDescriptor& operator=(const FileDescriptor&) = delete;
            FileDescriptor(FileDescriptor&& other) noexcept : m_Descriptor(std::exchange(other.m_Descriptor, -1)) {}
            FileDescriptor& operator=(FileDescriptor&& other) noexcept
            {
                if (this != &other)
                {
                    Reset();
                    m_Descriptor = std::exchange(other.m_Descriptor, -1);
                }
                return *this;
            }

            int Get() const { return m_Descriptor; }
            explicit operator bool() const { return m_Descriptor >= 0; }
            int Release() { return std::exchange(m_Descriptor, -1); }

        private:
            void Reset()
            {
                if (m_Descriptor >= 0)
                    close(m_Descriptor);
                m_Descriptor = -1;
            }

            int m_Descriptor = -1;
        };

        class CreatedDirectoryGuard
        {
        public:
            CreatedDirectoryGuard(int parentDescriptor, std::string name, const struct stat& status)
                : m_ParentDescriptor(parentDescriptor), m_Name(std::move(name)),
                  m_Device(status.st_dev), m_Inode(status.st_ino)
            {
            }

            ~CreatedDirectoryGuard()
            {
                if (!m_Armed)
                    return;
                struct stat status {};
                if (fstatat(m_ParentDescriptor, m_Name.c_str(), &status, AT_SYMLINK_NOFOLLOW) == 0
                    && S_ISDIR(status.st_mode) && status.st_dev == m_Device && status.st_ino == m_Inode)
                    unlinkat(m_ParentDescriptor, m_Name.c_str(), AT_REMOVEDIR);
            }

            void Release() noexcept { m_Armed = false; }

        private:
            int m_ParentDescriptor = -1;
            std::string m_Name;
            dev_t m_Device = 0;
            ino_t m_Inode = 0;
            bool m_Armed = true;
        };

        struct FileIdentity
        {
            dev_t Device = 0;
            ino_t Inode = 0;
            mode_t Mode = 0;
            nlink_t LinkCount = 0;
            off_t Size = 0;
            timespec Modified {};
            timespec Changed {};
            u64 MountId = 0;
        };

        struct InventoryRecord
        {
            std::string RelativePath;
            std::vector<std::string> Segments;
            FileIdentity Identity;
            bool Directory = false;
        };

        struct Inventory
        {
            FileIdentity RootIdentity;
            std::vector<InventoryRecord> Files;
            std::vector<InventoryRecord> Directories;
            u64 AggregateBytes = 0;
        };

        struct SnapshotPayload
        {
            ~SnapshotPayload()
            {
                CleanupSnapshotDirectory(ParentDescriptor, DirectoryDescriptor,
                    StagingName, DirectoryDevice, DirectoryInode);
                if (DirectoryDescriptor >= 0)
                    close(DirectoryDescriptor);
                if (ParentDescriptor >= 0)
                    close(ParentDescriptor);
            }

            std::filesystem::path Directory;
            std::vector<LocalPackageSnapshotEntry> Entries;
            std::string TreeSha256;
            std::string RootRelativePath;
            u64 DirectoryDevice = 0;
            u64 DirectoryInode = 0;
            int ParentDescriptor = -1;
            int DirectoryDescriptor = -1;
            std::string StagingName;
        };

        bool GetMountId(int descriptor, u64& mountId)
        {
            struct statx extendedStatus {};
            if (statx(descriptor, "", AT_EMPTY_PATH | AT_NO_AUTOMOUNT,
                STATX_MNT_ID, &extendedStatus) != 0
                || (extendedStatus.stx_mask & STATX_MNT_ID) == 0)
            {
                return false;
            }
            mountId = extendedStatus.stx_mnt_id;
            return true;
        }

        FileIdentity GetIdentity(const struct stat& status, u64 mountId = 0)
        {
            return {
                status.st_dev,
                status.st_ino,
                status.st_mode,
                status.st_nlink,
                status.st_size,
                status.st_mtim,
                status.st_ctim,
                mountId
            };
        }

        bool SameTime(const timespec& left, const timespec& right)
        {
            return left.tv_sec == right.tv_sec && left.tv_nsec == right.tv_nsec;
        }

        bool SameIdentity(const FileIdentity& left, const FileIdentity& right)
        {
            return left.Device == right.Device
                && left.Inode == right.Inode
                && left.Mode == right.Mode
                && left.LinkCount == right.LinkCount
                && left.Size == right.Size
                && SameTime(left.Modified, right.Modified)
                && SameTime(left.Changed, right.Changed)
                && left.MountId == right.MountId;
        }
#endif

        bool Checkpoint(
            const LocalPackageSnapshotOptions& options,
            LocalPackageSnapshotHookPoint point,
            std::string_view relativePath,
            const ProgressState& state,
            std::string& error)
        {
            if (options.TestHook)
                options.TestHook(point, relativePath);
            if (options.Progress)
            {
                options.Progress({
                    point,
                    state.FilesCompleted,
                    state.FileCount,
                    state.BytesCompleted,
                    state.AggregateBytes,
                    relativePath
                });
            }
            if (options.IsCancelled && options.IsCancelled())
            {
                error = "local package snapshot was cancelled";
                return false;
            }
            return true;
        }

        bool IsAsciiCaseInsensitiveEqual(std::string_view left, std::string_view right)
        {
            if (left.size() != right.size())
                return false;
            for (size_t index = 0; index < left.size(); ++index)
            {
                const auto fold = [](unsigned char value)
                {
                    return value >= 'a' && value <= 'z' ? static_cast<unsigned char>(value - ('a' - 'A')) : value;
                };
                if (fold(static_cast<unsigned char>(left[index])) != fold(static_cast<unsigned char>(right[index])))
                    return false;
            }
            return true;
        }

        std::string AsciiCaseFold(std::string_view value)
        {
            std::string result(value);
            for (char& character : result)
            {
                if (character >= 'A' && character <= 'Z')
                    character = static_cast<char>(character + ('a' - 'A'));
            }
            return result;
        }

        bool HasForbiddenPayloadExtension(std::string_view relativePath)
        {
            const std::string folded = AsciiCaseFold(relativePath);
            constexpr std::array<std::string_view, 25> extensions {
                ".zip", ".7z", ".rar", ".tar", ".tgz", ".gz", ".bz2", ".xz",
                ".exe", ".dll", ".so", ".dylib", ".com", ".msi", ".scr", ".app",
                ".bat", ".cmd", ".ps1", ".sh", ".py", ".js", ".vbs", ".jar", ".wasm"
            };
            return std::any_of(extensions.begin(), extensions.end(), [&folded](std::string_view extension)
            {
                return folded.size() >= extension.size()
                    && folded.compare(folded.size() - extension.size(), extension.size(), extension) == 0;
            });
        }

        bool HasForbiddenPayloadMagic(std::span<const u8> bytes)
        {
            const auto starts = [bytes](std::initializer_list<u8> prefix)
            {
                return bytes.size() >= prefix.size()
                    && std::equal(prefix.begin(), prefix.end(), bytes.begin());
            };
            return starts({ '#', '!' }) || starts({ 'M', 'Z' })
                || starts({ 0x7f, 'E', 'L', 'F' }) || starts({ 0x00, 'a', 's', 'm' })
                || starts({ 'P', 'K', 0x03, 0x04 }) || starts({ 'P', 'K', 0x05, 0x06 })
                || starts({ 'P', 'K', 0x07, 0x08 }) || starts({ 0x1f, 0x8b })
                || starts({ '7', 'z', 0xbc, 0xaf, 0x27, 0x1c })
                || starts({ 'R', 'a', 'r', '!', 0x1a, 0x07 })
                || starts({ 0xfe, 0xed, 0xfa, 0xce }) || starts({ 0xce, 0xfa, 0xed, 0xfe })
                || starts({ 0xfe, 0xed, 0xfa, 0xcf }) || starts({ 0xcf, 0xfa, 0xed, 0xfe });
        }

        bool IsWindowsReservedName(std::string_view segment)
        {
            const size_t period = segment.find('.');
            const std::string_view stem = segment.substr(0, period);
            if (IsAsciiCaseInsensitiveEqual(stem, "CON")
                || IsAsciiCaseInsensitiveEqual(stem, "PRN")
                || IsAsciiCaseInsensitiveEqual(stem, "AUX")
                || IsAsciiCaseInsensitiveEqual(stem, "NUL"))
            {
                return true;
            }
            if (stem.size() == 4 && stem[3] >= '1' && stem[3] <= '9')
            {
                const std::string_view prefix = stem.substr(0, 3);
                return IsAsciiCaseInsensitiveEqual(prefix, "COM") || IsAsciiCaseInsensitiveEqual(prefix, "LPT");
            }
            return false;
        }

        bool ValidateSegment(std::string_view segment, u64 maximumBytes, std::string& error)
        {
            if (segment.empty() || segment == "." || segment == ".." || segment.size() > maximumBytes)
            {
                error = "package path segment is empty, relative, or exceeds the configured limit";
                return false;
            }
            if (segment.back() == '.' || segment.back() == ' ' || IsWindowsReservedName(segment))
            {
                error = "package path segment is not portable";
                return false;
            }
            for (unsigned char character : segment)
            {
                if (character < 0x20 || character > 0x7e || character == '/' || character == '\\'
                    || character == ':' || character == '<' || character == '>' || character == '"'
                    || character == '|' || character == '?' || character == '*')
                {
                    error = "package path contains a non-portable character";
                    return false;
                }
            }
            return true;
        }

        std::string JoinSegments(const std::vector<std::string>& segments)
        {
            std::string result;
            for (const std::string& segment : segments)
            {
                if (!result.empty())
                    result.push_back('/');
                result += segment;
            }
            return result;
        }

        bool AddObjectPath(
            const std::vector<std::string>& segments,
            const LocalPackageSnapshotLimits& limits,
            std::unordered_set<std::string>& foldedPaths,
            std::string& relativePath,
            std::string& error)
        {
            if (segments.size() > limits.MaximumDepth)
            {
                error = "package path exceeds the configured depth limit";
                return false;
            }
            relativePath = JoinSegments(segments);
            if (relativePath.size() > limits.MaximumPathBytes)
            {
                error = "package path exceeds the configured byte limit";
                return false;
            }
            if (!foldedPaths.insert(AsciiCaseFold(relativePath)).second)
            {
                error = "package paths collide after portable ASCII case folding";
                return false;
            }
            return true;
        }

#if defined(GE_PLATFORM_LINUX)
        bool IsWithin(std::string_view parent, std::string_view child)
        {
            if (parent == child)
                return true;
            if (parent == "/")
                return !child.empty() && child.front() == '/';
            return child.size() > parent.size() && child[parent.size()] == '/'
                && child.substr(0, parent.size()) == parent;
        }

        bool ReadDescriptorPath(int descriptor, std::string& path)
        {
            const std::string link = "/proc/self/fd/" + std::to_string(descriptor);
            std::vector<char> buffer(256);
            while (buffer.size() <= 1024 * 1024)
            {
                const ssize_t length = readlink(link.c_str(), buffer.data(), buffer.size());
                if (length < 0)
                    return false;
                if (static_cast<size_t>(length) < buffer.size())
                {
                    path.assign(buffer.data(), static_cast<size_t>(length));
                    return true;
                }
                buffer.resize(buffer.size() * 2);
            }
            return false;
        }

        FileDescriptor OpenAbsoluteDirectoryNoFollow(const std::filesystem::path& input)
        {
            if (input.empty())
                return {};
            std::error_code filesystemError;
            const std::filesystem::path absolute = std::filesystem::absolute(input, filesystemError).lexically_normal();
            if (filesystemError || !absolute.is_absolute())
                return {};

            FileDescriptor current(open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC));
            if (!current)
                return {};
            for (const std::filesystem::path& componentPath : absolute.relative_path())
            {
                const std::string component = componentPath.string();
                if (component.empty() || component == ".")
                    continue;
                if (component == "..")
                    return {};
                FileDescriptor next(openat(current.Get(), component.c_str(),
                    O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
                if (!next)
                    return {};
                current = std::move(next);
            }
            return current;
        }

        bool EnumerateDirectory(
            int directoryDescriptor,
            const std::vector<std::string>& parentSegments,
            dev_t rootDevice,
            u64 rootMountId,
            const LocalPackageSnapshotOptions& options,
            ProgressState& progress,
            Inventory& inventory,
            std::unordered_set<std::string>& foldedPaths,
            u64& directoryCount,
            std::string& error)
        {
            struct stat beforeStatus {};
            u64 directoryMountId = 0;
            if (fstat(directoryDescriptor, &beforeStatus) != 0 || !S_ISDIR(beforeStatus.st_mode)
                || beforeStatus.st_dev != rootDevice
                || !GetMountId(directoryDescriptor, directoryMountId) || directoryMountId != rootMountId)
            {
                error = "package directory changed identity or crossed a device boundary";
                return false;
            }
            const FileIdentity beforeIdentity = GetIdentity(beforeStatus, directoryMountId);

            FileDescriptor duplicate(OpenDirectoryCursor(directoryDescriptor));
            if (!duplicate)
            {
                error = "could not enumerate package directory";
                return false;
            }
            DIR* rawDirectory = fdopendir(duplicate.Release());
            if (!rawDirectory)
            {
                error = "could not enumerate package directory";
                return false;
            }
            std::unique_ptr<DIR, DirectoryCloser> directory(rawDirectory);
            std::vector<std::string> names;
            const u64 rawEntryLimit = options.Limits.MaximumFileCount > std::numeric_limits<u64>::max() / 2
                ? std::numeric_limits<u64>::max()
                : options.Limits.MaximumFileCount * 2;
            errno = 0;
            while (dirent* entry = readdir(directory.get()))
            {
                const std::string_view name(entry->d_name);
                if (name != "." && name != "..")
                {
                    if (names.size() >= rawEntryLimit)
                    {
                        error = "package object count exceeds the configured inventory bound";
                        return false;
                    }
                    names.emplace_back(name);
                    if ((names.size() & 255u) == 0 && options.IsCancelled && options.IsCancelled())
                    {
                        error = "local package snapshot was cancelled";
                        return false;
                    }
                }
                errno = 0;
            }
            if (errno != 0)
            {
                error = "could not enumerate package directory";
                return false;
            }
            std::sort(names.begin(), names.end());

            for (const std::string& name : names)
            {
                if (!ValidateSegment(name, options.Limits.MaximumSegmentBytes, error))
                    return false;
                std::vector<std::string> segments = parentSegments;
                segments.push_back(name);
                std::string relativePath;
                if (!AddObjectPath(segments, options.Limits, foldedPaths, relativePath, error))
                    return false;

                struct stat pathStatus {};
                if (fstatat(directoryDescriptor, name.c_str(), &pathStatus, AT_SYMLINK_NOFOLLOW) != 0)
                {
                    error = "package entry changed during inventory";
                    return false;
                }
                if (S_ISLNK(pathStatus.st_mode))
                {
                    error = "package links are not allowed";
                    return false;
                }
                if (pathStatus.st_dev != rootDevice)
                {
                    error = "package entry crosses a device or mount boundary";
                    return false;
                }

                if (S_ISDIR(pathStatus.st_mode))
                {
                    if (directoryCount == std::numeric_limits<u64>::max()
                        || ++directoryCount > options.Limits.MaximumFileCount)
                    {
                        error = "package directory count exceeds the configured file-count bound";
                        return false;
                    }
                    FileDescriptor child(openat(directoryDescriptor, name.c_str(),
                        O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
                    struct stat openedStatus {};
                    u64 openedMountId = 0;
                    if (!child || fstat(child.Get(), &openedStatus) != 0
                        || !SameIdentity(GetIdentity(pathStatus), GetIdentity(openedStatus))
                        || !GetMountId(child.Get(), openedMountId) || openedMountId != rootMountId)
                    {
                        error = "package directory changed during inventory";
                        return false;
                    }
                    inventory.Directories.push_back({
                        relativePath, segments, GetIdentity(openedStatus, openedMountId), true
                    });
                    if (!Checkpoint(options, LocalPackageSnapshotHookPoint::InventoryEntry,
                        relativePath, progress, error))
                        return false;
                    if (!EnumerateDirectory(child.Get(), segments, rootDevice, rootMountId, options, progress,
                        inventory, foldedPaths, directoryCount, error))
                        return false;
                    continue;
                }

                if (!S_ISREG(pathStatus.st_mode))
                {
                    error = "package contains a non-regular object";
                    return false;
                }
                if ((pathStatus.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH)) != 0
                    || HasForbiddenPayloadExtension(relativePath))
                {
                    error = "package contains an executable, script, or nested archive payload";
                    return false;
                }
                FileDescriptor file(openat(directoryDescriptor, name.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
                struct stat openedStatus {};
                u64 openedMountId = 0;
                if (!file || fstat(file.Get(), &openedStatus) != 0
                    || !SameIdentity(GetIdentity(pathStatus), GetIdentity(openedStatus))
                    || !GetMountId(file.Get(), openedMountId) || openedMountId != rootMountId)
                {
                    error = "package file changed during inventory";
                    return false;
                }
                if (openedStatus.st_nlink != 1)
                {
                    error = "package hard-linked files are not allowed";
                    return false;
                }
                if (openedStatus.st_size < 0
                    || static_cast<u64>(openedStatus.st_size) > options.Limits.MaximumFileBytes)
                {
                    error = "package file exceeds the configured size limit";
                    return false;
                }
                if (inventory.Files.size() >= options.Limits.MaximumFileCount)
                {
                    error = "package file count exceeds the configured limit";
                    return false;
                }
                const u64 size = static_cast<u64>(openedStatus.st_size);
                if (inventory.AggregateBytes > options.Limits.MaximumAggregateBytes
                    || size > options.Limits.MaximumAggregateBytes - inventory.AggregateBytes)
                {
                    error = "package aggregate size exceeds the configured limit";
                    return false;
                }
                inventory.AggregateBytes += size;
                inventory.Files.push_back({
                    relativePath, segments, GetIdentity(openedStatus, openedMountId), false
                });
                progress.FileCount = static_cast<u64>(inventory.Files.size());
                progress.AggregateBytes = inventory.AggregateBytes;
                if (!Checkpoint(options, LocalPackageSnapshotHookPoint::InventoryEntry,
                    relativePath, progress, error))
                    return false;
            }

            struct stat afterStatus {};
            u64 afterMountId = 0;
            if (fstat(directoryDescriptor, &afterStatus) != 0
                || !GetMountId(directoryDescriptor, afterMountId)
                || !SameIdentity(beforeIdentity, GetIdentity(afterStatus, afterMountId)))
            {
                error = "package directory changed during inventory";
                return false;
            }
            return true;
        }

        bool BuildInventory(
            int rootDescriptor,
            const LocalPackageSnapshotOptions& options,
            ProgressState& progress,
            Inventory& inventory,
            std::string& error)
        {
            struct stat rootStatus {};
            if (fstat(rootDescriptor, &rootStatus) != 0 || !S_ISDIR(rootStatus.st_mode))
            {
                error = "selected package root is not a directory";
                return false;
            }
            u64 rootMountId = 0;
            if (!GetMountId(rootDescriptor, rootMountId))
            {
                error = "could not establish the package root mount identity";
                return false;
            }
            inventory.RootIdentity = GetIdentity(rootStatus, rootMountId);
            std::unordered_set<std::string> foldedPaths;
            u64 directoryCount = 0;
            if (!EnumerateDirectory(rootDescriptor, {}, rootStatus.st_dev, rootMountId, options, progress,
                inventory, foldedPaths, directoryCount, error))
                return false;
            std::sort(inventory.Files.begin(), inventory.Files.end(), [](const auto& left, const auto& right)
            {
                return left.RelativePath < right.RelativePath;
            });
            std::sort(inventory.Directories.begin(), inventory.Directories.end(), [](const auto& left, const auto& right)
            {
                return left.RelativePath < right.RelativePath;
            });
            progress.FileCount = static_cast<u64>(inventory.Files.size());
            progress.AggregateBytes = inventory.AggregateBytes;
            return true;
        }

        bool SameInventory(const Inventory& left, const Inventory& right)
        {
            if (!SameIdentity(left.RootIdentity, right.RootIdentity)
                || left.AggregateBytes != right.AggregateBytes
                || left.Files.size() != right.Files.size()
                || left.Directories.size() != right.Directories.size())
                return false;
            const auto sameRecords = [](const auto& leftRecords, const auto& rightRecords)
            {
                for (size_t index = 0; index < leftRecords.size(); ++index)
                {
                    if (leftRecords[index].RelativePath != rightRecords[index].RelativePath
                        || leftRecords[index].Directory != rightRecords[index].Directory
                        || !SameIdentity(leftRecords[index].Identity, rightRecords[index].Identity))
                        return false;
                }
                return true;
            };
            return sameRecords(left.Files, right.Files) && sameRecords(left.Directories, right.Directories);
        }

        FileDescriptor OpenRelativeFile(
            int rootDescriptor,
            const std::vector<std::string>& segments,
            dev_t rootDevice,
            u64 rootMountId,
            std::string& error)
        {
            FileDescriptor parent(DuplicateCloexec(rootDescriptor));
            if (!parent)
            {
                error = "could not reopen package root";
                return {};
            }
            for (size_t index = 0; index + 1 < segments.size(); ++index)
            {
                FileDescriptor child(openat(parent.Get(), segments[index].c_str(),
                    O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
                struct stat status {};
                u64 mountId = 0;
                if (!child || fstat(child.Get(), &status) != 0 || !S_ISDIR(status.st_mode)
                    || status.st_dev != rootDevice
                    || !GetMountId(child.Get(), mountId) || mountId != rootMountId)
                {
                    error = "package path changed before copying";
                    return {};
                }
                parent = std::move(child);
            }
            FileDescriptor file(openat(parent.Get(), segments.back().c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
            if (!file)
                error = "package file changed before copying";
            return file;
        }

        FileDescriptor CreateDestinationFile(
            int stagingDescriptor,
            const std::vector<std::string>& segments,
            std::string& error)
        {
            FileDescriptor parent(DuplicateCloexec(stagingDescriptor));
            if (!parent)
            {
                error = "could not prepare snapshot destination";
                return {};
            }
            for (size_t index = 0; index + 1 < segments.size(); ++index)
            {
                if (mkdirat(parent.Get(), segments[index].c_str(), 0700) != 0 && errno != EEXIST)
                {
                    error = "could not create snapshot directory";
                    return {};
                }
                FileDescriptor child(openat(parent.Get(), segments[index].c_str(),
                    O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
                if (!child)
                {
                    error = "could not open snapshot directory";
                    return {};
                }
                parent = std::move(child);
            }
            FileDescriptor file(openat(parent.Get(), segments.back().c_str(),
                O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600));
            if (!file)
                error = "could not create exclusive snapshot file";
            return file;
        }

        bool WriteAll(int descriptor, const u8* bytes, size_t size, std::string& error)
        {
            size_t offset = 0;
            while (offset < size)
            {
                const ssize_t written = write(descriptor, bytes + offset, size - offset);
                if (written > 0)
                {
                    offset += static_cast<size_t>(written);
                    continue;
                }
                if (written < 0 && errno == EINTR)
                    continue;
                error = "could not write snapshot file";
                return false;
            }
            return true;
        }

        bool CopyFile(
            int sourceRootDescriptor,
            int stagingDescriptor,
            dev_t rootDevice,
            u64 rootMountId,
            const InventoryRecord& record,
            const LocalPackageSnapshotOptions& options,
            ProgressState& progress,
            LocalPackageSnapshotEntry& entry,
            Sha256Builder::Digest& digest,
            std::string& error)
        {
            FileDescriptor source = OpenRelativeFile(
                sourceRootDescriptor, record.Segments, rootDevice, rootMountId, error);
            struct stat beforeStatus {};
            u64 beforeMountId = 0;
            if (!source || fstat(source.Get(), &beforeStatus) != 0
                || !GetMountId(source.Get(), beforeMountId)
                || !SameIdentity(record.Identity, GetIdentity(beforeStatus, beforeMountId))
                || !S_ISREG(beforeStatus.st_mode) || beforeStatus.st_nlink != 1)
            {
                if (error.empty()) error = "package file changed before copying";
                return false;
            }
            FileDescriptor destination = CreateDestinationFile(stagingDescriptor, record.Segments, error);
            if (!destination)
                return false;
            if (!Checkpoint(options, LocalPackageSnapshotHookPoint::BeforeFileCopy,
                record.RelativePath, progress, error))
                return false;

            Sha256Builder hash;
            std::array<u8, 64 * 1024> buffer {};
            u64 copiedBytes = 0;
            while (true)
            {
                ssize_t count = read(source.Get(), buffer.data(), buffer.size());
                if (count < 0 && errno == EINTR)
                    continue;
                if (count < 0)
                {
                    error = "could not read package file";
                    return false;
                }
                if (count == 0)
                    break;
                const size_t byteCount = static_cast<size_t>(count);
                if (copiedBytes == 0
                    && HasForbiddenPayloadMagic(std::span<const u8>(buffer.data(), byteCount)))
                {
                    error = "package contains executable or nested-archive payload bytes";
                    return false;
                }
                if (copiedBytes > static_cast<u64>(record.Identity.Size)
                    || byteCount > static_cast<u64>(record.Identity.Size) - copiedBytes)
                {
                    error = "package file grew while being copied";
                    return false;
                }
                hash.Update(std::span<const u8>(buffer.data(), byteCount));
                if (!WriteAll(destination.Get(), buffer.data(), byteCount, error))
                    return false;
                copiedBytes += byteCount;
                progress.BytesCompleted += byteCount;
                if (options.Progress)
                {
                    options.Progress({
                        LocalPackageSnapshotHookPoint::BeforeFileCopy,
                        progress.FilesCompleted,
                        progress.FileCount,
                        progress.BytesCompleted,
                        progress.AggregateBytes,
                        record.RelativePath
                    });
                }
                if (options.IsCancelled && options.IsCancelled())
                {
                    error = "local package snapshot was cancelled";
                    return false;
                }
            }
            struct stat afterStatus {};
            u64 afterMountId = 0;
            if (copiedBytes != static_cast<u64>(record.Identity.Size)
                || fstat(source.Get(), &afterStatus) != 0
                || !GetMountId(source.Get(), afterMountId)
                || !SameIdentity(record.Identity, GetIdentity(afterStatus, afterMountId)))
            {
                error = "package file changed while being copied";
                return false;
            }
            if (fchmod(destination.Get(), 0400) != 0)
            {
                error = "could not make snapshot file read-only";
                return false;
            }
            digest = hash.FinalizeBytes();
            entry = { record.RelativePath, copiedBytes, Sha256Builder::ToHex(digest) };
            ++progress.FilesCompleted;
            return Checkpoint(options, LocalPackageSnapshotHookPoint::AfterFileCopy,
                record.RelativePath, progress, error);
        }
#endif

        u32 ReadLittleU32(const u8* bytes)
        {
            return static_cast<u32>(bytes[0])
                | (static_cast<u32>(bytes[1]) << 8)
                | (static_cast<u32>(bytes[2]) << 16)
                | (static_cast<u32>(bytes[3]) << 24);
        }

        int HexValue(char character);

        class StrictJsonParser
        {
        public:
            StrictJsonParser(std::span<const u8> bytes,
                const LocalPackageSnapshotOptions& options, std::string& error)
                : m_Bytes(bytes), m_Options(options), m_Error(error)
            {
            }

            bool ParseDocument()
            {
                SkipWhitespace();
                if (m_Position >= m_Bytes.size() || m_Bytes[m_Position] != '{')
                    return Fail("glTF JSON must contain exactly one top-level object");
                if (!ParseObject(0))
                    return false;
                SkipWhitespace();
                return m_Position == m_Bytes.size()
                    || Fail("glTF JSON has non-whitespace trailing bytes");
            }

        private:
            bool Fail(std::string_view message)
            {
                m_Error = message;
                return false;
            }

            bool CheckCancellation()
            {
                if (m_Position < m_NextCancellationByte)
                    return true;
                m_NextCancellationByte = m_Position + 64 * 1024;
                if (m_Options.IsCancelled && m_Options.IsCancelled())
                    return Fail("local package snapshot was cancelled");
                return true;
            }

            void SkipWhitespace()
            {
                while (m_Position < m_Bytes.size())
                {
                    const u8 value = m_Bytes[m_Position];
                    if (value != ' ' && value != '\t' && value != '\r' && value != '\n')
                        break;
                    ++m_Position;
                }
            }

            bool Consume(u8 expected)
            {
                if (m_Position >= m_Bytes.size() || m_Bytes[m_Position] != expected)
                    return false;
                ++m_Position;
                return true;
            }

            bool ParseValue(size_t depth)
            {
                if (depth > 256)
                    return Fail("glTF JSON nesting exceeds the supported bound");
                if (!CheckCancellation())
                    return false;
                SkipWhitespace();
                if (m_Position >= m_Bytes.size())
                    return Fail("glTF JSON value is truncated");
                switch (m_Bytes[m_Position])
                {
                    case '{': return ParseObject(depth);
                    case '[': return ParseArray(depth);
                    case '"': return ParseString();
                    case 't': return MatchLiteral("true");
                    case 'f': return MatchLiteral("false");
                    case 'n': return MatchLiteral("null");
                    default: return ParseNumber();
                }
            }

            bool ParseObject(size_t depth)
            {
                if (!Consume('{'))
                    return Fail("glTF JSON object is malformed");
                SkipWhitespace();
                if (Consume('}'))
                    return true;
                while (true)
                {
                    if (!ParseString())
                        return false;
                    SkipWhitespace();
                    if (!Consume(':'))
                        return Fail("glTF JSON object member is missing a colon");
                    if (!ParseValue(depth + 1))
                        return false;
                    SkipWhitespace();
                    if (Consume('}'))
                        return true;
                    if (!Consume(','))
                        return Fail("glTF JSON object members are not comma-separated");
                    SkipWhitespace();
                    if (m_Position < m_Bytes.size() && m_Bytes[m_Position] == '}')
                        return Fail("glTF JSON object has a trailing comma");
                }
            }

            bool ParseArray(size_t depth)
            {
                if (!Consume('['))
                    return Fail("glTF JSON array is malformed");
                SkipWhitespace();
                if (Consume(']'))
                    return true;
                while (true)
                {
                    if (!ParseValue(depth + 1))
                        return false;
                    SkipWhitespace();
                    if (Consume(']'))
                        return true;
                    if (!Consume(','))
                        return Fail("glTF JSON array elements are not comma-separated");
                    SkipWhitespace();
                    if (m_Position < m_Bytes.size() && m_Bytes[m_Position] == ']')
                        return Fail("glTF JSON array has a trailing comma");
                }
            }

            bool ParseHexCodeUnit(u32& codeUnit)
            {
                if (m_Position + 4 > m_Bytes.size())
                    return Fail("glTF JSON Unicode escape is truncated");
                codeUnit = 0;
                for (size_t index = 0; index < 4; ++index)
                {
                    const int digit = HexValue(static_cast<char>(m_Bytes[m_Position++]));
                    if (digit < 0)
                        return Fail("glTF JSON Unicode escape is malformed");
                    codeUnit = (codeUnit << 4) | static_cast<u32>(digit);
                }
                return true;
            }

            bool ParseRawUtf8()
            {
                const u8 first = m_Bytes[m_Position];
                size_t length = 0;
                if (first >= 0xc2 && first <= 0xdf) length = 2;
                else if (first >= 0xe0 && first <= 0xef) length = 3;
                else if (first >= 0xf0 && first <= 0xf4) length = 4;
                else return Fail("glTF JSON contains malformed UTF-8");
                if (m_Position + length > m_Bytes.size())
                    return Fail("glTF JSON contains truncated UTF-8");
                const u8 second = m_Bytes[m_Position + 1];
                if ((second & 0xc0) != 0x80
                    || (first == 0xe0 && second < 0xa0)
                    || (first == 0xed && second > 0x9f)
                    || (first == 0xf0 && second < 0x90)
                    || (first == 0xf4 && second > 0x8f))
                    return Fail("glTF JSON contains noncanonical UTF-8");
                for (size_t index = 2; index < length; ++index)
                    if ((m_Bytes[m_Position + index] & 0xc0) != 0x80)
                        return Fail("glTF JSON contains malformed UTF-8");
                m_Position += length;
                return true;
            }

            bool ParseString()
            {
                if (!Consume('"'))
                    return Fail("glTF JSON object key or string is malformed");
                while (m_Position < m_Bytes.size())
                {
                    if (!CheckCancellation())
                        return false;
                    const u8 value = m_Bytes[m_Position++];
                    if (value == '"')
                        return true;
                    if (value < 0x20)
                        return Fail("glTF JSON string contains an unescaped control byte");
                    if (value >= 0x80)
                    {
                        --m_Position;
                        if (!ParseRawUtf8())
                            return false;
                        continue;
                    }
                    if (value != '\\')
                        continue;
                    if (m_Position >= m_Bytes.size())
                        return Fail("glTF JSON string escape is truncated");
                    const u8 escaped = m_Bytes[m_Position++];
                    if (escaped == '"' || escaped == '\\' || escaped == '/'
                        || escaped == 'b' || escaped == 'f' || escaped == 'n'
                        || escaped == 'r' || escaped == 't')
                        continue;
                    if (escaped != 'u')
                        return Fail("glTF JSON string escape is malformed");
                    u32 firstCodeUnit = 0;
                    if (!ParseHexCodeUnit(firstCodeUnit))
                        return false;
                    if (firstCodeUnit >= 0xd800 && firstCodeUnit <= 0xdbff)
                    {
                        if (!Consume('\\') || !Consume('u'))
                            return Fail("glTF JSON high surrogate is missing its low surrogate");
                        u32 secondCodeUnit = 0;
                        if (!ParseHexCodeUnit(secondCodeUnit))
                            return false;
                        if (secondCodeUnit < 0xdc00 || secondCodeUnit > 0xdfff)
                            return Fail("glTF JSON surrogate pair is malformed");
                    }
                    else if (firstCodeUnit >= 0xdc00 && firstCodeUnit <= 0xdfff)
                        return Fail("glTF JSON contains an unpaired low surrogate");
                }
                return Fail("glTF JSON string is truncated");
            }

            bool MatchLiteral(std::string_view literal)
            {
                if (m_Bytes.size() - m_Position < literal.size()
                    || !std::equal(literal.begin(), literal.end(), m_Bytes.begin() + m_Position))
                    return Fail("glTF JSON literal is malformed");
                m_Position += literal.size();
                return true;
            }

            bool ParseNumber()
            {
                const size_t start = m_Position;
                Consume('-');
                if (m_Position >= m_Bytes.size())
                    return Fail("glTF JSON number is truncated");
                if (Consume('0'))
                {
                    if (m_Position < m_Bytes.size() && m_Bytes[m_Position] >= '0'
                        && m_Bytes[m_Position] <= '9')
                        return Fail("glTF JSON number has a leading zero");
                }
                else
                {
                    if (m_Bytes[m_Position] < '1' || m_Bytes[m_Position] > '9')
                        return Fail("glTF JSON value is malformed");
                    while (m_Position < m_Bytes.size() && m_Bytes[m_Position] >= '0'
                        && m_Bytes[m_Position] <= '9')
                        ++m_Position;
                }
                if (Consume('.'))
                {
                    const size_t fraction = m_Position;
                    while (m_Position < m_Bytes.size() && m_Bytes[m_Position] >= '0'
                        && m_Bytes[m_Position] <= '9')
                        ++m_Position;
                    if (fraction == m_Position)
                        return Fail("glTF JSON fraction is empty");
                }
                if (m_Position < m_Bytes.size()
                    && (m_Bytes[m_Position] == 'e' || m_Bytes[m_Position] == 'E'))
                {
                    ++m_Position;
                    if (m_Position < m_Bytes.size()
                        && (m_Bytes[m_Position] == '+' || m_Bytes[m_Position] == '-'))
                        ++m_Position;
                    const size_t exponent = m_Position;
                    while (m_Position < m_Bytes.size() && m_Bytes[m_Position] >= '0'
                        && m_Bytes[m_Position] <= '9')
                        ++m_Position;
                    if (exponent == m_Position)
                        return Fail("glTF JSON exponent is empty");
                }
                return m_Position > start;
            }

            std::span<const u8> m_Bytes;
            const LocalPackageSnapshotOptions& m_Options;
            std::string& m_Error;
            size_t m_Position = 0;
            size_t m_NextCancellationByte = 0;
        };

        bool ValidateJsonDocument(std::span<const u8> bytes,
            const LocalPackageSnapshotOptions& options, std::string& error)
        {
            return StrictJsonParser(bytes, options, error).ParseDocument();
        }

        template<typename ReadAt>
        bool ReadSnapshotRootJsonBytes(
            u64 rootBytes,
            bool isGlb,
            ReadAt&& readAt,
            const LocalPackageSnapshotOptions& options,
            std::vector<u8>& bytes,
            u64& binaryChunkBytes,
            u32& binaryChunkCount,
            std::string& error)
        {
            binaryChunkBytes = 0;
            binaryChunkCount = 0;
            if (!isGlb)
            {
                if (rootBytes > options.Limits.MaximumGltfJsonBytes
                    || rootBytes > std::numeric_limits<size_t>::max())
                {
                    error = "glTF JSON exceeds the configured size limit";
                    return false;
                }
                bytes.resize(static_cast<size_t>(rootBytes));
                return readAt(0, bytes) && ValidateJsonDocument(bytes, options, error);
            }

            constexpr u32 kGlbMagic = 0x46546c67u;
            constexpr u32 kJsonChunk = 0x4e4f534au;
            constexpr u32 kBinaryChunk = 0x004e4942u;
            std::array<u8, 12> header {};
            if (rootBytes < 20 || !readAt(0, header)
                || ReadLittleU32(header.data()) != kGlbMagic
                || ReadLittleU32(header.data() + 4) != 2
                || static_cast<u64>(ReadLittleU32(header.data() + 8)) != rootBytes)
            {
                if (error.empty())
                    error = "GLB header is corrupt or unsupported";
                return false;
            }
            u64 offset = header.size();
            bool foundJson = false;
            while (offset < rootBytes)
            {
                std::array<u8, 8> chunkHeader {};
                if (rootBytes - offset < chunkHeader.size() || !readAt(offset, chunkHeader))
                {
                    if (error.empty()) error = "GLB chunk header is truncated";
                    return false;
                }
                const u64 chunkBytes = ReadLittleU32(chunkHeader.data());
                const u32 chunkType = ReadLittleU32(chunkHeader.data() + 4);
                offset += chunkHeader.size();
                if (chunkBytes % 4 != 0 || chunkBytes > rootBytes - offset)
                {
                    error = "GLB chunk exceeds the declared package root";
                    return false;
                }
                if (!foundJson)
                {
                    if (chunkType != kJsonChunk)
                    {
                        error = "GLB first chunk is not JSON";
                        return false;
                    }
                    if (chunkBytes > options.Limits.MaximumGltfJsonBytes
                        || chunkBytes > std::numeric_limits<size_t>::max())
                    {
                        error = "GLB JSON chunk exceeds the configured size limit";
                        return false;
                    }
                    bytes.resize(static_cast<size_t>(chunkBytes));
                    if (!readAt(offset, bytes))
                        return false;
                    foundJson = true;
                }
                else if (chunkType == kJsonChunk)
                {
                    error = "GLB contains duplicate JSON chunks";
                    return false;
                }
                else if (chunkType == kBinaryChunk)
                {
                    if (++binaryChunkCount != 1)
                    {
                        error = "GLB contains duplicate binary chunks";
                        return false;
                    }
                    binaryChunkBytes = chunkBytes;
                }
                offset += chunkBytes;
            }
            if (!foundJson || offset != rootBytes)
            {
                error = "GLB chunk table is incomplete";
                return false;
            }
            return ValidateJsonDocument(bytes, options, error);
        }

#if defined(GE_PLATFORM_LINUX)
        bool ReadAtExactly(int descriptor, u64 offset, std::span<u8> bytes,
            const LocalPackageSnapshotOptions& options, std::string& error)
        {
            size_t completed = 0;
            while (completed < bytes.size())
            {
                const ssize_t count = pread(descriptor, bytes.data() + completed,
                    bytes.size() - completed, static_cast<off_t>(offset + completed));
                if (count < 0 && errno == EINTR)
                    continue;
                if (count <= 0)
                {
                    error = "snapshot root could not be read completely";
                    return false;
                }
                completed += static_cast<size_t>(count);
                if (options.IsCancelled && options.IsCancelled())
                {
                    error = "local package snapshot was cancelled";
                    return false;
                }
            }
            return true;
        }

        FileDescriptor OpenSnapshotFile(
            int stagingDescriptor,
            const std::vector<std::string>& segments,
            std::string& error)
        {
            FileDescriptor parent(DuplicateCloexec(stagingDescriptor));
            if (!parent)
            {
                error = "could not inspect snapshot root";
                return {};
            }
            for (size_t index = 0; index + 1 < segments.size(); ++index)
            {
                FileDescriptor child(openat(parent.Get(), segments[index].c_str(),
                    O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
                if (!child)
                {
                    error = "snapshot root path is unavailable";
                    return {};
                }
                parent = std::move(child);
            }
            FileDescriptor file(openat(parent.Get(), segments.back().c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
            if (!file)
                error = "snapshot root is unavailable";
            return file;
        }

        bool SealSnapshotDirectory(int directoryDescriptor, u32 depth, std::string& error)
        {
            if (depth > 64)
            {
                error = "snapshot directory depth exceeds the sealing bound";
                return false;
            }
            FileDescriptor duplicate(OpenDirectoryCursor(directoryDescriptor));
            if (!duplicate)
            {
                error = "could not enumerate snapshot directory while sealing";
                return false;
            }
            DIR* rawDirectory = fdopendir(duplicate.Release());
            if (!rawDirectory)
            {
                error = "could not enumerate snapshot directory while sealing";
                return false;
            }
            std::unique_ptr<DIR, DirectoryCloser> directory(rawDirectory);
            errno = 0;
            while (dirent* entry = readdir(directory.get()))
            {
                const std::string_view name(entry->d_name);
                if (name == "." || name == "..")
                    continue;
                struct stat status {};
                if (fstatat(directoryDescriptor, entry->d_name, &status, AT_SYMLINK_NOFOLLOW) != 0)
                {
                    error = "snapshot entry changed while sealing";
                    return false;
                }
                if (S_ISDIR(status.st_mode))
                {
                    FileDescriptor child(openat(directoryDescriptor, entry->d_name,
                        O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
                    if (!child || !SealSnapshotDirectory(child.Get(), depth + 1, error))
                        return false;
                }
                else if (!S_ISREG(status.st_mode) || status.st_nlink != 1
                    || (status.st_mode & 0777) != 0400)
                {
                    error = "snapshot contains a mutable or non-regular object while sealing";
                    return false;
                }
                errno = 0;
            }
            if (errno != 0 || fchmod(directoryDescriptor, 0500) != 0)
            {
                error = "could not make snapshot directory read-only";
                return false;
            }
            return true;
        }

        bool VerifyStagedSnapshot(
            int stagingDescriptor,
            const Inventory& sourceInventory,
            const std::vector<LocalPackageSnapshotEntry>& entries,
            const LocalPackageSnapshotOptions& options,
            std::string& error)
        {
            LocalPackageSnapshotOptions verificationOptions;
            verificationOptions.Limits = options.Limits;
            verificationOptions.IsCancelled = options.IsCancelled;
            ProgressState progress;
            Inventory stagedInventory;
            if (!BuildInventory(stagingDescriptor, verificationOptions, progress, stagedInventory, error)
                || stagedInventory.Files.size() != entries.size()
                || stagedInventory.Directories.size() != sourceInventory.Directories.size())
            {
                if (error.empty()) error = "sealed snapshot inventory does not match the copied package";
                return false;
            }
            for (size_t index = 0; index < sourceInventory.Directories.size(); ++index)
            {
                if (stagedInventory.Directories[index].RelativePath
                    != sourceInventory.Directories[index].RelativePath)
                {
                    error = "sealed snapshot directory inventory changed before commit";
                    return false;
                }
            }
            for (size_t index = 0; index < entries.size(); ++index)
            {
                const InventoryRecord& record = stagedInventory.Files[index];
                const LocalPackageSnapshotEntry& expected = entries[index];
                if (record.RelativePath != expected.RelativePath || record.Identity.Size < 0
                    || static_cast<u64>(record.Identity.Size) != expected.SizeBytes
                    || (record.Identity.Mode & 0777) != 0400 || record.Identity.LinkCount != 1)
                {
                    error = "sealed snapshot file metadata changed before commit";
                    return false;
                }
                FileDescriptor file = OpenRelativeFile(stagingDescriptor, record.Segments,
                    stagedInventory.RootIdentity.Device, stagedInventory.RootIdentity.MountId, error);
                struct stat beforeStatus {};
                u64 beforeMountId = 0;
                if (!file || fstat(file.Get(), &beforeStatus) != 0
                    || !GetMountId(file.Get(), beforeMountId)
                    || !SameIdentity(record.Identity, GetIdentity(beforeStatus, beforeMountId)))
                {
                    if (error.empty()) error = "sealed snapshot file changed before verification";
                    return false;
                }
                Sha256Builder hash;
                std::array<u8, 64 * 1024> buffer {};
                u64 bytesRead = 0;
                while (true)
                {
                    const ssize_t count = read(file.Get(), buffer.data(), buffer.size());
                    if (count < 0 && errno == EINTR)
                        continue;
                    if (count < 0)
                    {
                        error = "could not verify sealed snapshot bytes";
                        return false;
                    }
                    if (count == 0)
                        break;
                    const size_t byteCount = static_cast<size_t>(count);
                    if (bytesRead > expected.SizeBytes || byteCount > expected.SizeBytes - bytesRead)
                    {
                        error = "sealed snapshot file grew before commit";
                        return false;
                    }
                    hash.Update(std::span<const u8>(buffer.data(), byteCount));
                    bytesRead += byteCount;
                    if (options.IsCancelled && options.IsCancelled())
                    {
                        error = "local package snapshot was cancelled";
                        return false;
                    }
                }
                struct stat afterStatus {};
                u64 afterMountId = 0;
                if (bytesRead != expected.SizeBytes
                    || fstat(file.Get(), &afterStatus) != 0
                    || !GetMountId(file.Get(), afterMountId)
                    || !SameIdentity(record.Identity, GetIdentity(afterStatus, afterMountId))
                    || hash.FinalizeHex() != expected.Sha256)
                {
                    error = "sealed snapshot bytes changed before commit";
                    return false;
                }
            }
            return true;
        }

        bool ReadSnapshotRootJson(
            int stagingDescriptor,
            const InventoryRecord& root,
            bool isGlb,
            const LocalPackageSnapshotOptions& options,
            std::vector<u8>& bytes,
            u64& binaryChunkBytes,
            u32& binaryChunkCount,
            std::string& error)
        {
            FileDescriptor file = OpenSnapshotFile(stagingDescriptor, root.Segments, error);
            if (!file)
                return false;
            if (root.Identity.Size < 0)
            {
                error = "snapshot root is too large for this host";
                return false;
            }
            return ReadSnapshotRootJsonBytes(static_cast<u64>(root.Identity.Size), isGlb,
                [&](u64 offset, std::span<u8> destination)
                {
                    return ReadAtExactly(file.Get(), offset, destination, options, error);
                }, options, bytes, binaryChunkBytes, binaryChunkCount, error);
        }
#endif

        int HexValue(char character)
        {
            if (character >= '0' && character <= '9') return character - '0';
            if (character >= 'a' && character <= 'f') return character - 'a' + 10;
            if (character >= 'A' && character <= 'F') return character - 'A' + 10;
            return -1;
        }

        bool ResolveDependencyUri(
            std::string_view uri,
            const std::vector<std::string>& rootSegments,
            const LocalPackageSnapshotOptions& options,
            std::string& relativePath,
            bool& embedded,
            std::string& error)
        {
            embedded = false;
            if (uri.size() >= 5 && IsAsciiCaseInsensitiveEqual(uri.substr(0, 5), "data:"))
            {
                error = "data URI dependencies are not admitted by the first package profile";
                return false;
            }
            if (uri.empty() || uri.front() == '/' || uri.find('\\') != std::string_view::npos
                || uri.find('?') != std::string_view::npos || uri.find('#') != std::string_view::npos)
            {
                error = "glTF dependency URI is empty, absolute, or contains a query, fragment, or backslash";
                return false;
            }
            const size_t slash = uri.find('/');
            const size_t colon = uri.find(':');
            if (colon != std::string_view::npos && (slash == std::string_view::npos || colon < slash))
            {
                error = "glTF dependency URI schemes are not allowed";
                return false;
            }

            std::vector<std::string> segments(rootSegments.begin(), rootSegments.end() - 1);
            size_t offset = 0;
            while (offset <= uri.size())
            {
                const size_t separator = uri.find('/', offset);
                const std::string_view encoded = uri.substr(offset,
                    separator == std::string_view::npos ? uri.size() - offset : separator - offset);
                std::string decoded;
                decoded.reserve(encoded.size());
                for (size_t index = 0; index < encoded.size(); ++index)
                {
                    unsigned char character = static_cast<unsigned char>(encoded[index]);
                    if (character == '%')
                    {
                        if (index + 2 >= encoded.size())
                        {
                            error = "glTF dependency URI has invalid percent encoding";
                            return false;
                        }
                        const int high = HexValue(encoded[index + 1]);
                        const int low = HexValue(encoded[index + 2]);
                        if (high < 0 || low < 0)
                        {
                            error = "glTF dependency URI has invalid percent encoding";
                            return false;
                        }
                        character = static_cast<unsigned char>((high << 4) | low);
                        index += 2;
                        if (character == '/' || character == '\\')
                        {
                            error = "glTF dependency URI contains an encoded separator";
                            return false;
                        }
                    }
                    decoded.push_back(static_cast<char>(character));
                }
                if (decoded == "." || decoded == "..")
                {
                    error = "glTF dependency URI contains traversal";
                    return false;
                }
                if (!ValidateSegment(decoded, options.Limits.MaximumSegmentBytes, error))
                    return false;
                segments.push_back(std::move(decoded));
                if (separator == std::string_view::npos)
                    break;
                offset = separator + 1;
            }
            relativePath = JoinSegments(segments);
            if (relativePath.size() > options.Limits.MaximumPathBytes
                || segments.size() > options.Limits.MaximumDepth)
            {
                error = "glTF dependency URI exceeds package path limits";
                return false;
            }
            return true;
        }

        template<typename InventoryType, typename InventoryRecordType, typename ReadRoot>
        bool ValidateDependencyClosure(
            const InventoryType& inventory,
            const InventoryRecordType& root,
            ReadRoot&& readRoot,
            const LocalPackageSnapshotOptions& options,
            std::string& error)
        {
            const std::string foldedRoot = AsciiCaseFold(root.RelativePath);
            const bool isGlb = foldedRoot.size() >= 4 && foldedRoot.substr(foldedRoot.size() - 4) == ".glb";
            std::vector<u8> bytes;
            u64 binaryChunkBytes = 0;
            u32 binaryChunkCount = 0;
            if (!readRoot(root, isGlb, options,
                bytes, binaryChunkBytes, binaryChunkCount, error))
                return false;

            cgltf_options parseOptions {};
            cgltf_data* rawDocument = nullptr;
            const cgltf_result parseResult = cgltf_parse(
                &parseOptions, bytes.data(), bytes.size(), &rawDocument);
            if (parseResult != cgltf_result_success || !rawDocument)
            {
                if (rawDocument)
                    cgltf_free(rawDocument);
                error = isGlb ? "GLB payload is corrupt" : "glTF JSON is corrupt";
                return false;
            }
            std::unique_ptr<cgltf_data, decltype(&cgltf_free)> document(rawDocument, cgltf_free);
            if (document->file_type != cgltf_file_type_gltf || !document->asset.version
                || std::strcmp(document->asset.version, "2.0") != 0
                || cgltf_validate(document.get()) != cgltf_result_success)
            {
                error = "glTF structure is invalid";
                return false;
            }

            std::unordered_map<std::string, u64> files;
            files.reserve(inventory.Files.size());
            for (const auto& file : inventory.Files)
                files.emplace(file.RelativePath, static_cast<u64>(file.Identity.Size));
            const auto validateUri = [&](char* rawUri, std::optional<u64> minimumBytes = std::nullopt) -> bool
            {
                if (!rawUri)
                    return true;
                const cgltf_size decodedSize = cgltf_decode_string(rawUri);
                if (std::strlen(rawUri) != decodedSize)
                {
                    error = "glTF dependency URI decodes to an embedded NUL byte";
                    return false;
                }
                std::string relativePath;
                bool embedded = false;
                if (!ResolveDependencyUri(rawUri, root.Segments, options, relativePath, embedded, error))
                    return false;
                const auto found = files.find(relativePath);
                if (!embedded && found == files.end())
                {
                    error = "glTF dependency is missing from the immutable snapshot";
                    return false;
                }
                if (!embedded && minimumBytes && found->second < *minimumBytes)
                {
                    error = "glTF buffer dependency is shorter than its declared byteLength";
                    return false;
                }
                return true;
            };
            bool consumedBinaryChunk = false;
            for (cgltf_size index = 0; index < document->buffers_count; ++index)
            {
                char* uri = document->buffers[index].uri;
                if (!uri)
                {
                    if (!isGlb || index != 0 || consumedBinaryChunk || binaryChunkCount != 1
                        || binaryChunkBytes < document->buffers[index].size)
                    {
                        error = "glTF buffer has no valid GLB binary chunk or external dependency URI";
                        return false;
                    }
                    consumedBinaryChunk = true;
                    continue;
                }
                if (!validateUri(uri, static_cast<u64>(document->buffers[index].size)))
                    return false;
            }
            if (binaryChunkCount != 0 && !consumedBinaryChunk)
            {
                error = "GLB binary chunk is not referenced by its first buffer";
                return false;
            }
            for (cgltf_size index = 0; index < document->images_count; ++index)
            {
                const cgltf_image& image = document->images[index];
                if (!image.uri && !image.buffer_view)
                {
                    error = "glTF image has neither a dependency URI nor an embedded buffer view";
                    return false;
                }
                if (!validateUri(image.uri))
                    return false;
            }
            return true;
        }

        void AppendU64(Sha256Builder& hash, u64 value)
        {
            std::array<u8, 8> bytes {};
            for (size_t index = 0; index < bytes.size(); ++index)
                bytes[bytes.size() - 1 - index] = static_cast<u8>(value >> (index * 8));
            hash.Update(bytes);
        }

        std::string ComputeTreeHash(
            const std::vector<LocalPackageSnapshotEntry>& entries,
            const std::vector<Sha256Builder::Digest>& digests)
        {
            Sha256Builder hash;
            constexpr std::array<u8, 25> domain = {
                'S','p','i','r','a','l','L','o','c','a','l','P','a','c','k','a','g','e','T','r','e','e','V','1',0
            };
            hash.Update(domain);
            AppendU64(hash, static_cast<u64>(entries.size()));
            for (size_t index = 0; index < entries.size(); ++index)
            {
                AppendU64(hash, static_cast<u64>(entries[index].RelativePath.size()));
                hash.Update(entries[index].RelativePath);
                AppendU64(hash, entries[index].SizeBytes);
                hash.Update(digests[index]);
            }
            return hash.FinalizeHex();
        }

        bool HasRootExtension(std::string_view path)
        {
            const std::string folded = AsciiCaseFold(path);
            return (folded.size() >= 5 && folded.substr(folded.size() - 5) == ".gltf")
                || (folded.size() >= 4 && folded.substr(folded.size() - 4) == ".glb");
        }

#if defined(GE_PLATFORM_WINDOWS)
        class WindowsHandle
        {
        public:
            WindowsHandle() = default;
            explicit WindowsHandle(HANDLE handle) : m_Handle(handle) {}
            ~WindowsHandle() { Reset(); }

            WindowsHandle(const WindowsHandle&) = delete;
            WindowsHandle& operator=(const WindowsHandle&) = delete;
            WindowsHandle(WindowsHandle&& other) noexcept
                : m_Handle(std::exchange(other.m_Handle, INVALID_HANDLE_VALUE)) {}
            WindowsHandle& operator=(WindowsHandle&& other) noexcept
            {
                if (this != &other)
                {
                    Reset();
                    m_Handle = std::exchange(other.m_Handle, INVALID_HANDLE_VALUE);
                }
                return *this;
            }

            HANDLE Get() const { return m_Handle; }
            explicit operator bool() const
            {
                return m_Handle && m_Handle != INVALID_HANDLE_VALUE;
            }
            HANDLE Release() { return std::exchange(m_Handle, INVALID_HANDLE_VALUE); }

        private:
            void Reset()
            {
                if (*this)
                    ::CloseHandle(m_Handle);
                m_Handle = INVALID_HANDLE_VALUE;
            }

            HANDLE m_Handle = INVALID_HANDLE_VALUE;
        };

        struct WindowsFileIdentity
        {
            u64 VolumeSerial = 0;
            FILE_ID_128 FileId {};
            u64 Size = 0;
            u64 LinkCount = 0;
            DWORD Attributes = 0;
            i64 LastWriteTime = 0;
            i64 ChangeTime = 0;
        };

        struct WindowsInventoryRecord
        {
            std::string RelativePath;
            std::vector<std::string> Segments;
            WindowsFileIdentity Identity;
            bool Directory = false;
        };

        struct WindowsInventory
        {
            WindowsFileIdentity RootIdentity;
            std::vector<WindowsInventoryRecord> Files;
            std::vector<WindowsInventoryRecord> Directories;
            u64 AggregateBytes = 0;
        };

        struct WindowsSnapshotPayload
        {
            ~WindowsSnapshotPayload()
            {
                CleanupSnapshotDirectoryWindows(DirectoryHandle);
                if (DirectoryHandle)
                    ::CloseHandle(DirectoryHandle);
                if (ParentHandle)
                    ::CloseHandle(ParentHandle);
            }

            std::filesystem::path Directory;
            std::vector<LocalPackageSnapshotEntry> Entries;
            std::string TreeSha256;
            std::string RootRelativePath;
            HANDLE ParentHandle = nullptr;
            HANDLE DirectoryHandle = nullptr;
            std::string StagingName;
        };

        WindowsHandle OpenWindowsPath(
            const std::filesystem::path& path,
            DWORD access,
            DWORD creation,
            DWORD flags)
        {
            WindowsHandle handle(::CreateFileW(path.c_str(), access,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                nullptr, creation, flags, nullptr));
            if (handle)
                ::SetHandleInformation(handle.Get(), HANDLE_FLAG_INHERIT, 0);
            return handle;
        }

        // The Win32 path APIs below are deliberately used only to bootstrap a
        // volume root.  Every object below an accepted root is opened with the
        // parent handle as RootDirectory; converting a handle back into a DOS
        // name would re-introduce an ancestor replacement race.
        using NtCreateFileFn = NTSTATUS (NTAPI *)(PHANDLE, ACCESS_MASK,
            POBJECT_ATTRIBUTES, PIO_STATUS_BLOCK, PLARGE_INTEGER, ULONG,
            ULONG, ULONG, ULONG, PVOID, ULONG);

        NtCreateFileFn GetNtCreateFile()
        {
            static NtCreateFileFn function = []() -> NtCreateFileFn
            {
                HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");
                return ntdll ? reinterpret_cast<NtCreateFileFn>(
                    ::GetProcAddress(ntdll, "NtCreateFile")) : nullptr;
            }();
            return function;
        }

        WindowsHandle OpenWindowsChildNative(HANDLE parent, std::wstring_view name,
            ACCESS_MASK access, ULONG disposition, bool directory, ULONG share = FILE_SHARE_READ)
        {
            NtCreateFileFn create = GetNtCreateFile();
            if (!create || !parent || parent == INVALID_HANDLE_VALUE || name.empty()
                || name.find(L'\\') != std::wstring_view::npos
                || name.find(L'/') != std::wstring_view::npos)
                return {};
            UNICODE_STRING childName {};
            childName.Buffer = const_cast<PWSTR>(name.data());
            childName.Length = static_cast<USHORT>(name.size() * sizeof(wchar_t));
            childName.MaximumLength = childName.Length;
            OBJECT_ATTRIBUTES attributes {};
            InitializeObjectAttributes(&attributes, &childName,
                OBJ_CASE_INSENSITIVE, parent, nullptr);
            IO_STATUS_BLOCK io {};
            HANDLE handle = INVALID_HANDLE_VALUE;
            const ULONG options = FILE_SYNCHRONOUS_IO_NONALERT | FILE_OPEN_REPARSE_POINT
                | (directory ? FILE_DIRECTORY_FILE : FILE_NON_DIRECTORY_FILE);
            const NTSTATUS status = create(&handle, access | SYNCHRONIZE, &attributes, &io,
                nullptr, FILE_ATTRIBUTE_NORMAL, share, disposition, options, nullptr, 0);
            if (status < 0 || handle == INVALID_HANDLE_VALUE)
                return {};
            ::SetHandleInformation(handle, HANDLE_FLAG_INHERIT, 0);
            return WindowsHandle(handle);
        }

        bool GetWindowsIdentity(HANDLE handle, WindowsFileIdentity& identity)
        {
            if (!handle || handle == INVALID_HANDLE_VALUE || ::GetFileType(handle) != FILE_TYPE_DISK)
                return false;
            FILE_ID_INFO id {};
            FILE_STANDARD_INFO standard {};
            FILE_BASIC_INFO basic {};
            if (!::GetFileInformationByHandleEx(handle, FileIdInfo, &id, sizeof(id))
                || !::GetFileInformationByHandleEx(handle, FileStandardInfo, &standard, sizeof(standard))
                || !::GetFileInformationByHandleEx(handle, FileBasicInfo, &basic, sizeof(basic))
                || standard.EndOfFile.QuadPart < 0)
                return false;
            identity.VolumeSerial = id.VolumeSerialNumber;
            identity.FileId = id.FileId;
            identity.Size = static_cast<u64>(standard.EndOfFile.QuadPart);
            identity.LinkCount = standard.NumberOfLinks;
            identity.Attributes = basic.FileAttributes;
            identity.LastWriteTime = basic.LastWriteTime.QuadPart;
            identity.ChangeTime = basic.ChangeTime.QuadPart;
            return true;
        }

        bool SameWindowsIdentity(const WindowsFileIdentity& left, const WindowsFileIdentity& right)
        {
            return left.VolumeSerial == right.VolumeSerial
                && std::memcmp(&left.FileId, &right.FileId, sizeof(left.FileId)) == 0
                && left.Size == right.Size
                && left.LinkCount == right.LinkCount
                && left.Attributes == right.Attributes
                && left.LastWriteTime == right.LastWriteTime
                && left.ChangeTime == right.ChangeTime;
        }

        bool SameWindowsObject(const WindowsFileIdentity& left, const WindowsFileIdentity& right)
        {
            return left.VolumeSerial == right.VolumeSerial
                && std::memcmp(&left.FileId, &right.FileId, sizeof(left.FileId)) == 0;
        }

        bool IsWindowsDirectory(const WindowsFileIdentity& identity)
        {
            return (identity.Attributes & FILE_ATTRIBUTE_DIRECTORY) != 0
                && (identity.Attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0;
        }

        bool IsWindowsRegularFile(const WindowsFileIdentity& identity)
        {
            return (identity.Attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT
                | FILE_ATTRIBUTE_DEVICE | FILE_ATTRIBUTE_OFFLINE)) == 0;
        }

        bool HasOnlyDefaultWindowsDataStream(HANDLE handle)
        {
            // FileStreamInformation is handle-native.  Filesystems which do
            // not implement it are rejected: treating an unsupported query as
            // "no ADS" was a fail-open boundary.
            std::array<u8, 64 * 1024> bytes {};
            if (!::GetFileInformationByHandleEx(handle, FileStreamInfo,
                bytes.data(), static_cast<DWORD>(bytes.size())))
                return false;
            const auto* stream = reinterpret_cast<const FILE_STREAM_INFO*>(bytes.data());
            size_t offset = 0;
            u32 count = 0;
            while (true)
            {
                if (offset > bytes.size() - sizeof(FILE_STREAM_INFO)
                    || stream->StreamNameLength > bytes.size() - offset - offsetof(FILE_STREAM_INFO, StreamName)
                    || stream->StreamSize.QuadPart < 0)
                    return false;
                if (++count != 1 || stream->StreamNameLength != 7 * sizeof(wchar_t)
                    || std::wstring_view(stream->StreamName,
                        stream->StreamNameLength / sizeof(wchar_t)) != L"::$DATA")
                    return false;
                if (stream->NextEntryOffset == 0)
                    return true;
                if (stream->NextEntryOffset < sizeof(FILE_STREAM_INFO)
                    || stream->NextEntryOffset > bytes.size() - offset)
                    return false;
                offset += stream->NextEntryOffset;
                stream = reinterpret_cast<const FILE_STREAM_INFO*>(bytes.data() + offset);
            }
        }

        bool WindowsPathIsWithin(
            const std::filesystem::path& parentPath,
            const std::filesystem::path& childPath)
        {
            std::wstring parent = parentPath.wstring();
            std::wstring child = childPath.wstring();
            while (parent.size() > 4 && (parent.back() == L'\\' || parent.back() == L'/'))
                parent.pop_back();
            if (child.size() < parent.size()
                || ::CompareStringOrdinal(parent.data(), static_cast<int>(parent.size()),
                    child.data(), static_cast<int>(parent.size()), TRUE) != CSTR_EQUAL)
                return false;
            return child.size() == parent.size()
                || child[parent.size()] == L'\\' || child[parent.size()] == L'/';
        }

        bool GetCurrentUserSidBytes(std::vector<u8>& sidBytes)
        {
            WindowsHandle token;
            HANDLE rawToken = nullptr;
            if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &rawToken))
                return false;
            token = WindowsHandle(rawToken);
            DWORD required = 0;
            ::GetTokenInformation(token.Get(), TokenUser, nullptr, 0, &required);
            if (required == 0 || ::GetLastError() != ERROR_INSUFFICIENT_BUFFER)
                return false;
            std::vector<u8> tokenBytes(required);
            if (!::GetTokenInformation(token.Get(), TokenUser,
                tokenBytes.data(), required, &required))
                return false;
            const auto* user = reinterpret_cast<const TOKEN_USER*>(tokenBytes.data());
            const DWORD sidLength = ::GetLengthSid(user->User.Sid);
            sidBytes.resize(sidLength);
            return ::CopySid(sidLength, sidBytes.data(), user->User.Sid) != FALSE;
        }

        bool IsPrivateWindowsDirectory(HANDLE handle)
        {
            std::vector<u8> currentUser;
            if (!GetCurrentUserSidBytes(currentUser))
                return false;
            PSID owner = nullptr;
            PACL dacl = nullptr;
            PSECURITY_DESCRIPTOR descriptor = nullptr;
            const DWORD result = ::GetSecurityInfo(handle, SE_FILE_OBJECT,
                OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
                &owner, nullptr, &dacl, nullptr, &descriptor);
            const auto releaseDescriptor = [&]()
            {
                if (descriptor)
                    ::LocalFree(descriptor);
            };
            if (result != ERROR_SUCCESS || !owner || !dacl
                || !::EqualSid(owner, currentUser.data()))
            {
                releaseDescriptor();
                return false;
            }

            std::array<u8, SECURITY_MAX_SID_SIZE> systemSid {};
            std::array<u8, SECURITY_MAX_SID_SIZE> administratorsSid {};
            std::array<u8, SECURITY_MAX_SID_SIZE> creatorOwnerSid {};
            DWORD systemBytes = static_cast<DWORD>(systemSid.size());
            DWORD administratorsBytes = static_cast<DWORD>(administratorsSid.size());
            DWORD creatorBytes = static_cast<DWORD>(creatorOwnerSid.size());
            if (!::CreateWellKnownSid(WinLocalSystemSid, nullptr, systemSid.data(), &systemBytes)
                || !::CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr,
                    administratorsSid.data(), &administratorsBytes)
                || !::CreateWellKnownSid(WinCreatorOwnerSid, nullptr,
                    creatorOwnerSid.data(), &creatorBytes))
            {
                releaseDescriptor();
                return false;
            }

            bool privateDirectory = true;
            ACL_SIZE_INFORMATION information {};
            if (!::GetAclInformation(dacl, &information, sizeof(information), AclSizeInformation))
                privateDirectory = false;
            for (DWORD index = 0; privateDirectory && index < information.AceCount; ++index)
            {
                void* rawAce = nullptr;
                if (!::GetAce(dacl, index, &rawAce))
                {
                    privateDirectory = false;
                    break;
                }
                const auto* header = static_cast<const ACE_HEADER*>(rawAce);
                if (header->AceType == ACCESS_ALLOWED_OBJECT_ACE_TYPE
                    || header->AceType == ACCESS_ALLOWED_CALLBACK_ACE_TYPE
                    || header->AceType == ACCESS_ALLOWED_CALLBACK_OBJECT_ACE_TYPE)
                {
                    privateDirectory = false;
                    break;
                }
                if (header->AceType != ACCESS_ALLOWED_ACE_TYPE)
                    continue;
                const auto* ace = static_cast<const ACCESS_ALLOWED_ACE*>(rawAce);
                PSID sid = const_cast<DWORD*>(&ace->SidStart);
                if (!::EqualSid(sid, currentUser.data())
                    && !::EqualSid(sid, systemSid.data())
                    && !::EqualSid(sid, administratorsSid.data())
                    && !::EqualSid(sid, creatorOwnerSid.data()))
                    privateDirectory = false;
            }
            releaseDescriptor();
            return privateDirectory;
        }

        bool SetWindowsOwnerAccess(HANDLE handle, DWORD permissions)
        {
            std::vector<u8> currentUser;
            if (!GetCurrentUserSidBytes(currentUser))
                return false;
            EXPLICIT_ACCESSW access {};
            access.grfAccessPermissions = permissions;
            access.grfAccessMode = SET_ACCESS;
            access.grfInheritance = NO_INHERITANCE;
            ::BuildTrusteeWithSidW(&access.Trustee, currentUser.data());
            PACL dacl = nullptr;
            const DWORD aclResult = ::SetEntriesInAclW(1, &access, nullptr, &dacl);
            if (aclResult != ERROR_SUCCESS || !dacl)
                return false;
            const DWORD setResult = ::SetSecurityInfo(handle, SE_FILE_OBJECT,
                DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
                nullptr, nullptr, dacl, nullptr);
            ::LocalFree(dacl);
            return setResult == ERROR_SUCCESS;
        }

        bool SetWindowsReadOnly(HANDLE handle, bool directory)
        {
            FILE_BASIC_INFO basic {};
            if (!::GetFileInformationByHandleEx(handle, FileBasicInfo, &basic, sizeof(basic)))
                return false;
            basic.FileAttributes |= FILE_ATTRIBUTE_READONLY;
            if (!::SetFileInformationByHandle(handle, FileBasicInfo, &basic, sizeof(basic)))
                return false;
            DWORD access = FILE_GENERIC_READ | READ_CONTROL | SYNCHRONIZE;
            if (directory)
                access |= FILE_LIST_DIRECTORY | FILE_TRAVERSE;
            return SetWindowsOwnerAccess(handle, access);
        }

        bool ClearWindowsReadOnlyAndGrantDelete(HANDLE handle)
        {
            if (!SetWindowsOwnerAccess(handle, GENERIC_ALL))
                return false;
            FILE_BASIC_INFO basic {};
            if (!::GetFileInformationByHandleEx(handle, FileBasicInfo, &basic, sizeof(basic)))
                return false;
            basic.FileAttributes &= ~FILE_ATTRIBUTE_READONLY;
            if (basic.FileAttributes == 0)
                basic.FileAttributes = FILE_ATTRIBUTE_NORMAL;
            return ::SetFileInformationByHandle(handle, FileBasicInfo, &basic, sizeof(basic)) != FALSE;
        }

        bool MarkWindowsHandleForDeletion(HANDLE handle)
        {
            FILE_DISPOSITION_INFO disposition { TRUE };
            return ::SetFileInformationByHandle(
                handle, FileDispositionInfo, &disposition, sizeof(disposition)) != FALSE;
        }

        bool EnumerateWindowsNames(HANDLE directory, std::vector<std::wstring>& names);

        bool CleanupWindowsContents(HANDLE directoryHandle, u32 depth)
        {
            if (!directoryHandle || directoryHandle == INVALID_HANDLE_VALUE || depth > 64)
                return false;
            std::vector<std::wstring> names;
            if (!EnumerateWindowsNames(directoryHandle, names))
                return false;
            bool success = true;
            for (const std::wstring& name : names)
            {
                if (name == L"." || name == L"..")
                    continue;
                WindowsHandle child = OpenWindowsChildNative(directoryHandle, name,
                    GENERIC_READ | FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES
                        | FILE_WRITE_ATTRIBUTES | READ_CONTROL | WRITE_DAC | DELETE | SYNCHRONIZE,
                    FILE_OPEN, true, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE);
                if (!child)
                    child = OpenWindowsChildNative(directoryHandle, name,
                        GENERIC_READ | FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES
                            | READ_CONTROL | WRITE_DAC | DELETE | SYNCHRONIZE,
                        FILE_OPEN, false, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE);
                WindowsFileIdentity identity;
                if (!child || !GetWindowsIdentity(child.Get(), identity)
                    || identity.LinkCount != 1)
                {
                    success = false;
                    continue;
                }
                if (IsWindowsDirectory(identity)
                    && !CleanupWindowsContents(child.Get(), depth + 1))
                    success = false;
                if (!ClearWindowsReadOnlyAndGrantDelete(child.Get())
                    || !MarkWindowsHandleForDeletion(child.Get()))
                    success = false;
            }
            return success;
        }

        void CleanupSnapshotDirectoryWindows(HANDLE directoryHandle) noexcept
        {
            if (!directoryHandle || directoryHandle == INVALID_HANDLE_VALUE)
                return;
            try
            {
                CleanupWindowsContents(directoryHandle, 0);
                if (ClearWindowsReadOnlyAndGrantDelete(directoryHandle))
                    MarkWindowsHandleForDeletion(directoryHandle);
            }
            catch (...)
            {
            }
        }
#endif

#if defined(GE_PLATFORM_WINDOWS)
        WindowsHandle OpenWindowsAbsoluteDirectoryNoFollow(
            const std::filesystem::path& input, DWORD access, ULONG childShare)
        {
            if (input.empty())
                return {};
            std::error_code filesystemError;
            const std::filesystem::path absolute =
                std::filesystem::absolute(input, filesystemError).lexically_normal();
            if (filesystemError || !absolute.is_absolute() || absolute.root_path().empty())
                return {};

            WindowsHandle current = OpenWindowsPath(absolute.root_path(), access,
                OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT);
            WindowsFileIdentity currentIdentity;
            if (!current || !GetWindowsIdentity(current.Get(), currentIdentity)
                || !IsWindowsDirectory(currentIdentity))
                return {};
            for (const std::filesystem::path& componentPath : absolute.relative_path())
            {
                const std::wstring component = componentPath.wstring();
                if (component.empty() || component == L".")
                    continue;
                if (component == L"..")
                    return {};
                WindowsHandle next = OpenWindowsChildNative(current.Get(), component, access,
                    FILE_OPEN, true, childShare);
                WindowsFileIdentity nextIdentity;
                if (!next || !GetWindowsIdentity(next.Get(), nextIdentity)
                    || !IsWindowsDirectory(nextIdentity))
                    return {};
                current = std::move(next);
            }
            return current;
        }

        WindowsHandle DuplicateWindowsHandle(HANDLE source)
        {
            HANDLE duplicate = nullptr;
            if (!source || source == INVALID_HANDLE_VALUE
                || !::DuplicateHandle(::GetCurrentProcess(), source, ::GetCurrentProcess(),
                    &duplicate, 0, FALSE, DUPLICATE_SAME_ACCESS))
                return {};
            ::SetHandleInformation(duplicate, HANDLE_FLAG_INHERIT, 0);
            return WindowsHandle(duplicate);
        }

        bool WindowsNameToPortableAscii(std::wstring_view wideName, std::string& name)
        {
            name.clear();
            name.reserve(wideName.size());
            for (wchar_t character : wideName)
            {
                if (character < 0x20 || character > 0x7e)
                    return false;
                name.push_back(static_cast<char>(character));
            }
            return true;
        }

        WindowsHandle OpenWindowsChild(
            HANDLE parent,
            std::wstring_view name,
            DWORD access,
            bool directory)
        {
            return OpenWindowsChildNative(parent, name, access, FILE_OPEN, directory);
        }

        bool EnumerateWindowsNames(HANDLE directory, std::vector<std::wstring>& names)
        {
            names.clear();
            std::array<u8, 64 * 1024> buffer {};
            bool restart = true;
            while (true)
            {
                if (!::GetFileInformationByHandleEx(directory,
                    restart ? FileIdBothDirectoryRestartInfo : FileIdBothDirectoryInfo,
                    buffer.data(), static_cast<DWORD>(buffer.size())))
                {
                    const DWORD failure = ::GetLastError();
                    return failure == ERROR_NO_MORE_FILES;
                }
                restart = false;
                size_t offset = 0;
                while (true)
                {
                    if (offset > buffer.size() - offsetof(FILE_ID_BOTH_DIR_INFO, FileName)
                        || reinterpret_cast<const FILE_ID_BOTH_DIR_INFO*>(buffer.data() + offset)->FileNameLength
                            > buffer.size() - offset - offsetof(FILE_ID_BOTH_DIR_INFO, FileName))
                        return false;
                    const auto* entry = reinterpret_cast<const FILE_ID_BOTH_DIR_INFO*>(buffer.data() + offset);
                    names.emplace_back(entry->FileName, entry->FileNameLength / sizeof(wchar_t));
                    if (entry->NextEntryOffset == 0)
                        break;
                    if (entry->NextEntryOffset < sizeof(FILE_ID_BOTH_DIR_INFO)
                        || entry->NextEntryOffset > buffer.size() - offset)
                        return false;
                    offset += entry->NextEntryOffset;
                }
            }
        }

        bool EnumerateWindowsDirectory(
            HANDLE directoryHandle,
            const std::vector<std::string>& parentSegments,
            u64 rootVolume,
            const LocalPackageSnapshotOptions& options,
            ProgressState& progress,
            WindowsInventory& inventory,
            std::unordered_set<std::string>& foldedPaths,
            u64& directoryCount,
            std::string& error)
        {
            WindowsFileIdentity beforeIdentity;
            if (!GetWindowsIdentity(directoryHandle, beforeIdentity)
                || !IsWindowsDirectory(beforeIdentity)
                || beforeIdentity.VolumeSerial != rootVolume)
            {
                error = "package directory changed identity or crossed a volume boundary";
                return false;
            }
            std::vector<std::wstring> names;
            const u64 rawEntryLimit = options.Limits.MaximumFileCount > std::numeric_limits<u64>::max() / 2
                ? std::numeric_limits<u64>::max() : options.Limits.MaximumFileCount * 2;
            if (!EnumerateWindowsNames(directoryHandle, names))
            {
                error = "could not enumerate package directory";
                return false;
            }
            if (names.size() > rawEntryLimit) { error = "package object count exceeds the configured inventory bound"; return false; }
            std::sort(names.begin(), names.end());

            for (const std::wstring& wideName : names)
            {
                std::string name;
                if (!WindowsNameToPortableAscii(wideName, name)
                    || !ValidateSegment(name, options.Limits.MaximumSegmentBytes, error))
                {
                    if (error.empty()) error = "package path contains a non-portable character";
                    return false;
                }
                std::vector<std::string> segments = parentSegments;
                segments.push_back(name);
                std::string relativePath;
                if (!AddObjectPath(segments, options.Limits, foldedPaths, relativePath, error))
                    return false;

                WindowsHandle entry = OpenWindowsChild(directoryHandle, wideName,
                    GENERIC_READ | FILE_READ_ATTRIBUTES | SYNCHRONIZE, true);
                WindowsFileIdentity entryIdentity;
                if (!entry || !GetWindowsIdentity(entry.Get(), entryIdentity))
                {
                    error = "package entry changed during inventory";
                    return false;
                }
                if ((entryIdentity.Attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
                {
                    error = "package reparse points are not allowed";
                    return false;
                }
                if (entryIdentity.VolumeSerial != rootVolume)
                {
                    error = "package entry crosses a volume boundary";
                    return false;
                }
                if (IsWindowsDirectory(entryIdentity))
                {
                    if (directoryCount == std::numeric_limits<u64>::max()
                        || ++directoryCount > options.Limits.MaximumFileCount)
                    {
                        error = "package directory count exceeds the configured file-count bound";
                        return false;
                    }
                    WindowsFileIdentity openedIdentity;
                    if (!GetWindowsIdentity(entry.Get(), openedIdentity)
                        || !SameWindowsIdentity(entryIdentity, openedIdentity))
                    {
                        error = "package directory changed during inventory";
                        return false;
                    }
                    inventory.Directories.push_back({ relativePath, segments, openedIdentity, true });
                    if (!Checkpoint(options, LocalPackageSnapshotHookPoint::InventoryEntry,
                        relativePath, progress, error)
                        || !EnumerateWindowsDirectory(entry.Get(), segments, rootVolume, options,
                            progress, inventory, foldedPaths, directoryCount, error))
                        return false;
                    continue;
                }
                if (!IsWindowsRegularFile(entryIdentity))
                {
                    error = "package contains a non-regular object";
                    return false;
                }
                WindowsFileIdentity streamCheckedIdentity;
                if (!HasOnlyDefaultWindowsDataStream(entry.Get())
                    || !GetWindowsIdentity(entry.Get(), streamCheckedIdentity)
                    || !SameWindowsIdentity(entryIdentity, streamCheckedIdentity))
                {
                    error = "package file contains an alternate stream or changed during inventory";
                    return false;
                }
                if (HasForbiddenPayloadExtension(relativePath))
                {
                    error = "package contains an executable, script, or nested archive payload";
                    return false;
                }
                if (entryIdentity.LinkCount != 1)
                {
                    error = "package hard-linked files are not allowed";
                    return false;
                }
                if (entryIdentity.Size > options.Limits.MaximumFileBytes)
                {
                    error = "package file exceeds the configured size limit";
                    return false;
                }
                if (inventory.Files.size() >= options.Limits.MaximumFileCount)
                {
                    error = "package file count exceeds the configured limit";
                    return false;
                }
                if (inventory.AggregateBytes > options.Limits.MaximumAggregateBytes
                    || entryIdentity.Size > options.Limits.MaximumAggregateBytes - inventory.AggregateBytes)
                {
                    error = "package aggregate size exceeds the configured limit";
                    return false;
                }
                inventory.AggregateBytes += entryIdentity.Size;
                inventory.Files.push_back({ relativePath, segments, entryIdentity, false });
                progress.FileCount = static_cast<u64>(inventory.Files.size());
                progress.AggregateBytes = inventory.AggregateBytes;
                if (!Checkpoint(options, LocalPackageSnapshotHookPoint::InventoryEntry,
                    relativePath, progress, error))
                    return false;
            }

            WindowsFileIdentity afterIdentity;
            if (!GetWindowsIdentity(directoryHandle, afterIdentity)
                || !SameWindowsIdentity(beforeIdentity, afterIdentity))
            {
                error = "package directory changed during inventory";
                return false;
            }
            return true;
        }

        bool BuildWindowsInventory(
            HANDLE rootHandle,
            const LocalPackageSnapshotOptions& options,
            ProgressState& progress,
            WindowsInventory& inventory,
            std::string& error)
        {
            WindowsFileIdentity rootIdentity;
            if (!GetWindowsIdentity(rootHandle, rootIdentity) || !IsWindowsDirectory(rootIdentity))
            {
                error = "selected package root is not a directory";
                return false;
            }
            inventory.RootIdentity = rootIdentity;
            std::unordered_set<std::string> foldedPaths;
            u64 directoryCount = 0;
            if (!EnumerateWindowsDirectory(rootHandle, {}, rootIdentity.VolumeSerial, options,
                progress, inventory, foldedPaths, directoryCount, error))
                return false;
            std::sort(inventory.Files.begin(), inventory.Files.end(), [](const auto& left, const auto& right)
            {
                return left.RelativePath < right.RelativePath;
            });
            std::sort(inventory.Directories.begin(), inventory.Directories.end(), [](const auto& left, const auto& right)
            {
                return left.RelativePath < right.RelativePath;
            });
            progress.FileCount = static_cast<u64>(inventory.Files.size());
            progress.AggregateBytes = inventory.AggregateBytes;
            return true;
        }

        bool SameWindowsInventory(const WindowsInventory& left, const WindowsInventory& right)
        {
            if (!SameWindowsIdentity(left.RootIdentity, right.RootIdentity)
                || left.AggregateBytes != right.AggregateBytes
                || left.Files.size() != right.Files.size()
                || left.Directories.size() != right.Directories.size())
                return false;
            const auto sameRecords = [](const auto& leftRecords, const auto& rightRecords)
            {
                for (size_t index = 0; index < leftRecords.size(); ++index)
                {
                    if (leftRecords[index].RelativePath != rightRecords[index].RelativePath
                        || leftRecords[index].Directory != rightRecords[index].Directory
                        || !SameWindowsIdentity(leftRecords[index].Identity, rightRecords[index].Identity))
                        return false;
                }
                return true;
            };
            return sameRecords(left.Files, right.Files)
                && sameRecords(left.Directories, right.Directories);
        }

        WindowsHandle OpenWindowsRelativeFile(
            HANDLE rootHandle,
            const std::vector<std::string>& segments,
            u64 rootVolume,
            DWORD access,
            std::string& error)
        {
            WindowsHandle parent = DuplicateWindowsHandle(rootHandle);
            if (!parent || segments.empty())
            {
                error = "could not reopen package root";
                return {};
            }
            for (size_t index = 0; index + 1 < segments.size(); ++index)
            {
                const std::wstring name(segments[index].begin(), segments[index].end());
                WindowsHandle child = OpenWindowsChild(parent.Get(), name,
                    GENERIC_READ | FILE_READ_ATTRIBUTES | SYNCHRONIZE, true);
                WindowsFileIdentity identity;
                if (!child || !GetWindowsIdentity(child.Get(), identity)
                    || !IsWindowsDirectory(identity)
                    || (rootVolume != 0 && identity.VolumeSerial != rootVolume))
                {
                    error = "package path changed before copying";
                    return {};
                }
                parent = std::move(child);
            }
            const std::wstring name(segments.back().begin(), segments.back().end());
            WindowsHandle file = OpenWindowsChild(parent.Get(), name, access, false);
            if (!file)
                error = "package file changed before copying";
            return file;
        }

        WindowsHandle CreateWindowsDestinationFile(
            HANDLE stagingHandle,
            const std::vector<std::string>& segments,
            u64 stagingVolume,
            std::string& error)
        {
            WindowsHandle parent = DuplicateWindowsHandle(stagingHandle);
            if (!parent || segments.empty())
            {
                error = "could not prepare snapshot destination";
                return {};
            }
            for (size_t index = 0; index + 1 < segments.size(); ++index)
            {
                const std::wstring name(segments[index].begin(), segments[index].end());
                WindowsHandle child = OpenWindowsChildNative(parent.Get(), name,
                    GENERIC_READ | FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES
                        | READ_CONTROL | WRITE_DAC | DELETE | SYNCHRONIZE,
                    FILE_CREATE, true, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE);
                if (!child)
                    child = OpenWindowsChildNative(parent.Get(), name,
                        GENERIC_READ | FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES
                            | READ_CONTROL | WRITE_DAC | DELETE | SYNCHRONIZE,
                        FILE_OPEN, true, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE);
                WindowsFileIdentity identity;
                if (!child || !GetWindowsIdentity(child.Get(), identity)
                    || !IsWindowsDirectory(identity) || identity.VolumeSerial != stagingVolume
                    || !SetWindowsOwnerAccess(child.Get(), GENERIC_ALL))
                {
                    error = "could not open a stable snapshot directory";
                    return {};
                }
                parent = std::move(child);
            }

            const std::wstring name(segments.back().begin(), segments.back().end());
            WindowsHandle file = OpenWindowsChildNative(parent.Get(), name,
                GENERIC_READ | GENERIC_WRITE | FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES
                    | READ_CONTROL | WRITE_DAC | DELETE | SYNCHRONIZE,
                FILE_CREATE, false, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE);
            if (!file)
                error = "could not create exclusive snapshot file";
            else if (!SetWindowsOwnerAccess(file.Get(), GENERIC_ALL))
            {
                error = "could not protect snapshot file access";
                return {};
            }
            return file;
        }

        bool WriteWindowsAll(HANDLE handle, const u8* bytes, size_t size, std::string& error)
        {
            size_t offset = 0;
            while (offset < size)
            {
                const DWORD requested = static_cast<DWORD>(std::min<size_t>(
                    size - offset, std::numeric_limits<DWORD>::max()));
                DWORD written = 0;
                if (!::WriteFile(handle, bytes + offset, requested, &written, nullptr)
                    || written == 0)
                {
                    error = "could not write snapshot file";
                    return false;
                }
                offset += written;
            }
            return true;
        }

        bool CopyWindowsFile(
            HANDLE sourceRoot,
            HANDLE stagingHandle,
            u64 sourceVolume,
            u64 stagingVolume,
            const WindowsInventoryRecord& record,
            const LocalPackageSnapshotOptions& options,
            ProgressState& progress,
            LocalPackageSnapshotEntry& entry,
            Sha256Builder::Digest& digest,
            std::string& error)
        {
            WindowsHandle source = OpenWindowsRelativeFile(sourceRoot, record.Segments,
                sourceVolume, GENERIC_READ | FILE_READ_ATTRIBUTES | SYNCHRONIZE, error);
            WindowsFileIdentity beforeIdentity;
            if (!source || !GetWindowsIdentity(source.Get(), beforeIdentity)
                || !SameWindowsIdentity(record.Identity, beforeIdentity)
                || !IsWindowsRegularFile(beforeIdentity) || beforeIdentity.LinkCount != 1)
            {
                if (error.empty()) error = "package file changed before copying";
                return false;
            }
            WindowsHandle destination = CreateWindowsDestinationFile(
                stagingHandle, record.Segments, stagingVolume, error);
            if (!destination)
                return false;
            if (!Checkpoint(options, LocalPackageSnapshotHookPoint::BeforeFileCopy,
                record.RelativePath, progress, error))
                return false;

            Sha256Builder hash;
            std::array<u8, 64 * 1024> buffer {};
            u64 copiedBytes = 0;
            while (true)
            {
                DWORD count = 0;
                if (!::ReadFile(source.Get(), buffer.data(),
                    static_cast<DWORD>(buffer.size()), &count, nullptr))
                {
                    error = "could not read package file";
                    return false;
                }
                if (count == 0)
                    break;
                const size_t byteCount = count;
                if (copiedBytes == 0
                    && HasForbiddenPayloadMagic(std::span<const u8>(buffer.data(), byteCount)))
                {
                    error = "package contains executable or nested-archive payload bytes";
                    return false;
                }
                if (copiedBytes > record.Identity.Size
                    || byteCount > record.Identity.Size - copiedBytes)
                {
                    error = "package file grew while being copied";
                    return false;
                }
                hash.Update(std::span<const u8>(buffer.data(), byteCount));
                if (!WriteWindowsAll(destination.Get(), buffer.data(), byteCount, error))
                    return false;
                copiedBytes += byteCount;
                progress.BytesCompleted += byteCount;
                if (options.Progress)
                {
                    options.Progress({
                        LocalPackageSnapshotHookPoint::BeforeFileCopy,
                        progress.FilesCompleted,
                        progress.FileCount,
                        progress.BytesCompleted,
                        progress.AggregateBytes,
                        record.RelativePath
                    });
                }
                if (options.IsCancelled && options.IsCancelled())
                {
                    error = "local package snapshot was cancelled";
                    return false;
                }
            }
            WindowsFileIdentity afterIdentity;
            if (copiedBytes != record.Identity.Size
                || !GetWindowsIdentity(source.Get(), afterIdentity)
                || !SameWindowsIdentity(record.Identity, afterIdentity))
            {
                error = "package file changed while being copied";
                return false;
            }
            if (!::FlushFileBuffers(destination.Get())
                || !SetWindowsReadOnly(destination.Get(), false))
            {
                error = "could not make snapshot file read-only";
                return false;
            }
            digest = hash.FinalizeBytes();
            entry = { record.RelativePath, copiedBytes, Sha256Builder::ToHex(digest) };
            ++progress.FilesCompleted;
            return Checkpoint(options, LocalPackageSnapshotHookPoint::AfterFileCopy,
                record.RelativePath, progress, error);
        }

        bool ReadWindowsAtExactly(
            HANDLE handle,
            u64 offset,
            std::span<u8> bytes,
            const LocalPackageSnapshotOptions& options,
            std::string& error)
        {
            if (offset > static_cast<u64>(std::numeric_limits<i64>::max()))
            {
                error = "snapshot root offset is too large for this host";
                return false;
            }
            LARGE_INTEGER position {};
            position.QuadPart = static_cast<i64>(offset);
            if (!::SetFilePointerEx(handle, position, nullptr, FILE_BEGIN))
            {
                error = "snapshot root could not be positioned";
                return false;
            }
            size_t completed = 0;
            while (completed < bytes.size())
            {
                const DWORD requested = static_cast<DWORD>(std::min<size_t>(
                    bytes.size() - completed, std::numeric_limits<DWORD>::max()));
                DWORD count = 0;
                if (!::ReadFile(handle, bytes.data() + completed, requested, &count, nullptr)
                    || count == 0)
                {
                    error = "snapshot root could not be read completely";
                    return false;
                }
                completed += count;
                if (options.IsCancelled && options.IsCancelled())
                {
                    error = "local package snapshot was cancelled";
                    return false;
                }
            }
            return true;
        }

        bool ReadWindowsSnapshotRootJson(
            HANDLE stagingHandle,
            u64 stagingVolume,
            const WindowsInventoryRecord& root,
            bool isGlb,
            const LocalPackageSnapshotOptions& options,
            std::vector<u8>& bytes,
            u64& binaryChunkBytes,
            u32& binaryChunkCount,
            std::string& error)
        {
            WindowsHandle file = OpenWindowsRelativeFile(stagingHandle, root.Segments,
                stagingVolume, GENERIC_READ | FILE_READ_ATTRIBUTES | SYNCHRONIZE, error);
            WindowsFileIdentity identity;
            if (!file || !GetWindowsIdentity(file.Get(), identity)
                || !IsWindowsRegularFile(identity) || identity.LinkCount != 1
                || identity.Size != root.Identity.Size)
            {
                if (error.empty()) error = "snapshot root is unavailable";
                return false;
            }
            return ReadSnapshotRootJsonBytes(root.Identity.Size, isGlb,
                [&](u64 offset, std::span<u8> destination)
                {
                    return ReadWindowsAtExactly(file.Get(), offset, destination, options, error);
                }, options, bytes, binaryChunkBytes, binaryChunkCount, error);
        }

        bool SealWindowsSnapshotDirectory(HANDLE directoryHandle, u32 depth, std::string& error)
        {
            if (depth > 64)
            {
                error = "snapshot directory depth exceeds the sealing bound";
                return false;
            }
            WindowsFileIdentity directoryIdentity;
            if (!GetWindowsIdentity(directoryHandle, directoryIdentity)
                || !IsWindowsDirectory(directoryIdentity))
            {
                error = "could not enumerate snapshot directory while sealing";
                return false;
            }
            std::vector<std::wstring> names;
            if (!EnumerateWindowsNames(directoryHandle, names))
            {
                error = "could not enumerate snapshot directory while sealing";
                return false;
            }
            bool success = true;
            for (const std::wstring& name : names)
            {
                if (name == L"." || name == L"..")
                    continue;
                WindowsHandle child = OpenWindowsChildNative(directoryHandle, name,
                    GENERIC_READ | FILE_READ_ATTRIBUTES | READ_CONTROL | SYNCHRONIZE,
                    FILE_OPEN, true);
                WindowsFileIdentity childIdentity;
                if (!child || !GetWindowsIdentity(child.Get(), childIdentity)
                    || childIdentity.VolumeSerial != directoryIdentity.VolumeSerial
                    || (childIdentity.Attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
                {
                    error = "snapshot entry changed while sealing";
                    success = false;
                    break;
                }
                if (IsWindowsDirectory(childIdentity))
                {
                    WindowsHandle writableDirectory = OpenWindowsChildNative(directoryHandle, name,
                        GENERIC_READ | FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES
                            | READ_CONTROL | WRITE_DAC | DELETE | SYNCHRONIZE,
                        FILE_OPEN, true, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE);
                    WindowsFileIdentity writableIdentity;
                    if (!writableDirectory
                        || !GetWindowsIdentity(writableDirectory.Get(), writableIdentity)
                        || !SameWindowsIdentity(childIdentity, writableIdentity)
                        || !SealWindowsSnapshotDirectory(
                            writableDirectory.Get(), depth + 1, error))
                    {
                        if (error.empty()) error = "could not make snapshot directory read-only";
                        success = false;
                        break;
                    }
                }
                else
                {
                    WindowsHandle writeProbe = OpenWindowsChildNative(directoryHandle, name,
                        GENERIC_WRITE, FILE_OPEN, false, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE);
                    if (!IsWindowsRegularFile(childIdentity) || childIdentity.LinkCount != 1
                        || (childIdentity.Attributes & FILE_ATTRIBUTE_READONLY) == 0
                        || writeProbe)
                    {
                        error = "snapshot contains a mutable or non-regular object while sealing";
                        success = false;
                        break;
                    }
                }
            }
            if (!success)
                return false;
            if (!SetWindowsReadOnly(directoryHandle, true))
            {
                error = "could not make snapshot directory read-only";
                return false;
            }
            return true;
        }

        bool VerifyWindowsStagedSnapshot(
            HANDLE stagingHandle,
            const WindowsInventory& sourceInventory,
            const std::vector<LocalPackageSnapshotEntry>& entries,
            const LocalPackageSnapshotOptions& options,
            std::string& error)
        {
            LocalPackageSnapshotOptions verificationOptions;
            verificationOptions.Limits = options.Limits;
            verificationOptions.IsCancelled = options.IsCancelled;
            ProgressState progress;
            WindowsInventory stagedInventory;
            if (!BuildWindowsInventory(stagingHandle, verificationOptions, progress,
                stagedInventory, error)
                || stagedInventory.Files.size() != entries.size()
                || stagedInventory.Directories.size() != sourceInventory.Directories.size())
            {
                if (error.empty()) error = "sealed snapshot inventory does not match the copied package";
                return false;
            }
            for (size_t index = 0; index < sourceInventory.Directories.size(); ++index)
            {
                if (stagedInventory.Directories[index].RelativePath
                    != sourceInventory.Directories[index].RelativePath)
                {
                    error = "sealed snapshot directory inventory changed before commit";
                    return false;
                }
            }
            for (size_t index = 0; index < entries.size(); ++index)
            {
                const WindowsInventoryRecord& record = stagedInventory.Files[index];
                const LocalPackageSnapshotEntry& expected = entries[index];
                if (record.RelativePath != expected.RelativePath
                    || record.Identity.Size != expected.SizeBytes
                    || record.Identity.LinkCount != 1
                    || (record.Identity.Attributes & FILE_ATTRIBUTE_READONLY) == 0)
                {
                    error = "sealed snapshot file metadata changed before commit";
                    return false;
                }
                WindowsHandle file = OpenWindowsRelativeFile(stagingHandle, record.Segments,
                    stagedInventory.RootIdentity.VolumeSerial,
                    GENERIC_READ | FILE_READ_ATTRIBUTES | SYNCHRONIZE, error);
                WindowsFileIdentity beforeIdentity;
                if (!file || !GetWindowsIdentity(file.Get(), beforeIdentity)
                    || !SameWindowsIdentity(record.Identity, beforeIdentity))
                {
                    if (error.empty()) error = "sealed snapshot file changed before verification";
                    return false;
                }
                Sha256Builder hash;
                std::array<u8, 64 * 1024> buffer {};
                u64 bytesRead = 0;
                while (true)
                {
                    DWORD count = 0;
                    if (!::ReadFile(file.Get(), buffer.data(), static_cast<DWORD>(buffer.size()),
                        &count, nullptr))
                    {
                        error = "could not verify sealed snapshot bytes";
                        return false;
                    }
                    if (count == 0)
                        break;
                    if (bytesRead > expected.SizeBytes || count > expected.SizeBytes - bytesRead)
                    {
                        error = "sealed snapshot file grew before commit";
                        return false;
                    }
                    hash.Update(std::span<const u8>(buffer.data(), count));
                    bytesRead += count;
                    if (options.IsCancelled && options.IsCancelled())
                    {
                        error = "local package snapshot was cancelled";
                        return false;
                    }
                }
                WindowsFileIdentity afterIdentity;
                if (bytesRead != expected.SizeBytes
                    || !GetWindowsIdentity(file.Get(), afterIdentity)
                    || !SameWindowsIdentity(record.Identity, afterIdentity)
                    || hash.FinalizeHex() != expected.Sha256)
                {
                    error = "sealed snapshot bytes changed before commit";
                    return false;
                }
            }
            return true;
        }

        bool CreateWindowsSnapshot(
            const std::filesystem::path& sourceDirectory,
            const std::filesystem::path& privateStagingParent,
            const LocalPackageSnapshotOptions& options,
            WindowsSnapshotPayload& payload,
            std::string& error)
        {
            ProgressState progress;
            if (!Checkpoint(options, LocalPackageSnapshotHookPoint::Started, {}, progress, error))
                return false;

            constexpr DWORD directoryReadAccess =
                GENERIC_READ | FILE_READ_ATTRIBUTES | READ_CONTROL | SYNCHRONIZE;
            WindowsHandle sourceRoot = OpenWindowsAbsoluteDirectoryNoFollow(
                sourceDirectory, directoryReadAccess, FILE_SHARE_READ);
            WindowsHandle stagingParent = OpenWindowsAbsoluteDirectoryNoFollow(
                privateStagingParent, directoryReadAccess,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE);
            if (!sourceRoot || !stagingParent)
            {
                error = "source and private staging paths must be existing non-reparse directories";
                return false;
            }
            if (!IsPrivateWindowsDirectory(stagingParent.Get()))
            {
                error = "private staging parent must be owned by the current user without untrusted access";
                return false;
            }
            std::error_code absoluteError;
            const std::filesystem::path sourcePath = std::filesystem::absolute(sourceDirectory,
                absoluteError).lexically_normal();
            const std::filesystem::path stagingParentPath = std::filesystem::absolute(privateStagingParent,
                absoluteError).lexically_normal();
            if (absoluteError) { error = "could not validate source and staging directory separation"; return false; }
            if (WindowsPathIsWithin(sourcePath, stagingParentPath))
            {
                error = "private staging directory must be outside the selected package root";
                return false;
            }

            static std::atomic<u64> sequence { 1 };
            std::string stagingName;
            WindowsHandle staging;
            for (u32 attempt = 0; attempt < 128; ++attempt)
            {
                stagingName = ".spiral-package-snapshot-"
                    + std::to_string(static_cast<u64>(::GetCurrentProcessId())) + '-'
                    + std::to_string(sequence.fetch_add(1, std::memory_order_relaxed));
                const std::wstring wideName(stagingName.begin(), stagingName.end());
                WindowsHandle created = OpenWindowsChildNative(stagingParent.Get(), wideName,
                    GENERIC_READ | FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES | READ_CONTROL
                        | WRITE_DAC | DELETE | SYNCHRONIZE, FILE_CREATE, true,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE);
                if (created)
                {
                    // Retain this first handle: no DOS-path reopen occurs after creation.
                    staging = std::move(created);
                    break;
                }
                stagingName.clear();
            }
            if (stagingName.empty())
            {
                error = "could not allocate a unique snapshot staging directory";
                return false;
            }

            WindowsFileIdentity stagingIdentity;
            if (!staging || !GetWindowsIdentity(staging.Get(), stagingIdentity)
                || !IsWindowsDirectory(stagingIdentity)
                || !SetWindowsOwnerAccess(staging.Get(), GENERIC_ALL)
                || !IsPrivateWindowsDirectory(staging.Get()))
            {
                if (staging)
                    CleanupSnapshotDirectoryWindows(staging.Get());
                error = "private snapshot staging directory has invalid identity or permissions";
                return false;
            }
            const std::wstring wideStagingName(stagingName.begin(), stagingName.end());
            WindowsHandle stagedFromRetainedParent = OpenWindowsChildNative(stagingParent.Get(), wideStagingName,
                GENERIC_READ | FILE_READ_ATTRIBUTES | READ_CONTROL | SYNCHRONIZE,
                FILE_OPEN, true, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE);
            WindowsFileIdentity retainedNameIdentity;
            if (!stagedFromRetainedParent
                || !GetWindowsIdentity(stagedFromRetainedParent.Get(), retainedNameIdentity)
                || !SameWindowsObject(stagingIdentity, retainedNameIdentity))
            {
                CleanupSnapshotDirectoryWindows(staging.Get());
                error = "private snapshot staging parent changed during creation";
                return false;
            }

            WindowsHandle retainedParent = DuplicateWindowsHandle(stagingParent.Get());
            WindowsHandle retainedDirectory = DuplicateWindowsHandle(staging.Get());
            DWORD parentFlags = HANDLE_FLAG_INHERIT;
            DWORD directoryFlags = HANDLE_FLAG_INHERIT;
            if (!retainedParent || !retainedDirectory
                || !::GetHandleInformation(retainedParent.Get(), &parentFlags)
                || !::GetHandleInformation(retainedDirectory.Get(), &directoryFlags)
                || (parentFlags & HANDLE_FLAG_INHERIT) != 0
                || (directoryFlags & HANDLE_FLAG_INHERIT) != 0)
            {
                CleanupSnapshotDirectoryWindows(staging.Get());
                error = "could not retain non-inheritable snapshot directory ownership";
                return false;
            }
            payload.ParentHandle = retainedParent.Release();
            payload.DirectoryHandle = retainedDirectory.Release();
            payload.StagingName = stagingName;
            if (!Checkpoint(options, LocalPackageSnapshotHookPoint::StagingCreated, {}, progress, error))
                return false;

            WindowsInventory inventory;
            if (!BuildWindowsInventory(sourceRoot.Get(), options, progress, inventory, error))
                return false;
            progress.FileCount = static_cast<u64>(inventory.Files.size());
            progress.AggregateBytes = inventory.AggregateBytes;
            if (!Checkpoint(options, LocalPackageSnapshotHookPoint::InventoryComplete, {}, progress, error))
                return false;

            const WindowsInventoryRecord* root = nullptr;
            for (const WindowsInventoryRecord& file : inventory.Files)
            {
                if (HasRootExtension(file.RelativePath))
                {
                    if (root)
                    {
                        error = "package must contain exactly one glTF or GLB root";
                        return false;
                    }
                    root = &file;
                }
            }
            if (!root)
            {
                error = "package must contain exactly one glTF or GLB root";
                return false;
            }

            std::vector<LocalPackageSnapshotEntry> entries(inventory.Files.size());
            std::vector<Sha256Builder::Digest> digests(inventory.Files.size());
            for (size_t index = 0; index < inventory.Files.size(); ++index)
            {
                if (!CopyWindowsFile(sourceRoot.Get(), staging.Get(),
                    inventory.RootIdentity.VolumeSerial, stagingIdentity.VolumeSerial,
                    inventory.Files[index], options, progress,
                    entries[index], digests[index], error))
                    return false;
            }

            if (!Checkpoint(options, LocalPackageSnapshotHookPoint::BeforeReenumeration,
                {}, progress, error))
                return false;
            WindowsInventory verifiedInventory;
            ProgressState verificationProgress;
            LocalPackageSnapshotOptions verificationOptions;
            verificationOptions.Limits = options.Limits;
            verificationOptions.IsCancelled = options.IsCancelled;
            if (!BuildWindowsInventory(sourceRoot.Get(), verificationOptions,
                verificationProgress, verifiedInventory, error)
                || !SameWindowsInventory(inventory, verifiedInventory))
            {
                if (error.empty()) error = "package changed between inventory and final verification";
                return false;
            }

            if (!Checkpoint(options, LocalPackageSnapshotHookPoint::BeforeDependencyValidation,
                root->RelativePath, progress, error)
                || !ValidateDependencyClosure(inventory, *root,
                    [&](const WindowsInventoryRecord& selectedRoot, bool isGlb,
                        const LocalPackageSnapshotOptions& selectedOptions,
                        std::vector<u8>& bytes, u64& binaryChunkBytes,
                        u32& binaryChunkCount, std::string& selectedError)
                    {
                        return ReadWindowsSnapshotRootJson(staging.Get(),
                            stagingIdentity.VolumeSerial, selectedRoot, isGlb,
                            selectedOptions, bytes, binaryChunkBytes,
                            binaryChunkCount, selectedError);
                    }, options, error))
                return false;
            if (!Checkpoint(options, LocalPackageSnapshotHookPoint::BeforeCommit, {}, progress, error))
                return false;

            if (!SealWindowsSnapshotDirectory(staging.Get(), 0, error)
                || !VerifyWindowsStagedSnapshot(staging.Get(), inventory, entries, options, error))
                return false;

            WindowsFileIdentity finalStagingIdentity;
            if (!GetWindowsIdentity(staging.Get(), finalStagingIdentity)
                || !SameWindowsObject(stagingIdentity, finalStagingIdentity)
                || !IsWindowsDirectory(finalStagingIdentity)
                || (finalStagingIdentity.Attributes & FILE_ATTRIBUTE_READONLY) == 0)
            {
                error = "private snapshot staging identity changed before commit";
                return false;
            }
            WindowsHandle namedStaging = OpenWindowsChildNative(stagingParent.Get(), wideStagingName,
                GENERIC_READ | FILE_READ_ATTRIBUTES | READ_CONTROL | SYNCHRONIZE,
                FILE_OPEN, true, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE);
            WindowsFileIdentity namedIdentity;
            if (!namedStaging || !GetWindowsIdentity(namedStaging.Get(), namedIdentity)
                || !SameWindowsObject(finalStagingIdentity, namedIdentity)
                || !IsWindowsDirectory(namedIdentity))
            {
                error = "private snapshot staging identity changed before commit";
                return false;
            }
            // This diagnostic path is never reopened by snapshot operations.
            payload.Directory = stagingParentPath / stagingName;

            payload.Entries = std::move(entries);
            payload.TreeSha256 = ComputeTreeHash(payload.Entries, digests);
            payload.RootRelativePath = root->RelativePath;
            error.clear();
            return true;
        }

        bool StreamWindowsSnapshotEntry(HANDLE directoryHandle,
            const std::vector<std::string>& segments, const LocalPackageSnapshotEntry& expected,
            const std::function<bool(std::span<const u8>)>& onChunk, std::string& error)
        {
            WindowsHandle file = OpenWindowsRelativeFile(directoryHandle, segments, 0,
                GENERIC_READ | FILE_READ_ATTRIBUTES | SYNCHRONIZE, error);
            WindowsFileIdentity before {}, after {};
            if (!file || !GetWindowsIdentity(file.Get(), before) || !IsWindowsRegularFile(before)
                || before.LinkCount != 1 || before.Size != expected.SizeBytes
                || !HasOnlyDefaultWindowsDataStream(file.Get()))
            { if (error.empty()) error = "snapshot entry changed"; return false; }
            Sha256Builder hash; std::array<u8, 64 * 1024> buffer {}; u64 total = 0;
            while (true)
            {
                DWORD count = 0;
                if (!::ReadFile(file.Get(), buffer.data(), static_cast<DWORD>(buffer.size()), &count, nullptr))
                { error = "snapshot entry could not be read"; return false; }
                if (!count) break;
                if (total > expected.SizeBytes || count > expected.SizeBytes - total
                    || !onChunk(std::span<const u8>(buffer.data(), count)))
                { error = "snapshot entry callback aborted"; return false; }
                hash.Update(std::span<const u8>(buffer.data(), count)); total += count;
            }
            if (!GetWindowsIdentity(file.Get(), after) || !SameWindowsIdentity(before, after)
                || total != expected.SizeBytes || hash.FinalizeHex() != expected.Sha256)
            { error = "snapshot entry changed while reading"; return false; }
            return true;
        }
#endif

#if defined(GE_PLATFORM_LINUX)
        bool CreateLinuxSnapshot(
            const std::filesystem::path& sourceDirectory,
            const std::filesystem::path& privateStagingParent,
            const LocalPackageSnapshotOptions& options,
            SnapshotPayload& payload,
            std::string& error)
        {
            ProgressState progress;
            if (!Checkpoint(options, LocalPackageSnapshotHookPoint::Started, {}, progress, error))
                return false;

            FileDescriptor sourceRoot = OpenAbsoluteDirectoryNoFollow(sourceDirectory);
            FileDescriptor stagingParent = OpenAbsoluteDirectoryNoFollow(privateStagingParent);
            if (!sourceRoot || !stagingParent)
            {
                error = "source and private staging paths must be existing non-symlink directories";
                return false;
            }
            struct stat stagingParentStatus {};
            if (fstat(stagingParent.Get(), &stagingParentStatus) != 0
                || !S_ISDIR(stagingParentStatus.st_mode)
                || stagingParentStatus.st_uid != geteuid()
                || (stagingParentStatus.st_mode & 0077) != 0)
            {
                error = "private staging parent must be owned by the current user with no group or other access";
                return false;
            }
            std::string sourceDescriptorPath;
            std::string stagingDescriptorPath;
            if (!ReadDescriptorPath(sourceRoot.Get(), sourceDescriptorPath)
                || !ReadDescriptorPath(stagingParent.Get(), stagingDescriptorPath))
            {
                error = "could not validate source and staging directory separation";
                return false;
            }
            if (IsWithin(sourceDescriptorPath, stagingDescriptorPath))
            {
                error = "private staging directory must be outside the selected package root";
                return false;
            }

            static std::atomic<u64> sequence { 1 };
            std::string stagingName;
            for (u32 attempt = 0; attempt < 128; ++attempt)
            {
                stagingName = ".spiral-package-snapshot-" + std::to_string(static_cast<u64>(getpid()))
                    + '-' + std::to_string(sequence.fetch_add(1, std::memory_order_relaxed));
                if (mkdirat(stagingParent.Get(), stagingName.c_str(), 0700) == 0)
                    break;
                if (errno != EEXIST)
                {
                    error = "could not create private snapshot staging directory";
                    return false;
                }
                stagingName.clear();
            }
            if (stagingName.empty())
            {
                error = "could not allocate a unique snapshot staging directory";
                return false;
            }
            struct stat createdStatus {};
            if (fstatat(stagingParent.Get(), stagingName.c_str(), &createdStatus,
                AT_SYMLINK_NOFOLLOW) != 0 || !S_ISDIR(createdStatus.st_mode))
            {
                error = "could not retain the created snapshot directory identity";
                return false;
            }
            CreatedDirectoryGuard createdDirectory(stagingParent.Get(), stagingName, createdStatus);
            FileDescriptor staging(openat(stagingParent.Get(), stagingName.c_str(),
                O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
            struct stat stagingStatus {};
            if (!staging || fstat(staging.Get(), &stagingStatus) != 0
                || !S_ISDIR(stagingStatus.st_mode) || (stagingStatus.st_mode & 0777) != 0700
                || stagingStatus.st_dev != createdStatus.st_dev
                || stagingStatus.st_ino != createdStatus.st_ino)
            {
                error = "private snapshot staging directory has invalid identity or permissions";
                return false;
            }
            payload.DirectoryDevice = static_cast<u64>(stagingStatus.st_dev);
            payload.DirectoryInode = static_cast<u64>(stagingStatus.st_ino);
            const int retainedParent = DuplicateCloexec(stagingParent.Get());
            const int retainedDirectory = DuplicateCloexec(staging.Get());
            if (retainedParent < 0 || retainedDirectory < 0)
            {
                if (retainedParent >= 0) close(retainedParent);
                if (retainedDirectory >= 0) close(retainedDirectory);
                error = "could not retain private snapshot directory ownership";
                return false;
            }
            payload.ParentDescriptor = retainedParent;
            payload.DirectoryDescriptor = retainedDirectory;
            payload.StagingName = stagingName;
            payload.Directory = std::filesystem::path("/proc/self/fd")
                / std::to_string(retainedDirectory);
            createdDirectory.Release();
            if (!Checkpoint(options, LocalPackageSnapshotHookPoint::StagingCreated, {}, progress, error))
                return false;

            Inventory inventory;
            if (!BuildInventory(sourceRoot.Get(), options, progress, inventory, error))
                return false;
            progress.FileCount = static_cast<u64>(inventory.Files.size());
            progress.AggregateBytes = inventory.AggregateBytes;
            if (!Checkpoint(options, LocalPackageSnapshotHookPoint::InventoryComplete, {}, progress, error))
                return false;

            const InventoryRecord* root = nullptr;
            for (const InventoryRecord& file : inventory.Files)
            {
                if (HasRootExtension(file.RelativePath))
                {
                    if (root)
                    {
                        error = "package must contain exactly one glTF or GLB root";
                        return false;
                    }
                    root = &file;
                }
            }
            if (!root)
            {
                error = "package must contain exactly one glTF or GLB root";
                return false;
            }

            std::vector<LocalPackageSnapshotEntry> entries(inventory.Files.size());
            std::vector<Sha256Builder::Digest> digests(inventory.Files.size());
            for (size_t index = 0; index < inventory.Files.size(); ++index)
            {
                if (!CopyFile(sourceRoot.Get(), staging.Get(), inventory.RootIdentity.Device,
                    inventory.RootIdentity.MountId,
                    inventory.Files[index], options, progress, entries[index], digests[index], error))
                    return false;
            }

            if (!Checkpoint(options, LocalPackageSnapshotHookPoint::BeforeReenumeration, {}, progress, error))
                return false;
            Inventory verifiedInventory;
            ProgressState verificationProgress;
            LocalPackageSnapshotOptions verificationOptions;
            verificationOptions.Limits = options.Limits;
            verificationOptions.IsCancelled = options.IsCancelled;
            if (!BuildInventory(sourceRoot.Get(), verificationOptions,
                verificationProgress, verifiedInventory, error)
                || !SameInventory(inventory, verifiedInventory))
            {
                if (error.empty()) error = "package changed between inventory and final verification";
                return false;
            }

            if (!Checkpoint(options, LocalPackageSnapshotHookPoint::BeforeDependencyValidation,
                root->RelativePath, progress, error)
                || !ValidateDependencyClosure(inventory, *root,
                    [&](const InventoryRecord& selectedRoot, bool isGlb,
                        const LocalPackageSnapshotOptions& selectedOptions,
                        std::vector<u8>& bytes, u64& binaryChunkBytes,
                        u32& binaryChunkCount, std::string& selectedError)
                    {
                        return ReadSnapshotRootJson(staging.Get(), selectedRoot, isGlb,
                            selectedOptions, bytes, binaryChunkBytes, binaryChunkCount, selectedError);
                    }, options, error))
                return false;
            if (!Checkpoint(options, LocalPackageSnapshotHookPoint::BeforeCommit, {}, progress, error))
                return false;

            if (!SealSnapshotDirectory(staging.Get(), 0, error)
                || !VerifyStagedSnapshot(staging.Get(), inventory, entries, options, error))
                return false;

            struct stat finalStagingStatus {};
            struct stat finalPathStatus {};
            if (fstat(staging.Get(), &finalStagingStatus) != 0
                || fstatat(stagingParent.Get(), stagingName.c_str(), &finalPathStatus, AT_SYMLINK_NOFOLLOW) != 0
                || !S_ISDIR(finalStagingStatus.st_mode) || !S_ISDIR(finalPathStatus.st_mode)
                || finalStagingStatus.st_dev != stagingStatus.st_dev
                || finalStagingStatus.st_ino != stagingStatus.st_ino
                || finalPathStatus.st_dev != stagingStatus.st_dev
                || finalPathStatus.st_ino != stagingStatus.st_ino
                || (finalPathStatus.st_mode & 0777) != 0500)
            {
                error = "private snapshot staging identity changed before commit";
                return false;
            }

            payload.Entries = std::move(entries);
            payload.TreeSha256 = ComputeTreeHash(payload.Entries, digests);
            payload.RootRelativePath = root->RelativePath;
            error.clear();
            return true;
        }
#endif
    }
#endif

    bool LocalPackageSnapshot::Create(
        const std::filesystem::path& sourceDirectory,
        const std::filesystem::path& privateStagingParent,
        const LocalPackageSnapshotOptions& options,
        LocalPackageSnapshot& snapshot,
        std::string& error)
    {
        error.clear();
#if defined(GE_PLATFORM_LINUX)
        try
        {
            SnapshotPayload payload;
            if (!CreateLinuxSnapshot(sourceDirectory, privateStagingParent, options, payload, error))
                return false;
            LocalPackageSnapshot candidate;
            candidate.m_Directory = std::move(payload.Directory);
            candidate.m_Entries = std::move(payload.Entries);
            candidate.m_TreeSha256 = std::move(payload.TreeSha256);
            candidate.m_RootRelativePath = std::move(payload.RootRelativePath);
            candidate.m_DirectoryDevice = payload.DirectoryDevice;
            candidate.m_DirectoryInode = payload.DirectoryInode;
            candidate.m_StagingParentDescriptor = payload.ParentDescriptor;
            candidate.m_DirectoryDescriptor = payload.DirectoryDescriptor;
            candidate.m_StagingName = std::move(payload.StagingName);
            payload.ParentDescriptor = -1;
            payload.DirectoryDescriptor = -1;
            snapshot = std::move(candidate);
            payload.Directory.clear();
            return true;
        }
        catch (...)
        {
            error = "local package snapshot callback or allocation failed";
            return false;
        }
#elif defined(GE_PLATFORM_WINDOWS)
        try
        {
            WindowsSnapshotPayload payload;
            if (!CreateWindowsSnapshot(sourceDirectory, privateStagingParent, options, payload, error))
                return false;
            LocalPackageSnapshot candidate;
            candidate.m_Directory = std::move(payload.Directory);
            candidate.m_Entries = std::move(payload.Entries);
            candidate.m_TreeSha256 = std::move(payload.TreeSha256);
            candidate.m_RootRelativePath = std::move(payload.RootRelativePath);
            candidate.m_StagingParentHandle = payload.ParentHandle;
            candidate.m_DirectoryHandle = payload.DirectoryHandle;
            candidate.m_StagingName = std::move(payload.StagingName);
            payload.ParentHandle = nullptr;
            payload.DirectoryHandle = nullptr;
            snapshot = std::move(candidate);
            payload.Directory.clear();
            return true;
        }
        catch (...)
        {
            error = "local package snapshot callback or allocation failed";
            return false;
        }
#else
        (void)sourceDirectory;
        (void)privateStagingParent;
        (void)options;
        (void)snapshot;
        error = "secure local package snapshots are currently implemented only on Windows and Linux";
        return false;
#endif
    }
}
