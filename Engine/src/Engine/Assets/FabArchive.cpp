#include "Engine/Assets/FabArchive.h"

#ifndef MINIZ_NO_ZLIB_COMPATIBLE_NAMES
    #define MINIZ_NO_ZLIB_COMPATIBLE_NAMES
#endif
#include "miniz.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <limits>
#include <unordered_set>

namespace Engine
{
    namespace
    {
        constexpr u32 EndRecordSignature = 0x06054b50u;
        constexpr u32 Zip64LocatorSignature = 0x07064b50u;
        constexpr u32 CentralSignature = 0x02014b50u;
        constexpr u32 LocalSignature = 0x04034b50u;
        constexpr size_t EndRecordSize = 22;
        constexpr size_t Zip64LocatorSize = 20;
        constexpr size_t CentralHeaderSize = 46;
        constexpr size_t LocalHeaderSize = 30;
        constexpr size_t MaximumEndCommentBytes = 65535;
        constexpr size_t MaximumExtraBytes = 1024;
        constexpr size_t MaximumCommentBytes = 1024;
        constexpr size_t HeadBytes = 512;
        constexpr u16 FlagEncrypted = 1u << 0;
        constexpr u16 FlagDescriptor = 1u << 3;
        constexpr u16 FlagStrongEncryption = 1u << 6;
        constexpr u16 FlagMaskedLocalHeader = 1u << 13;
        constexpr u16 AllowedFlags = 0x0006u | FlagDescriptor | 0x0800u;
        constexpr u16 Zip64ExtraId = 0x0001;
        constexpr u16 MethodStore = 0;
        constexpr u16 MethodDeflate = 8;

        constexpr u32 ModeTypeMask = 0170000u;
        constexpr u32 ModeRegular = 0100000u;
        constexpr u32 ModeDirectory = 0040000u;
        constexpr u32 DosDirectoryAttribute = 0x10u;
        constexpr u32 DosReparseAttribute = 0x400u;

        u16 ReadLe16(const u8* data)
        {
            return static_cast<u16>(data[0] | (data[1] << 8));
        }

        u32 ReadLe32(const u8* data)
        {
            return static_cast<u32>(data[0]) | (static_cast<u32>(data[1]) << 8)
                | (static_cast<u32>(data[2]) << 16) | (static_cast<u32>(data[3]) << 24);
        }

        bool Fail(std::string& error, const char* message)
        {
            error = message;
            return false;
        }

        u64 SaturatingMultiply(u64 left, u64 right)
        {
            if (left != 0 && right > std::numeric_limits<u64>::max() / left)
                return std::numeric_limits<u64>::max();
            return left * right;
        }

        char AsciiLower(char character)
        {
            return character >= 'A' && character <= 'Z' ? static_cast<char>(character - 'A' + 'a') : character;
        }

        std::string AsciiFold(std::string_view text)
        {
            std::string folded(text);
            std::transform(folded.begin(), folded.end(), folded.begin(), AsciiLower);
            return folded;
        }

        bool IsReservedDeviceStem(std::string_view segment)
        {
            const size_t dot = segment.find('.');
            const std::string stem = AsciiFold(segment.substr(0, dot));
            if (stem == "con" || stem == "prn" || stem == "aux" || stem == "nul")
                return true;
            return stem.size() == 4 && (stem.starts_with("com") || stem.starts_with("lpt"))
                && stem[3] >= '1' && stem[3] <= '9';
        }

        bool IsForbiddenPayloadName(std::string_view path)
        {
            static constexpr std::array<std::string_view, 24> forbidden {
                ".zip", ".7z", ".rar", ".tar", ".gz", ".tgz", ".xz", ".bz2", ".zst", ".jar",
                ".exe", ".dll", ".so", ".dylib", ".msi", ".com", ".scr", ".bat", ".cmd",
                ".ps1", ".vbs", ".sh", ".py", ".js"
            };
            const size_t dot = path.find_last_of('.');
            if (dot == std::string_view::npos)
                return false;
            const std::string extension = AsciiFold(path.substr(dot));
            return std::find(forbidden.begin(), forbidden.end(), extension) != forbidden.end();
        }

