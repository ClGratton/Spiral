#include "Engine/Assets/FabZipStaging.h"

#include "Engine/Core/Sha256.h"

#include <cerrno>
#include <cstring>
#include <random>
#include <utility>
#include <vector>

#if defined(GE_PLATFORM_LINUX)
    #include <dirent.h>
    #include <fcntl.h>
    #include <sys/stat.h>
    #include <sys/types.h>
    #include <unistd.h>
#endif

namespace Engine
{
    namespace
    {
        constexpr std::string_view kUnsupported = "Fab package staging is not implemented on this platform";

        bool Fail(std::string& error, std::string message)
        {
            error = std::move(message);
            return false;
        }

        bool IsSingleSegmentName(std::string_view name)
        {
            if (name.empty() || name.size() > 255 || name == "." || name == "..")
                return false;
            for (const unsigned char character : name)
            {
                const bool allowed = (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z')
                    || (character >= '0' && character <= '9') || character == '.' || character == '_' || character == '-';
                if (!allowed)
                    return false;
            }
            return true;
        }
    }

#if defined(GE_PLATFORM_LINUX)
    namespace
    {
        constexpr size_t kCopyChunkBytes = 256 * 1024;
        constexpr int kMaximumRemovalDepth = 64;

        class ScopedDescriptor
        {
        public:
            ScopedDescriptor() = default;
            explicit ScopedDescriptor(int descriptor)
                : m_Descriptor(descriptor)
            {
            }

            ~ScopedDescriptor() { Close(); }

            ScopedDescriptor(const ScopedDescriptor&) = delete;
            ScopedDescriptor& operator=(const ScopedDescriptor&) = delete;
            ScopedDescriptor(ScopedDescriptor&& other) noexcept
                : m_Descriptor(std::exchange(other.m_Descriptor, -1))
            {
            }

            ScopedDescriptor& operator=(ScopedDescriptor&& other) noexcept
            {
                if (this != &other)
                {
                    Close();
                    m_Descriptor = std::exchange(other.m_Descriptor, -1);
                }
                return *this;
            }

            int Get() const { return m_Descriptor; }
            bool IsValid() const { return m_Descriptor >= 0; }
            int Release() { return std::exchange(m_Descriptor, -1); }

            void Close()
            {
                if (m_Descriptor >= 0)
                    ::close(m_Descriptor);
                m_Descriptor = -1;
            }

        private:
            int m_Descriptor = -1;
        };

        std::string ErrnoText(std::string_view what)
        {
            return std::string(what) + ": " + std::strerror(errno);
        }

        bool IsOwnerOnlyDirectory(const struct stat& status)
        {
            return S_ISDIR(status.st_mode) && status.st_uid == ::geteuid() && (status.st_mode & 0077) == 0;
        }

        bool SameIdentity(const struct stat& status, u64 device, u64 inode)
        {
            return static_cast<u64>(status.st_dev) == device && static_cast<u64>(status.st_ino) == inode;
        }

        std::string RandomSuffix()
        {
            static constexpr char kDigits[] = "0123456789abcdef";
            std::random_device device;
            std::string suffix;
            for (int word = 0; word < 2; ++word)
            {
                const unsigned int value = device();
                for (int shift = 28; shift >= 0; shift -= 4)
                    suffix.push_back(kDigits[(value >> shift) & 0xf]);
            }
            return suffix;
        }

