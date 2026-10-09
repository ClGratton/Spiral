#pragma once

#include "Engine/Core/Base.h"

#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Engine
{
    // MaximumFileCount bounds central-directory entries, directories included.
    // Per-member expansion is bounded by max(MinimumPerEntryAllowance,
    // compressed * MaximumPerEntryExpansion) and aggregate expansion by
    // max(MinimumAggregateAllowance, archive bytes * MaximumAggregateExpansion);
    // all products saturate instead of wrapping.
    struct FabArchiveLimits
    {
        u64 MaximumCompressedBytes = 4ull * 1024ull * 1024ull * 1024ull;
        u64 MaximumFileCount = 4096;
        u64 MaximumDepth = 16;
        u64 MaximumPathBytes = 1024;
        u64 MaximumSegmentBytes = 255;
        u64 MaximumFileBytes = 1ull * 1024ull * 1024ull * 1024ull;
        u64 MaximumAggregateBytes = 8ull * 1024ull * 1024ull * 1024ull;
        u64 MaximumPerEntryExpansion = 100;
        u64 MaximumAggregateExpansion = 50;
        u64 MinimumPerEntryAllowance = 1ull * 1024ull * 1024ull;
        u64 MinimumAggregateAllowance = 16ull * 1024ull * 1024ull;
    };

    // One archive source: a caller-owned memory span or a regular file that is
    // opened once and read through the same handle for validation and extraction.
    struct FabArchiveInput
    {
        std::span<const u8> Memory;
        std::filesystem::path FilePath;
        bool IsFile = false;

        static FabArchiveInput FromMemory(std::span<const u8> bytes);
        static FabArchiveInput FromFile(std::filesystem::path path);
    };

    struct FabArchiveEntry
    {
        std::string RelativePath;
        u64 UncompressedBytes = 0;
        u64 CompressedBytes = 0;
        u32 Crc32 = 0;
    };

    // Receives regular members in canonical path order. BeginFile is called
    // only after the member's leading bytes passed the content-magic check;
    // EndFile is called only after the exact declared size and CRC verified.
    // A false return from any call stops extraction. AbortFile is called once
    // when BeginFile succeeded and the member does not reach a successful
    // EndFile; the sink must discard that partial member. Members committed by
    // earlier EndFile calls remain the caller's to discard (staging directory).
    class FabArchiveSink
    {
    public:
        virtual ~FabArchiveSink() = default;
        virtual bool BeginFile(std::string_view relativePath, u64 size) = 0;
        virtual bool Write(std::span<const u8> bytes) = 0;
        virtual bool EndFile() = 0;
        virtual void AbortFile() = 0;
    };

    struct FabMemoryFile
    {
        std::string RelativePath;
        std::vector<u8> Bytes;
    };

    class FabMemoryArchiveSink final : public FabArchiveSink
    {
    public:
        const std::vector<FabMemoryFile>& Files() const { return m_Committed; }

        bool BeginFile(std::string_view relativePath, u64 size) override;
        bool Write(std::span<const u8> bytes) override;
        bool EndFile() override;
        void AbortFile() override;

    private:
        std::vector<FabMemoryFile> m_Committed;
        FabMemoryFile m_Current;
    };

    // Structural validation of the central directory, local headers, names,
    // limits, and entry types. It reads headers only and has no output side
    // effects: entries is replaced only on success. Content-dependent checks
    // (Deflate validity, CRC, exact size, nested-archive/executable magic) run
    // during ExtractFabZip, whose success is the admission authority.
    // Admits only single-disk, non-Zip64, unencrypted Store/Deflate archives
    // whose end-of-central-directory record is the last bytes of the input.
    bool ValidateFabZip(const FabArchiveInput& input, const FabArchiveLimits& limits,
        std::vector<FabArchiveEntry>& entries, std::string& error);

    // Revalidates the input, then streams each regular member in 64 KiB-or-
    // smaller chunks to the sink. Nothing is buffered beyond one chunk and a
    // 512-byte member head.
    bool ExtractFabZip(const FabArchiveInput& input, const FabArchiveLimits& limits,
        FabArchiveSink& sink, std::string& error);
}