        struct MagicSignature
        {
            size_t Offset;
            std::string_view Bytes;
        };

        // Archive, executable, and script containers rejected by content so a
        // misleading extension cannot smuggle them through.
        bool HasForbiddenMagic(std::span<const u8> head)
        {
            using namespace std::string_view_literals;
            static constexpr std::array<MagicSignature, 17> signatures {{
                { 0, "PK\x03\x04"sv }, { 0, "PK\x05\x06"sv }, { 0, "PK\x07\x08"sv },
                { 0, "7z\xBC\xAF\x27\x1C"sv }, { 0, "Rar!\x1A\x07"sv }, { 0, "\x1F\x8B"sv },
                { 0, "\xFD" "7zXZ\x00"sv }, { 0, "BZh"sv }, { 257, "ustar"sv },
                { 0, "\x7F" "ELF"sv }, { 0, "\xFE\xED\xFA\xCE"sv }, { 0, "\xFE\xED\xFA\xCF"sv },
                { 0, "\xCE\xFA\xED\xFE"sv }, { 0, "\xCF\xFA\xED\xFE"sv }, { 0, "\xCA\xFE\xBA\xBE"sv },
                { 0, "MZ"sv }, { 0, "#!"sv }
            }};
            for (const MagicSignature& signature : signatures)
            {
                if (head.size() >= signature.Offset + signature.Bytes.size()
                    && std::memcmp(head.data() + signature.Offset, signature.Bytes.data(), signature.Bytes.size()) == 0)
                    return true;
            }
            return false;
        }

        enum class NameFault
        {
            None,
            Unsafe,
            PathTooLong,
            SegmentTooLong,
            TooDeep
        };

        NameFault CanonicalizeName(std::string_view raw, const FabArchiveLimits& limits,
            bool& directory, std::string& canonical)
        {
            canonical.clear();
            directory = !raw.empty() && raw.back() == '/';
            const std::string_view body = directory ? raw.substr(0, raw.size() - 1) : raw;
            if (body.empty() || body.front() == '/')
                return NameFault::Unsafe;
            for (const unsigned char character : body)
            {
                if (character < 0x20 || character > 0x7e || character == '\\' || character == ':' || character == '*'
                    || character == '?' || character == '"' || character == '<' || character == '>' || character == '|')
                    return NameFault::Unsafe;
            }
            if (body.size() > limits.MaximumPathBytes)
                return NameFault::PathTooLong;

            u64 depth = 0;
            size_t begin = 0;
            while (true)
            {
                const size_t end = body.find('/', begin);
                const std::string_view segment = body.substr(begin, end == std::string_view::npos ? std::string_view::npos : end - begin);
                if (segment.empty() || segment == "." || segment == ".." || IsReservedDeviceStem(segment)
                    || segment.back() == '.' || segment.back() == ' ')
                    return NameFault::Unsafe;
                if (segment.size() > limits.MaximumSegmentBytes)
                    return NameFault::SegmentTooLong;
                if (++depth > limits.MaximumDepth)
                    return NameFault::TooDeep;
                if (end == std::string_view::npos)
                    break;
                begin = end + 1;
            }
            canonical.assign(body);
            return NameFault::None;
        }

        const char* NameFaultMessage(NameFault fault)
        {
            switch (fault)
            {
            case NameFault::PathTooLong: return "ZIP member path exceeds policy";
            case NameFault::SegmentTooLong: return "ZIP member path segment exceeds policy";
            case NameFault::TooDeep: return "ZIP member path depth exceeds policy";
            default: return "ZIP member name is unsafe";
            }
        }

        // Returns nullptr when every extra field is well formed and none is Zip64.
        const char* ScanExtraFields(std::span<const u8> extra)
        {
            size_t cursor = 0;
            while (cursor < extra.size())
            {
                if (extra.size() - cursor < 4)
                    return "ZIP extra field is malformed";
                const u16 identifier = ReadLe16(extra.data() + cursor);
                const size_t length = ReadLe16(extra.data() + cursor + 2);
                if (length > extra.size() - cursor - 4)
                    return "ZIP extra field is malformed";
                if (identifier == Zip64ExtraId)
                    return "ZIP64 archives are not admitted";
                cursor += 4 + length;
            }
            return nullptr;
        }

