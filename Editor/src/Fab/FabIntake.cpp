#include "FabIntake.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <iterator>
#include <string_view>
#include <vector>
#include <fstream>
#include <system_error>

#if defined(__linux__) || defined(__APPLE__)
    #include <fcntl.h>
    #include <sys/stat.h>
    #include <unistd.h>
    #define SPIRAL_FAB_INTAKE_POSIX 1
#endif

namespace Fab
{
    namespace
    {
        using Engine::u32;
        using Engine::u64;
        using Engine::u8;

        constexpr u64 kZipTailBytes = 22 + 65535;
        constexpr size_t kChunkBytes = 64 * 1024;
        constexpr size_t kMaximumDisplayNameBytes = 96;

        // Read-only handle that never follows a final symlink and refuses
        // anything but a regular file.
        class RegularFile
        {
        public:
            RegularFile() = default;
            RegularFile(const RegularFile&) = delete;
            RegularFile& operator=(const RegularFile&) = delete;
#if SPIRAL_FAB_INTAKE_POSIX
            ~RegularFile()
            {
                if (m_Descriptor >= 0)
                    ::close(m_Descriptor);
            }

            FabIntakeReason Open(const std::filesystem::path& path)
            {
                m_Descriptor = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
                if (m_Descriptor < 0)
                    return errno == ENOENT ? FabIntakeReason::Missing
                        : (errno == ELOOP || errno == ENXIO || errno == EISDIR ? FabIntakeReason::NotRegularObject
                                                                               : FabIntakeReason::IoError);
                struct stat status {};
                if (::fstat(m_Descriptor, &status) != 0)
                    return FabIntakeReason::IoError;
                if (!S_ISREG(status.st_mode))
                    return FabIntakeReason::NotRegularObject;
                m_Size = status.st_size < 0 ? 0 : static_cast<u64>(status.st_size);
                return FabIntakeReason::None;
            }

            bool ReadAt(u64 offset, void* destination, size_t count) const
            {
                u8* bytes = static_cast<u8*>(destination);
                while (count > 0)
                {
                    const ssize_t read = ::pread(m_Descriptor, bytes, count, static_cast<off_t>(offset));
                    if (read < 0 && errno == EINTR)
                        continue;
                    if (read <= 0)
                        return false;
                    bytes += read;
                    offset += static_cast<u64>(read);
                    count -= static_cast<size_t>(read);
                }
                return true;
            }

        private:
            int m_Descriptor = -1;
#else
            FabIntakeReason Open(const std::filesystem::path& path)
            {
                std::error_code error;
                const std::filesystem::file_status status = std::filesystem::symlink_status(path, error);
                if (error || !std::filesystem::exists(status))
                    return FabIntakeReason::Missing;
                if (status.type() != std::filesystem::file_type::regular)
                    return FabIntakeReason::NotRegularObject;
                m_Stream.open(path, std::ios::binary);
                if (!m_Stream)
                    return FabIntakeReason::IoError;
                const std::uintmax_t size = std::filesystem::file_size(path, error);
                if (error)
                    return FabIntakeReason::IoError;
                m_Size = size;
                return FabIntakeReason::None;
            }

            bool ReadAt(u64 offset, void* destination, size_t count) const
            {
                m_Stream.clear();
                m_Stream.seekg(static_cast<std::streamoff>(offset));
                m_Stream.read(static_cast<char*>(destination), static_cast<std::streamsize>(count));
                return static_cast<size_t>(m_Stream.gcount()) == count;
            }

        private:
            mutable std::ifstream m_Stream;
#endif

        public:
            u64 Size() const { return m_Size; }

        private:
            u64 m_Size = 0;
        };

        u32 ReadLe32(const u8* bytes)
        {
            return static_cast<u32>(bytes[0]) | (static_cast<u32>(bytes[1]) << 8)
                | (static_cast<u32>(bytes[2]) << 16) | (static_cast<u32>(bytes[3]) << 24);
        }

        bool IsJsonWhitespace(u8 byte)
        {
            return byte == ' ' || byte == '\t' || byte == '\n' || byte == '\r';
        }

        std::string LowerExtension(const std::filesystem::path& path)
        {
            std::string extension = path.extension().string();
            for (char& character : extension)
            {
                if (character >= 'A' && character <= 'Z')
                    character = static_cast<char>(character - 'A' + 'a');
            }
            return extension;
        }

        bool IsZipContent(const RegularFile& file)
        {
            // A local-file-header start, or the end-of-central-directory
            // signature of an empty archive, plus an end record in the tail
            // (a truncated download has none).
            std::array<u8, 4> head {};
            if (file.Size() < 22 || !file.ReadAt(0, head.data(), head.size()))
                return false;
            const bool localHeader = head[0] == 'P' && head[1] == 'K' && head[2] == 3 && head[3] == 4;
            const bool emptyArchive = head[0] == 'P' && head[1] == 'K' && head[2] == 5 && head[3] == 6;
            return localHeader || emptyArchive;
        }