        bool RemoveContents(int directoryDescriptor, int depth, std::string& error)
        {
            if (depth > kMaximumRemovalDepth)
                return Fail(error, "staging tree is deeper than the removal bound");
            const int duplicate = ::fcntl(directoryDescriptor, F_DUPFD_CLOEXEC, 0);
            if (duplicate < 0)
                return Fail(error, ErrnoText("could not duplicate the staging directory descriptor"));
            DIR* stream = ::fdopendir(duplicate);
            if (!stream)
            {
                ::close(duplicate);
                return Fail(error, ErrnoText("could not enumerate the staging directory"));
            }
            std::vector<std::string> names;
            ::rewinddir(stream);
            while (const dirent* entry = ::readdir(stream))
            {
                const std::string_view name = entry->d_name;
                if (name != "." && name != "..")
                    names.emplace_back(name);
            }
            ::closedir(stream);

            for (const std::string& name : names)
            {
                struct stat status {};
                if (::fstatat(directoryDescriptor, name.c_str(), &status, AT_SYMLINK_NOFOLLOW) != 0)
                {
                    if (errno == ENOENT)
                        continue;
                    return Fail(error, ErrnoText("could not inspect a staging entry"));
                }
                if (S_ISDIR(status.st_mode))
                {
                    ScopedDescriptor child(::openat(directoryDescriptor, name.c_str(),
                        O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
                    struct stat opened {};
                    if (!child.IsValid() || ::fstat(child.Get(), &opened) != 0 || !SameIdentity(opened,
                            static_cast<u64>(status.st_dev), static_cast<u64>(status.st_ino)))
                        return Fail(error, "a staging directory changed while it was removed");
                    if (opened.st_uid == ::geteuid() && (opened.st_mode & 0700) != 0700)
                        ::fchmod(child.Get(), 0700);
                    if (!RemoveContents(child.Get(), depth + 1, error))
                        return false;
                    child.Close();
                    if (::unlinkat(directoryDescriptor, name.c_str(), AT_REMOVEDIR) != 0)
                        return Fail(error, ErrnoText("could not remove a staging directory"));
                }
                else if (::unlinkat(directoryDescriptor, name.c_str(), 0) != 0 && errno != ENOENT)
                    return Fail(error, ErrnoText("could not remove a staging file"));
            }
            return true;
        }

        // The identity of a directory created by CreateOwnerOnlyDirectory, handed to
        // FabStagingDirectory, which keeps its own members private.
        struct CreatedDirectory
        {
            int ParentDescriptor = -1;
            int Descriptor = -1;
            u64 Device = 0;
            u64 Inode = 0;
        };

        // Creates "<parentDescriptor>/<name>" with mode 0700; the name must not exist.
        bool CreateOwnerOnlyDirectory(int parentDescriptor, const std::string& name, CreatedDirectory& created,
            std::string& error)
        {
            if (::mkdirat(parentDescriptor, name.c_str(), 0700) != 0)
                return Fail(error, errno == EEXIST ? "staging entry already exists: " + name
                                                   : ErrnoText("could not create a staging directory"));
            ScopedDescriptor directory(::openat(parentDescriptor, name.c_str(),
                O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
            struct stat status {};
            if (!directory.IsValid() || ::fstat(directory.Get(), &status) != 0 || !S_ISDIR(status.st_mode)
                || status.st_uid != ::geteuid())
            {
                ::unlinkat(parentDescriptor, name.c_str(), AT_REMOVEDIR);
                return Fail(error, "the new staging directory is not an owned directory");
            }
            if (::fchmod(directory.Get(), 0700) != 0)
            {
                ::unlinkat(parentDescriptor, name.c_str(), AT_REMOVEDIR);
                return Fail(error, ErrnoText("could not restrict the staging directory"));
            }
            const int parentDuplicate = ::fcntl(parentDescriptor, F_DUPFD_CLOEXEC, 0);
            if (parentDuplicate < 0)
            {
                ::unlinkat(parentDescriptor, name.c_str(), AT_REMOVEDIR);
                return Fail(error, ErrnoText("could not retain the staging parent"));
            }
            created.ParentDescriptor = parentDuplicate;
            created.Descriptor = directory.Release();
            created.Device = static_cast<u64>(status.st_dev);
            created.Inode = static_cast<u64>(status.st_ino);
            return true;
        }

        bool IsCanonicalMemberPath(std::string_view path, std::vector<std::string>& segments)
        {
            segments.clear();
            if (path.empty() || path.size() > 4096)
                return false;
            size_t begin = 0;
            while (begin <= path.size())
            {
                const size_t end = path.find('/', begin);
                const std::string_view segment = path.substr(begin, end == std::string_view::npos ? end : end - begin);
                if (segment.empty() || segment == "." || segment == ".." || segment.size() > 255)
                    return false;
                for (const unsigned char character : segment)
                    if (character < 0x20 || character == 0x7f || character == '\\')
                        return false;
                segments.emplace_back(segment);
                if (end == std::string_view::npos)
                    break;
                begin = end + 1;
            }
            return !segments.empty();
        }

        bool WriteAll(int descriptor, const u8* data, size_t size)
        {
            while (size > 0)
            {
                const ssize_t written = ::write(descriptor, data, size);
                if (written < 0)
                {
                    if (errno == EINTR)
                        continue;
                    return false;
                }
                if (written == 0)
                    return false;
                data += written;
                size -= static_cast<size_t>(written);
            }
            return true;
        }
    }

    bool IsFabStagingSupported()
    {
        return true;
    }

    bool PrepareFabStagingRoot(const std::filesystem::path& root, std::string& error)
    {
        std::error_code filesystemError;
        std::filesystem::path absolute = std::filesystem::absolute(root, filesystemError);
        if (filesystemError || root.empty())
            return Fail(error, "the staging root path is empty or cannot be made absolute");
        absolute = absolute.lexically_normal();
        if (!absolute.has_filename())
            absolute = absolute.parent_path();

        std::filesystem::path current = absolute.root_path();
        const std::filesystem::path relative = absolute.relative_path();
        for (const std::filesystem::path& element : relative)
        {
            current /= element;
            struct stat status {};
            if (::lstat(current.c_str(), &status) != 0)
            {
                if (errno != ENOENT)
                    return Fail(error, ErrnoText("could not inspect a staging root component"));
                if (::mkdir(current.c_str(), 0700) != 0 && errno != EEXIST)
                    return Fail(error, ErrnoText("could not create a staging root component"));
            }
        }
        struct stat status {};
        if (::lstat(absolute.c_str(), &status) != 0 || !IsOwnerOnlyDirectory(status))
            return Fail(error, "the staging root is not a real directory owned by this user with no group/other access");
        return true;
    }

    FabStagingDirectory::~FabStagingDirectory()
    {
        std::string ignored;
        Remove(ignored);
        Reset();
    }

    FabStagingDirectory::FabStagingDirectory(FabStagingDirectory&& other) noexcept
        : m_Path(std::move(other.m_Path))
        , m_Name(std::move(other.m_Name))
        , m_ParentDescriptor(std::exchange(other.m_ParentDescriptor, -1))
        , m_Descriptor(std::exchange(other.m_Descriptor, -1))
        , m_Device(std::exchange(other.m_Device, 0))
        , m_Inode(std::exchange(other.m_Inode, 0))
    {
        other.m_Path.clear();
        other.m_Name.clear();
    }

    FabStagingDirectory& FabStagingDirectory::operator=(FabStagingDirectory&& other) noexcept
    {
        if (this != &other)
        {
            std::string ignored;
            Remove(ignored);
            Reset();
            m_Path = std::move(other.m_Path);
            m_Name = std::move(other.m_Name);
            m_ParentDescriptor = std::exchange(other.m_ParentDescriptor, -1);
            m_Descriptor = std::exchange(other.m_Descriptor, -1);
            m_Device = std::exchange(other.m_Device, 0);
            m_Inode = std::exchange(other.m_Inode, 0);
            other.m_Path.clear();
            other.m_Name.clear();
        }
        return *this;
    }

    void FabStagingDirectory::Reset() noexcept
    {
        if (m_Descriptor >= 0)
            ::close(m_Descriptor);
        if (m_ParentDescriptor >= 0)
            ::close(m_ParentDescriptor);
        m_Descriptor = -1;
        m_ParentDescriptor = -1;
        m_Device = 0;
        m_Inode = 0;
        m_Path.clear();
        m_Name.clear();
    }

    bool FabStagingDirectory::Create(const std::filesystem::path& root, std::string_view prefix,
        FabStagingDirectory& out, std::string& error)
    {
        if (!IsSingleSegmentName(prefix))
            return Fail(error, "the staging directory prefix is not a single portable name");
        ScopedDescriptor rootDescriptor(::open(root.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
        struct stat status {};
        if (!rootDescriptor.IsValid() || ::fstat(rootDescriptor.Get(), &status) != 0 || !IsOwnerOnlyDirectory(status))
            return Fail(error, "the staging root is not an owner-only directory");

        for (int attempt = 0; attempt < 8; ++attempt)
        {
            const std::string name = std::string(prefix) + "-" + RandomSuffix();
            CreatedDirectory created;
            std::string createError;
            if (!CreateOwnerOnlyDirectory(rootDescriptor.Get(), name, created, createError))
            {
                if (createError.starts_with("staging entry already exists"))
                    continue;
                return Fail(error, createError);
            }
            FabStagingDirectory directory;
            directory.m_Path = root / name;
            directory.m_Name = name;
            directory.m_ParentDescriptor = created.ParentDescriptor;
            directory.m_Descriptor = created.Descriptor;
            directory.m_Device = created.Device;
            directory.m_Inode = created.Inode;
            out = std::move(directory);
            error.clear();
            return true;
        }
        return Fail(error, "could not choose an unused staging directory name");
    }

    bool FabStagingDirectory::CreateChild(std::string_view name, FabStagingDirectory& out, std::string& error) const
    {
        if (!IsValid() || !IsSingleSegmentName(name))
            return Fail(error, "the staging child name is invalid or the parent is not valid");
        CreatedDirectory created;
        if (!CreateOwnerOnlyDirectory(m_Descriptor, std::string(name), created, error))
            return false;
        FabStagingDirectory child;
        child.m_Path = m_Path / std::string(name);
        child.m_Name = std::string(name);
        child.m_ParentDescriptor = created.ParentDescriptor;
        child.m_Descriptor = created.Descriptor;
        child.m_Device = created.Device;
        child.m_Inode = created.Inode;
        out = std::move(child);
        error.clear();
        return true;
    }

    bool FabStagingDirectory::RemoveChild(std::string_view name, std::string& error) const
    {
        if (!IsValid() || !IsSingleSegmentName(name))
            return Fail(error, "the staging child name is invalid or the parent is not valid");
        const std::string childName(name);
        struct stat status {};
        if (::fstatat(m_Descriptor, childName.c_str(), &status, AT_SYMLINK_NOFOLLOW) != 0)
        {
            if (errno == ENOENT)
                return true;
            return Fail(error, ErrnoText("could not inspect a staging child"));
        }
        if (!S_ISDIR(status.st_mode))
        {
            if (::unlinkat(m_Descriptor, childName.c_str(), 0) != 0 && errno != ENOENT)
                return Fail(error, ErrnoText("could not remove a staging file"));
            return true;
        }
        ScopedDescriptor child(::openat(m_Descriptor, childName.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
        struct stat opened {};
        if (!child.IsValid() || ::fstat(child.Get(), &opened) != 0
            || !SameIdentity(opened, static_cast<u64>(status.st_dev), static_cast<u64>(status.st_ino)))
            return Fail(error, "a staging child changed while it was removed");
        if (opened.st_uid == ::geteuid() && (opened.st_mode & 0700) != 0700)
            ::fchmod(child.Get(), 0700);
        if (!RemoveContents(child.Get(), 1, error))
            return false;
        child.Close();
        if (::unlinkat(m_Descriptor, childName.c_str(), AT_REMOVEDIR) != 0)
            return Fail(error, ErrnoText("could not remove a staging directory"));
        return true;
    }

    bool FabStagingDirectory::Remove(std::string& error)
    {
        if (!IsValid())
            return true;
        struct stat status {};
        if (::fstatat(m_ParentDescriptor, m_Name.c_str(), &status, AT_SYMLINK_NOFOLLOW) != 0)
        {
            if (errno != ENOENT)
                return Fail(error, ErrnoText("could not inspect the staging directory"));
            Reset();
            return true;
        }
        if (!S_ISDIR(status.st_mode) || !SameIdentity(status, m_Device, m_Inode))
        {
            // The name now belongs to someone else; it is not ours to delete.
            Reset();
            return true;
        }
        if (!RemoveContents(m_Descriptor, 1, error))
            return false;
        if (::unlinkat(m_ParentDescriptor, m_Name.c_str(), AT_REMOVEDIR) != 0 && errno != ENOENT)
            return Fail(error, ErrnoText("could not remove the staging directory"));
        Reset();
        return true;
    }

    bool CopyRegularFileIntoStaging(const std::filesystem::path& source, const FabStagingDirectory& destination,
        std::string_view name, u64 maximumBytes, const std::function<bool()>& isCancelled,
        FabStagedCopy& out, std::string& error)
    {
        if (!destination.IsValid() || !IsSingleSegmentName(name))
            return Fail(error, "the staged copy name is invalid or the destination is not valid");
        const std::string leaf(name);

        ScopedDescriptor input(::open(source.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK));
        if (!input.IsValid())
            return Fail(error, ErrnoText("could not open the source file"));
        struct stat before {};
        if (::fstat(input.Get(), &before) != 0 || !S_ISREG(before.st_mode))
            return Fail(error, "the source is not a regular file");
        if (before.st_size < 0 || static_cast<u64>(before.st_size) > maximumBytes)
            return Fail(error, "the source file is larger than the intake limit");

        ScopedDescriptor output(::openat(destination.m_Descriptor, leaf.c_str(),
            O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600));
        if (!output.IsValid())
            return Fail(error, ErrnoText("could not create the staged copy"));
        struct stat created {};
        if (::fstat(output.Get(), &created) != 0 || !S_ISREG(created.st_mode))
        {
            output.Close();
            ::unlinkat(destination.m_Descriptor, leaf.c_str(), 0);
            return Fail(error, "the staged copy is not a regular file");
        }
        const u64 device = static_cast<u64>(created.st_dev);
        const u64 inode = static_cast<u64>(created.st_ino);
        const auto discard = [&]()
        {
            output.Close();
            struct stat current {};
            if (::fstatat(destination.m_Descriptor, leaf.c_str(), &current, AT_SYMLINK_NOFOLLOW) == 0
                && SameIdentity(current, device, inode))
                ::unlinkat(destination.m_Descriptor, leaf.c_str(), 0);
        };

        Sha256Builder hash;
        std::vector<u8> buffer(kCopyChunkBytes);
        u64 total = 0;
        for (;;)
        {
            if (isCancelled && isCancelled())
            {
                discard();
                return Fail(error, "cancelled");
            }
            const ssize_t count = ::read(input.Get(), buffer.data(), buffer.size());
            if (count < 0)
            {
                if (errno == EINTR || errno == EAGAIN)
                    continue;
                discard();
                return Fail(error, ErrnoText("could not read the source file"));
            }
            if (count == 0)
                break;
            total += static_cast<u64>(count);
            if (total > maximumBytes)
            {
                discard();
                return Fail(error, "the source file grew past the intake limit while it was copied");
            }
            hash.Update(std::span<const u8>(buffer.data(), static_cast<size_t>(count)));
            if (!WriteAll(output.Get(), buffer.data(), static_cast<size_t>(count)))
            {
                discard();
                return Fail(error, ErrnoText("could not write the staged copy"));
            }
        }
        struct stat after {};
        if (::fstat(input.Get(), &after) != 0 || static_cast<u64>(before.st_size) != total
            || static_cast<u64>(after.st_size) != total)
        {
            discard();
            return Fail(error, "the source file changed while it was copied");
        }
        if (::fsync(output.Get()) != 0)
        {
            discard();
            return Fail(error, ErrnoText("could not flush the staged copy"));
        }
        out.Bytes = total;
        out.Sha256 = hash.FinalizeHex();
        error.clear();
        return true;
    }

    FabZipStagingSink::FabZipStagingSink(const FabStagingDirectory& destination, std::function<bool()> isCancelled)
        : m_IsCancelled(std::move(isCancelled))
    {
        if (destination.IsValid())
            m_RootDescriptor = ::fcntl(destination.m_Descriptor, F_DUPFD_CLOEXEC, 0);
    }

    FabZipStagingSink::~FabZipStagingSink()
    {
        DiscardCurrent();
        if (m_RootDescriptor >= 0)
            ::close(m_RootDescriptor);
    }

    void FabZipStagingSink::DiscardCurrent() noexcept
    {
        if (m_FileDescriptor >= 0)
        {
            ::close(m_FileDescriptor);
            struct stat current {};
            if (m_ParentDescriptor >= 0 && ::fstatat(m_ParentDescriptor, m_Leaf.c_str(), &current, AT_SYMLINK_NOFOLLOW) == 0
                && SameIdentity(current, m_Device, m_Inode))
                ::unlinkat(m_ParentDescriptor, m_Leaf.c_str(), 0);
        }
        if (m_ParentDescriptor >= 0)
            ::close(m_ParentDescriptor);
        m_FileDescriptor = -1;
        m_ParentDescriptor = -1;
        m_Leaf.clear();
        m_Device = 0;
        m_Inode = 0;
        m_Declared = 0;
        m_Written = 0;
    }

    bool FabZipStagingSink::BeginFile(std::string_view relativePath, u64 size)
    {
        if (m_RootDescriptor < 0 || m_FileDescriptor >= 0 || (m_IsCancelled && m_IsCancelled()))
            return false;
        std::vector<std::string> segments;
        if (!IsCanonicalMemberPath(relativePath, segments))
            return false;

        ScopedDescriptor directory(::fcntl(m_RootDescriptor, F_DUPFD_CLOEXEC, 0));
        for (size_t index = 0; directory.IsValid() && index + 1 < segments.size(); ++index)
        {
            if (::mkdirat(directory.Get(), segments[index].c_str(), 0700) != 0 && errno != EEXIST)
                return false;
            ScopedDescriptor next(::openat(directory.Get(), segments[index].c_str(),
                O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
            struct stat status {};
            if (!next.IsValid() || ::fstat(next.Get(), &status) != 0 || !S_ISDIR(status.st_mode)
                || status.st_uid != ::geteuid())
                return false;
            directory = std::move(next);
        }
        if (!directory.IsValid())
            return false;

        ScopedDescriptor file(::openat(directory.Get(), segments.back().c_str(),
            O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600));
        if (!file.IsValid())
            return false;
        struct stat created {};
        if (::fstat(file.Get(), &created) != 0 || !S_ISREG(created.st_mode) || created.st_nlink != 1)
        {
            file.Close();
            ::unlinkat(directory.Get(), segments.back().c_str(), 0);
            return false;
        }
        m_Device = static_cast<u64>(created.st_dev);
        m_Inode = static_cast<u64>(created.st_ino);
        m_Leaf = segments.back();
        m_ParentDescriptor = directory.Release();
        m_FileDescriptor = file.Release();
        m_Declared = size;
        m_Written = 0;
        return true;
    }

    bool FabZipStagingSink::Write(std::span<const u8> bytes)
    {
        if (m_FileDescriptor < 0 || (m_IsCancelled && m_IsCancelled()) || bytes.size() > m_Declared - m_Written)
            return false;
        if (!WriteAll(m_FileDescriptor, bytes.data(), bytes.size()))
            return false;
        m_Written += bytes.size();
        return true;
    }

    bool FabZipStagingSink::EndFile()
    {
        if (m_FileDescriptor < 0 || m_Written != m_Declared || ::fsync(m_FileDescriptor) != 0)
            return false;
        struct stat linked {};
        if (::fstatat(m_ParentDescriptor, m_Leaf.c_str(), &linked, AT_SYMLINK_NOFOLLOW) != 0
            || !S_ISREG(linked.st_mode) || !SameIdentity(linked, m_Device, m_Inode) || linked.st_nlink != 1
            || static_cast<u64>(linked.st_size) != m_Declared)
            return false;
        ::close(m_FileDescriptor);
        ::close(m_ParentDescriptor);
        m_FileDescriptor = -1;
        m_ParentDescriptor = -1;
        ++m_CommittedFiles;
        m_CommittedBytes += m_Declared;
        m_Leaf.clear();
        return true;
    }

    void FabZipStagingSink::AbortFile()
    {
        DiscardCurrent();
    }
#else
    bool IsFabStagingSupported()
    {
        return false;
    }

    bool PrepareFabStagingRoot(const std::filesystem::path&, std::string& error)
    {
        return Fail(error, std::string(kUnsupported));
    }

    FabStagingDirectory::~FabStagingDirectory() = default;

    FabStagingDirectory::FabStagingDirectory(FabStagingDirectory&&) noexcept = default;

    FabStagingDirectory& FabStagingDirectory::operator=(FabStagingDirectory&&) noexcept = default;

    void FabStagingDirectory::Reset() noexcept
    {
        m_Path.clear();
    }

    bool FabStagingDirectory::Create(const std::filesystem::path&, std::string_view, FabStagingDirectory&,
        std::string& error)
    {
        return Fail(error, std::string(kUnsupported));
    }

    bool FabStagingDirectory::CreateChild(std::string_view, FabStagingDirectory&, std::string& error) const
    {
        return Fail(error, std::string(kUnsupported));
    }

    bool FabStagingDirectory::RemoveChild(std::string_view, std::string& error) const
    {
        return Fail(error, std::string(kUnsupported));
    }

    bool FabStagingDirectory::Remove(std::string&)
    {
        return true;
    }

    bool CopyRegularFileIntoStaging(const std::filesystem::path&, const FabStagingDirectory&, std::string_view, u64,
        const std::function<bool()>&, FabStagedCopy&, std::string& error)
    {
        return Fail(error, std::string(kUnsupported));
    }

    FabZipStagingSink::FabZipStagingSink(const FabStagingDirectory&, std::function<bool()> isCancelled)
        : m_IsCancelled(std::move(isCancelled))
    {
    }

    FabZipStagingSink::~FabZipStagingSink() = default;

    void FabZipStagingSink::DiscardCurrent() noexcept {}

    bool FabZipStagingSink::BeginFile(std::string_view, u64)
    {
        return false;
    }

    bool FabZipStagingSink::Write(std::span<const u8>)
    {
        return false;
    }

    bool FabZipStagingSink::EndFile()
    {
        return false;
    }

    void FabZipStagingSink::AbortFile() {}
#endif

    bool ExtractFabZipToStaging(const FabArchiveInput& input, const FabArchiveLimits& limits,
        const FabStagingDirectory& destination, const std::function<bool()>& isCancelled,
        u64* outFileCount, u64* outBytes, std::string& error)
    {
        if (!IsFabStagingSupported())
            return Fail(error, std::string(kUnsupported));
        if (!destination.IsValid())
            return Fail(error, "the extraction destination is not valid");
        FabZipStagingSink sink(destination, isCancelled);
        std::string extractError;
        if (!ExtractFabZip(input, limits, sink, extractError))
        {
            if (isCancelled && isCancelled())
                return Fail(error, "cancelled");
            return Fail(error, std::move(extractError));
        }
        if (outFileCount)
            *outFileCount = sink.CommittedFileCount();
        if (outBytes)
            *outBytes = sink.CommittedBytes();
        error.clear();
        return true;
    }
}