        const char* FlagFault(u16 flags)
        {
            if (flags & (FlagEncrypted | FlagStrongEncryption | FlagMaskedLocalHeader))
                return "ZIP member is encrypted";
            if (flags & ~AllowedFlags)
                return "ZIP member uses an unsupported flag";
            return nullptr;
        }

        class ArchiveReader
        {
        public:
            bool Open(const FabArchiveInput& input, const FabArchiveLimits& limits, std::string& error)
            {
                m_IsFile = input.IsFile;
                if (m_IsFile)
                {
                    std::error_code status;
                    if (!std::filesystem::is_regular_file(input.FilePath, status) || status)
                        return Fail(error, "ZIP input file could not be read");
                    const std::uintmax_t size = std::filesystem::file_size(input.FilePath, status);
                    if (status)
                        return Fail(error, "ZIP input file could not be read");
                    m_Size = size;
                    if (m_Size == 0 || m_Size > limits.MaximumCompressedBytes)
                        return Fail(error, "ZIP compressed input is empty or exceeds policy");
                    m_File.open(input.FilePath, std::ios::binary);
                    if (!m_File)
                        return Fail(error, "ZIP input file could not be read");
                }
                else
                {
                    m_Memory = input.Memory;
                    m_Size = m_Memory.size();
                    if (m_Size == 0 || m_Size > limits.MaximumCompressedBytes)
                        return Fail(error, "ZIP compressed input is empty or exceeds policy");
                }
                return true;
            }

            u64 Size() const { return m_Size; }

            size_t ReadAvailable(u64 offset, void* destination, size_t count)
            {
                if (offset >= m_Size || count == 0)
                    return 0;
                const size_t available = static_cast<size_t>(std::min<u64>(m_Size - offset, count));
                if (!m_IsFile)
                {
                    std::memcpy(destination, m_Memory.data() + offset, available);
                    return available;
                }
                m_File.clear();
                m_File.seekg(static_cast<std::streamoff>(offset));
                m_File.read(static_cast<char*>(destination), static_cast<std::streamsize>(available));
                return m_File ? available : static_cast<size_t>(std::max<std::streamsize>(m_File.gcount(), 0));
            }

            bool Read(u64 offset, void* destination, size_t count)
            {
                if (count == 0)
                    return true;
                return offset <= m_Size && count <= m_Size - offset && ReadAvailable(offset, destination, count) == count;
            }

            static size_t MinizRead(void* opaque, mz_uint64 offset, void* destination, size_t count)
            {
                return static_cast<ArchiveReader*>(opaque)->ReadAvailable(offset, destination, count);
            }

        private:
            std::span<const u8> m_Memory;
            std::ifstream m_File;
            u64 m_Size = 0;
            bool m_IsFile = false;
        };

        struct Member
        {
            u32 CentralIndex = 0;
            std::string RawName;
            std::string Canonical;
            bool Directory = false;
            u16 Flags = 0;
            u16 Method = 0;
            u32 Crc = 0;
            u32 CompressedSize = 0;
            u32 UncompressedSize = 0;
            u32 LocalOffset = 0;
        };

        struct Plan
        {
            std::vector<Member> Files;
            u32 EntryCount = 0;
            u64 CentralOffset = 0;
            u64 CentralSize = 0;
        };

        struct EndRecord
        {
            u64 Offset = 0;
            u32 EntryCount = 0;
            u64 CentralSize = 0;
            u64 CentralOffset = 0;
        };