        bool HasZipEndRecord(const RegularFile& file)
        {
            const u64 tail = std::min<u64>(file.Size(), kZipTailBytes);
            std::vector<u8> bytes(static_cast<size_t>(tail));
            if (bytes.size() < 22 || !file.ReadAt(file.Size() - tail, bytes.data(), bytes.size()))
                return false;
            for (size_t position = bytes.size() - 22 + 1; position-- > 0;)
            {
                const u8* record = bytes.data() + position;
                if (record[0] != 'P' || record[1] != 'K' || record[2] != 5 || record[3] != 6)
                    continue;
                const size_t commentBytes = record[20] | (static_cast<size_t>(record[21]) << 8);
                if (position + 22 + commentBytes == bytes.size())
                    return true;
            }
            return false;
        }

        bool IsGlbContent(const RegularFile& file)
        {
            std::array<u8, 20> header {};
            if (file.Size() < header.size() || file.Size() > 0xFFFFFFFFull || !file.ReadAt(0, header.data(), header.size()))
                return false;
            const bool magic = header[0] == 'g' && header[1] == 'l' && header[2] == 'T' && header[3] == 'F';
            if (!magic || ReadLe32(header.data() + 4) != 2 || ReadLe32(header.data() + 8) != file.Size())
                return false;
            const u32 firstChunkBytes = ReadLe32(header.data() + 12);
            const bool jsonChunk = header[16] == 'J' && header[17] == 'S' && header[18] == 'O' && header[19] == 'N';
            if (!jsonChunk || firstChunkBytes < 2 || firstChunkBytes > file.Size() - header.size())
                return false;
            // The JSON chunk is padded with spaces; its first meaningful byte
            // must open an object.
            u64 offset = header.size();
            const u64 end = header.size() + firstChunkBytes;
            std::array<u8, 64> probe {};
            while (offset < end)
            {
                const size_t count = static_cast<size_t>(std::min<u64>(probe.size(), end - offset));
                if (!file.ReadAt(offset, probe.data(), count))
                    return false;
                for (size_t index = 0; index < count; ++index)
                {
                    if (!IsJsonWhitespace(probe[index]))
                        return probe[index] == '{';
                }
                offset += count;
            }
            return false;
        }

        bool IsGltfContent(const RegularFile& file, u64 maximumBytes)
        {
            static constexpr std::string_view kAssetKey = "\"asset\"";
            if (file.Size() < 2 || file.Size() > maximumBytes)
                return false;

            std::array<u8, 512> edge {};
            const size_t edgeBytes = static_cast<size_t>(std::min<u64>(edge.size(), file.Size()));
            if (!file.ReadAt(0, edge.data(), edgeBytes))
                return false;
            const auto* opening = std::find_if_not(edge.begin(), edge.begin() + edgeBytes, IsJsonWhitespace);
            if (opening == edge.begin() + edgeBytes || *opening != '{')
                return false;
            if (!file.ReadAt(file.Size() - edgeBytes, edge.data(), edgeBytes))
                return false;
            const auto closing = std::find_if_not(
                std::make_reverse_iterator(edge.begin() + edgeBytes), std::make_reverse_iterator(edge.begin()),
                IsJsonWhitespace);
            if (closing == std::make_reverse_iterator(edge.begin()) || *closing != '}')
                return false;

            // The asset block is mandatory in glTF; search every byte once,
            // keeping a key-length overlap between chunks.
            std::vector<char> chunk(kChunkBytes + kAssetKey.size());
            size_t carried = 0;
            for (u64 offset = 0; offset < file.Size();)
            {
                const size_t count = static_cast<size_t>(std::min<u64>(kChunkBytes, file.Size() - offset));
                if (!file.ReadAt(offset, chunk.data() + carried, count))
                    return false;
                const size_t total = carried + count;
                if (std::string_view(chunk.data(), total).find(kAssetKey) != std::string_view::npos)
                    return true;
                carried = std::min(kAssetKey.size() - 1, total);
                std::memmove(chunk.data(), chunk.data() + total - carried, carried);
                offset += count;
            }
            return false;
        }

        FabIntakeClassification Reject(FabIntakeReason reason, u64 size = 0)
        {
            return { FabIntakeKind::Unsupported, reason, size };
        }

