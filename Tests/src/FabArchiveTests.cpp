#include "FabArchiveTests.h"

#include "Engine/Assets/FabArchive.h"

#ifndef MINIZ_NO_ZLIB_COMPATIBLE_NAMES
    #define MINIZ_NO_ZLIB_COMPATIBLE_NAMES
#endif
#include "miniz.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace
{
    using namespace Engine;

    using Bytes = std::vector<u8>;

    bool Check(bool condition, const std::string& message)
    {
        if (!condition)
            std::cerr << "Fab ZIP admission test failed: " << message << '\n';
        return condition;
    }

    Bytes ToBytes(std::string_view text)
    {
        return Bytes(text.begin(), text.end());
    }

    // Independent oracle: a bitwise CRC-32 that shares no code with miniz.
    u32 Crc32(const Bytes& bytes)
    {
        u32 crc = 0xFFFFFFFFu;
        for (const u8 byte : bytes)
        {
            crc ^= byte;
            for (int bit = 0; bit < 8; ++bit)
                crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
        return ~crc;
    }

    Bytes Pattern(size_t size, u32 seed)
    {
        Bytes bytes(size);
        u32 state = seed;
        for (u8& byte : bytes)
        {
            state = state * 1664525u + 1013904223u;
            byte = static_cast<u8>('a' + ((state >> 24) % 16));
        }
        return bytes;
    }

    Bytes RawDeflate(const Bytes& input)
    {
        if (input.empty())
            return { 0x03, 0x00 };
        size_t size = 0;
        void* heap = tdefl_compress_mem_to_heap(input.data(), input.size(), &size,
            static_cast<int>(tdefl_create_comp_flags_from_zip_params(6, -MZ_DEFAULT_WINDOW_BITS, MZ_DEFAULT_STRATEGY)));
        if (!heap)
            return {};
        Bytes output(static_cast<const u8*>(heap), static_cast<const u8*>(heap) + size);
        mz_free(heap);
        return output;
    }

    void Append16(Bytes& out, u32 value)
    {
        out.push_back(static_cast<u8>(value & 0xFFu));
        out.push_back(static_cast<u8>((value >> 8) & 0xFFu));
    }

    void Append32(Bytes& out, u32 value)
    {
        Append16(out, value & 0xFFFFu);
        Append16(out, value >> 16);
    }

    void Put16(Bytes& bytes, size_t offset, u32 value)
    {
        bytes[offset] = static_cast<u8>(value & 0xFFu);
        bytes[offset + 1] = static_cast<u8>((value >> 8) & 0xFFu);
    }

    void Put32(Bytes& bytes, size_t offset, u32 value)
    {
        Put16(bytes, offset, value & 0xFFFFu);
        Put16(bytes, offset + 2, value >> 16);
    }

    struct Member
    {
        std::string Name;
        Bytes Data;
        u16 Method = 0;
        u16 Flags = 0;
        u16 Made = 0x031E;
        u32 Attr = 0100644u << 16;
        u16 Disk = 0;
        u32 Crc = 0;
        u32 CompressedSize = 0;
        u32 UncompressedSize = 0;
        Bytes CentralExtra;
        Bytes LocalExtra;
        std::string Comment;
    };

    struct Layout
    {
        std::vector<size_t> Local;
        std::vector<size_t> Central;
        size_t CentralStart = 0;
        size_t CentralSize = 0;
        size_t Eocd = 0;
    };

    Member Stored(std::string name, const Bytes& bytes)
    {
        Member member;
        member.Name = std::move(name);
        member.Data = bytes;
        member.Crc = Crc32(bytes);
        member.CompressedSize = static_cast<u32>(bytes.size());
        member.UncompressedSize = static_cast<u32>(bytes.size());
        return member;
    }

    Member Deflated(std::string name, const Bytes& bytes)
    {
        Member member;
        member.Name = std::move(name);
        member.Data = RawDeflate(bytes);
        member.Method = 8;
        member.Crc = Crc32(bytes);
        member.CompressedSize = static_cast<u32>(member.Data.size());
        member.UncompressedSize = static_cast<u32>(bytes.size());
        return member;
    }

    // Header-only fixture: valid structure whose Deflate payload is never inflated.
    Member JunkDeflated(std::string name, u32 compressed, u32 uncompressed)
    {
        Member member;
        member.Name = std::move(name);
        member.Data = Bytes(compressed, 0x55);
        member.Method = 8;
        member.CompressedSize = compressed;
        member.UncompressedSize = uncompressed;
        return member;
    }

    Member Directory(std::string name)
    {
        Member member;
        member.Name = std::move(name);
        member.Attr = (040755u << 16) | 0x10u;
        return member;
    }

    Member Good()
    {
        return Stored("ok.bin", ToBytes("payload bytes"));
    }

    Bytes BuildZip(const std::vector<Member>& members, Layout* layout = nullptr)
    {
        Bytes out;
        Layout local;
        for (const Member& member : members)
        {
            local.Local.push_back(out.size());
            Append32(out, 0x04034b50u);
            Append16(out, 20);
            Append16(out, member.Flags);
            Append16(out, member.Method);
            Append16(out, 0);
            Append16(out, 0x0021);
            Append32(out, member.Crc);
            Append32(out, member.CompressedSize);
            Append32(out, member.UncompressedSize);
            Append16(out, static_cast<u32>(member.Name.size()));
            Append16(out, static_cast<u32>(member.LocalExtra.size()));
            out.insert(out.end(), member.Name.begin(), member.Name.end());
            out.insert(out.end(), member.LocalExtra.begin(), member.LocalExtra.end());
            out.insert(out.end(), member.Data.begin(), member.Data.end());
        }
        local.CentralStart = out.size();
        for (size_t index = 0; index < members.size(); ++index)
        {
            const Member& member = members[index];
            local.Central.push_back(out.size());
            Append32(out, 0x02014b50u);
            Append16(out, member.Made);
            Append16(out, 20);
            Append16(out, member.Flags);
            Append16(out, member.Method);
            Append16(out, 0);
            Append16(out, 0x0021);
            Append32(out, member.Crc);
            Append32(out, member.CompressedSize);
            Append32(out, member.UncompressedSize);
            Append16(out, static_cast<u32>(member.Name.size()));
            Append16(out, static_cast<u32>(member.CentralExtra.size()));
            Append16(out, static_cast<u32>(member.Comment.size()));
            Append16(out, member.Disk);
            Append16(out, 0);
            Append32(out, member.Attr);
            Append32(out, static_cast<u32>(local.Local[index]));
            out.insert(out.end(), member.Name.begin(), member.Name.end());
            out.insert(out.end(), member.CentralExtra.begin(), member.CentralExtra.end());
            out.insert(out.end(), member.Comment.begin(), member.Comment.end());
        }
        local.CentralSize = out.size() - local.CentralStart;
        local.Eocd = out.size();
        Append32(out, 0x06054b50u);
        Append16(out, 0);
        Append16(out, 0);
        Append16(out, static_cast<u32>(members.size()));
        Append16(out, static_cast<u32>(members.size()));
        Append32(out, static_cast<u32>(local.CentralSize));
        Append32(out, static_cast<u32>(local.CentralStart));
        Append16(out, 0);
        if (layout)
            *layout = local;
        return out;
    }

    Bytes BuildZip(const Member& member, Layout* layout = nullptr)
    {
        return BuildZip(std::vector<Member> { member }, layout);
    }

    class RecordingSink final : public FabArchiveSink
    {
    public:
        u64 FailBeginAt = 0;
        u64 FailEndAt = 0;
        u64 FailWriteAfterBytes = ~0ull;
        u64 BeginCalls = 0;
        u64 EndCalls = 0;
        u64 Begun = 0;
        u64 Ends = 0;
        u64 Aborts = 0;
        u64 Writes = 0;
        size_t LargestWrite = 0;
        u64 BytesAccepted = 0;
        FabMemoryArchiveSink Memory;

        u64 Events() const { return BeginCalls + EndCalls + Aborts + Writes; }

        bool Consistent() const
        {
            return Begun == Ends + Aborts && Ends == Memory.Files().size();
        }

        bool Committed(std::string_view path) const
        {
            return std::any_of(Memory.Files().begin(), Memory.Files().end(),
                [path](const FabMemoryFile& file) { return file.RelativePath == path; });
        }

        bool BeginFile(std::string_view relativePath, u64 size) override
        {
            if (++BeginCalls == FailBeginAt)
                return false;
            ++Begun;
            m_Current = 0;
            return Memory.BeginFile(relativePath, size);
        }

        bool Write(std::span<const u8> bytes) override
        {
            if (m_Current + bytes.size() > FailWriteAfterBytes)
                return false;
            ++Writes;
            LargestWrite = std::max(LargestWrite, bytes.size());
            m_Current += bytes.size();
            BytesAccepted += bytes.size();
            return Memory.Write(bytes);
        }

        bool EndFile() override
        {
            if (++EndCalls == FailEndAt)
                return false;
            ++Ends;
            return Memory.EndFile();
        }

        void AbortFile() override
        {
            ++Aborts;
            Memory.AbortFile();
        }

    private:
        u64 m_Current = 0;
    };

    FabArchiveInput In(const Bytes& bytes)
    {
        return FabArchiveInput::FromMemory(bytes);
    }

    bool Contains(const std::string& text, std::string_view needle)
    {
        return text.find(needle) != std::string::npos;
    }

    bool SameEntries(const std::vector<FabArchiveEntry>& left, const std::vector<FabArchiveEntry>& right)
    {
        return std::equal(left.begin(), left.end(), right.begin(), right.end(),
            [](const FabArchiveEntry& a, const FabArchiveEntry& b)
            {
                return a.RelativePath == b.RelativePath && a.UncompressedBytes == b.UncompressedBytes
                    && a.CompressedBytes == b.CompressedBytes && a.Crc32 == b.Crc32;
            });
    }

    // The archive is structurally invalid: validation must fail with the stable
    // substring, leave the caller's entries untouched, and extraction must not
    // reach the sink at all.
    bool ExpectStructuralRejection(const std::string& label, const Bytes& zip, const FabArchiveLimits& limits,
        std::string_view expected)
    {
        const std::vector<FabArchiveEntry> sentinel { { "sentinel", 7, 7, 7 } };
        std::vector<FabArchiveEntry> entries = sentinel;
        std::string error;
        bool ok = Check(!ValidateFabZip(In(zip), limits, entries, error), label + ": validation rejects");
        ok &= Check(Contains(error, expected), label + ": validation error '" + error + "' lacks '" + std::string(expected) + "'");
        ok &= Check(SameEntries(entries, sentinel), label + ": rejected validation leaves entries untouched");

        RecordingSink sink;
        std::string extractError;
        ok &= Check(!ExtractFabZip(In(zip), limits, sink, extractError), label + ": extraction rejects");
        ok &= Check(Contains(extractError, expected), label + ": extraction error '" + extractError + "' lacks '" + std::string(expected) + "'");
        ok &= Check(sink.Events() == 0, label + ": sink receives no event for a structural rejection");
        return ok;
    }

    bool ExpectStructuralRejection(const std::string& label, const Bytes& zip, std::string_view expected)
    {
        return ExpectStructuralRejection(label, zip, FabArchiveLimits {}, expected);
    }

    // Structure validates but streaming must fail; nothing for the offending
    // path may be committed and every begun member is ended or aborted.
    bool ExpectExtractionRejection(const std::string& label, const Bytes& zip, const FabArchiveLimits& limits,
        std::string_view expected, std::string_view path, RecordingSink& sink)
    {
        std::vector<FabArchiveEntry> entries;
        std::string error;
        bool ok = Check(ValidateFabZip(In(zip), limits, entries, error), label + ": structure validates (" + error + ")");
        std::string extractError;
        ok &= Check(!ExtractFabZip(In(zip), limits, sink, extractError), label + ": extraction rejects");
        ok &= Check(Contains(extractError, expected), label + ": extraction error '" + extractError + "' lacks '" + std::string(expected) + "'");
        ok &= Check(!sink.Committed(path), label + ": offending member is never committed");
        ok &= Check(sink.Consistent(), label + ": every begun member is ended or aborted");
        return ok;
    }

    bool ExpectExtractionRejection(const std::string& label, const Bytes& zip, std::string_view expected,
        std::string_view path)
    {
        RecordingSink sink;
        return ExpectExtractionRejection(label, zip, FabArchiveLimits {}, expected, path, sink);
    }

    struct Expected
    {
        std::string Path;
        Bytes Content;
    };

    bool ExpectAccepted(const std::string& label, const FabArchiveInput& input, const FabArchiveLimits& limits,
        const std::vector<Expected>& expected)
    {
        std::vector<FabArchiveEntry> entries;
        std::string error;
        bool ok = Check(ValidateFabZip(input, limits, entries, error), label + ": validates (" + error + ")");
        RecordingSink sink;
        ok &= Check(ExtractFabZip(input, limits, sink, error), label + ": extracts (" + error + ")");
        if (!ok)
            return false;
        ok &= Check(entries.size() == expected.size() && sink.Memory.Files().size() == expected.size(),
            label + ": entry count");
        ok &= Check(sink.Consistent() && sink.Aborts == 0, label + ": sink saw only committed members");
        for (size_t index = 0; ok && index < expected.size(); ++index)
        {
            ok &= Check(entries[index].RelativePath == expected[index].Path
                    && entries[index].UncompressedBytes == expected[index].Content.size()
                    && entries[index].Crc32 == Crc32(expected[index].Content),
                label + ": canonical entry " + expected[index].Path);
            ok &= Check(sink.Memory.Files()[index].RelativePath == expected[index].Path
                    && sink.Memory.Files()[index].Bytes == expected[index].Content,
                label + ": streamed bytes " + expected[index].Path);
        }
        return ok;
    }

    bool ExpectAccepted(const std::string& label, const Bytes& zip, const std::vector<Expected>& expected)
    {
        return ExpectAccepted(label, In(zip), FabArchiveLimits {}, expected);
    }

    bool ValidatesOnly(const Bytes& zip, const FabArchiveLimits& limits, std::string& error)
    {
        std::vector<FabArchiveEntry> entries;
        return ValidateFabZip(In(zip), limits, entries, error);
    }

    // Exact boundary pair: the limit itself is admitted and limit-1 is refused.
    bool ExpectBoundary(const std::string& label, const Bytes& zip, u64 FabArchiveLimits::*field,
        u64 boundary, std::string_view expected)
    {
        FabArchiveLimits limits;
        std::string error;
        limits.*field = boundary;
        bool ok = Check(ValidatesOnly(zip, limits, error), label + ": limit admitted (" + error + ")");
        limits.*field = boundary - 1;
        ok &= Check(!ValidatesOnly(zip, limits, error) && Contains(error, expected),
            label + ": limit-1 rejected with '" + std::string(expected) + "' got '" + error + "'");
        return ok;
    }

    bool TestValidArchives()
    {
        bool ok = Check(Crc32(ToBytes("123456789")) == 0xCBF43926u, "CRC oracle matches the published check value");

        const Bytes alpha = ToBytes("alpha stored bytes");
        const Bytes beta = Pattern(5000, 1);
        const Bytes empty;
        Member emptyDeflate = Deflated("empty-deflate.txt", empty);
        std::vector<Member> members { Stored("z.bin", alpha), Deflated("a/b.bin", beta), Directory("a/"),
            Stored("a.bin", ToBytes("dot sorts before slash")), Stored("Mixed.PNG", ToBytes("\x89PNG\r\n\x1a\n")),
            Stored("empty.txt", empty), emptyDeflate };
        const Bytes zip = BuildZip(members);
        ok &= ExpectAccepted("Store and Deflate", zip,
            { { "Mixed.PNG", ToBytes("\x89PNG\r\n\x1a\n") }, { "a.bin", ToBytes("dot sorts before slash") },
                { "a/b.bin", beta }, { "empty-deflate.txt", empty }, { "empty.txt", empty }, { "z.bin", alpha } });

        std::vector<FabArchiveEntry> entries;
        std::string error;
        ok &= Check(ValidateFabZip(In(zip), FabArchiveLimits {}, entries, error) && entries.size() == 6,
            "directory entries are ignored");
        for (const FabArchiveEntry& entry : entries)
            ok &= Check(entry.RelativePath.back() != '/', "entries are canonical files");
        ok &= Check(entries.size() == 6 && entries[3].CompressedBytes == 2 && entries[2].CompressedBytes < beta.size(),
            "entries report compressed sizes");

        mz_zip_archive writer {};
        mz_zip_zero_struct(&writer);
        bool built = mz_zip_writer_init_heap(&writer, 0, 0) != MZ_FALSE;
        const Bytes gamma = Pattern(70000, 2);
        built = built && mz_zip_writer_add_mem(&writer, "tool/gamma.bin", gamma.data(), gamma.size(), MZ_BEST_COMPRESSION)
            && mz_zip_writer_add_mem(&writer, "tool/", nullptr, 0, MZ_DEFAULT_LEVEL)
            && mz_zip_writer_add_mem(&writer, "tool/tiny.txt", "tiny", 4, MZ_NO_COMPRESSION);
        void* heap = nullptr;
        size_t heapSize = 0;
        built = built && mz_zip_writer_finalize_heap_archive(&writer, &heap, &heapSize);
        ok &= Check(built, "miniz writer fixture builds");
        if (built)
        {
            const Bytes third(static_cast<const u8*>(heap), static_cast<const u8*>(heap) + heapSize);
            ok &= ExpectAccepted("miniz-written archive", third, { { "tool/gamma.bin", gamma }, { "tool/tiny.txt", ToBytes("tiny") } });
        }
        mz_free(heap);
        mz_zip_writer_end(&writer);

        Member descriptor = Stored("descriptor.bin", alpha);
        descriptor.Flags = 0x0008;
        Layout layout;
        Bytes withDescriptor = BuildZip(descriptor, &layout);
        Put32(withDescriptor, layout.Local[0] + 14, 0);
        Put32(withDescriptor, layout.Local[0] + 18, 0);
        Put32(withDescriptor, layout.Local[0] + 22, 0);
        ok &= ExpectAccepted("data-descriptor local sizes may be zero", withDescriptor, { { "descriptor.bin", alpha } });

        Member executableBit = Good();
        executableBit.Attr = 0100755u << 16;
        ok &= ExpectAccepted("regular file with executable permission bit", BuildZip(executableBit),
            { { "ok.bin", ToBytes("payload bytes") } });
        Member ntfsHost = Good();
        ntfsHost.Made = 0x0A00 | 45;
        ntfsHost.Attr = 0x20;
        ok &= ExpectAccepted("NTFS-host archive attribute", BuildZip(ntfsHost), { { "ok.bin", ToBytes("payload bytes") } });

        for (const char* name : { "COM0.txt", "COM10", "console.txt", "a b.txt", ".hidden", "a.b.c", "UPPER/lower.bin" })
        {
            Member member = Stored(name, ToBytes("x"));
            ok &= ExpectAccepted(std::string("name admitted: ") + name, BuildZip(member), { { name, ToBytes("x") } });
        }

        std::vector<Member> nested { Directory("d/"), Stored("d/x.txt", ToBytes("x")) };
        ok &= ExpectAccepted("directory beside its child", BuildZip(nested), { { "d/x.txt", ToBytes("x") } });

        ok &= ExpectAccepted("empty archive", BuildZip(std::vector<Member> {}), {});
        return ok;
    }

    class ScopedTempDirectory
    {
    public:
        ScopedTempDirectory()
        {
            static std::atomic<u64> sequence { 0 };
            const u64 timestamp = static_cast<u64>(std::chrono::steady_clock::now().time_since_epoch().count());
            std::error_code error;
            m_Path = std::filesystem::temp_directory_path(error)
                / ("FabArchiveTests-" + std::to_string(timestamp) + "-" + std::to_string(sequence.fetch_add(1)));
            m_Created = !error && std::filesystem::create_directory(m_Path, error) && !error;
        }

        ~ScopedTempDirectory()
        {
            std::error_code error;
            if (m_Created)
                std::filesystem::remove_all(m_Path, error);
        }

        ScopedTempDirectory(const ScopedTempDirectory&) = delete;
        ScopedTempDirectory& operator=(const ScopedTempDirectory&) = delete;

        bool IsReady() const { return m_Created; }
        const std::filesystem::path& Path() const { return m_Path; }

    private:
        std::filesystem::path m_Path;
        bool m_Created = false;
    };

    bool WriteFile(const std::filesystem::path& path, const Bytes& bytes)
    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        return static_cast<bool>(output);
    }

    bool TestFilePathInput()
    {
        ScopedTempDirectory directory;
        if (!Check(directory.IsReady(), "unique temporary directory creation"))
            return false;

        const Bytes alpha = Pattern(200000, 3);
        const Bytes beta = Pattern(900000, 4);
        const Bytes zip = BuildZip(std::vector<Member> { Stored("big.bin", alpha), Deflated("sub/deflated.bin", beta) });
        const std::filesystem::path archive = directory.Path() / "fixture.zip";
        bool ok = Check(WriteFile(archive, zip), "fixture archive written");
        ok &= ExpectAccepted("file input", FabArchiveInput::FromFile(archive), FabArchiveLimits {},
            { { "big.bin", alpha }, { "sub/deflated.bin", beta } });

        FabArchiveLimits limits;
        limits.MaximumCompressedBytes = zip.size();
        ok &= ExpectAccepted("file size equals compressed limit", FabArchiveInput::FromFile(archive), limits,
            { { "big.bin", alpha }, { "sub/deflated.bin", beta } });
        limits.MaximumCompressedBytes = zip.size() - 1;
        std::vector<FabArchiveEntry> entries;
        std::string error;
        ok &= Check(!ValidateFabZip(FabArchiveInput::FromFile(archive), limits, entries, error) && Contains(error, "exceeds policy"),
            "file one byte over the compressed limit is rejected before parsing");

        RecordingSink sink;
        ok &= Check(!ExtractFabZip(FabArchiveInput::FromFile(directory.Path() / "missing.zip"), FabArchiveLimits {}, sink, error)
                && Contains(error, "could not be read"),
            "missing file is rejected");
        ok &= Check(!ExtractFabZip(FabArchiveInput::FromFile(directory.Path()), FabArchiveLimits {}, sink, error)
                && Contains(error, "could not be read"),
            "directory path is rejected as a non-regular input");
        ok &= Check(WriteFile(directory.Path() / "empty.zip", {})
                && !ExtractFabZip(FabArchiveInput::FromFile(directory.Path() / "empty.zip"), FabArchiveLimits {}, sink, error)
                && Contains(error, "empty or exceeds"),
            "zero-length file is rejected");
        Bytes truncated(zip.begin(), zip.end() - 40);
        ok &= Check(WriteFile(directory.Path() / "truncated.zip", truncated)
                && !ExtractFabZip(FabArchiveInput::FromFile(directory.Path() / "truncated.zip"), FabArchiveLimits {}, sink, error)
                && Contains(error, "end of central directory"),
            "truncated file is rejected");

        Bytes corrupt = zip;
        Layout layout;
        BuildZip(std::vector<Member> { Stored("big.bin", alpha), Deflated("sub/deflated.bin", beta) }, &layout);
        corrupt[layout.Local[0] + 30 + 7 + 150000] ^= 0x40;
        ok &= Check(WriteFile(directory.Path() / "corrupt.zip", corrupt), "corrupt fixture written");
        RecordingSink corruptSink;
        ok &= Check(!ExtractFabZip(FabArchiveInput::FromFile(directory.Path() / "corrupt.zip"), FabArchiveLimits {}, corruptSink, error)
                && Contains(error, "CRC mismatch") && corruptSink.Aborts == 1 && !corruptSink.Committed("big.bin"),
            "file input CRC corruption aborts the in-flight member");
        return ok;
    }

    bool TestStreamingChunks()
    {
        const Bytes stored = Pattern(300000, 5);
        const Bytes deflated = Pattern(3 * 1024 * 1024, 6);
        const Bytes zip = BuildZip(std::vector<Member> { Stored("stored.bin", stored), Deflated("deflated.bin", deflated) });
        RecordingSink sink;
        std::string error;
        bool ok = Check(ExtractFabZip(In(zip), FabArchiveLimits {}, sink, error), "large members extract (" + error + ")");
        ok &= Check(sink.LargestWrite > 0 && sink.LargestWrite <= 64 * 1024, "no Write exceeds 64 KiB");
        ok &= Check(sink.Writes > 40 && sink.BytesAccepted == stored.size() + deflated.size(), "members arrive as many bounded chunks");
        ok &= Check(sink.Memory.Files().size() == 2 && sink.Memory.Files()[0].Bytes == deflated && sink.Memory.Files()[1].Bytes == stored,
            "chunks reassemble to the exact bytes");
        return ok;
    }

    bool TestZip64SplitAndFlags()
    {
        Layout layout;
        const Bytes good = BuildZip(Good(), &layout);
        bool ok = true;

        Bytes locator = good;
        Bytes locatorBytes(20, 0);
        Put32(locatorBytes, 0, 0x07064b50u);
        locator.insert(locator.begin() + static_cast<std::ptrdiff_t>(layout.Eocd), locatorBytes.begin(), locatorBytes.end());
        ok &= ExpectStructuralRejection("Zip64 end-of-central-directory locator", locator, "ZIP64");

        Bytes sentinelTotal = good;
        Put16(sentinelTotal, layout.Eocd + 8, 0xFFFF);
        Put16(sentinelTotal, layout.Eocd + 10, 0xFFFF);
        ok &= ExpectStructuralRejection("Zip64 entry-count sentinel", sentinelTotal, "ZIP64");
        Bytes sentinelOffset = good;
        Put32(sentinelOffset, layout.Eocd + 16, 0xFFFFFFFFu);
        ok &= ExpectStructuralRejection("Zip64 central offset sentinel", sentinelOffset, "ZIP64");
        Bytes sentinelSize = good;
        Put32(sentinelSize, layout.Eocd + 12, 0xFFFFFFFFu);
        ok &= ExpectStructuralRejection("Zip64 central size sentinel", sentinelSize, "ZIP64");

        for (const size_t field : { size_t { 20 }, size_t { 24 }, size_t { 42 } })
        {
            Bytes sentinelEntry = good;
            Put32(sentinelEntry, layout.Central[0] + field, 0xFFFFFFFFu);
            ok &= ExpectStructuralRejection("Zip64 entry sentinel at +" + std::to_string(field), sentinelEntry, "ZIP64");
        }

        Member centralExtra = Good();
        centralExtra.CentralExtra = { 0x01, 0x00, 0x08, 0x00, 0, 0, 0, 0, 0, 0, 0, 0 };
        ok &= ExpectStructuralRejection("Zip64 extra field in the central directory", BuildZip(centralExtra), "ZIP64");
        Member localExtra = Good();
        localExtra.LocalExtra = { 0x01, 0x00, 0x08, 0x00, 0, 0, 0, 0, 0, 0, 0, 0 };
        ok &= ExpectStructuralRejection("Zip64 extra field in a local header", BuildZip(localExtra), "ZIP64");
        Member harmlessExtra = Good();
        harmlessExtra.CentralExtra = { 0x55, 0x54, 0x01, 0x00, 0x03 };
        harmlessExtra.LocalExtra = harmlessExtra.CentralExtra;
        ok &= ExpectAccepted("harmless extra field", BuildZip(harmlessExtra), { { "ok.bin", ToBytes("payload bytes") } });
        Member brokenExtra = Good();
        brokenExtra.CentralExtra = { 0x99, 0x99, 0xFF, 0x00, 0x01 };
        ok &= ExpectStructuralRejection("overrunning extra field", BuildZip(brokenExtra), "extra field is malformed");

        Bytes thisDisk = good;
        Put16(thisDisk, layout.Eocd + 4, 1);
        ok &= ExpectStructuralRejection("split archive: this disk", thisDisk, "split");
        Bytes centralDisk = good;
        Put16(centralDisk, layout.Eocd + 6, 1);
        ok &= ExpectStructuralRejection("split archive: central directory disk", centralDisk, "split");
        Bytes entriesOnDisk = good;
        Put16(entriesOnDisk, layout.Eocd + 8, 2);
        ok &= ExpectStructuralRejection("split archive: entries on disk differ", entriesOnDisk, "split");
        Member diskStart = Good();
        diskStart.Disk = 1;
        ok &= ExpectStructuralRejection("split archive: member starts on another disk", BuildZip(diskStart), "split");

        for (const u16 flag : { u16 { 1 }, u16 { 1 << 6 }, u16 { 1 << 13 } })
        {
            Member member = Good();
            member.Flags = flag;
            ok &= ExpectStructuralRejection("encryption flag " + std::to_string(flag), BuildZip(member), "encrypted");
        }
        for (const u16 flag : { u16 { 1 << 4 }, u16 { 1 << 5 }, u16 { 1 << 7 }, u16 { 1 << 14 }, u16 { 1 << 15 } })
        {
            Member member = Good();
            member.Flags = flag;
            ok &= ExpectStructuralRejection("unsupported flag " + std::to_string(flag), BuildZip(member), "unsupported flag");
        }
        Bytes localEncrypted = good;
        Put16(localEncrypted, layout.Local[0] + 6, 1);
        ok &= ExpectStructuralRejection("encrypted flag only in the local header", localEncrypted, "encrypted");
        for (const u16 method : { u16 { 1 }, u16 { 9 }, u16 { 12 }, u16 { 14 }, u16 { 93 }, u16 { 99 } })
        {
            Member member = Good();
            member.Method = method;
            ok &= ExpectStructuralRejection("compression method " + std::to_string(method), BuildZip(member), "compression method");
        }
        return ok;
    }

    bool TestEntryTypes()
    {
        bool ok = true;
        struct TypeCase
        {
            const char* Label;
            u16 Made;
            u32 Attr;
        };
        const TypeCase cases[] {
            { "symlink", 0x031E, 0120777u << 16 },
            { "block device", 0x031E, 0060660u << 16 },
            { "character device", 0x031E, 0020660u << 16 },
            { "fifo", 0x031E, 0010644u << 16 },
            { "socket", 0x031E, 0140755u << 16 },
            { "directory mode on a file name", 0x031E, 0040755u << 16 },
            { "symlink with non-Unix host byte", 0x000A, 0120777u << 16 },
            { "DOS directory attribute on a file name", 0x000A, 0x10 },
            { "NTFS reparse attribute", 0x0A00, 0x400 }
        };
        for (const TypeCase& typeCase : cases)
        {
            Member member = Good();
            member.Made = typeCase.Made;
            member.Attr = typeCase.Attr;
            ok &= ExpectStructuralRejection(typeCase.Label, BuildZip(member), "not a regular file");
        }
        Member directoryWithData = Directory("d/");
        directoryWithData.Data = ToBytes("xx");
        directoryWithData.CompressedSize = 2;
        directoryWithData.UncompressedSize = 2;
        ok &= ExpectStructuralRejection("directory entry that carries data", BuildZip(directoryWithData), "carries data");
        Member symlinkDirectory = Directory("d/");
        symlinkDirectory.Attr = 0120777u << 16;
        ok &= ExpectStructuralRejection("symlink mode on a directory name", BuildZip(symlinkDirectory), "not a regular file");
        return ok;
    }

    bool TestMalformedStructure()
    {
        Layout layout;
        const Bytes good = BuildZip(Good(), &layout);
        bool ok = true;

        ok &= ExpectStructuralRejection("empty input", Bytes {}, "empty or exceeds");
        ok &= ExpectStructuralRejection("21-byte input", Bytes(21, 'P'), "end of central directory");
        ok &= ExpectStructuralRejection("truncated end record", Bytes(good.begin(), good.end() - 10), "end of central directory");
        ok &= ExpectStructuralRejection("truncated to the first local header", Bytes(good.begin(), good.begin() + 20), "end of central directory");
        ok &= ExpectStructuralRejection("end record only claims entries", Bytes(good.begin() + static_cast<std::ptrdiff_t>(layout.Eocd), good.end()),
            "central directory");

        Bytes trailing = good;
        trailing.insert(trailing.end(), { 'j', 'u', 'n', 'k' });
        ok &= ExpectStructuralRejection("trailing garbage after the end record", trailing, "trailing data");
        Bytes commentLie = good;
        Put16(commentLie, layout.Eocd + 20, 5);
        ok &= ExpectStructuralRejection("comment length claims missing bytes", commentLie, "trailing data");
        Bytes prefixed = good;
        prefixed.insert(prefixed.begin(), 8, 'x');
        ok &= ExpectStructuralRejection("prefix data before the first local header", prefixed, "central directory");
        Bytes polyglot = good;
        Put16(polyglot, layout.Eocd + 20, 22);
        polyglot.insert(polyglot.end(), good.begin() + static_cast<std::ptrdiff_t>(layout.Eocd), good.end());
        ok &= ExpectStructuralRejection("second end record hidden inside the comment", polyglot, "central directory");

        Bytes offsetPast = good;
        Put32(offsetPast, layout.Eocd + 16, static_cast<u32>(layout.CentralStart + 1));
        ok &= ExpectStructuralRejection("central directory offset past its position", offsetPast, "central directory is out of range");
        Bytes offsetBefore = good;
        Put32(offsetBefore, layout.Eocd + 16, static_cast<u32>(layout.CentralStart - 1));
        ok &= ExpectStructuralRejection("central directory offset before its position", offsetBefore, "central directory is out of range");
        Bytes sizeHuge = good;
        Put32(sizeHuge, layout.Eocd + 12, 0xFFFFFFFEu);
        ok &= ExpectStructuralRejection("central directory size near 4 GiB", sizeHuge, "central directory is out of range");
        Bytes sizeSmall = good;
        Put32(sizeSmall, layout.Eocd + 12, 20);
        ok &= ExpectStructuralRejection("central directory smaller than one header", sizeSmall, "central directory is out of range");
        Bytes countHigh = good;
        Put16(countHigh, layout.Eocd + 8, 4096);
        Put16(countHigh, layout.Eocd + 10, 4096);
        ok &= ExpectStructuralRejection("entry count larger than the directory holds", countHigh, "central directory is out of range");
        Bytes countLow = good;
        Put16(countLow, layout.Eocd + 8, 0);
        Put16(countLow, layout.Eocd + 10, 0);
        ok &= ExpectStructuralRejection("entry count smaller than the directory holds", countLow, "central directory is out of range");
        Bytes badSignature = good;
        Put32(badSignature, layout.Central[0], 0x12345678u);
        ok &= ExpectStructuralRejection("central header signature", badSignature, "central directory is out of range");
        Bytes nameOverrun = good;
        Put16(nameOverrun, layout.Central[0] + 28, 4000);
        ok &= ExpectStructuralRejection("central name length overruns the directory", nameOverrun, "central directory is out of range");
        Bytes commentOverrun = good;
        Put16(commentOverrun, layout.Central[0] + 32, 900);
        ok &= ExpectStructuralRejection("central comment overruns the directory", commentOverrun, "central directory is out of range");

        Bytes localOffset = good;
        Put32(localOffset, layout.Central[0] + 42, 0x7FFFFFF0u);
        ok &= ExpectStructuralRejection("local header offset out of range", localOffset, "local header is out of range");
        Member hugeCompressed = Good();
        hugeCompressed.CompressedSize = 0x00FFFFFFu;
        hugeCompressed.UncompressedSize = 0x00FFFFFFu;
        ok &= ExpectStructuralRejection("compressed size beyond the data area", BuildZip(hugeCompressed), "local header is out of range");
        Member zeroCompressed = JunkDeflated("zero.bin", 0, 100);
        ok &= ExpectStructuralRejection("data declared with no compressed bytes", BuildZip(zeroCompressed), "does not match its header");
        Member storedLie = Good();
        storedLie.UncompressedSize += 1;
        ok &= ExpectStructuralRejection("stored member whose sizes differ", BuildZip(storedLie), "does not match its header");

        Bytes localSignature = good;
        Put32(localSignature, layout.Local[0], 0);
        ok &= ExpectStructuralRejection("local header signature", localSignature, "disagrees");
        Bytes localName = good;
        localName[layout.Local[0] + 30] ^= 0x01;
        ok &= ExpectStructuralRejection("local name differs from the central name", localName, "disagrees");
        Bytes localNameLength = good;
        Put16(localNameLength, layout.Local[0] + 26, 3);
        ok &= ExpectStructuralRejection("local name length differs", localNameLength, "disagrees");
        Bytes localMethod = good;
        Put16(localMethod, layout.Local[0] + 8, 8);
        ok &= ExpectStructuralRejection("local method differs", localMethod, "disagrees");
        Bytes localCrc = good;
        Put32(localCrc, layout.Local[0] + 14, 1);
        ok &= ExpectStructuralRejection("local CRC differs without a data descriptor", localCrc, "disagrees");
        Bytes localSize = good;
        Put32(localSize, layout.Local[0] + 22, 99);
        ok &= ExpectStructuralRejection("local size differs without a data descriptor", localSize, "disagrees");

        const Bytes inner = BuildZip(Stored("b.bin", ToBytes("inner payload")));
        Layout innerLayout;
        BuildZip(Stored("b.bin", ToBytes("inner payload")), &innerLayout);
        const Bytes embedded(inner.begin(), inner.begin() + static_cast<std::ptrdiff_t>(innerLayout.CentralStart));
        Layout overlapLayout;
        Bytes overlap = BuildZip(std::vector<Member> { Stored("a.bin", embedded), Stored("b.bin", ToBytes("inner payload")) }, &overlapLayout);
        Put32(overlap, overlapLayout.Central[1] + 42, static_cast<u32>(overlapLayout.Local[0] + 30 + 5));
        ok &= ExpectStructuralRejection("member whose local record hides inside another member", overlap, "overlap");
        Bytes sharedHeader = BuildZip(std::vector<Member> { Stored("a.bin", ToBytes("one")), Stored("b.bin", ToBytes("two")) }, &overlapLayout);
        Put32(sharedHeader, overlapLayout.Central[1] + 42, static_cast<u32>(overlapLayout.Local[0]));
        ok &= ExpectStructuralRejection("two central entries share one local record", sharedHeader, "disagrees");

        Member longName = Good();
        longName.Name = std::string(65535, 'a');
        ok &= ExpectStructuralRejection("65535-byte central name", BuildZip(longName), "central directory is out of range");
        Member longComment = Good();
        longComment.Comment = std::string(2000, 'c');
        ok &= ExpectStructuralRejection("2000-byte member comment", BuildZip(longComment), "central directory is out of range");
        Member longExtra = Good();
        longExtra.CentralExtra = Bytes(2000, 0);
        ok &= ExpectStructuralRejection("2000-byte central extra", BuildZip(longExtra), "central directory is out of range");
        return ok;
    }

    bool TestNames()
    {
        bool ok = true;
        struct NameCase
        {
            const char* Label;
            std::string Name;
        };
        const NameCase unsafe[] {
            { "empty name", "" }, { "dot", "." }, { "dot-dot", ".." }, { "slash only", "/" },
            { "parent traversal", "../x" }, { "inner traversal", "a/../b" }, { "dot segment prefix", "./a" },
            { "dot segment inner", "a/./b" }, { "empty segment", "a//b" }, { "absolute", "/abs" },
            { "double slash absolute", "//a" }, { "leading backslash", "\\evil" }, { "inner backslash", "a\\b" },
            { "backslash traversal", "..\\x" }, { "drive colon", "C:evil" }, { "inner colon", "a:b" },
            { "embedded NUL", std::string("a\0b", 3) }, { "control byte", "a\x01" "b" }, { "DEL byte", "a\x7f" },
            { "non-ASCII UTF-8", "caf\xC3\xA9" }, { "high byte", "a\x80" }, { "asterisk", "w*ld" },
            { "question mark", "q?x" }, { "quote", "q\"x" }, { "less-than", "l<s" }, { "greater-than", "g>t" },
            { "pipe", "p|e" }, { "trailing dot", "trail." }, { "trailing space", "trail " },
            { "trailing-dot directory segment", "dir./f" }, { "CON", "CON" }, { "con with extension", "con.txt" },
            { "nested AUX", "aux/x" }, { "COM1 with extension", "COM1.png" }, { "lpt9", "lpt9" },
            { "NUL in the middle", "a/NUL.txt/b" }, { "PRN", "Prn" }, { "dot-dot directory entry", "../d/" },
            { "root directory entry", "/" }
        };
        for (const NameCase& nameCase : unsafe)
        {
            Member member = Stored(nameCase.Name, ToBytes("x"));
            if (!nameCase.Name.empty() && nameCase.Name.back() == '/')
                member = Directory(nameCase.Name);
            ok &= ExpectStructuralRejection(std::string("name: ") + nameCase.Label, BuildZip(member), "name is unsafe");
        }

        for (const char* name : { "a.zip", "b.EXE", "c/d.sh", "e.tar.gz", "f.7z", "g.Dll", "h.py", "i.js", "j.ps1", "k.bat", "l.rar",
                 "m.so", "n.dylib", "o.xz", "p.jar", "q.msi" })
        {
            ok &= ExpectStructuralRejection(std::string("nested or executable extension: ") + name,
                BuildZip(Stored(name, ToBytes("x"))), "nested archive or executable");
        }

        const std::string longAdmitted = std::string(200, 'a') + "/" + std::string(200, 'b') + "/" + std::string(202, 'c');
        ok &= ExpectAccepted("602-byte path is neither truncated nor rejected", BuildZip(Stored(longAdmitted, ToBytes("x"))),
            { { longAdmitted, ToBytes("x") } });
        const std::string hiddenTraversal = std::string(200, 'a') + "/" + std::string(200, 'b') + "/" + std::string(150, 'c') + "/../evil";
        ok &= Check(hiddenTraversal.size() > 512, "traversal fixture lies beyond miniz's 512-byte name buffer");
        ok &= ExpectStructuralRejection("traversal beyond 512 bytes", BuildZip(Stored(hiddenTraversal, ToBytes("x"))), "name is unsafe");
        const std::string hiddenExtension = std::string(200, 'a') + "/" + std::string(200, 'b') + "/" + std::string(150, 'c') + ".exe";
        ok &= ExpectStructuralRejection("executable extension beyond 512 bytes", BuildZip(Stored(hiddenExtension, ToBytes("x"))),
            "nested archive or executable");
        const std::string tooLong = std::string(205, 'a') + "/" + std::string(205, 'b') + "/" + std::string(205, 'c') + "/"
            + std::string(205, 'd') + "/" + std::string(205, 'e');
        ok &= ExpectStructuralRejection("1025-byte path", BuildZip(Stored(tooLong, ToBytes("x"))), "path exceeds policy");

        ok &= ExpectStructuralRejection("duplicate exact names",
            BuildZip(std::vector<Member> { Stored("a.txt", ToBytes("1")), Stored("a.txt", ToBytes("2")) }), "duplicate");
        ok &= ExpectStructuralRejection("duplicate directory names",
            BuildZip(std::vector<Member> { Directory("d/"), Directory("d/") }), "duplicate");
        ok &= ExpectStructuralRejection("ASCII case-fold collision",
            BuildZip(std::vector<Member> { Stored("A.txt", ToBytes("1")), Stored("a.TXT", ToBytes("2")) }), "case-colliding");
        ok &= ExpectStructuralRejection("case-fold collision across directories",
            BuildZip(std::vector<Member> { Stored("Dir/x.bin", ToBytes("1")), Stored("dir/X.bin", ToBytes("2")) }), "case-colliding");
        ok &= ExpectStructuralRejection("file and directory share a path",
            BuildZip(std::vector<Member> { Stored("a", ToBytes("1")), Directory("a/") }), "duplicate");
        ok &= ExpectStructuralRejection("file path passes through another file",
            BuildZip(std::vector<Member> { Stored("a", ToBytes("1")), Stored("a/b", ToBytes("2")) }), "passes through a file");
        ok &= ExpectStructuralRejection("case-folded file ancestor",
            BuildZip(std::vector<Member> { Stored("A", ToBytes("1")), Stored("a/b", ToBytes("2")) }), "passes through a file");
        return ok;
    }

    bool TestContentMagic()
    {
        bool ok = true;
        struct MagicCase
        {
            const char* Label;
            Bytes Payload;
        };
        Bytes tar(600, 0);
        std::copy_n("ustar", 5, tar.begin() + 257);
        const MagicCase cases[] {
            { "zip local header", ToBytes("PK\x03\x04rest of a nested archive") },
            { "empty zip", ToBytes("PK\x05\x06" + std::string(18, '\0')) },
            { "zip data descriptor", ToBytes("PK\x07\x08rest") },
            { "7z", ToBytes(std::string("7z\xBC\xAF\x27\x1C\x00\x04", 8)) },
            { "rar", ToBytes(std::string("Rar!\x1A\x07\x00", 7)) },
            { "gzip", ToBytes("\x1F\x8B\x08\x00") },
            { "xz", ToBytes(std::string("\xFD" "7zXZ\x00", 6)) },
            { "bzip2", ToBytes("BZh91AY&SY") },
            { "tar ustar at offset 257", tar },
            { "ELF", ToBytes("\x7F" "ELF\x02\x01\x01") },
            { "Mach-O 32 big endian", ToBytes("\xFE\xED\xFA\xCE" "xxxx") },
            { "Mach-O 64 big endian", ToBytes("\xFE\xED\xFA\xCF" "xxxx") },
            { "Mach-O 32 little endian", ToBytes("\xCE\xFA\xED\xFE" "xxxx") },
            { "Mach-O 64 little endian", ToBytes("\xCF\xFA\xED\xFE" "xxxx") },
            { "Mach-O fat", ToBytes("\xCA\xFE\xBA\xBE" "xxxx") },
            { "PE", ToBytes(std::string("MZ\x90\x00\x03", 5)) },
            { "shebang", ToBytes("#!/bin/sh\necho owned\n") },
            { "two-byte MZ", ToBytes("MZ") }
        };
        for (const MagicCase& magic : cases)
        {
            ok &= ExpectExtractionRejection(std::string("magic in texture.png (stored): ") + magic.Label,
                BuildZip(Stored("texture.png", magic.Payload)), "nested archive or executable", "texture.png");
        }

        Bytes largeZip = ToBytes("PK\x03\x04");
        const Bytes tailPattern = Pattern(5000, 7);
        largeZip.insert(largeZip.end(), tailPattern.begin(), tailPattern.end());
        RecordingSink sink;
        ok &= ExpectExtractionRejection("magic in a large stored member", BuildZip(Stored("texture.png", largeZip)),
            FabArchiveLimits {}, "nested archive or executable", "texture.png", sink);
        ok &= Check(sink.BeginCalls == 0 && sink.Events() == 0, "a magic hit is rejected before the sink sees the member");
        RecordingSink deflateSink;
        ok &= ExpectExtractionRejection("magic in a Deflate member", BuildZip(Deflated("mesh.glb", largeZip)),
            FabArchiveLimits {}, "nested archive or executable", "mesh.glb", deflateSink);
        ok &= Check(deflateSink.Events() == 0, "a Deflate magic hit is rejected before the sink sees the member");

        RecordingSink laterSink;
        ok &= ExpectExtractionRejection("hostile member after a good one",
            BuildZip(std::vector<Member> { Stored("a.bin", ToBytes("fine")), Stored("b.png", largeZip) }),
            FabArchiveLimits {}, "nested archive or executable", "b.png", laterSink);
        ok &= Check(laterSink.Committed("a.bin") && laterSink.Memory.Files().size() == 1, "earlier members remain committed for the caller to discard");

        const Bytes png = ToBytes("\x89PNG\r\n\x1a\n" "data");
        const Bytes shortTar(200, 0);
        ok &= ExpectAccepted("benign content", BuildZip(std::vector<Member> { Stored("a.png", png), Stored("b.bin", shortTar),
                Stored("c.txt", ToBytes("MX is not MZ")), Stored("d.txt", ToBytes("P")) }),
            { { "a.png", png }, { "b.bin", shortTar }, { "c.txt", ToBytes("MX is not MZ") }, { "d.txt", ToBytes("P") } });
        return ok;
    }

    bool TestContentIntegrity()
    {
        bool ok = true;
        const Bytes payload = Pattern(4000, 8);

        Member badCrc = Stored("crc.bin", payload);
        badCrc.Crc ^= 1;
        ok &= ExpectExtractionRejection("stored header CRC mismatch", BuildZip(badCrc), "CRC mismatch", "crc.bin");
        Member badDeflateCrc = Deflated("crc.bin", payload);
        badDeflateCrc.Crc ^= 0x80000000u;
        ok &= ExpectExtractionRejection("Deflate header CRC mismatch", BuildZip(badDeflateCrc), "CRC mismatch", "crc.bin");

        Layout layout;
        Bytes flipped = BuildZip(Stored("flip.bin", payload), &layout);
        flipped[layout.Local[0] + 30 + 8 + 1234] ^= 0x01;
        ok &= ExpectExtractionRejection("stored data bit flip", flipped, "CRC mismatch", "flip.bin");

        const Bytes large = Pattern(300000, 9);
        Bytes largeFlip = BuildZip(Stored("large.bin", large), &layout);
        largeFlip[layout.Local[0] + 30 + 9 + 299000] ^= 0x01;
        RecordingSink largeSink;
        ok &= ExpectExtractionRejection("late stored bit flip after streaming began", largeFlip, FabArchiveLimits {},
            "CRC mismatch", "large.bin", largeSink);
        ok &= Check(largeSink.Begun == 1 && largeSink.Aborts == 1 && largeSink.Ends == 0, "streamed member is aborted, never ended");

        Member garbage;
        garbage.Name = "garbage.bin";
        garbage.Data = Bytes(40, 0xFF);
        garbage.Method = 8;
        garbage.CompressedSize = 40;
        garbage.UncompressedSize = 64;
        ok &= ExpectExtractionRejection("invalid Deflate block type", BuildZip(garbage), "Deflate stream is corrupt", "garbage.bin");

        Member truncatedStream = Deflated("cut.bin", large);
        truncatedStream.Data.resize(truncatedStream.Data.size() / 2);
        truncatedStream.CompressedSize = static_cast<u32>(truncatedStream.Data.size());
        ok &= ExpectExtractionRejection("truncated Deflate stream", BuildZip(truncatedStream), "Deflate stream is corrupt", "cut.bin");

        const Bytes zeros(100000, 0);
        Member lyingSmall = Deflated("lie.bin", zeros);
        lyingSmall.UncompressedSize = 10;
        lyingSmall.Crc = Crc32(Bytes(10, 0));
        RecordingSink smallSink;
        ok &= ExpectExtractionRejection("header declares 10 bytes, stream inflates 100000", BuildZip(lyingSmall), FabArchiveLimits {},
            "beyond its declared size", "lie.bin", smallSink);
        ok &= Check(smallSink.Events() == 0, "tiny lying member never reaches the sink");

        const Bytes bigZeros(600000, 0);
        Member lyingMedium = Deflated("lie.bin", bigZeros);
        lyingMedium.UncompressedSize = 100000;
        lyingMedium.Crc = Crc32(Bytes(100000, 0));
        RecordingSink mediumSink;
        ok &= ExpectExtractionRejection("header declares 100000 bytes, stream inflates 600000", BuildZip(lyingMedium), FabArchiveLimits {},
            "beyond its declared size", "lie.bin", mediumSink);
        ok &= Check(mediumSink.BytesAccepted <= 100000 && mediumSink.Aborts == 1,
            "no byte beyond the declared size reaches the sink");

        const Bytes shortStream(100, 'q');
        Member lyingLarge = Deflated("lie.bin", shortStream);
        lyingLarge.UncompressedSize = 150;
        ok &= ExpectExtractionRejection("header declares 150 bytes, stream inflates 100", BuildZip(lyingLarge), "does not match its header", "lie.bin");
        Member lyingLargeBig = Deflated("lie.bin", large);
        lyingLargeBig.UncompressedSize += 5;
        RecordingSink bigLieSink;
        ok &= ExpectExtractionRejection("large member shorter than its header", BuildZip(lyingLargeBig), FabArchiveLimits {},
            "does not match its header", "lie.bin", bigLieSink);
        ok &= Check(bigLieSink.Aborts == 1, "short large member is aborted");

        Member zeroLie = Deflated("zero.bin", Bytes {});
        zeroLie.Crc = 5;
        ok &= ExpectExtractionRejection("empty member with a non-zero CRC", BuildZip(zeroLie), "CRC mismatch", "zero.bin");
        Member emptyWithBytes = Deflated("zero.bin", shortStream);
        emptyWithBytes.UncompressedSize = 0;
        emptyWithBytes.Crc = 0;
        ok &= ExpectExtractionRejection("empty declaration over a non-empty stream", BuildZip(emptyWithBytes), "beyond its declared size", "zero.bin");
        return ok;
    }

    bool TestSinkFailureAndCancellation()
    {
        bool ok = true;
        const Bytes first = Pattern(1000, 10);
        const Bytes second = Pattern(200000, 11);
        const Bytes zip = BuildZip(std::vector<Member> { Stored("a.bin", first), Stored("b.bin", second) });

        RecordingSink midFile;
        midFile.FailWriteAfterBytes = 70000;
        ok &= ExpectExtractionRejection("sink fails mid-file", zip, FabArchiveLimits {}, "sink rejected", "b.bin", midFile);
        ok &= Check(midFile.Begun == 2 && midFile.Ends == 1 && midFile.Aborts == 1, "mid-file failure aborts exactly the in-flight member");
        ok &= Check(midFile.Committed("a.bin") && !midFile.Committed("b.bin") && midFile.Memory.Files().size() == 1,
            "only the finished member is committed after a mid-file failure");

        RecordingSink onBegin;
        onBegin.FailBeginAt = 2;
        ok &= ExpectExtractionRejection("sink cancels on BeginFile", zip, FabArchiveLimits {}, "sink rejected", "b.bin", onBegin);
        ok &= Check(onBegin.Aborts == 0 && onBegin.Memory.Files().size() == 1, "a refused BeginFile never starts a member");

        RecordingSink onFirstBegin;
        onFirstBegin.FailBeginAt = 1;
        ok &= ExpectExtractionRejection("sink cancels before the first member", zip, FabArchiveLimits {}, "sink rejected", "a.bin", onFirstBegin);
        ok &= Check(onFirstBegin.Memory.Files().empty() && onFirstBegin.Writes == 0, "nothing is committed or written after the first refusal");

        RecordingSink onEnd;
        onEnd.FailEndAt = 2;
        ok &= ExpectExtractionRejection("sink fails on EndFile", zip, FabArchiveLimits {}, "sink rejected", "b.bin", onEnd);
        ok &= Check(onEnd.Aborts == 1 && onEnd.Ends == 1, "a failed EndFile aborts the member");

        RecordingSink onHeadWrite;
        onHeadWrite.FailWriteAfterBytes = 0;
        ok &= ExpectExtractionRejection("sink fails on the first write", zip, FabArchiveLimits {}, "sink rejected", "a.bin", onHeadWrite);
        ok &= Check(onHeadWrite.Aborts == 1 && onHeadWrite.Memory.Files().empty(), "first-write failure aborts and commits nothing");

        RecordingSink complete;
        std::string error;
        ok &= Check(ExtractFabZip(In(zip), FabArchiveLimits {}, complete, error) && complete.Memory.Files().size() == 2 && complete.Aborts == 0,
            "the same archive succeeds with a cooperative sink");
        return ok;
    }

    bool TestLimitBoundaries()
    {
        bool ok = true;

        std::vector<Member> three { Stored("a.bin", ToBytes("1")), Stored("b.bin", ToBytes("2")), Directory("d/") };
        ok &= ExpectBoundary("entry count (directories included)", BuildZip(three), &FabArchiveLimits::MaximumFileCount, 3, "file count exceeds");

        const std::string path(100, 'p');
        ok &= ExpectBoundary("path bytes", BuildZip(Stored(path, ToBytes("x"))), &FabArchiveLimits::MaximumPathBytes, 100, "path exceeds");
        const std::string segment = "a/" + std::string(50, 's');
        ok &= ExpectBoundary("segment bytes", BuildZip(Stored(segment, ToBytes("x"))), &FabArchiveLimits::MaximumSegmentBytes, 50, "segment exceeds");
        ok &= ExpectBoundary("depth", BuildZip(Stored("a/b/c/d", ToBytes("x"))), &FabArchiveLimits::MaximumDepth, 4, "depth exceeds");
        ok &= ExpectBoundary("directory depth", BuildZip(Directory("a/b/c/")), &FabArchiveLimits::MaximumDepth, 3, "depth exceeds");

        const Bytes zip = BuildZip(std::vector<Member> { Stored("a.bin", Pattern(40, 12)), Stored("b.bin", Pattern(60, 13)) });
        ok &= ExpectBoundary("per-file bytes", zip, &FabArchiveLimits::MaximumFileBytes, 60, "size exceeds");
        ok &= ExpectBoundary("aggregate bytes", zip, &FabArchiveLimits::MaximumAggregateBytes, 100, "aggregate size exceeds");
        ok &= ExpectBoundary("compressed input bytes", zip, &FabArchiveLimits::MaximumCompressedBytes, zip.size(), "empty or exceeds");

        ok &= ExpectAccepted("boundary archive also extracts", In(zip), [&]
            {
                FabArchiveLimits exact;
                exact.MaximumFileCount = 2;
                exact.MaximumFileBytes = 60;
                exact.MaximumAggregateBytes = 100;
                exact.MaximumCompressedBytes = zip.size();
                return exact;
            }(), { { "a.bin", Pattern(40, 12) }, { "b.bin", Pattern(60, 13) } });

        FabArchiveLimits ratio;
        ratio.MinimumPerEntryAllowance = 1;
        ratio.MaximumPerEntryExpansion = 7;
        std::string error;
        const u32 compressed = 5000;
        ok &= Check(ValidatesOnly(BuildZip(JunkDeflated("r.bin", compressed, compressed * 7)), ratio, error), "per-entry ratio: C*r admitted (" + error + ")");
        ok &= Check(!ValidatesOnly(BuildZip(JunkDeflated("r.bin", compressed, compressed * 7 + 1)), ratio, error)
                && Contains(error, "expansion ratio exceeds"),
            "per-entry ratio: C*r+1 rejected (" + error + ")");

        FabArchiveLimits floor;
        floor.MaximumPerEntryExpansion = 1;
        floor.MinimumPerEntryAllowance = 4096;
        ok &= Check(ValidatesOnly(BuildZip(JunkDeflated("r.bin", 3, 4096)), floor, error), "per-entry floor: allowance admitted (" + error + ")");
        ok &= Check(!ValidatesOnly(BuildZip(JunkDeflated("r.bin", 3, 4097)), floor, error) && Contains(error, "expansion ratio exceeds"),
            "per-entry floor: allowance+1 rejected");
        FabArchiveLimits defaults;
        ok &= Check(ValidatesOnly(BuildZip(JunkDeflated("r.bin", 3, 1024 * 1024)), defaults, error), "default 1 MiB floor admitted");
        ok &= Check(!ValidatesOnly(BuildZip(JunkDeflated("r.bin", 3, 1024 * 1024 + 1)), defaults, error), "default 1 MiB floor + 1 rejected");
        ok &= Check(!ValidatesOnly(BuildZip(JunkDeflated("r.bin", 3, 0xFFFFFFFEu)), defaults, error),
            "huge declared size is refused from the header alone");

        FabArchiveLimits aggregateRatio;
        aggregateRatio.MaximumPerEntryExpansion = 1000000;
        aggregateRatio.MinimumAggregateAllowance = 1;
        aggregateRatio.MaximumAggregateExpansion = 3;
        const Bytes probe = BuildZip(std::vector<Member> { JunkDeflated("a.bin", 10, 1), JunkDeflated("b.bin", 10, 1) });
        const u64 target = probe.size() * 3;
        auto aggregateArchive = [&](u64 total)
        {
            return BuildZip(std::vector<Member> { JunkDeflated("a.bin", 10, static_cast<u32>(total / 2)),
                JunkDeflated("b.bin", 10, static_cast<u32>(total - total / 2)) });
        };
        ok &= Check(aggregateArchive(target).size() == probe.size(), "aggregate fixture keeps its size");
        ok &= Check(ValidatesOnly(aggregateArchive(target), aggregateRatio, error), "aggregate ratio: S*k admitted (" + error + ")");
        ok &= Check(!ValidatesOnly(aggregateArchive(target + 1), aggregateRatio, error) && Contains(error, "aggregate expansion ratio exceeds"),
            "aggregate ratio: S*k+1 rejected (" + error + ")");
        FabArchiveLimits aggregateFloor = aggregateRatio;
        aggregateFloor.MaximumAggregateExpansion = 1;
        aggregateFloor.MinimumAggregateAllowance = 5000;
        ok &= Check(ValidatesOnly(aggregateArchive(5000), aggregateFloor, error), "aggregate floor: allowance admitted");
        ok &= Check(!ValidatesOnly(aggregateArchive(5001), aggregateFloor, error) && Contains(error, "aggregate expansion ratio exceeds"),
            "aggregate floor: allowance+1 rejected");

        FabArchiveLimits overflow;
        overflow.MaximumPerEntryExpansion = ~0ull;
        overflow.MaximumAggregateExpansion = ~0ull;
        overflow.MaximumAggregateBytes = ~0ull;
        overflow.MaximumFileBytes = ~0ull;
        ok &= Check(ValidatesOnly(BuildZip(JunkDeflated("a.bin", 0xFFFF, 0xFFFFFFFEu)), overflow, error),
            "saturating ratio arithmetic does not wrap (" + error + ")");
        FabArchiveLimits zeroAggregate;
        zeroAggregate.MaximumAggregateBytes = 0;
        ok &= Check(!ValidatesOnly(BuildZip(Stored("a.bin", ToBytes("x"))), zeroAggregate, error) && Contains(error, "aggregate size exceeds"),
            "zero aggregate limit rejects any payload");
        return ok;
    }
}

namespace SpiralTests
{
    bool TestFabZipAdmission()
    {
        bool ok = true;
        ok &= TestValidArchives();
        ok &= TestFilePathInput();
        ok &= TestStreamingChunks();
        ok &= TestZip64SplitAndFlags();
        ok &= TestEntryTypes();
        ok &= TestMalformedStructure();
        ok &= TestNames();
        ok &= TestContentMagic();
        ok &= TestContentIntegrity();
        ok &= TestSinkFailureAndCancellation();
        ok &= TestLimitBoundaries();
        return ok;
    }
}