        bool ReadEndRecord(ArchiveReader& reader, const FabArchiveLimits& limits, EndRecord& end, std::string& error)
        {
            const u64 archiveSize = reader.Size();
            if (archiveSize < EndRecordSize)
                return Fail(error, "ZIP end of central directory is missing or followed by trailing data");
            const size_t tailLength = static_cast<size_t>(
                std::min<u64>(archiveSize, EndRecordSize + MaximumEndCommentBytes + Zip64LocatorSize));
            std::vector<u8> tail(tailLength);
            if (!reader.Read(archiveSize - tailLength, tail.data(), tail.size()))
                return Fail(error, "ZIP input file could not be read");

            size_t record = tailLength;
            for (size_t position = tailLength - EndRecordSize + 1; position-- > 0;)
            {
                if (ReadLe32(tail.data() + position) == EndRecordSignature
                    && position + EndRecordSize + ReadLe16(tail.data() + position + 20) == tailLength)
                {
                    record = position;
                    break;
                }
            }
            if (record == tailLength)
                return Fail(error, "ZIP end of central directory is missing or followed by trailing data");

            const u8* fields = tail.data() + record;
            const u16 thisDisk = ReadLe16(fields + 4);
            const u16 centralDisk = ReadLe16(fields + 6);
            const u16 entriesOnDisk = ReadLe16(fields + 8);
            const u16 entryCount = ReadLe16(fields + 10);
            const u32 centralSize = ReadLe32(fields + 12);
            const u32 centralOffset = ReadLe32(fields + 16);
            const bool locator = record >= Zip64LocatorSize
                && ReadLe32(tail.data() + record - Zip64LocatorSize) == Zip64LocatorSignature;
            if (locator || entryCount == 0xFFFFu || entriesOnDisk == 0xFFFFu
                || centralSize == 0xFFFFFFFFu || centralOffset == 0xFFFFFFFFu)
                return Fail(error, "ZIP64 archives are not admitted");
            if (thisDisk != 0 || centralDisk != 0 || entriesOnDisk != entryCount)
                return Fail(error, "ZIP split or multi-disk archives are not admitted");

            end.Offset = archiveSize - tailLength + record;
            end.EntryCount = entryCount;
            end.CentralSize = centralSize;
            end.CentralOffset = centralOffset;
            if (end.EntryCount > limits.MaximumFileCount)
                return Fail(error, "ZIP file count exceeds policy");
            return true;
        }

        bool ReadCentralDirectory(ArchiveReader& reader, const FabArchiveLimits& limits, const EndRecord& end,
            std::vector<Member>& members, std::string& error)
        {
            const u64 minimumSize = static_cast<u64>(end.EntryCount) * CentralHeaderSize;
            const u64 perEntryMaximum = CentralHeaderSize + limits.MaximumPathBytes + 1
                + MaximumExtraBytes + MaximumCommentBytes;
            const u64 maximumSize = SaturatingMultiply(end.EntryCount, perEntryMaximum);
            if (end.CentralSize < minimumSize || end.CentralSize > maximumSize
                || end.CentralOffset + end.CentralSize != end.Offset)
                return Fail(error, "ZIP central directory is out of range or malformed");

            std::vector<u8> directory(static_cast<size_t>(end.CentralSize));
            if (!reader.Read(end.CentralOffset, directory.data(), directory.size()))
                return Fail(error, "ZIP input file could not be read");

            members.reserve(end.EntryCount);
            size_t cursor = 0;
            for (u32 index = 0; index < end.EntryCount; ++index)
            {
                if (directory.size() - cursor < CentralHeaderSize)
                    return Fail(error, "ZIP central directory is out of range or malformed");
                const u8* header = directory.data() + cursor;
                if (ReadLe32(header) != CentralSignature)
                    return Fail(error, "ZIP central directory is out of range or malformed");
                const size_t nameLength = ReadLe16(header + 28);
                const size_t extraLength = ReadLe16(header + 30);
                const size_t commentLength = ReadLe16(header + 32);
                if (extraLength > MaximumExtraBytes || commentLength > MaximumCommentBytes
                    || nameLength + extraLength + commentLength > directory.size() - cursor - CentralHeaderSize)
                    return Fail(error, "ZIP central directory is out of range or malformed");

                Member member;
                member.CentralIndex = index;
                member.Flags = ReadLe16(header + 8);
                member.Method = ReadLe16(header + 10);
                member.Crc = ReadLe32(header + 16);
                member.CompressedSize = ReadLe32(header + 20);
                member.UncompressedSize = ReadLe32(header + 24);
                member.LocalOffset = ReadLe32(header + 42);
                const u32 attributes = ReadLe32(header + 38);
                member.RawName.assign(reinterpret_cast<const char*>(header + CentralHeaderSize), nameLength);

                const std::span<const u8> extra(header + CentralHeaderSize + nameLength, extraLength);
                if (const char* extraFault = ScanExtraFields(extra))
                    return Fail(error, extraFault);
                if (member.CompressedSize == 0xFFFFFFFFu || member.UncompressedSize == 0xFFFFFFFFu
                    || member.LocalOffset == 0xFFFFFFFFu)
                    return Fail(error, "ZIP64 archives are not admitted");
                if (ReadLe16(header + 34) != 0)
                    return Fail(error, "ZIP split or multi-disk archives are not admitted");
                if (const char* flagFault = FlagFault(member.Flags))
                    return Fail(error, flagFault);
                if (member.Method != MethodStore && member.Method != MethodDeflate)
                    return Fail(error, "ZIP member uses an unsupported compression method");

                const NameFault nameFault = CanonicalizeName(member.RawName, limits, member.Directory, member.Canonical);
                if (nameFault != NameFault::None)
                    return Fail(error, NameFaultMessage(nameFault));
                const u32 mode = attributes >> 16;
                const u32 modeType = mode & ModeTypeMask;
                const bool modeAllowed = modeType == 0 || modeType == ModeRegular
                    || (modeType == ModeDirectory && member.Directory);
                if (!modeAllowed || (attributes & DosReparseAttribute)
                    || ((attributes & DosDirectoryAttribute) && !member.Directory))
                    return Fail(error, "ZIP member is not a regular file or directory");
                if (member.Directory && (member.CompressedSize != 0 || member.UncompressedSize != 0))
                    return Fail(error, "ZIP directory entry carries data");
                if (!member.Directory && IsForbiddenPayloadName(member.Canonical))
                    return Fail(error, "ZIP member is a nested archive or executable");

                members.push_back(std::move(member));
                cursor += CentralHeaderSize + nameLength + extraLength + commentLength;
            }
            if (cursor != directory.size())
                return Fail(error, "ZIP central directory is out of range or malformed");
            return true;
        }

