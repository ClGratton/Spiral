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

    LocalPackageSnapshot::~LocalPackageSnapshot()
    {
        Reset();
    }

    LocalPackageSnapshot::LocalPackageSnapshot(LocalPackageSnapshot&& other) noexcept
        : m_Directory(std::move(other.m_Directory)),
          m_Entries(std::move(other.m_Entries)),
          m_TreeSha256(std::move(other.m_TreeSha256)),
          m_RootRelativePath(std::move(other.m_RootRelativePath)),
          m_DirectoryDevice(std::exchange(other.m_DirectoryDevice, 0)),
          m_DirectoryInode(std::exchange(other.m_DirectoryInode, 0)),
          m_StagingParentDescriptor(std::exchange(other.m_StagingParentDescriptor, -1)),
          m_DirectoryDescriptor(std::exchange(other.m_DirectoryDescriptor, -1)),
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
        m_DirectoryDevice = std::exchange(other.m_DirectoryDevice, 0);
        m_DirectoryInode = std::exchange(other.m_DirectoryInode, 0);
        m_StagingParentDescriptor = std::exchange(other.m_StagingParentDescriptor, -1);
        m_DirectoryDescriptor = std::exchange(other.m_DirectoryDescriptor, -1);
        m_StagingName = std::move(other.m_StagingName);
        other.m_Directory.clear();
        return *this;
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
        m_DirectoryDevice = 0;
        m_DirectoryInode = 0;
        m_StagingParentDescriptor = -1;
        m_DirectoryDescriptor = -1;
        m_StagingName.clear();
    }

#if defined(GE_PLATFORM_LINUX)
    namespace
    {
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

        struct ProgressState
        {
            u64 FilesCompleted = 0;
            u64 FileCount = 0;
            u64 BytesCompleted = 0;
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
            binaryChunkBytes = 0;
            binaryChunkCount = 0;
            FileDescriptor file = OpenSnapshotFile(stagingDescriptor, root.Segments, error);
            if (!file)
                return false;
            if (root.Identity.Size < 0)
            {
                error = "snapshot root is too large for this host";
                return false;
            }
            const u64 rootBytes = static_cast<u64>(root.Identity.Size);
            if (!isGlb)
            {
                if (rootBytes > options.Limits.MaximumGltfJsonBytes
                    || rootBytes > std::numeric_limits<size_t>::max())
                {
                    error = "glTF JSON exceeds the configured size limit";
                    return false;
                }
                bytes.resize(static_cast<size_t>(rootBytes));
                return ReadAtExactly(file.Get(), 0, bytes, options, error)
                    && ValidateJsonDocument(bytes, options, error);
            }

            constexpr u32 kGlbMagic = 0x46546c67u;
            constexpr u32 kJsonChunk = 0x4e4f534au;
            constexpr u32 kBinaryChunk = 0x004e4942u;
            std::array<u8, 12> header {};
            if (rootBytes < 20 || !ReadAtExactly(file.Get(), 0, header, options, error)
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
                if (rootBytes - offset < chunkHeader.size()
                    || !ReadAtExactly(file.Get(), offset, chunkHeader, options, error))
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
                    if (!ReadAtExactly(file.Get(), offset, bytes, options, error))
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

        bool ValidateDependencyClosure(
            int stagingDescriptor,
            const Inventory& inventory,
            const InventoryRecord& root,
            const LocalPackageSnapshotOptions& options,
            std::string& error)
        {
            const std::string foldedRoot = AsciiCaseFold(root.RelativePath);
            const bool isGlb = foldedRoot.size() >= 4 && foldedRoot.substr(foldedRoot.size() - 4) == ".glb";
            std::vector<u8> bytes;
            u64 binaryChunkBytes = 0;
            u32 binaryChunkCount = 0;
            if (!ReadSnapshotRootJson(stagingDescriptor, root, isGlb, options,
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
            for (const InventoryRecord& file : inventory.Files)
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
                || !ValidateDependencyClosure(staging.Get(), inventory, *root, options, error))
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
#else
        (void)sourceDirectory;
        (void)privateStagingParent;
        (void)options;
        (void)snapshot;
        error = "secure local package snapshots are currently implemented only on Linux";
        return false;
#endif
    }
}