        FabIntakeClassification ClassifyFile(const std::filesystem::path& path, const FabIntakeLimits& limits)
        {
            RegularFile file;
            const FabIntakeReason openReason = file.Open(path);
            if (openReason != FabIntakeReason::None)
                return Reject(openReason);
            const u64 size = file.Size();
            if (size == 0)
                return Reject(FabIntakeReason::Empty);
            if (size > limits.MaximumFileBytes)
                return Reject(FabIntakeReason::TooLarge, size);

            FabIntakeKind kind = FabIntakeKind::Unsupported;
            if (IsZipContent(file))
            {
                if (!HasZipEndRecord(file))
                    return Reject(FabIntakeReason::IncompleteZip, size);
                kind = FabIntakeKind::Zip;
            }
            else if (IsGlbContent(file))
            {
                kind = FabIntakeKind::Glb;
            }
            else if (IsGltfContent(file, limits.MaximumGltfJsonBytes))
            {
                kind = FabIntakeKind::Gltf;
            }
            else
            {
                return Reject(FabIntakeReason::UnrecognizedContent, size);
            }

            const std::string extension = LowerExtension(path);
            const bool knownExtension = extension == ".zip" || extension == ".glb" || extension == ".gltf";
            const bool matches = (kind == FabIntakeKind::Zip && extension == ".zip")
                || (kind == FabIntakeKind::Glb && extension == ".glb")
                || (kind == FabIntakeKind::Gltf && extension == ".gltf");
            if (knownExtension && !matches)
                return Reject(FabIntakeReason::ExtensionContentMismatch, size);
            return { kind, FabIntakeReason::None, size };
        }

        FabIntakeClassification ClassifyFolder(const std::filesystem::path& path, const FabIntakeLimits& limits)
        {
            std::error_code error;
            std::filesystem::recursive_directory_iterator iterator(path, std::filesystem::directory_options::none, error);
            if (error)
                return Reject(FabIntakeReason::IoError);
            u64 visited = 0;
            for (const std::filesystem::recursive_directory_iterator end; iterator != end; iterator.increment(error))
            {
                if (error)
                    return Reject(FabIntakeReason::IoError);
                if (++visited > limits.MaximumFolderEntries)
                    return Reject(FabIntakeReason::FolderTooLarge);
                const std::filesystem::file_status status = iterator->symlink_status(error);
                if (error)
                    return Reject(FabIntakeReason::IoError);
                if (status.type() == std::filesystem::file_type::directory)
                {
                    if (static_cast<u32>(iterator.depth()) + 1 >= limits.MaximumFolderDepth)
                        iterator.disable_recursion_pending();
                    continue;
                }
                if (status.type() != std::filesystem::file_type::regular)
                    continue;
                const std::string extension = LowerExtension(iterator->path());
                if (extension != ".glb" && extension != ".gltf")
                    continue;
                const FabIntakeClassification root = ClassifyFile(iterator->path(), limits);
                if (root.Kind == FabIntakeKind::Glb || root.Kind == FabIntakeKind::Gltf)
                    return { FabIntakeKind::Folder, FabIntakeReason::None, 0 };
            }
            return Reject(FabIntakeReason::FolderWithoutGltf);
        }

        std::string DisplayNameOf(const std::filesystem::path& path)
        {
            std::string name = path.filename().string();
            if (name.size() > kMaximumDisplayNameBytes)
                name.resize(kMaximumDisplayNameBytes);
            for (char& character : name)
            {
                const unsigned char value = static_cast<unsigned char>(character);
                if (value < 0x20 || value == 0x7f)
                    character = '?';
            }
            return name;
        }
    }

    FabIntakeClassification ClassifyFabIntakePath(const std::filesystem::path& path, const FabIntakeLimits& limits)
    {
        std::error_code error;
        const std::filesystem::file_status status = std::filesystem::symlink_status(path, error);
        if (error || status.type() == std::filesystem::file_type::not_found)
            return Reject(FabIntakeReason::Missing);
        if (status.type() == std::filesystem::file_type::directory)
            return ClassifyFolder(path, limits);
        if (status.type() != std::filesystem::file_type::regular)
            return Reject(FabIntakeReason::NotRegularObject);
        return ClassifyFile(path, limits);
    }

    FabIntakePlan PlanFabIntake(FabIntakeOrigin origin, std::span<const std::string> utf8Paths, const FabIntakeLimits& limits)
    {
        FabIntakePlan plan;
        std::vector<std::filesystem::path> seen;
        for (const std::string& text : utf8Paths)
        {
            const std::filesystem::path path(std::u8string(text.begin(), text.end()));
            if (seen.size() >= limits.MaximumPathsPerSubmission)
            {
                plan.Rejected.push_back({ DisplayNameOf(path), FabIntakeReason::TooManyPaths });
                continue;
            }
            const std::filesystem::path normal = path.lexically_normal();
            if (std::find(seen.begin(), seen.end(), normal) != seen.end())
                continue;
            seen.push_back(normal);

            const FabIntakeClassification result = ClassifyFabIntakePath(path, limits);
            if (result.Kind == FabIntakeKind::Unsupported)
                plan.Rejected.push_back({ DisplayNameOf(path), result.Reason });
            else
                plan.Accepted.push_back({ path, origin, result.Kind, result.SizeBytes });
        }
        return plan;
    }
}