        bool CheckSizeLimits(const std::vector<Member>& members, u64 archiveSize, const FabArchiveLimits& limits,
            std::string& error)
        {
            const u64 aggregateRatioLimit = std::max(limits.MinimumAggregateAllowance,
                SaturatingMultiply(archiveSize, limits.MaximumAggregateExpansion));
            u64 aggregate = 0;
            for (const Member& member : members)
            {
                if (member.Directory)
                    continue;
                if (member.UncompressedSize > limits.MaximumFileBytes)
                    return Fail(error, "ZIP member size exceeds policy");
                if (member.Method == MethodStore && member.CompressedSize != member.UncompressedSize)
                    return Fail(error, "ZIP member size does not match its header");
                if (member.UncompressedSize != 0 && member.CompressedSize == 0)
                    return Fail(error, "ZIP member size does not match its header");
                const u64 perEntryLimit = std::max(limits.MinimumPerEntryAllowance,
                    SaturatingMultiply(member.CompressedSize, limits.MaximumPerEntryExpansion));
                if (member.UncompressedSize > perEntryLimit)
                    return Fail(error, "ZIP member expansion ratio exceeds policy");
                if (member.UncompressedSize > limits.MaximumAggregateBytes - aggregate)
                    return Fail(error, "ZIP aggregate size exceeds policy");
                aggregate += member.UncompressedSize;
                if (aggregate > aggregateRatioLimit)
                    return Fail(error, "ZIP aggregate expansion ratio exceeds policy");
            }
            return true;
        }

        bool CheckNamesAndCollisions(const std::vector<Member>& members, std::string& error)
        {
            std::unordered_set<std::string> exact;
            std::unordered_set<std::string> folded;
            std::unordered_set<std::string> filePaths;
            for (const Member& member : members)
            {
                if (!exact.insert(member.Canonical).second)
                    return Fail(error, "ZIP contains duplicate members");
                if (!folded.insert(AsciiFold(member.Canonical)).second)
                    return Fail(error, "ZIP contains case-colliding members");
                if (!member.Directory)
                    filePaths.insert(AsciiFold(member.Canonical));
            }
            for (const Member& member : members)
            {
                const std::string path = AsciiFold(member.Canonical);
                for (size_t slash = path.find('/'); slash != std::string::npos; slash = path.find('/', slash + 1))
                {
                    if (filePaths.contains(path.substr(0, slash)))
                        return Fail(error, "ZIP member path passes through a file");
                }
            }
            return true;
        }

        bool CheckLocalHeaders(ArchiveReader& reader, const std::vector<Member>& members, u64 centralOffset,
            std::string& error)
        {
            struct Range
            {
                u64 Begin;
                u64 End;
            };
            std::vector<Range> ranges;
            ranges.reserve(members.size());
            for (const Member& member : members)
            {
                std::array<u8, LocalHeaderSize> header {};
                if (member.LocalOffset > centralOffset || centralOffset - member.LocalOffset < LocalHeaderSize
                    || !reader.Read(member.LocalOffset, header.data(), header.size()))
                    return Fail(error, "ZIP local header is out of range");
                const u16 localFlags = ReadLe16(header.data() + 6);
                const size_t nameLength = ReadLe16(header.data() + 26);
                const size_t extraLength = ReadLe16(header.data() + 28);
                if (ReadLe32(header.data()) != LocalSignature || ReadLe16(header.data() + 8) != member.Method
                    || nameLength != member.RawName.size() || extraLength > MaximumExtraBytes)
                    return Fail(error, "ZIP local header disagrees with the central directory");
                if (const char* flagFault = FlagFault(localFlags))
                    return Fail(error, flagFault);
                if (!(localFlags & FlagDescriptor)
                    && (ReadLe32(header.data() + 14) != member.Crc || ReadLe32(header.data() + 18) != member.CompressedSize
                        || ReadLe32(header.data() + 22) != member.UncompressedSize))
                    return Fail(error, "ZIP local header disagrees with the central directory");

                const u64 variableBegin = static_cast<u64>(member.LocalOffset) + LocalHeaderSize;
                const u64 dataBegin = variableBegin + nameLength + extraLength;
                if (dataBegin > centralOffset || member.CompressedSize > centralOffset - dataBegin)
                    return Fail(error, "ZIP local header is out of range");
                std::vector<u8> variable(nameLength + extraLength);
                if (!reader.Read(variableBegin, variable.data(), variable.size()))
                    return Fail(error, "ZIP input file could not be read");
                if (std::memcmp(variable.data(), member.RawName.data(), nameLength) != 0)
                    return Fail(error, "ZIP local header disagrees with the central directory");
                if (const char* extraFault = ScanExtraFields(std::span<const u8>(variable.data() + nameLength, extraLength)))
                    return Fail(error, extraFault);
                ranges.push_back({ member.LocalOffset, dataBegin + member.CompressedSize });
            }
            std::sort(ranges.begin(), ranges.end(), [](const Range& left, const Range& right) { return left.Begin < right.Begin; });
            for (size_t index = 1; index < ranges.size(); ++index)
            {
                if (ranges[index].Begin < ranges[index - 1].End)
                    return Fail(error, "ZIP member data ranges overlap");
            }
            return true;
        }

        bool BuildPlan(ArchiveReader& reader, const FabArchiveLimits& limits, Plan& plan, std::string& error)
        {
            EndRecord end;
            std::vector<Member> members;
            if (!ReadEndRecord(reader, limits, end, error)
                || !ReadCentralDirectory(reader, limits, end, members, error)
                || !CheckSizeLimits(members, reader.Size(), limits, error)
                || !CheckNamesAndCollisions(members, error)
                || !CheckLocalHeaders(reader, members, end.CentralOffset, error))
                return false;

            plan.EntryCount = end.EntryCount;
            plan.CentralOffset = end.CentralOffset;
            plan.CentralSize = end.CentralSize;
            plan.Files.clear();
            for (Member& member : members)
            {
                if (!member.Directory)
                    plan.Files.push_back(std::move(member));
            }
            std::sort(plan.Files.begin(), plan.Files.end(),
                [](const Member& left, const Member& right) { return left.Canonical < right.Canonical; });
            return true;
        }

        enum class StreamFault
        {
            None,
            Overflow,
            Nested,
            Sink
        };

        struct StreamState
        {
            FabArchiveSink* Sink = nullptr;
            const Member* Current = nullptr;
            StreamFault Fault = StreamFault::None;
            u64 Written = 0;
            mz_ulong Crc = MZ_CRC32_INIT;
            std::array<u8, HeadBytes> Head {};
            size_t HeadFill = 0;
            bool Begun = false;

            bool ReleaseHead()
            {
                if (HeadFill != 0 && HasForbiddenMagic(std::span<const u8>(Head.data(), HeadFill)))
                {
                    Fault = StreamFault::Nested;
                    return false;
                }
                if (!Sink->BeginFile(Current->Canonical, Current->UncompressedSize))
                {
                    Fault = StreamFault::Sink;
                    return false;
                }
                Begun = true;
                if (HeadFill != 0 && !Sink->Write(std::span<const u8>(Head.data(), HeadFill)))
                {
                    Fault = StreamFault::Sink;
                    return false;
                }
                return true;
            }
        };

        size_t StreamChunk(void* opaque, mz_uint64, const void* buffer, size_t count)
        {
            StreamState& state = *static_cast<StreamState*>(opaque);
            if (state.Fault != StreamFault::None)
                return 0;
            const u64 declared = state.Current->UncompressedSize;
            if (count > declared - state.Written)
            {
                state.Fault = StreamFault::Overflow;
                return 0;
            }
            const u8* bytes = static_cast<const u8*>(buffer);
            state.Crc = mz_crc32(state.Crc, bytes, count);
            state.Written += count;

            size_t consumed = 0;
            if (!state.Begun)
            {
                const size_t headTarget = static_cast<size_t>(std::min<u64>(declared, HeadBytes));
                consumed = std::min(count, headTarget - state.HeadFill);
                std::memcpy(state.Head.data() + state.HeadFill, bytes, consumed);
                state.HeadFill += consumed;
                // A member that fits in the head stays invisible to the sink
                // until its size and CRC are verified.
                if (declared <= HeadBytes || state.HeadFill < headTarget)
                    return count;
                if (!state.ReleaseHead())
                    return 0;
            }
            if (consumed < count && !state.Sink->Write(std::span<const u8>(bytes + consumed, count - consumed)))
            {
                state.Fault = StreamFault::Sink;
                return 0;
            }
            return count;
        }

        bool MatchesCentralDirectory(mz_zip_archive& zip, const Member& member)
        {
            mz_zip_archive_file_stat stat {};
            if (!mz_zip_reader_file_stat(&zip, member.CentralIndex, &stat)
                || stat.m_crc32 != member.Crc || stat.m_comp_size != member.CompressedSize
                || stat.m_uncomp_size != member.UncompressedSize || stat.m_method != member.Method
                || stat.m_bit_flag != member.Flags || stat.m_local_header_ofs != member.LocalOffset)
                return false;
            const mz_uint length = mz_zip_reader_get_filename(&zip, member.CentralIndex, nullptr, 0);
            if (length != member.RawName.size() + 1)
                return false;
            std::string name(length, '\0');
            if (mz_zip_reader_get_filename(&zip, member.CentralIndex, name.data(), length) != length)
                return false;
            name.resize(length - 1);
            return name == member.RawName;
        }

        bool ExtractMember(mz_zip_archive& zip, const Member& member, FabArchiveSink& sink, std::string& error)
        {
            if (!MatchesCentralDirectory(zip, member))
                return Fail(error, "ZIP central directory changed during extraction");

            StreamState state;
            state.Sink = &sink;
            state.Current = &member;
            mz_zip_clear_last_error(&zip);
            const bool inflated = mz_zip_reader_extract_to_callback(&zip, member.CentralIndex, StreamChunk, &state, 0) != MZ_FALSE;
            const mz_zip_error zipError = mz_zip_peek_last_error(&zip);

            const bool sizeMatches = state.Written == member.UncompressedSize;
            const char* failure = nullptr;
            if (state.Fault == StreamFault::Nested)
                failure = "ZIP member content is a nested archive or executable";
            else if (state.Fault == StreamFault::Sink)
                failure = "ZIP extraction sink rejected the member";
            else if (state.Fault == StreamFault::Overflow)
                failure = "ZIP member decompresses beyond its declared size";
            else if (sizeMatches && static_cast<u32>(state.Crc) != member.Crc)
                failure = "ZIP member CRC mismatch";
            else if (zipError == MZ_ZIP_UNEXPECTED_DECOMPRESSED_SIZE || (inflated && !sizeMatches))
                failure = "ZIP member size does not match its header";
            else if (!inflated || !sizeMatches)
                failure = "ZIP member Deflate stream is corrupt";
            if (!failure && !state.Begun && !state.ReleaseHead())
                failure = state.Fault == StreamFault::Nested ? "ZIP member content is a nested archive or executable"
                                                             : "ZIP extraction sink rejected the member";
            if (!failure && !sink.EndFile())
            {
                state.Fault = StreamFault::Sink;
                failure = "ZIP extraction sink rejected the member";
            }
            if (failure)
            {
                if (state.Begun)
                    sink.AbortFile();
                return Fail(error, failure);
            }
            return true;
        }

        struct ZipReaderEnd
        {
            mz_zip_archive* Zip;
            ~ZipReaderEnd() { mz_zip_reader_end(Zip); }
        };
    }

    FabArchiveInput FabArchiveInput::FromMemory(std::span<const u8> bytes)
    {
        FabArchiveInput input;
        input.Memory = bytes;
        return input;
    }

    FabArchiveInput FabArchiveInput::FromFile(std::filesystem::path path)
    {
        FabArchiveInput input;
        input.FilePath = std::move(path);
        input.IsFile = true;
        return input;
    }

    bool FabMemoryArchiveSink::BeginFile(std::string_view relativePath, u64)
    {
        m_Current = FabMemoryFile {};
        m_Current.RelativePath.assign(relativePath);
        return true;
    }

    bool FabMemoryArchiveSink::Write(std::span<const u8> bytes)
    {
        m_Current.Bytes.insert(m_Current.Bytes.end(), bytes.begin(), bytes.end());
        return true;
    }

    bool FabMemoryArchiveSink::EndFile()
    {
        m_Committed.push_back(std::move(m_Current));
        m_Current = FabMemoryFile {};
        return true;
    }

    void FabMemoryArchiveSink::AbortFile()
    {
        m_Current = FabMemoryFile {};
    }

    bool ValidateFabZip(const FabArchiveInput& input, const FabArchiveLimits& limits,
        std::vector<FabArchiveEntry>& entries, std::string& error)
    {
        error.clear();
        ArchiveReader reader;
        Plan plan;
        if (!reader.Open(input, limits, error) || !BuildPlan(reader, limits, plan, error))
            return false;

        std::vector<FabArchiveEntry> validated;
        validated.reserve(plan.Files.size());
        for (const Member& member : plan.Files)
            validated.push_back({ member.Canonical, member.UncompressedSize, member.CompressedSize, member.Crc });
        entries = std::move(validated);
        return true;
    }

    bool ExtractFabZip(const FabArchiveInput& input, const FabArchiveLimits& limits,
        FabArchiveSink& sink, std::string& error)
    {
        error.clear();
        ArchiveReader reader;
        Plan plan;
        if (!reader.Open(input, limits, error) || !BuildPlan(reader, limits, plan, error))
            return false;

        mz_zip_archive zip {};
        zip.m_pRead = ArchiveReader::MinizRead;
        zip.m_pIO_opaque = &reader;
        if (!mz_zip_reader_init(&zip, reader.Size(), MZ_ZIP_FLAG_DO_NOT_SORT_CENTRAL_DIRECTORY))
            return Fail(error, "ZIP central directory is malformed");
        const ZipReaderEnd guard { &zip };
        if (mz_zip_is_zip64(&zip))
            return Fail(error, "ZIP64 archives are not admitted");
        if (zip.m_total_files != plan.EntryCount || zip.m_central_directory_file_ofs != plan.CentralOffset
            || mz_zip_get_central_dir_size(&zip) != plan.CentralSize)
            return Fail(error, "ZIP central directory changed during extraction");

        for (const Member& member : plan.Files)
        {
            if (!ExtractMember(zip, member, sink, error))
                return false;
        }
        return true;
    }
}
